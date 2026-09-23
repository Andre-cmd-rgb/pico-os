/*
 * Ordering lines: sort uniq
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

/* ------------------------------------------------------------ keys */

#define MAX_KEYS	8

/* Per key, or for the whole line: how to compare. */
struct how {
	bool	 numeric, human, reverse, fold, blanks;
};

struct key {
	int		 f1, c1, f2, c2;	/* fields and characters, from 1; f2 0 is the end of line */
	struct how	 how;
	bool		 blanks_end;		/* the b on the second position */
};

struct sorter {
	struct key	 keys[MAX_KEYS];
	int		 nkeys;
	struct how	 global;
	int		 sep;			/* -t, or -1 for runs of blanks */
	bool		 unique, stable, check;
	const char	*output;
};

struct line {
	const char	*s;
	size_t		 len;			/* without the '\n' */
};

static bool blank(char c)
{
	return c == ' ' || c == '\t';
}

/*
 * Where field f (from 1) starts and ends. With -t each separator ends a
 * field; without, a field is the blanks before it and the text up to the
 * next blank, as POSIX has it -- which is why -b exists.
 */
static void field_span(const struct sorter *st, const struct line *l, int f, size_t *from, size_t *to)
{
	size_t at = 0;

	for (int k = 1; k < f && at < l->len; k++) {
		if (st->sep >= 0) {
			const char *p = memchr(l->s + at, st->sep, l->len - at);

			at = p ? (size_t)(p - l->s) + 1 : l->len;
		} else {
			while (at < l->len && blank(l->s[at]))
				at++;
			while (at < l->len && !blank(l->s[at]))
				at++;
		}
	}
	*from = at;
	if (st->sep >= 0) {
		const char *p = memchr(l->s + at, st->sep, l->len - at);

		*to = p ? (size_t)(p - l->s) : l->len;
	} else {
		size_t e = at;

		while (e < l->len && blank(l->s[e]))
			e++;
		while (e < l->len && !blank(l->s[e]))
			e++;
		*to = e;
	}
}

/* The part of the line key k looks at. */
static void key_span(const struct sorter *st, const struct key *k, const struct line *l,
		     const char **s, size_t *n)
{
	size_t fs, fe, start, end;

	field_span(st, l, k->f1, &fs, &fe);
	if (k->how.blanks)
		while (fs < fe && blank(l->s[fs]))
			fs++;
	start = k->c1 > 1 ? fs + (size_t)(k->c1 - 1) : fs;
	if (start > l->len)
		start = l->len;
	if (!k->f2) {
		end = l->len;
	} else {
		field_span(st, l, k->f2, &fs, &fe);
		if (k->blanks_end)
			while (fs < fe && blank(l->s[fs]))
				fs++;
		end = k->c2 ? fs + (size_t)k->c2 : fe;	/* .0 or none: to the field's end */
		if (end > l->len)
			end = l->len;
	}
	if (end < start)
		end = start;
	*s = l->s + start;
	*n = end - start;
}

/* ------------------------------------------------------------ comparing */

/*
 * Numbers compared as digit strings, so that a long one loses nothing:
 * sign, then the length of the whole part, then the digits, then the
 * fraction. Anything that is not a number counts as zero.
 */
static int compare_numbers(const char *a, size_t an, const char *b, size_t bn)
{
	size_t i = 0, j = 0;
	bool neg_a, neg_b;

	while (i < an && blank(a[i]))
		i++;
	while (j < bn && blank(b[j]))
		j++;
	neg_a = i < an && a[i] == '-';
	neg_b = j < bn && b[j] == '-';
	i += neg_a;
	j += neg_b;
	while (i < an && a[i] == '0')
		i++;
	while (j < bn && b[j] == '0')
		j++;
	size_t ai = i, bj = j;

	while (i < an && isdigit((unsigned char)a[i]))
		i++;
	while (j < bn && isdigit((unsigned char)b[j]))
		j++;
	size_t alen = i - ai, blen = j - bj;
	/* the fractions, if any, and whether each number is zero */
	const char *af = i < an && a[i] == '.' ? a + i + 1 : NULL, *bf = j < bn && b[j] == '.' ? b + j + 1 : NULL;
	size_t afn = 0, bfn = 0;

	if (af)
		while (af + afn < a + an && isdigit((unsigned char)af[afn]))
			afn++;
	if (bf)
		while (bf + bfn < b + bn && isdigit((unsigned char)bf[bfn]))
			bfn++;
	bool zero_a = !alen, zero_b = !blen;

	for (size_t k = 0; k < afn && zero_a; k++)
		zero_a = af[k] == '0';
	for (size_t k = 0; k < bfn && zero_b; k++)
		zero_b = bf[k] == '0';
	if (zero_a)
		neg_a = false;			/* -0 is 0 */
	if (zero_b)
		neg_b = false;
	if (neg_a != neg_b)
		return neg_a ? -1 : 1;
	int sign = neg_a ? -1 : 1, c = 0;

	if (alen != blen)
		c = alen < blen ? -1 : 1;
	else if ((c = memcmp(a + ai, b + bj, alen)))
		c = c < 0 ? -1 : 1;
	else
		for (size_t k = 0; !c && (k < afn || k < bfn); k++) {
			char x = k < afn ? af[k] : '0', y = k < bfn ? bf[k] : '0';

			c = x < y ? -1 : x > y;
		}
	return sign * c;
}

