/*
 * sed: the stream editor.
 *
 * POSIX sed -- addresses, ranges, ! and { }, the commands s y d D p P n N
 * g G h H x a i c r w = l q b t and labels -- plus the GNU extensions
 * people type without thinking: -i to edit files in place, -E, -s,
 * one-line a/i/c, the I flag, 0,/re/, first~step, addr,+N, Q, T and z.
 *
 * The script is parsed once into an array of commands; a { holds the index
 * of its }, and a branch the index of its label. Each line of input then
 * runs through the array. The last line is known because one line is
 * always read ahead, which is what $ needs.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regex.h"
#include "util.h"

#define MAX_BLOCKS	32		/* { } nesting */
#define MAX_WFILES	8		/* w files, which stay open for the whole run */
#define WRAP		70		/* l: the width of its lines */

/* ------------------------------------------------------------ text */

/* A growable string: the pattern space, the hold space, the output. */
struct buf {
	char	*s;
	size_t	 len, cap;
};

static int buf_add(struct buf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 128;
		char *g;

		while (cap < b->len + n + 1)
			cap *= 2;
		if (!(g = pt_realloc(b->s, cap)))
			return -ENOMEM;
		b->s = g;
		b->cap = cap;
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
	return 0;
}

static int buf_set(struct buf *b, const char *s, size_t n)
{
	b->len = 0;
	return buf_add(b, s, n);
}

/* ------------------------------------------------------------ the script */

enum addr_type {
	A_NONE,
	A_LINE,			/* N; 0 only as 0,/re/ */
	A_LAST,			/* $ */
	A_RE,			/* /re/ */
	A_STEP,			/* first~step */
	A_PLUS,			/* the second of addr,+N */
	A_MULT,			/* the second of addr,~N */
};

struct addr {
	enum addr_type	 type;
	long		 n, step;
	struct regex	*re;		/* NULL for //: the last one used */
};

/* A replacement is literal text with \0-\9 (and &) in it: a list of parts. */
struct part {
	int	 group;			/* -1 for text */
	size_t	 at, len;		/* in the replacement's text */
};

struct subst {
	struct regex	*re;
	char		*text;
	struct part	*parts;
	int		 nparts;
	long		 nth;		/* the Nth match, or with g the Nth on */
	bool		 global, print;
	int		 wfile;		/* -1, or an index into the w files */
};

struct cmd {
	struct addr	 a1, a2;
	bool		 negate;
	bool		 active;	/* inside its range */
	long		 end;		/* where an addr,+N range ends */
	char		 c;
	char		*arg;		/* a i c: the text; b t T :: the label; r: the file */
	int		 target;	/* { : its }; b t T: the command after the label */
	int		 wfile;		/* w */
	int		 code;		/* q Q: the exit status; l: the width */
	struct subst	*s;
	unsigned char	*map;		/* y */
};

struct wfile {
	char	*name;
	int	 fd;
};

struct sed {
	struct cmd	*cmds;
	int		 ncmds, cap;
	struct wfile	 wfiles[MAX_WFILES];
	int		 nwfiles;
	int		 re_flags;	/* RE_EXTENDED with -E */
	bool		 quiet, separate, in_place;
	const char	*suffix;	/* -i's backup suffix, or "" */

	/* running */
	struct buf	 ps, hold, out, appended;
	struct regex	*last_re;
	int		 out_fd;
	bool		 missing_nl;	/* the output ends in a line without its newline */
	bool		 missing_nl_in;	/* the pattern space came from a line without one */
	bool		 hold_nl_missing;	/* and the hold space, the same way */
	bool		 tty;		/* the output is a terminal: say each line at once */
	bool		 replaced;	/* for t and T */
	bool		 quit;
	int		 status;
};

/* A parse error: where it happened, and what. */
struct parser {
	struct sed	*sd;
	const char	*p, *start;
	bool		 failed;
};

static void bad(struct parser *ps, const char *what)
{
	if (!ps->failed)
		pt_dprintf(PT_STDERR, "sed: char %d: %s\n", (int)(ps->p - ps->start) + 1, what);
	ps->failed = true;
}

static void skip_blanks(struct parser *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t')
		ps->p++;
}

/*
 * The text between two delimiters, as a pattern our regex parser reads:
 * \delim becomes the delimiter itself, made literal if it is special.
 * Returns NULL for no closing delimiter.
 */
static char *delimited(struct parser *ps, char delim, bool regex)
{
	struct buf b = { 0 };
	bool ere = ps->sd->re_flags & RE_EXTENDED;

	for (;;) {
		char c = *ps->p;

		if (!c || (c == '\n' && regex)) {
			pt_free(b.s);
			return NULL;
		}
		ps->p++;
		if (c == delim)
			break;
		if (c == '\\' && *ps->p == delim) {
			ps->p++;
			/* a delimiter that means something to the pattern: literal */
			if (regex && (strchr(".[\\*^$", delim) || (ere && strchr("|+?(){}", delim))))
				buf_add(&b, "\\", 1);
			buf_add(&b, &delim, 1);
			continue;
		}
		if (c == '\\' && *ps->p == '\n' && !regex) {
			ps->p++;
			buf_add(&b, "\\\n", 2);	/* a newline in the replacement */
			continue;
		}
		if (c == '\\' && *ps->p) {
			buf_add(&b, ps->p - 1, 2);
			ps->p++;
			continue;
		}
		if (regex && c == '[') {
			/* a bracket expression is taken whole: the delimiter is
			 * an ordinary character inside it */
			const char *q = ps->p;

			if (*q == '^')
				q++;
			if (*q == ']')
				q++;
			while (*q && *q != ']' && *q != '\n') {
				if (q[0] == '[' && (q[1] == ':' || q[1] == '.' || q[1] == '=')) {
					const char *close = strstr(q + 2, (char[]){ q[1], ']', 0 });

					q = close ? close + 2 : q + 1;
				} else {
					q++;
				}
			}
			if (*q == ']') {
				buf_add(&b, ps->p - 1, q + 1 - (ps->p - 1));
				ps->p = q + 1;
				continue;
			}
		}
		buf_add(&b, &c, 1);
	}
	return b.s ? b.s : pt_strdup("");
}

