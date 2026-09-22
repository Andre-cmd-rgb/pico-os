/*
 * A GSM/LTE modem on a serial port.
 *
 * Modems still speak the Hayes command set: lines of text starting with
 * AT, each answered by lines ending in OK or ERROR. That is all this
 * driver is -- a careful reader and writer of those lines -- plus the two
 * things they are wanted for: text messages, and a PPP link that gives
 * the whole system an internet connection over the mobile network.
 *
 * It has been written against the command set the common modules share
 * (SIM800, SIM7600, A7670, EC200), and asks the module what it is at
 * start-up rather than assuming.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_MODEM

#include "lwip/dns.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"

#define PORT		UART_NUM_1
#define RX_BUFFER	2048
#define REPLY_MAX	1024
#define LINE_WAIT_MS	100
#define PROBE_TRIES	15		/* a second apart: modules boot slowly */

static void probe_task(void *arg);

static SemaphoreHandle_t lock;
static TaskHandle_t	 reader;
static bool		 present, data_mode;
static char		 model[48], imei[20];
static ppp_pcb		*ppp;
static struct netif	 ppp_netif;
static volatile bool	 ppp_up, ppp_stop;

/* ------------------------------------------------------------ AT */

/* One line from the modem, without its line ending; false on timeout. */
static bool read_line(char *out, size_t size, int timeout_ms)
{
	size_t len = 0;
	int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

	while (esp_timer_get_time() < end) {
		uint8_t c;

		if (uart_read_bytes(PORT, &c, 1, pdMS_TO_TICKS(LINE_WAIT_MS)) != 1)
			continue;
		if (c == '\r')
			continue;
		if (c == '\n') {
			if (!len)
				continue;	/* the blank line between answers */
			out[len] = '\0';
			return true;
		}
		if (len + 1 < size)
			out[len++] = c;
	}
	out[len] = '\0';
	return len > 0;
}

static bool is_final(const char *line, int *err)
{
	if (!strcmp(line, "OK")) {
		*err = 0;
		return true;
	}
	if (!strcmp(line, "ERROR") || !strncmp(line, "+CME ERROR", 10) ||
	    !strncmp(line, "+CMS ERROR", 10) || !strcmp(line, "NO CARRIER")) {
		*err = -EIO;
		return true;
	}
	return false;
}

/*
 * Sends a command and collects what comes back, up to OK or ERROR. The
 * reply keeps the informational lines only; the final OK is not part of
 * it.
 */
int modem_at(const char *cmd, char *reply, size_t size, int timeout_ms)
{
	char line[256];
	size_t len = 0;
	int err = -ETIMEDOUT;

	if (!present)
		return -ENODEV;
	if (data_mode)
		return -EBUSY;		/* the port is carrying PPP */
	if (reply && size)
		*reply = '\0';
	xSemaphoreTake(lock, portMAX_DELAY);
	uart_flush_input(PORT);
	uart_write_bytes(PORT, cmd, strlen(cmd));
	uart_write_bytes(PORT, "\r\n", 2);
	while (read_line(line, sizeof(line), timeout_ms)) {
		if (!strcmp(line, cmd))
			continue;	/* the echo, if it is still on */
		if (is_final(line, &err))
			break;
		if (reply && len + strlen(line) + 2 < size)
			len += snprintf(reply + len, size - len, "%s\n", line);
	}
	xSemaphoreGive(lock);
	return err;
}

/* The part of a reply after "+XXX: ", trimmed of quotes. */
static const char *field(const char *reply, const char *tag)
{
	const char *p = strstr(reply, tag);

	return p ? p + strlen(tag) : NULL;
}

static void unquote(char *s)
{
	char *out = s;

	for (char *p = s; *p; p++)
		if (*p != '"')
			*out++ = *p;
	*out = '\0';
}

bool modem_present(void)
{
	return present;
}

