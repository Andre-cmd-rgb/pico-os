/*
 * theme - the screen's colours and looks.
 *
 * The themes themselves, and /etc/theme where the choice is kept, are
 * the terminal driver's (drivers/tty/theme.c): the colours have to be
 * right from the first line of the boot log, before any program runs.
 * This is only the way to change them.
 */
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "util.h"

#define USAGE	"usage: theme [NAME] [dark|light|auto [FROM UNTIL]]\n" \
		"       theme set SLOT COLOR | reset | list\n" \
		"       theme cursor block|underline|bar\n" \
		"       theme blink on|off|MS | bar top|bottom\n"

/* "7:00" or "7" into minutes after midnight; -1 if it is not a time. */
static int clock_time(const char *s)
{
	char *end;
	long h = strtol(s, &end, 10), m = 0;

	if (end == s || h < 0 || h > 23)
		return -1;
	if (*end == ':' || *end == '.') {
		const char *mm = end + 1;

		m = strtol(mm, &end, 10);
		if (end == mm || m < 0 || m > 59)
			return -1;
	}
	return *end ? -1 : (int)(h * 60 + m);
}

/* "#1d2021", "1d2021", or "#fff"; -1 if it is not a colour. */
static long parse_color(const char *s)
{
	char *end;
	unsigned long v;
	size_t n;

	if (*s == '#')
		s++;
	n = strlen(s);
	v = strtoul(s, &end, 16);
	if (*end || (n != 6 && n != 3))
		return -1;
	if (n == 3)			/* #abc is #aabbcc */
		v = (v >> 8 & 0xf) * 0x110000 + (v >> 4 & 0xf) * 0x1100 + (v & 0xf) * 0x11;
	return (long)v;
}

/* The sixteen colours as they are now, a block each, normal then bright. */
static void swatch(void)
{
	pt_puts("  ");
	for (int i = 0; i < 8; i++)
		pt_printf("\x1b[4%dm   ", i);
	pt_puts("\x1b[0m\n  ");
	for (int i = 0; i < 8; i++)
		pt_printf("\x1b[10%dm   ", i);
	pt_puts("\x1b[0m\n");
}

static void show(void)
{
	static const char *const shapes[] = { "a block", "an underline", "a bar" };
	struct theme_state st;

	theme_get(&st);
	if (st.mode == THEME_AUTO)
		pt_printf("theme %s, auto: light %d:%02d-%d:%02d, %s now\n", st.name,
			  st.day_from / 60, st.day_from % 60, st.day_until / 60, st.day_until % 60,
			  st.light ? "light" : "dark");
	else
		pt_printf("theme %s, %s\n", st.name, st.light ? "light" : "dark");
	swatch();
	pt_printf("\x1b[1mbold\x1b[0m \x1b[2mfaint\x1b[0m \x1b[7minverse\x1b[0m "
		  "\x1b[31mred\x1b[0m \x1b[32mgreen\x1b[0m \x1b[33myellow\x1b[0m "
		  "\x1b[34mblue\x1b[0m \x1b[35mmagenta\x1b[0m \x1b[36mcyan\x1b[0m\n");
	pt_printf("cursor %s, %s; status line at the %s\n", shapes[st.cursor],
		  st.blink_ms ? "blinking" : "steady", st.bar_top ? "top" : "bottom");
	if (st.own)
		pt_printf("%d colour%s of your own (theme reset drops them)\n", st.own,
			  st.own == 1 ? "" : "s");
}

static void list(void)
{
	struct theme_state st;

	theme_get(&st);
	for (int i = 0; i < theme_count(); i++)
		pt_printf("%c %-11s %s\n", strcmp(theme_name_at(i), st.name) ? ' ' : '*',
			  theme_name_at(i), theme_about_at(i));
}

static int mode_arg(int argc, char **argv, int i)
{
	struct theme_state st;
	int from, until, err;

	theme_get(&st);
	from = st.day_from;
	until = st.day_until;
	if (!strcmp(argv[i], "dark"))
		err = theme_set_mode(THEME_DARK, from, until);
	else if (!strcmp(argv[i], "light"))
		err = theme_set_mode(THEME_LIGHT, from, until);
	else if (!strcmp(argv[i], "auto")) {
		if (i + 2 < argc) {
			from = clock_time(argv[i + 1]);
			until = clock_time(argv[i + 2]);
			if (from < 0 || until < 0 || from == until) {
				pt_dprintf(PT_STDERR, "theme: auto FROM UNTIL: times of day, "
					   "light from 7:00 until 20:00, say\n");
				return -1;
			}
		} else if (i + 1 < argc) {
			pt_dprintf(PT_STDERR, USAGE);
			return -1;
		}
		err = theme_set_mode(THEME_AUTO, from, until);
	} else {
		pt_dprintf(PT_STDERR, "theme: dark, light or auto, not %s\n", argv[i]);
		return -1;
	}
	return err ? -1 : 0;
}

