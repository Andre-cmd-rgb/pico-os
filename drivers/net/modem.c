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
 *
 * A module also speaks unasked: a message has come (+CMTI), a call rings
 * (RING, +CLIP), the network is found or lost (+CREG), its supply sags
 * (UNDER-VOLTAGE), it has restarted (RDY). kmodem waits for anything the
 * port receives between commands and hands those lines to news(); a
 * command that meets one among its answers does the same. The rest of a
 * restart's settings are made again, and a SIM800 left auto-bauding is
 * given a fixed speed, so that a restart is heard at all.
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
#define PROBE_ROUNDS	8		/* at boot: modules take their time to start */

static void probe_task(void *arg);
static void watch(void);

/*
 * The speeds tried, the configured one first. A SIM800 left at a fixed
 * speed by whoever used it last answers at that one only; at first
 * power-on it takes any, from the "AT" it is sent.
 */
static const int bauds[] = { CONFIG_PT_MODEM_BAUD, 115200, 57600, 38400, 19200, 9600 };

static SemaphoreHandle_t lock;
static TaskHandle_t	 reader;
static bool		 present, data_mode;
static volatile bool	 probing;		/* AT allowed before `present` */
static int		 baud = CONFIG_PT_MODEM_BAUD;
static QueueHandle_t	 events;		/* the UART driver's: bytes came */
static volatile bool	 resetup, heard_start;	/* it restarted; one started up */
static volatile int	 new_sms = -1;		/* where a message that came was put */
static int		 reg = -1;		/* +CREG: the network, last heard */
static bool		 latin1;		/* texts go as ISO 8859-1, not GSM */
static char		 ussd[256];
static SemaphoreHandle_t ussd_done;
static int64_t		 last_call_us;
static int64_t		 started_at[4];		/* its last restarts, for restarts_lately() */
static bool		 sleepy;		/* AT+CSCLK=2 taken: it sleeps when let be */
static int64_t		 setup_at;		/* after a restart: set it up from then */
static volatile bool	 booted;		/* "SMS Ready": it has finished starting */
static int64_t		 warned_at;		/* the restart warning, at most every 10 min */
static char		 sim_state[24];		/* +CPIN's last word, logged when it changes */
static int64_t		 last_io_us;		/* the port last used, for wake() */

/* /etc/modem, kept by the driver: `modem apn`, `modem off` */
static struct {
	char	apn[64], user[32], pass[32];
	bool	radio_off;
} conf;

static int text_mode(void);
static int read_sms(int index, struct sms *out, bool peek);
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
		last_io_us = esp_timer_get_time();
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

/* ------------------------------------------------------------ news */

static const char *const news_kinds[] = {
	"RING", "+CLIP:", "+CMTI:", "+CREG:", "+CPIN:", "+CUSD:", "+CFUN:", "RDY",
	"Call Ready", "SMS Ready", "UNDER-VOLTAGE", "OVER-VOLTAGE", "NORMAL POWER DOWN",
	"+PDP: DEACT",
};

/*
 * Whether a line is the module speaking unasked. "+CREG: 1,1" is the
 * answer to AT+CREG?, not news, so a kind with the name of the command
 * being answered is taken for its answer -- all but +CUSD, whose answer
 * only ever comes afterwards.
 */
static bool is_news(const char *line, const char *cmd)
{
	for (size_t i = 0; i < sizeof(news_kinds) / sizeof(news_kinds[0]); i++) {
		const char *k = news_kinds[i];
		size_t n = strlen(k);

		if (strncmp(line, k, n))
			continue;
		if (k[0] == '+' && strcmp(k, "+CUSD:") && cmd && !strncmp(cmd, "AT", 2) &&
		    !strncmp(cmd + 2, k, n - 1))
			return false;
		return true;
	}
	return false;
}

/* What the network's registration codes mean, for the log and `modem`. */
const char *modem_network_text(int state)
{
	switch (state) {
	case 0:	return "not looking for a network";
	case 1:	return "registered";
	case 2:	return "searching";
	case 3:	return "refused by the network: is the SIM activated?";
	case 5:	return "registered, roaming";
	default: return "unknown";
	}
}

