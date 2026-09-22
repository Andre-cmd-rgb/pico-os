/*
 * top - CPU use per core and per task, refreshed every second.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/drivers.h"
#include "util.h"

#define MAX_TASKS	48

struct sample {
	TaskHandle_t	handle;
	uint32_t	runtime;
};

struct row {
	char		name[16];
	int		core;
	unsigned	prio;
	unsigned long	stack_free;
	uint32_t	delta;
};

static int compare_rows(const void *a, const void *b)
{
	const struct row *x = a, *y = b;

	return x->delta < y->delta ? 1 : x->delta > y->delta ? -1 : strcmp(x->name, y->name);
}

static uint32_t previous(const struct sample *prev, int n, TaskHandle_t h, uint32_t now)
{
	for (int i = 0; i < n; i++)
		if (prev[i].handle == h)
			return prev[i].runtime;
	return now;	/* new task: count from now */
}

static void percent(char *out, size_t size, uint64_t part, uint64_t whole)
{
	unsigned tenths = whole ? (unsigned)(part * 1000 / whole) : 0;

	if (tenths > 1000)
		tenths = 1000;
	snprintf(out, size, "%3u.%u", tenths / 10, tenths % 10);
}

PT_PROGRAM(top, "show CPU use per core and per task\n"
	   "usage: top [-n count]\n"
	   "Refreshes every second; q or Esc quits.")
{
	TaskStatus_t *tasks = pt_malloc(MAX_TASKS * sizeof(*tasks));
	struct sample prev[MAX_TASKS];
	struct row rows[MAX_TASKS];
	int nprev = 0, count = -1, cols, lines;
	bool tty = pt_isatty(PT_STDIN) && pt_isatty(PT_STDOUT);
	int64_t prev_time = esp_timer_get_time();

	if (argc == 3 && !strcmp(argv[1], "-n"))
		count = atoi(argv[2]);
	if (!tasks)
		return fail("top", NULL, -ENOMEM);
	if (!tty && count < 0)
		count = 1;
	pt_tty_size(PT_STDOUT, &cols, &lines);

	/* first sample: nothing to compare against yet */
	int n = uxTaskGetSystemState(tasks, MAX_TASKS, NULL);
	for (int i = 0; i < n; i++)
		prev[i] = (struct sample) { tasks[i].xHandle, tasks[i].ulRunTimeCounter };
	nprev = n;

	if (tty) {
		pt_tty_raw(PT_STDIN, true);
		pt_puts("\x1b[?25l\x1b[2J");
	}
	for (int round = 0; count < 0 || round < count; round++) {
		int wait = 1000;
		bool quit = false;

		if (tty) {
			pt_ioctl(PT_STDIN, PT_TTY_SETTIMEOUT, &wait);
			unsigned char c;
			ssize_t r = pt_read(PT_STDIN, &c, 1);
			if (r == 1 && (c == 'q' || c == 'Q' || c == 0x1b || c == 0x03))
				quit = true;
		} else {
			pt_sleep_ms(wait);
		}
		if (quit)
			break;

		int64_t now_time = esp_timer_get_time();
		uint64_t elapsed = now_time - prev_time;
		prev_time = now_time;
		n = uxTaskGetSystemState(tasks, MAX_TASKS, NULL);
		uint32_t idle[2] = { 0, 0 };
		struct sample next[MAX_TASKS];
		for (int i = 0; i < n; i++) {
			uint32_t delta = tasks[i].ulRunTimeCounter -
					 previous(prev, nprev, tasks[i].xHandle, tasks[i].ulRunTimeCounter);
			rows[i] = (struct row) {
				.core = tasks[i].xCoreID == tskNO_AFFINITY ? -1 : tasks[i].xCoreID,
				.prio = tasks[i].uxCurrentPriority,
				.stack_free = tasks[i].usStackHighWaterMark,
				.delta = delta,
			};
			strlcpy(rows[i].name, tasks[i].pcTaskName, sizeof(rows[i].name));
			if (!strcmp(rows[i].name, "IDLE0"))
				idle[0] = delta;
			if (!strcmp(rows[i].name, "IDLE1"))
				idle[1] = delta;
			next[i] = (struct sample) { tasks[i].xHandle, tasks[i].ulRunTimeCounter };
		}
		memcpy(prev, next, n * sizeof(next[0]));
		nprev = n;
		qsort(rows, n, sizeof(rows[0]), compare_rows);

		char c0[8], c1[8], p[8], elapsed_s[16];
		int min, max;
		long up = now_time / 1000000;
		percent(c0, sizeof(c0), elapsed - (idle[0] < elapsed ? idle[0] : elapsed), elapsed);
		percent(c1, sizeof(c1), elapsed - (idle[1] < elapsed ? idle[1] : elapsed), elapsed);
		cpufreq_get(&min, &max);
		snprintf(elapsed_s, sizeof(elapsed_s), "%ld:%02ld:%02ld", up / 3600, up / 60 % 60, up % 60);

		if (tty)
			pt_puts("\x1b[H");
		pt_printf("top - up %s, %d procs, %d tasks\x1b[K\n", elapsed_s, proc_count(), n);
		pt_printf("cpu0 %s%%  cpu1 %s%%  %d MHz (%s)\x1b[K\n", c0, c1, cpufreq_current_mhz(),
			  cpufreq_policy_name(min, max));
		pt_printf("\x1b[K\n\x1b[7m%6s %4s %4s %8s %-15s\x1b[0m\x1b[K\n", "CPU%", "CORE", "PRIO", "STACKFRE", "TASK");
		int shown = tty ? lines - 5 : n;
		for (int i = 0; i < n && i < shown; i++) {
			if (!strncmp(rows[i].name, "IDLE", 4))
				continue;
			percent(p, sizeof(p), rows[i].delta, elapsed);
			pt_printf("%6s %4s %4u %7luB %-15s\x1b[K\n", p,
				  rows[i].core < 0 ? "-" : rows[i].core ? "1" : "0",
				  rows[i].prio, rows[i].stack_free, rows[i].name);
		}
		if (tty)
			pt_puts("\x1b[J");
	}
	if (tty) {
		int off = -1;
		pt_ioctl(PT_STDIN, PT_TTY_SETTIMEOUT, &off);
		pt_tty_raw(PT_STDIN, false);
		pt_puts("\x1b[?25h\n");
	}
	pt_free(tasks);
	return 0;
}
