/*
 * A Bluetooth gamepad.
 *
 * A terminal cannot say when a key is let go, which is fine for a shell
 * and hopeless for a platformer. A pad can, so this is what games should
 * be played with.
 *
 * The radio is net/ble.c's, started when a pad is asked for (`pad on`);
 * this is the HID host on it, and what a pad's reports mean.
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

#if !CONFIG_BT_NIMBLE_HID_SERVICE
#error "Gamepads need NimBLE's HID service, CONFIG_BT_NIMBLE_HID_SERVICE (boards/fragments/ble.config sets it)"
#endif

#include "esp_hidh.h"
#include "esp_hid_common.h"

#define MAP_FILE		"/etc/gamepad"
#define REPORT_MAX		16
#define CLOSE_WAIT_MS		3000

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
static uint8_t		 report[REPORT_MAX];
static int		 report_len;
static uint16_t		 buttons;
static bool		 hidh_up;
static volatile bool	 connected;
static esp_hidh_dev_t	*dev;
static char		 pad_name[32];

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

/* ------------------------------------------------------------ the HID host */

static void hidh_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	esp_hidh_event_data_t *p = data;

	switch ((esp_hidh_event_t)id) {
	case ESP_HIDH_OPEN_EVENT:
		connected = esp_hidh_dev_exists(p->open.dev);
		if (connected) {
			const char *name = esp_hidh_dev_name_get(p->open.dev);

			dev = p->open.dev;
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
		dev = NULL;
		buttons = 0;
		klog("pad: %s disconnected", pad_name);
		break;
	default:
		break;
	}
}

int pad_start(void)
{
	esp_hidh_config_t cfg = {
		.callback = hidh_event,
		.event_stack_size = 4096,
		.callback_arg = NULL,
	};
	int ret;

	ble_hold();
	if ((ret = ble_start()) || hidh_up)
		goto out;
	if (esp_hidh_init(&cfg) != ESP_OK) {
		klog("pad: the HID host would not start");
		ret = -EIO;
		goto out;
	}
	hidh_up = true;
	load_map();
	klog("pad: looking for a gamepad");
out:
	ble_release();
	return ret;
}

/* Called by ble_stop(), the radio held: a pad still connected is let go. */
int pad_stop(void)
{
	if (!hidh_up)
		return 0;
	if (connected && dev) {
		esp_hidh_dev_close(dev);
		for (int i = 0; i < CLOSE_WAIT_MS / 100 && connected; i++)
			vTaskDelay(pdMS_TO_TICKS(100));
	}
	if (esp_hidh_deinit() != ESP_OK)
		return -EBUSY;
	hidh_up = false;
	return 0;
}

bool pad_started(void)
{
	return hidh_up;
}

int pad_connect(int index)
{
	const struct ble_found *p;
	int ret = 0;

	ble_hold();
	if (!hidh_up || !ble_started()) {
		ret = -ENODEV;
		goto out;
	}
	if (!(p = ble_found_get(index))) {
		ret = -ENOENT;
		goto out;
	}
	klog("pad: connecting to %s", p->name);
	if (!esp_hidh_dev_open((uint8_t *)p->addr, ESP_HID_TRANSPORT_BLE, p->addr_type))
		ret = -EIO;
out:
	ble_release();
	return ret;
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

#else

int  pad_start(void) { return -ENODEV; }
int  pad_stop(void) { return 0; }
bool pad_started(void) { return false; }
int  pad_connect(int index) { return -ENODEV; }
bool pad_connected(void) { return false; }
uint16_t pad_buttons(void) { return 0; }
const char *pad_device_name(void) { return ""; }
int  pad_last_report(uint8_t *out, int max) { return 0; }

#endif
