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

PT_COMPLETE(nes, ": -q -v -f <file:.nes>\n*: <file:.nes>\n")

PT_PROGRAM_STACK(nes, 16, "play a NES game\n"
		 "usage: nes [-q] [-v] [-f skip] [rom.nes]\n"
		 "With no file, the games in ~/roms are offered as a list.\n"
		 "  -q  no sound\n"
		 "  -v  at the end, how fast it ran\n"
		 "  -f  draw one frame in every skip+1 at most\n"
		 "      (default 1: every other); one is also left\n"
		 "      undrawn while the last is still being sent,\n"
		 "      so the game itself always runs at full speed\n"
		 "Arrows or WASD move, X and Z are A and B, Enter\n"
		 "starts, space selects, q, Esc or Ctrl-C quits. A\n"
		 "terminal cannot say when a key is let go, so a press\n"
		 "counts as held for a moment.")
{
	struct nes_options opt = { .sound = true, .frameskip = 1 };
	struct nes_stats stats;
	char rom[PT_PATH_MAX] = "";
	bool verbose = false;
	int i = 1, ret;

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "-q")) {
			opt.sound = false;
		} else if (!strcmp(argv[i], "-v")) {
			verbose = true;
		} else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
			opt.frameskip = atoi(argv[++i]);
			if (opt.frameskip < 0 || opt.frameskip > 8) {
				pt_dprintf(PT_STDERR, "nes: -f takes 0 to 8\n");
				return 2;
			}
		} else {
			pt_dprintf(PT_STDERR, "usage: nes [-q] [-v] [-f skip] [rom.nes]\n");
			return 2;
		}
	}
	if (i + 1 < argc) {
		pt_dprintf(PT_STDERR, "usage: nes [-q] [-v] [-f skip] [rom.nes]\n");
		return 2;
	}
	if (!vt_has_display()) {
		pt_dprintf(PT_STDERR, "nes: there is no screen to play on\n");
		return 1;
	}

	/* No game named: show the shelf rather than asking for a path. */
	if (i == argc) {
		static const char *const roms[] = { ".nes", NULL };
		char dir[64];

		home_dir(dir, sizeof(dir), "roms");
		ret = pick_file(dir, roms, "games", rom, sizeof(rom));
		if (ret == -ECANCELED)
			return 0;
		if (ret)
			return fail("nes", dir, ret);
	} else {
		strlcpy(rom, argv[i], sizeof(rom));
	}

	ret = nes_run(rom, &opt);
	vt_redraw();

	if (ret == -EBUSY) {
		pt_dprintf(PT_STDERR, "nes: a game is running on another terminal already;\n"
			   "  there is room for one at a time\n");
		return 1;
	}
	if (ret)
		return fail("nes", rom, ret);
	if (!verbose)
		return 0;
	nes_last_stats(&stats);
	pt_printf("%d frames in %d s: %d.%d emulated a second, %d drawn\n", stats.frames,
		  stats.seconds, stats.fps_tenths / 10, stats.fps_tenths % 10, stats.blits);
	if (!stats.lit_pixels)
		pt_printf("nothing was drawn: the ROM may not have started\n");
	return 0;
}