/* 2K < 1M: the suffix first, then the number. */
static int compare_human(const char *a, size_t an, const char *b, size_t bn)
{
	static const char units[] = "KMGTPE";
	int ua = 0, ub = 0;
	size_t i = 0;

	while (i < an && (blank(a[i]) || isdigit((unsigned char)a[i]) || a[i] == '.' || a[i] == '-'))
		i++;
	if (i < an && a[i] && strchr(units, toupper((unsigned char)a[i])))
		ua = (int)(strchr(units, toupper((unsigned char)a[i])) - units) + 1;
	i = 0;
	while (i < bn && (blank(b[i]) || isdigit((unsigned char)b[i]) || b[i] == '.' || b[i] == '-'))
		i++;
	if (i < bn && b[i] && strchr(units, toupper((unsigned char)b[i])))
		ub = (int)(strchr(units, toupper((unsigned char)b[i])) - units) + 1;
	if (ua != ub)
		return ua < ub ? -1 : 1;
	return compare_numbers(a, an, b, bn);
}

static int compare_text(const char *a, size_t an, const char *b, size_t bn, bool fold)
{
	size_t n = an < bn ? an : bn;
	int c;

	if (fold) {
		for (size_t k = 0; k < n; k++) {
			int x = toupper((unsigned char)a[k]), y = toupper((unsigned char)b[k]);

			if (x != y)
				return x < y ? -1 : 1;
		}
		c = 0;
	} else {
		c = memcmp(a, b, n);
	}
	if (c)
		return c < 0 ? -1 : 1;
	return an < bn ? -1 : an > bn;
}

static int compare_how(const struct how *h, const char *a, size_t an, const char *b, size_t bn)
{
	int c;

	if (h->blanks && !h->numeric && !h->human) {
		while (an && blank(*a))
			a++, an--;
		while (bn && blank(*b))
			b++, bn--;
	}
	c = h->human ? compare_human(a, an, b, bn)
	  : h->numeric ? compare_numbers(a, an, b, bn)
	  : compare_text(a, an, b, bn, h->fold);
	return h->reverse ? -c : c;
}

/* The keys only: 0 means equal for -u. */
static int compare_keys(const struct sorter *st, const struct line *x, const struct line *y)
{
	if (!st->nkeys)
		return compare_how(&st->global, x->s, x->len, y->s, y->len);
	for (int i = 0; i < st->nkeys; i++) {
		const char *a, *b;
		size_t an, bn;
		int c;

		key_span(st, &st->keys[i], x, &a, &an);
		key_span(st, &st->keys[i], y, &b, &bn);
		if ((c = compare_how(&st->keys[i].how, a, an, b, bn)))
			return c;
	}
	return 0;
}

/* Keys, then -- unless -s or -u -- the whole line byte by byte, as GNU does. */
static int compare(const struct sorter *st, const struct line *x, const struct line *y)
{
	int c = compare_keys(st, x, y);

	if (c || st->stable || st->unique)
		return c;
	c = compare_text(x->s, x->len, y->s, y->len, false);
	return st->global.reverse ? -c : c;
}

/* ------------------------------------------------------------ sorting */

