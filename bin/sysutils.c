/*
 * System programs: help ps kill free dmesg uptime uname date sleep env which
 * clear reboot poweroff true false, and the hardware checks lcdtest keytest.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/drivers.h"
#include "util.h"

/* ------------------------------------------------------------ help */

/*
 * A line of help, broken at spaces to fit the terminal. What does not fit
 * goes on under where the line's description starts -- after the gap of
 * two spaces that ends an option or an example -- or under the line's own
 * indent, so a table of options still reads as one on a narrow screen.
 */
static void print_wrapped(const char *line, size_t len, int cols)
{
	size_t indent = 0, at = 0;

	while (indent < len && line[indent] == ' ')
		indent++;
	const char *gap = memmem(line + indent, len - indent, "  ", 2);
	size_t hang = gap ? (size_t)(gap - line) : indent;

	while (gap && hang < len && line[hang] == ' ')
		hang++;
	if (hang > (size_t)cols / 2)
		hang = indent + 2;
	while (at < len) {
		size_t room = cols - 1 - (at ? hang : 0), end = at + room;

		if (len - at <= room) {
			end = len;
		} else {
			while (end > at && line[end] != ' ')
				end--;
			if (end == at)
				end = at + room;	/* one word wider than the screen */
		}
		pt_printf("%*s%.*s\n", at ? (int)hang : 0, "", (int)(end - at), line + at);
		for (at = end; at < len && line[at] == ' '; at++)
			;
	}
	if (!len)
		pt_puts("\n");
}

PT_PROGRAM(help, "list commands, or explain one\nusage: help [command]")
{
	if (argc > 1) {
		const struct pt_program *p = program_find(argv[1]);
		char *text;

		if (!p) {
			pt_dprintf(PT_STDERR, "help: no command named %s\n", argv[1]);
			return 1;
		}
		int cols, rows;

		pt_tty_size(PT_STDOUT, &cols, &rows);
		if (!pt_isatty(PT_STDOUT) || !(text = pt_malloc(strlen(p->name) + strlen(p->help) + 4))) {
			pt_printf("%s - %s\n", p->name, p->help);
			return 0;
		}
		sprintf(text, "%s - %s", p->name, p->help);
		for (const char *line = text; *line;) {
			size_t n = strcspn(line, "\n");

			print_wrapped(line, n, cols);
			line += n + (line[n] == '\n');
		}
		pt_free(text);
		return 0;
	}

	int cols, rows, width = 0, n = 0;
	pt_tty_size(PT_STDOUT, &cols, &rows);
	for (const struct pt_program *p = program_first(); p; p = p->next, n++)
		if ((int)strlen(p->name) > width)
			width = strlen(p->name);
	width += 2;
	int per_row = cols / width > 0 ? cols / width : 1;
	int lines = (n + per_row - 1) / per_row;

	const struct pt_program **list = pt_malloc(n * sizeof(*list));
	int count = 0;

	if (!list)
		return fail("help", NULL, -ENOMEM);
	for (const struct pt_program *p = program_first(); p; p = p->next)
		list[count++] = p;
	pt_printf("\x1b[1mCommands\x1b[0m (help <command> for details)\n");
	for (int r = 0; r < lines; r++) {
		for (int c = 0; c < per_row; c++) {
			int i = c * lines + r;
			if (i < count)
				pt_printf("%-*s", c + 1 < per_row ? width : 0, list[i]->name);
		}
		pt_puts("\n");
	}
	pt_free(list);
	pt_printf("\x1b[2mTab completes, Up/Down is history, Esc clears the\n"
		  "line or stops what runs. Fn is the control key: Fn C\n"
		  "is Ctrl-C, Fn 1-4 a terminal. `man intro` has more.\x1b[0m\n");
	return 0;
}

/* ------------------------------------------------------------ processes */

