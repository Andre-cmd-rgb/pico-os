/*
 * Themes: the screen's colours, and a few other looks.
 *
 * A theme is two palettes, a dark one and a light one, each the sixteen
 * ANSI colours and the terminal's own (vt.c): text, background, faint,
 * bold, the status line's text and background, the cursor. The light
 * halves follow one rule so that programs stay readable in them: "white"
 * (7 and 15) is dark text and "black" (0) a light grey, as gruvbox's
 * light palette has it, since programs here colour text far more often
 * than they colour a background.
 *
 * The font's strokes are one pixel wide, and this panel washes pale
 * colours out further, so every colour meant for text stands clear of
 * its background by a contrast ratio (WCAG's): 5 to 5.5 for the colours,
 * 10 for the text itself, 3.5 for faint text and greys, and the status
 * line's text 5.5 from its bar. The palettes keep each theme's hues and
 * were moved only in lightness, just far enough to reach those.
 *
 * Over the chosen palette go colours of your own (`theme set bg #1d2021`),
 * and the mode can be auto: light by day, dark by night, changing on the
 * minute. The cursor's shape and blink and the status line's place are
 * kept here too. All of it lives in /etc/theme, which is plain text:
 *
 *	theme gruvbox
 *	mode auto 7:00 20:00
 *	cursor underline
 *	blink 530
 *	bar top
 *	color bg #1d2021
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"

#ifndef CONFIG_PT_CURSOR_BLINK_MS
#define CONFIG_PT_CURSOR_BLINK_MS 0		/* display disabled */
#endif

#define THEME_FILE	"/etc/theme"
#define MAX_OWN		8		/* colours of your own */

struct theme {
	const char	*name;
	const char	*about;
	uint32_t	 dark[VT_COLORS];
	uint32_t	 light[VT_COLORS];
};

/*
 * Each palette: black red green yellow blue magenta cyan white, the same
 * bright, then text, background, faint, bold, bar text, bar, cursor.
 */
