#include "pt/keys.h"

#define ESC_TIMEOUT_MS	40

static int read_byte(int fd, int timeout_ms)
{
	unsigned char c;
	int off = -1;

	if (timeout_ms >= 0)
		pt_ioctl(fd, PT_TTY_SETTIMEOUT, &timeout_ms);
	ssize_t n = pt_read(fd, &c, 1);
	if (timeout_ms >= 0)
		pt_ioctl(fd, PT_TTY_SETTIMEOUT, &off);

	if (n == 1)
		return c;
	if (n == 0)
		return PT_KEY_EOF;
	if (n == -EAGAIN)
		return PT_KEY_NONE;	/* the timeout passed, no key */
	return n == -EINTR ? PT_KEY_INTR : PT_KEY_ERROR;
}

static int tilde_key(int num)
{
	switch (num) {
	case 1:
	case 7:
		return PT_KEY_HOME;
	case 2:
		return PT_KEY_INSERT;
	case 3:
		return PT_KEY_DELETE;
	case 4:
	case 8:
		return PT_KEY_END;
	case 5:
		return PT_KEY_PGUP;
	case 6:
		return PT_KEY_PGDN;
	case 11:
		return PT_KEY_F1;
	case 12:
		return PT_KEY_F2;
	case 13:
		return PT_KEY_F3;
	case 14:
		return PT_KEY_F4;
	}
	return PT_KEY_UNKNOWN;
}

static int final_key(int c)
{
	switch (c) {
	case 'A': return PT_KEY_UP;
	case 'B': return PT_KEY_DOWN;
	case 'C': return PT_KEY_RIGHT;
	case 'D': return PT_KEY_LEFT;
	case 'H': return PT_KEY_HOME;
	case 'F': return PT_KEY_END;
	case 'P': return PT_KEY_F1;
	case 'Q': return PT_KEY_F2;
	case 'R': return PT_KEY_F3;
	case 'S': return PT_KEY_F4;
	}
	return PT_KEY_UNKNOWN;
}

int pt_readkey(int fd)
{
	return pt_readkey_timeout(fd, -1);
}

int pt_readkey_timeout(int fd, int ms)
{
	int c = read_byte(fd, ms);

	if (c != 0x1b)
		return c;

	/* The rest of an escape sequence arrives at once or not at all. */
	c = read_byte(fd, ESC_TIMEOUT_MS);
	if (c == 'O')
		return final_key(read_byte(fd, ESC_TIMEOUT_MS));
	if (c == PT_KEY_NONE)
		return PT_KEY_ESC;		/* Esc pressed on its own */
	if (c != '[')
		return c < 0 ? c : PT_KEY_ESC;

	int num = 0;
	for (;;) {
		c = read_byte(fd, ESC_TIMEOUT_MS);
		if (c >= '0' && c <= '9')
			num = num * 10 + (c - '0');
		else if (c == ';')
			num = 0;		/* modifiers: ignored */
		else
			break;
	}
	if (c < 0)
		return c == PT_KEY_ESC ? PT_KEY_UNKNOWN : c;
	return c == '~' ? tilde_key(num) : final_key(c);
}

int pt_tty_raw(int fd, bool raw)
{
	int on = raw;

	return pt_ioctl(fd, PT_TTY_SETRAW, &on);
}

void pt_tty_size(int fd, int *cols, int *rows)
{
	struct pt_winsize ws;

	if (pt_ioctl(fd, PT_TTY_GETSIZE, &ws) || !ws.cols || !ws.rows) {
		ws.cols = 80;
		ws.rows = 24;
	}
	*cols = ws.cols;
	*rows = ws.rows;
}
