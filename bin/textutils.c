/*
 * Text programs: echo head tail wc grep cmp hexdump more
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regex.h"
#include "util.h"

/* Colour for a terminal, and none at all for a file or a pipe. */
#define DIM(tty)	((tty) ? "\x1b[2m" : "")
#define BOLD(tty)	((tty) ? "\x1b[1;33m" : "")
#define PLAIN(tty)	((tty) ? "\x1b[0m" : "")

/* ------------------------------------------------------------ echo */

PT_PROGRAM(echo, "print arguments\n"
	   "usage: echo [-ne] [text...]\n"
	   "  -n  no newline at the end\n"
	   "  -e  interpret \\n \\t \\\\ \\0NNN \\xHH and friends; \\c stops")
{
	uint32_t flags = 0;
	size_t size = 2, len = 0;
	char *out;
	int i = 1, first;

	/* Its own parsing, because echo prints whatever it is given: an
	 * argument is an option only while every letter in it is one, so
	 * `echo ---` and `echo -foo` print themselves rather than
	 * complaining, as every other echo does. */
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		const char *c = argv[i] + 1;

		while (*c == 'n' || *c == 'e')
			c++;
		if (*c)
			break;
		for (c = argv[i] + 1; *c; c++)
			flags |= 1u << (*c - 'a');
	}
	/* One write for it all, as the shell runs it in its own process
	 * too: the words and the spaces between, which escapes only shrink. */
	for (int k = i; k < argc; k++)
		size += strlen(argv[k]) + 1;
	if (!(out = pt_malloc(size)))
		return fail("echo", "output", -ENOMEM);
	for (first = i; i < argc; i++) {
		if (i > first)
			out[len++] = ' ';
		if (!FLAG(flags, 'e')) {
			size_t n = strlen(argv[i]);

			memcpy(out + len, argv[i], n);
			len += n;
			continue;
		}
		for (const char *s = argv[i]; *s;) {
			char c = *s++;

			if (c == '\\' && *s == 'c')
				goto out;		/* \c: nothing more at all */
			if (c == '\\')
				c = (char)escape_char_0(&s);
			out[len++] = c;
		}
	}
	if (!FLAG(flags, 'n'))
		out[len++] = '\n';
out:
	write_all(PT_STDOUT, out, len);
	pt_free(out);
	return 0;
}

/* ------------------------------------------------------------ head, tail */

struct span {
	long	 count;
	bool	 bytes;		/* -c: count bytes, not lines */
	bool	 from;		/* tail -n +N: from line N on */
	bool	 follow;
	int	 files;
	int	 shown;		/* headers printed so far */
};

/* "-n N", "-c N", "-N" and "-nN": the first operand's index, or -1. */
static int parse_span(const char *prog, int argc, char **argv, struct span *sp, bool tail)
{
	struct opt o = { .ind = 1 };
	int c;

	sp->count = 10;
	if (argc > 1 && argv[1][0] == '-' && isdigit((unsigned char)argv[1][1])) {
		if (parse_long(argv[1] + 1, &sp->count)) {
			pt_dprintf(PT_STDERR, "%s: %s: not a number\n", prog, argv[1] + 1);
			return -1;
		}
		o.ind = 2;
	}
	while ((c = getopt_pt(&o, prog, argc, argv, tail ? "n:c:f" : "n:c:")) != -1) {
		const char *arg = o.arg;

		switch (c) {
		case 'f':
			sp->follow = true;
			continue;
		case 'c':
		case 'n':
			sp->bytes = c == 'c';
			sp->from = tail && *arg == '+';
			if (*arg == '+' || (tail && *arg == '-'))
				arg++;
			if (parse_long(arg, &sp->count) || sp->count < 0) {
				pt_dprintf(PT_STDERR, "%s: %s: not a number\n", prog, o.arg);
				return -1;
			}
			continue;
		}
		return -1;
	}
	sp->files = argc - o.ind;
	return o.ind;
}

/* Not a static count: a built-in program's statics outlive the run. */
static void header(const char *name, struct span *sp)
{
	if (sp->files > 1)
		pt_printf("%s==> %s <==\n", sp->shown++ ? "\n" : "", name ? name : "standard input");
}