/* Processor time as ps shows it: M:SS, or H:MM:SS past an hour. */
static void format_cpu(uint64_t us, char *out, size_t size)
{
	unsigned long s = us / 1000000;

	if (s >= 3600)
		snprintf(out, size, "%lu:%02lu:%02lu", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(out, size, "%lu:%02lu", s / 60, s % 60);
}

static int compare_tasks(const void *a, const void *b)
{
	return strcmp(pcTaskGetName(((const TaskStatus_t *)a)->xHandle),
		      pcTaskGetName(((const TaskStatus_t *)b)->xHandle));
}

PT_PROGRAM(ps, "list processes\nusage: ps [-a]\n  -a  also list kernel tasks")
{
	struct pt_procinfo procs[CONFIG_PT_MAX_PROCS];
	uint32_t flags;

	if (parse_flags("ps", argc, argv, "a", &flags) < 0)
		return 2;

	int n = proc_list(procs, CONFIG_PT_MAX_PROCS);
	pt_printf("%5s %5s %s %9s %8s %s\n", "PID", "PPID", "S", "STACK", "TIME", "COMMAND");	/* STACK: peak used/size */
	for (int i = 0; i < n; i++) {
		char cpu[16], stack[16];

		format_cpu(procs[i].cpu_us, cpu, sizeof(cpu));
		if (procs[i].state == 'Z')	/* finished: its stack is gone */
			snprintf(stack, sizeof(stack), "-");
		else
			snprintf(stack, sizeof(stack), "%lu/%luK",
				 (unsigned long)(procs[i].stack_kb * 1024 - procs[i].stack_free + 1023) / 1024,
				 (unsigned long)procs[i].stack_kb);
		pt_printf("%5d %5d %c %9s %8s %s\n", procs[i].pid, procs[i].ppid, procs[i].state,
			  stack, cpu, procs[i].name);
	}
	if (!FLAG(flags, 'a'))
		return 0;

	UBaseType_t count = uxTaskGetNumberOfTasks();
	TaskStatus_t *tasks = pt_malloc(count * sizeof(*tasks));
	if (!tasks)
		return fail("ps", NULL, -ENOMEM);
	count = uxTaskGetSystemState(tasks, count, NULL);
	qsort(tasks, count, sizeof(*tasks), compare_tasks);
	pt_printf("\n%-16s %4s %4s %9s\n", "KERNEL TASK", "CORE", "PRIO", "STACK FREE");
	for (UBaseType_t i = 0; i < count; i++) {
		bool is_proc = false;
		for (int k = 0; k < n; k++)
			is_proc |= !strncmp(procs[k].name, tasks[i].pcTaskName, 15);
		if (is_proc)
			continue;
		BaseType_t core = tasks[i].xCoreID;
		pt_printf("[%-14.14s] %4s %4u %8luB\n", tasks[i].pcTaskName,
			  core == tskNO_AFFINITY ? "-" : core ? "1" : "0",
			  (unsigned)tasks[i].uxCurrentPriority,
			  (unsigned long)tasks[i].usStackHighWaterMark);
	}
	pt_free(tasks);
	return 0;
}

static const struct {
	const char	*name;
	int		 sig;
} signals[] = {
	{ "INT", PT_SIGINT }, { "KILL", PT_SIGKILL }, { "TERM", PT_SIGTERM },
	{ "CONT", PT_SIGCONT }, { "STOP", PT_SIGSTOP }, { "TSTP", PT_SIGTSTP },
};

PT_PROGRAM(kill, "send a signal to a process\n"
	   "usage: kill [-SIG | -N | -s SIG] pid...   kill -l\n"
	   "Signals: INT TERM KILL (end it), STOP TSTP (stop it),\n"
	   "CONT (carry on). Default is TERM. In the shell,\n"
	   "%1 means job 1.")
{
	const char *name = NULL;
	int sig = PT_SIGTERM, i = 1, status = 0;

	if (i < argc && !strcmp(argv[i], "-l")) {
		for (size_t k = 0; k < sizeof(signals) / sizeof(signals[0]); k++)
			pt_printf("%2d %s\n", signals[k].sig, signals[k].name);
		return 0;
	}
	if (i + 1 < argc && !strcmp(argv[i], "-s")) {
		name = argv[i + 1];
		i += 2;
	} else if (i < argc && argv[i][0] == '-' && argv[i][1]) {
		name = argv[i++] + 1;
	}
	if (name) {
		size_t k = 0;

		if (!strncmp(name, "SIG", 3))
			name += 3;
		while (k < sizeof(signals) / sizeof(signals[0]) && strcmp(name, signals[k].name) &&
		       atoi(name) != signals[k].sig)
			k++;
		if (k == sizeof(signals) / sizeof(signals[0])) {
			pt_dprintf(PT_STDERR, "kill: unknown signal %s (kill -l lists them)\n", name);
			return 2;
		}
		sig = signals[k].sig;
	}
	if (i == argc) {
		pt_dprintf(PT_STDERR, "usage: kill [-SIG] pid...\n");
		return 2;
	}
	for (; i < argc; i++) {
		char *end;
		long pid = strtol(argv[i], &end, 10);
		int err = *end || end == argv[i] ? -EINVAL : pt_kill((int)pid, sig);

		if (err)
			status = fail("kill", argv[i], err == -EINVAL ? -ESRCH : err);
	}
	return status;
}

/* ------------------------------------------------------------ memory, log, time */

PT_PROGRAM(free, "show memory use in KB")
{
	static const struct {
		const char *name;
		uint32_t    caps;
	} pools[] = {
		{ "internal", MALLOC_CAP_INTERNAL },
		{ "psram", MALLOC_CAP_SPIRAM },
	};

	pt_printf("%-8s %7s %7s %7s %7s %7s\n", "", "total", "used", "free", "largest", "low");
	for (size_t i = 0; i < sizeof(pools) / sizeof(pools[0]); i++) {
		multi_heap_info_t info;
		heap_caps_get_info(&info, pools[i].caps);
		size_t total = info.total_free_bytes + info.total_allocated_bytes;

		/* "low" is the least free there has ever been since boot:
		 * the number that says how close this came to the edge. */
		pt_printf("%-8s %7zu %7zu %7zu %7zu %7zu\n", pools[i].name, total / 1024,
			  info.total_allocated_bytes / 1024, info.total_free_bytes / 1024,
			  info.largest_free_block / 1024,
			  heap_caps_get_minimum_free_size(pools[i].caps) / 1024);
	}
	return 0;
}

PT_PROGRAM(dmesg, "print the kernel log\nusage: dmesg [-c]\n  -c  clear the log after printing")
{
	uint32_t flags;
	char buf[256];
	size_t off = 0, n;

	if (parse_flags("dmesg", argc, argv, "c", &flags) < 0)
		return 2;
	while ((n = klog_read(off, buf, sizeof(buf))) > 0) {
		write_all(PT_STDOUT, buf, n);
		off += n;
	}
	if (FLAG(flags, 'c'))
		klog_clear();
	return 0;
}

/*
 * As Linux words it: "up 12 min" under an hour, "up 3:05" under a day,
 * "up 2 days, 3:05" after that -- never "10:09" for ten minutes, which
 * reads as ten hours.
 */
PT_PROGRAM(uptime, "show how long the system has been running")
{
	unsigned long m = pt_uptime_us() / 60000000;
	time_t now = time(NULL);
	char clock[16] = "--:--:--", up[32];
	struct tm tm;

	if (m < 60)
		snprintf(up, sizeof(up), "%lu min", m);
	else if (m < 24 * 60)
		snprintf(up, sizeof(up), "%lu:%02lu", m / 60, m % 60);
	else
		snprintf(up, sizeof(up), "%lu day%s, %lu:%02lu", m / 1440, m / 1440 == 1 ? "" : "s",
			 m / 60 % 24, m % 60);
	if (now > 1600000000 && localtime_r(&now, &tm))
		strftime(clock, sizeof(clock), "%H:%M:%S", &tm);
	pt_printf("%s up %s, %d process%s\n", clock, up, proc_count(),
		  proc_count() == 1 ? "" : "es");
	return 0;
}

PT_PROGRAM(uname, "print system information\n"
	   "usage: uname [-asnrvm]\n"
	   "  -s system  -n host  -r release  -v version  -m machine  -a all")
{
	const esp_app_desc_t *app = esp_app_get_description();
	const char *host = pt_getenv("HOSTNAME") ? pt_getenv("HOSTNAME") : "pockettype";
	char version[112];
	bool want[5] = { false }, any = false;	/* s n r v m, in that order */
	struct opt o = { .ind = 1 };
	int c;

	while ((c = getopt_pt(&o, "uname", argc, argv, "asnrvm")) != -1) {
		const char *at = strchr("snrvm", c);

		if (c == 'a')
			want[0] = want[1] = want[2] = want[3] = want[4] = true;
		else if (at)
			want[at - "snrvm"] = true;
		else
			return 2;
		any = true;
	}
	if (o.ind < argc) {
		pt_dprintf(PT_STDERR, "usage: uname [-asnrvm]\n");
		return 2;
	}
	if (!any)
		want[0] = true;
	snprintf(version, sizeof(version), "#1 SMP %s %s esp-idf-%s", app->date, app->time,
		 app->idf_ver);
#if CONFIG_IDF_TARGET_ARCH_RISCV
	const char *arch = "riscv32";
#else
	const char *arch = "xtensa";
#endif
	const char *parts[5] = { "PocketType", host, PT_VERSION, version, arch };

	for (int i = 0, n = 0; i < 5; i++)
		if (want[i])
			pt_printf("%s%s", n++ ? " " : "", parts[i]);
	pt_puts("\n");
	return 0;
}

PT_PROGRAM(date, "show or set the date and time\n"
	   "usage: date [+format]\n"
	   "       date -s \"YYYY-MM-DD HH:MM[:SS]\"\n"
	   "Wi-Fi sets the clock by itself; -s is for when there is none.")
{
	char out[128];
	struct tm tm = { 0 };

	if (argc == 3 && !strcmp(argv[1], "-s")) {
		int sec = 0;
		int got = sscanf(argv[2], "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
				 &tm.tm_hour, &tm.tm_min, &sec);
		if (got < 5) {
			pt_dprintf(PT_STDERR, "date: expected \"YYYY-MM-DD HH:MM[:SS]\"\n");
			return 2;
		}
		tm.tm_year -= 1900;
		tm.tm_mon -= 1;
		tm.tm_sec = sec;
		tm.tm_isdst = -1;
		struct timeval tv = { .tv_sec = mktime(&tm) };
		settimeofday(&tv, NULL);
		clock_changed(false);
	}

	time_t now = time(NULL);
	localtime_r(&now, &tm);
	const char *format = argc == 2 && argv[1][0] == '+' ? argv[1] + 1 : "%a %b %e %H:%M:%S %Z %Y";
	if (!strftime(out, sizeof(out), format, &tm))
		out[0] = '\0';			/* empty, or longer than out */
	pt_printf("%s\n", out);
	return 0;
}

PT_PROGRAM(time, "run a command and show how long it took\nusage: time command [args...]")
{
	if (argc < 2) {
		pt_dprintf(PT_STDERR, "usage: time command [args...]\n");
		return 2;
	}
	const struct pt_spawn req = {
		.cmd = argv[1],
		.argc = argc - 1,
		.argv = argv + 1,
		.fd = { PT_STDIN, PT_STDOUT, PT_STDERR },
		.pgid = 0,
	};
	int64_t start = pt_uptime_us();
	int pid = pt_spawn(&req);
	if (pid < 0) {
		fail("time", argv[1], pid);
		return 127;
	}

	int group = pid, self = pt_getpid(), status = 0;
	bool tty = pt_isatty(PT_STDIN);
	pt_sigcatch(true);
	if (tty)
		pt_ioctl(PT_STDIN, PT_TTY_SETPGRP, &group);
	while (pt_wait(pid, &status, false) == -EINTR)
		;
	if (tty)
		pt_ioctl(PT_STDIN, PT_TTY_SETPGRP, &self);
	int64_t us = pt_uptime_us() - start;
	pt_dprintf(PT_STDERR, "real %lld.%03llds\n", us / 1000000, us / 1000 % 1000);
	return status;
}

PT_PROGRAM(sleep, "wait for a number of seconds\nusage: sleep seconds")
{
	char *end;
	double s = argc == 2 ? strtod(argv[1], &end) : -1;

	if (argc != 2 || *end || !(s >= 0 && s <= 4000000)) {
		pt_dprintf(PT_STDERR, "usage: sleep seconds   (0 to 4000000, fractions allowed)\n");
		return 2;
	}
	return pt_sleep_ms((uint32_t)(s * 1000)) ? 1 : 0;
}

PT_PROGRAM(env, "print the environment, or run a command\n"
	   "with changes to it\n"
	   "usage: env [-i] [-u NAME] [NAME=value]...\n"
	   "           [command [argument...]]\n"
	   "  -i  start from an empty environment\n"
	   "  -u NAME  leave NAME out")
{
	int i = 1;

	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-i")) {
			const char *entry;

			/* one at a time from the front: each unset moves the rest up */
			while (pt_environ(0, &entry)) {
				char name[64];
				size_t n = strcspn(entry, "=");

				snprintf(name, sizeof(name), "%.*s", (int)n, entry);
				if (pt_unsetenv(name))
					break;
			}
		} else if (!strcmp(argv[i], "-u") && i + 1 < argc) {
			pt_unsetenv(argv[++i]);
		} else if (!strcmp(argv[i], "--")) {
			i++;
			break;
		} else {
			pt_dprintf(PT_STDERR, "usage: env [-i] [-u NAME] [NAME=value]... [command...]\n");
			return 2;
		}
	}
	/* this process's environment is what the command is started with */
	for (; i < argc && strchr(argv[i], '=') && argv[i][0] != '='; i++) {
		char *eq = strchr(argv[i], '=');

		*eq = '\0';
		int err = pt_setenv(argv[i], eq + 1);

		*eq = '=';
		if (err)
			return fail("env", argv[i], err);
	}
	if (i < argc) {
		int status = run_command(argc - i, argv + i);

		if (status == -ENOENT) {
			pt_dprintf(PT_STDERR, "env: %s: command not found\n", argv[i]);
			return 127;
		}
		return status < 0 ? fail("env", argv[i], status) + 125 : status;
	}
	const char *entry;

	for (int k = 0; pt_environ(k, &entry); k++)
		pt_printf("%s\n", entry);
	return 0;
}

