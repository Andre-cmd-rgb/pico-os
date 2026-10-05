/*
 * LRC lyrics (lrc.h).
 *
 * A stamp with no words after it is not a line. LRC marks a pause with
 * one, and shown as an empty line (or the note so many players put there)
 * it is fine in a ballad's long break and awful in rap, where every breath
 * between bars becomes one and the words keep jumping. So such a stamp
 * only ends the line before it, which stays lit through the gap.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "lrc.h"
#include "pt/sys.h"

#define TAIL_MS		8000	/* how long the last line stays lit */

struct stamp {			/* while parsing: one stamp of one line */
	int32_t	 ms;
	int	 order;		/* in the file, so that sorting keeps it */
	const char *text;	/* NULL: a stamp with no words */
	int	 nwords;
	struct lrc_word *words;
};

/* "mm:ss", "mm:ss.xx" or "mm:ss:xxx" at s, inside its brackets: ms, or -1. */
static int32_t clock_ms(const char *s, const char *end)
{
	int32_t min = 0, sec = 0, frac = 0, scale = 1000;
	int digits = 0;

	for (; s < end && *s >= '0' && *s <= '9' && digits < 3; s++, digits++)
		min = min * 10 + (*s - '0');
	if (!digits || s >= end || *s++ != ':')
		return -1;
	for (digits = 0; s < end && *s >= '0' && *s <= '9' && digits < 2; s++, digits++)
		sec = sec * 10 + (*s - '0');
	if (!digits || sec > 59)
		return -1;
	if (s < end && (*s == '.' || *s == ':')) {
		for (s++, digits = 0; s < end && *s >= '0' && *s <= '9' && digits < 3; s++, digits++) {
			frac = frac * 10 + (*s - '0');
			scale /= 10;
		}
		if (!digits)
			return -1;
		frac *= scale;
	}
	return s == end ? min * 60000 + sec * 1000 + frac : -1;
}

/* A stamp in `open` `close` brackets at s: its ms and where it ends, or -1. */
static int32_t stamp_at(const char *s, const char *end, char open, char close, const char **after)
{
	const char *shut;

	if (s >= end || *s != open || !(shut = memchr(s, close, end - s)))
		return -1;
	*after = shut + 1;
	return clock_ms(s + 1, shut);
}

static bool tag_named(const char *s, const char *end, const char *name, const char **value)
{
	size_t n = strlen(name);

	if ((size_t)(end - s) < n + 3 || s[0] != '[' || end[-1] != ']' || s[n + 1] != ':')
		return false;
	for (size_t i = 0; i < n; i++)
		if ((s[1 + i] | 0x20) != name[i])
			return false;
	*value = s + n + 2;
	return true;
}

static int by_time(const void *pa, const void *pb)
{
	const struct stamp *a = pa, *b = pb;

	if (a->ms != b->ms)
		return a->ms < b->ms ? -1 : 1;
	return a->order - b->order;
}

/*
 * One line of the file: its stamps go into `stamps`, its words (the word
 * stamps taken out and the spaces made single) into `out`, and its word
 * timings into `words`. Returns how many stamps it had; 0 is plain text.
 */
static int parse_line(const char *s, const char *end, struct stamp *stamps, int order,
		      char **out, struct lrc_word **words)
{
	const char *p = s, *after, *text;
	int32_t ms;
	int n = 0, nw = 0, kept = 0, marks = 0;
	char *start = *out, *w = *out;
	struct lrc_word *first = *words;

	while ((ms = stamp_at(p, end, '[', ']', &after)) >= 0) {
		stamps[n++] = (struct stamp){ .ms = ms, .order = order };
		for (p = after; p < end && (*p == ' ' || *p == '\t'); p++)
			;
	}
	if (!n)
		return 0;
	text = p;
	for (const char *q = text; q < end; q++)
		marks += *q == '<' && stamp_at(q, end, '<', '>', &after) >= 0;
	for (p = text; p < end;) {
		if (*p == '<' && (ms = stamp_at(p, end, '<', '>', &after)) >= 0) {
			if (marks >= 2) {
				if (w > start && w[-1] != ' ')
					*w++ = ' ';	/* words apart, even where the file ran them on */
				first[nw++] = (struct lrc_word){ .ms = ms, .at = w - start };
			}
			p = after;
			continue;
		}
		if ((*p == ' ' || *p == '\t') && (w == start || w[-1] == ' ')) {
			p++;
			continue;
		}
		*w++ = *p == '\t' ? ' ' : *p;
		p++;
	}
	while (w > start && w[-1] == ' ')
		w--;
	*w++ = '\0';
	/* each word runs to the next one's start; a word that came out empty goes */
	for (int i = 0; i < nw; i++) {
		size_t to = i + 1 < nw ? first[i + 1].at : strlen(start);

		while (to > first[i].at && start[to - 1] == ' ')
			to--;
		if (to > first[i].at) {
			first[kept] = first[i];
			first[kept++].len = to - first[i].at;
		}
	}
	nw = kept;
	if (nw < 2)
		nw = 0;			/* one timed word is the line's own stamp again */
	for (int i = 0; i < n; i++) {
		stamps[i].text = *start ? start : NULL;
		stamps[i].nwords = nw;
		stamps[i].words = nw ? first : NULL;
	}
	*out = w;
	*words = first + nw;
	return n;
}