static int head_fd(int fd, const char *name, void *ctx)
{
	struct span *sp = ctx;
	long left = sp->count;
	struct lines l;
	size_t len;
	char *line;

	header(name, sp);
	if (sp->bytes) {
		char buf[512];
		ssize_t n = 1;

		while (left > 0 && (n = pt_read(fd, buf, left < (long)sizeof(buf) ? left : (long)sizeof(buf))) > 0) {
			write_all(PT_STDOUT, buf, n);
			left -= n;
		}
		return n < 0 ? fail("head", name, n) : 0;
	}
	lines_init(&l, fd);
	while (left > 0 && (line = lines_next(&l, &len))) {
		write_all(PT_STDOUT, line, len);
		left--;
	}
	lines_free(&l);
	return l.err ? fail("head", name, l.err) : 0;
}

PT_PROGRAM(head, "print the first lines\n"
	   "usage: head [-n N | -c N] [file...]\n"
	   "  -n N  the first N lines (10 without it)\n"
	   "  -c N  the first N bytes")
{
	struct span sp = { 0 };
	int i = parse_span("head", argc, argv, &sp, false);

	if (i < 0)
		return 2;
	return for_each_input("head", argc, argv, i, head_fd, &sp);
}

/* The last lines, kept in a ring that holds exactly as many as wanted. */
static int tail_lines(int fd, const char *name, struct span *sp)
{
	struct lines l;
	char **ring = NULL;
	size_t *lens = NULL, len;
	long cap = 0, total = 0, line_no = 0;
	char *line;
	int err = 0;

	lines_init(&l, fd);
	while ((line = lines_next(&l, &len))) {
		line_no++;
		if (sp->from) {			/* +N: everything from line N */
			if (line_no >= sp->count)
				write_all(PT_STDOUT, line, len);
			continue;
		}
		if (!sp->count)
			continue;
		if (total < sp->count && total == cap) {	/* grow up to the count */
			long grown = cap ? cap * 2 : 16;

			if (grown > sp->count)
				grown = sp->count;
			char **r = pt_realloc(ring, grown * sizeof(*r));
			size_t *n = r ? pt_realloc(lens, grown * sizeof(*n)) : NULL;

			if (r)
				ring = r;
			if (!r || !n) {
				err = -ENOMEM;
				break;
			}
			lens = n;
			for (long k = cap; k < grown; k++)
				ring[k] = NULL;
			cap = grown;
		}
		long slot = total % sp->count;

		pt_free(ring[slot]);
		if (!(ring[slot] = pt_malloc(len))) {
			err = -ENOMEM;
			break;
		}
		memcpy(ring[slot], line, len);
		lens[slot] = len;
		total++;
	}
	for (long k = total > sp->count ? total - sp->count : 0; !err && k < total; k++)
		write_all(PT_STDOUT, ring[k % sp->count], lens[k % sp->count]);
	for (long k = 0; k < cap; k++)
		pt_free(ring[k]);
	pt_free(ring);
	pt_free(lens);
	if (!err)
		err = l.err;
	lines_free(&l);
	return err ? fail("tail", name, err) : 0;
}

static int tail_fd(int fd, const char *name, void *ctx)
{
	struct span *sp = ctx;
	int status;

	header(name, sp);
	if (sp->bytes) {
		struct pt_stat st;
		off_t size = name && !pt_stat(name, &st) ? (off_t)st.size : -1;

		/* a file can be skipped to the place; a pipe has to be read */
		if (size >= 0) {
			off_t at = sp->from ? (sp->count ? sp->count - 1 : 0)
				 : (size > sp->count ? size - sp->count : 0);

			if (pt_lseek(fd, at, SEEK_SET) < 0)
				return fail("tail", name, -EIO);
			status = copy_fd(fd, PT_STDOUT) < 0;
		} else {
			/* the last N bytes of a pipe: a ring of them */
			char *ring = pt_malloc(sp->count + 1);
			long head = 0, have = 0;
			char buf[512];
			ssize_t n;

			if (!ring)
				return fail("tail", name, -ENOMEM);
			while ((n = pt_read(fd, buf, sizeof(buf))) > 0) {
				for (ssize_t i = 0; i < n && sp->count; i++) {
					ring[head] = buf[i];
					head = (head + 1) % sp->count;
					have += have < sp->count;
				}
			}
			if (have == sp->count) {	/* oldest first: from head round */
				write_all(PT_STDOUT, ring + head, sp->count - head);
				write_all(PT_STDOUT, ring, head);
			} else {
				write_all(PT_STDOUT, ring, have);
			}
			pt_free(ring);
			status = 0;
		}
	} else {
		status = tail_lines(fd, name, sp);
	}
	/* -f: keep printing what is added until interrupted */
	while (sp->follow && !status && !pt_interrupted()) {
		ssize_t r = copy_fd(fd, PT_STDOUT);

		if (r < 0)
			return fail("tail", name, r);
		if (!r && pt_sleep_ms(500))
			break;
	}
	return status;
}