PT_PROGRAM(whoami, "print the user's name")
{
	const char *user = pt_getenv("USER");

	pt_printf("%s\n", user ? user : CONFIG_PT_USERNAME);
	return 0;
}

PT_PROGRAM(hostname, "print the machine's name\n"
	   "usage: hostname\n"
	   "It is $HOSTNAME, which /etc/profile can set.")
{
	const char *host = pt_getenv("HOSTNAME");

	pt_printf("%s\n", host ? host : "pockettype");
	return 0;
}

PT_PROGRAM(which, "show where a command comes from\nusage: which command...")
{
	int status = 0;

	for (int i = 1; i < argc; i++) {
		if (program_find(argv[i])) {
			pt_printf("%s: built in\n", argv[i]);
			continue;
		}
		const char *path = pt_getenv("PATH");
		bool found = false;
		for (const char *dir = path ? path : ""; *dir && !found;) {
			size_t len = strcspn(dir, ":");
			char full[PT_PATH_MAX];
			struct pt_stat st;
			snprintf(full, sizeof(full), "%.*s/%s", (int)len, dir, argv[i]);
			if (!pt_stat(full, &st) && !st.is_dir) {
				pt_printf("%s\n", full);
				found = true;
			}
			dir += len + (dir[len] == ':');
		}
		if (!found) {
			pt_dprintf(PT_STDERR, "which: %s not found\n", argv[i]);
			status = 1;
		}
	}
	return status;
}

