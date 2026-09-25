#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

int fail(const char *prog, const char *what, int err)
{
	if (what)
		pt_dprintf(PT_STDERR, "%s: %s: %s\n", prog, what, pt_strerror(err));
	else
		pt_dprintf(PT_STDERR, "%s: %s\n", prog, pt_strerror(err));
	return 1;
}

int parse_flags(const char *prog, int argc, char **argv, const char *allowed, uint32_t *flags)
{
	int i = 1;

	*flags = 0;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "--"))
			return i + 1;
		for (const char *c = argv[i] + 1; *c; c++) {
			if (*c < 'a' || *c > 'z' || !strchr(allowed, *c)) {
				pt_dprintf(PT_STDERR, "%s: unknown option -%c (try 'help %s')\n", prog, *c, prog);
				return -1;
			}
			*flags |= 1u << (*c - 'a');
		}
	}
	return i;
}

int getopt_pt(struct opt *o, const char *prog, int argc, char **argv, const char *spec)
{
	const char *at;
	char c;

	o->arg = NULL;
	if (!o->pos) {
		if (o->ind >= argc || argv[o->ind][0] != '-' || !argv[o->ind][1])
			return -1;
		if (!strcmp(argv[o->ind], "--")) {
			o->ind++;
			return -1;
		}
		o->pos = 1;
	}
	c = argv[o->ind][o->pos++];
	at = c != ':' ? strchr(spec, c) : NULL;
	if (!at) {
		pt_dprintf(PT_STDERR, "%s: unknown option -%c (try 'help %s')\n", prog, c, prog);
		o->pos = 0;
		o->ind++;
		return '?';
	}
	if (at[1] == ':') {
		if (argv[o->ind][o->pos]) {
			o->arg = argv[o->ind] + o->pos;	/* -n5 */
		} else if (o->ind + 1 < argc) {
			o->arg = argv[++o->ind];	/* -n 5 */
		} else {
			pt_dprintf(PT_STDERR, "%s: -%c needs an argument\n", prog, c);
			o->pos = 0;
			o->ind++;
			return '?';
		}
		o->pos = 0;
		o->ind++;
		return c;
	}
	if (!argv[o->ind][o->pos]) {
		o->pos = 0;
		o->ind++;
	}
	return c;
}

int write_all(int fd, const char *s, size_t n)
{
	while (n) {
		ssize_t r = pt_write(fd, s, n);
		if (r <= 0)
			return r ? (int)r : -EIO;
		s += r;
		n -= r;
	}
	return 0;
}

uint32_t crc32_of(uint32_t crc, const void *data, size_t n)
{
	const uint8_t *p = data;

	crc = ~crc;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
	}
	return ~crc;
}

ssize_t copy_fd(int in, int out)
{
	char *buf = pt_malloc(BUF_SIZE);
	ssize_t total = 0, n;

	if (!buf)
		return -ENOMEM;
	while ((n = pt_read(in, buf, BUF_SIZE)) > 0) {
		int err = write_all(out, buf, n);
		if (err) {
			n = err;
			break;
		}
		total += n;
	}
	pt_free(buf);
	return n < 0 ? n : total;
}

void human_size(uint64_t bytes, char *out, size_t size)
{
	static const char units[] = "BKMGT";
	double v = bytes;
	int u = 0;

	while (v >= 1024 && u < 4) {
		v /= 1024;
		u++;
	}
	if (u == 0)
		snprintf(out, size, "%lluB", (unsigned long long)bytes);
	else
		snprintf(out, size, v < 10 ? "%.1f%c" : "%.0f%c", v, units[u]);
}

const char *join_path(const char *dir, const char *name, char *out, size_t size)
{
	size_t len = strlen(dir);
	int n = snprintf(out, size, "%s%s%s", dir, len && dir[len - 1] == '/' ? "" : "/", name);

	return n >= 0 && (size_t)n < size ? out : NULL;
}

/* ------------------------------------------------------------ lines */

#define LINES_READ	4096

void lines_init(struct lines *l, int fd)
{
	memset(l, 0, sizeof(*l));
	l->fd = fd;
}

char *lines_next(struct lines *l, size_t *len)
{
	for (;;) {
		char *nl = l->end > l->start ? memchr(l->buf + l->start, '\n', l->end - l->start) : NULL;

		if (nl || (l->eof && l->end > l->start)) {
			char *line = l->buf + l->start;

			*len = nl ? (size_t)(nl + 1 - line) : l->end - l->start;
			l->start += *len;
			return line;
		}
		if (l->eof || l->err)
			return NULL;
		/* room for another read: the unread part to the front, grown if full */
		if (l->start) {
			memmove(l->buf, l->buf + l->start, l->end - l->start);
			l->end -= l->start;
			l->start = 0;
		}
		if (l->cap - l->end < LINES_READ) {
			size_t cap = l->cap ? l->cap * 2 : 2 * LINES_READ;
			char *grown = pt_realloc(l->buf, cap);

			if (!grown) {
				l->err = -ENOMEM;
				return NULL;
			}
			l->buf = grown;
			l->cap = cap;
		}
		ssize_t n = pt_read(l->fd, l->buf + l->end, l->cap - l->end);

		if (n < 0)
			l->err = (int)n;
		else if (n == 0)
			l->eof = true;
		else
			l->end += n;
	}
}

