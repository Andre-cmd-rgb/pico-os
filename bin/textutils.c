/*
 * Text programs: echo head tail wc grep hexdump more
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define LINE_MAX_LEN	1024

/* Line reader over an fd: returns length, 0 at end. Lines longer than the
 * buffer are returned in pieces. */
struct reader {
	int	fd;
	char	buf[512];
	size_t	pos, len;
	bool	eof;
};

static ssize_t read_line(struct reader *r, char *line, size_t size)
{
	size_t n = 0;

	while (n + 1 < size) {
		if (r->pos == r->len) {
			if (r->eof)
				break;
			ssize_t got = pt_read(r->fd, r->buf, sizeof(r->buf));
			if (got <= 0) {
				r->eof = true;
				if (got < 0 && !n)
					return got;
				break;
			}
			r->pos = 0;
			r->len = got;
		}
		char c = r->buf[r->pos++];
		line[n++] = c;
		if (c == '\n')
			break;
	}
	line[n] = '\0';
	return n;
}

/* Runs fn on each named file, or on stdin when there are none. */
static int for_each_input(const char *prog, int argc, char **argv, int first,
			  int (*fn)(int fd, const char *name, void *ctx), void *ctx)
{
	int status = 0;

	if (first >= argc)
		return fn(PT_STDIN, NULL, ctx);
	for (int i = first; i < argc && !pt_interrupted(); i++) {
		int fd = pt_open(argv[i], O_RDONLY);
		if (fd < 0) {
			status = fail(prog, argv[i], fd);
			continue;
		}
		status |= fn(fd, argv[i], ctx);
		pt_close(fd);
	}
	return status;
}

static int count_arg(const char *prog, int *i, int argc, char **argv, long *count)
{
	if (*i < argc && !strcmp(argv[*i], "-n")) {
		if (*i + 1 >= argc) {
			pt_dprintf(PT_STDERR, "%s: -n needs a number\n", prog);
			return -1;
		}
		*count = strtol(argv[*i + 1], NULL, 10);
		*i += 2;
	} else if (*i < argc && argv[*i][0] == '-' && isdigit((unsigned char)argv[*i][1])) {
		*count = strtol(argv[*i] + 1, NULL, 10);
		(*i)++;
	}
	return 0;
}

/* ------------------------------------------------------------ echo */

static void unescape(const char *s)
{
	for (; *s; s++) {
		char c = *s;
		if (c == '\\' && s[1]) {
			switch (*++s) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'e': c = 0x1b; break;
			case '\\': c = '\\'; break;
			default: c = *s; break;
			}
		}
		pt_write(PT_STDOUT, &c, 1);
	}
}

PT_PROGRAM(echo, "print arguments\nusage: echo [-ne] [text...]\n  -n  no trailing newline\n  -e  interpret \\n \\t \\e \\\\")
{
	uint32_t flags = 0;
	int i = 1;

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
	for (int first = i; i < argc; i++) {
		if (i > first)
			pt_puts(" ");
		if (FLAG(flags, 'e'))
			unescape(argv[i]);
		else
			pt_puts(argv[i]);
	}
	if (!FLAG(flags, 'n'))
		pt_puts("\n");
	return 0;
}

/* ------------------------------------------------------------ head, tail */

static int head_fd(int fd, const char *name, void *ctx)
{
	long left = *(long *)ctx;
	struct reader r = { .fd = fd };
	char line[LINE_MAX_LEN];
	ssize_t n;

	while (left > 0 && (n = read_line(&r, line, sizeof(line))) > 0) {
		pt_write(PT_STDOUT, line, n);
		if (line[n - 1] == '\n')
			left--;
	}
	return 0;
}

PT_PROGRAM(head, "print the first lines\nusage: head [-n count] [file...]\nDefault is 10 lines.")
{
	long count = 10;
	int i = 1;

	if (count_arg("head", &i, argc, argv, &count))
		return 2;
	return for_each_input("head", argc, argv, i, head_fd, &count);
}

static int tail_fd(int fd, const char *name, void *ctx)
{
	long count = *(long *)ctx;
	char **ring = pt_calloc(count, sizeof(*ring));
	struct reader r = { .fd = fd };
	char line[LINE_MAX_LEN];
	long total = 0;
	ssize_t n;

	if (!ring)
		return fail("tail", name, -ENOMEM);
	while ((n = read_line(&r, line, sizeof(line))) > 0) {
		pt_free(ring[total % count]);
		ring[total % count] = pt_strdup(line);
		total++;
	}
	long start = total > count ? total - count : 0;
	for (long k = start; k < total; k++) {
		if (ring[k % count])
			pt_puts(ring[k % count]);
		pt_free(ring[k % count]);
	}
	pt_free(ring);
	return 0;
}