PT_PROGRAM(tail, "print the last lines\n"
	   "usage: tail [-f] [-n N | -n +N | -c N] [file...]\n"
	   "  -n N   the last N lines (10 without it)\n"
	   "  -n +N  everything from line N on\n"
	   "  -c N   the last N bytes\n"
	   "  -f     then keep printing what is added, until Ctrl-C")
{
	struct span sp = { 0 };
	int i = parse_span("tail", argc, argv, &sp, true);

	if (i < 0)
		return 2;
	return for_each_input("tail", argc, argv, i, tail_fd, &sp);
}

/* ------------------------------------------------------------ wc */

struct wc {
	uint32_t flags;
	long	 lines, words, bytes;
	int	 files;
	int	 width;
};

static void wc_print(struct wc *w, long lines, long words, long bytes, const char *name)
{
	bool l = FLAG(w->flags, 'l'), wd = FLAG(w->flags, 'w'), c = FLAG(w->flags, 'c');
	bool all = !l && !wd && !c;

	if (all || l)
		pt_printf("%*ld", w->width, lines);
	if (all || wd)
		pt_printf("%s%*ld", all || l ? " " : "", w->width, words);
	if (all || c)
		pt_printf("%s%*ld", all || l || wd ? " " : "", w->width, bytes);
	pt_printf("%s%s\n", name ? " " : "", name ? name : "");
}

static int wc_fd(int fd, const char *name, void *ctx)
{
	struct wc *w = ctx;
	char buf[512];
	long lines = 0, words = 0, bytes = 0;
	bool in_word = false;
	ssize_t n;

	while ((n = pt_read(fd, buf, sizeof(buf))) > 0) {
		bytes += n;
		for (ssize_t i = 0; i < n; i++) {
			bool space = isspace((unsigned char)buf[i]);

			lines += buf[i] == '\n';
			words += !space && !in_word;
			in_word = !space;
		}
	}
	wc_print(w, lines, words, bytes, name);
	w->lines += lines;
	w->words += words;
	w->bytes += bytes;
	w->files++;
	return n < 0 ? fail("wc", name, n) : 0;
}

/*
 * The columns are as wide as the biggest number can be -- the files' sizes
 * added up tell, before a byte is read -- or 7 when something is a pipe.
 * One count of one file is printed bare: `... | wc -l` is 3, not "      3",
 * which is what scripts compare against.
 */
static int wc_width(struct wc *w, int argc, char **argv, int first)
{
	int counts = !!FLAG(w->flags, 'l') + !!FLAG(w->flags, 'w') + !!FLAG(w->flags, 'c');
	uint64_t total = 0;
	int width = 1;

	if (counts == 1 && argc - first <= 1)
		return 1;
	if (first >= argc)
		return 7;
	for (int i = first; i < argc; i++) {
		struct pt_stat st;

		if (!strcmp(argv[i], "-"))
			return 7;
		if (!pt_stat(argv[i], &st))
			total += st.size;
	}
	for (; total >= 10; total /= 10)
		width++;
	return width;
}

PT_PROGRAM(wc, "count lines, words and bytes\nusage: wc [-lwc] [file...]")
{
	struct wc w = { 0 };
	int i = parse_flags("wc", argc, argv, "lwc", &w.flags);

	if (i < 0)
		return 2;
	w.width = wc_width(&w, argc, argv, i);
	int status = for_each_input("wc", argc, argv, i, wc_fd, &w);

	if (w.files > 1)
		wc_print(&w, w.lines, w.words, w.bytes, "total");
	return status;
}

/* ------------------------------------------------------------ grep */

#define GREP_MAX_PATTERNS	32

