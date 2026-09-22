/*
 * A Bluetooth gamepad.
 *
 * A terminal cannot say when a key is let go, which is fine for a shell
 * and hopeless for a platformer. A pad can, so this is what games should
 * be played with.
 *
 * The radio is not started until it is asked for (`pad on`), because the
 * Bluetooth stack costs tens of kilobytes of the internal RAM everything
 * else competes for.
 *
 * Pads do not agree on what their reports look like, so the mapping from
 * bytes to buttons lives in /etc/gamepad rather than in this file:
 *
 *	# button = byte, mask      pressed when those bits are set
 *	# button = byte, <value    pressed when the byte is below it
 *	a     = 5, 0x01
 *	b     = 5, 0x02
 *	left  = 3, <0x40
 *	right = 3, >0xc0
 *
 * `pad watch` prints the raw reports as they arrive, so whatever is in
 * your hand can be mapped in a couple of minutes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_BLE_PAD

#include "esp_hidh.h"
#include "esp_hid_common.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#define HID_SERVICE_UUID	0x1812
#define SCAN_MAX		8
#define MAP_FILE		"/etc/gamepad"
#define REPORT_MAX		16

/* The buttons a game asks about, in the order pad_buttons() reports them. */
static const char *const button_names[] = {
	"up", "down", "left", "right", "a", "b", "start", "select",
};

struct rule {
	bool	 used;
	uint8_t	 byte;
	uint8_t	 value;		/* a mask, or the number to compare with */
	char	 op;		/* '&' bits set, '<' below, '>' above */
};

static struct rule	 rules[PAD_BUTTON_COUNT];
static struct pad_found	 found[SCAN_MAX];
static int		 nfound;
static uint8_t		 report[REPORT_MAX];
static int		 report_len;
static uint16_t		 buttons;
static bool		 started, connected, scanning;
static bool		 scan_everything, found_is_hid;
static bool		 watching;
static char		 pad_name[32];
static SemaphoreHandle_t scan_done;

/* ------------------------------------------------------------ mapping */

static void default_map(void)
{
	/*
	 * What a plain HID gamepad usually sends, and a guess until `pad
	 * watch` says otherwise: two axis bytes, then a byte of buttons.
	 */
	static const struct rule guess[PAD_BUTTON_COUNT] = {
		[PAD_UP]     = { true, 1, 0x40, '<' },
		[PAD_DOWN]   = { true, 1, 0xc0, '>' },
		[PAD_LEFT]   = { true, 0, 0x40, '<' },
		[PAD_RIGHT]  = { true, 0, 0xc0, '>' },
		[PAD_A]      = { true, 5, 0x20, '&' },
		[PAD_B]      = { true, 5, 0x10, '&' },
		[PAD_START]  = { true, 6, 0x08, '&' },
		[PAD_SELECT] = { true, 6, 0x04, '&' },
	};

	memcpy(rules, guess, sizeof(rules));
}

static void load_map(void)
{
	char path[64], line[96];
	FILE *f;

	default_map();
	if (!mount_resolve(MAP_FILE, path, sizeof(path)))
		return;
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char name[16];
		unsigned byte, value;
		char op = '&';
		char *eq = strchr(line, '=');
		const char *v;

		if (line[0] == '#' || !eq)
			continue;
		*eq = '\0';
		if (sscanf(line, " %15s", name) != 1)
			continue;
		v = eq + 1;
		while (*v == ' ')
			v++;
		if (sscanf(v, "%u , %c%x", &byte, &op, &value) != 3 &&
		    sscanf(v, "%u , %x", &byte, &value) != 2)
			continue;
		if (op != '<' && op != '>')
			op = '&';
		for (int i = 0; i < PAD_BUTTON_COUNT; i++)
			if (!strcmp(name, button_names[i])) {
				rules[i] = (struct rule){ true, byte, value, op };
				break;
			}
	}
	fclose(f);
	klog("pad: mapping read from %s", MAP_FILE);
}

static void apply_report(const uint8_t *data, int len)
{
	uint16_t state = 0;

	if (len > REPORT_MAX)
		len = REPORT_MAX;
	memcpy(report, data, len);
	report_len = len;
	for (int i = 0; i < PAD_BUTTON_COUNT; i++) {
		const struct rule *r = &rules[i];
		uint8_t b;

		if (!r->used || r->byte >= len)
			continue;
		b = data[r->byte];
		if (r->op == '&' ? (b & r->value) : r->op == '<' ? b < r->value : b > r->value)
			state |= 1u << i;
	}
	buttons = state;
}

/* ------------------------------------------------------------ the radio */

