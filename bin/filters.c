/*
 * Filters that work on the text going through them: cut tr tee
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt/match.h"
#include "util.h"

/* ------------------------------------------------------------ cut */

#define MAX_RANGES	64

struct cut {
	struct {
		long	lo, hi;			/* from 1; hi -1 is "to the end" */
	}	 r[MAX_RANGES];
	int	 n;
	char	 mode;				/* 'b', 'c' or 'f' */
	char	 delim;
	bool	 only_delimited;
};

static bool cut_wants(const struct cut *ct, long k)
{
	for (int i = 0; i < ct->n; i++)
		if (k >= ct->r[i].lo && (ct->r[i].hi < 0 || k <= ct->r[i].hi))
			return true;
	return false;
}

/* "1,3-5,7-,-2": 0, or -1 after saying what is wrong. */
static int cut_list(struct cut *ct, const char *list)
{
	for (const char *p = list; *p;) {
		long lo = 1, hi;
		bool have_lo = *p != '-';
		char *end;

		if (ct->n == MAX_RANGES) {
			pt_dprintf(PT_STDERR, "cut: too many ranges\n");
			return -1;
		}
		if (have_lo) {
			lo = strtol(p, &end, 10);
			if (end == p || lo < 1)
				goto bad;
			p = end;
		}
		hi = lo;
		if (*p == '-') {
			p++;
			if (*p >= '0' && *p <= '9') {
				hi = strtol(p, &end, 10);
				p = end;
				if (hi < lo)
					goto bad;
			} else if (have_lo) {
				hi = -1;		/* N- is N to the end */
			} else {
				goto bad;		/* a "-" with neither end */
			}
		}
		ct->r[ct->n].lo = lo;
		ct->r[ct->n].hi = hi;
		ct->n++;
		if (*p == ',')
			p++;
		else if (*p)
			goto bad;
	}
	if (ct->n)
		return 0;
bad:
	pt_dprintf(PT_STDERR, "cut: %s: not a list of positions (like 1,3-5,7-)\n", list);
	return -1;
}

static int cut_fd(int fd, const char *name, void *ctx)
{
	struct cut *ct = ctx;
	struct lines l;
	size_t len;
	char *line;

	lines_init(&l, fd);
	while ((line = lines_next(&l, &len))) {
		bool nl = len && line[len - 1] == '\n';

		len -= nl;
		if (ct->mode == 'f') {
			if (!memchr(line, ct->delim, len)) {
				/* no delimiter: the whole line, unless -s */
				if (!ct->only_delimited) {
					write_all(PT_STDOUT, line, len);
					write_all(PT_STDOUT, "\n", 1);
				}
				continue;
			}
			bool first = true;
			size_t at = 0;

			for (long f = 1; at <= len; f++) {
				const char *d = memchr(line + at, ct->delim, len - at);
				size_t end = d ? (size_t)(d - line) : len;

				if (cut_wants(ct, f)) {
					if (!first)
						write_all(PT_STDOUT, &ct->delim, 1);
					write_all(PT_STDOUT, line + at, end - at);
					first = false;
				}
				if (!d)
					break;
				at = end + 1;
			}
		} else {
			/* -b counts bytes; -c counts characters, a UTF-8 one whole */
			long k = 0;

			for (size_t at = 0; at < len;) {
				size_t n = 1;

				if (ct->mode == 'c')
					while (at + n < len && ((unsigned char)line[at + n] & 0xc0) == 0x80)
						n++;
				if (cut_wants(ct, ++k))
					write_all(PT_STDOUT, line + at, n);
				at += n;
			}
		}
		write_all(PT_STDOUT, "\n", 1);
	}
	int err = l.err;

	lines_free(&l);
	return err ? fail("cut", name, err) : 0;
}

PT_PROGRAM(cut, "print chosen parts of each line\n"
	   "usage: cut -b LIST | -c LIST | -f LIST [-d C] [-s] [file...]\n"
	   "  -b LIST  these bytes     -c LIST  these characters\n"
	   "  -f LIST  these fields, separated by tabs, or by C with -d C\n"
	   "  -s       with -f, leave out lines that have no separator\n"
	   "LIST is positions from 1, like 1,3-5,7- (7- is 7 to the end).")
{
	struct cut ct = { .delim = '\t' };
	struct opt o = { .ind = 1 };
	int c;

	while ((c = getopt_pt(&o, "cut", argc, argv, "b:c:f:d:sn")) != -1) {
		switch (c) {
		case 'b': case 'c': case 'f':
			if (ct.mode) {
				pt_dprintf(PT_STDERR, "cut: only one of -b, -c and -f\n");
				return 2;
			}
			ct.mode = (char)c;
			if (cut_list(&ct, o.arg))
				return 2;
			break;
		case 'd':
			if (strlen(o.arg) != 1) {
				pt_dprintf(PT_STDERR, "cut: -d wants one character\n");
				return 2;
			}
			ct.delim = o.arg[0];
			break;
		case 's': ct.only_delimited = true; break;
		case 'n': break;			/* POSIX: do not split characters; they are not */
		default: return 2;
		}
	}
	if (!ct.mode) {
		pt_dprintf(PT_STDERR, "usage: cut -b LIST | -c LIST | -f LIST [-d C] [-s] [file...]\n");
		return 2;
	}
	return for_each_input("cut", argc, argv, o.ind, cut_fd, &ct) ? 1 : 0;
}