void lines_free(struct lines *l)
{
	pt_free(l->buf);
	l->buf = NULL;
	l->cap = l->start = l->end = 0;
}

int for_each_input(const char *prog, int argc, char **argv, int first,
		   int (*fn)(int fd, const char *name, void *ctx), void *ctx)
{
	int status = 0;

	if (first >= argc)
		return fn(PT_STDIN, NULL, ctx);
	for (int i = first; i < argc && !pt_interrupted(); i++) {
		if (!strcmp(argv[i], "-")) {
			status |= fn(PT_STDIN, "-", ctx);
			continue;
		}
		int fd = pt_open(argv[i], O_RDONLY);

		if (fd < 0) {
			status |= fail(prog, argv[i], fd);
			continue;
		}
		status |= fn(fd, argv[i], ctx);
		pt_close(fd);
	}
	return status;
}

int parse_long(const char *s, long *out)
{
	char *end;
	long v;

	if (!s || !*s)
		return -EINVAL;
	v = strtol(s, &end, 10);
	if (*end)
		return -EINVAL;
	*out = v;
	return 0;
}

int escape_char(const char **s)
{
	const char *p = *s;
	int c = (unsigned char)*p++, v, n;

	switch (c) {
	case 'n': c = '\n'; break;
	case 't': c = '\t'; break;
	case 'a': c = '\a'; break;
	case 'b': c = '\b'; break;
	case 'f': c = '\f'; break;
	case 'r': c = '\r'; break;
	case 'v': c = '\v'; break;
	case 'e': c = 0x1b; break;
	case 'x':
		for (v = 0, n = 0; n < 2 && isxdigit((unsigned char)*p); n++, p++)
			v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (*p | 32) - 'a' + 10);
		c = n ? v : 'x';
		break;
	case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
		/* \NNN, up to three digits */
		for (v = c - '0', n = 1; n < 3 && *p >= '0' && *p <= '7'; n++, p++)
			v = v * 8 + (*p - '0');
		c = v & 0xff;
		break;
	case '\0':
		c = '\\';			/* a backslash at the very end */
		p--;
		break;
	}
	*s = p;
	return c;
}

int escape_char_0(const char **s)
{
	int v = 0;

	if (**s != '0')
		return escape_char(s);
	(*s)++;
	for (int n = 0; n < 3 && **s >= '0' && **s <= '7'; n++)
		v = v * 8 + (*(*s)++ - '0');
	return v & 0xff;
}

/* ------------------------------------------------------------ commands */

/*
 * A command run to its end: spawned with this program's standard files,
 * then waited for. A child starts a process group of its own, so Ctrl-C
 * reaches this program and not it: while waiting, an interruption is
 * passed on, and the child is waited for again before this one gives up.
 */
int run_command(int argc, char *const argv[])
{
	struct pt_spawn req = {
		.cmd = argv[0], .argc = argc, .argv = argv, .fd = { -1, -1, -1 },
	};
	int pid, status = 0, got;
	bool passed = false;

	pt_sigcatch(true);
	pid = pt_spawn(&req);
	if (pid < 0) {
		pt_sigcatch(false);
		return pid;
	}
	while ((got = pt_wait(pid, &status, false)) == -EINTR) {
		if (!passed)
			pt_kill(pid, PT_SIGINT);
		passed = true;
		pt_sigcatch(true);	/* handled: clear it, or every wait returns at once */
	}
	pt_sigcatch(false);
	if (passed)
		pt_kill(pt_getpid(), PT_SIGINT);	/* and now it is this program's turn */
	return got < 0 ? got : status;
}

int utf8_width(const char *s, size_t n)
{
	int w = 0;

	for (size_t i = 0; i < n; i++)
		w += ((unsigned char)s[i] & 0xc0) != 0x80;
	return w;
}

size_t utf8_prefix(const char *s, size_t n, int cols)
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

bool ask_line(int row, const char *prompt, char *buf, size_t size)
{
	size_t n = strlen(buf);

	for (;;) {
		int key;

		pt_printf("\x1b[%d;1H\x1b[0m%s%s\x1b[K\x1b[?25h", row, prompt, buf);
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
		} else if (key == PT_CTRL('u')) {
			n = 0;
			buf[0] = '\0';
		} else if (key >= ' ' && key < 0x100 && key != 0x7f && n + 1 < size) {
			buf[n++] = key;
			buf[n] = '\0';
		}
	}
}