PT_PROGRAM(tail, "print the last lines\nusage: tail [-n count] [file...]\nDefault is 10 lines.")
{
	long count = 10;
	int i = 1;

	if (count_arg("tail", &i, argc, argv, &count))
		return 2;
	if (count <= 0)
		return 0;
	return for_each_input("tail", argc, argv, i, tail_fd, &count);
}

/* ------------------------------------------------------------ wc */

struct wc {
	uint32_t flags;
	long	 lines, words, bytes;
	int	 files;
};

static void wc_print(struct wc *w, long lines, long words, long bytes, const char *name)
{
	bool all = !(w->flags & ((1u << ('l' - 'a')) | (1u << ('w' - 'a')) | (1u << ('c' - 'a'))));

	if (all || FLAG(w->flags, 'l'))
		pt_printf("%7ld ", lines);
	if (all || FLAG(w->flags, 'w'))
		pt_printf("%7ld ", words);
	if (all || FLAG(w->flags, 'c'))
		pt_printf("%7ld ", bytes);
	pt_printf("%s\n", name ? name : "");
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

PT_PROGRAM(wc, "count lines, words and bytes\nusage: wc [-lwc] [file...]")
{
	struct wc w = { 0 };
	int i = parse_flags("wc", argc, argv, "lwc", &w.flags);

	if (i < 0)
		return 2;
	int status = for_each_input("wc", argc, argv, i, wc_fd, &w);
	if (w.files > 1)
		wc_print(&w, w.lines, w.words, w.bytes, "total");
	return status;
}

/* ------------------------------------------------------------ grep */

struct grep {
	const char *pattern;
	uint32_t    flags;
	bool	    names;
	long	    matches;
};

static bool contains(const char *line, const char *pattern, bool fold)
{
	if (!fold)
		return strstr(line, pattern);
	size_t n = strlen(pattern);
	for (; *line; line++)
		if (!strncasecmp(line, pattern, n))
			return true;
	return !n;
}

static int grep_fd(int fd, const char *name, void *ctx)
{
	struct grep *g = ctx;
	struct reader r = { .fd = fd };
	char line[LINE_MAX_LEN];
	long number = 0, count = 0;
	ssize_t n;

	while ((n = read_line(&r, line, sizeof(line))) > 0 && !pt_interrupted()) {
		number++;
		bool hit = contains(line, g->pattern, FLAG(g->flags, 'i')) != (bool)FLAG(g->flags, 'v');
		if (!hit)
			continue;
		count++;
		if (FLAG(g->flags, 'c'))
			continue;
		if (g->names)
			pt_printf("\x1b[2m%s:\x1b[0m", name);
		if (FLAG(g->flags, 'n'))
			pt_printf("\x1b[2m%ld:\x1b[0m", number);
		pt_puts(line);
		if (line[n - 1] != '\n')
			pt_puts("\n");
	}
	if (FLAG(g->flags, 'c'))
		pt_printf("%s%s%ld\n", g->names ? name : "", g->names ? ":" : "", count);
	g->matches += count;
	return 0;
}

PT_PROGRAM(grep, "print lines containing text\n"
	   "usage: grep [-invc] text [file...]\n"
	   "  -i  ignore case   -n  line numbers\n"
	   "  -v  non-matching  -c  count only\n"
	   "Plain text match, no regular expressions.")
{
	struct grep g = { 0 };
	int i = parse_flags("grep", argc, argv, "invc", &g.flags);

	if (i < 0 || i >= argc) {
		pt_dprintf(PT_STDERR, "usage: grep [-invc] text [file...]\n");
		return 2;
	}
	g.pattern = argv[i++];
	g.names = argc - i > 1;
	int status = for_each_input("grep", argc, argv, i, grep_fd, &g);
	return status ? 2 : g.matches ? 0 : 1;
}

/* ------------------------------------------------------------ hexdump */

static int hexdump_fd(int fd, const char *name, void *ctx)
{
	int cols, rows;
	unsigned char buf[16];
	unsigned long offset = 0;
	ssize_t n;

	pt_tty_size(PT_STDOUT, &cols, &rows);
	int width = cols >= 78 ? 16 : 8;

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

PT_PROGRAM(more, "show text one screen at a time\nusage: more [file...]\nspace next page, enter next line, q quit")
{
	if (!pt_isatty(KEYS))
		return for_each_input("more", argc, argv, 1, head_fd, &(long) { 1L << 30 });
	return for_each_input("more", argc, argv, 1, more_fd, NULL);
}