PT_PROGRAM(clear, "clear the screen")
{
	pt_puts("\x1b[2J\x1b[H");
	return 0;
}

PT_PROGRAM(true, "do nothing, successfully")
{
	return 0;
}

PT_PROGRAM(false, "do nothing, unsuccessfully")
{
	return 1;
}

PT_PROGRAM(reboot, "restart the system")
{
	pt_puts("rebooting...\n");
	vfs_sync_all();
	sd_unmount();
	pt_sleep_ms(200);
	esp_restart();
	return 0;
}

/*
 * ESP-IDF's table of power locks and modes, which is ninety columns wide,
 * cut down to what fits on the screen: each lock, what it holds the chip
 * to, how much of the time it has held it, and whether it does now; then
 * the time spent at each speed.
 */
static void print_pm_stats(char *text)
{
	enum { NONE, LOCKS, MODES } part = NONE;

	for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		char name[24], type[24], pct[8];
		unsigned long arg, active, count, us;
		int mhz;

		if (!strncmp(line, "Lock stats", 10)) {
			part = LOCKS;
			pt_printf("%-16s %-15s %5s %s\n", "LOCK", "HOLDS THE CHIP AT", "TIME", "NOW");
		} else if (!strncmp(line, "Mode stats", 10)) {
			part = MODES;
			pt_printf("%-16s %7s %5s\n", "MODE", "CPU", "TIME");
		} else if (part == LOCKS &&
			   sscanf(line, "%23s %23s %lu %lu %lu %lu %7[0-9]", name, type, &arg, &active,
				  &count, &us, pct) == 7) {
			pt_printf("%-16.16s %-15.15s %4s%% %s\n", name, type, pct, active ? "held" : "");
		} else if (part == MODES &&
			   sscanf(line, "%23s %d M %lu %7[0-9]", name, &mhz, &us, pct) == 4) {
			pt_printf("%-16.16s %3d MHz %4s%%\n", name, mhz, pct);
		}
	}
}

PT_PROGRAM(power, "show power state, or switch idle sleep\n"
	   "usage: power [sleep on | sleep off]\n"
	   "Idle sleep lets the chip light-sleep between events, keeping RAM.\n"
	   "It never happens while a PC uses the native USB console or a USB\n"
	   "keyboard is plugged in. The table shows time spent in each mode.")
{
	int min, max;

	if (argc == 3 && !strcmp(argv[1], "sleep") && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
		int err = cpufreq_set_idle_sleep(!strcmp(argv[2], "on"));
		if (err)
			return fail("power", "sleep", err);
	} else if (argc != 1) {
		pt_dprintf(PT_STDERR, "usage: power [sleep on | sleep off]\n");
		return 2;
	}
	cpufreq_get(&min, &max);
	pt_printf("cpufreq %s (%d-%d MHz), idle sleep %s\n", cpufreq_policy_name(min, max), min, max,
		  cpufreq_idle_sleep() ? "on" : "off");
	struct battery_status b;
	if (!battery_status(&b) && b.state != BATTERY_NONE && b.state != BATTERY_USB) {
		pt_printf("battery %d.%02d V (%d%%), %s, %d mA, %+d mV/min\n", b.mv / 1000,
			  b.mv % 1000 / 10, b.percent, battery_state_name(b.state), b.ma, b.trend);
		pt_printf("cell %d mAh, %d.%02d ohm\n", b.capacity_mah, b.mohm / 1000,
			  b.mohm % 1000 / 10);
	} else if (!battery_status(&b))
		pt_printf("battery none fitted\n");

	char *stats = pt_malloc(2048);
	if (stats && cpufreq_stats(stats, 2048) > 0)
		print_pm_stats(stats);
	pt_free(stats);
	return 0;
}

PT_PROGRAM(battery, "show the battery level\n"
	   "usage: battery [-w] [-c mAh]\n"
	   "  -w  keep watching, once a second, until Ctrl-C\n"
	   "  -c  the capacity of the cell fitted; a new cell's\n"
	   "      resistance is learned again from scratch\n"
	   "The current is estimated from the backlight, CPU and\n"
	   "radio: the board cannot measure it.")
{
	bool watch = false;
	int mah = 0, ret;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-w")) {
			watch = true;
		} else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			mah = atoi(argv[++i]);
		} else {
			pt_dprintf(PT_STDERR, "usage: battery [-w] [-c mAh]\n");
			return 2;
		}
	}
	if (battery_millivolts() == -ENODEV) {
		pt_dprintf(PT_STDERR, "battery: this board has no battery sensing\n");
		return 1;
	}
	if (mah && (ret = battery_set_capacity(mah))) {
		pt_dprintf(PT_STDERR, "battery: the capacity is 50 to 20000 mAh\n");
		return 2;
	}
	do {
		struct battery_status b;
		char left[40] = "";

		if ((ret = battery_status(&b)))
			return fail("battery", NULL, ret);
		if (b.minutes_left > 0)
			snprintf(left, sizeof(left), ", about %dh %02dm left",
				 b.minutes_left / 60, b.minutes_left % 60);
		else if (b.minutes_full > 0)
			snprintf(left, sizeof(left), ", full in about %dh %02dm",
				 b.minutes_full / 60, b.minutes_full % 60);
		if (b.state == BATTERY_NONE || b.state == BATTERY_USB)
			pt_printf("no battery fitted%s", watch ? "   \r" : "\n");
		else
			pt_printf("%d.%02d V  %d%%  %s%s%s", b.mv / 1000, b.mv % 1000 / 10,
				  b.percent, battery_state_name(b.state), left,
				  watch ? "      \r" : "\n");
		if (watch)
			pt_sleep_ms(1000);
	} while (watch && !pt_interrupted());
	if (watch)
		pt_puts("\n");
	return 0;
}

PT_PROGRAM(suspend, "deep sleep until the side button is pressed\n"
	   "usage: suspend [-t seconds]\n"
	   "The side button wakes it, and so does -t after that many seconds.\n"
	   "Waking is a fresh boot: running programs and anything not saved\n"
	   "are gone.")
{
	uint32_t seconds = 0;

	if (argc == 3 && !strcmp(argv[1], "-t"))
		seconds = strtoul(argv[2], NULL, 10);
	else if (argc != 1) {
		pt_dprintf(PT_STDERR, "usage: suspend [-t seconds]\n");
		return 2;
	}
	pt_puts("suspending...\n");
	pt_sleep_ms(100);
	int err = power_suspend(seconds);
	return fail("suspend", NULL, err);
}

