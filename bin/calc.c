/*
 * calc: a calculator.
 *
 *	calc 2^10/3            one answer, and back to the shell
 *	calc                   a prompt: ans, variables (r = 2.5), deg and rad
 *	echo 'sqrt(2)' | calc  a line at a time, as bc does
 *
 * A recursive descent over doubles, the way it is written on paper:
 *
 *	expr    = term { (+ | -) term }
 *	term    = unary { (* | / | %) unary }	% between two numbers: the rest
 *	unary   = (+ | -) unary | power
 *	power   = postfix [ ^ unary ]		right to left, and -2^2 is -4
 *	postfix = primary { ! | % }		5! and 50% (a hundredth)
 *	primary = number | name | name ( args ) | ( expr )
 *
 * Numbers: 12, 1.5, .5, 1e-3, 0x1F, 0b101.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

#define VARS		16
#define NAME_MAX_C	16
#define ARGS_MAX	8

struct calc {
	const char	*s, *p;		/* the line, and where the reading is */
	const char	*err;		/* the first mistake found */
	bool		 deg;		/* angles in degrees */
	double		 ans;
	int		 nvars;
	char		 var[VARS][NAME_MAX_C];
	double		 val[VARS];
};

static double expr(struct calc *c);

static void fail_at(struct calc *c, const char *why)
{
	if (!c->err)
		c->err = why;
}

static void blanks(struct calc *c)
{
	while (*c->p == ' ' || *c->p == '\t')
		c->p++;
}

/* Whether a number can start at q: a % before one is a remainder, not a percentage. */
static bool operand_at(const char *q)
{
	while (*q == ' ' || *q == '\t')
		q++;
	return isalnum((unsigned char)*q) || *q == '(' || *q == '.' || *q == '_';
}

static double number(struct calc *c)
{
	char *end;
	double v;

	if (c->p[0] == '0' && (c->p[1] == 'b' || c->p[1] == 'B')) {
		v = 0;
		c->p += 2;
		if (*c->p != '0' && *c->p != '1')
			fail_at(c, "a binary number wants 0s and 1s");
		while (*c->p == '0' || *c->p == '1')
			v = v * 2 + (*c->p++ - '0');
		return v;
	}
	v = strtod(c->p, &end);
	if (end == c->p)
		fail_at(c, "a number is missing");
	c->p = end;
	return v;
}

static double to_rad(const struct calc *c, double x)
{
	return c->deg ? x * M_PI / 180 : x;
}

static double from_rad(const struct calc *c, double x)
{
	return c->deg ? x * 180 / M_PI : x;
}

/* sin(pi) is 1.2e-16 in doubles: on a calculator it is 0. */
static double tidy(double x)
{
	return fabs(x) < 1e-12 ? 0 : x;
}

static double gcd(double a, double b)
{
	a = fabs(a);
	b = fabs(b);
	while (b >= 0.5) {
		double t = fmod(a, b);

		a = b;
		b = t;
	}
	return a;
}

/* name(args): the functions a school calculator has. */
static double call(struct calc *c, const char *name, const double *a, int n)
{
	static const struct {
		const char	*name;
		double		(*f)(double);
	} one[] = {
		{ "sqrt", sqrt }, { "cbrt", cbrt }, { "abs", fabs }, { "exp", exp },
		{ "ln", log }, { "log", log10 }, { "log2", log2 }, { "floor", floor },
		{ "ceil", ceil }, { "round", round }, { "trunc", trunc },
		{ "sinh", sinh }, { "cosh", cosh }, { "tanh", tanh },
	};

	for (size_t i = 0; i < sizeof(one) / sizeof(one[0]); i++)
		if (!strcmp(name, one[i].name)) {
			if (n != 1)
				fail_at(c, "that function takes one number");
			return one[i].f(a[0]);
		}
	if (!strcmp(name, "sin") || !strcmp(name, "cos") || !strcmp(name, "tan")) {
		if (n != 1)
			fail_at(c, "that function takes one number");
		return tidy(name[0] == 's' ? sin(to_rad(c, a[0])) :
			    name[0] == 'c' ? cos(to_rad(c, a[0])) : tan(to_rad(c, a[0])));
	}
	if (!strcmp(name, "asin") || !strcmp(name, "acos") || !strcmp(name, "atan")) {
		if (n != 1)
			fail_at(c, "that function takes one number");
		return from_rad(c, name[1] == 's' ? asin(a[0]) : name[1] == 'c' ? acos(a[0]) :
				   atan(a[0]));
	}
	if (!strcmp(name, "atan2") || !strcmp(name, "hypot") || !strcmp(name, "pow") ||
	    !strcmp(name, "root") || !strcmp(name, "gcd")) {
		if (n != 2) {
			fail_at(c, "that function takes two numbers");
			return 0;
		}
		if (name[0] == 'a')
			return from_rad(c, atan2(a[0], a[1]));
		if (name[0] == 'h')
			return hypot(a[0], a[1]);
		if (name[0] == 'p')
			return pow(a[0], a[1]);
		if (name[0] == 'r')
			return a[0] < 0 && fmod(a[1], 2) == 1 ? -pow(-a[0], 1 / a[1]) :
			       pow(a[0], 1 / a[1]);
		return gcd(a[0], a[1]);
	}
	if (!strcmp(name, "min") || !strcmp(name, "max")) {
		double v = a[0];

		if (n < 1)
			fail_at(c, "min and max want numbers");
		for (int i = 1; i < n; i++)
			v = (name[1] == 'i') == (a[i] < v) ? a[i] : v;
		return v;
	}
	fail_at(c, "no such function");
	return 0;
}