struct grep {
	struct regex	*re[GREP_MAX_PATTERNS];
	int		 npat;
	bool		 invert, word, whole, count, list, list_without, quiet;
	bool		 silent, numbers, only, recursive, text, names, tty;
	bool		 here;			/* -r with no operand: "file", not "./file" */
	long		 max, before, after;
	long		 selected;		/* lines picked, all files */
	bool		 error;
	/* context: the lines before a match, and how many after are still owed */
	char		**ring;
	size_t		*ring_len;
	long		*ring_no;
	long		 ring_n, owed, last_printed;
};

static bool word_char(unsigned char c)
{
	return isalnum(c) || c == '_' || c >= 0x80;
}

/* Past the character at `at`: one byte, or a whole UTF-8 sequence. */
static size_t next_char(const char *s, size_t len, size_t at)
{
	at++;
	while (at < len && ((unsigned char)s[at] & 0xc0) == 0x80)
		at++;
	return at;
}

/*
 * Pattern k's first match at or after `from` -- with -w, the first that
 * is a whole word by GNU's rule: when the longest match at a place does
 * not end a word, shorter ones there are tried before looking further on,
 * and an empty match counts where both sides are not word characters.
 */
static bool grep_one(struct grep *g, int k, const char *line, size_t len, size_t from,
		     long *so, long *eo)
{
	struct re_match m;

	while (from <= len && re_search(g->re[k], line, len, from, false, &m)) {
		long s = m.so[0], e = m.eo[0];

		if (!g->word) {
			*so = s;
			*eo = e;
			return true;
		}
		if (!s || !word_char(line[s - 1])) {
			for (;;) {
				if ((size_t)e == len || !word_char(line[e])) {
					*so = s;
					*eo = e;
					return true;
				}
				/* a shorter one here, but not an empty one */
				if (e == s || (e = re_longest_at(g->re[k], line, len, s, e - 1, false)) <= s)
					break;
			}
		}
		from = next_char(line, len, s);
	}
	return false;
}

/* The leftmost match of any pattern in line, at or after from: its span. */
static bool grep_find(struct grep *g, const char *line, size_t len, size_t from,
		      long *so, long *eo)
{
	bool found = false;

	for (int k = 0; k < g->npat; k++) {
		long s, e;

		if (grep_one(g, k, line, len, from, &s, &e) &&
		    (!found || s < *so || (s == *so && e > *eo))) {
			*so = s;
			*eo = e;
			found = true;
		}
	}
	return found;
}

static void grep_prefix(struct grep *g, const char *name, long no, char sep)
{
	if (g->names)
		pt_printf("%s%s%c%s", DIM(g->tty), name, sep, PLAIN(g->tty));
	if (g->numbers)
		pt_printf("%s%ld%c%s", DIM(g->tty), no, sep, PLAIN(g->tty));
}

/* A line as grep prints it: the matches picked out on a terminal. */
static void grep_print(struct grep *g, const char *name, long no, const char *line,
		       size_t len, char sep)
{
	size_t at = 0;
	long so, eo;

	if (g->last_printed && no > g->last_printed + 1 && (g->before || g->after))
		pt_puts("--\n");
	g->last_printed = no;
	grep_prefix(g, name, no, sep);
	if (g->tty && !g->invert && sep == ':') {
		while (at <= len && grep_find(g, line, len, at, &so, &eo) && eo > so) {
			write_all(PT_STDOUT, line + at, so - at);
			pt_printf("%s%.*s%s", BOLD(g->tty), (int)(eo - so), line + so, PLAIN(g->tty));
			at = eo;
		}
	}
	write_all(PT_STDOUT, line + at, len - at);
	pt_puts("\n");
}

static void ring_push(struct grep *g, const char *line, size_t len, long no)
{
	if (!g->before)
		return;
	long slot = no % g->before;
	char *copy = pt_malloc(len + 1);

	if (!copy)
		return;
	memcpy(copy, line, len);
	pt_free(g->ring[slot]);
	g->ring[slot] = copy;
	g->ring_len[slot] = len;
	g->ring_no[slot] = no;
}

