#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

typedef void *TaskHandle_t;
struct proc {
	int state, pid, status;
	char name[16];
	TaskHandle_t task;
	atomic_bool exiting;
	bool forced_cleanup, kill_waiting;
	int64_t kill_deadline_us;
	bool (*cleanup)(void *arg);
	void *cleanup_arg;
};
#define PROC_RUNNING 1
#define PT_SIGKILL 9
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(n) (n)
#define LOCK() do { assert(!locked); locked = true; } while (0)
#define UNLOCK() do { assert(locked); locked = false; } while (0)

static struct proc process, *current;
static bool locked, task_locked, suspended;
static int callbacks, remaining, deleted, torn_down, resumed, delays;
static void *finished;
static jmp_buf exit_point;

static struct proc *proc_current(void) { return current; }
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return process.task; }
static void vTaskDelete(TaskHandle_t task) { assert(task); }
static void vTaskDeleteWithCaps(TaskHandle_t task)
{
	assert(task == process.task && suspended && !remaining && !process.cleanup);
	deleted++;
}
static void vTaskSuspend(TaskHandle_t task)
{
	if (!task)
		longjmp(exit_point, 1);
	suspended = true;
}
static void vTaskResume(TaskHandle_t task) { suspended = false; resumed++; }
static void vTaskDelay(unsigned ticks) { delays++; }
static bool holds_lock(TaskHandle_t task) { return task_locked; }
static void klog(const char *format, ...) { }
static void xQueueSend(void *queue, TaskHandle_t *task, unsigned timeout) { }
static void teardown(struct proc *p, int status)
{
	assert(!p->cleanup && !remaining);
	p->status = status;
	p->state = 0;
	torn_down++;
}

#include "proc_under_test.h"

static bool helper_exit(void *arg)
{
	assert(arg == &remaining && !locked && process.state == PROC_RUNNING);
	assert(atomic_load(&process.exiting) && !deleted && !torn_down);
	callbacks++;
	if (remaining)
		remaining--;
	return !remaining;
}

static void reset(void)
{
	process = (struct proc){ .state = PROC_RUNNING, .pid = 1,
		.task = (void *)1, .kill_deadline_us = 1 };
	current = &process;
	locked = task_locked = suspended = false;
	callbacks = deleted = torn_down = resumed = delays = 0;
	remaining = 3;
	assert(!proc_set_cleanup(helper_exit, &remaining));
}

int main(void)
{
	reset();
	task_locked = true;
	force_kill(&process, 2);
	assert(resumed == 1 && !deleted && !callbacks);
	task_locked = false;
	force_kill(&process, 2);
	assert(suspended && process.forced_cleanup && remaining == 2 && !deleted);
	assert(proc_set_cleanup(NULL, NULL) == -EBUSY);
	force_kill(&process, 3);
	assert(remaining == 1 && !deleted && !torn_down);
	force_kill(&process, 4);
	assert(deleted == 1 && torn_down == 1 && callbacks == 3 && process.status == 137);

	reset();
	if (!setjmp(exit_point))
		pt_exit(7);
	assert(torn_down == 1 && !deleted && callbacks == 3 && process.status == 7);
	current = NULL;
	assert(proc_set_cleanup(NULL, NULL) == -EPERM);
	puts("process cleanup: cooperative exit, forced exit, lock deferral and memory ordering passed");
	return 0;
}
