/*
 * USB keyboard on the S3's native USB port (host mode, boot protocol).
 *
 * Reports arrive in the HID driver's task and are queued; everything else,
 * including typematic repeat, happens in this driver's own task. Keys become
 * the same bytes a terminal sends: UTF-8 text, VT escape sequences for
 * cursor keys, control codes for Ctrl+letter.
 */
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_KBD_USB

#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/usb_host.h"

#define MOD_CTRL	(HID_LEFT_CONTROL | HID_RIGHT_CONTROL)
#define MOD_SHIFT	(HID_LEFT_SHIFT | HID_RIGHT_SHIFT)
#define MOD_ALTGR	HID_RIGHT_ALT

#define NOTIFY_OK	1
#define NOTIFY_FAIL	2

#if CONFIG_PT_KBD_LAYOUT_IT
#define LAYOUT_NAME	"Italian"
#else
#define LAYOUT_NAME	"US"
#endif

enum usb_event_kind {
	EV_CONNECTED,
	EV_REPORT,
	EV_DISCONNECTED,
	EV_ERROR,
};

struct usb_event {
	enum usb_event_kind	 kind;
	hid_host_device_handle_t dev;
	uint8_t			 report[8];
};

struct keydef {
	const char *normal, *shift, *altgr, *altgr_shift;
};

static QueueHandle_t events;

#if CONFIG_PM_ENABLE
#include "esp_pm.h"
/* USB host stops in light sleep: stay awake while a keyboard is plugged in */
static esp_pm_lock_handle_t awake;
#define STAY_AWAKE()	esp_pm_lock_acquire(awake)
#define MAY_SLEEP()	esp_pm_lock_release(awake)
#else
#define STAY_AWAKE()
#define MAY_SLEEP()
#endif

/* ------------------------------------------------------------ layouts */

#if CONFIG_PT_KBD_LAYOUT_IT
static const struct keydef keys[0x65] = {
	[0x08] = { "e", "E", "€" },
	[0x1e] = { "1", "!" }, [0x1f] = { "2", "\"" }, [0x20] = { "3", "£" },
	[0x21] = { "4", "$" }, [0x22] = { "5", "%" }, [0x23] = { "6", "&" },
	[0x24] = { "7", "/" }, [0x25] = { "8", "(" }, [0x26] = { "9", ")" },
	[0x27] = { "0", "=" },
	[0x2d] = { "'", "?" }, [0x2e] = { "ì", "^" },
	[0x2f] = { "è", "é", "[", "{" }, [0x30] = { "+", "*", "]", "}" },
	[0x31] = { "ù", "§" }, [0x32] = { "ù", "§" },
	[0x33] = { "ò", "ç", "@" }, [0x34] = { "à", "°", "#" },
	[0x35] = { "\\", "|" },
	[0x36] = { ",", ";" }, [0x37] = { ".", ":" }, [0x38] = { "-", "_" },
	[0x64] = { "<", ">" },
};
#else
static const struct keydef keys[0x65] = {
	[0x1e] = { "1", "!" }, [0x1f] = { "2", "@" }, [0x20] = { "3", "#" },
	[0x21] = { "4", "$" }, [0x22] = { "5", "%" }, [0x23] = { "6", "^" },
	[0x24] = { "7", "&" }, [0x25] = { "8", "*" }, [0x26] = { "9", "(" },
	[0x27] = { "0", ")" },
	[0x2d] = { "-", "_" }, [0x2e] = { "=", "+" },
	[0x2f] = { "[", "{" }, [0x30] = { "]", "}" },
	[0x31] = { "\\", "|" }, [0x32] = { "#", "~" },
	[0x33] = { ";", ":" }, [0x34] = { "'", "\"" }, [0x35] = { "`", "~" },
	[0x36] = { ",", "<" }, [0x37] = { ".", ">" }, [0x38] = { "/", "?" },
	[0x64] = { "\\", "|" },
};
#endif

static const char *const special[0x65] = {
	[0x28] = "\r", [0x29] = "\x1b", [0x2a] = "\x7f", [0x2b] = "\t", [0x2c] = " ",
	[0x3a] = "\x1bOP", [0x3b] = "\x1bOQ", [0x3c] = "\x1bOR", [0x3d] = "\x1bOS",
	[0x49] = "\x1b[2~", [0x4a] = "\x1b[H", [0x4b] = "\x1b[5~", [0x4c] = "\x1b[3~",
	[0x4d] = "\x1b[F", [0x4e] = "\x1b[6~", [0x4f] = "\x1b[C", [0x50] = "\x1b[D",
	[0x51] = "\x1b[B", [0x52] = "\x1b[A",
	[0x54] = "/", [0x55] = "*", [0x56] = "-", [0x57] = "+", [0x58] = "\r",
	[0x59] = "1", [0x5a] = "2", [0x5b] = "3", [0x5c] = "4", [0x5d] = "5",
	[0x5e] = "6", [0x5f] = "7", [0x60] = "8", [0x61] = "9", [0x62] = "0", [0x63] = ".",
};

static bool caps_lock;