static struct regex *compile(struct parser *ps, const char *pattern, bool icase)
{
	struct regex *re;
	char err[96];

	if (!*pattern)
		return NULL;			/* //: the last regular expression */
	if (re_compile(&re, pattern, ps->sd->re_flags | (icase ? RE_ICASE : 0), err, sizeof(err))) {
		pt_dprintf(PT_STDERR, "sed: %s: %s\n", pattern, err);
		ps->failed = true;
		return NULL;
	}
	return re;
}

static bool number(struct parser *ps, long *n)
{
	if (!isdigit((unsigned char)*ps->p))
		return false;
	for (*n = 0; isdigit((unsigned char)*ps->p); ps->p++)
		*n = *n * 10 + (*ps->p - '0');
	return true;
}

/* One address, or none: false after an error. */
static bool parse_addr(struct parser *ps, struct addr *a, bool second)
{
	char delim;

	a->type = A_NONE;
	if (second && (*ps->p == '+' || *ps->p == '~')) {
		a->type = *ps->p++ == '+' ? A_PLUS : A_MULT;
		if (!number(ps, &a->n)) {
			bad(ps, "expected a number");
			return false;
		}
		return true;
	}
	if (number(ps, &a->n)) {
		a->type = A_LINE;
		if (!second && *ps->p == '~') {
			ps->p++;
			a->type = A_STEP;
			if (!number(ps, &a->step)) {
				bad(ps, "expected a number after ~");
				return false;
			}
		}
		return true;
	}
	if (*ps->p == '$') {
		ps->p++;
		a->type = A_LAST;
		return true;
	}
	if (*ps->p != '/' && *ps->p != '\\')
		return true;
	if (*ps->p == '\\')
		ps->p++;
	delim = *ps->p++;
	if (!delim || delim == '\n' || delim == '\\') {
		bad(ps, "a bad delimiter");
		return false;
	}
	char *pat = delimited(ps, delim, true);

	if (!pat) {
		bad(ps, "unterminated address regex");
		return false;
	}
	a->type = A_RE;
	a->re = compile(ps, pat, *ps->p == 'I' && ps->p++);
	pt_free(pat);
	return !ps->failed;
}

/* To the end of the line: a label or a file name. */
static char *rest_of_line(struct parser *ps, bool label)
{
	const char *start;

	skip_blanks(ps);
	start = ps->p;
	/* labels end at a ; as well, as GNU's do: `:a;N;ba` */
	while (*ps->p && *ps->p != '\n' && !(label && (*ps->p == ';' || *ps->p == '}')))
		ps->p++;
	const char *end = ps->p;

	while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	char *s = pt_malloc(end - start + 1);

	if (s) {
		memcpy(s, start, end - start);
		s[end - start] = '\0';
	}
	return s;
}

/*
 * The text of a i c: "a\" and lines, each but the last ending in a
 * backslash, or GNU's "a text" on one line. A backslash takes the
 * character after it as it is.
 */
static char *text_arg(struct parser *ps)
{
	struct buf b = { 0 };

	skip_blanks(ps);
	if (*ps->p == '\\') {
		ps->p++;
		skip_blanks(ps);
		if (*ps->p == '\n')
			ps->p++;
	}
	while (*ps->p && *ps->p != '\n') {
		char c = *ps->p++;

		if (c == '\\' && *ps->p) {
			c = *ps->p++;
			if (c == 't')
				c = '\t';
		}
		buf_add(&b, &c, 1);
	}
	return b.s ? b.s : pt_strdup("");
}

static int open_wfile(struct parser *ps, const char *name)
{
	struct sed *sd = ps->sd;

	for (int i = 0; i < sd->nwfiles; i++)
		if (!strcmp(sd->wfiles[i].name, name))
			return i;
	if (sd->nwfiles == MAX_WFILES) {
		bad(ps, "too many w files");
		return -1;
	}
	int fd = !strcmp(name, "/dev/stdout") ? PT_STDOUT : !strcmp(name, "/dev/stderr") ? PT_STDERR
		 : pt_open(name, O_WRONLY | O_CREAT | O_TRUNC);

	if (fd < 0) {
		fail("sed", name, fd);
		ps->failed = true;
		return -1;
	}
	sd->wfiles[sd->nwfiles].name = pt_strdup(name);
	sd->wfiles[sd->nwfiles].fd = fd;
	return sd->nwfiles++;
}