PT_PROGRAM(poweroff, "put the system into deep sleep\nPress the board's reset button to wake it.")
{
	pt_puts("powering off, press reset to wake\n");
	power_quiesce();
	pt_sleep_ms(200);
	esp_deep_sleep_start();
	return 0;
}

/* ------------------------------------------------------------ CPU */

PT_PROGRAM(cpufreq, "show or set the CPU frequency policy\n"
	   "usage: cpufreq [performance | ondemand | powersave | MIN-MAX]\n"
	   "  performance  always the fastest (240 MHz on the S3)\n"
	   "  ondemand     fastest while busy, slowest while idle\n"
	   "  powersave    always the slowest (80 MHz on the S3)\n"
	   "  80-160       any range of the chip's three speeds\n"
	   "It always says the fastest now: running this\n"
	   "command is what does that. The line below it is\n"
	   "where the time really went.")
{
	int min, max, slow, middle, fast;

	cpufreq_speeds(&slow, &middle, &fast);
	if (argc == 2) {
		if (!strcmp(argv[1], "performance"))
			min = max = fast;
		else if (!strcmp(argv[1], "powersave"))
			min = max = slow;
		else if (!strcmp(argv[1], "ondemand"))
			min = slow, max = fast;
		else if (sscanf(argv[1], "%d-%d", &min, &max) != 2 &&
			 !(sscanf(argv[1], "%d", &min) == 1 && (max = min)))
			min = max = -1;
		int err = cpufreq_set(min, max);
		if (err)
			return fail("cpufreq", argv[1], err);
	} else if (argc > 2) {
		pt_dprintf(PT_STDERR, "usage: cpufreq [performance | ondemand | powersave | MIN-MAX]\n");
		return 2;
	}
	cpufreq_get(&min, &max);
	pt_printf("policy %s, %d-%d MHz, now %d MHz\n", cpufreq_policy_name(min, max), min, max,
		  cpufreq_current_mhz());
	/*
	 * "now" is always the top of the range: asking costs a running
	 * program. Where the time went is the honest answer.
	 */
	char since[96];
	if (cpufreq_time_summary(since, sizeof(since)) > 0)
		pt_printf("since boot %s\n", since);
	return 0;
}

/* ------------------------------------------------------------ status LED */

PT_PROGRAM(led, "control the status LED\n"
	   "usage: led [on | off | heartbeat | RRGGBB]\n"
	   "With no argument, shows the current setting.")
{
	static const char *const names[] = { "off", "on", "heartbeat" };
	uint8_t r, g, b;

	if (argc == 1) {
		led_get_color(&r, &g, &b);
		pt_printf("%s, color %02x%02x%02x\n", names[led_get_mode()], r, g, b);
		return 0;
	}
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!strcmp(argv[1], names[i])) {
			led_set_mode((enum led_mode)i);
			return 0;
		}
	}
	const char *hex = argv[1][0] == '#' ? argv[1] + 1 : argv[1];
	char *end;
	unsigned long rgb = strtoul(hex, &end, 16);
	if (strlen(hex) != 6 || *end) {
		pt_dprintf(PT_STDERR, "usage: led [on | off | heartbeat | RRGGBB]\n");
		return 2;
	}
	led_set_color(rgb >> 16, rgb >> 8, rgb);
	return 0;
}

/* ------------------------------------------------------------ hardware checks */

PT_PROGRAM(lcdtest, "draw test patterns to check display wiring\n"
	   "Shows 8 color bars, then a border. Wrong colors or garbage mean a\n"
	   "data line is swapped or loose; see docs/WIRING.md.")
{
	static const uint16_t bars[8] = {
		0xf800, 0x07e0, 0x001f, 0xffe0, 0xf81f, 0x07ff, 0xffff, 0x0000,
	};
	static const char *const names = "red green blue yellow magenta cyan white black";

	if (!vt_has_display()) {
		pt_dprintf(PT_STDERR, "lcdtest: display disabled in menuconfig\n");
		return 1;
	}
	int w = lcd_width(), h = lcd_height();
	for (int i = 0; i < 8; i++)
		lcd_fill(i * w / 8, 0, w / 8 + (i == 7 ? w % 8 : 0), h, bars[i]);
	pt_printf("bars left to right: %s\npress a key...\n", names);
	pt_tty_raw(PT_STDERR, true);
	pt_readkey(PT_STDERR);

	lcd_fill(0, 0, w, h, 0x0000);
	lcd_fill(0, 0, w, 2, 0xffff);
	lcd_fill(0, h - 2, w, 2, 0xffff);
	lcd_fill(0, 0, 2, h, 0xffff);
	lcd_fill(w - 2, 0, 2, h, 0xffff);
	lcd_fill(w / 2 - 20, h / 2 - 20, 40, 40, 0xf800);
	pt_printf("white border and a red square in the middle\npress a key...\n");
	pt_readkey(PT_STDERR);
	pt_tty_raw(PT_STDERR, false);
	vt_redraw();
	return 0;
}

#if CONFIG_PT_LCD_ILI9341_I80

/*
 * One register read twice, with the data pins pulled up and then down. Bits
 * that differ are lines nothing drives: printed as ? in that nibble. The
 * first byte of every read is a dummy the controller does not drive. Returns
 * how many of the real bits were driven; the bytes, pulled down, go to out.
 */
static int probe_read(const char *what, const uint8_t *index, int nindex, int n, uint8_t *out)
{
	uint8_t up[8], down[8];
	int driven = 0;

	lcd_probe_read(index, nindex, up, n, 1);
	lcd_probe_read(index, nindex, down, n, 0);
	pt_printf("  %-18s", what);
	for (int i = 0; i < n; i++) {
		uint8_t floating = up[i] ^ down[i];
		pt_printf(" %c%c", floating & 0xf0 ? '?' : "0123456789abcdef"[down[i] >> 4],
			  floating & 0x0f ? '?' : "0123456789abcdef"[down[i] & 15]);
		if (i)
			driven += 8 - __builtin_popcount(floating);
		if (out)
			out[i] = down[i];
	}
	pt_puts("\n");
	return driven;
}

