/*
 * cal, calendar - the month, and what is on.
 *
 * `cal` prints months the way util-linux's does, weeks from Monday unless
 * -s says Sunday, and a narrow screen gets two months to a row instead of
 * three. `calendar` is the diary: a month to move about in with the
 * arrows, and under it the day's events and what is due on it from
 * ~/todo.md. The events are in ~/calendar.txt, a line each, so they can
 * be typed on a PC as well:
 *
 *	2026-09-30 10:00 Verifica di storia !15
 *	2026-10-05 09:00-11:00 Laboratorio
 *	weekly mo,we 15:00-16:30 Calcio
 *	yearly 10-02 Compleanno di Marco !
 *
 * A !N asks for a reminder N minutes before (a lone ! at the time, or at
 * 9:00 for a day's event): the chime and the status bar, through the
 * alarms (drivers/misc/alarm.c). They are set whenever `calendar` runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dates.h"
#include "pda.h"
#include "util.h"

#ifdef ESP_PLATFORM
#include "drivers/drivers.h"
#endif

#define DIARY_LINE	256
#define REMIND_DAYS	60	/* one-off reminders are set this far ahead */
#define REMIND_MAX	12	/* and no more of them than this */
#define ALL_DAY_REMIND	(9 * 60)

enum { ONCE, WEEKLY, YEARLY };

struct event {
	int	line;
	int	kind;
	int	when;			/* ONCE: YYYYMMDD, WEEKLY: a set of days, YEARLY: MMDD */
	int	start, end;		/* minutes into the day; -1: all day, or no end */
	int	remind;			/* minutes before; -1: none */
	char	text[120];
};

struct diary {
	char		 path[PT_PATH_MAX];
	char		**lines;
	int		 nlines, cap;
	struct event	*ev;
	int		 n;
	bool		 no_reminders;	/* not ~/calendar.txt: leave the alarms be */
};

/* ------------------------------------------------------------ cal */

static const char *const heads[2] = { "Mo Tu We Th Fr Sa Su", "Su Mo Tu We Th Fr Sa" };

/* A month as util-linux lays it out: a title, the day names and six
 * weeks, each 20 wide. `today` is marked if it falls in it. */
struct block {
	char	line[8][21];
	int	mark_line, mark_col;
};

static void center(char *out, int width, const char *text)
{
	int len = (int)strlen(text), left;

	if (len > width)
		len = width;
	left = (width - len + 1) / 2;
	memset(out, ' ', width);
	memcpy(out + left, text, len);
	out[width] = '\0';
}

static void month_block(struct block *b, int y, int m, bool sunday, bool year_in_title, int today)
{
	char title[32];
	int first = day_weekday(day_make(y, m, 1)), col, row = 2;

	if (year_in_title)
		snprintf(title, sizeof(title), "%s %d", month_name[m - 1], y);
	else
		snprintf(title, sizeof(title), "%s", month_name[m - 1]);
	center(b->line[0], 20, title);
	strcpy(b->line[1], heads[sunday]);
	for (int r = 2; r < 8; r++)
		snprintf(b->line[r], 21, "%20s", "");
	b->mark_line = -1;
	col = sunday ? (first + 1) % 7 : first;
	for (int d = 1; d <= days_in_month(y, m); d++) {
		char num[12];

		snprintf(num, sizeof(num), "%2d", d);
		memcpy(b->line[row] + col * 3, num, 2);
		if (day_make(y, m, d) == today) {
			b->mark_line = row;
			b->mark_col = col * 3;
		}
		if (++col == 7) {
			col = 0;
			row++;
		}
	}
}

/* Blocks side by side, `gap` apart; every line is padded to the width. */
static void print_blocks(struct block *b, int n, int gap, bool color)
{
	for (int l = 0; l < 8; l++) {
		for (int i = 0; i < n; i++) {
			const char *s = b[i].line[l];

			if (i)
				pt_printf("%*s", gap, "");
			if (color && b[i].mark_line == l)
				pt_printf("%.*s\x1b[7m%.2s\x1b[0m%s", b[i].mark_col, s, s + b[i].mark_col,
					  s + b[i].mark_col + 2);
			else
				pt_printf("%s", s);
		}
		pt_puts("\n");
	}
}

