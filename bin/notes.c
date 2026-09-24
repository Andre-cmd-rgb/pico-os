/*
 * notes - read school notes: Markdown laid out for the screen.
 *
 * The notes live in ~/notes, one folder per subject if you like, typed on a
 * PC as Markdown or plain text. `notes` offers them in the file list; a
 * note opens laid out for the width of the terminal -- headings stand out,
 * paragraphs are wrapped, lists hang from their bullets -- and the arrows
 * and the space bar move through it. `o` lists the headings to jump to, `/`
 * searches, `+` and `-` turn the backlight up and down for reading in the
 * dark, and the place you stopped in each note is remembered in
 * ~/.notes_pos for next time.
 *
 * The screen has one hue at several strengths, so the styles are
 * brightnesses: headings and bold brightest, italics and bullets a paler
 * step, code in the theme's warning colour. The same escapes give colours
 * on a PC's terminal over the serial console.
 *
 * The renderer is plain C over the pt_ calls, so it builds on the PC too
 * (tools/host_pt.c), where `notes -p` is tested.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#ifdef ESP_PLATFORM
#include "drivers/drivers.h"
#endif

#define MAX_FILE	(1 << 20)	/* a megabyte of notes is a lot of notes */
#define POS_FILE	".notes_pos"
#define POS_KEEP	200		/* notes whose place is remembered */
#define TAB_WIDTH	4

enum style {
	S_TEXT, S_BOLD, S_ITALIC, S_BOLD_ITALIC, S_CODE, S_H1, S_H2, S_H3,
	S_MARK, S_BAR, S_DIM, S_COUNT
};
#define S_FOUND		0x80		/* a search match: drawn inverted */

static const char *const sgr[S_COUNT] = {
	[S_TEXT] = "0", [S_BOLD] = "0;1", [S_ITALIC] = "0;33", [S_BOLD_ITALIC] = "0;1;33",
	[S_CODE] = "0;31", [S_H1] = "0;1;97", [S_H2] = "0;1;33", [S_H3] = "0;33",
	[S_MARK] = "0;33", [S_BAR] = "0;90", [S_DIM] = "0;2",
};

/*
 * A laid-out note: every screen line's text and a style for each of its
 * bytes, kept end to end in two arenas, and the headings for the outline.
 */
struct line {
	size_t	off;			/* into text and style */
	int	len;			/* bytes */
	int	src;			/* source line, for remembering places */
	int	head;			/* the heading it is under, or -1 */
};

struct heading {
	int	level, line;
	char	title[64];
};

struct doc {
	char		*text;
	uint8_t		*style;
	size_t		 used, cap;
	struct line	*lines;
	int		 n, ncap;
	struct heading	*heads;
	int		 nheads, hcap;
	int		 width;
	bool		 blank;		/* the last line out was empty */
};

/* A run of styled text on its way to being wrapped. */
struct span {
	char	*text;
	uint8_t	*style;
	size_t	 len, cap;
};

/* ------------------------------------------------------------ building */

static bool grow(void **p, size_t *cap, size_t want, size_t each)
{
	size_t n = *cap ? *cap : 64;
	void *q;

	if (want <= *cap)
		return true;
	while (n < want)
		n *= 2;
	q = pt_realloc(*p, n * each);
	if (!q)
		return false;
	*p = q;
	*cap = n;
	return true;
}

static void span_add(struct span *s, const char *text, size_t n, uint8_t style)
{
	size_t cap = s->cap;

	if (!n || !grow((void **)&s->text, &cap, s->len + n, 1))
		return;
	if (cap != s->cap) {
		uint8_t *st = pt_realloc(s->style, cap);

		if (!st)
			return;
		s->style = st;
		s->cap = cap;
	}
	memcpy(s->text + s->len, text, n);
	memset(s->style + s->len, style, n);
	s->len += n;
}

/* Start a new screen line and give it `n` bytes of text in one style. */
static struct line *line_new(struct doc *d, int src)
{
	size_t cap = d->ncap;
	struct line *l;

	if (!grow((void **)&d->lines, &cap, d->n + 1, sizeof(*d->lines)))
		return NULL;
	d->ncap = cap;
	l = &d->lines[d->n++];
	l->off = d->used;
	l->len = 0;
	l->src = src;
	l->head = d->nheads - 1;
	d->blank = false;
	return l;
}

static void line_add(struct doc *d, struct line *l, const char *text, const uint8_t *style,
		     uint8_t one, size_t n)
{
	size_t cap = d->cap;

	if (!l || !n || !grow((void **)&d->text, &cap, d->used + n, 1))
		return;
	if (cap != d->cap) {
		uint8_t *st = pt_realloc(d->style, cap);

		if (!st)
			return;
		d->style = st;
		d->cap = cap;
	}
	memcpy(d->text + d->used, text, n);
	if (style)
		memcpy(d->style + d->used, style, n);
	else
		memset(d->style + d->used, one, n);
	d->used += n;
	l->len += n;
}

/* One empty line between blocks, however many the note had. */
static void gap(struct doc *d, int src)
{
	if (d->n && !d->blank) {
		line_new(d, src);
		d->blank = true;
	}
}

/* Characters on the screen: UTF-8 continuation bytes take no room. */
static int width_of(const char *s, size_t n)
{
	int w = 0;

	for (size_t i = 0; i < n; i++)
		w += ((unsigned char)s[i] & 0xc0) != 0x80;
	return w;
}

