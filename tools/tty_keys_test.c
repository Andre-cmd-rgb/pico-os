/* Real TTY routing and key decoder, with only the board I/O mocked. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "pt/keys.h"

#define CONFIG_PT_VT_COUNT 2
#define TTY_LINE_MAX 256
#define portMAX_DELAY 0
typedef void *StreamBufferHandle_t;
typedef void *SemaphoreHandle_t;
struct pt_file { void *priv; };
struct proc { int pid, pgid; };
#include "tty_keys_types.h"

static struct tty ttys[CONFIG_PT_VT_COUNT];
static int front, scroll_calls, dozes, cooked;
static bool switch_armed, owner_alive = true, owner_stopped;
static struct proc process = { 7, 7 };
static char input[2048];
static size_t used, pos;
static struct tty *fg(void) { return &ttys[front]; }
static bool tty_ready(const struct tty *tty) { return tty->input && tty->in_lock; }
static struct proc *proc_current(void) { return &process; }
static bool proc_alive(int pid) { return pid == 7 && owner_alive; }
static bool proc_stopped(int pid) { return pid == 7 && owner_stopped; }
static void power_doze(void) { dozes++; }
static bool power_key(void) { return true; }
static void power_remote_activity(void) {}
static bool alarm_key(const char *s, size_t n) { (void)s; (void)n; return false; }
static void vt_size(int *c, int *r) { *c = 53; *r = 23; }
static int vt_scroll(int step) { scroll_calls++; return step > 0 ? step : 0; }
static void vt_scroll_end(void) { scroll_calls++; }
static int xSemaphoreTake(void *s, int wait) { (void)s; (void)wait; return 1; }
static void xSemaphoreGive(void *s) { (void)s; }
static void cooked_byte(struct tty *tty, int c, bool last)
{ (void)tty; (void)c; (void)last; cooked++; }
static int tty_switch(int which) { front = which; return 0; }
static size_t xStreamBufferSend(void *s, const void *data, size_t n, int wait)
{
	(void)s; (void)wait;
	assert(used + n < sizeof(input));
	memcpy(input + used, data, n);
	used += n;
	return n;
}
void tty_input_remote(const char *s, size_t n);
#include "tty_keys_under_test.h"

int pt_ioctl(int fd, int req, void *arg)
{
	struct pt_file f = { &ttys[fd] };

	return tty_ioctl(&f, req, arg);
}
ssize_t pt_read(int fd, void *buf, size_t n)
{
	(void)fd;
	assert(n == 1);
	if (pos == used)
		return -EAGAIN;
	*(char *)buf = input[pos++];
	return 1;
}
#include "../kernel/keys.c"

static int decode(const char *s)
{
	used = strlen(s);
	pos = 0;
	memcpy(input, s, used);
	return pt_readkey(0);
}

static void app_key(const char *s, int key)
{
	int before = scroll_calls;

	used = pos = 0;
	tty_input_remote(s, strlen(s));
	assert(scroll_calls == before);
	assert(pt_readkey(0) == key && pos == used);
}

int main(void)
{
	int on = 1, off = 0;

	for (int i = 0; i < CONFIG_PT_VT_COUNT; i++) {
		ttys[i].input = ttys[i].in_lock = &ttys[i];
		ttys[i].raw = true;
		ttys[i].fg_pgid = 7;
	}
	assert(decode("\x1b[5~") == PT_KEY_PGUP);
	assert(decode("\x1b[5;2~") == PT_KEY_PGUP);
	assert(decode("\x1b[6;5~") == PT_KEY_PGDN);
	assert(decode("\x1b[1;5H") == PT_KEY_CTRL_HOME);
	assert(decode("\x1b[1;5F") == PT_KEY_CTRL_END);
	assert(decode("\x1b[999999999999999999999999~") == PT_KEY_UNKNOWN);
	used = pos = 0;
	tty_input_remote("\x01", 1);
	tty_input_remote("\x1b[A", 3);
	assert(scrolling && scroll_calls == 1 && !used);
	assert(!pt_ioctl(0, PT_TTY_SETSCROLL, &on));
	assert(!scrolling && ttys[0].app_scroll_pid == 7);
	app_key("\x01\x1b[A", PT_KEY_PGUP);
	app_key("\x01\x1b[B", PT_KEY_PGDN);
	app_key("\x01\x1b[H", PT_KEY_CTRL_HOME);
	app_key("\x01\x1b[F", PT_KEY_CTRL_END);
	app_key("\x1b[5;2~", PT_KEY_PGUP);
	app_key("\x1b[A", PT_KEY_UP);
	app_key("\x01\x01", PT_CTRL('a'));
	tty_input_remote("\x01" "2", 2);
	assert(front == 1 && !ttys[1].app_scroll_pid);
	front = 0;
	assert(!pt_ioctl(0, PT_TTY_SETSCROLL, &off));
	assert(!ttys[0].app_scroll_pid);
	assert(!pt_ioctl(0, PT_TTY_SETSCROLL, &on));
	owner_stopped = true;
	int before = scroll_calls;

	tty_input_remote("\x01\x1b[A", 4);
	assert(scrolling && scroll_calls == before + 1);
	owner_stopped = false;
	assert(!pt_ioctl(0, PT_TTY_SETSCROLL, &on));
	owner_alive = false;
	before = scroll_calls;

	tty_input_remote("\x01\x1b[A", 4);
	assert(!ttys[0].app_scroll_pid && scroll_calls == before + 1);
	puts("TTY keys: app/shell scrolling, switching, owner exit, modifiers and long sequences passed");
	return 0;
}