static int grep_fd(int fd, const char *name, void *ctx)
{
	struct grep *g = ctx;
	struct lines l;
	long no = 0, count = 0;
	size_t len;
	char *line;
	bool binary = false;
	const char *shown = name ? name : "(standard input)";

	g->owed = 0;
	g->last_printed = 0;
	for (long k = 0; k < g->before; k++) {
		pt_free(g->ring[k]);
		g->ring[k] = NULL;
	}
	lines_init(&l, fd);
	while (count != g->max && (line = lines_next(&l, &len)) && !pt_interrupted()) {
		long so, eo;

		no++;
		if (len && line[len - 1] == '\n')
			len--;
		bool hit;

		if (g->word || g->whole) {
			hit = grep_find(g, line, len, 0, &so, &eo);
			if (g->whole && hit)
				hit = so == 0 && (size_t)eo == len;
		} else {
			/* only whether: the quick way, without where */
			hit = false;
			for (int k = 0; k < g->npat && !hit; k++)
				hit = re_search(g->re[k], line, len, 0, false, NULL);
		}
		if (hit == g->invert) {
			if (g->owed > 0 && !binary && !g->count && !g->list && !g->list_without && !g->quiet) {
				grep_print(g, shown, no, line, len, '-');
				g->owed--;
			} else {
				ring_push(g, line, len, no);
			}
			continue;
		}
		count++;
		g->selected++;
		if (g->quiet || g->list || g->list_without)
			break;			/* the answer is known */
		if (!g->text && !binary && memchr(line, '\0', len))
			binary = true;
		if (binary) {
			if (!g->count)
				break;
		} else if (!g->count) {
			/* the lines before, then this one */
			for (long k = no - g->before; k < no; k++) {
				long slot = k > 0 ? k % g->before : 0;

				if (k > 0 && k > g->last_printed && g->ring[slot] && g->ring_no[slot] == k)
					grep_print(g, shown, k, g->ring[slot], g->ring_len[slot], '-');
			}
			if (g->only) {
				size_t at = 0;

				while (at <= len && grep_find(g, line, len, at, &so, &eo)) {
					if (eo > so) {
						grep_prefix(g, shown, no, ':');
						pt_printf("%s%.*s%s\n", BOLD(g->tty), (int)(eo - so),
							  line + so, PLAIN(g->tty));
					}
					at = eo > so ? (size_t)eo : next_char(line, len, so);
				}
				g->last_printed = no;
			} else {
				grep_print(g, shown, no, line, len, ':');
			}
			g->owed = g->after;
		}
	}
	if (l.err && !g->silent)
		fail("grep", shown, l.err);
	g->error |= l.err != 0;
	lines_free(&l);
	if (g->quiet)
		return 0;
	if (g->count) {
		if (g->names)
			pt_printf("%s%s:%s", DIM(g->tty), shown, PLAIN(g->tty));
		pt_printf("%ld\n", count);
	}
	if ((g->list && count) || (g->list_without && !count))
		pt_printf("%s\n", shown);
	else if (binary && count && !g->count)
		pt_printf("Binary file %s matches\n", shown);
	return 0;
}

static int grep_path(struct grep *g, const char *path);

static int grep_dir(struct grep *g, const char *dir)
{
	struct pt_dirent *ent = pt_malloc(sizeof(*ent));
	char *path = pt_malloc(PT_PATH_MAX);
	pt_dir_t *d;
	int err, status = 0;

	if (!ent || !path) {
		pt_free(ent);
		pt_free(path);
		return fail("grep", dir, -ENOMEM);
	}
	if ((err = pt_opendir(dir, &d))) {
		pt_free(ent);
		pt_free(path);
		return g->silent ? 1 : fail("grep", dir, err);
	}
	while (pt_readdir(d, ent) == 1 && !pt_interrupted() && !(g->quiet && g->selected)) {
		if (g->here && !strcmp(dir, "."))
			snprintf(path, PT_PATH_MAX, "%s", ent->name);
		else if (!join_path(dir, ent->name, path, PT_PATH_MAX))
			continue;
		status |= grep_path(g, path);
	}
	pt_closedir(d);
	pt_free(ent);
	pt_free(path);
	return status;
}

static int grep_path(struct grep *g, const char *path)
{
	struct pt_stat st;
	int err, fd;

	if (!strcmp(path, "-"))
		return grep_fd(PT_STDIN, NULL, g);
	if ((err = pt_stat(path, &st)))
		return g->silent ? 1 : fail("grep", path, err);
	if (st.is_dir) {
		if (g->recursive)
			return grep_dir(g, path);
		return g->silent ? 1 : fail("grep", path, -EISDIR);
	}
	if ((fd = pt_open(path, O_RDONLY)) < 0)
		return g->silent ? 1 : fail("grep", path, fd);
	grep_fd(fd, path, g);
	pt_close(fd);
	return 0;
}