static void emit_key(uint8_t key, uint8_t mods)
{
	bool shift = mods & MOD_SHIFT;
	bool ctrl = mods & MOD_CTRL;
	bool altgr = mods & MOD_ALTGR;
	char letter[2] = { 0 };
	const char *s = NULL;

	if (key >= sizeof(special) / sizeof(special[0]))
		return;
	if (special[key]) {
		tty_input(special[key], strlen(special[key]));
		return;
	}

	if (key >= 0x04 && key <= 0x1d) {
		letter[0] = (shift ^ caps_lock ? 'A' : 'a') + key - 0x04;
		if (ctrl) {
			letter[0] &= 0x1f;
			tty_input(letter, 1);
			return;
		}
		if (!(altgr && keys[key].altgr)) {
			tty_input(letter, 1);
			return;
		}
	}

	const struct keydef *k = &keys[key];
	if (altgr)
		s = shift && k->altgr_shift ? k->altgr_shift : k->altgr;
	else
		s = shift && k->shift ? k->shift : k->normal;
	if (!s)
		return;
	if (ctrl && s[0] >= '@' && s[0] <= '_' && !s[1]) {
		char c = s[0] & 0x1f;
		tty_input(&c, 1);
		return;
	}
	tty_input(s, strlen(s));
}

/* ------------------------------------------------------------ HID plumbing */

static void interface_cb(hid_host_device_handle_t dev, const hid_host_interface_event_t event, void *arg)
{
	struct usb_event e = { .dev = dev };
	size_t len = 0;

	switch (event) {
	case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
		if (hid_host_device_get_raw_input_report_data(dev, e.report, sizeof(e.report), &len) != ESP_OK ||
		    len < sizeof(e.report))
			return;
		e.kind = EV_REPORT;
		break;
	case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
		e.kind = EV_DISCONNECTED;
		break;
	case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
		e.kind = EV_ERROR;
		break;
	default:
		return;
	}
	xQueueSend(events, &e, 0);
}

static void driver_cb(hid_host_device_handle_t dev, const hid_host_driver_event_t event, void *arg)
{
	if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
		struct usb_event e = { .kind = EV_CONNECTED, .dev = dev };
		xQueueSend(events, &e, 0);
	}
}

static void keyboard_connected(hid_host_device_handle_t dev)
{
	hid_host_dev_params_t params;
	const hid_host_device_config_t cfg = { .callback = interface_cb };

	if (hid_host_device_get_params(dev, &params) != ESP_OK)
		return;
	if (params.proto != HID_PROTOCOL_KEYBOARD) {
		klog("usb: HID device on interface %d is not a keyboard, ignored", params.iface_num);
		return;
	}
	if (hid_host_device_open(dev, &cfg) != ESP_OK) {
		klog("usb: could not open keyboard");
		return;
	}
	if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
		hid_class_request_set_protocol(dev, HID_REPORT_PROTOCOL_BOOT);
		hid_class_request_set_idle(dev, 0, 0);
	}
	hid_host_device_start(dev);
	STAY_AWAKE();
	klog("usb: keyboard connected");
}

static void usbkbd_task(void *arg)
{
	const int64_t delay_us = CONFIG_PT_KBD_REPEAT_DELAY_MS * 1000LL;
	const int64_t rate_us = CONFIG_PT_KBD_REPEAT_RATE_MS * 1000LL;
	uint8_t held[6] = { 0 }, mods = 0, repeat_key = 0;
	int64_t repeat_at = 0;
	struct usb_event e;

	for (;;) {
		if (!xQueueReceive(events, &e, pdMS_TO_TICKS(10))) {
			if (repeat_key && esp_timer_get_time() >= repeat_at) {
				emit_key(repeat_key, mods);
				repeat_at = esp_timer_get_time() + rate_us;
			}
			continue;
		}

		switch (e.kind) {
		case EV_CONNECTED:
			keyboard_connected(e.dev);
			break;
		case EV_DISCONNECTED:
			hid_host_device_close(e.dev);
			MAY_SLEEP();
			memset(held, 0, sizeof(held));
			repeat_key = 0;
			klog("usb: keyboard disconnected");
			break;
		case EV_ERROR:
			klog("usb: keyboard transfer error");
			break;
		case EV_REPORT: {
			const uint8_t *now = e.report + 2;

			mods = e.report[0];
			if (repeat_key && !memchr(now, repeat_key, 6))
				repeat_key = 0;
			for (int i = 0; i < 6; i++) {
				uint8_t key = now[i];
				if (key < 0x04 || memchr(held, key, 6))
					continue;
				if (key == 0x39) {
					caps_lock = !caps_lock;
					continue;
				}
				emit_key(key, mods);
				repeat_key = key;
				repeat_at = esp_timer_get_time() + delay_us;
			}
			memcpy(held, now, sizeof(held));
			break;
		}
		}
	}
}

int usbkbd_init(void)
{
	const hid_host_driver_config_t hid_cfg = {
		.create_background_task = true,
		.task_priority = 5,
		.stack_size = 4096,
		.core_id = 0,
		.callback = driver_cb,
	};

	events = xQueueCreate(16, sizeof(struct usb_event));
#if CONFIG_PM_ENABLE
	esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usbkbd", &awake);
#endif
	if (usb_host_start())		/* shared with the storage driver */
		return -EIO;
	if (hid_host_install(&hid_cfg) != ESP_OK) {
		klog("usb: HID driver failed to start");
		return -EIO;
	}
	xTaskCreatePinnedToCore(usbkbd_task, "kusbkbd", 4096, NULL, 9, NULL, 0);
	klog("usb: host ready, %s keyboard layout", LAYOUT_NAME);
	return 0;
}

#else

int usbkbd_init(void)
{
	return -ENODEV;
}

#endif