/* How many times it has restarted in the last minute: more than once is its supply. */
static int restarts_lately(void)
{
	int64_t now = esp_timer_get_time();
	int n = 0;

	for (size_t i = 0; i < sizeof(started_at) / sizeof(started_at[0]); i++)
		if (started_at[i] && now - started_at[i] < 60000000)
			n++;
	return n;
}

/* The text between the first and the last quote: "+CLIP: \"+39...\",145". */
static void quoted(const char *line, char *out, size_t size)
{
	const char *a = strchr(line, '"'), *b = strrchr(line, '"');
	size_t n = a && b > a ? (size_t)(b - a - 1) : 0;

	if (n >= size)
		n = size - 1;
	memcpy(out, a ? a + 1 : "", n);
	out[n] = '\0';
}

/*
 * One line of news. Only what is quick is done here, where the port is
 * held; reading the message that came and setting a restarted module up
 * again are left to kmodem (after_news()).
 */
static void news(const char *line)
{
	char note[64], who[32];

	if (!strncmp(line, "+CMTI:", 6)) {
		const char *comma = strchr(line, ',');

		new_sms = comma ? atoi(comma + 1) : 0;
		klog("modem: a text message has come");
	} else if (!strncmp(line, "+CLIP:", 6)) {
		int64_t now = esp_timer_get_time();

		quoted(line, who, sizeof(who));
		/* rings every few seconds: logged once a call */
		if (now - last_call_us > 10000000)
			klog("modem: a call from %s (calls cannot be answered here)",
			     *who ? who : "a hidden number");
		last_call_us = now;
		snprintf(note, sizeof(note), "\u260e %s", *who ? who : "a call");
		vt_note(note);
	} else if (!strncmp(line, "+CREG:", 6)) {
		const char *p = strchr(line, ',');
		int state = atoi(p ? p + 1 : line + 6);

		/* a module restarting over and over says the same every time */
		if (state != reg && restarts_lately() < 2 && state != 0)
			klog("modem: network: %s", modem_network_text(state));
		reg = state;
	} else if (!strncmp(line, "+CPIN:", 6)) {
		if (strcmp(sim_state, line + 7) && restarts_lately() < 2)
			klog("modem: the SIM card: %s", line + 7);
		strlcpy(sim_state, line + 7, sizeof(sim_state));
	} else if (!strncmp(line, "+CUSD:", 6)) {
		strlcpy(ussd, line + 6, sizeof(ussd));
		xSemaphoreGive(ussd_done);
	} else if (!strcmp(line, "RDY")) {
		int64_t now = esp_timer_get_time();

		memmove(started_at + 1, started_at, sizeof(started_at) - sizeof(started_at[0]));
		started_at[0] = now;
		if (restarts_lately() < 3) {
			klog("modem: the module has started");
		} else if (!warned_at || now - warned_at > 600000000) {
			warned_at = now;
			klog("modem: the module keeps restarting (%d times in a minute): its "
			     "supply sags when it transmits (it needs 3.4-4.4 V with 2 A to spare)",
			     restarts_lately());
		}
		/* set up again once it has finished starting: before that, what
		 * it is told can be undone by its own start (the radio comes on) */
		booted = false;
		setup_at = now + 8000000;
		resetup = heard_start = true;
	} else if (!strncmp(line, "UNDER-VOLTAGE", 13) || !strncmp(line, "OVER-VOLTAGE", 12)) {
		klog("modem: %s: the module's supply is %s", line,
		     line[0] == 'U' ? "sagging (it needs 3.4 V, with 2 A to spare)"
				    : "too high (4.4 V at most)");
		if (strstr(line, "POWER DOWN"))
			present = false;
	} else if (!strcmp(line, "NORMAL POWER DOWN")) {
		klog("modem: the module has switched itself off");
		present = false;
	} else if (!strncmp(line, "+PDP: DEACT", 11)) {
		klog("modem: the network ended the data session");
	} else if (!strcmp(line, "SMS Ready")) {
		heard_start = booted = true;
	}
}

