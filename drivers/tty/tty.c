/*
 * The consoles: several terminals sharing one screen and one serial port,
 * the way a Linux console has several you switch between.
 *
 * Each has its own screen contents, its own line being edited and its own
 * shell; only the one on screen is painted and only it receives what the
 * keyboard sends. Ctrl-A then a digit switches, and so does `chvt`.
 *
 * Output goes to that terminal's screen and, for the one in front, is
 * mirrored to the serial console with LF turned into CRLF. Input from
 * every keyboard lands in tty_input().
 *
 * Cooked mode (the default) edits a line locally and hands it over on Enter:
 * Backspace, Ctrl-U erase; Ctrl-C, or a lone Esc on keyboards without Ctrl,
 * sends SIGINT to the foreground process group; Ctrl-D ends input. Raw mode
 * passes every byte straight through, escape sequences included.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#include "drivers/drivers.h"

#define INPUT_SIZE	1024
#define TTY_LINE_MAX	256

struct tty {
	StreamBufferHandle_t input;
	SemaphoreHandle_t    in_lock, read_lock;
	struct pt_file	    *file;
	char		     line[TTY_LINE_MAX];
	size_t		     line_len;
	bool		     raw, eof, last_cr;
	int		     esc_state;	/* 0 none, 1 after ESC, 2 in a sequence */
	int		     fg_pgid;
	int		     read_timeout_ms;
};

static struct tty	 ttys[CONFIG_PT_VT_COUNT];
static int		 front;		/* the terminal the keyboard talks to */
static SemaphoreHandle_t out_lock;
static void		(*mirror)(const char *s, size_t n);
static void		(*activate_hook)(int which);
static bool		 switch_armed;	/* Ctrl-A seen, waiting for a digit */

static struct tty *fg(void)
{
	return &ttys[front];
}

static bool tty_ready(const struct tty *tty)
{
	return tty->in_lock && tty->input;
}

/* ------------------------------------------------------------ output */

/* The serial copy of the output, with LF turned into CRLF. */
static void mirror_crlf(const char *s, size_t n)
{
	size_t start = 0;

	for (size_t i = 0; i < n; i++) {
		if (s[i] != '\n')
			continue;
		if (i > start)
			mirror(s + start, i - start);
		mirror("\r\n", 2);
		start = i + 1;
	}
	if (start < n)
		mirror(s + start, n - start);
}

/*
 * Each write reaches the serial port whole, so kernel messages never land in
 * the middle of a program's line or of a file transfer. The lock is recursive
 * because a driver may log while output is being written on the same task.
 */
static void tty_output_on(int which, const char *s, size_t n)
{
	xSemaphoreTakeRecursive(out_lock, portMAX_DELAY);
	vt_write_on(which, s, n);
	if (which == front && mirror)
		mirror_crlf(s, n);
	xSemaphoreGiveRecursive(out_lock);
}

/* Kernel messages and anything else with no terminal of its own. */
void tty_output(const char *s, size_t n)
{
	tty_output_on(front, s, n);
}

/* Kernel messages, serial only. They wait for the lock only so long: a task
 * killed while writing may never give it back, and the log must go on. */
void tty_mirror_output(const char *s, size_t n)
{
	if (!mirror)
		return;
	bool locked = xSemaphoreTakeRecursive(out_lock, pdMS_TO_TICKS(200)) == pdTRUE;
	mirror_crlf(s, n);
	if (locked)
		xSemaphoreGiveRecursive(out_lock);
}

void tty_set_mirror(void (*write)(const char *s, size_t n))
{
	mirror = write;
}

/* ------------------------------------------------------------ input */

static void interrupt(struct tty *tty)
{
	tty_output_on(tty - ttys, "^C\n", 3);
	tty->line_len = 0;
	if (tty->fg_pgid)
		proc_signal_group(tty->fg_pgid, PT_SIGINT);
}

static void submit_line(struct tty *tty)
{
	xStreamBufferSend(tty->input, tty->line, tty->line_len, 0);
	tty->line_len = 0;
}