/* The replacement of an s command, cut into text and group references. */
static bool parse_replacement(struct parser *ps, struct subst *s, char *text)
{
	char *o = text;
	struct part *parts = NULL;
	int n = 0;

	s->text = text;
	for (const char *p = text; *p;) {
		int group = -1;
		size_t at = o - text;

		if (*p == '&') {
			group = 0;
			p++;
		} else if (*p == '\\' && p[1] >= '0' && p[1] <= '9') {
			group = p[1] - '0';
			p += 2;
		} else {
			if (*p == '\\' && p[1]) {
				p++;
				*o++ = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
				p++;
			} else {
				*o++ = *p++;
			}
			/* text runs on: grow the part before, if it is text */
			if (n && parts[n - 1].group < 0) {
				parts[n - 1].len++;
				continue;
			}
		}
		struct part *g = pt_realloc(parts, (n + 1) * sizeof(*g));

		if (!g) {
			pt_free(parts);
			bad(ps, "out of memory");
			return false;
		}
		parts = g;
		parts[n++] = (struct part){ group, at, group < 0 };
	}
	s->parts = parts;
	s->nparts = n;
	return true;
}

static bool parse_s(struct parser *ps, struct cmd *c)
{
	struct subst *s = pt_calloc(1, sizeof(*s));
	char delim = *ps->p, *pat, *repl;
	bool icase = false;

	if (!s || !delim || delim == '\n' || delim == '\\') {
		pt_free(s);
		bad(ps, "unterminated s command");
		return false;
	}
	c->s = s;
	s->wfile = -1;
	ps->p++;
	if (!(pat = delimited(ps, delim, true))) {
		bad(ps, "unterminated s command");
		return false;
	}
	if (!(repl = delimited(ps, delim, false))) {
		pt_free(pat);
		bad(ps, "unterminated s command");
		return false;
	}
	for (;;) {
		long n;

		if (*ps->p == 'g') {
			s->global = true;
			ps->p++;
		} else if (*ps->p == 'p') {
			s->print = true;
			ps->p++;
		} else if (*ps->p == 'i' || *ps->p == 'I') {
			icase = true;
			ps->p++;
		} else if (number(ps, &n)) {
			if (!n || s->nth) {
				bad(ps, "a bad number in the s flags");
				break;
			}
			s->nth = n;
		} else if (*ps->p == 'w') {
			ps->p++;
			char *name = rest_of_line(ps, false);

			s->wfile = name && *name ? open_wfile(ps, name) : -1;
			if (!name || !*name)
				bad(ps, "w wants a file name");
			pt_free(name);
			break;
		} else {
			break;
		}
	}
	if (!s->nth)
		s->nth = 1;
	s->re = compile(ps, pat, icase);
	pt_free(pat);
	if (!parse_replacement(ps, s, repl))
		return false;
	if (s->re) {
		for (int i = 0; i < s->nparts; i++)
			if (s->parts[i].group > re_groups(s->re)) {
				bad(ps, "a reference to a group the pattern does not have");
				return false;
			}
	}
	return !ps->failed;
}

static bool parse_y(struct parser *ps, struct cmd *c)
{
	char delim = *ps->p;
	unsigned char *map = pt_malloc(256);
	char *from, *to;

	if (!map || !delim || delim == '\n' || delim == '\\') {
		pt_free(map);
		bad(ps, "unterminated y command");
		return false;
	}
	ps->p++;
	from = delimited(ps, delim, false);
	to = from ? delimited(ps, delim, false) : NULL;
	c->map = map;
	for (int i = 0; i < 256; i++)
		map[i] = (unsigned char)i;
	if (!to) {
		pt_free(from);
		bad(ps, "unterminated y command");
		return false;
	}
	/* \n and \\ mean themselves; the delimiter has already been handled */
	for (char *s = from, *t = to;; ) {
		int a = *s == '\\' && s[1] ? (s++, *s == 'n' ? '\n' : *s) : *s;
		int b = *t == '\\' && t[1] ? (t++, *t == 'n' ? '\n' : *t) : *t;

		if (!a || !b) {
			if (a || b)
				bad(ps, "y wants strings of the same length");
			break;
		}
		map[(unsigned char)a] = (unsigned char)b;
		s++;
		t++;
	}
	pt_free(from);
	pt_free(to);
	return !ps->failed;
}

static struct cmd *new_cmd(struct parser *ps)
{
	struct sed *sd = ps->sd;

	if (sd->ncmds == sd->cap) {
		int cap = sd->cap ? sd->cap * 2 : 16;
		struct cmd *g = pt_realloc(sd->cmds, cap * sizeof(*g));

		if (!g) {
			bad(ps, "out of memory");
			return NULL;
		}
		sd->cmds = g;
		sd->cap = cap;
	}
	struct cmd *c = &sd->cmds[sd->ncmds++];

	memset(c, 0, sizeof(*c));
	c->wfile = -1;
	return c;
}