static int		 late_ms;		/* after a timeout: answers may still come */

/*
 * What is waiting on the port: news, or what is left of an answer nobody
 * waited for. After a command that timed out, its answer may come yet,
 * and the next command would take its OK for its own and every answer
 * after would be one behind (the model's name read as the IMEI): so then
 * this waits for the line to go quiet for a while first.
 */
static void drain(void)
{
	char line[256];
	int quiet = late_ms ? late_ms : 20;

	late_ms = 0;
	while (read_line(line, sizeof(line), quiet))
		if (is_news(line, NULL))
			news(line);
}

/*
 * Asleep -- AT+CSCLK=2, after five seconds with nothing on the port -- a
 * SIM800 loses the byte that wakes it and needs a moment before it
 * listens: a throwaway AT first, whose answer, if any, drain() takes.
 */
static void wake(void)
{
	if (!sleepy || esp_timer_get_time() - last_io_us < 4000000)
		return;
	uart_write_bytes(PORT, "AT\r", 3);
	vTaskDelay(pdMS_TO_TICKS(120));
	late_ms = late_ms > 200 ? late_ms : 200;
}

/* Writing to the module, which keeps it awake. */
static void put(const void *data, size_t n)
{
	uart_write_bytes(PORT, data, n);
	last_io_us = esp_timer_get_time();
}

/*
 * Sends a command and collects what comes back, up to OK or ERROR. The
 * reply keeps the informational lines only; the final OK is not part of
 * it. News that comes in the middle goes to news().
 */
int modem_at(const char *cmd, char *reply, size_t size, int timeout_ms)
{
	char line[512];			/* AT+COPS=? answers on one long line */
	size_t len = 0;
	int err = -ETIMEDOUT;

	if (!present && !probing)
		return -ENODEV;
	if (data_mode)
		return -EBUSY;		/* the port is carrying PPP */
	if (reply && size)
		*reply = '\0';
	xSemaphoreTake(lock, portMAX_DELAY);
	wake();
	drain();
	put(cmd, strlen(cmd));
	put("\r\n", 2);
	while (read_line(line, sizeof(line), timeout_ms)) {
		if (!strcmp(line, cmd))
			continue;	/* the echo, if it is still on */
		if (is_news(line, cmd)) {
			news(line);
			continue;
		}
		if (is_final(line, &err))
			break;
		if (reply && len + strlen(line) + 2 < size)
			len += snprintf(reply + len, size - len, "%s\n", line);
	}
	if (err == -ETIMEDOUT)
		late_ms = 500;
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
	int err;

	memset(out, 0, sizeof(*out));
	out->reg = -1;
	if (!present)
		return -ENODEV;
	strlcpy(out->model, model, sizeof(out->model));
	strlcpy(out->imei, imei, sizeof(out->imei));
	out->data = ppp_up;

	out->restarts = restarts_lately();
	/*
	 * A module that does not answer the first question will not answer
	 * the rest either -- it is restarting, most likely -- and four
	 * timeouts in a row made `modem` look hung.
	 */
	if ((err = modem_at("AT+CPIN?", reply, sizeof(reply), 3000)) == -ETIMEDOUT)
		return err;
	if (!err && (p = field(reply, "+CPIN: ")))
		strlcpy(out->sim, p, strcspn(p, "\n") + 1 < sizeof(out->sim) ?
			strcspn(p, "\n") + 1 : sizeof(out->sim));
	else
		strlcpy(out->sim, "no card", sizeof(out->sim));
	if (!modem_at("AT+CSQ", reply, sizeof(reply), 2000) && (p = field(reply, "+CSQ: "))) {
		int rssi = atoi(p);

		/* 0..31 maps onto -113..-51 dBm; 99 means it cannot tell. */
		out->rssi = rssi == 99 ? 0 : -113 + 2 * rssi;
	}
	if (!modem_at("AT+COPS?", reply, sizeof(reply), 3000) && (p = field(reply, "+COPS: "))) {
		const char *name = strchr(p, '"');

		if (name) {
			strlcpy(out->operator, name, sizeof(out->operator));
			unquote(out->operator);
		}
	}
	if (!modem_at("AT+CREG?", reply, sizeof(reply), 2000) && (p = field(reply, "+CREG: "))) {
		int state = 0;

		sscanf(p, "%*d,%d", &state);
		out->reg = reg = state;
		out->registered = state == 1 || state == 5;	/* home or roaming */
		out->roaming = state == 5;
	}
	return 0;
}