int modem_info(struct modem_info *out)
{
	char reply[REPLY_MAX];
	const char *p;

	memset(out, 0, sizeof(*out));
	if (!present)
		return -ENODEV;
	strlcpy(out->model, model, sizeof(out->model));
	strlcpy(out->imei, imei, sizeof(out->imei));
	out->data = ppp_up;

	if (!modem_at("AT+CPIN?", reply, sizeof(reply), 5000) && (p = field(reply, "+CPIN: ")))
		strlcpy(out->sim, p, sizeof(out->sim));
	else
		strlcpy(out->sim, "no card", sizeof(out->sim));
	if (!modem_at("AT+CSQ", reply, sizeof(reply), 2000) && (p = field(reply, "+CSQ: "))) {
		int rssi = atoi(p);

		/* 0..31 maps onto -113..-51 dBm; 99 means it cannot tell. */
		out->rssi = rssi == 99 ? 0 : -113 + 2 * rssi;
	}
	if (!modem_at("AT+COPS?", reply, sizeof(reply), 10000) && (p = field(reply, "+COPS: "))) {
		const char *name = strchr(p, '"');

		if (name) {
			strlcpy(out->operator, name, sizeof(out->operator));
			unquote(out->operator);
		}
	}
	if (!modem_at("AT+CREG?", reply, sizeof(reply), 2000) && (p = field(reply, "+CREG: "))) {
		int state = 0;

		sscanf(p, "%*d,%d", &state);
		out->registered = state == 1 || state == 5;	/* home or roaming */
		out->roaming = state == 5;
	}
	return 0;
}

/* ------------------------------------------------------------ messages */

/*
 * Text mode, because the alternative (PDU mode) means encoding the
 * message by hand and every module in this class supports text.
 */
static int text_mode(void)
{
	int ret = modem_at("AT+CMGF=1", NULL, 0, 2000);

	if (!ret)
		modem_at("AT+CSCS=\"GSM\"", NULL, 0, 2000);
	return ret;
}

int modem_sms_send(const char *number, const char *text)
{
	char cmd[64], line[256];
	int err = -ETIMEDOUT;
	bool prompt = false;

	if (!present)
		return -ENODEV;
	if (data_mode)
		return -EBUSY;
	if ((err = text_mode()))
		return err;

	snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", number);
	xSemaphoreTake(lock, portMAX_DELAY);
	uart_flush_input(PORT);
	uart_write_bytes(PORT, cmd, strlen(cmd));
	uart_write_bytes(PORT, "\r", 1);

	/* The module answers "> " and then waits for the text. */
	for (int64_t end = esp_timer_get_time() + 5000000; esp_timer_get_time() < end;) {
		uint8_t c;

		if (uart_read_bytes(PORT, &c, 1, pdMS_TO_TICKS(100)) == 1 && c == '>') {
			prompt = true;
			break;
		}
	}
	if (!prompt) {
		xSemaphoreGive(lock);
		return -EIO;
	}
	uart_write_bytes(PORT, text, strlen(text));
	uart_write_bytes(PORT, "\x1a", 1);		/* Ctrl-Z sends it */

	err = -ETIMEDOUT;
	while (read_line(line, sizeof(line), 60000))	/* sending can take a while */
		if (is_final(line, &err))
			break;
	xSemaphoreGive(lock);
	return err;
}

/*
 * +CMGL: 3,"REC UNREAD","+391234567",,"26/09/20,22:14:31+08"
 * the text of the message
 */
static void parse_header(const char *line, struct sms *m)
{
	const char *p = line;
	char *out;

	m->index = atoi(p);
	m->unread = strstr(line, "UNREAD") != NULL;
	for (int i = 0; i < 2 && p; i++)		/* skip index and status */
		p = strchr(p + 1, ',');
	if (!p)
		return;
	out = m->from;
	for (p++; *p && *p != ','; p++)
		if (*p != '"' && out < m->from + sizeof(m->from) - 1)
			*out++ = *p;
	*out = '\0';
	p = strchr(p, '"');				/* the timestamp */
	if (!p)
		return;
	out = m->when;
	for (p++; *p && *p != '"'; p++)
		if (out < m->when + sizeof(m->when) - 1)
			*out++ = *p;
	*out = '\0';
}

int modem_sms_list(struct sms *out, int max, bool unread_only)
{
	char line[256];
	int n = 0, err;

	if (!present)
		return -ENODEV;
	if (data_mode)
		return -EBUSY;
	if ((err = text_mode()))
		return err;

	xSemaphoreTake(lock, portMAX_DELAY);
	uart_flush_input(PORT);
	uart_write_bytes(PORT, unread_only ? "AT+CMGL=\"REC UNREAD\"\r\n"
					   : "AT+CMGL=\"ALL\"\r\n", unread_only ? 22 : 16);
	while (read_line(line, sizeof(line), 10000)) {
		if (is_final(line, &err))
			break;
		if (!strncmp(line, "+CMGL:", 6)) {
			if (n == max)
				break;
			memset(&out[n], 0, sizeof(out[n]));
			parse_header(line + 6, &out[n]);
			n++;
		} else if (n && !*out[n - 1].text) {
			strlcpy(out[n - 1].text, line, sizeof(out[n - 1].text));
		}
	}
	xSemaphoreGive(lock);
	return n;
}

