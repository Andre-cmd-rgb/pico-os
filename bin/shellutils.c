/*
 * Small programs scripts are made of: printf seq basename dirname yes
 * realpath stat expr
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "regex.h"
#include "util.h"

/* ------------------------------------------------------------ printf */

struct printf_state {
	char	**args;
	int	  nargs, next;
	bool	  bad;			/* a number that was not one */
	bool	  stop;			/* \c in a %b argument: nothing more */
};

static const char *next_arg(struct printf_state *ps)
{
	return ps->next < ps->nargs ? ps->args[ps->next++] : NULL;
}

/* A numeric argument: 'c and "c are the character's code, as POSIX says. */
static bool num_arg(struct printf_state *ps, const char *s, long long *i, unsigned long long *u,
		    double *d)
{
	char *end;

	*i = 0;
	*u = 0;
	*d = 0;
	if (!s)
		return true;
	if ((s[0] == '\'' || s[0] == '"') && s[1]) {
		*i = *u = (unsigned char)s[1];
		*d = (double)*i;
		return true;
	}
	errno = 0;
	*d = strtod(s, &end);
	bool ok = end != s && !*end;

	*i = strtoll(s, &end, 0);
	if (end == s || *end)
		*i = (long long)*d;
	else
		ok = true;
	*u = *i < 0 ? (unsigned long long)*i : strtoull(s, NULL, 0);
	if (!ok || errno) {
		pt_dprintf(PT_STDERR, "printf: '%s': not a valid number\n", s);
		ps->bad = true;
	}
	return ok;
}

/* %b: the argument with its backslash escapes; \c ends all output. */
static void print_b(struct printf_state *ps, const char *s, int prec)
{
	char out[256];
	int n = 0, shown = 0;

	for (; s && *s && (prec < 0 || shown < prec); shown++) {
		char c = *s++;

		if (c == '\\' && *s == 'c') {
			ps->stop = true;
			break;
		}
		if (c == '\\')
			c = (char)escape_char_0(&s);	/* %b's octal is \0NNN */
		out[n++] = c;
		if (n == sizeof(out)) {
			write_all(PT_STDOUT, out, n);
			n = 0;
		}
	}
	write_all(PT_STDOUT, out, n);
}

/* One pass over the format; true if it took any arguments. */
static bool printf_once(struct printf_state *ps, const char *fmt)
{
	bool took = false;

	for (const char *p = fmt; *p && !ps->stop;) {
		if (*p == '\\') {
			p++;
			if (*p == 'c') {
				ps->stop = true;
				break;
			}
			char c = (char)escape_char(&p);

			write_all(PT_STDOUT, &c, 1);
			continue;
		}
		if (*p != '%') {
			const char *run = p;

			while (*p && *p != '%' && *p != '\\')
				p++;
			write_all(PT_STDOUT, run, p - run);
			continue;
		}
		if (p[1] == '%') {
			pt_puts("%");
			p += 2;
			continue;
		}
		/* %[flags][width][.precision]conversion */
		char spec[64], out[512];
		int n = 0, width = -1, prec = -1;
		const char *start = p++;

		spec[n++] = '%';
		while (*p && strchr("-+ #0", *p) && n < 10)
			spec[n++] = *p++;
		if (*p == '*') {
			long long i; unsigned long long u; double d;

			num_arg(ps, next_arg(ps), &i, &u, &d);
			took = true;
			width = (int)i;
			n += snprintf(spec + n, sizeof(spec) - n, "%d", width);
			p++;
		} else {
			while (isdigit((unsigned char)*p) && n < 20)
				spec[n++] = *p++;
		}
		if (*p == '.') {
			spec[n++] = *p++;
			if (*p == '*') {
				long long i; unsigned long long u; double d;

				num_arg(ps, next_arg(ps), &i, &u, &d);
				took = true;
				prec = (int)i;
				n += snprintf(spec + n, sizeof(spec) - n, "%d", prec);
				p++;
			} else {
				prec = 0;
				while (isdigit((unsigned char)*p) && n < 30) {
					prec = prec * 10 + (*p - '0');
					spec[n++] = *p++;
				}
			}
		}
		char conv = *p;

		if (!conv || !strchr("diouxXcsbeEfFgGaA", conv)) {
			pt_dprintf(PT_STDERR, "printf: %.*s: not a conversion\n", (int)(p - start + !!conv), start);
			ps->bad = true;
			if (conv)
				p++;
			continue;
		}
		p++;
		const char *arg = next_arg(ps);
		long long i;
		unsigned long long u;
		double d;
		int len = 0;

		took = true;
		switch (conv) {
		case 'd': case 'i':
			num_arg(ps, arg, &i, &u, &d);
			spec[n++] = 'l';
			spec[n++] = 'l';
			spec[n++] = conv;
			spec[n] = '\0';
			len = snprintf(out, sizeof(out), spec, i);
			break;
		case 'o': case 'u': case 'x': case 'X':
			num_arg(ps, arg, &i, &u, &d);
			spec[n++] = 'l';
			spec[n++] = 'l';
			spec[n++] = conv;
			spec[n] = '\0';
			len = snprintf(out, sizeof(out), spec, u);
			break;
		case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
			num_arg(ps, arg, &i, &u, &d);
			spec[n++] = conv;
			spec[n] = '\0';
			len = snprintf(out, sizeof(out), spec, d);
			break;
		case 'c':
			spec[n++] = 'c';
			spec[n] = '\0';
			len = snprintf(out, sizeof(out), spec, arg && *arg ? *arg : '\0');
			break;
		case 's':
			spec[n++] = 's';
			spec[n] = '\0';
			len = snprintf(out, sizeof(out), spec, arg ? arg : "");
			if (len >= (int)sizeof(out)) {	/* a long string: straight out */
				write_all(PT_STDOUT, arg ? arg : "", strlen(arg ? arg : ""));
				len = 0;
			}
			break;
		case 'b':
			print_b(ps, arg, prec);
			break;
		}
		if (len > 0)
			write_all(PT_STDOUT, out, len < (int)sizeof(out) ? len : (int)sizeof(out) - 1);
	}
	return took;
}