static int parse_script(struct sed *sd, const char *script)
{
	struct parser ps = { .sd = sd, .p = script, .start = script };
	int blocks[MAX_BLOCKS], nblocks = 0;

	/* #n on the first line is -n, as POSIX has it */
	if (script[0] == '#' && script[1] == 'n' && (script[2] == '\n' || !script[2]))
		sd->quiet = true;
	while (!ps.failed) {
		while (*ps.p == ' ' || *ps.p == '\t' || *ps.p == '\n' || *ps.p == ';')
			ps.p++;
		if (!*ps.p)
			break;
		if (*ps.p == '#') {
			while (*ps.p && *ps.p != '\n')
				ps.p++;
			continue;
		}
		struct cmd *c = new_cmd(&ps);

		if (!c)
			break;
		if (!parse_addr(&ps, &c->a1, false))
			break;
		if (c->a1.type != A_NONE && *ps.p == ',') {
			ps.p++;
			skip_blanks(&ps);
			if (!parse_addr(&ps, &c->a2, true))
				break;
			if (c->a2.type == A_NONE || c->a2.type == A_STEP) {
				bad(&ps, "unexpected ,");
				break;
			}
		}
		if (c->a1.type == A_LINE && !c->a1.n && c->a2.type != A_RE) {
			bad(&ps, "invalid usage of line address 0");
			break;
		}
		skip_blanks(&ps);
		while (*ps.p == '!') {
			c->negate = true;
			ps.p++;
			skip_blanks(&ps);
		}
		c->c = *ps.p;
		if (!c->c) {
			bad(&ps, "missing command");
			break;
		}
		ps.p++;
		switch (c->c) {
		case '{':
			if (nblocks == MAX_BLOCKS) {
				bad(&ps, "blocks nested too deeply");
				break;
			}
			blocks[nblocks++] = sd->ncmds - 1;
			continue;		/* the next command follows at once */
		case '}':
			if (!nblocks || c->a1.type != A_NONE) {
				bad(&ps, nblocks ? "} takes no address" : "unexpected }");
				break;
			}
			sd->cmds[blocks[--nblocks]].target = sd->ncmds - 1;
			break;
		case '=': case 'd': case 'D': case 'g': case 'G': case 'h': case 'H':
		case 'n': case 'N': case 'p': case 'P': case 'x': case 'z':
			break;
		case 'l': {
			long width = WRAP;

			skip_blanks(&ps);
			number(&ps, &width);
			c->code = (int)width;	/* 0 and 1: no wrapping */
			break;
		}
		case 'q': case 'Q': {
			long code = 0;

			skip_blanks(&ps);
			number(&ps, &code);
			c->code = (int)code;
			break;
		}
		case 'a': case 'i': case 'c':
			if (!(c->arg = text_arg(&ps)))
				bad(&ps, "out of memory");
			continue;		/* the text ran to the end of the line */
		case ':':
			if (c->a1.type != A_NONE) {
				bad(&ps, ": takes no address");
				break;
			}
			/* fall through */
		case 'b': case 't': case 'T':
			c->arg = rest_of_line(&ps, true);
			if (c->c == ':' && (!c->arg || !*c->arg))
				bad(&ps, "\":\" lacks a label");
			break;
		case 'r':
			c->arg = rest_of_line(&ps, false);
			break;
		case 'w':
			c->arg = rest_of_line(&ps, false);
			c->wfile = c->arg && *c->arg ? open_wfile(&ps, c->arg) : -1;
			if (!c->arg || !*c->arg)
				bad(&ps, "w wants a file name");
			break;
		case 's':
			parse_s(&ps, c);
			break;
		case 'y':
			parse_y(&ps, c);
			break;
		default:
			ps.p--;
			bad(&ps, "unknown command");
			break;
		}
		if (ps.failed)
			break;
		/* after a command: the end, a ; or a newline, a } or a comment */
		skip_blanks(&ps);
		if (*ps.p && *ps.p != ';' && *ps.p != '\n' && *ps.p != '}' && *ps.p != '#')
			bad(&ps, "extra characters after command");
	}
	if (!ps.failed && nblocks) {
		bad(&ps, "unmatched {");
	}
	/* branches to their labels: the command after the : */
	for (int i = 0; i < sd->ncmds && !ps.failed; i++) {
		struct cmd *c = &sd->cmds[i];

		if (!strchr("btT", c->c))
			continue;
		c->target = sd->ncmds;		/* no label: to the end of the script */
		if (!c->arg || !*c->arg)
			continue;
		int k = 0;

		while (k < sd->ncmds && !(sd->cmds[k].c == ':' && !strcmp(sd->cmds[k].arg, c->arg)))
			k++;
		if (k == sd->ncmds) {
			pt_dprintf(PT_STDERR, "sed: can't find label for jump to `%s'\n", c->arg);
			ps.failed = true;
		}
		c->target = k;
	}
	return ps.failed ? -1 : 0;
}

/* ------------------------------------------------------------ output */

static int flush_out(struct sed *sd)
{
	int err = write_all(sd->out_fd, sd->out.s, sd->out.len);

	sd->out.len = 0;
	return err;
}

/*
 * A line of output. A last line of input that had no newline goes out
 * without one, as GNU's sed does -- and gets it after all if something
 * else is printed after it.
 */
static void emit(struct sed *sd, const char *s, size_t n, bool newline)
{
	if (sd->missing_nl)
		buf_add(&sd->out, "\n", 1);
	buf_add(&sd->out, s, n);
	if (newline)
		buf_add(&sd->out, "\n", 1);
	sd->missing_nl = !newline;
	if (sd->out.len >= BUF_SIZE)
		flush_out(sd);
}

static void emit_wfile(struct sed *sd, int w, const char *s, size_t n)
{
	int fd = sd->wfiles[w].fd;

	if (fd == PT_STDOUT) {
		emit(sd, s, n, true);
		return;
	}
	write_all(fd, s, n);
	write_all(fd, "\n", 1);
}

/* l: the pattern space, with everything unprintable spelled out, in
 * lines of `width` ending in \, or in one line with a width of 0 or 1. */
