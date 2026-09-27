/*
 * Network commands: wifi, ntp, ping, modem and sms.
 *
 *	wifi                    what the radio is doing
 *	wifi scan               what is in range
 *	wifi connect ssid pass  join it, and remember it in /etc/wifi
 *	wifi forget ssid        stop remembering it
 *	wifi off / wifi on      the radio itself
 *	ntp                     set the clock from the network
 *	ping host               are we really on?
 *	modem                   the module, the SIM and the network
 *	modem data on           mobile internet, over PPP
 *	sms send 39... hello    text messages
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "util.h"

#if CONFIG_PT_WIFI

#include "lwip/netdb.h"
#include "ping/ping_sock.h"

#define SCAN_MAX	20
#define CONNECT_MS	20000

static int no_radio(const char *prog)
{
	pt_dprintf(PT_STDERR, "%s: the radio is off; `wifi on` starts it\n", prog);
	return 1;
}

/* Scanning and joining want the radio: they switch it on, as nmcli does. */
static int radio_wanted(void)
{
	int ret;

	if (wifi_started())
		return 0;
	if ((ret = wifi_radio(true)))
		return ret == -ENODEV ? no_radio("wifi") : fail("wifi", "on", ret);
	pt_printf("wifi: radio on\n");
	return 0;
}

/* Signal strength the way a phone shows it, because dBm means little. */
static const char *bars(int rssi)
{
	if (rssi >= -55)
		return "####";
	if (rssi >= -67)
		return "### ";
	if (rssi >= -78)
		return "##  ";
	return "#   ";
}

static int show_status(void)
{
	struct wifi_info w;
	int ret = wifi_state(&w);

	if (ret)
		return no_radio("wifi");
	if (!*w.ssid) {
		pt_printf("wifi: idle, no network\n");
		return 0;
	}
	if (!w.up && !wifi_radio_on()) {
		pt_printf("wifi: no saved network in range; the radio rests\n"
			  "      between tries (`wifi on` tries now)\n");
		return 0;
	}
	if (!w.up) {
		pt_printf("wifi: connecting to %s\n", w.ssid);
		return 0;
	}
	pt_printf("wifi: %s, %s %d dBm, channel %d\n", w.ssid, bars(w.rssi), w.rssi, w.channel);
	pt_printf("      %s  gateway %s  netmask %s\n", w.ip, w.gateway, w.netmask);
	return 0;
}

static int do_scan(void)
{
	struct wifi_ap *aps = pt_malloc(sizeof(*aps) * SCAN_MAX);
	int n;

	if (!aps)
		return fail("wifi", NULL, -ENOMEM);
	if (radio_wanted()) {
		pt_free(aps);
		return 1;
	}
	pt_printf("scanning...\n");
	n = wifi_scan(aps, SCAN_MAX);
	if (n < 0) {
		pt_free(aps);
		return n == -ENODEV ? no_radio("wifi") : fail("wifi", "scan", n);
	}
	for (int i = 0; i < n; i++)
		pt_printf("%2d  %-28s %s %4d dBm  ch %-3d %s\n", i + 1, aps[i].ssid,
			  bars(aps[i].rssi), aps[i].rssi, aps[i].channel,
			  aps[i].secure ? "locked" : "open");
	pt_printf("%d network%s\n", n, n == 1 ? "" : "s");
	pt_free(aps);
	return 0;
}

static int do_connect(int argc, char **argv)
{
	const char *ssid = argv[2], *pass = argc > 3 ? argv[3] : NULL;
	struct wifi_ap ap;
	int ret;

	if (argc > 4) {
		pt_dprintf(PT_STDERR, "usage: wifi connect ssid|#n [password]\n");
		return 2;
	}
	/* "#3" is the third line of the last scan: names with spaces,
	 * quotes or emoji in them never have to be typed. */
	if (ssid[0] == '#') {
		if (wifi_scan_get(atoi(ssid + 1), &ap)) {
			pt_dprintf(PT_STDERR, "wifi: %s: no such line in the last scan"
					      " (run `wifi scan` first)\n", ssid);
			return 1;
		}
		ssid = ap.ssid;
	}
	if (radio_wanted())
		return 1;
	pt_printf("connecting to %s...\n", ssid);
	ret = wifi_connect(ssid, pass, CONNECT_MS);
	if (ret == -ENODEV)
		return no_radio("wifi");
	if (ret) {
		pt_dprintf(PT_STDERR, "wifi: %s: %s\n", ssid,
			   ret == -ETIMEDOUT ? "no answer" : "could not join");
		return 1;
	}
	if ((ret = wifi_save(ssid, pass)))
		pt_dprintf(PT_STDERR, "wifi: cannot save the network (%s)\n", strerror(-ret));
	return show_status();		/* the clock sets itself: see dmesg */
}