/* How many bytes of s make up at most `cols` characters. */
static size_t bytes_for(const char *s, size_t n, int cols)
{
	size_t i = 0;

	while (i < n && cols > 0) {
		i++;
		while (i < n && ((unsigned char)s[i] & 0xc0) == 0x80)
			i++;
		cols--;
	}
	return i;
}

/* A new line with its prefix; after the first, the prefix is `rest`. */
static struct line *start(struct doc *d, const char **first, const char *rest, uint8_t pstyle,
			  int src)
{
	struct line *l = line_new(d, src);

	line_add(d, l, *first, NULL, pstyle, strlen(*first));
	*first = rest;
	return l;
}

/*
 * Lay a span out in lines of the document's width: `first` goes in front
 * of the first line and `rest` in front of the others, in style `pstyle`,
 * so a bullet hangs and a quote keeps its bar. A '\n' in the span ends a
 * line where the note asked for it; a word too long for a line is cut.
 */
static void wrap(struct doc *d, const struct span *s, const char *first, const char *rest,
		 uint8_t pstyle, int src)
{
	int avail = d->width - width_of(first, strlen(first));
	struct line *l = NULL;
	uint8_t between = S_TEXT;	/* the style of the space before a word */
	int used = 0;
	size_t i = 0;

	if (avail < 8)
		avail = 8;
	while (i < s->len) {
		size_t j;

		if (s->text[i] == '\n') {		/* a break the note asked for */
			if (!l)
				start(d, &first, rest, pstyle, src);
			l = NULL;
			i++;
			continue;
		}
		if (s->text[i] == ' ') {
			between = s->style[i++];
			continue;
		}
		for (j = i; j < s->len && s->text[j] != ' ' && s->text[j] != '\n'; j++)
			;
		while (i < j) {
			int w = width_of(s->text + i, j - i);

			if (l && used + 1 + w > avail) {
				l = NULL;		/* no room: the word starts a line */
				continue;
			}
			if (!l) {
				l = start(d, &first, rest, pstyle, src);
				used = 0;
			} else {
				line_add(d, l, " ", NULL, between, 1);
				used++;
			}
			if (w > avail) {		/* longer than a line: cut it */
				size_t k = bytes_for(s->text + i, j - i, avail - used);

				line_add(d, l, s->text + i, s->style + i, 0, k);
				used += width_of(s->text + i, k);
				i += k;
				l = NULL;
				continue;
			}
			line_add(d, l, s->text + i, s->style + i, 0, j - i);
			used += w;
			i = j;
		}
	}
}

/* A line kept as it is (code, a table): cut to the width, never reflowed. */
static void verbatim(struct doc *d, const char *text, size_t n, uint8_t style, const char *indent,
		     int src)
{
	char buf[256];
	size_t k = 0;
	int avail = d->width - (int)strlen(indent);

	for (size_t i = 0; i < n && k < sizeof(buf) - TAB_WIDTH; i++) {	/* tabs to spaces */
		if (text[i] == '\t')
			do buf[k++] = ' '; while (k % TAB_WIDTH);
		else
			buf[k++] = text[i];
	}
	do {
		size_t take = bytes_for(buf, k, avail);
		struct line *l = line_new(d, src);

		line_add(d, l, indent, NULL, S_TEXT, strlen(indent));
		line_add(d, l, buf, NULL, style, take);
		memmove(buf, buf + take, k - take);
		k -= take;
	} while (k);
}

/* ------------------------------------------------------------ Markdown inline */

static bool is_alnum(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (unsigned char)c >= 0x80;
}

static const char *find(const char *s, const char *end, const char *what)
{
	size_t n = strlen(what);

	for (; s + n <= end; s++)
		if (!memcmp(s, what, n))
			return s;
	return NULL;
}

static uint8_t emphasis(uint8_t base, bool bold, bool italic, bool strike)
{
	if (base != S_TEXT)
		return base;			/* headings keep their own look */
	if (strike)
		return S_DIM;
	if (bold && italic)
		return S_BOLD_ITALIC;
	return bold ? S_BOLD : italic ? S_ITALIC : S_TEXT;
}

/*
 * The inline part of Markdown, into a span: **bold**, *italic*, `code`,
 * ~~struck~~, links as their text, pictures as [their alt text], and
 * backslash escapes. A marker counts only if its partner comes later in
 * the paragraph, so a lone * in "2 * 3" stays a star.
 */