PT_PROGRAM(cal, "print a calendar\n"
	   "usage: cal [-ms3y] [[month] year]\n"
	   "  -m  weeks from Monday: the default here, where\n"
	   "      util-linux starts on Sunday in the C locale\n"
	   "  -s  weeks from Sunday\n"
	   "  -3  last month, this one and the next\n"
	   "  -y  the whole year; so does a year alone\n"
	   "`calendar` is the diary, with events.")
{
	struct opt o = { .ind = 1 };
	int c, i, today = day_today();
	int y = today / 10000, m = today / 100 % 100, cols = 80, rows, per_row;
	bool sunday = false, three = false, color = pt_isatty(PT_STDOUT), year_view = false;
	struct block b[3];
	char *end = "";

	while ((c = getopt_pt(&o, "cal", argc, argv, "ms3y")) != -1) {
		if (c == 'm' || c == 's')
			sunday = c == 's';
		else if (c == '3')
			three = true;
		else if (c == 'y')
			year_view = true;
		else
			return 2;
	}
	i = o.ind;
	if (argc - i == 1) {
		y = strtol(argv[i], &end, 10);
		year_view = true;
	} else if (argc - i == 2) {
		m = strtol(argv[i], &end, 10);
		if (*end || m < 1 || m > 12) {
			pt_dprintf(PT_STDERR, "cal: %s: not a month (1-12)\n", argv[i]);
			return 1;
		}
		y = strtol(argv[i + 1], &end, 10);
	} else if (argc - i > 2) {
		pt_dprintf(PT_STDERR, "usage: cal [-ms3y] [[month] year]\n");
		return 2;
	}
	if ((argc - i >= 1 && *end) || y < 1 || y > 9999) {
		pt_dprintf(PT_STDERR, "cal: not a year (1-9999)\n");
		return 1;
	}
	if (color)
		pt_tty_size(PT_STDOUT, &cols, &rows);

	if (year_view) {
		char title[80], number[8];

		per_row = cols >= 66 ? 3 : 2;
		snprintf(number, sizeof(number), "%d", y);
		center(title, per_row * 20 + (per_row - 1) * 3, number);
		pt_printf("%s\n\n", title);
		for (int first = 1; first <= 12; first += per_row) {
			int n = first + per_row - 1 <= 12 ? per_row : 12 - first + 1;

			for (int k = 0; k < n; k++)
				month_block(&b[k], y, first + k, sunday, false, today);
			print_blocks(b, n, 3, color);
		}
		return 0;
	}
	if (three) {
		for (int k = 0; k < 3; k++) {
			int mm = m - 1 + k, yy = y;

			if (mm < 1)
				mm += 12, yy--;
			if (mm > 12)
				mm -= 12, yy++;
			month_block(&b[k], yy, mm, sunday, true, today);
		}
		if (cols >= 64) {
			print_blocks(b, 3, 2, color);
		} else {
			for (int k = 0; k < 3; k++)
				print_blocks(&b[k], 1, 0, color);
		}
		return 0;
	}
	month_block(&b[0], y, m, sunday, true, today);
	print_blocks(b, 1, 0, color);
	return 0;
}

/* ------------------------------------------------------------ the diary file */

static bool add_line(struct diary *d, const char *text)
{
	char *copy = pt_strdup(text);

	if (!copy)
		return false;
	if (d->nlines == d->cap) {
		int cap = d->cap ? d->cap * 2 : 32;
		char **grown = pt_realloc(d->lines, cap * sizeof(*grown));

		if (!grown) {
			pt_free(copy);
			return false;
		}
		d->lines = grown;
		d->cap = cap;
	}
	d->lines[d->nlines++] = copy;
	return true;
}

/* "10:00", "10:00-11:30": minutes into the day. */
static bool span_parse(const char *s, int *start, int *end)
{
	char a[16];
	const char *dash = strchr(s, '-');
	int h, m;

	*end = -1;
	if (dash) {
		if (dash - s >= (int)sizeof(a))
			return false;
		memcpy(a, s, dash - s);
		a[dash - s] = '\0';
		if (!clock_parse(a, &h, &m))
			return false;
		*start = h * 60 + m;
		if (!clock_parse(dash + 1, &h, &m))
			return false;
		*end = h * 60 + m;
		return *end > *start;
	}
	if (!clock_parse(s, &h, &m))
		return false;
	*start = h * 60 + m;
	return true;
}