/* ------------------------------------------------------------ messages */

/*
 * Text mode, because the alternative (PDU mode) means encoding the
 * message by hand and every module in this class supports text. The
 * texts go to and fro as ISO 8859-1 where the module has it (SIM800
 * does), so that an Italian's è, à and ù arrive; it turns them into the
 * GSM alphabet itself. Without it, GSM, and only ASCII gets through.
 */
static int text_mode(void)
{
	int ret = modem_at("AT+CMGF=1", NULL, 0, 2000);

	if (!ret) {
		latin1 = !modem_at("AT+CSCS=\"8859-1\"", NULL, 0, 2000);
		if (!latin1)
			modem_at("AT+CSCS=\"GSM\"", NULL, 0, 2000);
	}
	return ret;
}

/* UTF-8 as the module takes it: ISO 8859-1, or ASCII; what it cannot be is '?'. */
static void to_module(char *out, size_t size, const char *in)
{
	const unsigned char *p = (const unsigned char *)in;
	size_t n = 0;

	while (*p && n + 1 < size) {
		unsigned c = *p++;
		int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

		if (c >= 0x80) {
			if (!more) {
				c = '?';		/* a stray continuation byte */
			} else {
				c &= 0x3f >> more;
				for (; more && (*p & 0xc0) == 0x80; more--)
					c = c << 6 | (*p++ & 0x3f);
				if (more || c > (latin1 ? 0xffu : 0x7fu))
					c = '?';
			}
		}
		out[n++] = (char)c;
	}
	out[n] = '\0';
}

/* What the module sends, as UTF-8: ISO 8859-1 widened, or as it is. */
static void from_module(char *out, size_t size, const char *in)
{
	size_t n = 0;

	for (const unsigned char *p = (const unsigned char *)in; *p && n + 1 < size; p++) {
		if (*p < 0x80 || !latin1) {
			out[n++] = *p < 0x80 ? (char)*p : '?';
		} else if (n + 2 < size) {
			out[n++] = (char)(0xc0 | *p >> 6);
			out[n++] = (char)(0x80 | (*p & 0x3f));
		} else {
			break;
		}
	}
	out[n] = '\0';
}

int modem_sms_send(const char *number, const char *text)
{
	char cmd[64], line[256], body[200];
	int err = -ETIMEDOUT;
	bool prompt = false;

	if (!present)
		return -ENODEV;
	if (data_mode)
		return -EBUSY;
	if ((err = text_mode()))
		return err;

	to_module(body, sizeof(body), text);
	snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", number);
	xSemaphoreTake(lock, portMAX_DELAY);
	wake();
	drain();
	put(cmd, strlen(cmd));
	put("\r", 1);

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
	put(body, strlen(body));
	put("\x1a", 1);				/* Ctrl-Z sends it */

	err = -ETIMEDOUT;
	while (read_line(line, sizeof(line), 60000)) {	/* sending can take a while */
		if (is_news(line, cmd))
			news(line);
		else if (is_final(line, &err))
			break;
	}
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
	wake();
	drain();
	put(unread_only ? "AT+CMGL=\"REC UNREAD\"\r\n" : "AT+CMGL=\"ALL\"\r\n",
	    unread_only ? 22 : 16);
	while (read_line(line, sizeof(line), 10000)) {
		bool text_due = n && !*out[n - 1].text;

		if (!text_due && is_news(line, "AT+CMGL")) {
			news(line);
			continue;
		}
		if (is_final(line, &err))
			break;
		if (!strncmp(line, "+CMGL:", 6)) {
			if (n == max)
				break;
			memset(&out[n], 0, sizeof(out[n]));
			parse_header(line + 6, &out[n]);
			n++;
		} else if (text_due) {
			from_module(out[n - 1].text, sizeof(out[n - 1].text), line);
		}
	}
	xSemaphoreGive(lock);
	return n;
}

