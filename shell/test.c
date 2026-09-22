/*
 * test and [: check a condition. A shell builtin and a program.
 *
 * With up to four arguments the POSIX rules decide by count, so that
 * `[ "$x" = -n ]` and `[ ! "$x" ]` mean what they look like. Longer
 * expressions are parsed with ! -a -o and parentheses, -a binding tighter.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "pt/program.h"
#include "sh.h"

struct test {
	char	**argv;
	int	  argc, pos;
	bool	  error;
};

static bool expr_or(struct test *t);

static bool is(const char *s, const char *what)
{
	return !strcmp(s, what);
}

static bool unary_op(const char *s)
{
	return s[0] == '-' && s[1] && !s[2] && strchr("defhLnrstwxz", s[1]);
}

static bool binary_op(const char *s)
{
	static const char *const ops[] = {
		"=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
	};

	for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
		if (is(s, ops[i]))
			return true;
	return false;
}

static void error(struct test *t, const char *what, const char *arg)
{
	if (!t->error)
		pt_dprintf(PT_STDERR, "test: %s%s%s\n", arg ? arg : "", arg ? ": " : "", what);
	t->error = true;
}

static bool integer(struct test *t, const char *s, long long *out)
{
	const char *p = s;
	char *end;

	while (isspace((unsigned char)*p))
		p++;
	*out = strtoll(p, &end, 10);
	while (isspace((unsigned char)*end))
		end++;
	if (end == p || *end) {
		error(t, "integer expected", s);
		return false;
	}
	return true;
}

static bool unary(struct test *t, const char *op, const char *arg)
{
	struct pt_stat st;

	switch (op[1]) {
	case 'z':
		return !*arg;
	case 'n':
		return *arg;
	case 't':
		return pt_isatty(atoi(arg));
	case 'h':
	case 'L':
		return false;	/* no symbolic links */
	}
	if (pt_stat(arg, &st))
		return false;
	switch (op[1]) {
	case 'f':
		return !st.is_dir;
	case 'd':
		return st.is_dir;
	case 's':
		return st.size > 0;
	}
	return true;	/* -e, and -r -w -x: there are no permissions */
}

static bool binary(struct test *t, const char *a, const char *op, const char *b)
{
	long long x, y;

	if (is(op, "=") || is(op, "=="))
		return is(a, b);
	if (is(op, "!="))
		return !is(a, b);
	if (is(op, "<"))
		return strcmp(a, b) < 0;
	if (is(op, ">"))
		return strcmp(a, b) > 0;
	if (!integer(t, a, &x) || !integer(t, b, &y))
		return false;
	switch (op[1] << 8 | op[2]) {
	case 'e' << 8 | 'q': return x == y;
	case 'n' << 8 | 'e': return x != y;
	case 'l' << 8 | 't': return x < y;
	case 'l' << 8 | 'e': return x <= y;
	case 'g' << 8 | 't': return x > y;
	}
	return x >= y;
}

static bool primary(struct test *t)
{
	int left = t->argc - t->pos;
	char **a = t->argv + t->pos;

	if (left <= 0) {
		error(t, "argument expected", NULL);
		return false;
	}
	if (left >= 3 && binary_op(a[1])) {
		t->pos += 3;
		return binary(t, a[0], a[1], a[2]);
	}
	if (is(a[0], "!")) {
		t->pos++;
		return !primary(t);
	}
	if (is(a[0], "(")) {
		t->pos++;
		bool v = expr_or(t);
		if (t->pos >= t->argc || !is(t->argv[t->pos], ")"))
			error(t, "missing )", NULL);
		t->pos++;
		return v;
	}
	if (left >= 2 && unary_op(a[0])) {
		t->pos += 2;
		return unary(t, a[0], a[1]);
	}
	t->pos++;
	return *a[0];
}

static bool expr_and(struct test *t)
{
	bool v = primary(t);

	while (t->pos < t->argc && is(t->argv[t->pos], "-a")) {
		t->pos++;
		v = primary(t) && v;
	}
	return v;
}

static bool expr_or(struct test *t)
{
	bool v = expr_and(t);

	while (t->pos < t->argc && is(t->argv[t->pos], "-o")) {
		t->pos++;
		v = expr_and(t) || v;
	}
	return v;
}

static bool evaluate(struct test *t)
{
	char **a = t->argv;

	switch (t->argc) {
	case 0:
		return false;
	case 1:
		return *a[0];
	case 2:
		if (is(a[0], "!"))
			return !*a[1];
		if (unary_op(a[0]))
			return unary(t, a[0], a[1]);
		break;
	case 3:
		if (binary_op(a[1]))
			return binary(t, a[0], a[1], a[2]);
		if (is(a[0], "(") && is(a[2], ")"))
			return *a[1];
		/* fall through */
	case 4:
		if (is(a[0], "!")) {
			t->argv++;
			t->argc--;
			return !evaluate(t);
		}
		if (t->argc == 4 && is(a[0], "(") && is(a[3], ")")) {
			t->argv++;
			t->argc -= 2;
			return evaluate(t);
		}
		break;
	}
	bool v = expr_or(t);
	if (t->pos < t->argc)
		error(t, "unexpected argument", t->argv[t->pos]);
	return v;
}

int sh_test(int argc, char **argv)
{
	struct test t = { .argv = argv + 1, .argc = argc - 1 };

	if (is(argv[0], "[")) {
		if (argc < 2 || !is(argv[argc - 1], "]")) {
			pt_dprintf(PT_STDERR, "[: missing ]\n");
			return 2;
		}
		t.argc--;
	}
	bool v = evaluate(&t);
	return t.error ? 2 : !v;
}

#define TEST_HELP							\
	"check a condition: status 0 if true, 1 if not\n"		\
	"usage: test expr   or   [ expr ]\n"				\
	"  -e -f -d -s path   exists, file, directory, not empty\n"	\
	"  -z -n string       empty, not empty\n"			\
	"  s1 = s2   s1 != s2\n"					\
	"  n1 -eq n2          also -ne -lt -le -gt -ge\n"		\
	"  ! expr   expr -a expr   expr -o expr   ( expr )"

PT_PROGRAM(test, TEST_HELP)
{
	return sh_test(argc, argv);
}

static int bracket_main(int argc, char **argv)
{
	return sh_test(argc, argv);
}

static struct pt_program bracket = {
	.name = "[",
	.main = bracket_main,
	.help = TEST_HELP,
};

__attribute__((constructor)) static void bracket_register(void)
{
	program_register(&bracket);
}