/*
 * The words after the day: an optional time, the text, and !N anywhere.
 * `words` is modified.
 */
static void rest_parse(char *words, struct event *e)
{
	char *save = NULL, *w;
	size_t len = 0;
	bool first = true;

	e->start = e->end = e->remind = -1;
	e->text[0] = '\0';
	for (w = strtok_r(words, " ", &save); w; w = strtok_r(NULL, " ", &save), first = false) {
		if (first && span_parse(w, &e->start, &e->end))
			continue;
		if (w[0] == '!' && (!w[1] || (atoi(w + 1) > 0 && atoi(w + 1) <= 7 * 24 * 60))) {
			e->remind = w[1] ? atoi(w + 1) : 0;
			continue;
		}
		len += snprintf(e->text + len, len < sizeof(e->text) ? sizeof(e->text) - len : 0,
				"%s%s", len ? " " : "", w);
	}
}

static bool event_parse(const char *line, struct event *e)
{
	char buf[DIARY_LINE], *sp;
	int y, m, d, n;

	strlcpy(buf, line, sizeof(buf));
	if (!buf[0] || buf[0] == '#')
		return false;
	sp = strchr(buf, ' ');
	if (sscanf(buf, "%d-%d-%d%n", &y, &m, &d, &n) == 3 && (buf[n] == ' ' || !buf[n])) {
		e->kind = ONCE;
		if (!(e->when = day_make(y, m, d)))
			return false;
		rest_parse(buf + n, e);
		return true;
	}
	if (!sp)
		return false;
	*sp = '\0';
	if (!strcmp(buf, "weekly") || !strcmp(buf, "yearly")) {
		char *arg = sp + 1, *after = strchr(arg, ' ');

		if (after)
			*after++ = '\0';
		if (buf[0] == 'w') {
			e->kind = WEEKLY;
			if ((e->when = weekdays_parse(arg)) < 0)
				return false;
		} else {
			e->kind = YEARLY;
			if (sscanf(arg, "%d-%d%n", &m, &d, &n) != 2 || arg[n] || !day_make(2000, m, d))
				return false;
			e->when = m * 100 + d;
		}
		rest_parse(after ? after : arg + strlen(arg), e);
		return true;
	}
	return false;
}

static void event_line(const struct event *e, char *out, size_t size)
{
	char when[32], time[32] = "", remind[16] = "";

	if (e->kind == ONCE)
		snprintf(when, sizeof(when), "%04d-%02d-%02d", e->when / 10000, e->when / 100 % 100,
			 e->when % 100);
	else if (e->kind == YEARLY)
		snprintf(when, sizeof(when), "yearly %02d-%02d", e->when / 100, e->when % 100);
	else {
		strcpy(when, "weekly ");
		weekdays_text(e->when, when + 7, sizeof(when) - 7);
	}
	if (e->start >= 0 && e->end >= 0)
		snprintf(time, sizeof(time), " %02d:%02d-%02d:%02d", e->start / 60, e->start % 60,
			 e->end / 60, e->end % 60);
	else if (e->start >= 0)
		snprintf(time, sizeof(time), " %02d:%02d", e->start / 60, e->start % 60);
	if (e->remind == 0)
		strcpy(remind, " !");
	else if (e->remind > 0)
		snprintf(remind, sizeof(remind), " !%d", e->remind);
	snprintf(out, size, "%s%s %s%s", when, time, e->text, remind);
}

static bool occurs(const struct event *e, int day)
{
	if (e->kind == ONCE)
		return e->when == day;
	if (e->kind == YEARLY)
		return e->when == day % 10000;
	return e->when & (1 << day_weekday(day));
}

/* A day's events, by time, the all-day ones first. */
static int by_time(const void *pa, const void *pb)
{
	const struct event *a = *(const struct event *const *)pa, *b = *(const struct event *const *)pb;

	if (a->start != b->start)
		return a->start < b->start ? -1 : 1;
	return a->line - b->line;
}