static void inline_md(struct span *s, const char *p, const char *end, uint8_t base)
{
	const char *begin = p;
	bool bold = false, italic = false, strike = false;

	while (p < end) {
		char c = *p;
		const char *q;

		if (c == '\\' && p + 1 < end && strchr("\\`*_{}[]()#+-.!|~>", p[1])) {
			span_add(s, p + 1, 1, emphasis(base, bold, italic, strike));
			p += 2;
		} else if (c == '`') {
			int ticks = 0;
			char fence[8] = { 0 };

			while (p + ticks < end && p[ticks] == '`' && ticks < 7)
				ticks++;
			memset(fence, '`', ticks);
			q = find(p + ticks, end, fence);
			if (!q) {
				span_add(s, p, ticks, emphasis(base, bold, italic, strike));
				p += ticks;
				continue;
			}
			span_add(s, p + ticks, q - p - ticks, base == S_TEXT ? S_CODE : base);
			p = q + ticks;
		} else if ((c == '*' || c == '_') && p + 1 < end && p[1] == c) {
			char two[3] = { c, c, 0 };

			if (bold || find(p + 2, end, two)) {
				bold = !bold;
				p += 2;
			} else {
				span_add(s, p, 2, emphasis(base, bold, italic, strike));
				p += 2;
			}
		} else if (c == '*' || c == '_') {
			char one[2] = { c, 0 };
			bool opens = p + 1 < end && p[1] != ' ' && find(p + 1, end, one);
			/* snake_case is not emphasis: an _ may not open after a
			 * letter, nor close before one */
			bool inword = c == '_' && (italic ? p + 1 < end && is_alnum(p[1]) :
						    p > begin && is_alnum(p[-1]));

			if (!inword && (italic || opens))
				italic = !italic;
			else
				span_add(s, p, 1, emphasis(base, bold, italic, strike));
			p++;
		} else if (c == '~' && p + 1 < end && p[1] == '~') {
			if (strike || find(p + 2, end, "~~"))
				strike = !strike;
			else
				span_add(s, p, 2, emphasis(base, bold, italic, strike));
			p += 2;
		} else if ((c == '[' || (c == '!' && p + 1 < end && p[1] == '[')) &&
			   (q = find(p, end, "](")) && memchr(q, ')', end - q)) {
			const char *text = p + (c == '!' ? 2 : 1);
			const char *close = memchr(q, ')', end - q);

			if (c == '!') {
				span_add(s, "[", 1, S_DIM);
				span_add(s, text, q - text, S_DIM);
				span_add(s, "]", 1, S_DIM);
			} else {
				inline_md(s, text, q, emphasis(base, bold, italic, strike));
			}
			p = close + 1;
		} else if (c == '<' && (q = memchr(p, '>', end - p)) &&
			   (!strncmp(p, "<http", 5) || !strncmp(p, "<br", 3))) {
			if (p[1] == 'b')
				span_add(s, "\n", 1, S_TEXT);
			else
				span_add(s, p + 1, q - p - 1, emphasis(base, bold, italic, strike));
			p = q + 1;
		} else {
			span_add(s, p, 1, emphasis(base, bold, italic, strike));
			p++;
		}
	}
}

/* ------------------------------------------------------------ Markdown blocks */

static const char *skip_spaces(const char *p, const char *end, int *n)
{
	int k = 0;

	while (p < end && (*p == ' ' || *p == '\t')) {
		k += *p == '\t' ? TAB_WIDTH : 1;
		p++;
	}
	if (n)
		*n = k;
	return p;
}

static bool blank(const char *p, const char *end)
{
	return skip_spaces(p, end, NULL) == end;
}

/* "---", "***", "___", three or more, spaces allowed between. */
static bool is_rule(const char *p, const char *end)
{
	char c = 0;
	int count = 0;

	for (p = skip_spaces(p, end, NULL); p < end; p++) {
		if (*p == ' ')
			continue;
		if (*p != '-' && *p != '*' && *p != '_')
			return false;
		if (c && *p != c)
			return false;
		c = *p;
		count++;
	}
	return count >= 3;
}

static bool is_fence(const char *p, const char *end)
{
	p = skip_spaces(p, end, NULL);
	return end - p >= 3 && (!strncmp(p, "```", 3) || !strncmp(p, "~~~", 3));
}

/* A list item: returns where its text starts and writes its marker. */
static const char *list_item(const char *p, const char *end, int *indent, char *marker)
{
	const char *q = skip_spaces(p, end, indent);

	if (q < end && strchr("-*+", *q) && q + 1 < end && q[1] == ' ') {
		strcpy(marker, "\xe2\x80\xa2");			/* • */
		return skip_spaces(q + 1, end, NULL);
	}
	const char *r = q;

	while (r < end && *r >= '0' && *r <= '9' && r - q < 4)
		r++;
	if (r > q && r + 1 < end && (*r == '.' || *r == ')') && r[1] == ' ') {
		snprintf(marker, 8, "%.*s.", (int)(r - q), q);
		return skip_spaces(r + 1, end, NULL);
	}
	return NULL;
}

/* A rule the width of the screen: under a title, or for a "---". */
static void rule(struct doc *d, int src)
{
	struct line *l = line_new(d, src);

	for (int i = 0; i < d->width; i++)
		line_add(d, l, "\xe2\x80\x94", NULL, S_BAR, 3);	/* — */
}

static void add_heading(struct doc *d, int level, const struct span *s)
{
	size_t cap = d->hcap;
	struct heading *h;
	size_t n = s->len < sizeof(h->title) - 1 ? s->len : sizeof(h->title) - 1;

	if (!grow((void **)&d->heads, &cap, d->nheads + 1, sizeof(*d->heads)))
		return;
	d->hcap = cap;
	h = &d->heads[d->nheads++];
	h->level = level;
	h->line = d->n;
	while (n < s->len && n && ((unsigned char)s->text[n] & 0xc0) == 0x80)
		n--;				/* whole characters only */
	memcpy(h->title, s->text, n);
	h->title[n] = '\0';
	for (char *c = h->title; *c; c++)
		if (*c == '\n')
			*c = ' ';
}

static void heading(struct doc *d, int level, const char *p, const char *end, int src)
{
	static const uint8_t look[] = { S_H1, S_H1, S_H2, S_H3, S_H3, S_H3, S_H3 };
	struct span s = { 0 };

	while (end > p && (end[-1] == ' ' || end[-1] == '#'))
		end--;				/* "## Title ##" */
	inline_md(&s, p, end, look[level]);
	gap(d, src);
	add_heading(d, level, &s);
	wrap(d, &s, "", "", S_TEXT, src);
	if (level == 1)
		rule(d, src);			/* under the title */
	pt_free(s.text);
	pt_free(s.style);
	d->blank = false;
}