/* Merge sort: stable, which qsort is not, and never quadratic. */
static void merge_sort(const struct sorter *st, struct line *v, struct line *tmp, size_t n)
{
	for (size_t width = 1; width < n; width *= 2) {
		for (size_t lo = 0; lo < n; lo += 2 * width) {
			size_t mid = lo + width < n ? lo + width : n;
			size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
			size_t i = lo, j = mid, k = lo;

			while (i < mid && j < hi)
				tmp[k++] = compare(st, &v[j], &v[i]) < 0 ? v[j++] : v[i++];
			while (i < mid)
				tmp[k++] = v[i++];
			while (j < hi)
				tmp[k++] = v[j++];
		}
		memcpy(v, tmp, n * sizeof(*v));
	}
}

/*
 * Every line, copied into chunks of 64 KB that never move, so a line is a
 * pointer and a length and reading a big file is not a malloc per line.
 */
#define CHUNK	(64 * 1024)

struct all_lines {
	struct line	*v;
	size_t		 n, cap;
	char		**chunks;
	int		 nchunks, capchunks;
	size_t		 used;			/* of the last chunk */
	size_t		 chunk_size;
	bool		 oom;
};

static char *arena_take(struct all_lines *al, size_t len)
{
	if (!al->nchunks || al->chunk_size - al->used < len) {
		size_t size = len > CHUNK ? len : CHUNK;
		char *chunk;

		if (al->nchunks == al->capchunks) {
			int cap = al->capchunks ? al->capchunks * 2 : 8;
			char **grown = pt_realloc(al->chunks, cap * sizeof(*grown));

			if (!grown)
				return NULL;
			al->chunks = grown;
			al->capchunks = cap;
		}
		if (!(chunk = pt_malloc(size)))
			return NULL;
		al->chunks[al->nchunks++] = chunk;
		al->used = 0;
		al->chunk_size = size;
	}
	char *p = al->chunks[al->nchunks - 1] + al->used;

	al->used += len;
	return p;
}

static int read_all(int fd, const char *name, void *ctx)
{
	struct all_lines *al = ctx;
	struct lines l;
	size_t len;
	char *line;

	lines_init(&l, fd);
	while (!al->oom && (line = lines_next(&l, &len))) {
		if (len && line[len - 1] == '\n')
			len--;
		if (al->n == al->cap) {
			size_t cap = al->cap ? al->cap * 2 : 256;
			struct line *grown = pt_realloc(al->v, cap * sizeof(*grown));

			if (!grown) {
				al->oom = true;
				break;
			}
			al->v = grown;
			al->cap = cap;
		}
		char *copy = arena_take(al, len);

		if (!copy) {
			al->oom = true;
			break;
		}
		memcpy(copy, line, len);
		al->v[al->n++] = (struct line){ copy, len };
	}
	int err = l.err;

	lines_free(&l);
	if (al->oom)
		return fail("sort", name, -ENOMEM);
	return err ? fail("sort", name, err) : 0;
}

/* F[.C][bfhnr]: a key position. `first`: the start, where .0 means nothing. */
static const char *parse_pos(const char *s, int *f, int *c, struct how *how, bool *blanks,
			     bool first)
{
	*f = 0;
	*c = 0;
	if (!isdigit((unsigned char)*s))
		return NULL;
	while (isdigit((unsigned char)*s))
		*f = *f * 10 + (*s++ - '0');
	if (*s == '.') {
		s++;
		if (!isdigit((unsigned char)*s))
			return NULL;
		while (isdigit((unsigned char)*s))
			*c = *c * 10 + (*s++ - '0');
		if (first && !*c)
			return NULL;		/* characters count from 1 */
	}
	for (; *s && *s != ','; s++) {
		switch (*s) {
		case 'b': *blanks = true; break;
		case 'f': how->fold = true; break;
		case 'n': how->numeric = true; break;
		case 'h': how->human = true; break;
		case 'r': how->reverse = true; break;
		default: return NULL;
		}
	}
	return s;
}

static int parse_key(struct sorter *st, const char *spec)
{
	struct key *k = &st->keys[st->nkeys];
	const char *p;
	bool b1 = false;

	if (st->nkeys == MAX_KEYS) {
		pt_dprintf(PT_STDERR, "sort: at most %d keys\n", MAX_KEYS);
		return -1;
	}
	memset(k, 0, sizeof(*k));
	p = parse_pos(spec, &k->f1, &k->c1, &k->how, &b1, true);
	if (p && *p == ',')
		p = parse_pos(p + 1, &k->f2, &k->c2, &k->how, &k->blanks_end, false);
	if (!p || *p || !k->f1 || (p != spec && strchr(spec, ',') && !k->f2)) {
		pt_dprintf(PT_STDERR, "sort: -k %s: not a key\n", spec);
		return -1;
	}
	k->how.blanks = b1;
	st->nkeys++;
	return 0;
}

