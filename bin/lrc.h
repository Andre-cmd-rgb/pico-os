/*
 * Lyrics in LRC, the way downloaded music carries them: each line stamped
 * "[01:02.50]", several stamps on one line for a chorus that comes back,
 * "[offset:+120]" moving them all, and the enhanced kind where each word
 * has a stamp of its own, "<01:02.80>" -- which is what lands a fast verse
 * on the beat. Lyrics with no stamps at all come out as plain lines.
 *
 * Plain C, so it builds on the PC too (tools/tags_test.c); it allocates
 * with pt_malloc.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LRC_LEAD_MS	120	/* a line lights this early: reaction time, and a frame */

struct lrc_word {
	int32_t	 ms;
	uint16_t at, len;	/* where it is in its line's text */
};

struct lrc_line {
	int32_t	 ms, end;	/* when it is sung, and until; -1 in plain lyrics */
	const char *text;	/* UTF-8, the stamps taken out */
	int	 nwords;	/* 0 when only the line has a stamp */
	const struct lrc_word *words;
};

struct lrc {
	bool	 synced;
	int	 n;
	struct lrc_line *lines;	/* in the order they are sung */
	void	*mem;		/* all of it, in one allocation */
};

/* 0 (n may be 0: nothing to show) or -ENOMEM. */
int	lrc_parse(struct lrc *l, const char *text, size_t len);
void	lrc_free(struct lrc *l);

/* The line being sung at `ms`, LRC_LEAD_MS early; -1 before the first. */
int	lrc_line_at(const struct lrc *l, int32_t ms);

/* How many of a line's words have been sung at `ms`; -1 if it has none timed. */
int	lrc_words_sung(const struct lrc_line *line, int32_t ms);