PT_PROGRAM(printf, "print formatted text\n"
	   "usage: printf FORMAT [argument...]\n"
	   "As in C: %s %d %x %o %c %e %f %g, with width, precision and\n"
	   "flags (%-8s %05.1f %*d), and %b for an argument with \\escapes.\n"
	   "The format is used again until the arguments run out.\n"
	   "  printf '%s=%d\\n' a 1 b 2      a=1 then b=2")
{
	struct printf_state ps = { .args = argv + 2, .nargs = argc - 2 };

	if (argc < 2) {
		pt_dprintf(PT_STDERR, "usage: printf FORMAT [argument...]\n");
		return 2;
	}
	/* again while there are arguments left and the format takes some */
	while (printf_once(&ps, argv[1]) && ps.next < ps.nargs && !ps.stop)
		;
	return ps.bad;
}

/* ------------------------------------------------------------ seq */

/* Digits after the point in a number as typed: "1.50" has two, and so
 * does "25e-3", as GNU's seq counts them. */
static int decimals(const char *s)
{
	const char *dot = strchr(s, '.'), *e = strpbrk(s, "eE");
	int n = 0;

	if (dot && (!e || dot < e))
		while (isdigit((unsigned char)dot[1 + n]))
			n++;
	if (e)
		n -= atoi(e + 1);
	return n > 0 ? n : 0;
}

/*
 * A -f format must have exactly one conversion, and a floating one: the
 * number is handed to it as a double, and %s or %d there would read what
 * was never passed.
 */
static bool float_format(const char *f)
{
	int convs = 0;

	for (const char *p = f; *p; p++) {
		if (*p != '%')
			continue;
		if (p[1] == '%') {
			p++;
			continue;
		}
		p++;
		while (*p && strchr("-+ #0'", *p))
			p++;
		while (isdigit((unsigned char)*p))
			p++;
		if (*p == '.')
			for (p++; isdigit((unsigned char)*p); p++)
				;
		if (!*p || !strchr("eEfFgGaA", *p))
			return false;
		convs++;
	}
	return convs == 1;
}