struct para {
	struct span	 s;
	int		 src;
	int		 kind;		/* 0 text, 1 list item, 2 quote */
	int		 indent;
	char		 marker[8];
	bool		 hard;		/* the last line ended in a line break */
};

static void para_flush(struct doc *d, struct para *pa)
{
	char first[40], rest[40];
	uint8_t pstyle = S_MARK;

	if (!pa->s.len && pa->kind != 1) {
		pa->kind = 0;
		return;
	}
	if (pa->kind == 1) {
		int pad = pa->indent / 2 * 2;
		int mw = width_of(pa->marker, strlen(pa->marker));

		if (pad > 12)
			pad = 12;
		snprintf(first, sizeof(first), "%*s%s ", pad, "", pa->marker);
		snprintf(rest, sizeof(rest), "%*s", pad + mw + 1, "");
	} else if (pa->kind == 2) {
		strcpy(first, "| ");
		strcpy(rest, "| ");
		pstyle = S_BAR;
	} else {
		first[0] = rest[0] = '\0';
		gap(d, pa->src);
	}
	wrap(d, &pa->s, first, rest, pstyle, pa->src);
	pa->s.len = 0;
	pa->kind = 0;
	pa->hard = false;
}

/* A line of paragraph text joins the one before it with a space, unless
 * that one ended in two spaces or a backslash: then it is a new line. */
static void para_add(struct para *pa, const char *p, const char *end)
{
	if (pa->s.len)
		span_add(&pa->s, pa->hard ? "\n" : " ", 1, S_TEXT);
	pa->hard = false;
	if (end - p >= 2 && end[-1] == ' ' && end[-2] == ' ') {
		pa->hard = true;
	} else if (end > p && end[-1] == '\\') {
		pa->hard = true;
		end--;
	}
	while (end > p && end[-1] == ' ')
		end--;
	inline_md(&pa->s, p, end, S_TEXT);
}

static void markdown(struct doc *d, const char *text, size_t size)
{
	const char *end = text + size, *p = text;
	struct para pa = { 0 };
	bool in_code = false, in_list = false, in_table = false;
	int src = 0;

	while (p < end) {
		const char *eol = memchr(p, '\n', end - p), *q;
		const char *next = eol ? eol + 1 : end;
		char marker[8];
		int indent, level;

		if (!eol)
			eol = end;
		if (eol > p && eol[-1] == '\r')
			eol--;
		src++;

		if (in_code) {
			if (is_fence(p, eol)) {
				in_code = false;
				gap(d, src);
			} else {
				verbatim(d, p, eol - p, S_CODE, "  ", src);
			}
			p = next;
			continue;
		}
		if (is_fence(p, eol)) {
			para_flush(d, &pa);
			in_list = false;
			gap(d, src);
			in_code = true;
			p = next;
			continue;
		}
		if (blank(p, eol)) {
			para_flush(d, &pa);
			if (in_list || in_table)
				gap(d, src);
			in_list = in_table = false;
			p = next;
			continue;
		}
		q = skip_spaces(p, eol, &indent);
		/* setext: a line of = or - under a paragraph makes it a heading */
		if (pa.kind == 0 && pa.s.len && indent < 4 && (*q == '=' || *q == '-')) {
			const char *r = q;

			while (r < eol && *r == *q)
				r++;
			if (blank(r, eol)) {
				struct span s = pa.s;

				gap(d, pa.src);
				add_heading(d, *q == '=' ? 1 : 2, &s);
				for (size_t i = 0; i < s.len; i++)
					if (s.style[i] == S_TEXT)
						s.style[i] = *q == '=' ? S_H1 : S_H2;
				wrap(d, &s, "", "", S_TEXT, pa.src);
				if (*q == '=')
					rule(d, pa.src);
				pa.s.len = 0;
				pa.hard = false;
				p = next;
				continue;
			}
		}
		if (indent < 4 && *q == '#') {
			level = 0;
			while (q + level < eol && q[level] == '#' && level < 7)
				level++;
			if (level <= 6 && (q + level == eol || q[level] == ' ')) {
				para_flush(d, &pa);
				in_list = false;
				heading(d, level, skip_spaces(q + level, eol, NULL), eol, src);
				p = next;
				continue;
			}
		}
		if (is_rule(p, eol) && !(pa.s.len && *q == '-')) {
			para_flush(d, &pa);
			in_list = false;
			gap(d, src);
			rule(d, src);
			gap(d, src);
			p = next;
			continue;
		}
		if (*q == '>') {
			if (pa.kind != 2)
				para_flush(d, &pa);
			if (pa.kind != 2)
				gap(d, src);
			pa.kind = 2;
			pa.src = pa.s.len ? pa.src : src;
			q++;
			while (q < eol && (*q == '>' || *q == ' '))
				q++;
			para_add(&pa, q, eol);
			p = next;
			continue;
		}
		if (*q == '|') {			/* a table row, kept as typed */
			const char *r = q;
			bool rule = true;

			para_flush(d, &pa);
			in_list = false;
			if (!in_table)
				gap(d, src);
			in_table = true;
			for (; r < eol; r++)
				if (!strchr("|-:+ ", *r))
					rule = false;
			while (eol > q && eol[-1] == ' ')
				eol--;
			verbatim(d, q, eol - q, rule ? S_DIM : S_TEXT, "", src);
			p = next;
			continue;
		}
		if ((q = list_item(p, eol, &indent, marker))) {
			para_flush(d, &pa);
			if (!in_list)
				gap(d, src);
			in_list = true;
			pa.kind = 1;
			pa.indent = indent;
			pa.src = src;
			strcpy(pa.marker, marker);
			para_add(&pa, q, eol);
			p = next;
			continue;
		}
		/* An indented or lazy line carries on the item or paragraph. */
		if (!pa.s.len && pa.kind == 0)
			pa.src = src;
		if (in_list && pa.kind != 1) {
			in_list = false;
			gap(d, src);
		}
		para_add(&pa, skip_spaces(p, eol, NULL), eol);
		p = next;
	}
	para_flush(d, &pa);
	pt_free(pa.s.text);
	pt_free(pa.s.style);
}

