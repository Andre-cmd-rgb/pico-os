/*
 * alarm - alarms and timers.
 *
 * The ringing is done by the system (drivers/misc/alarm.c), which keeps
 * the list in /etc/alarms and rings whatever is running; this command
 * only reads what people type and says what is set:
 *
 *	alarm 7:00                  the next 7:00, once
 *	alarm 7:00 mo-fr wake up    weekdays, with a label
 *	alarm 6:30pm tomorrow bus   a day, then it goes
 *	alarm 25m tea               a timer
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dates.h"
#include "drivers/drivers.h"
#include "util.h"

static void label_from(char **argv, int from, int argc, char *out, size_t size)
{
	size_t n = 0;

	out[0] = '\0';
	for (int i = from; i < argc && n + 1 < size; i++)
		n += snprintf(out + n, size - n, "%s%s", n ? " " : "", argv[i]);
}

/* What an alarm repeats on, short enough for the list. */
static void repeat_text(const struct alarm *a, char *out, size_t size)
{
	if (a->date && a->snoozes)
		snprintf(out, size, "snoozed");
	else if (a->date)
		snprintf(out, size, "%d %.3s", a->date % 100, month_name[a->date / 100 % 100 - 1]);
	else if (!a->days)
		snprintf(out, size, "once");
	else
		weekdays_text(a->days, out, size);
}

static void describe(const struct alarm *a, int number, time_t now)
{
	char repeat[24], when[32];
	time_t next = alarm_next(a, now);

	repeat_text(a, repeat, sizeof(repeat));
	if (next)
		when_text(next, now, when, sizeof(when));
	else
		snprintf(when, sizeof(when), a->on ? "never" : "off");
	pt_printf("%2d  %02d:%02d  %-8s %-17.17s %s\n", number, a->hour, a->min, repeat,
		  a->label[0] ? a->label : a->kind == ALARM_CHIME ? "reminder" : "-", when);
}

static int list(void)
{
	struct alarm a[ALARMS_MAX], ringing;
	time_t now = time(NULL), next;
	char when[32];
	int n = alarm_list(a, ALARMS_MAX);

	if (n < 0)
		return fail("alarm", NULL, n);
	if (alarm_ringing(&ringing))
		pt_printf("ringing: %02d:%02d %s -- `alarm stop` or `alarm snooze`\n", ringing.hour,
			  ringing.min, ringing.label);
	if (!n) {
		pt_printf("no alarms; `alarm 7:00` sets one, `alarm 25m` a timer\n");
		return 0;
	}
	for (int i = 0; i < n; i++)
		describe(&a[i], i + 1, now);
	if ((next = alarm_next_any(NULL))) {
		when_text(next, now, when, sizeof(when));
		pt_printf("next: %s\n", when);
	}
	return 0;
}

static int number(const char *s, int *i)
{
	struct alarm a[ALARMS_MAX];
	int n = alarm_list(a, ALARMS_MAX);
	char *end;
	long v = strtol(s, &end, 10);

	if (*end || v < 1 || v > n) {
		pt_dprintf(PT_STDERR, "alarm: %s: no such alarm (`alarm` lists them)\n", s);
		return 1;
	}
	*i = (int)v - 1;
	return 0;
}

static int set(const struct alarm *a)
{
	int i = alarm_add(a);
	time_t now = time(NULL), next;

	if (i < 0)
		return fail("alarm", NULL, i == -ENOSPC ? -ENOSPC : i);
	next = alarm_next(a, now);
	if (!next) {
		pt_printf("alarm %d is set, but that time has gone\n", i + 1);
		return 0;
	}
	describe(a, i + 1, now);
	return 0;
}

PT_COMPLETE(alarm, ": off on rm stop snooze test\n")

PT_PROGRAM(alarm, "alarms and timers\n"
	   "usage: alarm                  what is set\n"
	   "       alarm TIME [DAYS] [label]\n"
	   "       alarm 25m [label]      a timer\n"
	   "       alarm off|on|rm N, alarm stop|snooze|test\n"
	   "TIME is 7:00, 19.30 or 7pm. DAYS, right after it,\n"
	   "repeats it: daily, weekdays, weekends, mo-fr,\n"
	   "mo,we,fr, fri; a date rings it once: tomorrow,\n"
	   "30/9. With neither it rings once, at the next TIME.\n"
	   "Any key snoozes it for 9 minutes, Esc stops it, and\n"
	   "`suspend` wakes up for it.")
{
	struct alarm a = { .on = true, .kind = ALARM_RING };
	const char *cmd = argc > 1 ? argv[1] : "";
	int h, m, secs, i, from = 2, today = day_today();

	if (argc == 1)
		return list();
	if (!strcmp(cmd, "stop") || !strcmp(cmd, "snooze")) {
		if (!alarm_ringing(NULL)) {
			pt_dprintf(PT_STDERR, "alarm: nothing is ringing\n");
			return 1;
		}
		alarm_answer(!strcmp(cmd, "stop"));
		return 0;
	}
	if (!strcmp(cmd, "test")) {
		i = alarm_test(argc > 2 && !strcmp(argv[2], "chime") ? ALARM_CHIME : ALARM_RING);
		if (i)
			return fail("alarm", NULL, i);
		pt_printf("ringing now: any key snoozes, Esc stops\n");
		return 0;
	}
	if ((!strcmp(cmd, "off") || !strcmp(cmd, "on") || !strcmp(cmd, "rm") ||
	     !strcmp(cmd, "del")) && argc == 3) {
		int err;

		if (number(argv[2], &i))
			return 1;
		err = cmd[0] == 'o' ? alarm_enable(i, cmd[1] == 'n') : alarm_remove(i);
		if (err)
			return fail("alarm", argv[2], err);
		return list();
	}
	if ((secs = duration_parse(cmd)) > 0) {
		time_t at = time(NULL) + secs;
		struct tm tm;

		localtime_r(&at, &tm);
		a.date = day_of(at);
		a.hour = tm.tm_hour;
		a.min = tm.tm_min;
		a.sec = tm.tm_sec;
		label_from(argv, 2, argc, a.label, sizeof(a.label));
		if (!a.label[0])
			strlcpy(a.label, "timer", sizeof(a.label));
		return set(&a);
	}
	if (!clock_parse(cmd, &h, &m)) {
		pt_dprintf(PT_STDERR, "alarm: %s: not a time (7:00, 19.30, 7pm) or a timer "
			   "(25m, 1h30m)\n", cmd);
		return 2;
	}
	a.hour = h;
	a.min = m;
	if (argc > 2) {
		int days = weekdays_parse(argv[2]), day;

		if (!strcmp(argv[2], "once")) {
			from = 3;
		} else if (days > 0 && !strchr(argv[2], '/') && !strchr(argv[2], '.')) {
			/* a name of a day repeats every week; a date is once */
			a.days = days;
			from = 3;
		} else if ((day = day_parse(argv[2], today))) {
			if (day_time(day, h, m, 0) <= time(NULL)) {
				pt_dprintf(PT_STDERR, "alarm: %s at %02d:%02d has already gone\n",
					   argv[2], h, m);
				return 1;
			}
			a.date = day;
			from = 3;
		}
	}
	label_from(argv, from, argc, a.label, sizeof(a.label));
	return set(&a);
}
