/*
 * Bluetooth Low Energy: the radio, and what is around.
 *
 * The radio is started only when something asks for it -- `ble on`, or a
 * gamepad -- because the controller and NimBLE's host take tens of
 * kilobytes of the internal RAM everything else competes for, and `ble
 * off` gives them back. What uses the radio goes through here for it and
 * for scanning: the gamepad in input/blepad.c, so far.
 *
 * One lock, recursive, covers starting, stopping, scanning and whatever a
 * user of the radio does on it (ble_hold), so that `ble off` on one
 * terminal never pulls the radio from under a pad connecting on another.
 */
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_BLE

#if !CONFIG_BT_NIMBLE_ENABLED
#error "Bluetooth needs ESP-IDF's NimBLE host: boards/fragments/ble.config, or Component config > Bluetooth > Host"
#endif

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#define HID_SERVICE_UUID	0x1812
#define SCAN_MAX		16
#define SYNC_WAIT_MS		5000

static struct ble_found	 found[SCAN_MAX];
static int		 nfound;
static bool		 up;		/* controller and host initialised */
static volatile bool	 synced;	/* the host has its controller */
static volatile bool	 scanning;
static bool		 scan_hid_only;
static SemaphoreHandle_t lock, scan_done;

void ble_hold(void)
{
	xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

void ble_release(void)
{
	xSemaphoreGiveRecursive(lock);
}

/* ------------------------------------------------------------ scanning */

/* A keyboard, mouse or pad: by its appearance, or by the HID service. */
static bool says_hid(const struct ble_hs_adv_fields *fields)
{
	if (fields->appearance_is_present && (fields->appearance >> 6) == 0x0f)
		return true;
	for (int i = 0; i < fields->num_uuids16; i++)
		if (ble_uuid_u16(&fields->uuids16[i].u) == HID_SERVICE_UUID)
			return true;
	return false;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
	struct ble_hs_adv_fields fields;
	struct ble_found *f = NULL;
	bool hid;

	if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
		scanning = false;
		xSemaphoreGive(scan_done);
		return 0;
	}
	if (event->type != BLE_GAP_EVENT_DISC ||
	    ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data))
		return 0;
	hid = says_hid(&fields);

	/*
	 * A device's name and appearance often come in its scan response,
	 * a second packet after the advertisement: one heard before is
	 * completed rather than listed twice. The list holds only what is
	 * shown, so that `pad connect 2` is the second one printed.
	 */
	for (int i = 0; i < nfound; i++)
		if (!memcmp(found[i].addr, event->disc.addr.val, 6)) {
			f = &found[i];
			break;
		}
	if (!f) {
		if ((scan_hid_only && !hid) || nfound == SCAN_MAX)
			return 0;
		f = &found[nfound];
		memset(f, 0, sizeof(*f));
		memcpy(f->addr, event->disc.addr.val, 6);
		f->addr_type = event->disc.addr.type;
		strlcpy(f->name, "(no name)", sizeof(f->name));
		nfound++;
	}
	f->rssi = event->disc.rssi;
	f->is_hid |= hid;
	if (fields.appearance_is_present)
		f->appearance = fields.appearance;
	if (fields.name_len)
		snprintf(f->name, sizeof(f->name), "%.*s", fields.name_len,
			 (const char *)fields.name);
	return 0;
}

int ble_scan(struct ble_found *out, int max, int seconds, bool hid_only)
{
	struct ble_gap_disc_params params = { .filter_duplicates = 0, .passive = 0 };
	int64_t deadline = pt_uptime_us() + (seconds * 1000LL + 2000) * 1000;
	uint8_t own_type;
	int n = 0;

	if (!lock)
		return -ENODEV;
	ble_hold();
	if (!synced) {
		n = -ENODEV;
		goto out;
	}
	if (scanning) {
		n = -EBUSY;
		goto out;
	}
	nfound = 0;
	scan_hid_only = hid_only;
	xSemaphoreTake(scan_done, 0);		/* left over from a scan cut short */
	scanning = true;
	if (ble_hs_id_infer_auto(0, &own_type) ||
	    ble_gap_disc(own_type, seconds * 1000, &params, gap_event, NULL)) {
		scanning = false;
		n = -EIO;
		goto out;
	}
	while (xSemaphoreTake(scan_done, pdMS_TO_TICKS(100)) != pdTRUE) {
		if (pt_interrupted() || pt_uptime_us() > deadline) {
			ble_gap_disc_cancel();
			scanning = false;
			n = pt_interrupted() ? -EINTR : -ETIMEDOUT;
			goto out;
		}
	}
	for (n = 0; n < nfound && n < max; n++)
		out[n] = found[n];
out:
	ble_release();
	return n;
}

