#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

struct semaphore { int value; };
typedef struct semaphore *SemaphoreHandle_t;
typedef struct semaphore StaticSemaphore_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
struct pt_file;
struct pt_file_ops {
	ssize_t (*read)(struct pt_file *, void *, size_t);
	int (*ioctl)(struct pt_file *, int, void *);
};
struct pt_file { const struct pt_file_ops *ops; };
struct proc { struct pt_file *fd[1]; };
struct helper { TaskHandle_t task; SemaphoreHandle_t go, done, exited; volatile bool quit; };
struct blitter { TaskHandle_t task; SemaphoreHandle_t go, done, exited; volatile bool quit; };
#define MAX_SLICES 2
#define HELPER_CORE 0
#define pdPASS 1
#define PT_TTY_SETRAW 1
#define PT_TTY_TRYSETRAW 2
#define PT_TTY_SETTIMEOUT 3
#define PT_KEY_EOF -2
#define PT_KEY_NONE -3
#define PT_KEY_INTR -4
#define PT_KEY_ERROR -5
#define portMAX_DELAY 0xffffffffu
static int allocations, fail_at, resources, deleted, boosts, keeps, cooked;
static bool policy_busy;
static int tty_error, tty_calls;
static int read_timeout = -1;
static bool kill_pending;
static jmp_buf key_exit;
static struct proc key_process;
static void *key_cleanup;
static bool video_cleanup_step(void *arg);

static SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
	SemaphoreHandle_t s;

	if (++allocations == fail_at)
		return NULL;
	s = calloc(1, sizeof(*s));
	assert(s);
	resources++;
	return s;
}
static int xSemaphoreGive(SemaphoreHandle_t s) { assert(s); s->value = 1; return 1; }
static int xSemaphoreTake(SemaphoreHandle_t s, unsigned timeout)
{
	assert(s);
	if (!s->value)
		return 0;
	s->value = 0;
	return 1;
}
static void vSemaphoreDelete(SemaphoreHandle_t s) { assert(s); resources--; free(s); }
static int xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
				 void *arg, int priority, TaskHandle_t *task, int core)
{
	if (++allocations == fail_at)
		return 0;
	*task = malloc(1);
	assert(*task);
	resources++;
	return pdPASS;
}
static void vTaskDelete(TaskHandle_t task) { assert(task); deleted++; resources--; free(task); }
static void cpufreq_boost(bool on) { boosts += on ? 1 : -1; }
static void power_keep_screen(bool on) { keeps += on ? 1 : -1; }
static bool cpufreq_try_boost(bool on)
{
	if (policy_busy)
		return false;
	cpufreq_boost(on);
	return true;
}
static int tty_ioctl(struct pt_file *f, int request, void *value)
{
	if (request == PT_TTY_SETTIMEOUT) {
		assert(*(int *)value == -1 || *(int *)value == 200);
		if (tty_error)
			return tty_error;
		read_timeout = *(int *)value;
		return 0;
	}
	assert(request == PT_TTY_TRYSETRAW && !*(int *)value);
	tty_calls++;
	if (tty_error)
		return tty_error;
	cooked++;
	return 0;
}

#include "video_cleanup_under_test.h"

static struct proc *proc_current(void) { return &key_process; }
static struct pt_file *fd_file(struct proc *p, int fd)
{ assert(fd == 0); return p->fd[0]; }
static struct pt_file *file_get(struct pt_file *f) { return f; }
static void file_put(struct pt_file *f) { assert(f); }
static void proc_check_signals(void)
{
	if (kill_pending) {
		/* The reset syscall exits before it reaches its own ioctl. */
		assert(read_timeout == 200);
		assert(video_cleanup_step(key_cleanup));
		assert(read_timeout == -1);
		longjmp(key_exit, 1);
	}
}
static ssize_t tty_read(struct pt_file *file, void *buf, size_t n)
{
	assert(file && buf && n == 1 && read_timeout == 200);
	kill_pending = true;
	return -EAGAIN;
}
#include "keys_exit_under_test.h"

