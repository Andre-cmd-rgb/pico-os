/*
 * Wi-Fi.
 *
 * The radio comes up at boot only if there is somewhere to connect to --
 * idle, it holds 34 KB of internal memory -- and otherwise waits for `wifi
 * on` or `wifi connect`. The saved networks are in /etc/wifi, one per
 * line:
 *
 *	# my networks
 *	homenet          secretpassword
 *	"the cafe"       guest1234
 *	openhotspot
 *
 * A supplicant task does what wpa_supplicant does and no more: try the
 * saved networks in turn, and when a connection drops, wait a few seconds
 * and try again. Everything it does goes to dmesg.
 *
 * Trying costs: each attempt scans every channel with the receiver on,
 * about 90 mA for a couple of seconds. Carried away from its network the
 * board used to do that every twenty seconds for as long as it was
 * away, which drained the cell faster than the screen did. Now the wait
 * doubles after every round that finds nothing, up to ten minutes, and
 * in between the radio is stopped outright. A connection that drops, a
 * `wifi` command, or waking from sleep starts it at a few seconds again.
 * Connected, the radio sleeps between the access point's beacons,
 * waking for every third.
 *
 * Whenever an address arrives the clock is set from CONFIG_PT_NTP_SERVER,
 * and lwIP asks again every hour while the network stays up.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_WIFI

#define CONFIG_FILE	"/etc/wifi"
#define RETRY_MS	5000
#define RETRY_MAX_MS	(10 * 60 * 1000)
#define CONNECT_MS	15000
#define LISTEN_BEACONS	3	/* connected, it wakes for every third beacon */
#define WIFI_LINE	160
#define SCAN_KEEP	20	/* how many scan results stay remembered */

static esp_netif_t	*netif;
static EventGroupHandle_t events;
static SemaphoreHandle_t lock;		/* one connect or scan at a time */
static TaskHandle_t	 supplicant;
static bool		 started, want_connection;
static bool		 resting;	/* started, but stopped between tries */
static volatile bool	 paused;	/* dozing: no tries until wifi_retry_soon() */
static char		 current[33];	/* the network we are on or trying */
static int		 last_reason;
static bool		 sntp_ready;

#define BIT_GOT_IP	BIT0
#define BIT_FAILED	BIT1

static const char *reason_text(int reason)
{
	switch (reason) {
	case WIFI_REASON_NO_AP_FOUND:		return "no such network in range";
	case WIFI_REASON_AUTH_FAIL:
	case WIFI_REASON_HANDSHAKE_TIMEOUT:
	case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "wrong password";
	case WIFI_REASON_ASSOC_LEAVE:		return "disconnected";
	case WIFI_REASON_BEACON_TIMEOUT:	return "out of range";
	default:				return "connection failed";
	}
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		const wifi_event_sta_disconnected_t *e = data;

		last_reason = e->reason;
		xEventGroupClearBits(events, BIT_GOT_IP);
		xEventGroupSetBits(events, BIT_FAILED);
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		const ip_event_got_ip_t *e = data;

		klog("wifi: %s, " IPSTR, current, IP2STR(&e->ip_info.ip));
		xEventGroupClearBits(events, BIT_FAILED);
		xEventGroupSetBits(events, BIT_GOT_IP);
#if CONFIG_PT_NTP_AT_BOOT
		if (sntp_ready)
			esp_netif_sntp_start();		/* a new network: ask for the time now */
#endif
	}
}

bool wifi_up(void)
{
	return events && (xEventGroupGetBits(events) & BIT_GOT_IP);
}

bool wifi_started(void)
{
	return started;
}

/* Whether the radio is drawing current: on, and not resting between tries. */
bool wifi_radio_on(void)
{
	return started && !resting;
}

/*
 * The radio stopped between tries and started again for the next, with
 * the lock held so that nothing is scanning or connecting at the time.
 * Resting, the driver gives back its memory as well -- most of 100 KB of
 * internal RAM, which is what a game or a second terminal is short of --
 * and takes it again for the next try.
 */
static void rest_locked(bool rest)
{
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

	if (rest == resting || !started)
		return;
	if (rest) {
		if (esp_wifi_stop() == ESP_OK && esp_wifi_deinit() == ESP_OK)
			resting = true;
		return;
	}
	if (esp_wifi_init(&cfg) || esp_wifi_set_storage(WIFI_STORAGE_RAM) ||
	    esp_wifi_set_mode(WIFI_MODE_STA) || esp_wifi_start()) {
		esp_wifi_deinit();
		klog("wifi: no memory to wake the radio; trying later");
		return;
	}
	esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
	resting = false;
}

