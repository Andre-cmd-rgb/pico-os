/*
 * Days and times the way people type them, for alarm, todo and calendar.
 *
 * A day is an int, YYYYMMDD, so it sorts and compares as it reads and
 * goes into a file as it is. Weekdays count from Monday, 0, to Sunday, 6.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

int	day_today(void);
int	day_of(time_t t);			/* the local day a moment falls on */
int	day_make(int y, int m, int d);		/* 0 if there is no such day */
int	day_add(int day, int n);		/* n days on (or back) */
int	day_weekday(int day);
int	day_diff(int a, int b);			/* days from b to a */
time_t	day_time(int day, int h, int m, int s);	/* that moment, local time */
int	days_in_month(int y, int m);

/*
 * A day as typed: 2026-09-30, 30/9, 30/9/26, today, tomorrow (oggi,
 * domani), a weekday (the next one, today counting), +3 or +2w. 0 if it
 * is not one. `today` is what those count from.
 */
int	day_parse(const char *s, int today);

/* A weekday name or prefix, English or Italian (mon, lun, fr, ven); -1. */
int	weekday_parse(const char *s);

/*
 * Days of the week as a set, bit 0 Monday: daily, weekdays, weekends,
 * mo-fr, mo,we,fr, a single name. -1 if it is not one.
 */
int	weekdays_parse(const char *s);
void	weekdays_text(int set, char *out, size_t size);	/* daily, mo-fr, mo,we,fr */

/* 7:00, 07:30, 19.30, 7, 7am, 7:30pm. */
bool	clock_parse(const char *s, int *h, int *m);

/* 25m, 1h30m, 90s, 2h: seconds, or -1. */
int	duration_parse(const char *s);

/* "today", "tomorrow", "yesterday", "Wed 30 Sep", "Wed 30 Sep 2027". */
void	day_name(int day, int today, char *out, size_t size);
/* "in 25m 10s", "in 3h 12m", "tomorrow 07:00", "Wed 07:00", "30 Sep 07:00". */
void	when_text(time_t t, time_t now, char *out, size_t size);

extern const char *const weekday_short[7];	/* Mon ... Sun */
extern const char *const month_name[12];	/* January ... */
