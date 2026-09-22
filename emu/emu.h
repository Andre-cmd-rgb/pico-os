/*
 * Game emulators.
 *
 * One entry point per machine. The emulator takes over the screen, the
 * sound and the keyboard until the game is quit, then puts the terminal
 * back the way it was.
 */
#pragma once

#include <stdbool.h>

struct nes_options {
	bool	sound;
	bool	stretch;	/* fill the 320-pixel width instead of centring */
	int	frameskip;	/* 0 draws every frame */
};

/* Runs until the player quits. Returns 0, or -errno if the ROM will not load. */
int	nes_run(const char *rom_path, const struct nes_options *opt);

/* What the emulator did last time, for the command to print. */
struct nes_stats {
	int	frames;
	int	seconds;
	int	fps_tenths;	/* 597 is 59.7 frames a second */
	int	skipped;
	int	blits;		/* frames handed to the screen */
	int	lit_pixels;	/* not background, in the last frame drawn */
	int	sound_peak;	/* loudest sample written, 0 to 32767 */
};
void	nes_last_stats(struct nes_stats *out);