PT_COMPLETE(wifi, ": scan connect forget disconnect on off\n")

PT_PROGRAM(wifi, "join a wireless network\n"
	   "usage: wifi [scan | connect ssid|#n [password] | forget ssid |\n"
	   "             disconnect | on | off]\n"
	   "With no arguments, says what the radio is doing. Networks that\n"
	   "connect are saved in /etc/wifi and tried again at every boot.\n"
	   "`wifi connect #3 pass` joins the third network the last scan\n"
	   "found, which saves typing a name full of spaces or symbols.")
{
	const char *cmd = argc > 1 ? argv[1] : "";

	if (argc == 1)
		return show_status();
	if (!strcmp(cmd, "scan") && argc == 2)
		return do_scan();
	if (!strcmp(cmd, "connect") && argc >= 3)
		return do_connect(argc, argv);
	if (!strcmp(cmd, "forget") && argc == 3) {
		int ret = wifi_forget(argv[2]);

		if (ret)
			pt_dprintf(PT_STDERR, "wifi: %s: not saved\n", argv[2]);
		return ret ? 1 : 0;
	}
	if (!strcmp(cmd, "disconnect") && argc == 2)
		return wifi_disconnect() ? no_radio("wifi") : 0;
	if ((!strcmp(cmd, "on") || !strcmp(cmd, "off")) && argc == 2) {
		int ret = wifi_radio(cmd[1] == 'n');

		if (ret)
			return fail("wifi", cmd, ret);
		pt_printf("wifi: radio %s\n", cmd);
		return 0;
	}
	pt_dprintf(PT_STDERR, "usage: wifi [scan | connect ssid [password] | forget ssid |"
			      " disconnect | on | off]\n");
	return 2;
}

PT_PROGRAM(ntp, "set the clock from the network now\n"
	   "usage: ntp\n"
	   "It is set by itself whenever Wi-Fi connects, and every hour\n"
	   "after; this asks at once. The server is set in menuconfig.")
{
	int ret;
	time_t now;

	if (argc != 1) {
		pt_dprintf(PT_STDERR, "usage: ntp\n");
		return 2;
	}
	if (!wifi_up()) {
		pt_dprintf(PT_STDERR, "ntp: no network\n");
		return 1;
	}
	ret = wifi_ntp_sync(10000);
	if (ret) {
		pt_dprintf(PT_STDERR, "ntp: %s did not answer\n", CONFIG_PT_NTP_SERVER);
		return 1;
	}
	now = time(NULL);
	pt_printf("%s", ctime(&now));
	return 0;
}

/* ------------------------------------------------------------ ping */

/*
 * The replies arrive on the ping task, not on ours, and a program's
 * standard output belongs to the program, so the callbacks leave the
 * numbers here and this process prints them.
 */
#define PING_QUEUE	16

struct ping_reply {
	uint32_t	seq, ttl, ms, size;
	char		from[40];
};

struct ping_run {
	struct ping_reply queue[PING_QUEUE];
	volatile uint32_t written, read;	/* a reply ring, one writer */
	uint32_t	sent, received, total_ms;
	SemaphoreHandle_t done;
};

static void ping_reply_cb(esp_ping_handle_t h, void *arg)
{
	struct ping_run *r = arg;
	struct ping_reply *q = &r->queue[r->written % PING_QUEUE];
	ip_addr_t addr;

	esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &q->seq, sizeof(q->seq));
	esp_ping_get_profile(h, ESP_PING_PROF_TTL, &q->ttl, sizeof(q->ttl));
	esp_ping_get_profile(h, ESP_PING_PROF_IPADDR, &addr, sizeof(addr));
	esp_ping_get_profile(h, ESP_PING_PROF_SIZE, &q->size, sizeof(q->size));
	esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &q->ms, sizeof(q->ms));
	strlcpy(q->from, ipaddr_ntoa(&addr), sizeof(q->from));
	r->received++;
	r->total_ms += q->ms;
	r->written++;
}

static void ping_timeout_cb(esp_ping_handle_t h, void *arg)
{
	struct ping_run *r = arg;
	struct ping_reply *q = &r->queue[r->written % PING_QUEUE];

	esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &q->seq, sizeof(q->seq));
	q->ms = 0;
	q->from[0] = '\0';			/* the mark of a lost one */
	r->written++;
}