static void emit_l(struct sed *sd, const char *s, size_t n, int width)
{
	struct buf line = { 0 };
	int at = 0;

	for (size_t i = 0; i <= n; i++) {
		char piece[8];
		unsigned char c = i < n ? (unsigned char)s[i] : 0;
		const char *esc = strchr("\\\a\b\f\n\r\t\v", c);

		if (i == n)
			snprintf(piece, sizeof(piece), "$");
		else if (c && esc)
			snprintf(piece, sizeof(piece), "\\%c", "\\abfnrtv"[esc - "\\\a\b\f\n\r\t\v"]);
		else if (c < 0x20 || c >= 0x7f)
			snprintf(piece, sizeof(piece), "\\%03o", c);
		else
			snprintf(piece, sizeof(piece), "%c", c);
		int len = (int)strlen(piece);

		if (i < n && width > 1 && at + len > width - 1) {
			buf_add(&line, "\\", 1);
			emit(sd, line.s, line.len, true);
			line.len = at = 0;
		}
		buf_add(&line, piece, len);
		at += len;
	}
	emit(sd, line.s, line.len, true);
	pt_free(line.s);
}

/* What a and r queued: after the line, before the next is read. */
static void flush_appended(struct sed *sd)
{
	if (!sd->appended.len)
		return;
	if (sd->missing_nl)
		buf_add(&sd->out, "\n", 1);
	sd->missing_nl = false;
	buf_add(&sd->out, sd->appended.s, sd->appended.len);
	sd->appended.len = 0;
	if (sd->out.len >= BUF_SIZE)
		flush_out(sd);
}