/* Plain text: each line wrapped as it is, its indent kept for the lines after. */
static void plain(struct doc *d, const char *text, size_t size)
{
	const char *end = text + size, *p = text;
	int src = 0;

	while (p < end) {
		const char *eol = memchr(p, '\n', end - p);
		const char *next = eol ? eol + 1 : end;
		struct span s = { 0 };
		char pad[40];
		int indent;
		const char *q;

		if (!eol)
			eol = end;
		if (eol > p && eol[-1] == '\r')
			eol--;
		src++;
		if (blank(p, eol)) {
			line_new(d, src);
			d->blank = true;
			p = next;
			continue;
		}
		q = skip_spaces(p, eol, &indent);
		if (indent > 16)
			indent = 16;
		snprintf(pad, sizeof(pad), "%*s", indent, "");
		span_add(&s, q, eol - q, S_TEXT);
		wrap(d, &s, pad, pad, S_TEXT, src);
		pt_free(s.text);
		pt_free(s.style);
		p = next;
	}
}

static bool ends_with_ci(const char *name, const char *ext)
{
	size_t n = strlen(name), m = strlen(ext);

	if (n < m)
		return false;
	for (size_t i = 0; i < m; i++) {
		char c = name[n - m + i];

		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c != ext[i])
			return false;
	}
	return true;
}

static void doc_free(struct doc *d)
{
	pt_free(d->text);
	pt_free(d->style);
	pt_free(d->lines);
	pt_free(d->heads);
	memset(d, 0, sizeof(*d));
}

/* Read a note and lay it out `width` characters wide. */
static int doc_load(struct doc *d, const char *path, int width)
{
	int fd = pt_open(path, O_RDONLY);
	char *buf;
	ssize_t got;
	size_t size = 0;

	memset(d, 0, sizeof(*d));
	d->width = width;
	if (fd < 0)
		return fd;
	buf = pt_malloc(MAX_FILE);
	if (!buf) {
		pt_close(fd);
		return -ENOMEM;
	}
	while (size < MAX_FILE && (got = pt_read(fd, buf + size, MAX_FILE - size)) > 0)
		size += got;
	pt_close(fd);
	if (size >= 3 && !memcmp(buf, "\xef\xbb\xbf", 3)) {	/* a byte-order mark */
		memmove(buf, buf + 3, size - 3);
		size -= 3;
	}
	if (ends_with_ci(path, ".txt"))
		plain(d, buf, size);
	else
		markdown(d, buf, size);
	while (d->n && d->lines[d->n - 1].len == 0)
		d->n--;				/* no empty lines at the end */
	pt_free(buf);
	if (!d->n)
		line_new(d, 1);
	return 0;
}

/* ------------------------------------------------------------ printing */

struct out {
	char	*buf;
	size_t	 len, cap;
};

static void out_add(struct out *o, const char *s, size_t n)
{
	if (!n || !grow((void **)&o->buf, &o->cap, o->len + n, 1))
		return;
	memcpy(o->buf + o->len, s, n);
	o->len += n;
}

static void out_str(struct out *o, const char *s)
{
	out_add(o, s, strlen(s));
}


static void print_doc(const struct doc *d, bool color)
{
	struct out o = { 0 };
	char esc[16];

	for (int i = 0; i < d->n; i++) {
		const struct line *l = &d->lines[i];
		int cur = -1;

		for (int k = 0; k < l->len; k++) {
			int st = d->style[l->off + k];

			if (color && st != cur) {
				snprintf(esc, sizeof(esc), "\x1b[%sm", sgr[st]);
				out_str(&o, esc);
				cur = st;
			}
			out_add(&o, d->text + l->off + k, 1);
		}
		out_str(&o, color ? "\x1b[0m\n" : "\n");
	}
	write_all(PT_STDOUT, o.buf, o.len);
	pt_free(o.buf);
}

/* ------------------------------------------------------------ the reader */

