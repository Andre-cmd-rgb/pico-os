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

/*
 * The serial copy of the output, with LF turned into CRLF. A line goes out
 * as one write, not as its text and then its line ending: every write is a
 * trip through the USB driver's lock and ring buffer, and a line split in
 * two is a line the other end can get half of.
 */
static void mirror_crlf(const char *s, size_t n)
{
	char buf[128];
	size_t at = 0;

	for (size_t i = 0; i < n; i++) {
		if (at + 2 > sizeof(buf)) {
			mirror(buf, at);
			at = 0;
		}
		if (s[i] == '\n')
			buf[at++] = '\r';
		buf[at++] = s[i];
	}
	if (at)
		mirror(buf, at);
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
 * Ctrl-A then a digit switches terminals, the way screen and tmux do it,
 * and Ctrl-A then z dozes, from wherever you are: dark, with everything
 * kept, until a key -- the board's own buttons are out of reach in its
 * case. Ctrl-A twice sends one through,
 * so a program that wants it can have it. Everything else goes to the
 * terminal in front.
 */
static bool switch_key(uint8_t c)
{
	if (switch_armed) {
		switch_armed = false;
		if (c >= '1' && c <= '9') {
			tty_switch(c - '1');
			return true;
		}
		if (c == 'z' || c == 'Z') {
			power_doze();
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

/*
 * Looking back through what has scrolled off the terminal in front:
 * Ctrl-A then Up (Fn A, then up, on the CardKB) or Shift-PgUp on a USB
 * keyboard. Up and Down then go on half a screen at a time, PgUp and PgDn
 * a screen, Home and End (Fn up, Fn down) to the oldest line and back to
 * the screen as it is. Esc, Enter or q go back too; any other key goes
 * back and on to the terminal, as it would have.
 */
static bool scrolling;

/* The length of the whole ESC [ ... sequence at `s`, or 0. */
static size_t seq_len(const char *s, size_t n)
{
	if (n < 3 || s[0] != 0x1b || s[1] != '[')
		return 0;
	for (size_t i = 2; i < n && i < 8; i++)
		if (s[i] >= 0x40 && s[i] <= 0x7e)
			return i + 1;
	return 0;
}

/* How far a key moves the view back, negative forward; 0 if it is not one. */
static int scroll_step(const char *seq, size_t len, bool any)
{
	int c, r, page;

	vt_size(&c, &r);
	page = r > 2 ? r - 1 : 1;
	if (len >= 4 && seq[len - 1] == '~' && (seq[2] == '5' || seq[2] == '6') &&
	    (any || (len == 6 && seq[3] == ';' && seq[4] == '2')))	/* shifted */
		return seq[2] == '5' ? page : -page;
	if (!any || len != 3)
		return 0;
	switch (seq[2]) {
	case 'A': return page / 2;
	case 'B': return -(page / 2);
	case 'H': return 1 << 20;		/* Home: as far back as there is */
	case 'F': return -(1 << 20);		/* End */
	}
	return 0;
}

/*
 * Keys from a keyboard on the board. One that lights a dark screen does
 * nothing else: nobody could see what it would have done.
 */
void tty_input(const char *s, size_t n)
{
	if (!power_key())
		return;
	tty_input_remote(s, n);
}

/* Typing from the PC over the serial port: no light, but not idle. */
void tty_input_remote(const char *s, size_t n)
{
	struct tty *tty = fg();

	power_remote_activity();
	if (!tty_ready(tty))
		return;			/* keys before the console exists */
	if (alarm_key(s, n))
		return;			/* it snoozed or stopped a ringing alarm */
	xSemaphoreTake(tty->in_lock, portMAX_DELAY);
	for (size_t i = 0; i < n; i++) {
		uint8_t c = (uint8_t)s[i];
		size_t k = c == 0x1b ? seq_len(s + i, n - i) : 0;
		int step = k ? scroll_step(s + i, k, scrolling || switch_armed) : 0;

		if (step) {
			switch_armed = false;
			scrolling = vt_scroll(step) > 0;
			i += k - 1;
			continue;
		}
		if (scrolling) {
			scrolling = false;
			vt_scroll_end();
			if (c == '\r' || c == 'q' || (c == 0x1b && i + 1 == n))
				continue;	/* only the way back */
		}
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
	if (scrolling) {		/* the one left goes back to what it shows */
		vt_scroll_end();
		scrolling = false;
	}
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
	/* how often to look for a signal while waiting: less often in the dark */
	TickType_t poll = tty->read_timeout_ms >= 0 && tty->read_timeout_ms < 50
			? pdMS_TO_TICKS(tty->read_timeout_ms) : pdMS_TO_TICKS(power_poll_ms(50));

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
	case PT_TTY_GETRAW:
		*(int *)arg = tty->raw;
		return 0;
	}
	return -ENOTTY;
}

static const struct pt_file_ops tty_ops = {
	.read = tty_read,
	.write = tty_write,
	.ioctl = tty_ioctl,
};

/* The first of the calling process's fds 0-2 that is one of the terminals. */
int tty_of_current(void)
{
	struct proc *p = proc_current();

	for (int fd = 0; p && fd < 3; fd++)
		if (p->fd[fd] && p->fd[fd]->ops == &tty_ops)
			return (int)((struct tty *)p->fd[fd]->priv - ttys);
	return -1;
}

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