static const struct theme themes[] = {
	{ "amber", "amber CRT; light: amber ink on cream",
	  { 0x080604, 0xff5f3a, 0xffb000, 0xffd67a, 0xa57823, 0xffd67a, 0xffb000, 0xffb000,
	    0x8a5f0c, 0xff7f5a, 0xffc53d, 0xffe6a8, 0xc99a3f, 0xffe6a8, 0xffc53d, 0xfff1cc,
	    0xffb000, 0x080604, 0xa57823, 0xfff1cc, 0x080604, 0xffb000, 0xffb000 },
	  { 0xeadfc4, 0xb73010, 0x7a5200, 0x875a03, 0x7e5e1c, 0x875a03, 0x7a5200, 0x5c3d00,
	    0x977742, 0xc02f0d, 0x8f6200, 0x8e6206, 0x81631f, 0x8e6206, 0x8f6200, 0x2e1e00,
	    0x533702, 0xfbf1dc, 0x977742, 0x2e1e00, 0xfbf1dc, 0x875805, 0x976201 } },
	{ "green", "green CRT; light: green ink on mint",
	  { 0x030803, 0xff5f3a, 0x33ff66, 0x9cffb5, 0x1f9e45, 0x9cffb5, 0x33ff66, 0x33ff66,
	    0x357545, 0xff7f5a, 0x66ff8c, 0xccffd9, 0x2fc45c, 0xccffd9, 0x66ff8c, 0xe6ffec,
	    0x33ff66, 0x030803, 0x1f9e45, 0xe6ffec, 0x030803, 0x33ff66, 0x33ff66 },
	  { 0xd3e8d6, 0xb73023, 0x01732c, 0x367001, 0x2f6f4a, 0x367001, 0x01732c, 0x0b5d23,
	    0x588763, 0xbe2f22, 0x017b30, 0x3c7703, 0x2a764d, 0x3c7703, 0x017b30, 0x03300f,
	    0x024717, 0xeef7ee, 0x588763, 0x03300f, 0xeef7ee, 0x04722c, 0x1a7d36 } },
	{ "gruvbox", "warm retro browns and yellows",
	  { 0x282828, 0xfd6757, 0x9d9c22, 0xd79921, 0x61a0a3, 0xd07ea3, 0x6fa571, 0xccbda7,
	    0x928374, 0xfe7762, 0xb8bb26, 0xfabd2f, 0x83a598, 0xd5889d, 0x8ec07c, 0xf8e8bf,
	    0xebdbb2, 0x282828, 0x928374, 0xfbf1c7, 0x282828, 0xfabd2f, 0xebdbb2 },
	  { 0xebdbb2, 0xbf0f0e, 0x676606, 0x835a04, 0x296a6e, 0x974b6e, 0x3b6e3e, 0x5e5147,
	    0x87796a, 0x9d0006, 0x706b01, 0x8f5b05, 0x076678, 0x8f3f71, 0x397250, 0x3b3735,
	    0x3c3836, 0xfbf1c7, 0x87796a, 0x282828, 0xfbf1c7, 0x8a5703, 0x3c3836 } },
	{ "nord", "arctic blues and frost",
	  { 0x3b4252, 0xe8868d, 0xa3be8c, 0xebcb8b, 0x89a9c9, 0xbf99b8, 0x88c0d0, 0xe5e9f0,
	    0x7e899e, 0xf08d95, 0xa3be8c, 0xebcb8b, 0x90b1d1, 0xc7a0c0, 0x8fbcbb, 0xf6f9fe,
	    0xdfe5f0, 0x2e3440, 0x7b89a4, 0xf6f9fe, 0x2e3440, 0x88c0d0, 0xd8dee9 },
	  { 0xd8dee9, 0x9e434e, 0x426b17, 0x7f5702, 0x40628b, 0x7d537a, 0x276776, 0x475064,
	    0x6e7b93, 0xa8424e, 0x477125, 0x875b03, 0x446691, 0x825780, 0x336e7e, 0x2e3440,
	    0x2e3440, 0xeceff4, 0x6e7b93, 0x222730, 0xeceff4, 0x40628b, 0x2e3440 } },
	{ "dracula", "purple night; light is Alucard",
	  { 0x21222c, 0xfe6663, 0x50fa7b, 0xf1fa8c, 0xbd93f9, 0xff79c6, 0x8be9fd, 0xf8f8f2,
	    0x6c7caf, 0xfe7674, 0x69ff94, 0xffffa5, 0xd6acff, 0xff92df, 0xa4ffff, 0xffffff,
	    0xf8f8f2, 0x282a36, 0x6c7caf, 0xffffff, 0x282a36, 0xbd93f9, 0xf8f8f2 },
	  { 0xe8e2cc, 0xbf2d1e, 0x14710a, 0x786203, 0x644ac9, 0xa3144d, 0x036a96, 0x3f3f3f,
	    0x6c664b, 0xc53b2e, 0x0f7e02, 0x826b07, 0x715ac8, 0xbf185a, 0x0773a2, 0x1f1f1f,
	    0x1f1f1f, 0xfffbeb, 0x6c664b, 0x000000, 0xfffbeb, 0x644ac9, 0x1f1f1f } },
	{ "solarized", "Ethan Schoonover's classic",
	  { 0x073642, 0xff5a50, 0x889c0a, 0xb88c0a, 0x3698df, 0xf5589f, 0x2aa198, 0xeee8d5,
	    0x667d84, 0xf97546, 0x8fa41c, 0xc0941c, 0x41a1ea, 0x8c93e9, 0x35a9a0, 0xfdf6e3,
	    0xc1d3d5, 0x002b36, 0x667d84, 0xd5e4e4, 0x002b36, 0x41a1ea, 0x93a1a1 },
	  { 0xeee8d5, 0xc6131a, 0x5d6b06, 0x7e5f04, 0x046aa7, 0xbf1f72, 0x06726b, 0x43595f,
	    0x717e7e, 0xbf3f00, 0x647306, 0x876604, 0x036fb0, 0x5e62b4, 0x097770, 0x073642,
	    0x2e434a, 0xfdf6e3, 0x717e7e, 0x04333f, 0xfdf6e3, 0x046aa7, 0x586e75 } },
	{ "catppuccin", "pastels: mocha, and latte by day",
	  { 0x45475a, 0xf38ba8, 0xa6e3a1, 0xf9e2af, 0x89b4fa, 0xf5c2e7, 0x94e2d5, 0xbac2de,
	    0x6f7288, 0xf38ba8, 0xa6e3a1, 0xf9e2af, 0x89b4fa, 0xf5c2e7, 0x94e2d5, 0xd1d8f4,
	    0xcdd6f4, 0x1e1e2e, 0x6d7188, 0xffffff, 0x1e1e2e, 0xcba6f7, 0xf5e0dc },
	  { 0xbcc0cc, 0xbf0632, 0x1e7303, 0x8f5703, 0x0652e0, 0xa33389, 0x076b71, 0x4e5168,
	    0x777a8a, 0xce0436, 0x217706, 0x945b05, 0x115ae9, 0xa93a8f, 0x097379, 0x31334b,
	    0x383b54, 0xeff1f5, 0x777a8c, 0x232634, 0xeff1f5, 0x7e2ae3, 0xa75a4a } },
	{ "tokyonight", "Tokyo neon: night and day",
	  { 0x15161e, 0xf7768e, 0x9ece6a, 0xe0af68, 0x7aa2f7, 0xbb9af7, 0x7dcfff, 0xa9b1d6,
	    0x697193, 0xf7768e, 0x9ece6a, 0xe0af68, 0x7aa2f7, 0xbb9af7, 0x7dcfff, 0xc8d2fd,
	    0xc0caf5, 0x1a1b26, 0x646d98, 0xffffff, 0x1a1b26, 0x7aa2f7, 0xc0caf5 },
	  { 0xc4c8da, 0xaf0642, 0x466226, 0x715224, 0x0156b7, 0x772ac9, 0x015f7f, 0x394781,
	    0x6a6e8b, 0xbf0148, 0x4a662b, 0x7a5b2d, 0x015bc2, 0x7f36d4, 0x046689, 0x05237e,
	    0x0a2b86, 0xe1e2e7, 0x676e96, 0x05175b, 0xe1e2e7, 0x0156b7, 0x3760bf } },
	{ "rosepine", "soft rose: main, and dawn by day",
	  { 0x26233a, 0xeb6f92, 0x4f91ad, 0xf6c177, 0x9ccfd8, 0xc4a7e7, 0xebbcba, 0xe0def4,
	    0x716d89, 0xeb6f92, 0x5294b0, 0xf6c177, 0x9ccfd8, 0xc4a7e7, 0xebbcba, 0xe0def4,
	    0xe0def4, 0x191724, 0x716d89, 0xffffff, 0x191724, 0xebbcba, 0xe0def4 },
	  { 0xf2e9e1, 0x984a62, 0x286983, 0x8d5908, 0x2e6d78, 0x6f5a87, 0x9f4f4d, 0x575279,
	    0x807b8d, 0x9f5067, 0x286983, 0x975f07, 0x34737e, 0x77618f, 0xa25250, 0x393358,
	    0x403b60, 0xfaf4ed, 0x807b8d, 0x26233a, 0xfaf4ed, 0x9f4f4d, 0x575279 } },
	{ "mono", "grey on black, black on white",
	  { 0x000000, 0xd75f5f, 0x87af87, 0xd7af5f, 0x87afd7, 0xaf87af, 0x87afaf, 0xd0d0d0,
	    0x707070, 0xff8787, 0xafd7af, 0xffd787, 0xafd7ff, 0xd7afd7, 0xafd7d7, 0xffffff,
	    0xd0d0d0, 0x000000, 0x707070, 0xffffff, 0x000000, 0xd0d0d0, 0xffffff },
	  { 0xe0e0e0, 0xaf0000, 0x067b05, 0x875f00, 0x0057af, 0x870087, 0x005f87, 0x404040,
	    0x838383, 0xd70000, 0x058304, 0x8b6b02, 0x005fd7, 0xaf00af, 0x07779a, 0x000000,
	    0x202020, 0xffffff, 0x808080, 0x000000, 0xffffff, 0x202020, 0x202020 } },
};

