/*
 * nes: play a NES game.
 *
 * The emulator itself is in emu/ and third_party/nofrendo/; this is the
 * program around it -- arguments, raw keyboard, and putting the terminal
 * back afterwards.
 */
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "emu.h"
#include "util.h"

#define ROMS		"/home/" CONFIG_PT_USERNAME "/roms"

#if CONFIG_PT_NES

PT_PROGRAM_STACK(nes, 16, "play a NES game\n"
		 "usage: nes [-q] [-f skip] [rom.nes]\n"
		 "With no file, the games in ~/roms are offered as a list.\n"
		 "  -q  no sound\n"
		 "  -f  draw one frame in every skip+1 (default 1: every other)\n"
		 "Arrows or WASD move, X and Z are A and B, Enter starts,\n"
		 "space selects, q or Esc quits. A terminal cannot say when a\n"
		 "key is let go, so a press counts as held for a moment.")
{
	struct nes_options opt = { .sound = true, .frameskip = 1 };
	struct nes_stats stats;
	char rom[PT_PATH_MAX];
	int i = 1, ret;

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "-q")) {
			opt.sound = false;
		} else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
			opt.frameskip = atoi(argv[++i]);
			if (opt.frameskip < 0 || opt.frameskip > 8) {
				pt_dprintf(PT_STDERR, "nes: -f takes 0 to 8\n");
				return 2;
			}
		} else {
			pt_dprintf(PT_STDERR, "usage: nes [-q] [-f skip] rom.nes\n");
			return 2;
		}
	}
	if (i + 1 < argc) {
		pt_dprintf(PT_STDERR, "usage: nes [-q] [-f skip] [rom.nes]\n");
		return 2;
	}
	if (!vt_has_display()) {
		pt_dprintf(PT_STDERR, "nes: there is no screen to play on\n");
		return 1;
	}

	/* No game named: show the shelf rather than asking for a path. */
	if (i == argc) {
		static const char *const roms[] = { ".nes", NULL };

		ret = pick_file(ROMS, roms, "games", rom, sizeof(rom));
		if (ret == -ECANCELED)
			return 0;
		if (ret)
			return fail("nes", ROMS, ret);
	} else {
		strlcpy(rom, argv[i], sizeof(rom));
	}

	pt_tty_raw(PT_STDIN, true);
	ret = nes_run(rom, &opt);
	pt_tty_raw(PT_STDIN, false);
	vt_redraw();

	if (ret)
		return fail("nes", argv[i], ret);
	nes_last_stats(&stats);
	pt_printf("%d frames in %d s: %d.%d emulated a second, %d drawn\n", stats.frames,
		  stats.seconds, stats.fps_tenths / 10, stats.fps_tenths % 10, stats.blits);
	if (!stats.lit_pixels)
		pt_printf("nothing was drawn: the ROM may not have started\n");
	return 0;
}

#endif /* CONFIG_PT_NES */

#if CONFIG_PT_BLE_PAD

#define PAD_SCAN_MAX	8

static const char *const pad_button_names[] = {
	"up", "down", "left", "right", "a", "b", "start", "select",
};

PT_PROGRAM(pad, "a Bluetooth gamepad for the games\n"
	   "usage: pad [on | scan [all] | connect N | watch]\n"
	   "  on       start the radio (it costs RAM, so it waits to be asked)\n"
	   "  scan     look for pads; `scan all` shows every Bluetooth device\n"
	   "  connect  join the Nth from the last scan\n"
	   "  watch    print the raw reports, to work out /etc/gamepad\n"
	   "With no arguments, says what is connected.")
{
	const char *cmd = argc > 1 ? argv[1] : "";
	struct pad_found list[PAD_SCAN_MAX];
	int ret, n;

	if (argc == 1) {
		if (!pad_started())
			pt_printf("pad: the radio is off; `pad on` starts it\n");
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

		if (!pad_started() && (ret = pad_start()))
			return fail("pad", "bluetooth", ret);
		pt_printf("scanning for %s...\n", all ? "anything" : "gamepads");
		n = pad_scan_all(list, PAD_SCAN_MAX, 5, all);
		if (n < 0)
			return fail("pad", "scan", n);
		for (int i = 0; i < n; i++)
			pt_printf("%2d  %-24s %4d dBm  %02x:%02x:%02x:%02x:%02x:%02x%s\n", i + 1,
				  list[i].name, list[i].rssi, list[i].addr[5], list[i].addr[4],
				  list[i].addr[3], list[i].addr[2], list[i].addr[1],
				  list[i].addr[0], list[i].is_hid ? "  (input device)" : "");
		pt_printf("%d found\n", n);
		return 0;
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