static void queue_file(struct sed *sd, const char *name)
{
	char buf[512];
	int fd = pt_open(name, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return;				/* a file that is not there adds nothing */
	while ((n = pt_read(fd, buf, sizeof(buf))) > 0)
		buf_add(&sd->appended, buf, n);
	pt_close(fd);
}

/* ------------------------------------------------------------ input */

struct input {
	char		**files;
	int		  nfiles, next_file;
	int		  fd;
	struct lines	  l;
	bool		  reading;
	/* the line after this one, to know whether this one is the last */
	struct buf	  ahead;
	bool		  have_ahead, ahead_nl;
	long		  line_no;
	int		  status;
};

/* Reads the next line into ahead, opening the next file if need be. */
static void read_ahead(struct input *in)
{
	size_t len;
	char *line;

	in->have_ahead = false;
	for (;;) {
		if (!in->reading) {
			if (in->next_file >= in->nfiles)
				return;
			const char *name = in->files[in->next_file++];

			in->fd = !strcmp(name, "-") ? PT_STDIN : pt_open(name, O_RDONLY);
			if (in->fd < 0) {
				pt_dprintf(PT_STDERR, "sed: can't read %s: %s\n", name, pt_strerror(in->fd));
				in->status = 2;
				continue;
			}
			lines_init(&in->l, in->fd);
			in->reading = true;
		}
		if ((line = lines_next(&in->l, &len))) {
			in->ahead_nl = len && line[len - 1] == '\n';
			buf_set(&in->ahead, line, len - in->ahead_nl);
			in->have_ahead = true;
			return;
		}
		if (in->l.err) {
			fail("sed", in->files[in->next_file - 1], in->l.err);
			in->status = 2;
		}
		lines_free(&in->l);
		if (in->fd != PT_STDIN)
			pt_close(in->fd);
		in->reading = false;
	}
}

/* The next line into the pattern space: false at the end of the input. */
static bool next_line(struct sed *sd, struct input *in, bool append)
{
	if (!in->have_ahead)
		return false;
	if (append)
		buf_add(&sd->ps, "\n", 1);
	else
		sd->ps.len = 0;
	buf_add(&sd->ps, in->ahead.s, in->ahead.len);
	sd->missing_nl_in = !in->ahead_nl;
	in->line_no++;
	read_ahead(in);
	return true;
}

/* ------------------------------------------------------------ running */

static bool re_matches(struct sed *sd, struct regex *re, const char *s, size_t len)
{
	if (re)
		sd->last_re = re;
	else if (!(re = sd->last_re)) {
		pt_dprintf(PT_STDERR, "sed: no previous regular expression\n");
		sd->quit = true;
		sd->status = 1;
		return false;
	}
	return re_search(re, s, len, 0, false, NULL);
}

static bool addr_match(struct sed *sd, struct input *in, const struct addr *a)
{
	switch (a->type) {
	case A_NONE:
		return true;
	case A_LINE:
		return in->line_no == a->n;
	case A_LAST:
		return !in->have_ahead;
	case A_STEP:
		return a->step <= 0 ? in->line_no == a->n
				    : in->line_no >= a->n && (in->line_no - a->n) % a->step == 0;
	case A_RE:
		return re_matches(sd, a->re, sd->ps.s, sd->ps.len);
	default:
		return false;
	}
}

/* Whether command c applies to this line, keeping its range up to date. */
static bool selected(struct sed *sd, struct input *in, struct cmd *c)
{
	if (c->a1.type == A_NONE)
		return true;
	if (c->a2.type == A_NONE)
		return addr_match(sd, in, &c->a1);
	if (c->active) {
		/* inside the range: does this line end it? */
		switch (c->a2.type) {
		case A_LINE:
			c->active = in->line_no < c->a2.n;
			/* a line number already passed ends it before this line */
			if (in->line_no > c->a2.n)
				return false;
			break;
		case A_PLUS:
			c->active = in->line_no < c->end;
			break;
		case A_MULT:
			c->active = c->a2.n > 0 && in->line_no % c->a2.n;
			break;
		case A_LAST:
			c->active = in->have_ahead;
			break;
		default:
			c->active = !addr_match(sd, in, &c->a2);
			break;
		}
		return true;
	}
	/* 0,/re/: the range is open before the first line, so the first
	 * line can already end it */
	bool zero = c->a1.type == A_LINE && !c->a1.n && in->line_no == 1;

	if (!zero && !addr_match(sd, in, &c->a1))
		return false;
	switch (c->a2.type) {
	case A_LINE:
		c->active = c->a2.n > in->line_no;
		break;
	case A_PLUS:
		c->end = in->line_no + c->a2.n;
		c->active = c->a2.n > 0;
		break;
	case A_MULT:
		c->active = c->a2.n > 0 && in->line_no % c->a2.n;
		break;
	case A_LAST:
		c->active = in->have_ahead;
		break;
	default:
		c->active = zero ? !addr_match(sd, in, &c->a2) : true;
		break;
	}
	return true;
}

/* Past the character at s[at]: a whole UTF-8 sequence. */
static size_t char_len(const char *s, size_t len, size_t at)
{
	size_t n = 1;

	while (at + n < len && ((unsigned char)s[at + n] & 0xc0) == 0x80)
		n++;
	return n;
}

static bool substitute(struct sed *sd, struct subst *s)
{
	struct regex *re = s->re ? s->re : sd->last_re;
	struct buf out = { 0 };
	struct re_match m;
	size_t at = 0, len = sd->ps.len;
	long prev_end = -1, count = 0;
	bool did = false;

	if (!re) {
		pt_dprintf(PT_STDERR, "sed: no previous regular expression\n");
		sd->quit = true;
		sd->status = 1;
		return false;
	}
	sd->last_re = re;
	while (at <= len && re_search(re, sd->ps.s, len, at, false, &m)) {
		long so = m.so[0], eo = m.eo[0];

		/* an empty match just where the last one ended is not another */
		if (so == eo && so == prev_end) {
			if ((size_t)so == len)
				break;
			size_t n = char_len(sd->ps.s, len, so);

			buf_add(&out, sd->ps.s + at, so + n - at);
			at = so + n;
			prev_end = -1;
			continue;
		}
		buf_add(&out, sd->ps.s + at, so - at);
		if (++count < s->nth) {
			buf_add(&out, sd->ps.s + so, eo - so);
		} else {
			for (int i = 0; i < s->nparts; i++) {
				const struct part *p = &s->parts[i];

				if (p->group < 0)
					buf_add(&out, s->text + p->at, p->len);
				else if (m.so[p->group] >= 0)
					buf_add(&out, sd->ps.s + m.so[p->group], m.eo[p->group] - m.so[p->group]);
			}
			did = true;
			if (!s->global) {
				at = eo;
				break;
			}
		}
		prev_end = eo;
		at = eo;
		if (so == eo) {
			/* an empty match: the character after it goes through */
			if ((size_t)so == len)
				break;
			size_t n = char_len(sd->ps.s, len, so);

			buf_add(&out, sd->ps.s + so, n);
			at = so + n;
		}
	}
	if (did) {
		buf_add(&out, sd->ps.s + at, len - at);
		pt_free(sd->ps.s);
		sd->ps = out;
		if (!sd->ps.s)
			buf_set(&sd->ps, "", 0);
	} else {
		pt_free(out.s);
	}
	return did;
}

/* One line through the script. */
static void cycle(struct sed *sd, struct input *in)
{
	int pc = 0;
	bool autoprint = !sd->quiet;

	sd->replaced = false;
restart:
	while (pc < sd->ncmds && !sd->quit) {
		struct cmd *c = &sd->cmds[pc++];

		if (c->c == ':' || c->c == '}')
			continue;
		if (selected(sd, in, c) == c->negate) {
			if (c->c == '{')
				pc = c->target + 1;	/* the block is skipped */
			continue;
		}
		switch (c->c) {
		case '{':
			break;
		case '=':
			{
				char n[24];

				emit(sd, n, snprintf(n, sizeof(n), "%ld", in->line_no), true);
			}
			break;
		case 'a':
			buf_add(&sd->appended, c->arg, strlen(c->arg));
			buf_add(&sd->appended, "\n", 1);
			break;
		case 'i':
			emit(sd, c->arg, strlen(c->arg), true);
			break;
		case 'c':
			/* in a range, the text goes out once, at its end */
			if (c->a2.type == A_NONE || !c->active || c->negate)
				emit(sd, c->arg, strlen(c->arg), true);
			return;				/* deleted, as d */
		case 'r':
			queue_file(sd, c->arg);
			break;
		case 'w':
			emit_wfile(sd, c->wfile, sd->ps.s ? sd->ps.s : "", sd->ps.len);
			break;
		case 'd':
			return;
		case 'D': {
			char *nl = sd->ps.len ? memchr(sd->ps.s, '\n', sd->ps.len) : NULL;

			if (!nl)
				return;
			/* the rest again, from the top, without reading a line */
			size_t cut = nl + 1 - sd->ps.s;

			memmove(sd->ps.s, nl + 1, sd->ps.len - cut);
			sd->ps.len -= cut;
			flush_appended(sd);
			pc = 0;
			goto restart;
		}
		case 'p':
			emit(sd, sd->ps.s ? sd->ps.s : "", sd->ps.len, !sd->missing_nl_in);
			break;
		case 'P': {
			char *nl = sd->ps.len ? memchr(sd->ps.s, '\n', sd->ps.len) : NULL;

			if (nl)
				emit(sd, sd->ps.s, nl - sd->ps.s, true);
			else
				emit(sd, sd->ps.s ? sd->ps.s : "", sd->ps.len, !sd->missing_nl_in);
			break;
		}
		case 'l':
			emit_l(sd, sd->ps.s ? sd->ps.s : "", sd->ps.len, c->code);
			break;
		case 'n':
			if (!in->have_ahead) {
				sd->quit = true;	/* no more: the end, printed as usual */
				goto end;
			}
			if (autoprint)
				emit(sd, sd->ps.s ? sd->ps.s : "", sd->ps.len, !sd->missing_nl_in);
			flush_appended(sd);
			next_line(sd, in, false);
			break;
		case 'N':
			if (!in->have_ahead) {
				sd->quit = true;
				goto end;
			}
			flush_appended(sd);
			next_line(sd, in, true);
			break;
		/*
		 * Whether a last line without a newline still goes out
		 * without one travels with the text, as in GNU's sed.
		 */
		case 'g':
			buf_set(&sd->ps, sd->hold.s ? sd->hold.s : "", sd->hold.len);
			sd->missing_nl_in = sd->hold_nl_missing;
			break;
		case 'G':
			buf_add(&sd->ps, "\n", 1);
			buf_add(&sd->ps, sd->hold.s ? sd->hold.s : "", sd->hold.len);
			sd->missing_nl_in = sd->hold_nl_missing;
			break;
		case 'h':
			buf_set(&sd->hold, sd->ps.s ? sd->ps.s : "", sd->ps.len);
			sd->hold_nl_missing = sd->missing_nl_in;
			break;
		case 'H':
			buf_add(&sd->hold, "\n", 1);
			buf_add(&sd->hold, sd->ps.s ? sd->ps.s : "", sd->ps.len);
			sd->hold_nl_missing = sd->missing_nl_in;
			break;
		case 'x': {
			struct buf t = sd->ps;
			bool missing = sd->missing_nl_in;

			sd->ps = sd->hold;
			sd->hold = t;
			sd->missing_nl_in = sd->hold_nl_missing;
			sd->hold_nl_missing = missing;
			break;
		}
		case 'z':
			sd->ps.len = 0;
			break;
		case 'y':
			for (size_t k = 0; k < sd->ps.len; k++)
				sd->ps.s[k] = (char)c->map[(unsigned char)sd->ps.s[k]];
			break;
		case 's':
			if (substitute(sd, c->s)) {
				sd->replaced = true;
				if (c->s->print)
					emit(sd, sd->ps.s, sd->ps.len, !sd->missing_nl_in);
				if (c->s->wfile >= 0)
					emit_wfile(sd, c->s->wfile, sd->ps.s, sd->ps.len);
			}
			break;
		case 'b':
			pc = c->target;
			break;
		case 't':
			if (sd->replaced) {
				sd->replaced = false;
				pc = c->target;
			}
			break;
		case 'T':
			if (!sd->replaced)
				pc = c->target;
			else
				sd->replaced = false;
			break;
		case 'q':
			sd->quit = true;
			sd->status = c->code;
			goto end;
		case 'Q':
			sd->quit = true;
			sd->status = c->code;
			return;
		}
	}
end:
	if (autoprint)
		emit(sd, sd->ps.s ? sd->ps.s : "", sd->ps.len, !sd->missing_nl_in);
}

/* The lines of `files` as one stream, into the output as it is. */
static void run(struct sed *sd, char **files, int nfiles)
{
	struct input in = { .files = files, .nfiles = nfiles };

	for (int i = 0; i < sd->ncmds; i++)
		sd->cmds[i].active = false;
	read_ahead(&in);
	while (!sd->quit && !pt_interrupted() && next_line(sd, &in, false)) {
		cycle(sd, &in);
		flush_appended(sd);
		if (sd->tty)
			flush_out(sd);
	}
	if (in.reading) {
		lines_free(&in.l);
		if (in.fd != PT_STDIN)
			pt_close(in.fd);
	}
	pt_free(in.ahead.s);
	if (in.status && !sd->status)
		sd->status = in.status;
}

/* -i: each file into FILE.sed, which then takes its place. */
static void run_in_place(struct sed *sd, char *file)
{
	char tmp[PT_PATH_MAX], backup[PT_PATH_MAX];
	struct pt_stat st;
	int err;

	if ((err = pt_stat(file, &st)) || st.is_dir) {
		if (!err)
			pt_dprintf(PT_STDERR, "sed: couldn't edit %s: not a regular file\n", file);
		else
			fail("sed", file, err);
		sd->status = 4;
		return;
	}
	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.sed", file) >= sizeof(tmp) ||
	    (size_t)snprintf(backup, sizeof(backup), "%s%s", file, sd->suffix) >= sizeof(backup)) {
		fail("sed", file, -ENAMETOOLONG);
		sd->status = 4;
		return;
	}
	if ((sd->out_fd = pt_open(tmp, O_WRONLY | O_CREAT | O_TRUNC)) < 0) {
		fail("sed", tmp, sd->out_fd);
		sd->status = 4;
		return;
	}
	sd->missing_nl = false;
	run(sd, &file, 1);
	err = flush_out(sd);
	if (!err)
		err = pt_close(sd->out_fd);
	else
		pt_close(sd->out_fd);
	sd->out_fd = PT_STDOUT;
	/* the old file becomes the backup, if one was asked for */
	if (!err && *sd->suffix)
		err = pt_rename(file, backup);
	if (!err)
		err = pt_rename(tmp, file);
	if (err) {
		pt_unlink(tmp);
		fail("sed", file, err);
		sd->status = 4;
	}
}

