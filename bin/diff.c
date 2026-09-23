/*
 * diff: what changed between two files, line by line.
 *
 * Myers' algorithm, as GNU's diff uses: the shortest edit script is found
 * by following diagonals of equal lines outward, one more difference at a
 * time, so the work grows with how much differs rather than with the size
 * of the files. The search keeps each round's frontier (2d + 1 numbers for
 * the d-th difference) to walk back through afterwards, which is small for
 * the files that differ a little -- the usual case -- and is refused with
 * an error past MAX_WORK rather than let it take the machine's memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"

#define MAX_WORK	(4 * 1024 * 1024)	/* frontier entries kept, in all */
#define CONTEXT		3			/* -u: lines around each change */

struct text {
	const char	*name;
	char		*data;
	size_t		 size;
	const char	**line;
	size_t		*len;		/* without the '\n' */
	uint32_t	*hash;
	long		 n;
	bool		 no_newline;	/* the last line had none */
	time_t		 mtime;
};

static uint32_t hash_line(const char *s, size_t n)
{
	uint32_t h = 2166136261u;	/* FNV-1a */

	while (n--)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h;
}

static int load(struct text *t)
{
	int fd = !strcmp(t->name, "-") ? PT_STDIN : pt_open(t->name, O_RDONLY);
	size_t cap = 4096;
	ssize_t got;
	long cap_lines = 256;

	if (fd < 0)
		return fd;
	if (fd != PT_STDIN) {
		struct pt_stat st;

		if (!pt_stat(t->name, &st))
			t->mtime = st.mtime;
	}
	t->data = pt_malloc(cap);
	while (t->data && (got = pt_read(fd, t->data + t->size, cap - t->size)) > 0) {
		t->size += got;
		if (t->size == cap) {
			char *g = pt_realloc(t->data, cap *= 2);

			if (!g) {
				pt_free(t->data);
				t->data = NULL;
			}
			t->data = g;
		}
	}
	if (fd != PT_STDIN)
		pt_close(fd);
	if (!t->data)
		return -ENOMEM;
	t->line = pt_malloc(cap_lines * sizeof(*t->line));
	t->len = pt_malloc(cap_lines * sizeof(*t->len));
	t->hash = pt_malloc(cap_lines * sizeof(*t->hash));
	for (size_t at = 0; at < t->size && t->line && t->len && t->hash;) {
		const char *nl = memchr(t->data + at, '\n', t->size - at);
		size_t end = nl ? (size_t)(nl - t->data) : t->size;

		if (t->n == cap_lines) {
			cap_lines *= 2;
			const char **l = pt_realloc(t->line, cap_lines * sizeof(*l));
			size_t *n = pt_realloc(t->len, cap_lines * sizeof(*n));
			uint32_t *h = pt_realloc(t->hash, cap_lines * sizeof(*h));

			t->line = l ? l : t->line;
			t->len = n ? n : t->len;
			t->hash = h ? h : t->hash;
			if (!l || !n || !h)
				return -ENOMEM;
		}
		t->line[t->n] = t->data + at;
		t->len[t->n] = end - at;
		t->hash[t->n] = hash_line(t->data + at, end - at);
		t->n++;
		t->no_newline = !nl;
		at = end + 1;
	}
	return t->line && t->len && t->hash ? 0 : -ENOMEM;
}

static bool same(const struct text *a, long i, const struct text *b, long j)
{
	return a->hash[i] == b->hash[j] && a->len[i] == b->len[j] &&
	       !memcmp(a->line[i], b->line[j], a->len[i]);
}

/* What happens to each line: kept, deleted from a, or inserted from b. */
enum op { KEEP, DEL, INS };

struct script {
	char	*op;
	long	 n;
};

/*
 * The shortest edit script from a to b. `v` is the furthest x reached on
 * each diagonal k = x - y; round d's is kept at trace[offset[d]], indexed
 * by k + d, so the path can be walked back from the end.
 */