int modem_sms_read(int index, struct sms *out)
{
	char cmd[32], reply[REPLY_MAX];
	const char *p;
	int ret;

	memset(out, 0, sizeof(*out));
	if ((ret = text_mode()))
		return ret;
	snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
	if ((ret = modem_at(cmd, reply, sizeof(reply), 10000)))
		return ret;
	p = field(reply, "+CMGR: ");
	if (!p)
		return -ENOENT;
	out->index = index;
	parse_header(p, out);				/* no index in this one */
	out->index = index;
	p = strchr(p, '\n');
	if (p)
		strlcpy(out->text, p + 1, sizeof(out->text));
	for (char *nl = strchr(out->text, '\n'); nl; nl = strchr(out->text, '\n'))
		*nl = ' ';				/* keep it to one line */
	return 0;
}

int modem_sms_delete(int index)
{
	char cmd[32];

	snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
	return modem_at(cmd, NULL, 0, 10000);
}

/* ------------------------------------------------------------ data (PPP) */

static u32_t ppp_output(ppp_pcb *pcb, const void *data, u32_t len, void *ctx)
{
	return uart_write_bytes(PORT, data, len);
}

static void ppp_status(ppp_pcb *pcb, int err_code, void *ctx)
{
	const struct netif *nif = &ppp_netif;

	switch (err_code) {
	case PPPERR_NONE:
		ppp_up = true;
		klog("modem: data up, %s, dns %s", ip4addr_ntoa(netif_ip4_addr(nif)),
		     ipaddr_ntoa(dns_getserver(0)));
		break;
	case PPPERR_USER:
		ppp_up = false;
		klog("modem: data down");
		break;
	default:
		ppp_up = false;
		klog("modem: data link failed (%d)", err_code);
		break;
	}
}

/* In data mode every byte from the port belongs to PPP. */
static void reader_task(void *arg)
{
	uint8_t *buf = malloc(512);

	while (buf && !ppp_stop) {
		int n = uart_read_bytes(PORT, buf, 512, pdMS_TO_TICKS(50));

		if (n > 0 && ppp)
			pppos_input_tcpip(ppp, buf, n);
	}
	free(buf);
	reader = NULL;
	vTaskDelete(NULL);
}

bool modem_data_up(void)
{
	return ppp_up;
}

int modem_data(bool on)
{
	char cmd[96], line[128];
	int err = -ETIMEDOUT;

	if (!present)
		return -ENODEV;
	if (!on) {
		if (!ppp)
			return 0;
		pppapi_close(ppp, 0);
		for (int i = 0; i < 50 && ppp_up; i++)
			vTaskDelay(pdMS_TO_TICKS(100));
		ppp_stop = true;
		while (reader)
			vTaskDelay(pdMS_TO_TICKS(20));
		pppapi_free(ppp);
		ppp = NULL;
		data_mode = false;
		vTaskDelay(pdMS_TO_TICKS(1000));	/* the guard time before +++ */
		uart_write_bytes(PORT, "+++", 3);
		vTaskDelay(pdMS_TO_TICKS(1000));
		modem_at("ATH", NULL, 0, 5000);
		return 0;
	}
	if (ppp)
		return -EALREADY;

	snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", CONFIG_PT_MODEM_APN);
	if ((err = modem_at(cmd, NULL, 0, 5000)))
		return err;

	/* ATD dials the packet service; the answer is CONNECT, then PPP. */
	xSemaphoreTake(lock, portMAX_DELAY);
	uart_flush_input(PORT);
	uart_write_bytes(PORT, "ATD*99***1#\r\n", 13);
	err = -ETIMEDOUT;
	while (read_line(line, sizeof(line), 30000)) {
		if (!strncmp(line, "CONNECT", 7)) {
			err = 0;
			break;
		}
		if (is_final(line, &err))
			break;
	}
	xSemaphoreGive(lock);
	if (err) {
		klog("modem: the network did not answer the dial");
		return err ? err : -EIO;
	}

	ppp = pppapi_pppos_create(&ppp_netif, ppp_output, ppp_status, NULL);
	if (!ppp)
		return -ENOMEM;
	data_mode = true;
	ppp_stop = false;
	if (xTaskCreatePinnedToCore(reader_task, "kppp", 4096, NULL, 6, &reader, 0) != pdPASS) {
		pppapi_free(ppp);
		ppp = NULL;
		data_mode = false;
		return -ENOMEM;
	}
	ppp_set_usepeerdns(ppp, 1);
	pppapi_set_default(ppp);
	if (*CONFIG_PT_MODEM_USER)
		ppp_set_auth(ppp, PPPAUTHTYPE_ANY, CONFIG_PT_MODEM_USER,
			     CONFIG_PT_MODEM_PASSWORD);
	pppapi_connect(ppp, 0);
	for (int i = 0; i < 300 && !ppp_up; i++)	/* up to 30 s to negotiate */
		vTaskDelay(pdMS_TO_TICKS(100));
	return ppp_up ? 0 : -ETIMEDOUT;
}