static void erase_char(struct tty *tty)
{
	if (!tty->line_len)
		return;
	do
		tty->line_len--;
	while (tty->line_len && (tty->line[tty->line_len] & 0xc0) == 0x80);
	tty_output_on(tty - ttys, "\b \b", 3);
}

static void cooked_byte(struct tty *tty, uint8_t c, bool last_in_chunk)
{
	if (tty->esc_state == 1) {
		tty->esc_state = c == '[' || c == 'O' ? 2 : 0;
		return;
	}
	if (tty->esc_state == 2) {
		if (c >= 0x40 && c <= 0x7e)
			tty->esc_state = 0;
		return;
	}

	bool after_cr = tty->last_cr;
	tty->last_cr = c == '\r';

	switch (c) {
	case 0x1b:
		if (last_in_chunk)
			interrupt(tty);
		else
			tty->esc_state = 1;
		break;
	case 0x03:
		interrupt(tty);
		break;
	case 0x1a:			/* Ctrl-Z: the job in front stops */
		tty_output_on(tty - ttys, "^Z\n", 3);
		tty->line_len = 0;
		if (tty->fg_pgid)
			proc_signal_group(tty->fg_pgid, PT_SIGTSTP);
		break;
	case 0x04:
		if (tty->line_len)
			submit_line(tty);
		else
			tty->eof = true;
		break;
	case 0x7f:
	case '\b':
		erase_char(tty);
		break;
	case 0x15:
		while (tty->line_len)
			erase_char(tty);
		break;
	case '\n':
		if (after_cr)
			break;
		/* fall through */
	case '\r':
		tty->line[tty->line_len++] = '\n';
		tty_output_on(tty - ttys, "\n", 1);
		submit_line(tty);
		break;
	default:
		if ((c >= 0x20 || c == '\t') && tty->line_len < TTY_LINE_MAX - 1) {
			tty->line[tty->line_len++] = c;
			tty_output_on(tty - ttys, (const char *)&c, 1);
		}
		break;
	}
}

/*
 * Ctrl-A then a digit switches terminals, the way screen and tmux do it.
 * Ctrl-A twice sends one through, so a program that wants it can have
 * it. Everything else goes to the terminal in front.
 */
static bool switch_key(uint8_t c)
{
	if (switch_armed) {
		switch_armed = false;
		if (c >= '1' && c <= '9') {
			tty_switch(c - '1');
			return true;
		}
		if (c == 0x01)
			return false;		/* Ctrl-A Ctrl-A: pass one on */
	}
	if (c == 0x01) {
		switch_armed = true;
		return true;
	}
	return false;
}

void tty_input(const char *s, size_t n)
{
	struct tty *tty = fg();

	if (!tty_ready(tty))
		return;			/* keys before the console exists */
	xSemaphoreTake(tty->in_lock, portMAX_DELAY);
	for (size_t i = 0; i < n; i++) {
		uint8_t c = (uint8_t)s[i];

		if (switch_key(c)) {
			if (tty != fg()) {	/* the switch happened */
				xSemaphoreGive(tty->in_lock);
				tty = fg();
				if (!tty_ready(tty))
					return;
				xSemaphoreTake(tty->in_lock, portMAX_DELAY);
			}
			continue;
		}
		if (tty->raw)
			xStreamBufferSend(tty->input, &c, 1, 0);
		else
			cooked_byte(tty, c, i + 1 == n);
	}
	xSemaphoreGive(tty->in_lock);
}

int tty_switch(int which)
{
	if (which < 0 || which >= CONFIG_PT_VT_COUNT)
		return -EINVAL;
	if (which == front)
		return 0;
	/*
	 * Make it exist before it goes in front: a terminal nobody has
	 * used yet has no buffers, and the keyboard is about to write
	 * into them.
	 */
	if (!tty_console(which))
		return -ENOMEM;
	front = which;
	vt_switch(which);
	if (activate_hook)
		activate_hook(which);	/* start its shell if it has none */
	return 0;
}