/* -F: every character as itself, by escaping everything special. */
static char *fixed_pattern(const char *s, size_t n)
{
	char *out = pt_malloc(2 * n + 1), *o = out;

	if (!out)
		return NULL;
	for (size_t i = 0; i < n; i++) {
		if (strchr(".[\\*^$+?{}()|", s[i]))
			*o++ = '\\';
		*o++ = s[i];
	}
	*o = '\0';
	return out;
}

static int grep_add(struct grep *g, const char *text, int flags, bool fixed)
{
	/* a pattern with newlines in it is one pattern per line */
	for (const char *p = text;;) {
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - p) : strlen(p);
		char *pat = fixed ? fixed_pattern(p, n) : pt_malloc(n + 1), err[96];
		int ret;

		if (!pat)
			return -ENOMEM;
		if (!fixed) {
			memcpy(pat, p, n);
			pat[n] = '\0';
		}
		if (g->npat == GREP_MAX_PATTERNS) {
			pt_free(pat);
			pt_dprintf(PT_STDERR, "grep: at most %d patterns\n", GREP_MAX_PATTERNS);
			return -EINVAL;
		}
		ret = re_compile(&g->re[g->npat], pat, flags | RE_NOSUB, err, sizeof(err));
		pt_free(pat);
		if (ret) {
			pt_dprintf(PT_STDERR, "grep: %s\n", ret == -EINVAL ? err : pt_strerror(ret));
			return ret;
		}
		g->npat++;
		if (!nl)
			return 0;
		p = nl + 1;
	}
}

PT_PROGRAM(grep, "print lines that match a pattern\n"
	   "usage: grep [options] pattern [file...]\n"
	   "  -E  extended regular expressions   -F  fixed text, not a pattern\n"
	   "  -i  ignore case     -v  lines that do not match\n"
	   "  -w  whole words     -x  whole lines\n"
	   "  -n  line numbers    -c  count only    -o  only the matches\n"
	   "  -l  names of files that match   -L  of files that do not\n"
	   "  -q  quiet: only the exit status  -s  no errors about files\n"
	   "  -r  search directories too  -H/-h  with/without file names\n"
	   "  -e PATTERN  another pattern  -m N  stop after N matches\n"
	   "  -A N / -B N / -C N  lines after / before / around each match\n"
	   "  -a  treat binary files as text\n"
	   "Patterns are POSIX basic regular expressions: . * [...] ^ $ \\( \\)\n"
	   "\\{m,n\\} and \\+ \\? \\|, \\< \\> \\b \\w \\s; with -E, + ? | ( ) { }.")
{
	struct grep g = { .max = -1 };
	const char *patterns[GREP_MAX_PATTERNS];
	int npatterns = 0, flags = 0, c, status = 0, names = -1;
	bool fixed = false;
	struct opt o = { .ind = 1 };

	while ((c = getopt_pt(&o, "grep", argc, argv, "EFGivwxclLqsnHhorae:m:A:B:C:")) != -1) {
		long n;

		switch (c) {
		case 'E': flags |= RE_EXTENDED; fixed = false; break;
		case 'F': fixed = true; break;
		case 'G': flags &= ~RE_EXTENDED; fixed = false; break;
		case 'i': flags |= RE_ICASE; break;
		case 'v': g.invert = true; break;
		case 'w': g.word = true; break;
		case 'x': g.whole = true; break;
		case 'c': g.count = true; break;
		case 'l': g.list = true; break;
		case 'L': g.list_without = true; break;
		case 'q': g.quiet = true; break;
		case 's': g.silent = true; break;
		case 'n': g.numbers = true; break;
		case 'H': names = 1; break;
		case 'h': names = 0; break;
		case 'o': g.only = true; break;
		case 'r': g.recursive = true; break;
		case 'a': g.text = true; break;
		case 'e':
			if (npatterns == GREP_MAX_PATTERNS) {
				pt_dprintf(PT_STDERR, "grep: at most %d patterns\n", GREP_MAX_PATTERNS);
				return 2;
			}
			patterns[npatterns++] = o.arg;
			break;
		case 'm': case 'A': case 'B': case 'C':
			if (parse_long(o.arg, &n) || n < 0) {
				pt_dprintf(PT_STDERR, "grep: -%c %s: not a number\n", c, o.arg);
				return 2;
			}
			if (c == 'm')
				g.max = n;
			if (c == 'A' || c == 'C')
				g.after = n;
			if (c == 'B' || c == 'C')
				g.before = n;
			break;
		default:
			return 2;
		}
	}
	if (!npatterns) {
		if (o.ind >= argc) {
			pt_dprintf(PT_STDERR, "usage: grep [options] pattern [file...] (try 'help grep')\n");
			return 2;
		}
		patterns[npatterns++] = argv[o.ind++];
	}
	for (int i = 0; i < npatterns; i++)
		if (grep_add(&g, patterns[i], fixed ? RE_EXTENDED | (flags & RE_ICASE) : flags, fixed))
			return 2;
	if (g.before && !(g.ring = pt_calloc(g.before, sizeof(*g.ring))))
		return fail("grep", NULL, -ENOMEM) + 1;
	if (g.before) {
		g.ring_len = pt_calloc(g.before, sizeof(*g.ring_len));
		g.ring_no = pt_calloc(g.before, sizeof(*g.ring_no));
		if (!g.ring_len || !g.ring_no)
			return fail("grep", NULL, -ENOMEM) + 1;
	}
	g.tty = pt_isatty(PT_STDOUT);
	g.names = names >= 0 ? names : argc - o.ind > 1 || g.recursive;

	g.here = g.recursive && o.ind >= argc;
	if (o.ind >= argc)
		status = g.recursive ? grep_path(&g, ".") : grep_fd(PT_STDIN, NULL, &g);
	for (int i = o.ind; i < argc && !(g.quiet && g.selected); i++)
		status |= grep_path(&g, argv[i]);

	for (int i = 0; i < g.npat; i++)
		re_free(g.re[i]);
	for (long k = 0; k < g.before; k++)
		pt_free(g.ring[k]);
	pt_free(g.ring);
	pt_free(g.ring_len);
	pt_free(g.ring_no);
	if (g.quiet && g.selected)
		return 0;
	return status || g.error ? 2 : g.selected ? 0 : 1;
}