/* ------------------------------------------------------------ tr */

/*
 * A set as tr spells it, expanded into the bytes it stands for, in order:
 * ranges, escapes, [:class:], [=c=] and [c*n] (n copies; [c*] as many as
 * it takes to make the second set as long as the first).
 */
struct set {
	unsigned char	c[1024];
	int		n;
	int		fill_at;		/* where a [c*] goes, or -1 */
	unsigned char	fill;
};

static int set_parse(struct set *s, const char *spec, bool second)
{
	s->n = 0;
	s->fill_at = -1;
	for (const char *p = spec; *p;) {
		unsigned char lo;

		if (p[0] == '[' && p[1] == ':') {
			const char *end = strstr(p + 2, ":]");
			char name[16];

			if (end && (size_t)(end - p - 2) < sizeof(name)) {
				memcpy(name, p + 2, end - p - 2);
				name[end - p - 2] = '\0';
				if (pt_char_class(name, 'a') < 0) {
					pt_dprintf(PT_STDERR, "tr: unknown class [:%s:]\n", name);
					return -1;
				}
				for (int k = 0; k < 256 && s->n < (int)sizeof(s->c); k++)
					if (pt_char_class(name, (unsigned char)k) == 1)
						s->c[s->n++] = (unsigned char)k;
				p = end + 2;
				continue;
			}
		}
		if (p[0] == '[' && p[1] == '=' && p[2] && p[3] == '=' && p[4] == ']') {
			if (s->n < (int)sizeof(s->c))
				s->c[s->n++] = (unsigned char)p[2];
			p += 5;
			continue;
		}
		if (p[0] == '[' && p[1] && p[2] == '*') {
			/* [c*n] or [c*]: repeats, in the second set */
			const char *q = p + 3;
			char *end;
			long times = strtol(q, &end, *q == '0' ? 8 : 10);

			if (*end == ']') {
				if (!second) {
					pt_dprintf(PT_STDERR, "tr: [c*] only makes sense in the second set\n");
					return -1;
				}
				if (end == q || !times) {
					s->fill_at = s->n;
					s->fill = (unsigned char)p[1];
				} else {
					for (long k = 0; k < times && s->n < (int)sizeof(s->c); k++)
						s->c[s->n++] = (unsigned char)p[1];
				}
				p = end + 1;
				continue;
			}
		}
		if (*p == '\\' && p[1]) {
			p++;
			lo = (unsigned char)escape_char(&p);
		} else {
			lo = (unsigned char)*p++;
		}
		if (p[0] == '-' && p[1]) {
			unsigned char hi;

			p++;
			if (*p == '\\' && p[1]) {
				p++;
				hi = (unsigned char)escape_char(&p);
			} else {
				hi = (unsigned char)*p++;
			}
			if (hi < lo) {
				pt_dprintf(PT_STDERR, "tr: range %c-%c is backwards\n", lo, hi);
				return -1;
			}
			for (int k = lo; k <= hi && s->n < (int)sizeof(s->c); k++)
				s->c[s->n++] = (unsigned char)k;
			continue;
		}
		if (s->n < (int)sizeof(s->c))
			s->c[s->n++] = lo;
	}
	return 0;
}

