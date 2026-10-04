/*
 * cal - print months the way util-linux's does, weeks from Monday unless
 * -s says Sunday; a narrow screen gets two months to a row instead of
 * three.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dates.h"
#include "util.h"

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

PT_COMPLETE(cal, ": -m -s -3 -y\n")

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