/* For Tab: the themes' names first, and after one of them its halves. */
static void theme_more(const char *after, pt_complete_add add, void *ctx)
{
	for (int i = 0; i < theme_count(); i++) {
		if (!*after) {
			add(ctx, theme_name_at(i));
		} else if (!strcmp(after, theme_name_at(i))) {
			add(ctx, "dark");
			add(ctx, "light");
			add(ctx, "auto");
		}
	}
}

PT_COMPLETE_MORE(theme, ": list set reset cursor blink bar dark light auto <more>\n"
		 "cursor: block underline bar\n"
		 "blink: on off\n"
		 "bar: top bottom\n"
		 "set: fg bg dim bold bar bar-text cursor black red green yellow blue\n"
		 "*: <more>\n", theme_more)

PT_PROGRAM(theme, "the screen's colours: dark, light, your own\n"
	   USAGE
	   "Alone it shows the theme and its colours; list shows\n"
	   "them all. A NAME switches (its dark or light half),\n"
	   "dark and light switch halves, and auto is light from\n"
	   "FROM until UNTIL (7:00 20:00 unless given), dark the\n"
	   "rest of the day.\n"
	   "set gives one colour your own value, over the theme's:\n"
	   "SLOT is fg bg dim bold bar bar-text cursor, a colour\n"
	   "name (red, bright-red, ...) or 0-15; COLOR is #rrggbb.\n"
	   "  theme set bg #101418\n"
	   "Everything is kept in /etc/theme, for the next boot.")
{
	int err = 0;

	if (!vt_has_display()) {
		pt_dprintf(PT_STDERR, "theme: there is no screen\n");
		return 1;
	}
	if (argc == 1) {
		show();
		return 0;
	}
	if (!strcmp(argv[1], "list") && argc == 2) {
		list();
		return 0;
	}
	if (!strcmp(argv[1], "set") && argc == 4) {
		long rgb = parse_color(argv[3]);

		if (rgb < 0) {
			pt_dprintf(PT_STDERR, "theme: %s: a colour is #rrggbb\n", argv[3]);
			return 2;
		}
		err = theme_set_color(argv[2], (uint32_t)rgb);
		if (err == -EINVAL) {
			pt_dprintf(PT_STDERR, "theme: %s: the slots are fg bg dim bold bar "
				   "bar-text cursor, red..., 0-15\n", argv[2]);
			return 2;
		}
		if (err == -ENOSPC) {
			pt_dprintf(PT_STDERR, "theme: that many colours of your own is the "
				   "most; theme reset first\n");
			return 1;
		}
	} else if (!strcmp(argv[1], "reset") && argc == 2) {
		theme_reset_colors();
	} else if (!strcmp(argv[1], "cursor") && argc == 3) {
		struct theme_state st;
		enum vt_cursor c = !strcmp(argv[2], "block") ? VT_CURSOR_BLOCK :
				   !strcmp(argv[2], "underline") ? VT_CURSOR_UNDERLINE :
				   !strcmp(argv[2], "bar") ? VT_CURSOR_BAR : (enum vt_cursor)-1;

		if ((int)c < 0) {
			pt_dprintf(PT_STDERR, "theme: the cursor is a block, an underline or a bar\n");
			return 2;
		}
		theme_get(&st);
		theme_set_cursor(c, st.blink_ms);
	} else if (!strcmp(argv[1], "blink") && argc == 3) {
		struct theme_state st;
		int ms = !strcmp(argv[2], "on") ? 530 : !strcmp(argv[2], "off") ? 0 : atoi(argv[2]);

		if (ms < 0 || ms > 2000 || (!ms && strcmp(argv[2], "off") && strcmp(argv[2], "0"))) {
			pt_dprintf(PT_STDERR, "theme: blink on, off, or a period in ms up to 2000\n");
			return 2;
		}
		theme_get(&st);
		theme_set_cursor(st.cursor, ms);
	} else if (!strcmp(argv[1], "bar") && argc == 3 &&
		   (!strcmp(argv[2], "top") || !strcmp(argv[2], "bottom"))) {
		theme_set_bar(!strcmp(argv[2], "top"));
	} else if (!strcmp(argv[1], "dark") || !strcmp(argv[1], "light") ||
		   !strcmp(argv[1], "auto")) {
		if (mode_arg(argc, argv, 1))
			return 2;
	} else {
		if (theme_use(argv[1])) {
			pt_dprintf(PT_STDERR, "theme: no theme called %s; theme list shows them\n",
				   argv[1]);
			return 1;
		}
		if (argc > 2 && mode_arg(argc, argv, 2))
			return 2;
	}
	if ((err = theme_save()))
		pt_dprintf(PT_STDERR, "theme: could not save /etc/theme\n");
	show();
	return 0;
}