PT_PROGRAM(sort, "sort lines\n"
	   "usage: sort [-bcfhnrsu] [-k KEY]... [-t C] [-o FILE] [file...]\n"
	   "  -n  by number   -h  by size (2K < 1M)   -f  ignore case\n"
	   "  -r  backwards   -u  one of each   -b  skip leading blanks\n"
	   "  -c  only check that it is sorted   -s  keep equal lines in order\n"
	   "  -t C    fields are separated by C, not by blanks\n"
	   "  -k F[.C][,F[.C]]  sort on those fields (letters after either\n"
	   "          position -- bfhnr -- apply to that key only)\n"
	   "  -o FILE write there; it may be one of the inputs")
{
	struct sorter st = { .sep = -1 };
	struct all_lines al = { 0 };
	struct opt o = { .ind = 1 };
	int c, status = 0, out = PT_STDOUT;

	while ((c = getopt_pt(&o, "sort", argc, argv, "bcfhnrsuk:t:o:")) != -1) {
		switch (c) {
		case 'b': st.global.blanks = true; break;
		case 'c': st.check = true; break;
		case 'f': st.global.fold = true; break;
		case 'h': st.global.human = true; break;
		case 'n': st.global.numeric = true; break;
		case 'r': st.global.reverse = true; break;
		case 's': st.stable = true; break;
		case 'u': st.unique = true; break;
		case 'o': st.output = o.arg; break;
		case 'k':
			if (parse_key(&st, o.arg))
				return 2;
			break;
		case 't':
			if (strlen(o.arg) != 1 && strcmp(o.arg, "\\t") && strcmp(o.arg, "\\0")) {
				pt_dprintf(PT_STDERR, "sort: -t wants one character\n");
				return 2;
			}
			st.sep = !strcmp(o.arg, "\\t") ? '\t' : !strcmp(o.arg, "\\0") ? 0 : (unsigned char)o.arg[0];
			break;
		default:
			return 2;
		}
	}
	/* a key with no letters of its own takes the global ones */
	for (int i = 0; i < st.nkeys; i++) {
		struct how *h = &st.keys[i].how;

		if (!h->numeric && !h->human && !h->reverse && !h->fold && !h->blanks &&
		    !st.keys[i].blanks_end) {
			*h = st.global;
			st.keys[i].blanks_end = st.global.blanks;
		}
	}
	status = for_each_input("sort", argc, argv, o.ind, read_all, &al);
	if (al.oom)
		goto out;

	if (st.check) {
		for (size_t i = 1; i < al.n; i++) {
			int d = compare(&st, &al.v[i - 1], &al.v[i]);

			if (d > 0 || (st.unique && !d)) {
				pt_dprintf(PT_STDERR, "sort: %s:%zu: disorder: %.*s\n",
					   o.ind < argc ? argv[o.ind] : "-", i + 1,
					   (int)al.v[i].len, al.v[i].s);
				status = 1;
				goto out;
			}
		}
		goto out;
	}
	struct line *tmp = pt_malloc((al.n ? al.n : 1) * sizeof(*tmp));

	if (!tmp) {
		status = fail("sort", NULL, -ENOMEM);
		goto out;
	}
	merge_sort(&st, al.v, tmp, al.n);
	pt_free(tmp);

	/* everything is read before the output opens, so -o may be an input */
	if (st.output && (out = pt_open(st.output, O_WRONLY | O_CREAT | O_TRUNC)) < 0) {
		status = fail("sort", st.output, out);
		goto out;
	}
	for (size_t i = 0; i < al.n; i++) {
		if (st.unique && i && !compare_keys(&st, &al.v[i - 1], &al.v[i]))
			continue;
		if (write_all(out, al.v[i].s, al.v[i].len) || write_all(out, "\n", 1)) {
			status = fail("sort", st.output ? st.output : "stdout", -EIO);
			break;
		}
	}
	if (out != PT_STDOUT) {
		int err = pt_close(out);

		if (err && !status)
			status = fail("sort", st.output, err);
	}
out:
	for (int i = 0; i < al.nchunks; i++)
		pt_free(al.chunks[i]);
	pt_free(al.chunks);
	pt_free(al.v);
	return status ? (st.check ? 1 : 2) : 0;
}