int tty_front(void)
{
	return front;
}

void tty_set_activate_hook(void (*fn)(int which))
{
	activate_hook = fn;
}

/* ------------------------------------------------------------ file */

static ssize_t tty_read(struct pt_file *f, void *buf, size_t n)
{
	struct tty *tty = f->priv;
	ssize_t ret;

	int64_t deadline = tty->read_timeout_ms >= 0
			 ? pt_uptime_us() + tty->read_timeout_ms * 1000LL : 0;
	TickType_t poll = tty->read_timeout_ms >= 0 && tty->read_timeout_ms < 50
			? pdMS_TO_TICKS(tty->read_timeout_ms) : pdMS_TO_TICKS(50);

	xSemaphoreTake(tty->read_lock, portMAX_DELAY);
	for (;;) {
		size_t got = xStreamBufferReceive(tty->input, buf, n, poll);
		if (got) {
			ret = got;
			break;
		}
		if (tty->eof) {
			tty->eof = false;
			ret = 0;
			break;
		}
		if (pt_interrupted()) {
			ret = -EINTR;
			break;
		}
		if (proc_stop_pending()) {	/* Ctrl-Z: stop without the terminal locked */
			xSemaphoreGive(tty->read_lock);
			proc_stop_point();
			xSemaphoreTake(tty->read_lock, portMAX_DELAY);
		}
		if (deadline && pt_uptime_us() >= deadline) {
			ret = -EAGAIN;
			break;
		}
	}
	xSemaphoreGive(tty->read_lock);
	return ret;
}

static ssize_t tty_write(struct pt_file *f, const void *buf, size_t n)
{
	struct tty *tty = f->priv;

	tty_output_on(tty - ttys, buf, n);
	return n;
}

static int tty_ioctl(struct pt_file *f, int req, void *arg)
{
	struct tty *tty = f->priv;

	switch (req) {
	case PT_TTY_SETRAW:
		xSemaphoreTake(tty->in_lock, portMAX_DELAY);
		tty->raw = *(int *)arg;
		tty->line_len = 0;
		tty->esc_state = 0;
		xSemaphoreGive(tty->in_lock);
		return 0;
	case PT_TTY_GETSIZE: {
		struct pt_winsize *ws = arg;
		int c, r;
		vt_size(&c, &r);
		ws->cols = c;
		ws->rows = r;
		return 0;
	}
	case PT_TTY_SETPGRP:
		tty->fg_pgid = *(int *)arg;
		return 0;
	case PT_TTY_SETTIMEOUT:
		tty->read_timeout_ms = *(int *)arg;
		return 0;
	}
	return -ENOTTY;
}

static const struct pt_file_ops tty_ops = {
	.read = tty_read,
	.write = tty_write,
	.ioctl = tty_ioctl,
};

void tty_init(void)
{
	out_lock = xSemaphoreCreateRecursiveMutex();
	for (int i = 0; i < CONFIG_PT_VT_COUNT; i++) {
		struct tty *tty = &ttys[i];

		/* Only the first terminal is set up at boot; the others
		 * cost nothing until somebody switches to one. */
		tty->read_timeout_ms = -1;
		if (i == 0 && !tty_console(0))
			klog("tty: no memory for the console");
	}
}

/*
 * The file for a terminal, made the first time it is asked for.
 */
struct pt_file *tty_console(int which)
{
	struct tty *tty;

	if (which < 0 || which >= CONFIG_PT_VT_COUNT)
		return NULL;
	tty = &ttys[which];
	if (tty->file)
		return tty->file;
	tty->input = xStreamBufferCreate(INPUT_SIZE, 1);
	tty->in_lock = xSemaphoreCreateMutex();
	tty->read_lock = xSemaphoreCreateMutex();
	tty->read_timeout_ms = -1;
	if (!tty->input || !tty->in_lock || !tty->read_lock)
		return NULL;
	tty->file = file_alloc(&tty_ops, tty);
	if (tty->file)
		tty->file->is_tty = true;
	return tty->file;
}
