/*
 * sh: $((arithmetic)).
 *
 * C integer expressions on 64-bit values: + - * / % << >> < <= > >= == !=
 * & ^ | && || ! ~ ?: and parentheses, plus = += -= *= /= %= on variables.
 * Parameters have been expanded already; a bare name is a variable.
 * Precedence climbing keeps the recursion shallow.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh.h"

struct arith {
	struct sh	*sh;
	const char	*p;
	const char	*error;
	int		 skip;		/* inside a branch that is not taken */
};

static const struct {
	const char	*op;
	int		 prec;
} binops[] = {
	{ "||", 1 }, { "&&", 2 }, { "==", 6 }, { "!=", 6 }, { "<=", 7 }, { ">=", 7 },
	{ "<<", 8 }, { ">>", 8 }, { "|", 3 }, { "^", 4 }, { "&", 5 }, { "<", 7 },
	{ ">", 7 }, { "+", 9 }, { "-", 9 }, { "*", 10 }, { "/", 10 }, { "%", 10 },
};

static long long ternary(struct arith *a);

static void skip_space(struct arith *a)
{
	while (isspace((unsigned char)*a->p))
		a->p++;
}

static long long error(struct arith *a, const char *msg)
{
	if (!a->error)
		a->error = msg;
	return 0;
}

static long long apply(struct arith *a, const char *op, long long x, long long y)
{
	switch (op[0]) {
	case '+': return (long long)((unsigned long long)x + (unsigned long long)y);
	case '-': return (long long)((unsigned long long)x - (unsigned long long)y);
	case '*': return (long long)((unsigned long long)x * (unsigned long long)y);
	case '/':
	case '%':
		if (!y)
			return a->skip ? 0 : error(a, "division by zero");
		if (y == -1)
			return op[0] == '/' ? (long long)(0 - (unsigned long long)x) : 0;
		return op[0] == '/' ? x / y : x % y;
	case '<':
		if (op[1] == '<')
			return (long long)((unsigned long long)x << (y & 63));
		return op[1] == '=' ? x <= y : x < y;
	case '>':
		if (op[1] == '>')
			return x >> (y & 63);
		return op[1] == '=' ? x >= y : x > y;
	case '=': return x == y;
	case '!': return x != y;
	case '&': return x & y;
	case '^': return x ^ y;
	case '|': return x | y;
	}
	return error(a, "syntax error");
}

static long long variable(struct arith *a, const char *name)
{
	const char *v = pt_getenv(name);
	char *end;

	if (!v)
		return 0;
	while (isspace((unsigned char)*v))
		v++;
	if (!*v)
		return 0;
	long long n = strtoll(v, &end, 0);
	while (isspace((unsigned char)*end))
		end++;
	return *end ? error(a, "not a number") : n;
}

static long long primary(struct arith *a)
{
	char name[64];
	long long v;

	skip_space(a);
	char c = *a->p;
	if (sh_stack_low(a->sh))
		return error(a, "too deeply nested");
	if (c == '(') {
		a->p++;
		v = ternary(a);
		skip_space(a);
		if (*a->p != ')')
			return error(a, "missing )");
		a->p++;
		return v;
	}
	if (c && strchr("+-!~", c)) {
		a->p++;
		v = primary(a);
		return c == '-' ? (long long)(0 - (unsigned long long)v) : c == '!' ? !v : c == '~' ? ~v : v;
	}
	if (isdigit((unsigned char)c)) {
		char *end;
		v = strtoll(a->p, &end, 0);
		if (isalnum((unsigned char)*end) || *end == '_')
			return error(a, "bad number");
		a->p = end;
		return v;
	}
	if (!isalpha((unsigned char)c) && c != '_')
		return error(a, "syntax error");

	size_t len = 0;
	while (isalnum((unsigned char)a->p[len]) || a->p[len] == '_')
		len++;
	if (len >= sizeof(name))
		return error(a, "name too long");
	memcpy(name, a->p, len);
	name[len] = '\0';
	a->p += len;
	skip_space(a);

	char op[2] = { 0 };
	if (a->p[0] == '=' && a->p[1] != '=') {
		op[0] = '=';
		a->p++;
	} else if (a->p[0] && strchr("+-*/%", a->p[0]) && a->p[1] == '=') {
		op[0] = a->p[0];
		a->p += 2;
	}
	if (!op[0])
		return variable(a, name);

	long long rhs = ternary(a);
	v = op[0] == '=' ? rhs : apply(a, op, variable(a, name), rhs);
	if (!a->skip && !a->error) {
		char num[24];
		snprintf(num, sizeof(num), "%lld", v);
		pt_setenv(name, num);
	}
	return v;
}

static long long binary(struct arith *a, int min_prec)
{
	long long left = primary(a);

	while (!a->error) {
		size_t i;

		skip_space(a);
		for (i = 0; i < sizeof(binops) / sizeof(binops[0]); i++)
			if (!strncmp(a->p, binops[i].op, strlen(binops[i].op)))
				break;
		if (i == sizeof(binops) / sizeof(binops[0]) || binops[i].prec < min_prec)
			return left;
		const char *op = binops[i].op;
		a->p += strlen(op);
		if (op[1] == op[0] && (op[0] == '&' || op[0] == '|')) {
			bool skip = op[0] == '&' ? !left : !!left;
			a->skip += skip;
			long long right = binary(a, binops[i].prec + 1);
			a->skip -= skip;
			left = op[0] == '&' ? left && right : left || right;
		} else {
			left = apply(a, op, left, binary(a, binops[i].prec + 1));
		}
	}
	return left;
}

static long long ternary(struct arith *a)
{
	long long cond = binary(a, 1);

	skip_space(a);
	if (*a->p != '?' || a->error)
		return cond;
	a->p++;
	a->skip += !cond;
	long long x = ternary(a);
	a->skip -= !cond;
	skip_space(a);
	if (*a->p != ':')
		return error(a, "missing :");
	a->p++;
	a->skip += !!cond;
	long long y = ternary(a);
	a->skip -= !!cond;
	return cond ? x : y;
}

int arith_eval(struct sh *sh, const char *expr, long long *result)
{
	struct arith a = { .sh = sh, .p = expr };

	skip_space(&a);
	*result = *a.p ? ternary(&a) : 0;
	skip_space(&a);
	if (!a.error && *a.p)
		a.error = "syntax error";
	if (!a.error)
		return 0;
	pt_dprintf(PT_STDERR, "sh: arithmetic: %s in '%s'\n", a.error, expr);
	return -1;
}