/* With `peek`, the message keeps its unread mark (AT+CMGR's mode 1). */
static int read_sms(int index, struct sms *out, bool peek)
{
	char cmd[32], reply[REPLY_MAX];
	const char *p;
	int ret;

	memset(out, 0, sizeof(*out));
	if ((ret = text_mode()))
		return ret;
	snprintf(cmd, sizeof(cmd), peek ? "AT+CMGR=%d,1" : "AT+CMGR=%d", index);
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
		from_module(out->text, sizeof(out->text), p + 1);
	for (char *nl = strchr(out->text, '\n'); nl; nl = strchr(out->text, '\n'))
		*nl = ' ';				/* keep it to one line */
	return 0;
}

int modem_sms_read(int index, struct sms *out)
{
	return read_sms(index, out, false);
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
	last_io_us = esp_timer_get_time();
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

/*
 * /etc/modem, a line each: "apn NAME", "user NAME", "password WORD" (from
 * `modem apn`, else menuconfig's), and "radio off" (from `modem off`).
 */
static void conf_load(void)
{
	char path[64], line[96];
	FILE *f;

	strlcpy(conf.apn, CONFIG_PT_MODEM_APN, sizeof(conf.apn));
	strlcpy(conf.user, CONFIG_PT_MODEM_USER, sizeof(conf.user));
	strlcpy(conf.pass, CONFIG_PT_MODEM_PASSWORD, sizeof(conf.pass));
	conf.radio_off = false;
	if (!mount_resolve("/etc/modem", path, sizeof(path)) || !(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "apn ", 4))
			strlcpy(conf.apn, line + 4, sizeof(conf.apn));
		else if (!strncmp(line, "user ", 5))
			strlcpy(conf.user, line + 5, sizeof(conf.user));
		else if (!strncmp(line, "password ", 9))
			strlcpy(conf.pass, line + 9, sizeof(conf.pass));
		else if (!strcmp(line, "radio off"))
			conf.radio_off = true;
	}
	fclose(f);
}

/* Written whole to a new file and renamed over the old: never half a file. */
static int conf_save(void)
{
	char path[64], tmp[72];
	FILE *f;
	int err = 0;

	if (!mount_resolve("/etc/modem", path, sizeof(path)))
		return -ENOENT;
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if (!(f = fopen(tmp, "w")))
		return -EIO;
	fprintf(f, "apn %s\n", conf.apn);
	if (*conf.user)
		fprintf(f, "user %s\npassword %s\n", conf.user, conf.pass);
	if (conf.radio_off)
		fprintf(f, "radio off\n");
	if (fclose(f))
		err = -EIO;
	if (!err && rename(tmp, path))
		err = -EIO;
	if (err)
		remove(tmp);
	return err;
}

int modem_apn(char *apn, size_t asz, char *user, size_t usz, char *pass, size_t psz)
{
	strlcpy(apn, conf.apn, asz);
	strlcpy(user, conf.user, usz);
	strlcpy(pass, conf.pass, psz);
	return 1;
}

int modem_set_apn(const char *apn, const char *user, const char *pass)
{
	strlcpy(conf.apn, apn, sizeof(conf.apn));
	strlcpy(conf.user, user ? user : "", sizeof(conf.user));
	strlcpy(conf.pass, pass ? pass : "", sizeof(conf.pass));
	return conf_save();
}

/*
 * The radio on or off (AT+CFUN=1 or 0): off, the module takes about
 * 0.7 mA and hears nothing. Kept in /etc/modem, and made so again when
 * the module restarts.
 */