PT_PROGRAM(seq, "print a sequence of numbers\n"
	   "usage: seq [-w] [-s SEP] [-f FMT] [FIRST [STEP]] LAST\n"
	   "  seq 5        1 to 5         seq 2 2 10    2 4 6 8 10\n"
	   "  seq 1 0.5 2  1.0 1.5 2.0    -w  pad with zeros to one width\n"
	   "  -s SEP  between the numbers instead of a newline\n"
	   "  -f FMT  a printf format for each, like %03g")
{
	struct opt o = { .ind = 1 };
	const char *sep = "\n", *fmt = NULL;
	double v[3] = { 1, 1, 0 };
	bool pad = false;
	int c, n, dec = 0, width = 0;

	/* a negative number is an operand, not an option */
	while (o.ind < argc && !(argv[o.ind][0] == '-' && (isdigit((unsigned char)argv[o.ind][1]) ||
								argv[o.ind][1] == '.')) &&
	       (c = getopt_pt(&o, "seq", argc, argv, "ws:f:")) != -1) {
		switch (c) {
		case 'w': pad = true; break;
		case 's': sep = o.arg; break;
		case 'f': fmt = o.arg; break;
		default: return 2;
		}
	}
	n = argc - o.ind;
	if (n < 1 || n > 3) {
		pt_dprintf(PT_STDERR, "usage: seq [-w] [-s SEP] [-f FMT] [FIRST [STEP]] LAST\n");
		return 2;
	}
	for (int k = 0; k < n; k++) {
		const char *a = argv[o.ind + k];
		char *end;
		double x = strtod(a, &end);

		if (end == a || *end) {
			pt_dprintf(PT_STDERR, "seq: %s: not a number\n", a);
			return 2;
		}
		/* one operand is LAST; two are FIRST LAST; three FIRST STEP LAST */
		int slot = n == 1 ? 2 : n == 2 ? (k ? 2 : 0) : k;

		v[slot] = x;
		if (slot != 2 && decimals(a) > dec)
			dec = decimals(a);
	}
	if (!v[1]) {
		pt_dprintf(PT_STDERR, "seq: the step cannot be 0\n");
		return 2;
	}
	if (fmt && !float_format(fmt)) {
		pt_dprintf(PT_STDERR, "seq: -f %s: wants one %%e, %%f or %%g\n", fmt);
		return 2;
	}
	if (pad) {
		/* the widest of the first and last, as printed */
		char a[64], b[64];
		int wa = snprintf(a, sizeof(a), "%.*f", dec, v[0]);
		int wb = snprintf(b, sizeof(b), "%.*f", dec, v[2]);

		width = wa > wb ? wa : wb;
	}
	/*
	 * Counted in units of the smallest decimal typed -- tenths, for a
	 * step of 0.1 -- so that 0.1 ten times is exactly 1 and the last
	 * number is never lost to rounding. Numbers too big for that are
	 * worked out in floating point, each afresh rather than summed.
	 */
	double scale = 1;
	bool exact = dec <= 9;

	for (int k = 0; k < dec; k++)
		scale *= 10;
	/* and only if each is a whole number of units: 1e-1 has no digits
	 * after a point, so the unit is 1 and the step would round to 0 */
	for (int k = 0; k < 3 && exact; k++)
		exact = v[k] * scale > -9e15 && v[k] * scale < 9e15 &&
			fabs(v[k] * scale - llround(v[k] * scale)) < 1e-6;
	long long first = (long long)(v[0] * scale + (v[0] < 0 ? -0.5 : 0.5));
	long long step = (long long)(v[1] * scale + (v[1] < 0 ? -0.5 : 0.5));
	long long last = (long long)(v[2] * scale + (v[2] < 0 ? -0.5 : 0.5));

	for (long long k = 0;; k++) {
		double x;
		char out[128];
		int len;

		if (exact) {
			long long at = first + k * step;

			if (step > 0 ? at > last : at < last)
				break;
			x = (double)at / scale;
		} else {
			x = v[0] + (double)k * v[1];
			if (v[1] > 0 ? x > v[2] : x < v[2])
				break;
		}
		if (fmt)
			len = snprintf(out, sizeof(out), fmt, x);
		else if (pad)
			len = snprintf(out, sizeof(out), "%0*.*f", width, dec, x);
		else
			len = snprintf(out, sizeof(out), "%.*f", dec, x);
		if (k)
			pt_puts(sep);
		write_all(PT_STDOUT, out, len < (int)sizeof(out) ? len : (int)sizeof(out) - 1);
		if (pt_interrupted())
			break;
	}
	if (v[1] > 0 ? v[0] <= v[2] : v[0] >= v[2])
		pt_puts("\n");
	return 0;
}