static double *var_slot(struct calc *c, const char *name, bool make)
{
	for (int i = 0; i < c->nvars; i++)
		if (!strcmp(c->var[i], name))
			return &c->val[i];
	if (!make || c->nvars == VARS)
		return NULL;
	strlcpy(c->var[c->nvars], name, NAME_MAX_C);
	c->val[c->nvars] = 0;
	return &c->val[c->nvars++];
}

static double primary(struct calc *c)
{
	char name[NAME_MAX_C];
	size_t n = 0;
	double *slot;

	blanks(c);
	if (*c->p == '(') {
		double v;

		c->p++;
		v = expr(c);
		blanks(c);
		if (*c->p != ')')
			fail_at(c, "a ) is missing");
		else
			c->p++;
		return v;
	}
	if (isdigit((unsigned char)*c->p) || *c->p == '.')
		return number(c);
	if (!isalpha((unsigned char)*c->p) && *c->p != '_') {
		fail_at(c, *c->p ? "a number is missing" : "a number is missing at the end");
		return 0;
	}
	while (isalnum((unsigned char)*c->p) || *c->p == '_') {
		if (n < sizeof(name) - 1)
			name[n++] = tolower((unsigned char)*c->p);
		c->p++;
	}
	name[n] = '\0';
	blanks(c);
	if (*c->p == '(') {
		double a[ARGS_MAX];
		int k = 0;

		c->p++;
		blanks(c);
		if (*c->p != ')')
			for (;;) {
				double v = expr(c);

				if (k < ARGS_MAX)
					a[k++] = v;
				blanks(c);
				if (*c->p != ',')
					break;
				c->p++;
			}
		if (*c->p != ')')
			fail_at(c, "a ) is missing");
		else
			c->p++;
		return c->err ? 0 : call(c, name, a, k);
	}
	if (!strcmp(name, "pi"))
		return M_PI;
	if (!strcmp(name, "e"))
		return M_E;
	if (!strcmp(name, "ans"))
		return c->ans;
	if ((slot = var_slot(c, name, false)))
		return *slot;
	fail_at(c, "not a number, a function or a variable");
	return 0;
}

static double postfix(struct calc *c)
{
	double v = primary(c);

	for (;;) {
		blanks(c);
		if (*c->p == '!') {
			c->p++;
			if (v < 0 || v != floor(v) || v > 170)
				fail_at(c, "! wants a whole number from 0 to 170");
			else
				v = tgamma(v + 1);
		} else if (*c->p == '%' && !operand_at(c->p + 1)) {
			c->p++;
			v /= 100;			/* 50% */
		} else {
			return v;
		}
	}
}

static double unary(struct calc *c);

static double power(struct calc *c)
{
	double base = postfix(c);

	blanks(c);
	if (*c->p == '^') {
		c->p++;
		return pow(base, unary(c));		/* 2^3^2 is 2^9 */
	}
	return base;
}

static double unary(struct calc *c)
{
	blanks(c);
	if (*c->p == '-') {
		c->p++;
		return -unary(c);
	}
	if (*c->p == '+') {
		c->p++;
		return unary(c);
	}
	return power(c);
}

static double term(struct calc *c)
{
	double v = unary(c);

	for (;;) {
		char op;
		double r;

		blanks(c);
		op = *c->p;
		if (op != '*' && op != '/' && op != '%')
			return v;
		c->p++;
		r = unary(c);
		if ((op == '/' || op == '%') && r == 0 && !c->err)
			fail_at(c, "division by zero");
		else if (op == '*')
			v *= r;
		else if (op == '/')
			v /= r;
		else
			v = fmod(v, r);
	}
}

static double expr(struct calc *c)
{
	double v = term(c);

	for (;;) {
		blanks(c);
		if (*c->p == '+') {
			c->p++;
			v += term(c);
		} else if (*c->p == '-') {
			c->p++;
			v -= term(c);
		} else {
			return v;
		}
	}
}