/* ------------------------------------------------------------ cmp */

PT_PROGRAM(cmp, "compare two files byte by byte\n"
	   "usage: cmp [-ls] file1 file2\n"
	   "  -l  every difference: byte number and both values in octal\n"
	   "  -s  say nothing: only the exit status (0 same, 1 different)")
{
	uint32_t flags;
	int i = parse_flags("cmp", argc, argv, "ls", &flags), fd[2] = { -1, -1 }, status = 0;
	char a[512], b[512];
	long byte = 1, line = 1;

	if (i < 0 || argc - i != 2) {
		pt_dprintf(PT_STDERR, "usage: cmp [-ls] file1 file2\n");
		return 2;
	}
	for (int k = 0; k < 2; k++) {
		fd[k] = !strcmp(argv[i + k], "-") ? PT_STDIN : pt_open(argv[i + k], O_RDONLY);
		if (fd[k] < 0) {
			if (!FLAG(flags, 's'))
				fail("cmp", argv[i + k], fd[k]);
			if (k)
				pt_close(fd[0]);
			return 2;
		}
	}
	for (;;) {
		ssize_t na = 0, nb = 0, r;

		/* equal amounts from each, so the positions stay together */
		while (na < (ssize_t)sizeof(a) && (r = pt_read(fd[0], a + na, sizeof(a) - na)) > 0)
			na += r;
		while (nb < (ssize_t)sizeof(b) && (r = pt_read(fd[1], b + nb, sizeof(b) - nb)) > 0)
			nb += r;
		ssize_t n = na < nb ? na : nb, k;

		for (k = 0; k < n; k++, byte++) {
			if (a[k] != b[k]) {
				status = 1;
				if (FLAG(flags, 's'))
					goto done;
				if (!FLAG(flags, 'l')) {
					pt_printf("%s %s differ: byte %ld, line %ld\n",
						  argv[i], argv[i + 1], byte, line);
					goto done;
				}
				pt_printf("%ld %3o %3o\n", byte, (unsigned char)a[k], (unsigned char)b[k]);
			}
			line += a[k] == '\n';
		}
		if (na != nb) {
			if (!FLAG(flags, 's'))
				pt_dprintf(PT_STDERR, "cmp: EOF on %s after byte %ld\n",
					   argv[i + (na > nb)], byte - 1);
			status = 1;
			goto done;
		}
		if (!na)
			break;
	}
done:
	for (int k = 0; k < 2; k++)
		if (fd[k] != PT_STDIN)
			pt_close(fd[k]);
	return status;
}