#define NTHEMES	((int)(sizeof(themes) / sizeof(themes[0])))

static const char *const slot_names[VT_COLORS] = {
	"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
	"bright-black", "bright-red", "bright-green", "bright-yellow", "bright-blue",
	"bright-magenta", "bright-cyan", "bright-white",
	"fg", "bg", "dim", "bold", "bar-text", "bar", "cursor",
};

static struct {
	int		 theme;
	enum theme_mode	 mode;
	int		 day_from, day_until;
	bool		 light;			/* showing now */
	struct { int slot; uint32_t rgb; } own[MAX_OWN];
	int		 nown;
	enum vt_cursor	 cursor;
	int		 blink_ms;
	bool		 bar_top;
} cur = {
#if CONFIG_PT_THEME_GREEN
	.theme = 1,
#endif
	.day_from = 7 * 60, .day_until = 20 * 60,
	.blink_ms = CONFIG_PT_CURSOR_BLINK_MS, .bar_top = true,
};

void theme_palette(uint32_t rgb[VT_COLORS])
{
	const struct theme *t = &themes[cur.theme];

	memcpy(rgb, cur.light ? t->light : t->dark, sizeof(t->dark));
	for (int i = 0; i < cur.nown; i++)
		rgb[cur.own[i].slot] = cur.own[i].rgb;
}