static char lower(char c)
{
	return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

/* Where `query` is in the line, from byte `from`; smart case, as in vim. */
static int match(const struct doc *d, int line, const char *query, int from)
{
	const struct line *l = &d->lines[line];
	const char *t = d->text + l->off;
	size_t n = strlen(query);
	bool fold = true;

	for (const char *q = query; *q; q++)
		if (*q >= 'A' && *q <= 'Z')
			fold = false;
	for (int i = from; i + (int)n <= l->len; i++) {
		size_t k = 0;

		while (k < n && (fold ? lower(t[i + k]) : t[i + k]) == query[k])
			k++;
		if (k == n)
			return i;
	}
	return -1;
}

struct view {
	struct doc	 d;
	const char	*path, *name;
	int		 top, rows, cols, body;
	char		 query[48];
	char		 note[64];	/* a word on the status line, once */
};

static void draw_line(struct view *v, struct out *o, int i)
{
	const struct line *l = &v->d.lines[i];
	uint8_t marks[1024];
	int cur = -1, n = l->len < (int)sizeof(marks) ? l->len : (int)sizeof(marks);
	char esc[24];

	memset(marks, 0, n);
	if (v->query[0]) {
		int qn = strlen(v->query);

		for (int at = match(&v->d, i, v->query, 0); at >= 0 && at < n;
		     at = match(&v->d, i, v->query, at + 1))
			memset(marks + at, S_FOUND, at + qn <= n ? qn : n - at);
	}
	for (int k = 0; k < n; k++) {
		int st = v->d.style[l->off + k] | marks[k];

		if (st != cur) {
			snprintf(esc, sizeof(esc), "\x1b[%s%sm", sgr[st & 0x7f], st & S_FOUND ? ";7" : "");
			out_str(o, esc);
			cur = st;
		}
		out_add(o, v->d.text + l->off + k, 1);
	}
	/* A line that fills the row leaves the cursor on its last character,
	 * which an erase would take with it. */
	out_str(o, width_of(v->d.text + l->off, n) < v->cols ? "\x1b[0m\x1b[K" : "\x1b[0m");
}

static void draw_status(struct view *v, struct out *o)
{
	char left[128], right[16];
	const struct line *l = &v->d.lines[v->top];
	int max = v->d.n - v->body;
	int w;

	if (v->d.n <= v->body)
		strcpy(right, "all");
	else if (v->top <= 0)
		strcpy(right, "top");
	else if (v->top >= max)
		strcpy(right, "end");
	else
		snprintf(right, sizeof(right), "%d%%", v->top * 100 / max);
	if (v->note[0])
		snprintf(left, sizeof(left), " %s", v->note);
	else if (l->head >= 0)
		snprintf(left, sizeof(left), " %s \xc2\xb7 %s", v->name, v->d.heads[l->head].title);
	else
		snprintf(left, sizeof(left), " %s", v->name);
	v->note[0] = '\0';
	w = v->cols - (int)strlen(right) - 2;
	out_str(o, "\x1b[0;7m");
	out_add(o, left, bytes_for(left, strlen(left), w));
	for (int pad = w - width_of(left, bytes_for(left, strlen(left), w)); pad > 0; pad--)
		out_add(o, " ", 1);
	out_str(o, " ");
	out_str(o, right);
	out_str(o, " \x1b[0m");
}

static void draw(struct view *v)
{
	struct out o = { 0 };
	char at[24];

	for (int r = 0; r < v->body; r++) {
		snprintf(at, sizeof(at), "\x1b[%d;1H", r + 1);
		out_str(&o, at);
		if (v->top + r < v->d.n)
			draw_line(v, &o, v->top + r);
		else
			out_str(&o, "\x1b[0m\x1b[K");
	}
	snprintf(at, sizeof(at), "\x1b[%d;1H", v->rows);
	out_str(&o, at);
	draw_status(v, &o);
	write_all(PT_STDOUT, o.buf, o.len);
	pt_free(o.buf);
}

static void clamp(struct view *v)
{
	if (v->top > v->d.n - v->body)
		v->top = v->d.n - v->body;
	if (v->top < 0)
		v->top = 0;
}

/* The headings, to pick one and go there. */
static void outline(struct view *v)
{
	struct doc *d = &v->d;
	int sel = 0, top = 0;

	if (!d->nheads) {
		strcpy(v->note, "no headings in this note");
		return;
	}
	for (int h = 0; h < d->nheads; h++)
		if (d->heads[h].line <= v->top + 1)
			sel = h;
	for (;;) {
		struct out o = { 0 };
		char at[24];

		if (sel < top)
			top = sel;
		if (sel >= top + v->body)
			top = sel - v->body + 1;
		for (int r = 0; r < v->body; r++) {
			int h = top + r;

			snprintf(at, sizeof(at), "\x1b[%d;1H", r + 1);
			out_str(&o, at);
			if (h < d->nheads) {
				int pad = (d->heads[h].level - 1) * 2;
				const char *t = d->heads[h].title;

				out_str(&o, h == sel ? "\x1b[0;7m" : d->heads[h].level == 1 ? "\x1b[0;1m" : "\x1b[0m");
				for (int k = 0; k < pad && k < 10; k++)
					out_add(&o, " ", 1);
				out_add(&o, t, bytes_for(t, strlen(t), v->cols - pad));
			}
			out_str(&o, "\x1b[0m\x1b[K");
		}
		snprintf(at, sizeof(at), "\x1b[%d;1H", v->rows);
		out_str(&o, at);
		out_str(&o, "\x1b[0;7m outline: enter goes there, esc goes back\x1b[0m\x1b[K");
		write_all(PT_STDOUT, o.buf, o.len);
		pt_free(o.buf);

		switch (pt_readkey(PT_STDIN)) {
		case PT_KEY_UP: case 'k':	sel = sel ? sel - 1 : 0; break;
		case PT_KEY_DOWN: case 'j':	sel = sel < d->nheads - 1 ? sel + 1 : sel; break;
		case PT_KEY_PGUP:		sel = sel > v->body ? sel - v->body : 0; break;
		case PT_KEY_PGDN:
			sel = sel + v->body < d->nheads ? sel + v->body : d->nheads - 1;
			break;
		case '\r': case '\n': case PT_KEY_RIGHT:
			v->top = d->heads[sel].line;
			/* a blank line above a heading reads better than none */
			if (v->top > 0 && !d->lines[v->top - 1].len)
				v->top--;
			clamp(v);
			return;
		case PT_KEY_EOF: case PT_KEY_ERROR:
		case PT_KEY_ESC: case PT_CTRL('c'): case 'q': case 'o': case '\t':
			return;
		}
	}
}

/* A line of input on the status line: the search. */
static bool ask(struct view *v, const char *prompt, char *buf, size_t size)
{
	size_t n = 0;
	char at[24];

	buf[0] = '\0';
	for (;;) {
		struct out o = { 0 };
		int key;

		snprintf(at, sizeof(at), "\x1b[%d;1H", v->rows);
		out_str(&o, at);
		out_str(&o, "\x1b[0m");
		out_str(&o, prompt);
		out_str(&o, buf);
		out_str(&o, "\x1b[K\x1b[?25h");
		write_all(PT_STDOUT, o.buf, o.len);
		pt_free(o.buf);
		key = pt_readkey(PT_STDIN);
		pt_puts("\x1b[?25l");
		if (key == '\r' || key == '\n')
			return n > 0;
		if (key == PT_KEY_ESC || key == PT_CTRL('c') || key < 0)
			return false;
		if (key == 0x7f || key == PT_CTRL('h')) {
			while (n && ((unsigned char)buf[n - 1] & 0xc0) == 0x80)
				n--;		/* the rest of a UTF-8 character */
			if (n)
				n--;
			buf[n] = '\0';
		} else if (key >= ' ' && key < 0x100 && n + 1 < size) {
			buf[n++] = key;
			buf[n] = '\0';
		}
	}
}

static void search(struct view *v, int dir)
{
	int from = v->top + (dir > 0 ? 1 : -1);

	if (!v->query[0])
		return;
	for (int k = 0; k < v->d.n; k++) {
		int i = ((from + dir * k) % v->d.n + v->d.n) % v->d.n;

		if (match(&v->d, i, v->query, 0) >= 0) {
			v->top = i > 2 ? i - 2 : 0;	/* a little of what leads up to it */
			clamp(v);
			return;
		}
	}
	snprintf(v->note, sizeof(v->note), "\"%.32s\" is not in this note", v->query);
}

/* ------------------------------------------------------------ places */

static bool pos_path(char *out, size_t size)
{
	const char *home = pt_getenv("HOME");

	return home && join_path(home, POS_FILE, out, size);
}

/* The source line this note was left at, or 0. */
static int pos_load(const char *path)
{
	char file[PT_PATH_MAX], line[PT_PATH_MAX + 16];
	int fd, at = 0;
	struct lines ls;
	char *l;
	size_t n;

	if (!pos_path(file, sizeof(file)) || (fd = pt_open(file, O_RDONLY)) < 0)
		return 0;
	lines_init(&ls, fd);
	while ((l = lines_next(&ls, &n))) {
		char *sp;

		while (n && (l[n - 1] == '\n' || l[n - 1] == '\r'))
			n--;			/* lines_next keeps the newline */
		sp = memchr(l, ' ', n);
		if (!sp || n >= sizeof(line))
			continue;
		memcpy(line, l, n);
		line[n] = '\0';
		if (!strcmp(line + (sp - l) + 1, path))
			at = atoi(line);
	}
	lines_free(&ls);
	pt_close(fd);
	return at;
}

/* Remember where this note was left: newest first, the oldest dropped. */
static void pos_save(const char *path, int src)
{
	char file[PT_PATH_MAX], tmp[PT_PATH_MAX + 8];
	int in, out, kept = 1;
	struct lines ls;
	char *l;
	size_t n, plen = strlen(path);

	if (!pos_path(file, sizeof(file)))
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", file);
	if ((out = pt_open(tmp, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
		return;
	pt_dprintf(out, "%d %s\n", src, path);
	if ((in = pt_open(file, O_RDONLY)) >= 0) {
		lines_init(&ls, in);
		while ((l = lines_next(&ls, &n)) && kept < POS_KEEP) {
			char *sp;

			while (n && (l[n - 1] == '\n' || l[n - 1] == '\r'))
				n--;
			sp = memchr(l, ' ', n);
			if (!sp || ((size_t)(l + n - sp - 1) == plen && !memcmp(sp + 1, path, plen)))
				continue;
			write_all(out, l, n);
			write_all(out, "\n", 1);
			kept++;
		}
		lines_free(&ls);
		pt_close(in);
	}
	if (pt_close(out) == 0)
		pt_rename(tmp, file);
	else
		pt_unlink(tmp);
}

static int line_of_src(const struct doc *d, int src)
{
	for (int i = 0; i < d->n; i++)
		if (d->lines[i].src >= src)
			return i;
	return 0;
}

static void backlight_step(struct view *v, int by)
{
#ifdef ESP_PLATFORM
	int now = lcd_backlight_get() + by;

	now = now < 5 ? 5 : now > 100 ? 100 : now;
	lcd_backlight_set(now);
	snprintf(v->note, sizeof(v->note), "backlight %d%%", now);
#else
	strcpy(v->note, "no backlight here");
#endif
}

static int view_file(const char *path)
{
	struct view v = { .path = path };
	char abs[PT_PATH_MAX];
	const char *slash;
	int err, src;

	if (pt_abspath(path, abs, sizeof(abs)))
		strlcpy(abs, path, sizeof(abs));
	slash = strrchr(abs, '/');
	v.name = slash ? slash + 1 : abs;
	pt_tty_size(PT_STDOUT, &v.cols, &v.rows);
	v.body = v.rows - 1;
	if (v.body < 1)
		return -ENOTTY;
	if ((err = doc_load(&v.d, path, v.cols)))
		return err;
	v.top = line_of_src(&v.d, pos_load(abs));
	clamp(&v);

	pt_tty_raw(PT_STDIN, true);
	pt_puts("\x1b[?25l\x1b[2J");
	for (bool done = false; !done;) {
		draw(&v);
		switch (pt_readkey(PT_STDIN)) {
		case PT_KEY_UP: case 'k':		v.top--; break;
		case PT_KEY_DOWN: case 'j': case '\r': case '\n': v.top++; break;
		case ' ': case PT_KEY_PGDN: case 'f':	v.top += v.body - 1; break;
		case 'b': case PT_KEY_PGUP:		v.top -= v.body - 1; break;
		case 'd':				v.top += v.body / 2; break;
		case 'u':				v.top -= v.body / 2; break;
		case 'g': case PT_KEY_HOME: case '<':	v.top = 0; break;
		case 'G': case PT_KEY_END: case '>':	v.top = v.d.n; break;
		case 'o': case '\t':			outline(&v); break;
		case '/':
			if (ask(&v, "/", v.query, sizeof(v.query)))
				search(&v, 1);
			break;
		case 'n':				search(&v, 1); break;
		case 'N':				search(&v, -1); break;
		case '+': case '=':			backlight_step(&v, 10); break;
		case '-':				backlight_step(&v, -10); break;
		case 'e': {
			char *argv[] = { "edit", abs, NULL };

			src = v.d.lines[v.top].src;
			pt_puts("\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
			pt_tty_raw(PT_STDIN, false);
			run_command(2, argv);
			pt_tty_raw(PT_STDIN, true);
			pt_puts("\x1b[?25l\x1b[2J");
			doc_free(&v.d);
			if ((err = doc_load(&v.d, abs, v.cols)))
				done = true;
			else
				v.top = line_of_src(&v.d, src);
			break;
		}
		case 'h': case '?':
			strcpy(v.note, "spc/b page o outline / find +/- light q quit");
			break;
		case 'q': case PT_KEY_ESC: case PT_CTRL('c'): case PT_KEY_EOF: case PT_KEY_ERROR:
			done = true;
			break;
		}
		clamp(&v);
	}
	if (v.d.n)
		pos_save(abs, v.d.lines[v.top].src);
	doc_free(&v.d);
	pt_puts("\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
	pt_tty_raw(PT_STDIN, false);
	return err;
}

/* ------------------------------------------------------------ the command */

PT_PROGRAM_STACK(notes, 12, "read Markdown or text notes on the screen\n"
	   "usage: notes [file]   (none: pick one from ~/notes)\n"
	   "       notes -p [-w cols] file...  print laid out\n"
	   "Keys: space/b a page, arrows or j/k a line, g/G top/\n"
	   "end, o the headings, / search and n/N, + and - the\n"
	   "backlight, e edit, q quit. Each note reopens where\n"
	   "you left it.")
{
	static const char *const exts[] = { ".md", ".markdown", ".txt", NULL };
	struct opt o = { .ind = 1 };
	bool print = false;
	int width = 0, c, ret = 0;
	char dir[PT_PATH_MAX], path[PT_PATH_MAX];
	const char *home;

	while ((c = getopt_pt(&o, "notes", argc, argv, "pw:")) != -1) {
		if (c == 'p') {
			print = true;
		} else if (c == 'w') {
			width = atoi(o.arg);
			if (width < 10 || width > 250) {
				pt_dprintf(PT_STDERR, "notes: -w takes 10 to 250 columns\n");
				return 2;
			}
		} else {
			return 2;
		}
	}
	if (print) {
		bool color = pt_isatty(PT_STDOUT);
		int rows;

		if (!width && color)
			pt_tty_size(PT_STDOUT, &width, &rows);
		if (!width)
			width = 80;		/* a pipe or a file */
		if (o.ind == argc) {
			pt_dprintf(PT_STDERR, "usage: notes -p [-w cols] file...\n");
			return 2;
		}
		for (int i = o.ind; i < argc; i++) {
			struct doc d;
			int err = doc_load(&d, argv[i], width);

			if (err) {
				ret = fail("notes", argv[i], err);
				continue;
			}
			print_doc(&d, color);
			doc_free(&d);
		}
		return ret;
	}
	if (!pt_isatty(PT_STDIN) || !pt_isatty(PT_STDOUT)) {
		pt_dprintf(PT_STDERR, "notes: needs a terminal (notes -p prints instead)\n");
		return 2;
	}
	if (o.ind + 1 == argc)
		return (ret = view_file(argv[o.ind])) ? fail("notes", argv[o.ind], ret) : 0;
	if (o.ind < argc) {
		pt_dprintf(PT_STDERR, "usage: notes [file]\n");
		return 2;
	}
	home = pt_getenv("HOME");
	if (!home || !join_path(home, "notes", dir, sizeof(dir)))
		return fail("notes", "~/notes", -ENOENT);
	/* Back to the list after each note, in the folder it came from. */
	while ((ret = pick_file(dir, exts, "notes", path, sizeof(path))) == 0) {
		char *slash = strrchr(path, '/');

		if ((ret = view_file(path)))
			return fail("notes", path, ret);
		if (slash && slash != path) {
			*slash = '\0';
			strlcpy(dir, path, sizeof(dir));
		}
	}
	return ret == -ECANCELED ? 0 : fail("notes", dir, ret);
}