int modem_radio(bool on)
{
	int err = 0, saved;

	/* kept even if the module is not answering: it is made so when it does */
	conf.radio_off = !on;
	if (present)
		err = modem_at(on ? "AT+CFUN=1" : "AT+CFUN=0", NULL, 0, 10000);
	saved = conf_save();
	return err ? err : saved;
}

bool modem_radio_on(void)
{
	return !conf.radio_off;
}

/*
 * Before the board switches itself off (`poweroff`, or a flat cell): the
 * module stays on the cell's wires whatever the board does, so its radio
 * goes off too, not to be drained for days by a board that is off. Not
 * kept: at the next start the driver puts it back as /etc/modem says.
 */
void modem_power_off(void)
{
	if (present && !data_mode && !conf.radio_off && !modem_at("AT+CFUN=0", NULL, 0, 5000))
		klog("modem: radio off while the board is");
}

/*
 * What the module draws from the cell, for the battery's estimate: it is
 * on the cell's wires, and nothing measures it. Figures from SIMCom's
 * SIM800 sheet, rounded up: asleep and registered about 1.5 mA, awake 20,
 * a data session 150 on average, the radio off 0.7.
 */
int modem_load_ma(void)
{
	if (!present)
		return 0;
	if (data_mode)
		return 150;
	if (conf.radio_off)
		return 1;
	return sleepy && esp_timer_get_time() - last_io_us > 5000000 ? 2 : 20;
}

/* A USSD answer's text: GSM or 8859-1 as it is, UCS2 (scheme 72) from its hex. */
static void ussd_text(const char *raw, char *out, size_t size)
{
	char text[sizeof(ussd)];
	const char *comma = strrchr(raw, ',');
	int dcs = comma ? atoi(comma + 1) : 15;
	size_t n = 0;

	quoted(raw, text, sizeof(text));
	if (dcs != 72) {
		from_module(out, size, text);
		return;
	}
	for (const char *p = text; p[0] && p[1] && p[2] && p[3] && n + 4 < size; p += 4) {
		char hex[5] = { p[0], p[1], p[2], p[3], 0 };
		unsigned c = (unsigned)strtoul(hex, NULL, 16);

		if (c < 0x80) {
			out[n++] = (char)c;
		} else if (c < 0x800) {
			out[n++] = (char)(0xc0 | c >> 6);
			out[n++] = (char)(0x80 | (c & 0x3f));
		} else {
			out[n++] = (char)(0xe0 | c >> 12);
			out[n++] = (char)(0x80 | (c >> 6 & 0x3f));
			out[n++] = (char)(0x80 | (c & 0x3f));
		}
	}
	out[n] = '\0';
}

/*
 * A USSD code -- *123# for the credit, and whatever the network's codes
 * for its offers are. The answer comes as news some seconds after the
 * command's OK.
 */
int modem_ussd(const char *code, char *out, size_t size)
{
	char cmd[80];
	int err;

	if (!present)
		return -ENODEV;
	if (data_mode)
		return -EBUSY;
	xSemaphoreTake(ussd_done, 0);
	snprintf(cmd, sizeof(cmd), "AT+CUSD=1,\"%s\",15", code);
	if ((err = modem_at(cmd, NULL, 0, 10000)))
		return err;
	if (!xSemaphoreTake(ussd_done, pdMS_TO_TICKS(30000)))
		return -ETIMEDOUT;
	ussd_text(ussd, out, size);
	return 0;
}

