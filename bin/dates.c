/*
 * Days and times the way people type them: see dates.h.
 *
 * The arithmetic on days is done on a count of days, not through mktime,
 * so a day is a day whatever the clocks do in March and October; only
 * turning a day and an hour into a moment asks the time zone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dates.h"

const char *const weekday_short[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
const char *const month_name[12] = {
	"January", "February", "March", "April", "May", "June", "July", "August",
	"September", "October", "November", "December",
};

/* Days since 1970-01-01, and back: Howard Hinnant's civil calendar. */
static long days_from_civil(int y, int m, int d)
{
	long era;
	unsigned yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long)doe - 719468;
}

static int civil_from_days(long z)
{
	long era, y;
	unsigned doe, yoe, doy, mp, d, m;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = (long)yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	y += m <= 2;
	return (int)(y * 10000 + m * 100 + d);
}

static long serial(int day)
{
	return days_from_civil(day / 10000, day / 100 % 100, day % 100);
}

int days_in_month(int y, int m)
{
	static const int n[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
		return 29;
	return m >= 1 && m <= 12 ? n[m - 1] : 0;
}

int day_make(int y, int m, int d)
{
	if (y < 1 || m < 1 || m > 12 || d < 1 || d > days_in_month(y, m))
		return 0;
	return y * 10000 + m * 100 + d;
}

int day_of(time_t t)
{
	struct tm tm;

	localtime_r(&t, &tm);
	return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

int day_today(void)
{
	return day_of(time(NULL));
}

int day_add(int day, int n)
{
	return civil_from_days(serial(day) + n);
}

int day_diff(int a, int b)
{
	return (int)(serial(a) - serial(b));
}

int day_weekday(int day)
{
	long z = serial(day) + 3;		/* 1970-01-01 was a Thursday */

	return (int)(((z % 7) + 7) % 7);
}

time_t day_time(int day, int h, int m, int s)
{
	struct tm tm = {
		.tm_year = day / 10000 - 1900, .tm_mon = day / 100 % 100 - 1, .tm_mday = day % 100,
		.tm_hour = h, .tm_min = m, .tm_sec = s, .tm_isdst = -1,
	};

	return mktime(&tm);
}

/* ------------------------------------------------------------ reading */

static void lower_ascii(const char *s, char *out, size_t size)
{
	size_t n = 0;

	for (; *s && n + 1 < size; s++) {
		/* ì, as in lunedì: the names below are written without it */
		if ((unsigned char)s[0] == 0xc3 && (unsigned char)s[1] == 0xac) {
			out[n++] = 'i';
			s++;
			continue;
		}
		out[n++] = *s >= 'A' && *s <= 'Z' ? *s + 'a' - 'A' : *s;
	}
	out[n] = '\0';
}

int weekday_parse(const char *s)
{
	static const char *const names[2][7] = {
		{ "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday" },
		{ "lunedi", "martedi", "mercoledi", "giovedi", "venerdi", "sabato", "domenica" },
	};
	char w[16];
	size_t n;

	lower_ascii(s, w, sizeof(w));
	n = strlen(w);
	if (n < 2)
		return -1;
	for (int lang = 0; lang < 2; lang++)
		for (int d = 0; d < 7; d++)
			if (!strncmp(names[lang][d], w, n))
				return d;
	return -1;
}

int weekdays_parse(const char *s)
{
	char w[64], *p, *save = NULL;
	int set = 0;

	lower_ascii(s, w, sizeof(w));
	if (!strcmp(w, "daily") || !strcmp(w, "everyday") || !strcmp(w, "always"))
		return 0x7f;
	if (!strcmp(w, "weekdays") || !strcmp(w, "feriali"))
		return 0x1f;
	if (!strcmp(w, "weekends") || !strcmp(w, "weekend"))
		return 0x60;
	for (p = strtok_r(w, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
		char *dash = strchr(p, '-');
		int a, b;

		if (dash) {
			*dash = '\0';
			a = weekday_parse(p);
			b = weekday_parse(dash + 1);
			if (a < 0 || b < 0)
				return -1;
			for (int d = a;; d = (d + 1) % 7) {	/* fr-mo goes round */
				set |= 1 << d;
				if (d == b)
					break;
			}
		} else if ((a = weekday_parse(p)) >= 0) {
			set |= 1 << a;
		} else {
			return -1;
		}
	}
	return set ? set : -1;
}

void weekdays_text(int set, char *out, size_t size)
{
	static const char *const two[7] = { "mo", "tu", "we", "th", "fr", "sa", "su" };
	size_t n = 0;

	if (set == 0x7f) {
		snprintf(out, size, "daily");
		return;
	}
	if (set == 0x1f) {
		snprintf(out, size, "mo-fr");
		return;
	}
	if (set == 0x60) {
		snprintf(out, size, "sa,su");
		return;
	}
	out[0] = '\0';
	for (int d = 0; d < 7; d++)
		if (set & (1 << d))
			n += snprintf(out + n, n < size ? size - n : 0, "%s%s", n ? "," : "", two[d]);
}

int day_parse(const char *s, int today)
{
	char w[32], *end;
	int y, m, d, n, wd;

	lower_ascii(s, w, sizeof(w));
	if (!strcmp(w, "today") || !strcmp(w, "oggi"))
		return today;
	if (!strcmp(w, "tomorrow") || !strcmp(w, "domani"))
		return day_add(today, 1);
	if (!strcmp(w, "dopodomani"))
		return day_add(today, 2);
	if (!strcmp(w, "yesterday") || !strcmp(w, "ieri"))
		return day_add(today, -1);
	if (w[0] == '+') {
		n = strtol(w + 1, &end, 10);
		if (end == w + 1 || n < 0 || n > 3650)
			return 0;
		if (!*end || !strcmp(end, "d"))
			return day_add(today, n);
		if (!strcmp(end, "w"))
			return day_add(today, 7 * n);
		return 0;
	}
	if (sscanf(w, "%d-%d-%d%n", &y, &m, &d, &n) == 3 && !w[n] && y > 999)
		return day_make(y, m, d);
	/* 30/9, 30/9/26, 30.9.2026: the day first, as in Italy */
	if ((sscanf(w, "%d/%d%n", &d, &m, &n) == 2 || sscanf(w, "%d.%d%n", &d, &m, &n) == 2)) {
		const char *rest = w + n;

		if (*rest == '.' && !rest[1])
			rest++;			/* 30.9. */
		if (!*rest) {
			int this = day_make(today / 10000, m, d);

			/* a day well gone this year means next year's */
			if (this && day_diff(today, this) > 30)
				return day_make(today / 10000 + 1, m, d);
			return this;
		}
		if ((*rest == '/' || *rest == '.') && sscanf(rest + 1, "%d%n", &y, &n) == 1 &&
		    !rest[1 + n])
			return day_make(y < 100 ? 2000 + y : y, m, d);
		return 0;
	}
	if ((wd = weekday_parse(w)) >= 0)
		return day_add(today, (wd - day_weekday(today) + 7) % 7);
	return 0;
}

bool clock_parse(const char *s, int *h, int *m)
{
	char w[16], *p;
	int hh, mm = 0;
	bool pm = false, am = false;

	lower_ascii(s, w, sizeof(w));
	p = w + strlen(w);
	if (p - w > 2 && !strcmp(p - 2, "pm"))
		pm = true, p[-2] = '\0';
	else if (p - w > 2 && !strcmp(p - 2, "am"))
		am = true, p[-2] = '\0';
	hh = strtol(w, &p, 10);
	if (p == w || p - w > 2)
		return false;
	if (*p == ':' || *p == '.' || *p == 'h') {
		char *q;

		mm = strtol(p + 1, &q, 10);
		if (*p == 'h' && q == p + 1)
			q = p + 1, mm = 0;	/* 7h */
		else if (q != p + 3)
			return false;		/* two digits of minutes */
		p = q;
	}
	if (*p)
		return false;
	if (am || pm) {
		if (hh < 1 || hh > 12)
			return false;
		hh = hh % 12 + (pm ? 12 : 0);
	}
	if (hh < 0 || hh > 23 || mm < 0 || mm > 59)
		return false;
	*h = hh;
	*m = mm;
	return true;
}

int duration_parse(const char *s)
{
	long total = 0, n;
	char *end;
	bool any = false;

	while (*s) {
		n = strtol(s, &end, 10);
		if (end == s || n < 0)
			return -1;
		switch (*end) {
		case 'd': total += n * 86400; break;
		case 'h': total += n * 3600; break;
		case 'm': total += n * 60; break;
		case 's': total += n; break;
		case '\0':
			if (!any)
				return -1;	/* a bare number is not a duration */
			total += n * 60;	/* 1h30: the minutes */
			s = end;
			continue;
		default:
			return -1;
		}
		any = true;
		s = end + 1;
	}
	return any && total > 0 && total <= 30L * 86400 ? (int)total : -1;
}

/* ------------------------------------------------------------ writing */

void day_name(int day, int today, char *out, size_t size)
{
	int diff = day_diff(day, today);
	const char *m = month_name[day / 100 % 100 - 1];

	if (diff == 0)
		snprintf(out, size, "today");
	else if (diff == 1)
		snprintf(out, size, "tomorrow");
	else if (diff == -1)
		snprintf(out, size, "yesterday");
	else if (day / 10000 == today / 10000)
		snprintf(out, size, "%s %d %.3s", weekday_short[day_weekday(day)], day % 100, m);
	else
		snprintf(out, size, "%s %d %.3s %d", weekday_short[day_weekday(day)], day % 100, m,
			 day / 10000);
}

void when_text(time_t t, time_t now, char *out, size_t size)
{
	long left = (long)(t - now);
	int day = day_of(t), today = day_of(now), diff = day_diff(day, today);
	struct tm tm;

	localtime_r(&t, &tm);
	if (left <= 0)
		snprintf(out, size, "now");
	else if (left < 3600)
		snprintf(out, size, "in %ldm %02lds", left / 60, left % 60);
	else if (left < 12 * 3600)
		snprintf(out, size, "in %ldh %02ldm", left / 3600, left / 60 % 60);
	else if (diff == 0)
		snprintf(out, size, "today %02d:%02d", tm.tm_hour, tm.tm_min);
	else if (diff == 1)
		snprintf(out, size, "tomorrow %02d:%02d", tm.tm_hour, tm.tm_min);
	else if (diff < 7)
		snprintf(out, size, "%s %02d:%02d", weekday_short[day_weekday(day)], tm.tm_hour,
			 tm.tm_min);
	else
		snprintf(out, size, "%d %.3s %02d:%02d", day % 100, month_name[tm.tm_mon], tm.tm_hour,
			 tm.tm_min);
}