PT_PROGRAM(tr, "change or delete characters\n"
	   "usage: tr [-cdst] SET1 [SET2]\n"
	   "  tr a-z A-Z      each character in SET1 becomes the one in SET2\n"
	   "  -d  delete the characters in SET1   -s  squeeze runs of one to one\n"
	   "  -c  the characters NOT in SET1      -t  cut SET1 to SET2's length\n"
	   "Sets take ranges (a-z), \\n \\t \\\\ \\NNN, [:alpha:] [:digit:] [:space:]\n"
	   "[:upper:] [:lower:] and the rest, and [c*n] (n times c) in SET2.")
{
	uint32_t flags;
	int i = parse_flags("tr", argc, argv, "cdst", &flags);
	bool del = FLAG(flags, 'd'), squeeze = FLAG(flags, 's'), comp = FLAG(flags, 'c');
	struct set *a = pt_malloc(sizeof(*a)), *b = pt_malloc(sizeof(*b));
	unsigned char map[256];
	bool in1[256] = { false }, squeezable[256] = { false };
	int status = 2, operands = i < 0 ? 0 : argc - i;

	if (!a || !b) {
		status = fail("tr", NULL, -ENOMEM) + 1;
		goto out;
	}
	if (i < 0)
		goto out;
	if (operands < 1 || operands > 2 || (!del && !squeeze && operands != 2) ||
	    (del && !squeeze && operands != 1)) {
		pt_dprintf(PT_STDERR, "usage: tr [-cdst] SET1 [SET2] (try 'help tr')\n");
		goto out;
	}
	if (set_parse(a, argv[i], false) || (operands == 2 && set_parse(b, argv[i + 1], true)))
		goto out;
	for (int k = 0; k < a->n; k++)
		in1[a->c[k]] = true;
	if (comp) {				/* SET1 becomes every other byte, in order */
		a->n = 0;
		for (int k = 0; k < 256; k++)
			if (!in1[k])
				a->c[a->n++] = (unsigned char)k;
		for (int k = 0; k < 256; k++)
			in1[k] = !in1[k];
	}
	for (int k = 0; k < 256; k++)
		map[k] = (unsigned char)k;
	if (operands == 2 && !del) {
		if (FLAG(flags, 't') && a->n > b->n)
			a->n = b->n;
		/* SET2 made as long as SET1: a [c*] fills, else its last repeats */
		if (b->n < a->n) {
			if (b->fill_at >= 0) {
				int more = a->n - b->n;

				memmove(b->c + b->fill_at + more, b->c + b->fill_at, b->n - b->fill_at);
				memset(b->c + b->fill_at, b->fill, more);
				b->n = a->n;
			} else if (b->n) {
				while (b->n < a->n)
					b->c[b->n] = b->c[b->n - 1], b->n++;
			} else {
				pt_dprintf(PT_STDERR, "tr: SET2 is empty\n");
				goto out;
			}
		}
		for (int k = 0; k < a->n; k++)
			map[a->c[k]] = b->c[k];
	}
	/* -s squeezes the last set given: SET2 when translating, else SET1 */
	if (squeeze) {
		const struct set *last = operands == 2 ? b : a;

		for (int k = 0; k < last->n; k++)
			squeezable[last->c[k]] = true;
		if (operands == 1 && comp)
			for (int k = 0; k < 256; k++)
				squeezable[k] = in1[k];
	}

	char buf[512], out[512];
	ssize_t n;
	int prev = -1;

	status = 0;
	while ((n = pt_read(PT_STDIN, buf, sizeof(buf))) > 0) {
		size_t o = 0;

		for (ssize_t k = 0; k < n; k++) {
			unsigned char c = (unsigned char)buf[k];

			if (del && in1[c])
				continue;
			c = map[c];
			if (squeeze && squeezable[c] && prev == c)
				continue;
			prev = c;
			out[o++] = (char)c;
		}
		if (write_all(PT_STDOUT, out, o))
			break;
	}
	if (n < 0)
		status = fail("tr", NULL, n);
out:
	pt_free(a);
	pt_free(b);
	return status;
}

/* ------------------------------------------------------------ tee */

PT_PROGRAM(tee, "copy standard input to files and to standard output\n"
	   "usage: tee [-a] [file...]\n"
	   "  -a  add to the end of the files instead of replacing them")
{
	uint32_t flags;
	int i = parse_flags("tee", argc, argv, "a", &flags), status = 0, n_out;
	int *fds;
	char buf[BUF_SIZE / 2];
	ssize_t n;

	if (i < 0)
		return 2;
	n_out = argc - i;
	if (!(fds = pt_malloc((n_out + 1) * sizeof(*fds))))
		return fail("tee", NULL, -ENOMEM);
	for (int k = 0; k < n_out; k++) {
		fds[k] = pt_open(argv[i + k], O_WRONLY | O_CREAT |
				 (FLAG(flags, 'a') ? O_APPEND : O_TRUNC));
		if (fds[k] < 0)
			status = fail("tee", argv[i + k], fds[k]);
	}
	while ((n = pt_read(PT_STDIN, buf, sizeof(buf))) > 0) {
		write_all(PT_STDOUT, buf, n);
		/* one file failing is no reason to stop writing the others */
		for (int k = 0; k < n_out; k++)
			if (fds[k] >= 0 && write_all(fds[k], buf, n)) {
				status = fail("tee", argv[i + k], -EIO);
				pt_close(fds[k]);
				fds[k] = -1;
			}
	}
	for (int k = 0; k < n_out; k++)
		if (fds[k] >= 0 && pt_close(fds[k]))
			status = fail("tee", argv[i + k], -EIO);
	pt_free(fds);
	return status;
}