/* ------------------------------------------------------------ saved networks */

/* One line of /etc/wifi: an SSID, optionally quoted, and the rest is the
 * password. Returns false for a comment or a blank line. */
static bool parse_line(char *line, char **ssid, char **pass)
{
	char *p = line;

	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p || *p == '#')
		return false;
	if (*p == '"') {
		*ssid = ++p;
		p = strchr(p, '"');
		if (!p)
			return false;
		*p++ = '\0';
	} else {
		*ssid = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (*p)
			*p++ = '\0';
	}
	while (*p == ' ' || *p == '\t')
		p++;
	*pass = p;
	p += strlen(p);
	while (p > *pass && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '))
		*--p = '\0';
	return **ssid != '\0';
}

/* Calls `fn` for every saved network until it returns true. */
static bool each_saved(bool (*fn)(const char *ssid, const char *pass, void *ctx), void *ctx)
{
	char path[64], line[WIFI_LINE];
	FILE *f;
	bool done = false;

	if (!mount_resolve(CONFIG_FILE, path, sizeof(path)))
		return false;
	f = fopen(path, "r");
	if (!f)
		return false;
	while (!done && fgets(line, sizeof(line), f)) {
		char *ssid, *pass;

		if (parse_line(line, &ssid, &pass))
			done = fn(ssid, pass, ctx);
	}
	fclose(f);
	return done;
}

int wifi_save(const char *ssid, const char *pass)
{
	char path[64], line[WIFI_LINE];
	FILE *f;

	wifi_forget(ssid);
	if (!mount_resolve(CONFIG_FILE, path, sizeof(path)))
		return -ENOENT;
	f = fopen(path, "a");
	if (!f)
		return -EIO;
	snprintf(line, sizeof(line), "%s%s%s %s\n", strchr(ssid, ' ') ? "\"" : "", ssid,
		 strchr(ssid, ' ') ? "\"" : "", pass ? pass : "");
	fputs(line, f);
	return fclose(f) ? -EIO : 0;
}

struct saved_pass {
	const char	*ssid;
	char		*pass;
	size_t		 size;
};

static bool find_saved(const char *ssid, const char *pass, void *arg)
{
	struct saved_pass *want = arg;

	if (strcmp(ssid, want->ssid))
		return false;
	strlcpy(want->pass, pass, want->size);
	return true;
}

bool wifi_saved(const char *ssid, char *pass, size_t size)
{
	struct saved_pass want = { ssid, pass, size };

	return each_saved(find_saved, &want);
}

int wifi_forget(const char *ssid)
{
	char path[64], tmp[72], line[WIFI_LINE], copy[WIFI_LINE];
	FILE *in, *out;
	int removed = 0;

	if (!mount_resolve(CONFIG_FILE, path, sizeof(path)))
		return -ENOENT;
	in = fopen(path, "r");
	if (!in)
		return -ENOENT;
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	out = fopen(tmp, "w");
	if (!out) {
		fclose(in);
		return -EIO;
	}
	while (fgets(line, sizeof(line), in)) {
		char *name, *pass;

		strlcpy(copy, line, sizeof(copy));
		if (parse_line(copy, &name, &pass) && !strcmp(name, ssid))
			removed++;
		else
			fputs(line, out);
	}
	fclose(in);
	if (fclose(out)) {
		remove(tmp);
		return -EIO;
	}
	if (rename(tmp, path)) {		/* FAT will not rename over a file */
		remove(path);
		if (rename(tmp, path))
			return -EIO;
	}
	return removed ? 0 : -ENOENT;
}

/* ------------------------------------------------------------ connecting */

static int wait_for_ip(int timeout_ms)
{
	EventBits_t bits = xEventGroupWaitBits(events, BIT_GOT_IP | BIT_FAILED, pdFALSE,
					       pdFALSE, pdMS_TO_TICKS(timeout_ms));

	if (bits & BIT_GOT_IP)
		return 0;
	if (bits & BIT_FAILED)
		return -ECONNREFUSED;
	return -ETIMEDOUT;
}