int modem_data(bool on)
{
	char cmd[128], line[128], apn[64], user[32], pass[32];
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
		put("+++", 3);
		vTaskDelay(pdMS_TO_TICKS(1000));
		modem_at("ATH", NULL, 0, 5000);
		if (sleepy)
			modem_at("AT+CSCLK=2", NULL, 0, 2000);
		return 0;
	}
	if (ppp)
		return -EALREADY;

	if (conf.radio_off)
		return -ENETDOWN;
	modem_apn(apn, sizeof(apn), user, sizeof(user), pass, sizeof(pass));
	/* asleep it would lose a PPP frame's first byte: awake for the session */
	if (sleepy)
		modem_at("AT+CSCLK=0", NULL, 0, 2000);
	snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
	if ((err = modem_at(cmd, NULL, 0, 5000)))
		return err;

	/* ATD dials the packet service; the answer is CONNECT, then PPP. */
	xSemaphoreTake(lock, portMAX_DELAY);
	drain();
	put("ATD*99***1#\r\n", 13);
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
	if (*user)
		ppp_set_auth(ppp, PPPAUTHTYPE_ANY, user, pass);
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

	conf_load();
	lock = xSemaphoreCreateMutex();
	ussd_done = xSemaphoreCreateBinary();
	if (!lock || !ussd_done)
		return -ENOMEM;
	if (uart_driver_install(PORT, RX_BUFFER, 512, 16, &events, 0) ||
	    uart_param_config(PORT, &cfg) ||
	    uart_set_pin(PORT, CONFIG_PT_MODEM_TX, CONFIG_PT_MODEM_RX,
			 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)) {
		klog("modem: cannot open the serial port");
		return -EIO;
	}

	/* 6 KB: reading a message that came is a command and a message's worth */
	xTaskCreatePinnedToCore(probe_task, "kmodem", 6144, NULL, 3, NULL, 0);
	return 0;
}

/* One pass over the speeds; true, and the port left at it, when one answers. */
static bool answer_at(void)
{
	for (size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++) {
		if (i && bauds[i] == bauds[0])
			continue;
		uart_set_baudrate(PORT, bauds[i]);
		/* the first AT may only teach an auto-bauding module the speed */
		for (int k = 0; k < 2; k++) {
			if (!modem_at("AT", NULL, 0, 400)) {
				baud = bauds[i];
				return true;
			}
		}
	}
	uart_set_baudrate(PORT, bauds[0]);
	return false;
}

/*
 * How it is to behave, at first and again after it restarts: no echo,
 * errors with numbers, text mode, news of messages (+CMTI), of callers
 * (+CLIP) and of the network (+CREG), and a fixed speed, kept in its
 * profile, so that it says RDY at that speed when it restarts.
 */
static void setup(void)
{
	char cmd[32];

	modem_at("ATE0", NULL, 0, 1000);
	modem_at("AT+CMEE=1", NULL, 0, 1000);
	text_mode();
	modem_at("AT+CNMI=2,1,0,0,0", NULL, 0, 2000);
	modem_at("AT+CLIP=1", NULL, 0, 2000);
	modem_at("AT+CREG=1", NULL, 0, 2000);
	snprintf(cmd, sizeof(cmd), "AT+IPR=%d", baud);
	if (!modem_at(cmd, NULL, 0, 2000))
		modem_at("AT&W", NULL, 0, 2000);
	/* as /etc/modem has it; at the next start after `poweroff`, on again */
	modem_at(conf.radio_off ? "AT+CFUN=0" : "AT+CFUN=1", NULL, 0, 10000);
	/*
	 * Asleep whenever the port is quiet: about 1.5 mA registered instead
	 * of 20, and a message or a call still wakes it. Not while PPP runs.
	 */
	sleepy = !data_mode && !modem_at("AT+CSCLK=2", NULL, 0, 2000);
}

/* What it is, once it answers. */
static void identify(void)
{
	static bool registered;
	static char reply[REPLY_MAX];		/* on kmodem or a prober: one at a time */

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
	setup();
	if (!registered)
		proc_register("modem", gen_proc_modem);
	registered = true;
	present = true;
	klog("modem: %s on TX %d / RX %d at %d baud%s%s", *model ? model : "module",
	     CONFIG_PT_MODEM_TX, CONFIG_PT_MODEM_RX, baud, *imei ? ", imei " : "", imei);
}