PT_PROGRAM(lcdprobe, "check the display wiring slowly, without the display bus\n"
	   "usage: lcdprobe [RD-gpio]\n"
	   "Drives every display pin by hand and fills the screen red, green,\n"
	   "blue, white, black, then red again, which stays. With the shield's RD\n"
	   "(A0) moved from 3V3 to a free GPIO it also reads the controller's ID,\n"
	   "its power state and a pixel back (only on a shield whose chips run on\n"
	   "3.3 V). Reboot afterwards.")
{
	static const struct {
		const char *name;
		uint16_t    color;
	} steps[] = {
		{ "red", 0xf800 }, { "green", 0x07e0 }, { "blue", 0x001f },
		{ "white", 0xffff }, { "black", 0x0000 }, { "red again (it stays)", 0xf800 },
	};
	int rd = argc > 1 ? atoi(argv[1]) : CONFIG_PT_LCD_RD;
	uint8_t v[8];

	if (!vt_has_display()) {
		pt_dprintf(PT_STDERR, "lcdprobe: display disabled in menuconfig\n");
		return 1;
	}
	if (lcd_probe_begin(rd, true))
		return fail("lcdprobe", argv[1], -EINVAL);
	if (rd >= 0) {
		static const uint8_t d3[] = { 0xd3 }, id04[] = { 0x04 }, bf[] = { 0xbf },
				     ef[] = { 0xef }, reg0[] = { 0x00, 0x00 };
		int driven = 0, total = (3 + 3 + 5 + 5 + 1) * 8;
		pt_printf("controller ID reads (? = nothing drives that line;\n"
			  "the first byte of each is a dummy):\n");
		driven += probe_read("0xD3 (ILI9341)", d3, 1, 4, v);
		bool ili9341 = v[1] == 0x00 && v[2] == 0x93 && v[3] == 0x41;
		driven += probe_read("0x04 (ST7789...)", id04, 1, 4, NULL);
		driven += probe_read("0xBF (ILI948x)", bf, 1, 6, NULL);
		driven += probe_read("0xEF (ILI9327)", ef, 1, 6, NULL);
		driven += probe_read("reg 0 (ILI932x)", reg0, 2, 2, NULL);
		if (ili9341)
			pt_printf("-> an ILI9341, and every data line answers\n");
		else if (driven == 0)
			pt_printf("-> nothing answered: RD, CS, RS or the shield's power\n");
		else if (driven < total)
			pt_printf("-> %d of %d bits driven: the ? columns are data lines\n"
				  "   that are not connected\n", driven, total);
	}

	lcd_probe_init();
	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]) && !pt_interrupted(); i++) {
		pt_printf("filling %s...\n", steps[i].name);
		lcd_probe_fill(steps[i].color);
		pt_sleep_ms(700);
	}

	if (rd >= 0) {
		static const uint8_t power[] = { 0x0a }, format[] = { 0x0c }, pixel[] = { 0x2e };
		pt_printf("state after drawing:\n");
		probe_read("0x0A power mode", power, 1, 2, v);
		pt_printf("    %s, %s, %s, display %s\n", v[1] & 0x80 ? "booster on" : "BOOSTER OFF",
			  v[1] & 0x10 ? "awake" : "ASLEEP", v[1] & 0x08 ? "normal mode" : "NOT NORMAL MODE",
			  v[1] & 0x04 ? "on" : "OFF");
		probe_read("0x0C pixel format", format, 1, 2, NULL);
		probe_read("0x2E first pixel", pixel, 1, 4, v);
		pt_printf("    memory holds %s (red reads fc 00 00)\n",
			  v[1] >= 0xf0 && v[2] < 0x10 && v[3] < 0x10 ? "red, as drawn" : "SOMETHING ELSE");
	}
	pt_printf("done: the screen should be red now; type reboot to give it back\n"
		  "to the console\n");
	return 0;
}

PT_PROGRAM(lcdreg, "send commands to the display and read registers, by hand\n"
	   "usage: lcdreg RD-gpio STEP...\n"
	   "  reset          pulse the display's RESET line\n"
	   "  w CMD [ARG..]  write a command and its parameters (2-digit hex)\n"
	   "  r CMD N        read N bytes after CMD (the first is a dummy)\n"
	   "  d MS           wait MS milliseconds\n"
	   "  fill RRGGBB    fill the screen with a color\n"
	   "  init           the ILI9341 set-up the driver uses\n"
	   "  b CMD [ARG..]  send through the normal display bus instead: only\n"
	   "                 before any other step takes the pins\n"
	   "  edges GPIO N   send N bytes through the bus, count rising edges\n"
	   "                 on GPIO (a bus step too)\n"
	   "example: lcdreg 10 reset w 01 d 150 w 11 d 120 r 0a 2\n"
	   "Takes the display pins until the next reboot.")
{
	if (argc < 3) {
		pt_dprintf(PT_STDERR, "usage: lcdreg RD-gpio STEP... (help lcdreg)\n");
		return 2;
	}
	bool taken = false;		/* the pins are ours, not the bus's */
	int rd = atoi(argv[1]);

	/* RD must stay high while the bus writes, or the shield turns its data
	 * buffer around and fights the bus */
	if (rd >= 0 && GPIO_IS_VALID_OUTPUT_GPIO(rd)) {
		gpio_reset_pin(rd);
		gpio_set_direction(rd, GPIO_MODE_OUTPUT);
		gpio_set_level(rd, 1);
	}
	for (int i = 2; i < argc && !pt_interrupted();) {
		const char *op = argv[i++];
		if (!strcmp(op, "edges") && i + 1 < argc) {
			if (taken) {
				pt_dprintf(PT_STDERR, "lcdreg: edges needs the bus: put it first\n");
				return 2;
			}
			int gpio = atoi(argv[i++]), n = atoi(argv[i++]);
			pt_printf("edges on GPIO%d while sending %d bytes: %d\n", gpio, n,
				  lcd_bus_count_edges(gpio, n));
			continue;
		}
		if (!strcmp(op, "b") && i < argc) {
			if (taken) {
				pt_dprintf(PT_STDERR, "lcdreg: b needs the bus: put it first\n");
				return 2;
			}
			uint8_t cmd = strtoul(argv[i++], NULL, 16), args[32];
			int n = 0;
			while (i < argc && n < 32 && strlen(argv[i]) == 2 &&
			       isxdigit((unsigned char)argv[i][0]) && isxdigit((unsigned char)argv[i][1]))
				args[n++] = strtoul(argv[i++], NULL, 16);
			int err = lcd_bus_command(cmd, args, n);
			pt_printf("b %02x +%d%s\n", cmd, n, err ? " failed" : "");
			continue;
		}
		if (!strcmp(op, "d") && i < argc) {
			pt_sleep_ms(atoi(argv[i++]));	/* either way: keeps the bus */
			continue;
		}
		if (!taken) {
			if (lcd_probe_begin(atoi(argv[1]), false))
				return fail("lcdreg", argv[1], -EINVAL);
			taken = true;
		}
		if (!strcmp(op, "reset")) {
			lcd_probe_begin(atoi(argv[1]), true);
			pt_printf("reset\n");
		} else if (!strcmp(op, "init")) {
			lcd_probe_init();
			pt_printf("init sent\n");
		} else if (!strcmp(op, "d") && i < argc) {
			pt_sleep_ms(atoi(argv[i++]));
		} else if (!strcmp(op, "fill") && i < argc) {
			unsigned long rgb = strtoul(argv[i++], NULL, 16);
			uint16_t c = (rgb >> 8 & 0xf800) | (rgb >> 5 & 0x07e0) | (rgb >> 3 & 0x001f);
			lcd_probe_fill(c);
			pt_printf("filled %06lx\n", rgb);
		} else if (!strcmp(op, "w") && i < argc) {
			uint8_t cmd = strtoul(argv[i++], NULL, 16), args[32];
			int n = 0;
			/* parameters are two hex digits: "d" or "fill" start the next step */
			while (i < argc && n < 32 && strlen(argv[i]) == 2 &&
			       isxdigit((unsigned char)argv[i][0]) && isxdigit((unsigned char)argv[i][1]))
				args[n++] = strtoul(argv[i++], NULL, 16);
			lcd_probe_command(cmd, args, n);
			pt_printf("w %02x +%d\n", cmd, n);
		} else if (!strcmp(op, "r") && i + 1 < argc) {
			uint8_t cmd = strtoul(argv[i++], NULL, 16), v[8];
			int n = atoi(argv[i++]);
			if (n < 1 || n > 8)
				n = 2;
			char name[16];
			snprintf(name, sizeof(name), "r %02x", cmd);
			probe_read(name, &cmd, 1, n, v);
		} else {
			pt_dprintf(PT_STDERR, "lcdreg: bad step %s\n", op);
			return 2;
		}
	}
	return 0;
}