const struct ble_found *ble_found_get(int index)
{
	return index >= 1 && index <= nfound ? &found[index - 1] : NULL;
}

/* ------------------------------------------------------------ the radio */

static void on_sync(void)
{
	ble_hs_util_ensure_addr(0);
	synced = true;
}

static void on_reset(int reason)
{
	synced = false;
	klog("ble: the controller reset (%d)", reason);
}

static void host_task(void *arg)
{
	nimble_port_run();		/* until nimble_port_stop() */
	nimble_port_freertos_deinit();
}

/* The controller and the host; non-zero when there is no room for them. */
static int port_up(void)
{
	if (nimble_port_init() != ESP_OK)
		return -ENOMEM;
	ble_hs_cfg.sync_cb = on_sync;
	ble_hs_cfg.reset_cb = on_reset;
	ble_hs_cfg.sm_bonding = 1;
	ble_hs_cfg.sm_sc = 1;
	ble_hs_cfg.sm_our_key_dist = 3;
	ble_hs_cfg.sm_their_key_dist = 3;
	nimble_port_freertos_init(host_task);
	return 0;
}

static int port_down(void)
{
	int ret = nimble_port_stop();

	if (ret)
		return -EBUSY;
	nimble_port_deinit();
	up = synced = false;
	return 0;
}

int ble_start(void)
{
	int ret = 0;

	if (!lock)
		return -ENODEV;
	ble_hold();
	if (synced)
		goto out;
	/*
	 * Both radios want internal RAM, and with NimBLE's own allocations
	 * in PSRAM they usually both fit. When they do not, the one that has
	 * to go is the one not being used -- somebody asking for Bluetooth
	 * wants it now -- and `wifi on` brings it back afterwards.
	 */
	if (!up && (ret = port_up()) && wifi_started()) {
		klog("ble: not enough memory beside Wi-Fi; turning it off");
		wifi_radio(false);
		vTaskDelay(pdMS_TO_TICKS(300));
		ret = port_up();
	}
	if (ret) {
		klog("ble: the radio would not start");
		goto out;
	}
	up = true;
	for (int i = 0; i < SYNC_WAIT_MS / 100 && !synced; i++)
		vTaskDelay(pdMS_TO_TICKS(100));
	if (!synced) {
		/* left up if it will not stop: the next start waits again */
		klog("ble: the radio did not answer");
		port_down();
		ret = -ETIMEDOUT;
		goto out;
	}
	klog("ble: radio on");
out:
	ble_release();
	return ret;
}

int ble_stop(void)
{
	int ret = 0;

	if (!lock)
		return -ENODEV;
	ble_hold();
	if (!up)
		goto out;
	if (scanning) {
		ret = -EBUSY;
		goto out;
	}
	if ((ret = pad_stop()) || (ret = port_down()))
		goto out;
	klog("ble: radio off");
out:
	ble_release();
	return ret;
}

bool ble_started(void)
{
	return synced;
}

int ble_init(void)
{
	lock = xSemaphoreCreateRecursiveMutex();
	scan_done = xSemaphoreCreateBinary();
	if (!lock || !scan_done)
		return -ENOMEM;
	klog("ble: radio off until asked; `ble on` starts it");
	return 0;
}

#else

int  ble_init(void) { return -ENODEV; }
int  ble_start(void) { return -ENODEV; }
int  ble_stop(void) { return -ENODEV; }
bool ble_started(void) { return false; }
void ble_hold(void) { }
void ble_release(void) { }
int  ble_scan(struct ble_found *out, int max, int seconds, bool hid_only) { return -ENODEV; }
const struct ble_found *ble_found_get(int index) { return NULL; }

#endif
