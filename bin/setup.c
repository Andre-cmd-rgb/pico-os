/*
 * setup - what a new machine asks: a name, the time zone, Wi-Fi, the
 * clock and the colours.
 *
 * init runs it (-f) the first time the machine starts, before any shell,
 * while there is no /etc/user; after that `setup` asks it all again, and
 * `setup timezone`, say, asks one thing. What it learns is kept by
 * whoever owns it: the name and the time zone by the kernel (user.c,
 * /etc/user and /etc/timezone), the networks by the Wi-Fi driver
 * (/etc/wifi), the colours by the terminal (/etc/theme).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "drivers/drivers.h"
#include "pt/kernel.h"
#include "util.h"

#define ANSWER_MAX	96
#define PLAUSIBLE	1704067200	/* 2024: a clock before it was never set */

/*
 * Time zones by the name the tz database gives them, and the POSIX
 * string that says the same, which is all the C library reads: the
 * offset, and when summer time starts and ends. The first of a kind is
 * the one offered when only the string is known.
 */
static const struct zone {
	const char *name, *tz;
} zones[] = {
	{ "UTC", "UTC0" },
	{ "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
	{ "Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0" },
	{ "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0" },
	{ "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Istanbul", "<+03>-3" },
	{ "Europe/Moscow", "MSK-3" },
	{ "Africa/Casablanca", "<+01>-1" },
	{ "Africa/Lagos", "WAT-1" },
	{ "Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24" },
	{ "Africa/Johannesburg", "SAST-2" },
	{ "Africa/Nairobi", "EAT-3" },
	{ "Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0" },
	{ "Asia/Dubai", "<+04>-4" },
	{ "Asia/Tehran", "<+0330>-3:30" },
	{ "Asia/Karachi", "PKT-5" },
	{ "Asia/Kolkata", "IST-5:30" },
	{ "Asia/Dhaka", "<+06>-6" },
	{ "Asia/Bangkok", "<+07>-7" },
	{ "Asia/Jakarta", "WIB-7" },
	{ "Asia/Shanghai", "CST-8" },
	{ "Asia/Hong_Kong", "HKT-8" },
	{ "Asia/Taipei", "CST-8" },
	{ "Asia/Singapore", "<+08>-8" },
	{ "Asia/Manila", "PST-8" },
	{ "Asia/Seoul", "KST-9" },
	{ "Asia/Tokyo", "JST-9" },
	{ "Australia/Perth", "AWST-8" },
	{ "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
	{ "Australia/Brisbane", "AEST-10" },
	{ "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
	{ "Pacific/Honolulu", "HST10" },
	{ "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
	{ "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
	{ "America/Vancouver", "PST8PDT,M3.2.0,M11.1.0" },
	{ "America/Phoenix", "MST7" },
	{ "America/Denver", "MST7MDT,M3.2.0,M11.1.0" },
	{ "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
	{ "America/Mexico_City", "CST6" },
	{ "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
	{ "America/Toronto", "EST5EDT,M3.2.0,M11.1.0" },
	{ "America/Bogota", "<-05>5" },
	{ "America/Lima", "<-05>5" },
	{ "America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
	{ "America/Sao_Paulo", "<-03>3" },
	{ "America/Buenos_Aires", "<-03>3" },
};

#define NZONES	((int)(sizeof(zones) / sizeof(zones[0])))

/* "Europe/Rome" is "Rome"; "New_York" is typed "new york" too. */
static const char *city(const char *name)
{
	const char *slash = strrchr(name, '/');

	return slash ? slash + 1 : name;
}

static bool same_words(const char *typed, const char *name)
{
	for (; *typed && *name; typed++, name++)
		if (tolower((unsigned char)*typed) != tolower((unsigned char)*name) &&
		    !(*typed == ' ' && *name == '_'))
			return false;
	return !*typed && !*name;
}

static const struct zone *find_zone(const char *typed)
{
	for (int i = 0; i < NZONES; i++)
		if (same_words(typed, zones[i].name) || same_words(typed, city(zones[i].name)))
			return &zones[i];
	return NULL;
}

/* "CET-1CEST,...", "<+04>-4": an offset in it, and no spaces or slashes before a comma. */
static bool looks_like_tz(const char *s)
{
	size_t head = strcspn(s, ",");

	return (isalpha((unsigned char)*s) || *s == '<') && strpbrk(s, "0123456789") &&
	       !strchr(s, ' ') && strcspn(s, "/") >= head;
}

static void heading(const char *title)
{
	pt_printf("\n\x1b[1m%s\x1b[0m\n", title);
}

/* ------------------------------------------------------------ the steps */

static int step_name(bool first)
{
	char name[ANSWER_MAX], old[USER_NAME_MAX + 1];
	int err;

	heading("Your name");
	pt_puts("For the prompt, and your home directory: /home/NAME.\n");
	strlcpy(old, user_name(), sizeof(old));
	for (;;) {
		if (ask_text("Name", old, name, sizeof(name)))
			return -ECANCELED;
		for (char *p = name; *p; p++)
			*p = (char)tolower((unsigned char)*p);
		if (user_name_ok(name))
			break;
		pt_printf("  up to %d small letters, digits, - and _,\n"
			  "  starting with a letter\n", USER_NAME_MAX);
	}
	if ((err = user_set_name(name))) {
		pt_printf("setup: cannot keep the name: %s\n", pt_strerror(err));
		return 0;
	}
	if (strcmp(name, old)) {
		pt_printf("Home is now %s.\n", user_home());
		if (!first)
			pt_puts("Shells already open keep the old $HOME until\n"
				"they start again; `reboot` starts them all.\n");
	}
	return 0;
}

static void list_zones(void)
{
	int col = 0, len;
	char name[32];

	for (int i = 0; i < NZONES; i++) {
		strlcpy(name, city(zones[i].name), sizeof(name));
		for (char *p = name; *p; p++)
			if (*p == '_')
				*p = ' ';
		len = (int)strlen(name) + 1;		/* and its comma */
		if (col && col + 1 + len > 53) {
			pt_puts("\n");
			col = 0;
		} else if (col) {
			pt_puts(" ");
			col++;
		}
		pt_printf("%s%s", name, i + 1 < NZONES ? "," : "\n");
		col += len;
	}
}

static const char *current_zone(void)
{
	if (*user_tz_name())
		return city(user_tz_name());
	for (int i = 0; i < NZONES; i++)
		if (!strcmp(zones[i].tz, user_tz()))
			return city(zones[i].name);
	return user_tz();
}

static int step_timezone(void)
{
	char typed[ANSWER_MAX], dflt[USER_TZ_MAX + 1];
	const struct zone *z;
	int err;

	heading("Time zone");
	pt_puts("A city near you (? lists them), or a POSIX TZ\n"
		"string such as CET-1CEST,M3.5.0,M10.5.0/3.\n");
	strlcpy(dflt, current_zone(), sizeof(dflt));
	for (char *p = dflt; *p; p++)
		if (*p == '_')
			*p = ' ';
	for (;;) {
		if (ask_text("Time zone", dflt, typed, sizeof(typed)))
			return -ECANCELED;
		if (!strcmp(typed, "?")) {
			list_zones();
			continue;
		}
		if ((z = find_zone(typed))) {
			err = user_set_tz(z->name, z->tz);
			break;
		}
		if (looks_like_tz(typed)) {
			err = user_set_tz(NULL, typed);
			break;
		}
		pt_printf("  %s: not a city here (? lists them)\n", typed);
	}
	if (err)
		pt_printf("setup: cannot keep the time zone: %s\n", pt_strerror(err));
	return 0;
}

static int step_wifi(void)
{
	struct wifi_info info;
	time_t now;
	int r;

	heading("Wi-Fi");
	if (!wifi_state(&info) && info.up) {
		pt_printf("On %s already.\n", info.ssid);
		r = ask_yes("Join another?", false);
	} else {
		r = ask_yes("Join a network now?", true);
	}
	if (r <= 0)
		return r;
	if (!wifi_started() && (r = wifi_radio(true))) {
		pt_printf("no Wi-Fi: %s\n", r == -ENODEV ? "this board has no radio" :
			  pt_strerror(r));
		return 0;
	}
	if ((r = wifi_choose()) != 1)
		return r < 0 ? r : 0;
	wifi_state(&info);
	pt_printf("joined %s; it is joined again at every start\n", info.ssid);
	if (!wifi_ntp_sync(10000)) {
		now = time(NULL);
		pt_printf("the clock is set from the network: %s", ctime(&now));
	}
	return 0;
}

static int step_clock(bool asked)
{
	char typed[ANSWER_MAX], shown[48];
	time_t now = time(NULL);
	struct tm tm = { 0 };
	int sec = 0;

	/* after Wi-Fi has set it, the whole of setup has nothing to ask */
	if (!asked && now >= PLAUSIBLE && wifi_up())
		return 0;
	heading("The clock");
	if (now >= PLAUSIBLE) {
		localtime_r(&now, &tm);
		strftime(shown, sizeof(shown), "%a %e %b %Y, %H:%M", &tm);
		pt_printf("It is %s.\nEnter if that is right, or type the date and time\n", shown);
	} else {
		pt_puts("The clock is not set: Wi-Fi sets it, or type the\ndate and time ");
	}
	pt_puts("as 2026-09-28 14:30.\n");
	for (;;) {
		if (ask_text("Now", NULL, typed, sizeof(typed)))
			return -ECANCELED;
		if (!*typed)
			return 0;
		memset(&tm, 0, sizeof(tm));
		sec = 0;
		if (sscanf(typed, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
			   &tm.tm_hour, &tm.tm_min, &sec) >= 5 && tm.tm_year >= 2024 &&
		    tm.tm_mon >= 1 && tm.tm_mon <= 12 && tm.tm_mday >= 1 && tm.tm_mday <= 31 &&
		    tm.tm_hour < 24 && tm.tm_min < 60)
			break;
		pt_puts("  as 2026-09-28 14:30, please\n");
	}
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	tm.tm_sec = sec;
	tm.tm_isdst = -1;
	struct timeval tv = { .tv_sec = mktime(&tm) };

	settimeofday(&tv, NULL);
	clock_changed(false);
	return 0;
}

static int step_theme(void)
{
	struct theme_state st;
	char typed[ANSWER_MAX];
	int i, n = theme_count();

	if (!vt_has_display())
		return 0;
	heading("Colours");
	for (i = 0; i < n; i++)
		pt_printf(i % 3 == 2 || i + 1 == n ? "%2d %s\n" : "%2d %-11s ", i + 1,
			  theme_name_at(i));
	pt_puts("A number shows that theme; Enter keeps it.\n");
	for (;;) {
		theme_get(&st);
		if (ask_text("Theme", st.name, typed, sizeof(typed)))
			return -ECANCELED;
		if (!strcmp(typed, st.name))
			break;
		i = atoi(typed);
		if (i >= 1 && i <= n)
			theme_use(theme_name_at(i - 1));
		else if (theme_use(typed))
			pt_printf("  %s: a number from 1 to %d\n", typed, n);
	}
	if (theme_save())
		pt_puts("setup: could not save /etc/theme\n");
	return 0;
}

/* ---------------------------------------------------------------- setup */

static const char *const steps[] = { "name", "timezone", "wifi", "clock", "theme", NULL };

static int run_step(const char *name, bool first, bool asked)
{
	if (!strcmp(name, "name"))
		return step_name(first);
	if (!strcmp(name, "timezone"))
		return step_timezone();
	if (!strcmp(name, "wifi"))
		return step_wifi();
	if (!strcmp(name, "clock"))
		return step_clock(asked);
	return step_theme();
}

PT_COMPLETE(setup, ": name timezone wifi clock theme\n")

PT_PROGRAM(setup, "set the machine up: name, time zone, Wi-Fi...\n"
	   "usage: setup [name | timezone | wifi | clock | theme]\n"
	   "Asks, in turn, your name (the prompt's, and your\n"
	   "home's: /home/NAME), the time zone, a Wi-Fi network\n"
	   "to join, the time if nothing has set it, and the\n"
	   "colours; Enter keeps the answer in [brackets], and\n"
	   "Esc or Ctrl-C leaves the rest as it is. With a word,\n"
	   "only that. The first start runs it by itself (-f),\n"
	   "until /etc/user is there.")
{
	bool first = argc == 2 && !strcmp(argv[1], "-f");
	const char *only = argc == 2 && !first ? argv[1] : NULL;
	int me = pt_getpid(), err = 0;

	if (argc > 2 || (only && !strcmp(only, "-f"))) {
		pt_dprintf(PT_STDERR, "usage: setup [name | timezone | wifi | clock | theme]\n");
		return 2;
	}
	if (only) {
		int i;

		for (i = 0; steps[i] && strcmp(steps[i], only); i++)
			;
		if (!steps[i]) {
			pt_dprintf(PT_STDERR, "setup: %s: it asks name, timezone, wifi, clock "
				   "or theme\n", only);
			return 2;
		}
	}
	/* started by init, not a shell: the terminal's keys are for us */
	if (first)
		pt_ioctl(PT_STDIN, PT_TTY_SETPGRP, &me);
	pt_sigcatch(true);
	if (first)
		pt_puts("\x1b[2J\x1b[H\x1b[1mWelcome to PocketType\x1b[0m\n"
			"A few questions to set it up. Enter keeps the\n"
			"answer in [brackets]; Esc leaves the rest for later\n"
			"(`setup` asks again).\n");
	if (only)
		err = run_step(only, false, true);
	for (int i = 0; !only && steps[i] && !err; i++)
		err = run_step(steps[i], first, false);

	/* the first start is over, whatever was answered */
	if (first && !user_configured() && user_set_name(user_name()))
		pt_puts("setup: cannot write /etc/user\n");
	if (err == -ECANCELED) {
		pt_puts(first ? "The rest is left for later: `setup` asks again.\n" : "");
		return first ? 0 : 1;
	}
	if (!only)
		pt_puts(first ? "\nAll set. `help` shows the commands, `man intro`\n"
				"is the place to start, and `setup` asks all\n"
				"this again.\n" : "\nDone.\n");
	return 0;
}