/* ------------------------------------------------------------ hexdump */

static int hexdump_fd(int fd, const char *name, void *ctx)
{
	int cols, rows;
	unsigned char buf[16];
	unsigned long offset = 0;
	ssize_t n;

	pt_tty_size(PT_STDOUT, &cols, &rows);
	int width = cols >= 78 || !pt_isatty(PT_STDOUT) ? 16 : 8;

	for (;;) {
		ssize_t r = 1;

		/* a whole row: a read may return less than asked */
		for (n = 0; n < width && (r = pt_read(fd, buf + n, width - n)) > 0;)
			n += r;
		if (n <= 0 || pt_interrupted())
			break;
		char line[128];
		int o = snprintf(line, sizeof(line), "%08lx ", offset);
		for (int i = 0; i < width; i++)
			o += i < n ? snprintf(line + o, sizeof(line) - o, " %02x", buf[i])
				   : snprintf(line + o, sizeof(line) - o, "   ");
		o += snprintf(line + o, sizeof(line) - o, "  |");
		for (int i = 0; i < n; i++)
			line[o++] = buf[i] >= 0x20 && buf[i] < 0x7f ? buf[i] : '.';
		o += snprintf(line + o, sizeof(line) - o, "|\n");
		pt_write(PT_STDOUT, line, o);
		offset += n;
	}
	pt_printf("%08lx\n", offset);
	return 0;
}

PT_PROGRAM(hexdump, "show a file's bytes in hex\nusage: hexdump [file...]")
{
	return for_each_input("hexdump", argc, argv, 1, hexdump_fd, NULL);
}

/* ------------------------------------------------------------ more */

/* Keys come from stderr: stdin may be the pipe being paged. */
#define KEYS	PT_STDERR

/* Text goes out in runs, not a byte per system call: a page is a few writes. */
static int more_fd(int fd, const char *name, void *ctx)
{
	int cols, rows, row = 0, col = 0;
	bool quit = false;
	char buf[512];
	ssize_t n;

	pt_tty_size(PT_STDOUT, &cols, &rows);
	pt_tty_raw(KEYS, true);
	while (!quit && (n = pt_read(fd, buf, sizeof(buf))) > 0) {
		ssize_t run = 0;			/* start of what is not written yet */

		for (ssize_t i = 0; i < n && !quit; i++) {
			char c = buf[i];
			bool wrapped = c != '\n' && !(((unsigned char)c & 0xc0) == 0x80) && ++col == cols;

			if (c == '\n' || wrapped) {
				write_all(PT_STDOUT, buf + run, i + 1 - run);
				run = i + 1;
				if (wrapped)
					pt_puts("\n");
				col = 0;
				row++;
			}
			if (row < rows - 1)
				continue;

			pt_puts("\x1b[7m--More-- space: page  enter: line  q: quit\x1b[0m");
			int k = pt_readkey(KEYS);
			pt_puts("\r\x1b[K");
			if (k == 'q' || k == 'Q' || k == PT_KEY_ESC || k < 0)
				quit = true;
			else if (k == '\r' || k == '\n' || k == PT_KEY_DOWN)
				row = rows - 2;
			else
				row = 0;
		}
		if (!quit && run < n)
			write_all(PT_STDOUT, buf + run, n - run);
	}
	pt_tty_raw(KEYS, false);
	return 0;
}

static int cat_fd(int fd, const char *name, void *ctx)
{
	ssize_t r = copy_fd(fd, PT_STDOUT);

	return r < 0 ? fail("more", name, r) : 0;
}

PT_PROGRAM(more, "show text one screen at a time\nusage: more [file...]\nspace next page, enter next line, q quit")
{
	if (!pt_isatty(KEYS) || !pt_isatty(PT_STDOUT))
		return for_each_input("more", argc, argv, 1, cat_fd, NULL);
	return for_each_input("more", argc, argv, 1, more_fd, NULL);
}