/*
 * Modules take their time: a SIM7600 is ten seconds from power to its
 * first answer, and some need the first few commands thrown away. So the
 * probe at boot runs on its own task and the rest of the system boots.
 */
static void probe_task(void *arg)
{
	probing = true;
	for (int i = 0; i < PROBE_ROUNDS && !present; i++) {
		if (answer_at())
			identify();
		else
			vTaskDelay(pdMS_TO_TICKS(1000));
	}
	probing = false;
	if (!present)
		klog("modem: nothing answers on TX %d / RX %d, at any speed from %d to %d baud",
		     CONFIG_PT_MODEM_TX, CONFIG_PT_MODEM_RX, 9600, 115200);
	watch();
}

/*
 * What news leaves to be done, with the port let go: set a restarted
 * module up again, look for one that has just started, and say who a
 * message that came is from (read without taking its unread mark).
 */
static void after_news(void)
{
	static struct sms m;			/* kmodem's only */
	char note[64];
	int index;

	if (!present && heard_start) {
		heard_start = false;
		modem_probe();
	}
	if (present && resetup && (booted || esp_timer_get_time() >= setup_at)) {
		resetup = false;
		setup();
		if (!*model)
			identify();
	}
	if (present && (index = new_sms) >= 0) {
		new_sms = -1;
		if (!read_sms(index, &m, true) && *m.from)
			snprintf(note, sizeof(note), "\u2709 %s", m.from);
		else
			snprintf(note, sizeof(note), "\u2709 a new message");
		vt_note(note);
	}
}

/*
 * Whether the module still answers, looked at now and then: an unplugged
 * one would otherwise count in the battery's estimate for ever, and
 * `modem` would wait on it. Gone, it is looked for again by `modem`.
 */
static void still_there(void)
{
	for (int i = 0; i < 3; i++)
		if (!modem_at("AT", NULL, 0, 1000))
			return;
	present = false;
	klog("modem: the module does not answer any more; `modem` looks for it again");
}

/*
 * kmodem, from the end of the look at boot: whenever the port receives
 * something with no command waiting for it, that is news.
 */
static void watch(void)
{
	uart_event_t ev;

	for (;;) {
		/* a second while a restarted module waits to be set up again,
		 * five minutes otherwise: whether it is still there at all */
		if (!xQueueReceive(events, &ev, pdMS_TO_TICKS(resetup ? 1000 : 300000))) {
			after_news();
			if (present && !resetup && !data_mode && !probing)
				still_there();
			continue;
		}
		if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
			if (!data_mode)
				uart_flush_input(PORT);
			xQueueReset(events);
			continue;
		}
		if (ev.type != UART_DATA || data_mode || probing)
			continue;
		xSemaphoreTake(lock, portMAX_DELAY);
		if (!data_mode)
			drain();
		xSemaphoreGive(lock);
		after_news();
	}
}

/*
 * Looks again, for a module plugged in or powered after boot: `modem`
 * calls this when there is none. -EINPROGRESS while the boot's look is
 * still going on.
 */
int modem_probe(void)
{
	if (present)
		return 0;
	if (!lock)
		return -ENODEV;
	if (probing)
		return -EINPROGRESS;
	probing = true;
	for (int i = 0; i < 2 && !present; i++)
		if (answer_at())
			identify();
	probing = false;
	return present ? 0 : -ENODEV;
}

#else /* !CONFIG_PT_MODEM */

int  modem_init(void) { return -ENODEV; }
int  modem_probe(void) { return -ENODEV; }
const char *modem_network_text(int reg) { return "unknown"; }
int  modem_ussd(const char *code, char *out, size_t size) { return -ENODEV; }
int  modem_apn(char *apn, size_t asz, char *user, size_t usz, char *pass, size_t psz) { return 0; }
int  modem_set_apn(const char *apn, const char *user, const char *pass) { return -ENODEV; }
int  modem_radio(bool on) { return -ENODEV; }
bool modem_radio_on(void) { return false; }
void modem_power_off(void) { }
int  modem_load_ma(void) { return 0; }
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