/* A number as a calculator shows it: whole ones whole, the rest to ten figures. */
static void show(double v)
{
	if (v == 0)
		v = 0;					/* not -0 */
	if (fabs(v) < 1e15 && v == floor(v))
		pt_printf("%.0f\n", v);
	else
		pt_printf("%.10g\n", v);
}

/* One line: an expression, or NAME = expression. 0, or 1 after saying what is wrong. */
static int line(struct calc *c, const char *text)
{
	const char *eq = strchr(text, '=');
	char name[NAME_MAX_C] = "";
	double v;

	c->s = c->p = text;
	c->err = NULL;
	if (eq) {
		const char *q = text;
		size_t n = 0;

		while (*q == ' ')
			q++;
		while ((isalnum((unsigned char)*q) || *q == '_') && n < sizeof(name) - 1)
			name[n++] = tolower((unsigned char)*q++);
		name[n] = '\0';
		while (*q == ' ')
			q++;
		if (q != eq || !n || isdigit((unsigned char)name[0]) || !strcmp(name, "pi") ||
		    !strcmp(name, "e") || !strcmp(name, "ans")) {
			pt_dprintf(PT_STDERR, "calc: %s: only a name can be given a value\n", text);
			return 1;
		}
		c->p = eq + 1;
	}
	v = expr(c);
	blanks(c);
	if (!c->err && *c->p)
		c->err = *c->p == ')' ? "a ( is missing" : "that is not part of a sum";
	if (!c->err && isnan(v))
		c->err = "outside what the function takes";
	if (!c->err && isinf(v))
		c->err = "too big";
	if (c->err) {
		pt_dprintf(PT_STDERR, "calc: %s: %s\n", text, c->err);
		return 1;
	}
	if (*name) {
		double *slot = var_slot(c, name, true);

		if (!slot) {
			pt_dprintf(PT_STDERR, "calc: no room for more names\n");
			return 1;
		}
		*slot = v;
	}
	c->ans = v;
	show(v);
	return 0;
}

static const char calc_help[] =
	"calc: + - * / ^, % (the rest, or 50% a hundredth), 5!,\n"
	"( ), 0x1F and 0b101; pi e ans; r = 2.5 keeps a value.\n"
	"sqrt cbrt abs exp ln log log2 floor ceil round trunc\n"
	"sin cos tan asin acos atan sinh cosh tanh, and with\n"
	"two numbers atan2 hypot pow root gcd; min and max.\n"
	"deg or rad for the angles, q to leave.\n";

PT_PROGRAM(calc, "a calculator\n"
	   "usage: calc [-d] [expression]\n"
	   "  calc 2^10/3     calc 'sqrt(2)*(1+3)'     calc 15%\n"
	   "With no expression, a prompt (help there); from a pipe,\n"
	   "a line at a time. -d: angles in degrees. Quote what has\n"
	   "( ) or * in it, or the shell takes them.")
{
	struct calc *c = pt_calloc(1, sizeof(*c));
	char buf[512];
	int i = 1, bad = 0;

	if (!c)
		return fail("calc", NULL, -ENOMEM);
	if (i < argc && !strcmp(argv[i], "-d")) {
		c->deg = true;
		i++;
	}
	if (i < argc) {
		size_t n = 0;

		for (; i < argc; i++)
			n += snprintf(buf + n, n < sizeof(buf) ? sizeof(buf) - n : 0, "%s%s",
				      n ? " " : "", argv[i]);
		if (n >= sizeof(buf)) {
			pt_dprintf(PT_STDERR, "calc: too long\n");
			bad = 1;
		} else {
			bad = line(c, buf);
		}
		pt_free(c);
		return bad;
	}
	for (;;) {
		size_t n = 0;
		ssize_t got;
		char ch;

		if (pt_isatty(PT_STDIN))
			pt_printf("%s> ", c->deg ? "calc deg" : "calc");
		while ((got = pt_read(PT_STDIN, &ch, 1)) == 1 && ch != '\n')
			if (n < sizeof(buf) - 1)
				buf[n++] = ch;
		if (got != 1 && !n)
			break;
		buf[n] = '\0';
		while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\r'))
			buf[--n] = '\0';
		if (!n)
			continue;
		if (!strcmp(buf, "q") || !strcmp(buf, "quit") || !strcmp(buf, "exit"))
			break;
		if (!strcmp(buf, "deg") || !strcmp(buf, "rad")) {
			c->deg = buf[0] == 'd';
			continue;
		}
		if (!strcmp(buf, "help") || !strcmp(buf, "?")) {
			pt_puts(calc_help);
			continue;
		}
		bad |= line(c, buf);
	}
	pt_free(c);
	return pt_isatty(PT_STDIN) ? 0 : bad;
}