int lrc_parse(struct lrc *l, const char *text, size_t len)
{
	const char *end = text + len, *p;
	size_t brackets = 0, angles = 0, lines = 1, plain = 0;
	int32_t offset = 0;
	int nstamps = 0, usable = 0, order = 0;
	struct stamp *stamps;
	struct lrc_word *words, *first_word;
	char *out, *plain_out;
	void *mem;

	memset(l, 0, sizeof(*l));
	for (p = text; p < end; p++) {
		brackets += *p == '[';
		angles += *p == '<';
		lines += *p == '\n';
	}
	/* room for the most there could be: every bracket a stamp, every line
	 * plain, and a space put between each pair of run-on timed words */
	mem = pt_malloc(brackets * sizeof(struct stamp) + (brackets + lines) * sizeof(struct lrc_line) +
			angles * sizeof(struct lrc_word) + 2 * (len + lines + 1) + angles);
	if (!mem)
		return -ENOMEM;
	stamps = mem;
	l->lines = (struct lrc_line *)(stamps + brackets);
	first_word = words = (struct lrc_word *)(l->lines + brackets + lines);
	out = (char *)(words + angles);
	plain_out = out + len + lines + angles + 1;
	l->mem = mem;

	for (p = text; p < end;) {
		const char *eol = memchr(p, '\n', end - p), *s = p, *e, *value;
		int n;

		e = eol ? eol : end;
		p = eol ? eol + 1 : end;
		if (e - s >= 3 && !memcmp(s, "\xef\xbb\xbf", 3))
			s += 3;			/* a byte-order mark */
		while (s < e && (*s == ' ' || *s == '\t' || *s == '\r'))
			s++;
		while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
			e--;
		if (s == e)
			continue;
		if (tag_named(s, e, "offset", &value)) {
			offset = (int32_t)strtol(value, NULL, 10);
			continue;
		}
		if (tag_named(s, e, "ti", &value) || tag_named(s, e, "ar", &value) ||
		    tag_named(s, e, "al", &value) || tag_named(s, e, "by", &value) ||
		    tag_named(s, e, "length", &value) || tag_named(s, e, "re", &value) ||
		    tag_named(s, e, "ve", &value) || tag_named(s, e, "au", &value) ||
		    tag_named(s, e, "id", &value))
			continue;
		n = parse_line(s, e, stamps + nstamps, order++, &out, &words);
		if (n) {
			nstamps += n;
			usable += stamps[nstamps - 1].text != NULL;
			continue;
		}
		/* plain text, kept in case nothing turns out to be stamped */
		memcpy(plain_out, s, e - s);
		plain_out[e - s] = '\0';
		l->lines[brackets + plain++] = (struct lrc_line){ .ms = -1, .end = -1, .text = plain_out };
		plain_out += e - s + 1;
	}

	if (!usable) {
		/* stamps with no words around plain lyrics: show the words */
		memmove(l->lines, l->lines + brackets, plain * sizeof(*l->lines));
		l->n = plain;
		return 0;
	}
	l->synced = true;
	qsort(stamps, nstamps, sizeof(*stamps), by_time);
	for (int i = 0; i < nstamps; i++) {
		struct lrc_line *line;
		int32_t next = i + 1 < nstamps ? stamps[i + 1].ms : stamps[i].ms + TAIL_MS;

		if (!stamps[i].text)
			continue;
		line = &l->lines[l->n++];
		line->ms = stamps[i].ms - offset > 0 ? stamps[i].ms - offset : 0;
		line->end = next - offset > line->ms ? next - offset : line->ms;
		line->text = stamps[i].text;
		line->nwords = stamps[i].nwords;
		line->words = stamps[i].words;
	}
	/* each word belongs to one line, though a chorus's stamps share it: once each */
	for (struct lrc_word *w = first_word; w < words; w++)
		w->ms = w->ms - offset > 0 ? w->ms - offset : 0;
	return 0;
}

void lrc_free(struct lrc *l)
{
	pt_free(l->mem);
	memset(l, 0, sizeof(*l));
}

int lrc_line_at(const struct lrc *l, int32_t ms)
{
	int lo = 0, hi = l->n - 1, found = -1;

	if (!l->synced)
		return -1;
	ms += LRC_LEAD_MS;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (l->lines[mid].ms <= ms) {
			found = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return found;
}

int lrc_words_sung(const struct lrc_line *line, int32_t ms)
{
	int n = 0;

	if (!line->nwords)
		return -1;
	ms += LRC_LEAD_MS;
	while (n < line->nwords && line->words[n].ms <= ms)
		n++;
	return n;
}