static void free_sed(struct sed *sd)
{
	for (int i = 0; i < sd->ncmds; i++) {
		struct cmd *c = &sd->cmds[i];

		re_free(c->a1.re);
		re_free(c->a2.re);
		pt_free(c->arg);
		pt_free(c->map);
		if (c->s) {
			re_free(c->s->re);
			pt_free(c->s->text);
			pt_free(c->s->parts);
			pt_free(c->s);
		}
	}
	for (int i = 0; i < sd->nwfiles; i++) {
		if (sd->wfiles[i].fd > PT_STDERR)
			pt_close(sd->wfiles[i].fd);
		pt_free(sd->wfiles[i].name);
	}
	pt_free(sd->cmds);
	pt_free(sd->ps.s);
	pt_free(sd->hold.s);
	pt_free(sd->out.s);
	pt_free(sd->appended.s);
}

/* The script from a file for -f. */
static int add_file(struct buf *script, const char *name)
{
	char buf[512];
	int fd = !strcmp(name, "-") ? PT_STDIN : pt_open(name, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return fail("sed", name, fd);
	while ((n = pt_read(fd, buf, sizeof(buf))) > 0)
		buf_add(script, buf, n);
	if (fd != PT_STDIN)
		pt_close(fd);
	if (script->len && script->s[script->len - 1] == '\n')
		script->len--;
	return n < 0 ? fail("sed", name, n) : 0;
}

PT_PROGRAM(sed, "edit text as it goes through\n"
	   "usage: sed [-nEis] [-e SCRIPT]... [-f FILE] [SCRIPT]\n"
	   "           [file...]\n"
	   "  sed 's/cat/dog/g' notes   every cat a dog\n"
	   "  sed -n '/todo/p' notes    only lines with todo\n"
	   "  sed -i '3d' notes         delete line 3 of the file\n"
	   "  -n  print only what p says  -E  extended regexes\n"
	   "  -i[SUF]  edit files in place (keeping FILE.SUF)\n"
	   "  -s  files one by one: line numbers, $ start again\n"
	   "Addresses: N $ /RE/ N~S, then ,N ,/RE/ ,+N; ! inverts\n"
	   "Commands: s/RE/TEXT/[gpNIw] y/abc/xyz/ d D p P n N\n"
	   "g G h H x, a i c TEXT, r w FILE, = l q Q z,\n"
	   ":LABEL b t T LABEL, { }")
{
	static char *dash[] = { "-" };
	struct sed sd = { .out_fd = PT_STDOUT, .suffix = "" };
	struct buf script = { 0 };
	bool have_script = false;
	int i = 1;

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}
		for (const char *o = argv[i] + 1; *o; o++) {
			const char *arg;

			switch (*o) {
			case 'n': sd.quiet = true; continue;
			case 'E': case 'r': sd.re_flags |= RE_EXTENDED; continue;
			case 's': sd.separate = true; continue;
			case 'i':
				sd.in_place = sd.separate = true;
				sd.suffix = o + 1;	/* -i.bak: the rest is the suffix */
				break;
			case 'e': case 'f':
				arg = o[1] ? o + 1 : i + 1 < argc ? argv[++i] : NULL;
				if (!arg) {
					pt_dprintf(PT_STDERR, "sed: -%c needs an argument\n", *o);
					goto usage;
				}
				if (script.len)
					buf_add(&script, "\n", 1);
				if (*o == 'e')
					buf_add(&script, arg, strlen(arg));
				else if (add_file(&script, arg))
					goto fail;
				have_script = true;
				break;
			default:
				pt_dprintf(PT_STDERR, "sed: unknown option -%c (try 'help sed')\n", *o);
				goto usage;
			}
			break;			/* the option took the rest of the word */
		}
	}
	if (!have_script) {
		if (i >= argc)
			goto usage;
		buf_add(&script, argv[i], strlen(argv[i]));
		i++;
	}
	if (parse_script(&sd, script.s ? script.s : ""))
		goto fail;
	sd.tty = pt_isatty(PT_STDOUT);

	if (sd.in_place) {
		if (i >= argc) {
			pt_dprintf(PT_STDERR, "sed: -i wants files to edit\n");
			goto fail;
		}
		for (; i < argc && !sd.quit && !pt_interrupted(); i++)
			run_in_place(&sd, argv[i]);
	} else if (sd.separate) {
		for (int k = i; (k < argc || k == i) && !sd.quit && !pt_interrupted(); k++)
			run(&sd, k < argc ? &argv[k] : dash, 1);
	} else {
		run(&sd, i < argc ? &argv[i] : dash, i < argc ? argc - i : 1);
	}
	flush_out(&sd);
	pt_free(script.s);
	free_sed(&sd);
	return sd.status;
usage:
	pt_dprintf(PT_STDERR, "usage: sed [-nEis] [-e SCRIPT]... [-f FILE] [SCRIPT] [file...]\n");
fail:
	pt_free(script.s);
	free_sed(&sd);
	return 1;
}
