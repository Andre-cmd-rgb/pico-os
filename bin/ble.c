/*
 * ble, pad: Bluetooth LE, and a gamepad on it.
 *
 * The radio is started only when something asks for it, since it costs
 * internal RAM while it runs, and `ble off` gives that back. The work is
 * in drivers/net/ble.c and drivers/input/blepad.c.
 */
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "util.h"

#if CONFIG_PT_BLE

#define SCAN_MAX	16
#define SCAN_SECONDS	5

static int scan(const char *prog, bool hid_only)
{
	struct ble_found list[SCAN_MAX];
	int n;

	pt_printf("scanning for %s...\n", hid_only ? "gamepads" : "anything");
	if ((n = ble_scan(list, SCAN_MAX, SCAN_SECONDS, hid_only)) < 0)
		return fail(prog, "scan", n);
	for (int i = 0; i < n; i++)
		pt_printf("%2d  %-24s %4d dBm  %02x:%02x:%02x:%02x:%02x:%02x%s\n", i + 1,
			  list[i].name, list[i].rssi, list[i].addr[5], list[i].addr[4],
			  list[i].addr[3], list[i].addr[2], list[i].addr[1], list[i].addr[0],
			  list[i].is_hid ? "  (input device)" : "");
	pt_printf("%d found\n", n);
	return 0;
}

PT_COMPLETE(ble, ": on off scan\n")

PT_PROGRAM(ble, "Bluetooth LE\n"
	   "usage: ble [on | off | scan]\n"
	   "  on    start the radio (it costs RAM, so it waits to be asked)\n"
	   "  off   stop it and give the RAM back\n"
	   "  scan  list what is around\n"
	   "With no arguments, says whether the radio is on.")
{
	const char *cmd = argc > 1 ? argv[1] : "";
	int ret;

	if (argc == 1) {
		pt_printf(ble_started() ? "ble: radio on\n" :
			  "ble: the radio is off; `ble on` starts it\n");
		return 0;
	}
	if (argc == 2 && !strcmp(cmd, "on")) {
		if ((ret = ble_start()))
			return fail("ble", "radio", ret);
		pt_printf("ble: radio on\n");
		return 0;
	}
	if (argc == 2 && !strcmp(cmd, "off")) {
		if ((ret = ble_stop()))
			return fail("ble", "radio", ret);
		pt_printf("ble: radio off\n");
		return 0;
	}
	if (argc == 2 && !strcmp(cmd, "scan")) {
		if ((ret = ble_start()))
			return fail("ble", "radio", ret);
		return scan("ble", false);
	}
	pt_dprintf(PT_STDERR, "usage: ble [on | off | scan]\n");
	return 2;
}

#endif /* CONFIG_PT_BLE */

#if CONFIG_PT_BLE_PAD

static const char *const pad_button_names[] = {
	"up", "down", "left", "right", "a", "b", "start", "select",
};

PT_COMPLETE(pad, ": on scan connect watch\nscan: all\n")

PT_PROGRAM(pad, "a Bluetooth gamepad for the games\n"
	   "usage: pad [on | scan [all] | connect N | watch]\n"
	   "  on       start the radio for a pad (`ble off` stops it)\n"
	   "  scan     look for pads; `scan all` shows every Bluetooth device\n"
	   "  connect  join the Nth from the last scan\n"
	   "  watch    print the raw reports, to work out /etc/gamepad\n"
	   "With no arguments, says what is connected.")
{
	const char *cmd = argc > 1 ? argv[1] : "";
	int ret;

	if (argc == 1) {
		if (!pad_started())
			pt_printf("pad: not started; `pad on` starts the radio\n");
		else if (!pad_connected())
			pt_printf("pad: radio on, nothing connected\n");
		else
			pt_printf("pad: %s connected\n", pad_device_name());
		return 0;
	}
	if (!strcmp(cmd, "on") && argc == 2) {
		if ((ret = pad_start()))
			return fail("pad", "bluetooth", ret);
		pt_printf("pad: radio on\n");
		return 0;
	}
	if (!strcmp(cmd, "scan") && argc <= 3) {
		bool all = argc == 3 && !strcmp(argv[2], "all");

		if ((ret = pad_start()))
			return fail("pad", "bluetooth", ret);
		return scan("pad", !all);
	}
	if (!strcmp(cmd, "connect") && argc == 3) {
		if ((ret = pad_connect(atoi(argv[2]))))
			return fail("pad", argv[2], ret);
		for (int i = 0; i < 50 && !pad_connected(); i++)
			pt_sleep_ms(100);
		pt_printf(pad_connected() ? "connected to %s\n" : "no answer from %s\n",
			  pad_connected() ? pad_device_name() : argv[2]);
		return pad_connected() ? 0 : 1;
	}
	if (!strcmp(cmd, "watch") && argc == 2) {
		uint8_t last[16] = { 0 };

		if (!pad_connected()) {
			pt_dprintf(PT_STDERR, "pad: nothing is connected\n");
			return 1;
		}
		pt_printf("press buttons; Ctrl-C stops\n");
		while (!pt_interrupted()) {
			uint8_t now[16];
			int len = pad_last_report(now, sizeof(now));
			uint16_t state = pad_buttons();

			if (len && memcmp(now, last, len)) {
				memcpy(last, now, len);
				for (int i = 0; i < len; i++)
					pt_printf("%02x ", now[i]);
				pt_printf(" ->");
				for (int i = 0; i < PAD_BUTTON_COUNT; i++)
					if (state & (1u << i))
						pt_printf(" %s", pad_button_names[i]);
				pt_printf("\n");
			}
			pt_sleep_ms(30);
		}
		return 0;
	}
	pt_dprintf(PT_STDERR, "usage: pad [on | scan [all] | connect N | watch]\n");
	return 2;
}

#endif /* CONFIG_PT_BLE_PAD */
