/*
 * Key decoding for full-screen and line-editing programs. Turns the bytes a
 * terminal sends in raw mode (including VT escape sequences from any of the
 * keyboards) into one value per key press.
 */
#pragma once

#include "pt/sys.h"

enum pt_key {
	PT_KEY_EOF = -1,
	PT_KEY_ERROR = -2,
	PT_KEY_INTR = -3,	/* read interrupted by a signal */
	PT_KEY_NONE = -4,	/* nothing was pressed before the timeout */

	PT_KEY_UP = 0x100,
	PT_KEY_DOWN,
	PT_KEY_LEFT,
	PT_KEY_RIGHT,
	PT_KEY_HOME,
	PT_KEY_END,
	PT_KEY_PGUP,
	PT_KEY_PGDN,
	PT_KEY_INSERT,
	PT_KEY_DELETE,
	PT_KEY_ESC,		/* Esc on its own */
	PT_KEY_F1,
	PT_KEY_F2,
	PT_KEY_F3,
	PT_KEY_F4,
	PT_KEY_UNKNOWN,		/* an escape sequence nobody maps */
	PT_KEY_CTRL_HOME,
	PT_KEY_CTRL_END,
};

#define PT_CTRL(c)	((c) & 0x1f)

/* Next key from `fd`: a byte (0-255, UTF-8 arrives byte by byte) or PT_KEY_*. */
int pt_readkey(int fd);

/*
 * The same, but waits at most `ms` for the first byte and answers
 * PT_KEY_NONE if nothing came. 0 means "whatever is already waiting".
 */
int pt_readkey_timeout(int fd, int ms);

/* Put the terminal on `fd` into raw (1) or cooked (0) mode. */
int pt_tty_raw(int fd, bool raw);

/* Terminal size, falling back to 80x24 when fd is not a terminal. */
void pt_tty_size(int fd, int *cols, int *rows);