static void ping_end_cb(esp_ping_handle_t h, void *arg)
{
	struct ping_run *r = arg;

	esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &r->sent, sizeof(r->sent));
	xSemaphoreGive(r->done);
}

static void ping_drain(struct ping_run *r)
{
	while (r->read != r->written) {
		const struct ping_reply *q = &r->queue[r->read % PING_QUEUE];

		if (*q->from)
			pt_printf("%lu bytes from %s: seq=%lu ttl=%lu time=%lu ms\n",
				  (unsigned long)q->size, q->from, (unsigned long)q->seq,
				  (unsigned long)q->ttl, (unsigned long)q->ms);
		else
			pt_printf("no answer to seq=%lu\n", (unsigned long)q->seq);
		r->read++;
	}
}

PT_PROGRAM(ping, "see whether a host answers\n"
	   "usage: ping [-c count] host")
{
	esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
	esp_ping_callbacks_t cbs = { .on_ping_success = ping_reply_cb,
				     .on_ping_timeout = ping_timeout_cb,
				     .on_ping_end = ping_end_cb };
	struct ping_run run = { 0 };
	esp_ping_handle_t ping;
	struct addrinfo hints = { .ai_family = AF_INET }, *res;
	const char *host;
	int i = 1;

	if (argc > 2 && !strcmp(argv[1], "-c")) {
		cfg.count = atoi(argv[2]);
		i = 3;
	}
	if (i + 1 != argc || cfg.count < 1) {
		pt_dprintf(PT_STDERR, "usage: ping [-c count] host\n");
		return 2;
	}
	host = argv[i];
	if (!wifi_up()) {
		pt_dprintf(PT_STDERR, "ping: no network\n");
		return 1;
	}
	if (getaddrinfo(host, NULL, &hints, &res) || !res) {
		pt_dprintf(PT_STDERR, "ping: %s: cannot look up that name\n", host);
		return 1;
	}
	struct in_addr a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	inet_addr_to_ip4addr(ip_2_ip4(&cfg.target_addr), &a);
	cfg.target_addr.type = IPADDR_TYPE_V4;
	freeaddrinfo(res);

	run.done = xSemaphoreCreateBinary();
	cbs.cb_args = &run;
	if (!run.done || esp_ping_new_session(&cfg, &cbs, &ping)) {
		pt_dprintf(PT_STDERR, "ping: cannot start\n");
		return 1;
	}
	/*
	 * The session is a task of ESP-IDF's writing into `run`, which is on
	 * this program's stack: it has to be over before this returns. So
	 * Ctrl-C is caught rather than let end the program at its next
	 * printf, and after a stop the session is waited for -- it finishes
	 * the ping it is on, a second at most, and then says it has ended.
	 */
	pt_sigcatch(true);
	pt_printf("PING %s\n", host);
	esp_ping_start(ping);
	while (!xSemaphoreTake(run.done, pdMS_TO_TICKS(100))) {
		ping_drain(&run);
		if (pt_interrupted()) {
			esp_ping_stop(ping);
			xSemaphoreTake(run.done, pdMS_TO_TICKS(5000));
			break;
		}
	}
	ping_drain(&run);
	pt_printf("%lu sent, %lu received%s", (unsigned long)run.sent,
		  (unsigned long)run.received, run.received ? ", " : "\n");
	if (run.received)
		pt_printf("average %lu ms\n", (unsigned long)(run.total_ms / run.received));
	esp_ping_delete_session(ping);
	vSemaphoreDelete(run.done);
	return run.received ? 0 : 1;
}

#endif /* CONFIG_PT_WIFI */

/* ------------------------------------------------------------ the modem */

#if CONFIG_PT_MODEM

#define SMS_MAX		20

static int no_modem(const char *prog)
{
	pt_dprintf(PT_STDERR, "%s: no modem answers on the serial header\n", prog);
	return 1;
}

static int modem_status(void)
{
	struct modem_info m;

	if (modem_info(&m))
		return no_modem("modem");
	pt_printf("module   %s\n", *m.model ? m.model : "unknown");
	pt_printf("imei     %s\n", *m.imei ? m.imei : "-");
	pt_printf("sim      %s\n", m.sim);
	pt_printf("network  %s%s%s\n", m.registered ? "registered" : "searching",
		  *m.operator ? " on " : "", m.operator);
	if (m.rssi)
		pt_printf("signal   %d dBm\n", m.rssi);
	pt_printf("data     %s\n", m.data ? "up" : "down");
	return 0;
}

PT_COMPLETE(modem, ": data at\ndata: on off\n")