static void hidh_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	esp_hidh_event_data_t *p = data;

	switch ((esp_hidh_event_t)id) {
	case ESP_HIDH_OPEN_EVENT:
		connected = esp_hidh_dev_exists(p->open.dev);
		if (connected) {
			const char *name = esp_hidh_dev_name_get(p->open.dev);

			strlcpy(pad_name, name ? name : "gamepad", sizeof(pad_name));
			klog("pad: %s connected", pad_name);
			load_map();
		}
		break;
	case ESP_HIDH_INPUT_EVENT:
		apply_report(p->input.data, p->input.length);
		break;
	case ESP_HIDH_CLOSE_EVENT:
		connected = false;
		buttons = 0;
		klog("pad: %s disconnected", pad_name);
		break;
	default:
		break;
	}
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
	struct ble_hs_adv_fields fields;

	if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
		scanning = false;
		xSemaphoreGive(scan_done);
		return 0;
	}
	if (event->type != BLE_GAP_EVENT_DISC)
		return 0;
	if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data))
		return 0;

	/* Only things that say they are a keyboard, mouse or pad -- unless
	 * we were asked for everything, which is how you tell a radio that
	 * hears nothing from a room with nothing in it. */
	bool is_hid = fields.appearance_is_present && (fields.appearance >> 6) == 0x0f;

	for (int i = 0; !is_hid && i < fields.num_uuids16; i++)
		is_hid = ble_uuid_u16(&fields.uuids16[i].u) == HID_SERVICE_UUID;
	if (!is_hid && !scan_everything)
		return 0;
	found_is_hid = is_hid;

	for (int i = 0; i < nfound; i++)
		if (!memcmp(found[i].addr, event->disc.addr.val, 6))
			return 0;		/* already seen */
	if (nfound == SCAN_MAX)
		return 0;

	memcpy(found[nfound].addr, event->disc.addr.val, 6);
	found[nfound].addr_type = event->disc.addr.type;
	found[nfound].rssi = event->disc.rssi;
	found[nfound].is_hid = found_is_hid;
	if (fields.name_len)
		snprintf(found[nfound].name, sizeof(found[nfound].name), "%.*s",
			 fields.name_len, (const char *)fields.name);
	else
		strlcpy(found[nfound].name, "(no name)", sizeof(found[nfound].name));
	nfound++;
	return 0;
}

static void on_sync(void)
{
	ble_hs_util_ensure_addr(0);
	started = true;
}

static void host_task(void *arg)
{
	nimble_port_run();
	nimble_port_freertos_deinit();
}

/* Brings the controller and host up; non-zero when there is no room. */
static int radio_up(void)
{
	if (nimble_port_init() != ESP_OK)
		return -ENOMEM;
	ble_hs_cfg.sync_cb = on_sync;
	ble_hs_cfg.sm_bonding = 1;
	ble_hs_cfg.sm_sc = 1;
	ble_hs_cfg.sm_our_key_dist = 3;
	ble_hs_cfg.sm_their_key_dist = 3;
	nimble_port_freertos_init(host_task);
	return 0;
}

int pad_start(void)
{
	esp_hidh_config_t cfg = {
		.callback = hidh_event,
		.event_stack_size = 4096,
		.callback_arg = NULL,
	};

	if (started)
		return 0;
	/*
	 * Both radios want internal RAM, and with NimBLE's own
	 * allocations in PSRAM they usually both fit. When they do not,
	 * the one that has to go is the one not being used -- somebody
	 * asking for a gamepad wants to play -- and `wifi on` brings it
	 * back afterwards.
	 */
	if (radio_up() && wifi_started()) {
		klog("pad: not enough memory beside Wi-Fi; turning it off");
		wifi_radio(false);
		vTaskDelay(pdMS_TO_TICKS(300));
		if (radio_up()) {
			klog("pad: the Bluetooth radio would not start");
			return -ENOMEM;
		}
	}
	scan_done = scan_done ? scan_done : xSemaphoreCreateBinary();
	if (!scan_done)
		return -ENOMEM;
	if (esp_hidh_init(&cfg) != ESP_OK) {
		klog("pad: the HID host would not start");
		return -EIO;
	}
	for (int i = 0; i < 50 && !started; i++)
		vTaskDelay(pdMS_TO_TICKS(100));
	if (!started)
		return -ETIMEDOUT;
	load_map();
	klog("pad: Bluetooth up, looking for a gamepad");
	return 0;
}

bool pad_started(void)
{
	return started;
}

int pad_scan(struct pad_found *out, int max, int seconds)
{
	return pad_scan_all(out, max, seconds, false);
}

int pad_scan_all(struct pad_found *out, int max, int seconds, bool everything)
{
	struct ble_gap_disc_params params = { .filter_duplicates = 1, .passive = 0 };
	uint8_t own_type;
	int n;

	if (!started)
		return -ENODEV;
	if (scanning)
		return -EBUSY;
	nfound = 0;
	scanning = true;
	scan_everything = everything;
	if (ble_hs_id_infer_auto(0, &own_type) ||
	    ble_gap_disc(own_type, seconds * 1000, &params, gap_event, NULL)) {
		scanning = false;
		return -EIO;
	}
	xSemaphoreTake(scan_done, pdMS_TO_TICKS(seconds * 1000 + 2000));
	n = nfound < max ? nfound : max;
	memcpy(out, found, n * sizeof(*out));
	return n;
}

int pad_connect(int index)
{
	struct pad_found *p;

	if (!started)
		return -ENODEV;
	if (index < 1 || index > nfound)
		return -ENOENT;
	p = &found[index - 1];
	klog("pad: connecting to %s", p->name);
	if (!esp_hidh_dev_open(p->addr, ESP_HID_TRANSPORT_BLE, p->addr_type))
		return -EIO;
	return 0;
}

bool pad_connected(void)
{
	return connected;
}

uint16_t pad_buttons(void)
{
	return connected ? buttons : 0;
}

const char *pad_device_name(void)
{
	return pad_name;
}

int pad_last_report(uint8_t *out, int max)
{
	int n = report_len < max ? report_len : max;

	memcpy(out, report, n);
	return n;
}

void pad_watch(bool on)
{
	watching = on;
}

#else

int  pad_start(void) { return -ENODEV; }
bool pad_started(void) { return false; }
int  pad_scan(struct pad_found *out, int max, int seconds) { return -ENODEV; }
int  pad_scan_all(struct pad_found *out, int max, int seconds, bool all) { return -ENODEV; }
int  pad_connect(int index) { return -ENODEV; }
bool pad_connected(void) { return false; }
uint16_t pad_buttons(void) { return 0; }
const char *pad_device_name(void) { return ""; }
int  pad_last_report(uint8_t *out, int max) { return 0; }
void pad_watch(bool on) { }

#endif