int main(void)
{
	struct helper h = { 0 };
	struct blitter b = { 0 };
	bool boosted = true, kept = true, screen = true;
	const struct pt_file_ops ops = { .read = tty_read, .ioctl = tty_ioctl };
	const struct pt_file_ops no_ioctl = { 0 };
	struct pt_file tty = { .ops = &ops };
	struct pt_file other = { .ops = &no_ioctl };
	struct video_cleanup c = { .helpers = &h, .blit = &b, .tty = &tty,
		.boosted = &boosted, .kept = &kept, .screen = &screen };
	int old_calls;

	c.guard = &c.guard_storage;
	c.guard->value = 1;
	boosts = keeps = 1;
	assert(!task_start(NULL, "decode", 4096, &h, &h.task, &h.go, &h.done, &h.exited));
	assert(!task_start(NULL, "blit", 3072, &b, &b.task, &b.go, &b.done, &b.exited));
	/* A completed frame is not an exited task. Keep all borrowed memory. */
	xSemaphoreGive(h.done);
	xSemaphoreGive(b.done);
	assert(!video_cleanup_step(&c) && h.quit && b.quit && !deleted && resources == 8);
	xSemaphoreGive(h.exited);
	assert(!video_cleanup_step(&c) && deleted == 1 && boosts == 1 && keeps == 1);
	xSemaphoreGive(b.exited);
	/* Retry a busy policy without blocking or retiring resources twice. */
	policy_busy = true;
	assert(!video_cleanup_step(&c) && deleted == 2 && !resources);
	assert(boosted && kept && boosts == 1 && keeps == 1 && !tty_calls);
	assert(!video_cleanup_step(&c) && deleted == 2 && !resources);
	policy_busy = false;
	/* Cooked-mode restoration may also need retries after counters drop. */
	read_timeout = 200;
	tty_error = -EAGAIN;
	assert(!video_cleanup_step(&c) && !boosts && !keeps && !cooked);
	assert(!boosted && !kept && tty_calls == 1 && read_timeout == 200);
	assert(!video_cleanup_step(&c) && !boosts && !keeps && !cooked);
	assert(tty_calls == 2 && deleted == 2 && !resources);
	tty_error = 0;
	assert(video_cleanup_step(&c) && deleted == 2 && !resources && !boosts && !keeps);
	assert(!boosted && !kept && cooked == 1 && read_timeout == -1);
	assert(video_cleanup_step(&c) && !boosts && !keeps && !resources);
	/* Redirected/nonterminal stdin must not keep a process alive forever. */
	tty_error = -ENOTTY;
	assert(video_cleanup_step(&c) && !boosts && !keeps && !resources);
	assert(cooked == 2);
	old_calls = tty_calls;
	c.tty = &other;
	assert(video_cleanup_step(&c) && tty_calls == old_calls);
	c.tty = NULL;
	assert(video_cleanup_step(&c) && tty_calls == old_calls);
	/* A half-published startup is protected by the parent's mutex. */
	c.guard->value = 0;
	assert(!video_cleanup_step(&c));
	c.guard->value = 1;
	for (int failure = 1; failure <= 4; failure++) {
		h = (struct helper){ 0 };
		allocations = 0;
		fail_at = failure;
		assert(task_start(NULL, "fail", 4096, &h, &h.task, &h.go, &h.done, &h.exited) == -ENOMEM);
		assert(video_cleanup_step(&c) && !resources);
	}
	/* Actual read_byte/pt_read/pt_ioctl: kill arrives after a timed read,
	 * and syscall entry exits before read_byte can restore its timeout. */
	c.tty = &tty;
	tty_error = 0;
	key_process.fd[0] = &tty;
	key_cleanup = &c;
	if (!setjmp(key_exit)) {
		read_byte(0, 200);
		assert(!"pending kill must exit the reset syscall");
	}
	assert(kill_pending && read_timeout == -1 && !resources && !boosts && !keeps);
	puts("video cleanup: helper ownership/retries, timed-read exit restoration, redirected input and startup failures passed");
	return 0;
}