static void show(void)
{
	uint32_t rgb[VT_COLORS];

	theme_palette(rgb);
	vt_set_palette(rgb);
}

/* Whether auto mode wants the light half now. */
static bool daytime(void)
{
	time_t now = time(NULL);
	struct tm tm;
	int m;

	if (now < 1000000000)
		return false;		/* the clock is not set: no idea */
	localtime_r(&now, &tm);
	m = tm.tm_hour * 60 + tm.tm_min;
	if (cur.day_from <= cur.day_until)
		return m >= cur.day_from && m < cur.day_until;
	return m >= cur.day_from || m < cur.day_until;
}

static bool want_light(void)
{
	return cur.mode == THEME_LIGHT || (cur.mode == THEME_AUTO && daytime());
}

void theme_tick(void)
{
	if (cur.mode == THEME_AUTO && want_light() != cur.light) {
		cur.light = !cur.light;
		show();
	}
}

void theme_default(void)
{
	cur.light = want_light();
	show();
}

int theme_count(void)
{
	return NTHEMES;
}

const char *theme_name_at(int i)
{
	return i >= 0 && i < NTHEMES ? themes[i].name : NULL;
}

const char *theme_about_at(int i)
{
	return i >= 0 && i < NTHEMES ? themes[i].about : NULL;
}

const char *theme_slot_name(int slot)
{
	return slot >= 0 && slot < VT_COLORS ? slot_names[slot] : NULL;
}

static int find_theme(const char *name)
{
	for (int i = 0; i < NTHEMES; i++)
		if (!strcmp(themes[i].name, name))
			return i;
	return -1;
}

int theme_use(const char *name)
{
	int i = find_theme(name);

	if (i < 0)
		return -ENOENT;
	cur.theme = i;
	cur.nown = 0;			/* your colours were for the old one */
	show();
	return 0;
}

int theme_set_mode(enum theme_mode mode, int day_from, int day_until)
{
	if (day_from < 0 || day_from >= 24 * 60 || day_until < 0 || day_until >= 24 * 60)
		return -EINVAL;
	cur.mode = mode;
	cur.day_from = day_from;
	cur.day_until = day_until;
	cur.light = want_light();
	show();
	return 0;
}

/* "fg", "bar", "red", "bright-red", or a number 0-15. */
static int slot_of(const char *name)
{
	char *end;
	long n = strtol(name, &end, 10);

	if (*name && !*end)
		return n >= 0 && n < 16 ? (int)n : -1;
	for (int i = 0; i < VT_COLORS; i++)
		if (!strcmp(slot_names[i], name))
			return i;
	return -1;
}