static int myers(const struct text *a, const struct text *b, struct script *s)
{
	long n = a->n, m = b->n, max = n + m, d, found = -1;
	long *trace = NULL, *offset = NULL, used = 0, cap = 0;

	if (!(offset = pt_malloc((max + 2) * sizeof(*offset))))
		return -ENOMEM;
	for (d = 0; d <= max && found < 0; d++) {
		long *v, *prev = d ? trace + offset[d - 1] : NULL;

		if (used + 2 * d + 1 > cap) {
			long grown = cap ? cap * 2 : 1024;

			while (grown < used + 2 * d + 1)
				grown *= 2;
			if (grown > MAX_WORK) {
				pt_free(trace);
				pt_free(offset);
				return -E2BIG;
			}
			long *g = pt_realloc(trace, grown * sizeof(*g));

			if (!g) {
				pt_free(trace);
				pt_free(offset);
				return -ENOMEM;
			}
			trace = g;
			cap = grown;
			prev = d ? trace + offset[d - 1] : NULL;
		}
		offset[d] = used;
		v = trace + used;
		used += 2 * d + 1;
		for (long k = -d; k <= d; k += 2) {
			long x;

			/* down (an insertion) or right (a deletion), whichever reaches further */
			if (k == -d || (k != d && prev[k - 1 + (d - 1)] < prev[k + 1 + (d - 1)]))
				x = d ? prev[k + 1 + (d - 1)] : 0;
			else
				x = prev[k - 1 + (d - 1)] + 1;
			long y = x - k;

			while (x < n && y < m && same(a, x, b, y))
				x++, y++;
			v[k + d] = x;
			if (x >= n && y >= m) {
				found = d;
				break;
			}
		}
	}
	/* walk back, writing the script from its end */
	s->n = 0;
	if (!(s->op = pt_malloc(max + 1))) {
		pt_free(trace);
		pt_free(offset);
		return -ENOMEM;
	}
	long x = n, y = m, at = max;

	for (d = found; d >= 0; d--) {
		long k = x - y, prev_k, px, py;

		if (!d) {
			while (x > 0 && y > 0)
				s->op[--at] = KEEP, x--, y--;
			break;
		}
		const long *prev = trace + offset[d - 1];

		if (k == -d || (k != d && prev[k - 1 + (d - 1)] < prev[k + 1 + (d - 1)]))
			prev_k = k + 1;
		else
			prev_k = k - 1;
		px = prev[prev_k + (d - 1)];
		py = px - prev_k;
		while (x > px && y > py)
			s->op[--at] = KEEP, x--, y--;
		s->op[--at] = prev_k == k + 1 ? INS : DEL;
		x = px;
		y = py;
	}
	s->n = max - at;
	memmove(s->op, s->op + at, s->n);
	pt_free(trace);
	pt_free(offset);
	return 0;
}

/* ------------------------------------------------------------ output */

static void put_line(char mark, const struct text *t, long i)
{
	char head[2] = { mark, ' ' };

	write_all(PT_STDOUT, head, mark == '<' || mark == '>' ? 2 : 1);
	write_all(PT_STDOUT, t->line[i], t->len[i]);
	write_all(PT_STDOUT, "\n", 1);
	if (i == t->n - 1 && t->no_newline)
		pt_puts("\\ No newline at end of file\n");
}

/* "3" or "3,5": lines from..to, counted from 1. */
static void range(char *out, size_t size, long from, long to)
{
	if (to <= from + 1)
		snprintf(out, size, "%ld", to > from ? from + 1 : from);
	else
		snprintf(out, size, "%ld,%ld", from + 1, to);
}

/* The classic format: 2c2, 5a6,7, 9d8 and the lines, < and >. */
static void normal(const struct script *s, const struct text *a, const struct text *b)
{
	long i = 0, j = 0;

	for (long k = 0; k < s->n;) {
		if (s->op[k] == KEEP) {
			i++, j++, k++;
			continue;
		}
		long di = i, dj = j;

		while (k < s->n && s->op[k] != KEEP) {
			if (s->op[k++] == DEL)
				di++;
			else
				dj++;
		}
		char ra[32], rb[32];
		char c = di > i && dj > j ? 'c' : di > i ? 'd' : 'a';

		range(ra, sizeof(ra), c == 'a' ? i : i, c == 'a' ? i : di);
		range(rb, sizeof(rb), c == 'd' ? j : j, c == 'd' ? j : dj);
		if (c == 'a')
			snprintf(ra, sizeof(ra), "%ld", i);
		if (c == 'd')
			snprintf(rb, sizeof(rb), "%ld", j);
		pt_printf("%s%c%s\n", ra, c, rb);
		for (long x = i; x < di; x++)
			put_line('<', a, x);
		if (c == 'c')
			pt_puts("---\n");
		for (long y = j; y < dj; y++)
			put_line('>', b, y);
		i = di;
		j = dj;
	}
}

static void unified_header(const struct text *t, char mark)
{
	char when[40] = "";
	struct tm tm;

	if (t->mtime > 0 && localtime_r(&t->mtime, &tm))
		strftime(when, sizeof(when), "\t%Y-%m-%d %H:%M:%S", &tm);
	pt_printf("%c%c%c %s%s\n", mark, mark, mark, t->name, when);
}