/* ------------------------------------------------------------ names */

/* A path with its trailing slashes off, except for "/" itself. */
static size_t trimmed(const char *s)
{
	size_t n = strlen(s);

	while (n > 1 && s[n - 1] == '/')
		n--;
	return n;
}

PT_PROGRAM(basename, "the last part of a path\n"
	   "usage: basename PATH [SUFFIX]\n"
	   "  basename /a/b/notes.txt .txt     notes")
{
	if (argc < 2 || argc > 3) {
		pt_dprintf(PT_STDERR, "usage: basename PATH [SUFFIX]\n");
		return 2;
	}
	const char *s = argv[1];
	size_t n = trimmed(s), start = n;

	if (n == 1 && s[0] == '/') {
		pt_puts("/\n");
		return 0;
	}
	while (start > 0 && s[start - 1] != '/')
		start--;
	size_t len = n - start;

	if (argc == 3) {
		size_t sl = strlen(argv[2]);

		if (sl < len && !memcmp(s + start + len - sl, argv[2], sl))
			len -= sl;
	}
	write_all(PT_STDOUT, s + start, len);
	pt_puts("\n");
	return 0;
}

PT_PROGRAM(dirname, "a path without its last part\n"
	   "usage: dirname PATH\n"
	   "  dirname /a/b/notes.txt     /a/b")
{
	if (argc != 2) {
		pt_dprintf(PT_STDERR, "usage: dirname PATH\n");
		return 2;
	}
	const char *s = argv[1];
	size_t n = trimmed(s);

	while (n > 0 && s[n - 1] != '/')
		n--;				/* the last part */
	while (n > 1 && s[n - 1] == '/')
		n--;				/* the slashes before it */
	if (!n)
		pt_puts(".\n");
	else {
		write_all(PT_STDOUT, s, n);
		pt_puts("\n");
	}
	return 0;
}

PT_PROGRAM(realpath, "a path as an absolute one, with . and .. worked out\n"
	   "usage: realpath PATH...\n"
	   "The last part need not exist yet; the folder it is in must.")
{
	char abs[PT_PATH_MAX];
	int status = 0;

	if (argc < 2) {
		pt_dprintf(PT_STDERR, "usage: realpath PATH...\n");
		return 2;
	}
	for (int i = 1; i < argc; i++) {
		struct pt_stat st;
		int err = pt_abspath(argv[i], abs, sizeof(abs));

		/* as GNU's: the last part need not exist yet, the folder must */
		if (!err && (err = pt_stat(abs, &st)) == -ENOENT) {
			char *slash = strrchr(abs, '/');

			*slash = '\0';
			err = pt_stat(slash == abs ? "/" : abs, &st);
			if (!err && !st.is_dir)
				err = -ENOTDIR;
			*slash = '/';
		}
		if (err)
			status = fail("realpath", argv[i], err);
		else
			pt_printf("%s\n", abs);
	}
	return status;
}

/* ------------------------------------------------------------ yes */

PT_PROGRAM(yes, "print a line over and over, until stopped\n"
	   "usage: yes [text...]\n"
	   "  yes | rm -i ...    answers y to every question")
{
	char buf[1024];
	size_t line = 0, n;

	/* the line -- "y", or the arguments with spaces -- then as many
	 * copies of it as fill the buffer, so each write is a big one */
	if (argc == 1)
		buf[line++] = 'y';
	for (int i = 1; i < argc; i++) {
		size_t len = strlen(argv[i]), room = sizeof(buf) / 2 - line - 2;

		if (i > 1 && room) {
			buf[line++] = ' ';
			room--;
		}
		if (len > room)
			len = room;
		memcpy(buf + line, argv[i], len);
		line += len;
	}
	buf[line++] = '\n';
	for (n = line; n + line <= sizeof(buf); n += line)
		memcpy(buf + n, buf, line);
	while (!pt_interrupted() && !write_all(PT_STDOUT, buf, n))
		;
	return 0;
}