int theme_set_color(const char *slot, uint32_t rgb)
{
	int i = slot_of(slot), k;

	if (i < 0 || rgb > 0xffffff)
		return -EINVAL;
	for (k = 0; k < cur.nown && cur.own[k].slot != i; k++)
		;
	if (k == cur.nown) {
		if (cur.nown == MAX_OWN)
			return -ENOSPC;
		cur.nown++;
	}
	cur.own[k].slot = i;
	cur.own[k].rgb = rgb;
	show();
	return 0;
}

void theme_reset_colors(void)
{
	cur.nown = 0;
	show();
}

void theme_set_cursor(enum vt_cursor shape, int blink_ms)
{
	cur.cursor = shape;
	cur.blink_ms = blink_ms;
	vt_set_cursor(shape, blink_ms);
}

void theme_set_bar(bool top)
{
	cur.bar_top = top;
	vt_set_bar(top);
}

void theme_get(struct theme_state *st)
{
	*st = (struct theme_state) {
		.name = themes[cur.theme].name, .mode = cur.mode, .light = cur.light,
		.day_from = cur.day_from, .day_until = cur.day_until, .own = cur.nown,
		.cursor = cur.cursor, .blink_ms = cur.blink_ms, .bar_top = cur.bar_top,
	};
}

static const char *const cursor_names[] = { "block", "underline", "bar" };

int theme_save(void)
{
	char path[64];
	FILE *f;

	if (!mount_resolve(THEME_FILE, path, sizeof(path)) || !(f = fopen(path, "w")))
		return -EIO;
	fprintf(f, "# the screen's looks: `theme` writes this, and reads it at boot\n"
		"theme %s\n", themes[cur.theme].name);
	if (cur.mode == THEME_AUTO)
		fprintf(f, "mode auto %d:%02d %d:%02d\n", cur.day_from / 60, cur.day_from % 60,
			cur.day_until / 60, cur.day_until % 60);
	else
		fprintf(f, "mode %s\n", cur.mode == THEME_LIGHT ? "light" : "dark");
	fprintf(f, "cursor %s\nblink %d\nbar %s\n", cursor_names[cur.cursor], cur.blink_ms,
		cur.bar_top ? "top" : "bottom");
	for (int i = 0; i < cur.nown; i++)
		fprintf(f, "color %s #%06lx\n", slot_names[cur.own[i].slot],
			(unsigned long)cur.own[i].rgb);
	return fclose(f) ? -EIO : 0;
}

void theme_restore(void)
{
	char path[64], line[80], word[24];
	FILE *f;

	if (!mount_resolve(THEME_FILE, path, sizeof(path)) || !(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof(line), f)) {
		int h1, m1, h2, m2, v;
		unsigned rgb;

		if (sscanf(line, "theme %23s", word) == 1 && find_theme(word) >= 0) {
			cur.theme = find_theme(word);
		} else if (sscanf(line, "mode auto %d:%d %d:%d", &h1, &m1, &h2, &m2) == 4) {
			cur.mode = THEME_AUTO;
			cur.day_from = (h1 * 60 + m1) % (24 * 60);
			cur.day_until = (h2 * 60 + m2) % (24 * 60);
		} else if (sscanf(line, "mode %23s", word) == 1) {
			cur.mode = !strcmp(word, "light") ? THEME_LIGHT :
				   !strcmp(word, "auto") ? THEME_AUTO : THEME_DARK;
		} else if (sscanf(line, "cursor %23s", word) == 1) {
			for (int i = 0; i < 3; i++)
				if (!strcmp(word, cursor_names[i]))
					cur.cursor = i;
		} else if (sscanf(line, "blink %d", &v) == 1 && v >= 0 && v <= 2000) {
			cur.blink_ms = v;
		} else if (sscanf(line, "bar %23s", word) == 1) {
			cur.bar_top = strcmp(word, "bottom") != 0;
		} else if (sscanf(line, "color %23s #%x", word, &rgb) == 2 && slot_of(word) >= 0 &&
			   cur.nown < MAX_OWN) {
			cur.own[cur.nown].slot = slot_of(word);
			cur.own[cur.nown++].rgb = rgb & 0xffffff;
		}
	}
	fclose(f);
	cur.light = want_light();
	vt_set_cursor(cur.cursor, cur.blink_ms);
	vt_set_bar(cur.bar_top);
	show();
}