int wifi_connect(const char *ssid, const char *pass, int timeout_ms)
{
	wifi_config_t cfg = { 0 };
	bool was_on;
	int ret;

	if (!started)
		return -ENODEV;
	if (strlen(ssid) > 32)
		return -EINVAL;
	strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
	if (pass && *pass)
		strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
	cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;	/* pick the strongest one */
	cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
	cfg.sta.listen_interval = LISTEN_BEACONS;

	xSemaphoreTake(lock, portMAX_DELAY);
	rest_locked(false);
	strlcpy(current, ssid, sizeof(current));
	want_connection = true;
	last_reason = 0;
	/*
	 * Off the network we are on, and its going heard before listening
	 * for the new one: otherwise the event for leaving it comes in late
	 * and is taken for the new one refusing us -- joining the network
	 * the board was already on said "is the password right?".
	 */
	was_on = xEventGroupGetBits(events) & BIT_GOT_IP;
	xEventGroupClearBits(events, BIT_FAILED);
	if (!esp_wifi_disconnect() && was_on)
		xEventGroupWaitBits(events, BIT_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
	xEventGroupClearBits(events, BIT_GOT_IP | BIT_FAILED);
	if (esp_wifi_set_config(WIFI_IF_STA, &cfg) || esp_wifi_connect()) {
		xSemaphoreGive(lock);
		return -EIO;
	}
	ret = wait_for_ip(timeout_ms);
	xSemaphoreGive(lock);
	if (ret)
		klog("wifi: %s: %s", ssid, reason_text(last_reason));
	return ret;
}

int wifi_disconnect(void)
{
	if (!started)
		return -ENODEV;
	want_connection = false;
	current[0] = '\0';
	xEventGroupClearBits(events, BIT_GOT_IP);
	return esp_wifi_disconnect() ? -EIO : 0;
}

static bool try_saved(const char *ssid, const char *pass, void *ctx)
{
	return wifi_connect(ssid, pass, CONNECT_MS) == 0;
}

/* Try again soon: something changed that makes it worth it. */
void wifi_retry_soon(void)
{
	paused = false;
	if (supplicant)
		xTaskNotifyGive(supplicant);
}

/*
 * The system dozes: off the network, the radio stopped and its memory
 * given back, and no tries until wifi_retry_soon() says someone is back.
 */
void wifi_rest(void)
{
	if (!started)
		return;
	paused = true;
	xSemaphoreTake(lock, portMAX_DELAY);
	if (!resting)
		esp_wifi_disconnect();
	rest_locked(true);
	xSemaphoreGive(lock);
}

/*
 * The supplicant: on the way up, and after every drop, walk the saved
 * networks until one answers. Between rounds that find none the radio
 * is stopped and the wait doubles, so a network that is simply not
 * there costs next to nothing.
 */
static void supplicant_task(void *arg)
{
	int wait = RETRY_MS;

	each_saved(try_saved, NULL);
	for (;;) {
		xEventGroupWaitBits(events, BIT_FAILED, pdFALSE, pdFALSE, portMAX_DELAY);
		if (paused) {
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);	/* until a key */
			wait = RETRY_MS;
			continue;
		}
		if (!want_connection || wifi_up()) {
			vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
			continue;
		}
		if (wait > RETRY_MS) {
			xSemaphoreTake(lock, portMAX_DELAY);
			rest_locked(true);
			xSemaphoreGive(lock);
		}
		/* woken early by wifi_retry_soon() */
		if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait)))
			wait = RETRY_MS;
		if (!want_connection || wifi_up())
			continue;
		if (each_saved(try_saved, NULL)) {
			wait = RETRY_MS;
		} else {
			wait = wait * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : wait * 2;
			if (wait >= 60 * 1000)
				klog("wifi: no saved network in range; the radio rests %d min",
				     wait / 60000);
		}
	}
}

/* ------------------------------------------------------------ scanning */

EXT_RAM_BSS_ATTR static struct wifi_ap last_scan[SCAN_KEEP];
static int last_scan_count;

/* The nth network from the last scan, so a name full of spaces or
 * emoji never has to be typed at all. */
int wifi_scan_get(int index, struct wifi_ap *out)
{
	if (index < 1 || index > last_scan_count)
		return -ENOENT;
	*out = last_scan[index - 1];
	return 0;
}