/* ------------------------------------------------------------ stat */

PT_PROGRAM(stat, "show what is known about files\n"
	   "usage: stat file...")
{
	int status = 0;

	if (argc < 2) {
		pt_dprintf(PT_STDERR, "usage: stat file...\n");
		return 2;
	}
	for (int i = 1; i < argc; i++) {
		struct pt_stat st;
		char abs[PT_PATH_MAX], when[40] = "unknown (the clock was not set)";
		struct tm tm;
		int err = pt_stat(argv[i], &st);

		if (err) {
			status = fail("stat", argv[i], err);
			continue;
		}
		if (pt_abspath(argv[i], abs, sizeof(abs)))
			snprintf(abs, sizeof(abs), "%s", argv[i]);
		if (st.mtime > 315532800 && localtime_r(&st.mtime, &tm))	/* after 1980 */
			strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
		pt_printf("  File: %s\n  Type: %s\n  Size: %llu bytes\nModify: %s\n",
			  abs, st.is_dir ? "directory" : "regular file",
			  (unsigned long long)st.size, when);
	}
	return status;
}

/* ------------------------------------------------------------ expr */

/*
 * POSIX expr, one argument per token:  a | b, a & b, a < <= = != >= > b,
 * a + - b, a * / % b, and STRING : REGEX -- the basic regular expression
 * anchored at the start, giving the length matched, or \( \) 's text.
 */
#define EXPR_MADE	64

struct expr {
	char	**tok;
	int	  n, at;
	bool	  bad;
	bool	  said;			/* the error has been explained already */
	char	 *made[EXPR_MADE];	/* values worked out along the way */
	int	  nmade;
};

/* Keeps a value made along the way, to free at the end: s, or "" if no room. */
static const char *expr_keep(struct expr *e, char *s)
{
	if (!s || e->nmade == EXPR_MADE) {
		pt_free(s);
		e->bad = true;
		return "";
	}
	e->made[e->nmade++] = s;
	return s;
}

static bool is_int(const char *s, long long *v)
{
	char *end;

	if (!*s)
		return false;
	*v = strtoll(s, &end, 10);
	return !*end;
}

static const char *expr_or(struct expr *e);

static const char *expr_primary(struct expr *e)
{
	if (e->at >= e->n) {
		e->bad = true;
		return "";
	}
	if (!strcmp(e->tok[e->at], "(") && e->at + 1 < e->n) {
		e->at++;
		const char *v = expr_or(e);

		if (e->at >= e->n || strcmp(e->tok[e->at], ")"))
			e->bad = true;
		else
			e->at++;
		return v;
	}
	return e->tok[e->at++];
}

static const char *expr_value(struct expr *e, long long v)
{
	char *s = pt_malloc(24);

	if (s)
		snprintf(s, 24, "%lld", v);
	return expr_keep(e, s);
}

static const char *expr_match(struct expr *e)
{
	const char *left = expr_primary(e);

	while (e->at < e->n && !strcmp(e->tok[e->at], ":")) {
		e->at++;
		const char *pat = expr_primary(e);
		char *anchored = pt_malloc(strlen(pat) + 2), err[96];
		struct regex *re;
		struct re_match m;

		if (!anchored) {
			e->bad = true;
			return "";
		}
		snprintf(anchored, strlen(pat) + 2, "^%s", pat);
		if (re_compile(&re, anchored, 0, err, sizeof(err))) {
			pt_dprintf(PT_STDERR, "expr: %s\n", err);
			e->said = true;
			pt_free(anchored);
			e->bad = true;
			return "";
		}
		bool hit = re_search(re, left, strlen(left), 0, false, &m);

		if (re_groups(re)) {
			size_t n = hit && m.so[1] >= 0 ? (size_t)(m.eo[1] - m.so[1]) : 0;
			char *s = pt_malloc(n + 1);

			if (s) {
				memcpy(s, left + (n ? m.so[1] : 0), n);
				s[n] = '\0';
			}
			left = expr_keep(e, s);
		} else {
			left = expr_value(e, hit ? m.eo[0] : 0);
		}
		re_free(re);
		pt_free(anchored);
	}
	return left;
}