/* ------------------------------------------------------------ boot */

static int gen_proc_modem(char *b, size_t n)
{
	struct modem_info info;

	if (modem_info(&info))
		return snprintf(b, n, "modem: none\n");
	return snprintf(b, n,
			"module    %s\n"
			"imei      %s\n"
			"sim       %s\n"
			"operator  %s%s\n"
			"signal    %d dBm\n"
			"network   %s\n"
			"data      %s\n",
			info.model, info.imei, info.sim,
			*info.operator ? info.operator : "-", info.roaming ? " (roaming)" : "",
			info.rssi, info.registered ? "registered" : "searching",
			info.data ? "up" : "down");
}

int modem_init(void)
{
	const uart_config_t cfg = {
		.baud_rate = CONFIG_PT_MODEM_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	lock = xSemaphoreCreateMutex();
	if (!lock)
		return -ENOMEM;
	if (uart_driver_install(PORT, RX_BUFFER, 512, 0, NULL, 0) ||
	    uart_param_config(PORT, &cfg) ||
	    uart_set_pin(PORT, CONFIG_PT_MODEM_TX, CONFIG_PT_MODEM_RX,
			 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)) {
		klog("modem: cannot open the serial port");
		return -EIO;
	}

	xTaskCreatePinnedToCore(probe_task, "kmodem", 4096, NULL, 3, NULL, 0);
	return 0;
}

/*
 * Modules take their time: a SIM7600 is ten seconds from power to its
 * first answer, and some need the first few commands thrown away. So the
 * probe runs on its own task and the rest of the system boots.
 */
static void probe_task(void *arg)
{
	char reply[REPLY_MAX];

	present = true;
	for (int i = 0; i < PROBE_TRIES; i++) {
		if (!modem_at("AT", NULL, 0, 1000))
			goto found;
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
	present = false;
	klog("modem: nothing answers on TX %d / RX %d at %d baud",
	     CONFIG_PT_MODEM_TX, CONFIG_PT_MODEM_RX, CONFIG_PT_MODEM_BAUD);
	uart_driver_delete(PORT);
	vTaskDelete(NULL);
	return;
found:
	modem_at("ATE0", NULL, 0, 1000);		/* stop echoing our commands */
	if (!modem_at("ATI", reply, sizeof(reply), 2000)) {
		char *nl = strchr(reply, '\n');

		if (nl)
			*nl = '\0';
		strlcpy(model, reply, sizeof(model));
	}
	if (!modem_at("AT+CGSN", reply, sizeof(reply), 2000)) {
		char *nl = strchr(reply, '\n');

		if (nl)
			*nl = '\0';
		strlcpy(imei, reply, sizeof(imei));
	}
	modem_at("AT+CMEE=1", NULL, 0, 1000);		/* numeric errors, not silence */
	text_mode();
	proc_register("modem", gen_proc_modem);
	klog("modem: %s on TX %d / RX %d%s%s", *model ? model : "module",
	     CONFIG_PT_MODEM_TX, CONFIG_PT_MODEM_RX, *imei ? ", imei " : "", imei);
	vTaskDelete(NULL);
}

#else /* !CONFIG_PT_MODEM */

int  modem_init(void) { return -ENODEV; }
bool modem_present(void) { return false; }
int  modem_at(const char *cmd, char *reply, size_t size, int timeout_ms) { return -ENODEV; }
int  modem_info(struct modem_info *out) { return -ENODEV; }
int  modem_sms_send(const char *number, const char *text) { return -ENODEV; }
int  modem_sms_list(struct sms *out, int max, bool unread_only) { return -ENODEV; }
int  modem_sms_read(int index, struct sms *out) { return -ENODEV; }
int  modem_sms_delete(int index) { return -ENODEV; }
int  modem_data(bool on) { return -ENODEV; }
bool modem_data_up(void) { return false; }

#endif