int wifi_scan(struct wifi_ap *out, int max)
{
	wifi_ap_record_t *records;
	uint16_t found = max;
	int n;

	if (!started)
		return -ENODEV;
	records = malloc(sizeof(*records) * max);
	if (!records)
		return -ENOMEM;
	xSemaphoreTake(lock, portMAX_DELAY);
	rest_locked(false);
	if (esp_wifi_scan_start(NULL, true) || esp_wifi_scan_get_ap_records(&found, records)) {
		xSemaphoreGive(lock);
		free(records);
		return -EIO;
	}
	xSemaphoreGive(lock);
	n = found;
	for (int i = 0; i < n; i++) {
		strlcpy(out[i].ssid, (const char *)records[i].ssid, sizeof(out[i].ssid));
		out[i].rssi = records[i].rssi;
		out[i].channel = records[i].primary;
		out[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
	}
	free(records);
	last_scan_count = n < SCAN_KEEP ? n : SCAN_KEEP;
	memcpy(last_scan, out, last_scan_count * sizeof(*out));
	return n;
}

/* ------------------------------------------------------------ state */

int wifi_state(struct wifi_info *out)
{
	wifi_ap_record_t ap;
	esp_netif_ip_info_t ip;

	memset(out, 0, sizeof(*out));
	if (!started)
		return -ENODEV;
	out->up = wifi_up();
	strlcpy(out->ssid, current, sizeof(out->ssid));
	if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
		strlcpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid));
		out->rssi = ap.rssi;
		out->channel = ap.primary;
	}
	if (out->up && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
		snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
		snprintf(out->gateway, sizeof(out->gateway), IPSTR, IP2STR(&ip.gw));
		snprintf(out->netmask, sizeof(out->netmask), IPSTR, IP2STR(&ip.netmask));
	}
	return 0;
}

static int gen_proc_net(char *b, size_t n)
{
	struct wifi_info w;
	uint8_t mac[6] = { 0 };

	if (wifi_state(&w))
		return snprintf(b, n, "wifi: off\n");
	esp_wifi_get_mac(WIFI_IF_STA, mac);
	return snprintf(b, n,
			"interface wlan0\n"
			"state     %s\n"
			"ssid      %s\n"
			"signal    %d dBm\n"
			"channel   %d\n"
			"mac       %02x:%02x:%02x:%02x:%02x:%02x\n"
			"address   %s\n"
			"gateway   %s\n"
			"netmask   %s\n",
			w.up ? "up" : *w.ssid ? "connecting" : "idle", *w.ssid ? w.ssid : "-",
			w.rssi, w.channel, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
			*w.ip ? w.ip : "-", *w.gateway ? w.gateway : "-",
			*w.netmask ? w.netmask : "-");
}

/* ------------------------------------------------------------ the clock */

/* On lwIP's task: nothing slow here, the saving is clock.c's to do. */
static void on_time(struct timeval *tv)
{
	(void)tv;
	clock_changed(true);
}

static void sntp_setup(void)
{
	esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_PT_NTP_SERVER);

	cfg.start = false;		/* not before there is a network */
	cfg.server_from_dhcp = false;
	cfg.sync_cb = on_time;
	sntp_ready = !esp_netif_sntp_init(&cfg);
}

/* Asks now, rather than at the next hourly turn, and waits for the answer. */
int wifi_ntp_sync(int timeout_ms)
{
	if (!wifi_up())
		return -ENETDOWN;
	if (!sntp_ready)
		return -EIO;
	esp_netif_sntp_sync_wait(0);		/* forget an earlier answer */
	if (esp_netif_sntp_start())
		return -EIO;
	return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) ? -ETIMEDOUT : 0;
}

/* ------------------------------------------------------------ boot */

/*
 * Bringing the radio up costs about 100 KB of internal RAM, which on this
 * chip is a lot, so `wifi off` gives it back properly rather than just
 * stopping: the buffers go too, and `wifi on` allocates them again.
 */
static int radio_up(void)
{
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

	if (started)
		return 0;
	if (esp_wifi_init(&cfg) ||
	    esp_wifi_set_storage(WIFI_STORAGE_RAM) ||	/* /etc/wifi is where they live */
	    esp_wifi_set_mode(WIFI_MODE_STA) ||
	    esp_wifi_start()) {
		esp_wifi_deinit();
		klog("wifi: the radio would not start");
		return -EIO;
	}
	started = true;
	resting = false;
	esp_wifi_set_ps(WIFI_PS_MAX_MODEM);		/* sleep between beacons */
	return 0;
}