static const char *expr_mul(struct expr *e)
{
	const char *left = expr_match(e);

	while (e->at < e->n && (!strcmp(e->tok[e->at], "*") || !strcmp(e->tok[e->at], "/") ||
				!strcmp(e->tok[e->at], "%"))) {
		char op = e->tok[e->at++][0];
		const char *right = expr_match(e);
		long long a, b;

		if (!is_int(left, &a) || !is_int(right, &b)) {
			pt_dprintf(PT_STDERR, "expr: not a number\n");
			e->said = true;
			e->bad = true;
			return "0";
		}
		if (op != '*' && !b) {
			pt_dprintf(PT_STDERR, "expr: division by zero\n");
			e->said = true;
			e->bad = true;
			return "0";
		}
		left = expr_value(e, op == '*' ? a * b : op == '/' ? a / b : a % b);
	}
	return left;
}

static const char *expr_add(struct expr *e)
{
	const char *left = expr_mul(e);

	while (e->at < e->n && (!strcmp(e->tok[e->at], "+") || !strcmp(e->tok[e->at], "-"))) {
		char op = e->tok[e->at++][0];
		const char *right = expr_mul(e);
		long long a, b;

		if (!is_int(left, &a) || !is_int(right, &b)) {
			pt_dprintf(PT_STDERR, "expr: not a number\n");
			e->said = true;
			e->bad = true;
			return "0";
		}
		left = expr_value(e, op == '+' ? a + b : a - b);
	}
	return left;
}

static const char *expr_cmp(struct expr *e)
{
	static const char *const ops[] = { "<", "<=", "=", "!=", ">=", ">" };
	const char *left = expr_add(e);

	while (e->at < e->n) {
		int op = -1;

		for (int k = 0; k < 6; k++)
			if (!strcmp(e->tok[e->at], ops[k]))
				op = k;
		if (op < 0)
			break;
		e->at++;
		const char *right = expr_add(e);
		long long a, b;
		int c = is_int(left, &a) && is_int(right, &b) ? (a > b) - (a < b) : strcmp(left, right);
		bool r = op == 0 ? c < 0 : op == 1 ? c <= 0 : op == 2 ? c == 0 :
			 op == 3 ? c != 0 : op == 4 ? c >= 0 : c > 0;

		left = r ? "1" : "0";
	}
	return left;
}

static bool expr_null(const char *s)
{
	long long v;

	return !*s || (is_int(s, &v) && !v);
}

static const char *expr_and(struct expr *e)
{
	const char *left = expr_cmp(e);

	while (e->at < e->n && !strcmp(e->tok[e->at], "&")) {
		e->at++;
		const char *right = expr_cmp(e);

		left = expr_null(left) || expr_null(right) ? "0" : left;
	}
	return left;
}

static const char *expr_or(struct expr *e)
{
	const char *left = expr_and(e);

	while (e->at < e->n && !strcmp(e->tok[e->at], "|")) {
		e->at++;
		const char *right = expr_and(e);

		left = !expr_null(left) ? left : !expr_null(right) ? right : "0";
	}
	return left;
}

PT_PROGRAM(expr, "work out an expression\n"
	   "usage: expr EXPRESSION   (each part its own argument)\n"
	   "  expr 2 + 3 \\* 4       14      expr 7 % 3    1\n"
	   "  expr abc = abc        1       expr 5 \\> 3   1\n"
	   "  expr hello : 'h.l'    3 (the length of the match)\n"
	   "  expr notes.txt : '\\(.*\\)\\.txt'    notes\n"
	   "The exit status is 1 when the result is 0 or empty, 2 on an error.")
{
	struct expr e = { .tok = argv + 1, .n = argc - 1 };
	const char *v;

	if (argc < 2) {
		pt_dprintf(PT_STDERR, "usage: expr EXPRESSION\n");
		return 2;
	}
	v = expr_or(&e);
	if (e.at < e.n)
		e.bad = true;
	int status = e.bad ? 2 : expr_null(v);

	if (!e.bad)
		pt_printf("%s\n", v);
	else if (!e.said)
		pt_dprintf(PT_STDERR, "expr: not a valid expression\n");
	for (int i = 0; i < e.nmade; i++)
		pt_free(e.made[i]);
	return status;
}