#endif /* CONFIG_PT_LCD_ILI9341_I80 */

#if CONFIG_PT_LCD_BACKLIGHT >= 0
PT_PROGRAM(backlight, "show or set the screen brightness\n"
	   "usage: backlight [0-100]\n"
	   "0 turns the backlight off; the screen keeps its contents.")
{
	char *end;
	long percent = argc > 1 ? strtol(argv[1], &end, 10) : -1;

	if (argc > 2 || (argc > 1 && (*end || percent < 0 || percent > 100))) {
		pt_dprintf(PT_STDERR, "usage: backlight [0-100]\n");
		return 2;
	}
	if (argc > 1)
		lcd_backlight_set(percent);
	pt_printf("backlight %d%%\n", lcd_backlight_get());
	return 0;
}
#endif

#if CONFIG_PT_LCD
/*
 * The two ways up a landscape screen has. The choice is kept in
 * /etc/rotate, "0" or "180", and init applies it at boot.
 */
PT_PROGRAM(rotate, "turn the screen upside down\n"
	   "usage: rotate [0 | 180]\n"
	   "Alone it turns the screen round; 0 is the usual way up and 180\n"
	   "the other. It stays that way after a restart.")
{
	int base = CONFIG_PT_LCD_ROTATION, now = lcd_rotation(), want, fd, err;

	if (argc > 2 || (argc == 2 && strcmp(argv[1], "0") && strcmp(argv[1], "180"))) {
		pt_dprintf(PT_STDERR, "usage: rotate [0 | 180]\n");
		return 2;
	}
	if (!vt_has_display()) {
		pt_dprintf(PT_STDERR, "rotate: there is no screen\n");
		return 1;
	}
	want = argc == 1 ? now ^ 2 : !strcmp(argv[1], "180") ? base ^ 2 : base;
	if ((err = lcd_set_rotation(want)))
		return fail("rotate", NULL, err);
	vt_redraw();
	if ((fd = pt_open("/etc/rotate", O_WRONLY | O_CREAT | O_TRUNC)) >= 0) {
		pt_dprintf(fd, "%s\n", want == base ? "0" : "180");
		pt_close(fd);
	}
	pt_printf("screen at %s°\n", want == base ? "0" : "180");
	return 0;
}
#endif

PT_PROGRAM(mkfs, "make a new filesystem on the SD card\n"
	   "usage: mkfs -y /mnt/sd\n"
	   "Everything on the card is lost. -y says you mean it.")
{
	uint32_t flags;
	int i = parse_flags("mkfs", argc, argv, "y", &flags);
	int ret;

	if (i < 0 || i + 1 != argc || strcmp(argv[i], "/mnt/sd")) {
		pt_dprintf(PT_STDERR, "usage: mkfs -y /mnt/sd\n");
		return 2;
	}
	if (!FLAG(flags, 'y')) {
		char desc[80];

		sd_describe(desc, sizeof(desc));
		pt_printf("this erases the card (%s)\n", desc);
		pt_printf("run `mkfs -y /mnt/sd` if you mean it\n");
		return 1;
	}
	pt_printf("formatting, this takes a moment...\n");
	if ((ret = sd_format()))
		return fail("mkfs", "/mnt/sd", ret);
	pt_printf("done; /mnt/sd is empty\n");
	return 0;
}

/*
 * Back to how it left the bench. The Wi-Fi settings are read into memory
 * first and written back afterwards, because a machine that forgets its
 * network is a machine you cannot reach to tell it anything -- everything
 * else on both filesystems goes.
 */
PT_PROGRAM_NAMED(factory_reset, "factory-reset", 0,
	   "erase both filesystems and start again\n"
	   "usage: factory-reset -y\n"
	   "The card is reformatted and the flash is wiped; /etc/wifi is kept.\n"
	   "The machine reboots when it is done.")
{
	uint32_t flags;
	int i = parse_flags("factory-reset", argc, argv, "y", &flags);
	char wifi[512];
	int n = 0, fd, ret;

	if (i < 0 || i != argc) {
		pt_dprintf(PT_STDERR, "usage: factory-reset -y\n");
		return 2;
	}
	if (!FLAG(flags, 'y')) {
		pt_printf("this erases the card and the flash, keeping only your\n"
			  "Wi-Fi settings. Run `factory-reset -y` if you mean it.\n");
		return 1;
	}

	if ((fd = pt_open("/etc/wifi", O_RDONLY)) >= 0) {
		n = pt_read(fd, wifi, sizeof(wifi) - 1);
		pt_close(fd);
		if (n < 0)
			n = 0;
	}

	if (sd_mounted()) {
		pt_printf("formatting the card...\n");
		if ((ret = sd_format()))
			return fail("factory-reset", "/mnt/sd", ret);
	}
	pt_printf("wiping the flash...\n");
	if ((ret = rootfs_format()))
		return fail("factory-reset", "/", ret);

	/* the format took /etc with it; the rest of the layout is made at boot */
	if (n > 0) {
		pt_mkdir("/etc");
		fd = pt_open("/etc/wifi", O_WRONLY | O_CREAT | O_TRUNC);
		ret = fd < 0 ? fd : write_all(fd, wifi, n);
		if (fd >= 0 && !ret)
			ret = pt_close(fd);
		else if (fd >= 0)
			pt_close(fd);
		if (ret)
			fail("factory-reset", "could not put back /etc/wifi", ret);
		else
			pt_printf("kept %d bytes of Wi-Fi settings\n", n);
	}
	pt_printf("done; rebooting\n");
	vfs_sync_all();
	pt_sleep_ms(300);
	esp_restart();
	return 0;
}