static int radio_down(void)
{
	if (!started)
		return 0;
	want_connection = false;
	current[0] = '\0';
	xEventGroupClearBits(events, BIT_GOT_IP | BIT_FAILED);
	if (!resting) {
		esp_wifi_disconnect();
		if (esp_wifi_stop() || esp_wifi_deinit())
			return -EIO;
	}
	started = resting = false;
	return 0;
}

int wifi_init(void)
{
	esp_err_t err = nvs_flash_init();

	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();			/* calibration data only */
		err = nvs_flash_init();
	}
	if (err)
		klog("wifi: no NVS (%s); the radio will calibrate on every boot",
		     esp_err_to_name(err));

	events = xEventGroupCreate();
	lock = xSemaphoreCreateMutex();
	if (!events || !lock)
		return -ENOMEM;
	if (esp_netif_init() || esp_event_loop_create_default())
		return -EIO;
	netif = esp_netif_create_default_wifi_sta();
	if (!netif)
		return -EIO;
	esp_log_level_set("wifi", ESP_LOG_ERROR);	/* its own log is very chatty */
	esp_log_level_set("wifi_init", ESP_LOG_ERROR);
	esp_log_level_set("phy_init", ESP_LOG_ERROR);
	if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
						on_event, NULL, NULL) ||
	    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
						on_event, NULL, NULL))
		return -EIO;
	sntp_setup();
	proc_register("net", gen_proc_net);
	return 0;			/* the radio waits for a reason to start */
}

static bool count_saved(const char *ssid, const char *pass, void *ctx)
{
	(void)ssid;
	(void)pass;
	*(int *)ctx += 1;
	return false;			/* every line */
}

/* The task that keeps trying the saved networks. */
static void supplicant_start(void)
{
	if (supplicant)
		return;
	want_connection = true;
	ktask_create(supplicant_task, "kwifi", 3584, NULL, 4, &supplicant, 0);
}

/*
 * Called once the filesystems are mounted, which is the first moment
 * /etc/wifi can be read -- and the first moment it is possible to know
 * whether the radio has anything to connect to. An idle radio holds
 * about 34 KB of internal memory on this chip, which is a quarter of
 * what a terminal and a shell cost, so a board with no saved network
 * leaves it off until someone says `wifi on`.
 */
void wifi_start_supplicant(void)
{
	int saved = 0;

	if (supplicant)
		return;
#if CONFIG_PT_WIFI_AT_BOOT
	each_saved(count_saved, &saved);
	if (!saved) {
		klog("wifi: radio off, no saved network; `wifi on` starts it");
		return;
	}
	if (!started && radio_up())
		return;
	klog("wifi: radio up (802.11 b/g/n), %d saved network%s", saved,
	     saved == 1 ? "" : "s");
	supplicant_start();
#else
	if (started)
		supplicant_start();
#endif
}

int wifi_radio(bool on)
{
	int ret;

	if (!events)
		return -ENODEV;			/* Wi-Fi is built in but never came up */
	if (!on)
		return radio_down();
	if ((ret = radio_up()))
		return ret;
	supplicant_start();			/* `wifi on` means try, saved or not */
	want_connection = true;
	xEventGroupSetBits(events, BIT_FAILED);	/* wake the supplicant */
	wifi_retry_soon();
	return 0;
}

#else /* !CONFIG_PT_WIFI */

int  wifi_init(void) { return -ENODEV; }
void wifi_start_supplicant(void) { }
bool wifi_up(void) { return false; }
bool wifi_started(void) { return false; }
bool wifi_radio_on(void) { return false; }
void wifi_rest(void) { }
void wifi_retry_soon(void) { }
int  wifi_connect(const char *ssid, const char *pass, int timeout_ms) { return -ENODEV; }
int  wifi_disconnect(void) { return -ENODEV; }
int  wifi_scan(struct wifi_ap *out, int max) { return -ENODEV; }
int  wifi_state(struct wifi_info *out) { return -ENODEV; }
int  wifi_save(const char *ssid, const char *pass) { return -ENODEV; }
int  wifi_forget(const char *ssid) { return -ENODEV; }
bool wifi_saved(const char *ssid, char *pass, size_t size) { return false; }
int  wifi_ntp_sync(int timeout_ms) { return -ENODEV; }
int  wifi_radio(bool on) { return -ENODEV; }

#endif