/* ------------------------------------------------------------ uniq */

struct uniq {
	long	 skip_fields, skip_chars;
	bool	 count, repeated, unique, fold;
	int	 out;
};

/* The part of a line uniq compares: after -f fields and -s characters. */
static void uniq_part(const struct uniq *u, const char *s, size_t n, const char **p, size_t *pn)
{
	size_t at = 0;

	for (long f = 0; f < u->skip_fields && at < n; f++) {
		while (at < n && blank(s[at]))
			at++;
		while (at < n && !blank(s[at]))
			at++;
	}
	at += (size_t)u->skip_chars < n - at ? (size_t)u->skip_chars : n - at;
	*p = s + at;
	*pn = n - at;
}

static void uniq_emit(const struct uniq *u, const char *s, size_t n, long times)
{
	if ((u->repeated && times < 2) || (u->unique && times > 1))
		return;
	if (u->count)
		pt_dprintf(u->out, "%7ld ", times);
	write_all(u->out, s, n);
	write_all(u->out, "\n", 1);
}

PT_PROGRAM(uniq, "leave out repeated lines that follow each other\n"
	   "usage: uniq [-cdui] [-f N] [-s N] [input [output]]\n"
	   "  -c  count how many times each came   -i  ignore case\n"
	   "  -d  only the lines that repeat   -u  only those that do not\n"
	   "  -f N  compare from after N fields   -s N  and after N more characters\n"
	   "Only neighbours are compared: sort first to count all of them.")
{
	struct uniq u = { .out = PT_STDOUT };
	struct opt o = { .ind = 1 };
	struct lines l;
	char *prev = NULL, *line;
	size_t prev_len = 0, len;
	long times = 0;
	int c, in = PT_STDIN, status = 0;

	while ((c = getopt_pt(&o, "uniq", argc, argv, "cduif:s:")) != -1) {
		switch (c) {
		case 'c': u.count = true; break;
		case 'd': u.repeated = true; break;
		case 'u': u.unique = true; break;
		case 'i': u.fold = true; break;
		case 'f':
		case 's':
			if (parse_long(o.arg, c == 'f' ? &u.skip_fields : &u.skip_chars) ||
			    (c == 'f' ? u.skip_fields : u.skip_chars) < 0) {
				pt_dprintf(PT_STDERR, "uniq: -%c %s: not a number\n", c, o.arg);
				return 2;
			}
			break;
		default:
			return 2;
		}
	}
	if (argc - o.ind > 2) {
		pt_dprintf(PT_STDERR, "usage: uniq [-cdui] [-f N] [-s N] [input [output]]\n");
		return 2;
	}
	if (o.ind < argc && strcmp(argv[o.ind], "-") && (in = pt_open(argv[o.ind], O_RDONLY)) < 0)
		return fail("uniq", argv[o.ind], in);
	if (o.ind + 1 < argc && (u.out = pt_open(argv[o.ind + 1], O_WRONLY | O_CREAT | O_TRUNC)) < 0) {
		if (in != PT_STDIN)
			pt_close(in);
		return fail("uniq", argv[o.ind + 1], u.out);
	}
	lines_init(&l, in);
	while ((line = lines_next(&l, &len))) {
		const char *a, *b;
		size_t an, bn;

		if (len && line[len - 1] == '\n')
			len--;
		if (prev) {
			uniq_part(&u, prev, prev_len, &a, &an);
			uniq_part(&u, line, len, &b, &bn);
			if (an == bn && (u.fold ? !strncasecmp(a, b, an) : !memcmp(a, b, an))) {
				times++;
				continue;
			}
			uniq_emit(&u, prev, prev_len, times);
		}
		char *copy = pt_realloc(prev, len ? len : 1);

		if (!copy) {
			status = fail("uniq", NULL, -ENOMEM);
			break;
		}
		prev = copy;
		memcpy(prev, line, len);
		prev_len = len;
		times = 1;
	}
	if (prev && !status)
		uniq_emit(&u, prev, prev_len, times);
	if (l.err)
		status = fail("uniq", NULL, l.err);
	lines_free(&l);
	pt_free(prev);
	if (in != PT_STDIN)
		pt_close(in);
	if (u.out != PT_STDOUT) {
		int err = pt_close(u.out);

		if (err && !status)
			status = fail("uniq", argv[o.ind + 1], err);
	}
	return status ? 2 : 0;
}