PT_PROGRAM(modem, "talk to the mobile module\n"
	   "usage: modem [data on | data off | at COMMAND]\n"
	   "With no arguments, shows the module, the SIM and the network.\n"
	   "`data on` brings up the mobile internet over PPP; the APN is\n"
	   "set in menuconfig. `at` sends a command straight through.")
{
	char reply[1024];
	int ret;

	if (argc == 1)
		return modem_status();
	if (!modem_present())
		return no_modem("modem");
	if (argc == 3 && !strcmp(argv[1], "data")) {
		bool on = !strcmp(argv[2], "on");

		if (!on && strcmp(argv[2], "off")) {
			pt_dprintf(PT_STDERR, "usage: modem data on|off\n");
			return 2;
		}
		pt_printf("%s the mobile data link...\n", on ? "bringing up" : "taking down");
		ret = modem_data(on);
		if (ret == -EALREADY) {
			pt_dprintf(PT_STDERR, "modem: the link is already up\n");
			return 1;
		}
		if (ret) {
			pt_dprintf(PT_STDERR, "modem: %s\n", ret == -ETIMEDOUT ?
				   "the network never finished the connection" :
				   "the link would not come up");
			return 1;
		}
		return modem_status();
	}
	if (argc >= 2 && !strcmp(argv[1], "at")) {
		char cmd[160] = "AT";

		for (int i = 2; i < argc; i++) {
			strlcat(cmd, i > 2 ? " " : "", sizeof(cmd));
			strlcat(cmd, argv[i], sizeof(cmd));
		}
		ret = modem_at(cmd, reply, sizeof(reply), 15000);
		pt_printf("%s", reply);
		if (ret)
			pt_dprintf(PT_STDERR, "modem: %s\n",
				   ret == -EBUSY ? "the port is carrying the data link" :
				   ret == -ETIMEDOUT ? "no answer" : "the module said no");
		return ret ? 1 : 0;
	}
	pt_dprintf(PT_STDERR, "usage: modem [data on | data off | at COMMAND]\n");
	return 2;
}

static int sms_list(bool unread_only)
{
	struct sms *list = pt_malloc(sizeof(*list) * SMS_MAX);
	int n;

	if (!list)
		return fail("sms", NULL, -ENOMEM);
	n = modem_sms_list(list, SMS_MAX, unread_only);
	if (n < 0) {
		pt_free(list);
		return n == -ENODEV ? no_modem("sms") : fail("sms", "list", n);
	}
	for (int i = 0; i < n; i++)
		pt_printf("%3d %c %-18s %s  %s\n", list[i].index, list[i].unread ? '*' : ' ',
			  list[i].from, list[i].when, list[i].text);
	pt_printf("%d message%s%s\n", n, n == 1 ? "" : "s", unread_only ? " unread" : "");
	pt_free(list);
	return 0;
}

PT_COMPLETE(sms, ": new\n")

PT_PROGRAM(sms, "text messages\n"
	   "usage: sms [new]\n"
	   "       sms send number text...\n"
	   "       sms read index\n"
	   "       sms delete index\n"
	   "A * in the list marks a message that has not been read yet.")
{
	const char *cmd = argc > 1 ? argv[1] : "";
	struct sms m;
	int ret;

	if (argc == 1)
		return sms_list(false);
	if (!strcmp(cmd, "new") && argc == 2)
		return sms_list(true);
	if (!modem_present())
		return no_modem("sms");
	if (!strcmp(cmd, "send") && argc >= 4) {
		char text[161] = "";

		for (int i = 3; i < argc; i++) {
			strlcat(text, i > 3 ? " " : "", sizeof(text));
			strlcat(text, argv[i], sizeof(text));
		}
		pt_printf("sending to %s...\n", argv[2]);
		ret = modem_sms_send(argv[2], text);
		if (ret) {
			pt_dprintf(PT_STDERR, "sms: not sent (%s)\n",
				   ret == -EBUSY ? "the data link has the port" : "the network refused");
			return 1;
		}
		pt_printf("sent\n");
		return 0;
	}
	if (!strcmp(cmd, "read") && argc == 3) {
		if ((ret = modem_sms_read(atoi(argv[2]), &m)))
			return fail("sms", argv[2], ret);
		pt_printf("from %s  %s\n\n%s\n", m.from, m.when, m.text);
		return 0;
	}
	if (!strcmp(cmd, "delete") && argc == 3) {
		if ((ret = modem_sms_delete(atoi(argv[2]))))
			return fail("sms", argv[2], ret);
		return 0;
	}
	pt_dprintf(PT_STDERR, "usage: sms [new | send number text... | read index | delete index]\n");
	return 2;
}

#endif /* CONFIG_PT_MODEM */