/* "3,4" as a unified hunk says it: the start and the count, 1 left out. */
static void hunk_range(char *out, size_t size, long start, long count)
{
	if (count == 1)
		snprintf(out, size, "%ld", start + 1);
	else
		snprintf(out, size, "%ld,%ld", count ? start + 1 : start, count);
}

/* -u: hunks of changes with CONTEXT lines of what is kept around them. */
static void unified(const struct script *s, const struct text *a, const struct text *b)
{
	unified_header(a, '-');
	unified_header(b, '+');
	for (long k = 0, i = 0, j = 0; k < s->n;) {
		if (s->op[k] == KEEP) {
			i++, j++, k++;
			continue;
		}
		/* a hunk: from CONTEXT before this change to CONTEXT after the
		 * last change that is no more than 2 * CONTEXT kept lines on */
		long start = k, end = k, keeps = 0;

		for (long e = k; e < s->n; e++) {
			if (s->op[e] == KEEP) {
				if (++keeps > 2 * CONTEXT)
					break;
			} else {
				keeps = 0;
				end = e + 1;
			}
		}
		long back = 0;

		while (back < CONTEXT && start > 0 && s->op[start - 1] == KEEP)
			start--, back++;
		long fwd = 0;

		while (fwd < CONTEXT && end < s->n && s->op[end] == KEEP)
			end++, fwd++;
		long ai = i - back, bj = j - back, na = 0, nb = 0;

		for (long e = start; e < end; e++) {
			na += s->op[e] != INS;
			nb += s->op[e] != DEL;
		}
		char ra[32], rb[32];

		hunk_range(ra, sizeof(ra), ai, na);
		hunk_range(rb, sizeof(rb), bj, nb);
		pt_printf("@@ -%s +%s @@\n", ra, rb);
		for (long e = start, x = ai, y = bj; e < end; e++) {
			if (s->op[e] == KEEP) {
				put_line(' ', a, x++);
				y++;
			} else if (s->op[e] == DEL) {
				put_line('-', a, x++);
			} else {
				put_line('+', b, y++);
			}
		}
		/* on past the hunk */
		for (long e = k; e < end; e++) {
			i += s->op[e] != INS;
			j += s->op[e] != DEL;
		}
		k = end;
	}
}

static void free_text(struct text *t)
{
	pt_free(t->data);
	pt_free(t->line);
	pt_free(t->len);
	pt_free(t->hash);
}

PT_PROGRAM(diff, "show how two files differ, line by line\n"
	   "usage: diff [-uq] file1 file2\n"
	   "  -u  unified: changes with 3 lines around them,\n"
	   "      - for lines taken out and + for lines put in\n"
	   "  -q  only say whether they differ\n"
	   "Without -u: 2c2 (changed), 5a6 (added), 9d8 (deleted),\n"
	   "< the first file's lines, > the second's.\n"
	   "Status 0: the same, 1: they differ, 2: trouble.")
{
	struct text a = { 0 }, b = { 0 };
	struct script s = { 0 };
	struct opt o = { .ind = 1 };
	bool unified_out = false, quiet = false;
	int c, err;

	while ((c = getopt_pt(&o, "diff", argc, argv, "uq")) != -1) {
		if (c == 'u')
			unified_out = true;
		else if (c == 'q')
			quiet = true;
		else
			return 2;
	}
	if (argc - o.ind != 2) {
		pt_dprintf(PT_STDERR, "usage: diff [-uq] file1 file2\n");
		return 2;
	}
	a.name = argv[o.ind];
	b.name = argv[o.ind + 1];
	if ((err = load(&a)) || (err = load(&b))) {
		fail("diff", err == -ENOMEM ? NULL : a.data && !b.data ? b.name : a.name, err);
		free_text(&a);
		free_text(&b);
		return 2;
	}
	bool differ = a.size != b.size || memcmp(a.data, b.data, a.size);

	if (differ && !quiet && (err = myers(&a, &b, &s)))
		fail("diff", err == -E2BIG ? "the files differ too much to compare" : NULL,
		     err == -E2BIG ? -ENOMEM : err);
	else if (differ && quiet)
		pt_printf("Files %s and %s differ\n", a.name, b.name);
	else if (differ && unified_out)
		unified(&s, &a, &b);
	else if (differ)
		normal(&s, &a, &b);
	pt_free(s.op);
	free_text(&a);
	free_text(&b);
	return err ? 2 : differ;
}