/*
 * A screenshot costs nothing until it is asked for: the panel is normally
 * the only copy of the picture, so this arms a capture, makes the screen
 * draw itself again, and writes what was drawn.
 */
PT_PROGRAM(screenshot, "save a picture of the screen\n"
	   "usage: screenshot [file.bmp]\n"
	   "Default is the next free ~/photos/shot-N.bmp, or /tmp when there\n"
	   "is no card.")
{
	char path[PT_PATH_MAX];
	struct pt_stat st;
	int ret;

	if (argc > 2) {
		pt_dprintf(PT_STDERR, "usage: screenshot [file.bmp]\n");
		return 2;
	}
	if (!vt_has_display()) {
		pt_dprintf(PT_STDERR, "screenshot: there is no screen\n");
		return 1;
	}
	if (argc == 2) {
		strlcpy(path, argv[1], sizeof(path));
	} else {
		const char *dir = sd_mounted() ? "/home/" CONFIG_PT_USERNAME "/photos" : "/tmp";
		int n = 1;

		if (sd_mounted())
			pt_mkdir(dir);
		do {
			snprintf(path, sizeof(path), "%s/shot-%d.bmp", dir, n++);
		} while (n < 1000 && !pt_stat(path, &st));
	}

	if ((ret = lcd_capture_begin()))
		return fail("screenshot", NULL, ret);
	vt_redraw();			/* paint the console into the capture */
	pt_sleep_ms(1300);		/* long enough for the status line's tick */
	ret = lcd_capture_save(path);
	lcd_capture_end();
	if (ret)
		return fail("screenshot", path, ret);
	pt_stat(path, &st);
	pt_printf("%s, %llu KB\n", path, st.size / 1024);
	return 0;
}

/*
 * The board's I2C bus is shared, and every bring-up question about it ("is
 * the keyboard seen? is the codec there?") is the same question, so: the
 * Linux tool, with the addresses this board can have spelled out.
 */
PT_PROGRAM(i2cdetect, "list the devices on an I2C bus\n"
	   "usage: i2cdetect [sda scl]\n"
	   "Without pins, scans the board's own bus.")
{
	static const struct { int addr; const char *what; } known[] = {
		{ 0x15, "touch controller (CST816)" },
		{ 0x18, "ES8311 audio codec" },
		{ 0x38, "touch controller (FT6x36)" },
		{ 0x3c, "OLED display" },
		{ 0x5f, "CardKB keyboard" },
		{ 0x68, "clock or motion sensor" },
	};
#if CONFIG_PT_AUDIO
	int sda = CONFIG_PT_AUDIO_I2C_SDA, scl = CONFIG_PT_AUDIO_I2C_SCL;
#elif CONFIG_PT_KBD_CARDKB
	int sda = CONFIG_PT_CARDKB_SDA, scl = CONFIG_PT_CARDKB_SCL;
#else
	int sda = -1, scl = -1;
#endif
	int found = 0;

	if (argc == 3) {
		sda = atoi(argv[1]);
		scl = atoi(argv[2]);
	} else if (argc != 1 || sda < 0) {
		pt_dprintf(PT_STDERR, "usage: i2cdetect [sda scl]\n");
		return 2;
	}
	pt_printf("scanning SDA %d / SCL %d\n", sda, scl);
	for (int addr = 0x08; addr <= 0x77 && !pt_interrupted(); addr++) {
		const char *what = "";
		int ret = 0;

		/* A bus with nothing on it but the chip's weak pull-ups rises
		 * slowly enough to fake an acknowledgement now and then, so
		 * an address only counts if it answers every time. */
		for (int try = 0; try < 3 && !ret; try++)
			ret = i2c_bus_probe(sda, scl, addr);
		if (ret == -EIO || ret == -ENOSPC)
			return fail("i2cdetect", "bus", ret);
		if (ret)
			continue;
		for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
			if (known[i].addr == addr)
				what = known[i].what;
		pt_printf("0x%02x  %s\n", addr, what);
		found++;
	}
	pt_printf("%d device%s\n", found, found == 1 ? "" : "s");
	if (found > 8)
		pt_printf("that many is usually a bus with no pull-up resistors\n");
	return 0;
}

PT_PROGRAM(chvt, "switch to another terminal\n"
	   "usage: chvt [number]\n"
	   "Each terminal keeps its own screen and its own shell. Ctrl-A\n"
	   "and a digit does the same thing without leaving what you are in.")
{
	int n;

	if (argc == 1) {
		pt_printf("terminal %d of %d\n", tty_front() + 1, vt_count());
		return 0;
	}
	if (argc != 2) {
		pt_dprintf(PT_STDERR, "usage: chvt [number]\n");
		return 2;
	}
	n = atoi(argv[1]);
	if (n < 1 || n > vt_count()) {
		pt_dprintf(PT_STDERR, "chvt: there are %d terminals\n", vt_count());
		return 1;
	}
	return tty_switch(n - 1) ? 1 : 0;
}

PT_PROGRAM(keytest, "show what each key sends\nPress q three times in a row to quit.")
{
	int quits = 0;

	pt_printf("press keys; q q q quits\n");
	pt_tty_raw(PT_STDIN, true);
	while (quits < 3) {
		unsigned char c;
		if (pt_read(PT_STDIN, &c, 1) != 1)
			break;
		quits = c == 'q' ? quits + 1 : 0;
		if (c >= 0x20 && c < 0x7f)
			pt_printf("0x%02x  '%c'\n", c, c);
		else
			pt_printf("0x%02x  %s\n", c, c == 0x1b ? "ESC" : c < 0x20 ? "control" : "UTF-8 byte");
	}
	pt_tty_raw(PT_STDIN, false);
	return 0;
}