static int on_day(const struct diary *d, int day, const struct event **out, int max)
{
	int n = 0;

	for (int i = 0; i < d->n && n < max; i++)
		if (occurs(&d->ev[i], day))
			out[n++] = &d->ev[i];
	qsort(out, n, sizeof(*out), by_time);
	return n;
}

static int reindex(struct diary *d)
{
	struct event e;

	pt_free(d->ev);
	d->ev = pt_malloc((d->nlines + 1) * sizeof(*d->ev));
	d->n = 0;
	if (!d->ev)
		return -ENOMEM;
	for (int i = 0; i < d->nlines; i++)
		if (event_parse(d->lines[i], &e)) {
			e.line = i;
			d->ev[d->n++] = e;
		}
	return 0;
}

static void diary_free(struct diary *d)
{
	for (int i = 0; i < d->nlines; i++)
		pt_free(d->lines[i]);
	pt_free(d->lines);
	pt_free(d->ev);
	memset(d, 0, sizeof(*d));
}

static int load(struct diary *d, const char *path)
{
	struct lines ls;
	char *line, buf[DIARY_LINE];
	size_t n;
	int fd;

	memset(d, 0, sizeof(*d));
	strlcpy(d->path, path, sizeof(d->path));
	if ((fd = pt_open(path, O_RDONLY)) >= 0) {
		lines_init(&ls, fd);
		while ((line = lines_next(&ls, &n))) {
			while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
				n--;
			if (n >= sizeof(buf))
				n = sizeof(buf) - 1;
			memcpy(buf, line, n);
			buf[n] = '\0';
			if (!add_line(d, buf))
				break;
		}
		lines_free(&ls);
		pt_close(fd);
	} else if (fd != -ENOENT) {
		return fd;
	}
	return reindex(d);
}

static int save(struct diary *d)
{
	char tmp[PT_PATH_MAX + 8];
	int fd, err = 0;

	snprintf(tmp, sizeof(tmp), "%s.tmp", d->path);
	if ((fd = pt_open(tmp, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
		return fd;
	for (int i = 0; i < d->nlines && !err; i++) {
		err = write_all(fd, d->lines[i], strlen(d->lines[i]));
		if (!err)
			err = write_all(fd, "\n", 1);
	}
	if (pt_close(fd) && !err)
		err = -EIO;
	if (err) {
		pt_unlink(tmp);
		return err;
	}
	return pt_rename(tmp, d->path);
}

static int add_event(struct diary *d, const struct event *e)
{
	char line[DIARY_LINE];

	if (!d->nlines && (!add_line(d, "# date [time[-end]] what [!minutes before, for a reminder]") ||
			   !add_line(d, "# 2026-09-30 10:00 ...  weekly mo,we 15:00-16:30 ...  "
					"yearly 10-02 ...")))
		return -ENOMEM;
	event_line(e, line, sizeof(line));
	return add_line(d, line) ? reindex(d) : -ENOMEM;
}

static int remove_event(struct diary *d, const struct event *e)
{
	int at = e->line;

	pt_free(d->lines[at]);
	memmove(&d->lines[at], &d->lines[at + 1], (d->nlines - at - 1) * sizeof(*d->lines));
	d->nlines--;
	return reindex(d);
}

/* ------------------------------------------------------------ reminders */

#ifdef ESP_PLATFORM
/* The moment an event's reminder is due on `day`. */
static void remind_at(const struct event *e, int day, int *rday, int *minute)
{
	int at = (e->start >= 0 ? e->start : ALL_DAY_REMIND) - e->remind;

	*rday = day;
	while (at < 0) {
		at += 24 * 60;
		*rday = day_add(*rday, -1);
	}
	*minute = at;
}

/*
 * Make the chiming alarms match the diary: the calendar owns every alarm
 * of the chime kind. A weekly event reminds weekly; the others, the next
 * time they fall within REMIND_DAYS.
 */
static void sync_reminders(const struct diary *d, int today)
{
	struct alarm have[ALARMS_MAX], want[ALARMS_MAX];
	int nhave, nwant = 0;
	time_t now = time(NULL);

	if (d->no_reminders)
		return;
	nhave = alarm_list(have, ALARMS_MAX);

	for (int i = 0; i < d->n && nwant < REMIND_MAX; i++) {
		const struct event *e = &d->ev[i];
		struct alarm a = { .on = true, .kind = ALARM_CHIME };
		int rday, minute;

		if (e->remind < 0)
			continue;
		strlcpy(a.label, e->text, sizeof(a.label));
		if (e->kind == WEEKLY) {
			remind_at(e, today, &rday, &minute);
			a.days = e->when;
			if (rday != today)	/* the day before: the set moves back one */
				a.days = (e->when >> 1 | e->when << 6) & 0x7f;
		} else {
			int day = 0;

			for (int k = 0; k <= REMIND_DAYS && !day; k++)
				if (occurs(e, day_add(today, k))) {
					remind_at(e, day_add(today, k), &rday, &minute);
					if (day_time(rday, minute / 60, minute % 60, 0) > now)
						day = rday;
				}
			if (!day)
				continue;
			a.date = day;
		}
		a.hour = minute / 60;
		a.min = minute % 60;
		want[nwant++] = a;
	}
	/* what is there and not wanted goes; what is wanted and not there comes */
	for (int i = nhave - 1; i >= 0; i--) {
		bool keep = false;

		if (have[i].kind != ALARM_CHIME)
			continue;
		for (int k = 0; k < nwant && !keep; k++)
			if (have[i].date == want[k].date && have[i].days == want[k].days &&
			    have[i].hour == want[k].hour && have[i].min == want[k].min &&
			    !strcmp(have[i].label, want[k].label)) {
				keep = true;
				want[k].kind = 0xff;	/* already there */
			}
		if (!keep)
			alarm_remove(i);
	}
	for (int k = 0; k < nwant; k++)
		if (want[k].kind == ALARM_CHIME)
			alarm_add(&want[k]);
}
#else
static void sync_reminders(const struct diary *d, int today)
{
}
#endif

/* ------------------------------------------------------------ showing a day */

static void span_text(const struct event *e, char *out, size_t size)
{
	if (e->start < 0)
		snprintf(out, size, "all day");
	else if (e->end >= 0)
		snprintf(out, size, "%02d:%02d-%02d:%02d", e->start / 60, e->start % 60, e->end / 60,
			 e->end % 60);
	else
		snprintf(out, size, "%02d:%02d", e->start / 60, e->start % 60);
}

static int todos_on(const struct todo_item *t, int nt, int day, bool late_too, int today)
{
	int n = 0;

	for (int i = 0; i < nt; i++)
		if (!t[i].done && t[i].due &&
		    (t[i].due == day || (late_too && day == today && t[i].due < today)))
			n++;
	return n;
}

static int agenda(const struct diary *d, const struct todo_item *t, int nt, int today, int days)
{
	const struct event *list[64];
	char name[32], span[24];
	bool any = false;

	for (int k = 0; k < days; k++) {
		int day = day_add(today, k), n = on_day(d, day, list, 64);
		int due = todos_on(t, nt, day, true, today);

		if (!n && !due)
			continue;
		day_name(day, today, name, sizeof(name));
		if (k <= 1)
			pt_printf("%s %d %.3s, %s\n", weekday_short[day_weekday(day)], day % 100,
				  month_name[day / 100 % 100 - 1], name);
		else
			pt_printf("%s\n", name);
		for (int i = 0; i < n; i++) {
			span_text(list[i], span, sizeof(span));
			pt_printf("  %-12s %s%s\n", span, list[i]->text, list[i]->remind >= 0 ? " (!)" : "");
		}
		for (int i = 0; i < nt; i++)
			if (!t[i].done && t[i].due &&
			    (t[i].due == day || (day == today && t[i].due < today)))
				pt_printf("  %-12s %s\n", t[i].due < today ? "late, to do" : "to do",
					  t[i].text);
		any = true;
	}
	if (!any)
		pt_printf("nothing in the next %d day%s\n", days, days == 1 ? "" : "s");
	return 0;
}

/* ------------------------------------------------------------ the month screen */

struct screen {
	struct diary		*d;
	struct todo_item	*t;
	int			 nt, sel, today, rows, cols;
	char			 note[64];
};

static bool busy_day(const struct screen *s, int day)
{
	for (int i = 0; i < s->d->n; i++)
		if (occurs(&s->d->ev[i], day))
			return true;
	return todos_on(s->t, s->nt, day, false, s->today) > 0;
}

static void draw(struct screen *s)
{
	int y = s->sel / 10000, m = s->sel / 100 % 100, first = day_weekday(day_make(y, m, 1));
	int left = (s->cols - 35) / 2, row = 3, col = first, n, due;
	const struct event *list[32];
	char title[32], name[32], span[24];

	if (left < 0)
		left = 0;
	snprintf(title, sizeof(title), "%s %d", month_name[m - 1], y);
	pt_printf("\x1b[1;1H\x1b[0m\x1b[K\x1b[1;%dH\x1b[0;1m%s\x1b[0m", (s->cols - (int)strlen(title)) / 2 + 1,
		  title);
	pt_printf("\x1b[2;1H\x1b[K\x1b[2;%dH\x1b[0;2m", left + 1);
	for (int k = 0; k < 7; k++)
		pt_printf(" %.2s  ", weekday_short[k]);
	pt_puts("\x1b[0m");
	for (int r = 3; r <= 8; r++)
		pt_printf("\x1b[%d;1H\x1b[K", r);
	for (int d = 1; d <= days_in_month(y, m); d++) {
		int day = day_make(y, m, d);
		const char *look = day == s->sel ? "\x1b[0;7m" : day == s->today ? "\x1b[0;1;97m" :
				   col >= 5 ? "\x1b[0;33m" : "\x1b[0m";

		pt_printf("\x1b[%d;%dH%s%3d%s\x1b[0m", row, left + col * 5 + 1, look, d,
			  busy_day(s, day) ? "\xe2\x80\xa2" : " ");	/* • */
		if (++col == 7) {
			col = 0;
			row++;
		}
	}

	/* the day under the cursor */
	day_name(s->sel, s->today, name, sizeof(name));
	pt_printf("\x1b[10;1H\x1b[0;1m %s %d %s%s%s\x1b[0m\x1b[K", weekday_short[day_weekday(s->sel)],
		  s->sel % 100, month_name[m - 1], s->sel == s->today ? ", " : "",
		  s->sel == s->today ? "today" : "");
	n = on_day(s->d, s->sel, list, 32);
	row = 11;
	for (int i = 0; i < n && row < s->rows; i++, row++) {
		span_text(list[i], span, sizeof(span));
		pt_printf("\x1b[%d;1H\x1b[0m %2d \x1b[0;33m%-11s\x1b[0m %.*s%s\x1b[K", row, i + 1, span,
			  (int)utf8_prefix(list[i]->text, strlen(list[i]->text), s->cols - 20),
			  list[i]->text, list[i]->remind >= 0 ? " \x1b[0;2m(!)\x1b[0m" : "");
	}
	due = 0;
	for (int i = 0; i < s->nt && row < s->rows; i++)
		if (!s->t[i].done && s->t[i].due == s->sel) {
			pt_printf("\x1b[%d;1H\x1b[0m    \x1b[0;2m%-11s\x1b[0m %.*s\x1b[K", row, "to do",
				  (int)utf8_prefix(s->t[i].text, strlen(s->t[i].text), s->cols - 17),
				  s->t[i].text);
			row++;
			due++;
		}
	if (!n && !due && row < s->rows)
		pt_printf("\x1b[%d;1H\x1b[0;2m    nothing on\x1b[0m\x1b[K", row++);
	for (; row < s->rows; row++)
		pt_printf("\x1b[%d;1H\x1b[K", row);
	pt_printf("\x1b[%d;1H\x1b[0;7m%s\x1b[0m\x1b[K", s->rows,
		  s->note[0] ? s->note : " arrows day  </> month  t today  a add  d del  q");
	s->note[0] = '\0';
}

static int interactive(struct diary *d, int today, const char *todo_path)
{
	struct screen s = { .d = d, .sel = today, .today = today };
	char buf[DIARY_LINE];
	int err = 0;

	s.nt = todo_items(todo_path, &s.t);
	if (s.nt < 0)
		s.nt = 0;
	pt_tty_size(PT_STDOUT, &s.cols, &s.rows);
	if (s.rows < 14 || s.cols < 36)
		return -ENOTTY;
	pt_tty_raw(PT_STDIN, true);
	pt_puts("\x1b[?25l\x1b[2J");
	for (bool done = false; !done && !err;) {
		int y = s.sel / 10000, m = s.sel / 100 % 100, d0 = s.sel % 100;

		draw(&s);
		switch (pt_readkey(PT_STDIN)) {
		case PT_KEY_LEFT: case 'h':	s.sel = day_add(s.sel, -1); break;
		case PT_KEY_RIGHT: case 'l':	s.sel = day_add(s.sel, 1); break;
		case PT_KEY_UP: case 'k':	s.sel = day_add(s.sel, -7); break;
		case PT_KEY_DOWN: case 'j':	s.sel = day_add(s.sel, 7); break;
		case '<': case ',': case PT_KEY_PGUP:
			m = m == 1 ? 12 : m - 1;
			y -= m == 12;
			s.sel = day_make(y, m, d0 <= days_in_month(y, m) ? d0 : days_in_month(y, m));
			break;
		case '>': case '.': case PT_KEY_PGDN:
			m = m == 12 ? 1 : m + 1;
			y += m == 1;
			s.sel = day_make(y, m, d0 <= days_in_month(y, m) ? d0 : days_in_month(y, m));
			break;
		case 't': case PT_KEY_HOME:	s.sel = today; break;
		case 'a': {
			struct event e = { .kind = ONCE, .when = s.sel };

			buf[0] = '\0';
			if (!ask_line(s.rows, "[10:00[-11:00]] what [!15]: ", buf, sizeof(buf)))
				break;
			rest_parse(buf, &e);
			if (!e.text[0])
				break;
			if (!(err = add_event(d, &e)) && !(err = save(d)))
				sync_reminders(d, today);
			break;
		}
		case 'd': case PT_KEY_DELETE: {
			const struct event *list[32];
			int n = on_day(d, s.sel, list, 32), which = 0, key;

			if (!n) {
				strlcpy(s.note, " nothing to delete on this day", sizeof(s.note));
				break;
			}
			if (n > 1) {
				pt_printf("\x1b[%d;1H\x1b[0;7m delete which, 1-%d? \x1b[0m\x1b[K", s.rows, n);
				key = pt_readkey(PT_STDIN);
				which = key - '1';
				if (which < 0 || which >= n)
					break;
			}
			pt_printf("\x1b[%d;1H\x1b[0;7m delete \"%.30s\"%s? y/n \x1b[0m\x1b[K", s.rows,
				  list[which]->text, list[which]->kind == ONCE ? "" : ", every time");
			if (pt_readkey(PT_STDIN) != 'y')
				break;
			if (!(err = remove_event(d, list[which])) && !(err = save(d)))
				sync_reminders(d, today);
			break;
		}
		case 'q': case PT_KEY_ESC: case PT_CTRL('c'): case PT_KEY_EOF: case PT_KEY_ERROR:
			done = true;
			break;
		}
	}
	pt_free(s.t);
	pt_puts("\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
	pt_tty_raw(PT_STDIN, false);
	return err;
}

/* ------------------------------------------------------------ the command */

/* "calendar add 30/9 10:00 what", "every mo,we 15:00 ...", "yearly 2/10 ..." */
static int add_words(struct diary *d, char **argv, int argc, int today)
{
	struct event e = { .kind = ONCE };
	char rest[DIARY_LINE];
	int i = 0, day;
	size_t len = 0;

	if (argc < 2)
		goto usage;
	if (!strcmp(argv[0], "every") || !strcmp(argv[0], "weekly")) {
		e.kind = WEEKLY;
		if ((e.when = weekdays_parse(argv[1])) < 0)
			goto usage;
		i = 2;
	} else if (!strcmp(argv[0], "yearly")) {
		e.kind = YEARLY;
		if (!(day = day_parse(argv[1], today)))
			goto usage;
		e.when = day % 10000;
		i = 2;
	} else {
		if (!(e.when = day_parse(argv[0], today)))
			goto usage;
		i = 1;
	}
	rest[0] = '\0';
	for (; i < argc; i++)
		len += snprintf(rest + len, len < sizeof(rest) ? sizeof(rest) - len : 0, "%s%s",
				len ? " " : "", argv[i]);
	rest_parse(rest, &e);
	if (!e.text[0])
		goto usage;
	return add_event(d, &e);
usage:
	pt_dprintf(PT_STDERR, "usage: calendar add DAY|every DAYS|yearly DAY [TIME[-END]] what [!N]\n"
		   "  DAY: today, tomorrow, fri, 30/9, 2026-09-30   DAYS: mo, mo,we, mo-fr\n");
	return -EINVAL;
}

PT_PROGRAM(calendar, "the diary: events in ~/calendar.txt\n"
	   "usage: calendar            the month, to browse\n"
	   "       calendar agenda [N] the next N days (7)\n"
	   "       calendar add DAY [10:00[-11:00]] what [!15]\n"
	   "       calendar add every mo,we|yearly 2/10 ...\n"
	   "       calendar rm DAY [N] the Nth event that day\n"
	   "  -f FILE another diary, -d DAY as if today\n"
	   "!15 chimes 15 minutes before; what is due in\n"
	   "~/todo.md shows up on its day.")
{
	struct opt o = { .ind = 1 };
	struct diary d;
	struct todo_item *t = NULL;
	char path[PT_PATH_MAX];
	const char *file = NULL, *cmd;
	int c, err, nt, today = day_today(), ret = 0;

	while ((c = getopt_pt(&o, "calendar", argc, argv, "f:d:")) != -1) {
		if (c == 'f') {
			file = o.arg;
		} else if (c == 'd') {
			if (!(today = day_parse(o.arg, day_today()))) {
				pt_dprintf(PT_STDERR, "calendar: %s: not a day\n", o.arg);
				return 2;
			}
		} else {
			return 2;
		}
	}
	if (!pda_file(file, "calendar.txt", path, sizeof(path)))
		return fail("calendar", "~/calendar.txt", -ENAMETOOLONG);
	if ((err = load(&d, path))) {
		diary_free(&d);
		return fail("calendar", path, err);
	}
	cmd = o.ind < argc ? argv[o.ind] : NULL;
	/* the reminders are ~/calendar.txt's: another diary does not touch them */
	if (file)
		d.no_reminders = true;
	sync_reminders(&d, today);

	if (!cmd && pt_isatty(PT_STDIN) && pt_isatty(PT_STDOUT)) {
		err = interactive(&d, today, NULL);
	} else if (!cmd || !strcmp(cmd, "agenda") || !strcmp(cmd, "ls")) {
		int days = o.ind + 1 < argc ? atoi(argv[o.ind + 1]) : 7;

		if (days < 1 || days > 366) {
			pt_dprintf(PT_STDERR, "calendar: agenda takes 1 to 366 days\n");
			ret = 2;
		} else {
			nt = todo_items(NULL, &t);
			agenda(&d, t, nt > 0 ? nt : 0, today, days);
		}
	} else if (!strcmp(cmd, "add")) {
		if ((err = add_words(&d, argv + o.ind + 1, argc - o.ind - 1, today)) == -EINVAL) {
			err = 0;
			ret = 2;
		} else if (!err && !(err = save(&d))) {
			sync_reminders(&d, today);
			pt_printf("%s\n", d.lines[d.nlines - 1]);
		}
	} else if (!strcmp(cmd, "rm") && argc - o.ind >= 2) {
		const struct event *list[32];
		int day = day_parse(argv[o.ind + 1], today), n = day ? on_day(&d, day, list, 32) : 0;
		int which = argc - o.ind >= 3 ? atoi(argv[o.ind + 2]) : 1;

		if (!day || which < 1 || which > n || (n > 1 && argc - o.ind < 3)) {
			pt_dprintf(PT_STDERR, n > 1 ? "calendar: %d events that day: which, 1-%d?\n" :
				   "calendar: no such event\n", n, n);
			ret = 1;
		} else {
			char gone[DIARY_LINE];

			strlcpy(gone, d.lines[list[which - 1]->line], sizeof(gone));
			if (!(err = remove_event(&d, list[which - 1])) && !(err = save(&d))) {
				sync_reminders(&d, today);
				pt_printf("removed: %s\n", gone);
			}
		}
	} else {
		pt_dprintf(PT_STDERR, "calendar: %s: agenda, add or rm\n", cmd);
		ret = 2;
	}
	pt_free(t);
	diary_free(&d);
	if (err)
		return fail("calendar", path, err);
	return ret;
}
