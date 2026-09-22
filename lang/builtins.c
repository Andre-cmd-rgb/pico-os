/*
 * Built-in functions. Each gets its arguments in place on the operand
 * stack, releases the references it was given, and returns 1 with its
 * result in args[0], 0 without a result, or -1 to stop the program (after
 * reporting an error, or for exit()).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "al.h"
#include "port.h"

#define RBUF_SIZE	512

#define STR(v)		((struct al_str *)(v).o)
#define ARR(v)		((struct al_array *)(v).o)
#define FILEOBJ(v)	((struct al_file *)(v).o)

static void rel(struct al_vm *vm, union al_val v)
{
	al_decref(vm, v.o);
}

static int oom(struct al_vm *vm)
{
	al_error(vm, "out of memory");
	return -1;
}

static int ret_int(union al_val *a, int32_t v)
{
	a[0].i = v;
	return 1;
}

static int ret_float(union al_val *a, float v)
{
	a[0].f = v;
	return 1;
}

static int ret_str(struct al_vm *vm, union al_val *a, struct al_str *s)
{
	if (!s)
		return oom(vm);
	a[0].o = &s->h;
	return 1;
}

static int ret_fmt(struct al_vm *vm, union al_val *a, struct al_fmt *f)
{
	struct al_str *s = f->oom ? NULL : al_str_new(vm, f->buf ? f->buf : "", f->len);

	port_free(f->buf);
	return ret_str(vm, a, s);
}

/* A signal arrived while we were blocked: end like the VM does. */
static int interrupted(struct al_vm *vm)
{
	if (!port_interrupted())
		return 0;
	al_flush(vm);
	if (vm->raw) {
		port_tty_raw(false);
		vm->raw = false;
	}
	port_die_interrupted();
	vm->failed = true;
	vm->status = 130;
	return -1;
}

static const char *path(struct al_vm *vm, struct al_str *s)
{
	if (strlen(s->data) != s->len) {
		al_error(vm, "file name contains a NUL byte");
		return NULL;
	}
	return s->data;
}

/* ------------------------------------------------------------ printing */

__attribute__((format(printf, 3, 4)))
static void fmt_printf(struct al_vm *vm, struct al_fmt *out, const char *spec, ...)
{
	va_list ap, copy;
	char small[64];

	va_start(ap, spec);
	va_copy(copy, ap);
	int n = vsnprintf(small, sizeof(small), spec, ap);
	va_end(ap);
	if (n < 0) {
		va_end(copy);
		return;
	}
	if (n < (int)sizeof(small)) {
		al_fmt_puts(vm, out, small, n);
	} else {
		char *big = port_alloc(n + 1);
		if (!big) {
			out->oom = true;
		} else {
			vsnprintf(big, n + 1, spec, copy);
			al_fmt_puts(vm, out, big, n);
			port_free(big);
		}
	}
	va_end(copy);
}

/* Literal text of a format, with %% turned into %. */
static void copy_literal(struct al_vm *vm, struct al_fmt *out, const char *s, const char *end)
{
	while (s < end) {
		const char *pct = memchr(s, '%', end - s);
		if (!pct) {
			al_fmt_puts(vm, out, s, end - s);
			return;
		}
		al_fmt_puts(vm, out, s, pct + 1 - s);
		s = pct + 2;
	}
}

static const char *desc_next(const char *d, const char *end)
{
	while (d < end && *d == '[')
		d++;
	if (d >= end)
		return NULL;
	if (*d == 'S')
		return end - d >= 5 ? d + 5 : NULL;
	return *d && strchr("ifbsFn", *d) ? d + 1 : NULL;
}

static bool desc_is_ref(const char *d)
{
	return *d == '[' || *d == 's' || *d == 'F' || *d == 'S' || *d == 'n';
}

/* Release variadic arguments a[from..to) described by desc. */
static void release_desc(struct al_vm *vm, union al_val *a, int from, int to, struct al_str *desc)
{
	const char *d = desc->data, *end = d + desc->len;

	for (int i = from; i < to && d; i++) {
		if (desc_is_ref(d))
			rel(vm, a[i]);
		d = desc_next(d, end);
	}
}

static int utf8_put(char *out, int32_t cp)
{
	if (cp < 0x80) {
		out[0] = cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = 0xc0 | cp >> 6;
		out[1] = 0x80 | (cp & 0x3f);
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = 0xe0 | cp >> 12;
		out[1] = 0x80 | ((cp >> 6) & 0x3f);
		out[2] = 0x80 | (cp & 0x3f);
		return 3;
	}
	out[0] = 0xf0 | cp >> 18;
	out[1] = 0x80 | ((cp >> 12) & 0x3f);
	out[2] = 0x80 | ((cp >> 6) & 0x3f);
	out[3] = 0x80 | (cp & 0x3f);
	return 4;
}


/* a[0] and the last argument of the print family must be strings */
static bool str_args(struct al_vm *vm, union al_val *a, int argc, bool fmt)
{
	if ((fmt && (!a[0].o || a[0].o->type != OT_STR)) ||
	    !a[argc - 1].o || a[argc - 1].o->type != OT_STR) {
		al_error(vm, "damaged executable (string expected)");
		return false;
	}
	return true;
}

/* printf-style formatting of a[1..argc-2]; a[0] is the format, a[argc-1] the descriptors. */
static bool format_args(struct al_vm *vm, struct al_fmt *out, union al_val *a, int argc)
{
	struct al_str *fmt = STR(a[0]), *desc = STR(a[argc - 1]);
	const char *f = fmt->data, *end = f + fmt->len, *lit = f;
	const char *d = desc->data, *dend = d + desc->len;
	int arg = 1, last = argc - 2;

	for (;;) {
		const char *spec = NULL;
		int conv = al_fmt_next(&f, end, &spec);
		copy_literal(vm, out, lit, conv > 0 ? spec : end);
		if (conv < 0) {
			al_error(vm, "bad conversion in format string \"%.40s\"", fmt->data);
			return false;
		}
		if (conv == 0)
			break;
		lit = f;
		const char *dnext = d ? desc_next(d, dend) : NULL;
		if (arg > last || !dnext) {
			al_error(vm, "format string \"%.40s\" wants more arguments", fmt->data);
			return false;
		}
		char sp[24];
		size_t sl = f - spec - 1;
		if (sl > sizeof(sp) - 4)
			sl = sizeof(sp) - 4;
		memcpy(sp, spec, sl);
		union al_val v = a[arg];
		bool fits = true;

		switch (conv) {
		case 'd':
		case 'i':
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			if (*d != 'i' && *d != 'b') {
				fits = false;
				break;
			}
			sp[sl] = conv;
			sp[sl + 1] = '\0';
			if ((conv == 'd' || conv == 'i') && sl == 1) {
				char num[12];		/* plain %d: no flags or width */
				al_fmt_puts(vm, out, num, al_fmt_int(num, v.i));
			} else if (conv == 'd' || conv == 'i')
				fmt_printf(vm, out, sp, (int)v.i);
			else
				fmt_printf(vm, out, sp, (unsigned)(uint32_t)v.i);
			break;
		case 'c': {
			char ch[5];
			if (*d != 'i' && *d != 'b') {
				fits = false;
				break;
			}
			int n = v.i >= 0 && v.i <= 0x10ffff ? utf8_put(ch, v.i) : 0;
			ch[n] = '\0';
			memcpy(sp + sl, "s", 2);
			fmt_printf(vm, out, sp, ch);
			break;
		}
		case 'f':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
			if (*d != 'f' && *d != 'i') {
				fits = false;
				break;
			}
			sp[sl] = conv;
			sp[sl + 1] = '\0';
			fmt_printf(vm, out, sp, *d == 'f' ? (double)v.f : (double)v.i);
			break;
		default: {
			memcpy(sp + sl, "s", 2);
			struct al_fmt tmp = { 0 };
			const char *sub = d;
			if (*d == 's' && v.o && v.o->type == OT_STR)
				al_fmt_puts(vm, &tmp, STR(v)->data, STR(v)->len);
			else
				al_fmt_value(vm, &tmp, v, &sub, 0, false);
			if (sl == 1 || !tmp.buf || strlen(tmp.buf) != tmp.len)
				al_fmt_puts(vm, out, tmp.buf ? tmp.buf : "", tmp.len);
			else
				fmt_printf(vm, out, sp, tmp.buf);
			if (tmp.oom)
				out->oom = true;
			port_free(tmp.buf);
			break;
		}
		}
		if (!fits) {
			al_error(vm, "%%%c does not fit argument %d of \"%.40s\"", conv, arg, fmt->data);
			return false;
		}
		arg++;
		d = dnext;
	}
	if (arg <= last) {
		al_error(vm, "more arguments than %% conversions in \"%.40s\"", fmt->data);
		return false;
	}
	if (out->oom) {
		oom(vm);
		return false;
	}
	return true;
}

static int bi_printf(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_fmt out = { 0 };

	if (!str_args(vm, a, argc, true))
		return -1;
	if (!format_args(vm, &out, a, argc)) {
		port_free(out.buf);
		return -1;
	}
	al_out(vm, 1, out.buf ? out.buf : "", out.len);
	port_free(out.buf);
	release_desc(vm, a, 1, argc - 1, STR(a[argc - 1]));
	rel(vm, a[0]);
	rel(vm, a[argc - 1]);
	return 0;
}

static int bi_format(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_fmt out = { 0 };

	if (!str_args(vm, a, argc, true))
		return -1;
	if (!format_args(vm, &out, a, argc)) {
		port_free(out.buf);
		return -1;
	}
	release_desc(vm, a, 1, argc - 1, STR(a[argc - 1]));
	rel(vm, a[0]);
	rel(vm, a[argc - 1]);
	return ret_fmt(vm, a, &out);
}

static int print_values(struct al_vm *vm, union al_val *a, int argc, bool newline)
{
	struct al_fmt out = { 0 };

	if (!str_args(vm, a, argc, false))
		return -1;
	struct al_str *desc = STR(a[argc - 1]);
	const char *d = desc->data;

	for (int i = 0; i < argc - 1 && d && *d; i++) {
		if (*d == 's' && a[i].o && a[i].o->type == OT_STR) {
			al_fmt_puts(vm, &out, STR(a[i])->data, STR(a[i])->len);
			d++;
		} else {
			al_fmt_value(vm, &out, a[i], &d, 0, false);
		}
	}
	if (newline)
		al_fmt_puts(vm, &out, "\n", 1);
	if (out.oom) {
		port_free(out.buf);
		return oom(vm);
	}
	al_out(vm, 1, out.buf ? out.buf : "", out.len);
	port_free(out.buf);
	release_desc(vm, a, 0, argc - 1, desc);
	rel(vm, a[argc - 1]);
	return 0;
}

static int bi_print(struct al_vm *vm, union al_val *a, int argc)
{
	return print_values(vm, a, argc, false);
}

static int bi_println(struct al_vm *vm, union al_val *a, int argc)
{
	return print_values(vm, a, argc, true);
}

/* ------------------------------------------------------------ strings */

static int bi_substr(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_str *s = STR(a[0]);
	int32_t start = a[1].i;
	int32_t count = argc > 2 ? a[2].i : INT32_MAX;

	if (start < 0 || (uint32_t)start > s->len) {
		al_error(vm, "substr() start %d is outside the string (length %u)", (int)start, (unsigned)s->len);
		return -1;
	}
	if (count < 0) {
		al_error(vm, "substr() count %d is negative", (int)count);
		return -1;
	}
	if ((uint32_t)count > s->len - start)
		count = s->len - start;
	struct al_str *r = al_str_new(vm, s->data + start, count);
	rel(vm, a[0]);
	return ret_str(vm, a, r);
}

static int32_t search(const struct al_str *s, const char *needle, uint32_t n, uint32_t from)
{
	if (n == 0)
		return from <= s->len ? (int32_t)from : -1;
	for (uint32_t i = from; n <= s->len && i <= s->len - n; i++)
		if (s->data[i] == needle[0] && !memcmp(s->data + i, needle, n))
			return i;
	return -1;
}

/* The compiler always picks @finds or @finda; this keeps the table aligned. */
static int bi_find(struct al_vm *vm, union al_val *a, int argc)
{
	al_error(vm, "damaged executable (find)");
	return -1;
}

static int bi_finds(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_str *s = STR(a[0]), *sub = STR(a[1]);
	int32_t from = argc > 2 ? a[2].i : 0;

	if (from < 0)
		from = 0;
	int32_t r = (uint32_t)from > s->len ? -1 : search(s, sub->data, sub->len, from);
	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_int(a, r);
}

static int bi_finda(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_array *arr = ARR(a[0]);
	union al_val v = a[1];
	int32_t r = -1;

	if (!arr) {
		al_error(vm, "find() in a null array");
		return -1;
	}
	for (uint32_t i = 0; i < arr->len && r < 0; i++) {
		union al_val x = arr->items[i];
		switch (arr->h.kind) {
		case K_INT:
		case K_BOOL:
			if (x.i == v.i)
				r = i;
			break;
		case K_FLOAT:
			if (x.f == v.f)
				r = i;
			break;
		case K_STR:
			if (x.o && v.o && !al_str_cmp(STR(x), STR(v)))
				r = i;
			break;
		default:
			if (x.o == v.o)
				r = i;
			break;
		}
	}
	if (KIND_IS_REF(arr->h.kind))
		rel(vm, v);
	rel(vm, a[0]);
	return ret_int(a, r);
}

static bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool push_str(struct al_vm *vm, struct al_array *arr, const char *s, size_t n)
{
	struct al_str *p = al_str_new(vm, s, n);

	if (!p)
		return false;
	if (!al_array_push(vm, arr, (union al_val){ .o = &p->h })) {
		al_decref(vm, &p->h);
		return false;
	}
	return true;
}

static int bi_split(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_str *s = STR(a[0]), *sep = argc > 1 ? STR(a[1]) : NULL;
	struct al_array *arr = al_array_new(vm, K_STR, 0);
	bool ok = arr != NULL;

	if (ok && (!sep || !sep->len)) {
		uint32_t i = 0;
		while (ok && i < s->len) {
			while (i < s->len && is_space(s->data[i]))
				i++;
			uint32_t start = i;
			while (i < s->len && !is_space(s->data[i]))
				i++;
			if (i > start)
				ok = push_str(vm, arr, s->data + start, i - start);
		}
	} else if (ok) {
		uint32_t start = 0;
		for (;;) {
			int32_t at = search(s, sep->data, sep->len, start);
			uint32_t end = at < 0 ? s->len : (uint32_t)at;
			if (!(ok = push_str(vm, arr, s->data + start, end - start)) || at < 0)
				break;
			start = at + sep->len;
		}
	}
	if (!ok) {
		if (arr)
			al_decref(vm, &arr->h);
		return oom(vm);
	}
	rel(vm, a[0]);
	if (argc > 1)
		rel(vm, a[1]);
	a[0].o = &arr->h;
	return 1;
}

static int bi_join(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_array *arr = ARR(a[0]);
	struct al_str *sep = STR(a[1]);
	struct al_fmt out = { 0 };

	if (!arr) {
		al_error(vm, "join() of a null array");
		return -1;
	}
	for (uint32_t i = 0; i < arr->len; i++) {
		if (i)
			al_fmt_puts(vm, &out, sep->data, sep->len);
		if (arr->items[i].o)
			al_fmt_puts(vm, &out, STR(arr->items[i])->data, STR(arr->items[i])->len);
	}
	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_fmt(vm, a, &out);
}

static int bi_trim(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_str *s = STR(a[0]);
	uint32_t start = 0, end = s->len;

	while (start < end && is_space(s->data[start]))
		start++;
	while (end > start && is_space(s->data[end - 1]))
		end--;
	if (start == 0 && end == s->len)
		return 1;		/* unchanged: hand the same string back */
	struct al_str *r = al_str_new(vm, s->data + start, end - start);
	rel(vm, a[0]);
	return ret_str(vm, a, r);
}

static int change_case(struct al_vm *vm, union al_val *a, bool up)
{
	struct al_str *s = STR(a[0]);
	struct al_str *r = al_str_new(vm, s->data, s->len);

	if (r) {
		for (uint32_t i = 0; i < r->len; i++) {
			char c = r->data[i];
			if (up && c >= 'a' && c <= 'z')
				r->data[i] = c - 32;
			else if (!up && c >= 'A' && c <= 'Z')
				r->data[i] = c + 32;
		}
	}
	rel(vm, a[0]);
	return ret_str(vm, a, r);
}

static int bi_upper(struct al_vm *vm, union al_val *a, int argc)
{
	return change_case(vm, a, true);
}

static int bi_lower(struct al_vm *vm, union al_val *a, int argc)
{
	return change_case(vm, a, false);
}

static int bi_replace(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_str *s = STR(a[0]), *old = STR(a[1]), *new = STR(a[2]);
	struct al_fmt out = { 0 };
	uint32_t pos = 0;

	if (!old->len) {
		rel(vm, a[1]);
		rel(vm, a[2]);
		return 1;
	}
	for (;;) {
		int32_t at = search(s, old->data, old->len, pos);
		if (at < 0)
			break;
		al_fmt_puts(vm, &out, s->data + pos, at - pos);
		al_fmt_puts(vm, &out, new->data, new->len);
		pos = at + old->len;
	}
	al_fmt_puts(vm, &out, s->data + pos, s->len - pos);
	rel(vm, a[0]);
	rel(vm, a[1]);
	rel(vm, a[2]);
	return ret_fmt(vm, a, &out);
}

static int affix(struct al_vm *vm, union al_val *a, bool start)
{
	struct al_str *s = STR(a[0]), *p = STR(a[1]);
	bool r = p->len <= s->len &&
		 !memcmp(start ? s->data : s->data + s->len - p->len, p->data, p->len);

	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_int(a, r);
}

static int bi_contains(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_str *s = STR(a[0]), *p = STR(a[1]);
	bool found = p->len == 0;

	for (size_t i = 0; !found && p->len <= s->len && i + p->len <= s->len; i++)
		found = !memcmp(s->data + i, p->data, p->len);
	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_int(a, found);
}

static int bi_repeat(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_str *s = STR(a[0]);
	int32_t times = a[1].i;
	struct al_str *r;
	char *out;

	if (times < 0)
		times = 0;
	if (times && s->len > (size_t)1 << 20 / (size_t)times) {
		al_error(vm, "repeat() would make a string of %lu bytes",
			 (unsigned long)(s->len * (size_t)times));
		rel(vm, a[0]);
		return -1;
	}
	out = al_alloc(vm, s->len * times + 1);
	if (!out) {
		rel(vm, a[0]);
		return -1;
	}
	for (int32_t i = 0; i < times; i++)
		memcpy(out + i * s->len, s->data, s->len);
	r = al_str_new(vm, out, s->len * times);
	al_free(vm, out, s->len * times + 1);
	rel(vm, a[0]);
	return ret_str(vm, a, r);
}

static int bi_starts_with(struct al_vm *vm, union al_val *a, int argc)
{
	return affix(vm, a, true);
}

static int bi_ends_with(struct al_vm *vm, union al_val *a, int argc)
{
	return affix(vm, a, false);
}

static int bi_chr(struct al_vm *vm, union al_val *a, int argc)
{
	char buf[4];

	if (a[0].i < 0 || a[0].i > 0x10ffff) {
		al_error(vm, "chr(%d): not a Unicode code point", (int)a[0].i);
		return -1;
	}
	return ret_str(vm, a, al_str_new(vm, buf, utf8_put(buf, a[0].i)));
}

static int bi_ord(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_str *s = STR(a[0]);
	const unsigned char *p = (const unsigned char *)s->data;
	int32_t cp;

	if (!s->len) {
		al_error(vm, "ord() of an empty string");
		return -1;
	}
	cp = p[0];
	int n = cp >= 0xf0 ? 4 : cp >= 0xe0 ? 3 : cp >= 0xc0 ? 2 : 1;
	if (n > 1 && (uint32_t)n <= s->len) {
		int32_t v = cp & (0x3f >> (n - 1));
		bool ok = true;
		for (int k = 1; k < n; k++) {
			ok &= (p[k] & 0xc0) == 0x80;
			v = v << 6 | (p[k] & 0x3f);
		}
		if (ok)
			cp = v;
	}
	rel(vm, a[0]);
	return ret_int(a, cp);
}

static int bi_to_int(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_str *s = STR(a[0]);
	int32_t result = argc > 1 ? a[1].i : 0;
	const char *p = s->data, *end = p + s->len;
	bool neg = false;
	uint64_t v = 0;
	int base = 10, digits = 0;

	while (p < end && is_space(*p))
		p++;
	while (end > p && is_space(end[-1]))
		end--;
	if (p < end && (*p == '-' || *p == '+'))
		neg = *p++ == '-';
	if (end - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		p += 2;
	}
	for (; p < end; p++, digits++) {
		int d = *p >= '0' && *p <= '9' ? *p - '0' :
			base == 16 && (*p | 0x20) >= 'a' && (*p | 0x20) <= 'f' ? (*p | 0x20) - 'a' + 10 : -1;
		if (d < 0 || v > 0xffffffffu)
			break;
		v = v * base + d;
	}
	if (p == end && digits && (base == 16 ? v <= 0xffffffffu : v <= (neg ? 0x80000000u : 0x7fffffffu)))
		result = neg ? (int32_t)(0u - (uint32_t)v) : (int32_t)(uint32_t)v;
	rel(vm, a[0]);
	return ret_int(a, result);
}

static int bi_to_float(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_str *s = STR(a[0]);
	float result = argc > 1 ? a[1].f : 0;
	char *end;

	if (strlen(s->data) == s->len) {
		const char *p = s->data;
		while (is_space(*p))
			p++;
		if (*p) {
			float v = strtof(p, &end);
			while (is_space(*end))
				end++;
			if (end != p && !*end)
				result = v;
		}
	}
	rel(vm, a[0]);
	return ret_float(a, result);
}

/* ------------------------------------------------------------ arrays */

static struct al_array *array_arg(struct al_vm *vm, union al_val v, const char *fn)
{
	if (!v.o) {
		al_error(vm, "%s() of a null array", fn);
		return NULL;
	}
	if (v.o->type != OT_ARRAY) {
		al_error(vm, "damaged executable (%s() wants an array)", fn);
		return NULL;
	}
	return ARR(v);
}


static bool array_room(struct al_vm *vm, struct al_array *arr, uint32_t n)
{
	if (n <= arr->cap)
		return true;
	if (n > (1u << 26))
		return false;
	uint32_t cap = arr->cap * 2 > n ? arr->cap * 2 : n;
	union al_val *items = al_grow(vm, arr->items, (size_t)arr->cap * sizeof(*items), (size_t)cap * sizeof(*items));
	if (!items)
		return false;
	arr->items = items;
	arr->cap = cap;
	return true;
}

static int bi_insert(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_array *arr = array_arg(vm, a[0], "insert");
	int32_t i = a[1].i;

	if (!arr)
		return -1;
	if (i < 0 || (uint32_t)i > arr->len) {
		al_error(vm, "insert() index %d out of range (length %u)", (int)i, (unsigned)arr->len);
		return -1;
	}
	if (!array_room(vm, arr, arr->len + 1))
		return oom(vm);
	memmove(arr->items + i + 1, arr->items + i, (arr->len - i) * sizeof(*arr->items));
	arr->items[i] = a[2];
	arr->len++;
	rel(vm, a[0]);
	return 0;
}

static int bi_remove_at(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_array *arr = array_arg(vm, a[0], "remove_at");
	int32_t i = a[1].i;

	if (!arr)
		return -1;
	if (i < 0 || (uint32_t)i >= arr->len) {
		al_error(vm, "remove_at() index %d out of range (length %u)", (int)i, (unsigned)arr->len);
		return -1;
	}
	union al_val v = arr->items[i];
	memmove(arr->items + i, arr->items + i + 1, (arr->len - i - 1) * sizeof(*arr->items));
	arr->len--;
	rel(vm, a[0]);
	a[0] = v;
	return 1;
}

static int bi_resize(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_array *arr = array_arg(vm, a[0], "resize");
	int32_t n = a[1].i;

	if (!arr)
		return -1;
	if (n < 0) {
		al_error(vm, "resize() to negative length %d", (int)n);
		return -1;
	}
	if ((uint32_t)n > arr->len && !array_room(vm, arr, n))
		return oom(vm);
	while (arr->len > (uint32_t)n) {
		union al_val v = arr->items[--arr->len];
		if (KIND_IS_REF(arr->h.kind))
			rel(vm, v);
	}
	while (arr->len < (uint32_t)n) {
		union al_val *v = &arr->items[arr->len++];
		memset(v, 0, sizeof(*v));
		if (arr->h.kind == K_STR) {
			v->o = &vm->empty->h;
			vm->empty->h.refs++;
		}
	}
	rel(vm, a[0]);
	return 0;
}

static int bi_slice(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_array *arr = array_arg(vm, a[0], "slice");
	int32_t start = a[1].i, end = a[2].i;

	if (!arr)
		return -1;
	if (start < 0 || (uint32_t)start > arr->len || end < start) {
		al_error(vm, "slice(%d, %d) of an array of length %u", (int)start, (int)end, (unsigned)arr->len);
		return -1;
	}
	if ((uint32_t)end > arr->len)
		end = arr->len;
	struct al_array *r = al_array_new(vm, arr->h.kind, end - start);
	if (!r)
		return oom(vm);
	for (int32_t i = start; i < end; i++) {
		union al_val v = arr->items[i];
		if (KIND_IS_REF(arr->h.kind))
			al_incref(v.o);
		r->items[r->len++] = v;
	}
	rel(vm, a[0]);
	a[0].o = &r->h;
	return 1;
}

static int cmp_int(const void *x, const void *y)
{
	int32_t a = ((const union al_val *)x)->i, b = ((const union al_val *)y)->i;

	return a < b ? -1 : a > b;
}

static int cmp_float(const void *x, const void *y)
{
	float a = ((const union al_val *)x)->f, b = ((const union al_val *)y)->f;

	return a < b ? -1 : a > b;
}

static int cmp_str(const void *x, const void *y)
{
	const struct al_str *a = STR(*(const union al_val *)x), *b = STR(*(const union al_val *)y);

	if (!a || !b)
		return !a - !b;
	return al_str_cmp(a, b);
}

/* Turns an array back to front, in place. */
static int bi_reverse(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_array *arr = array_arg(vm, a[0], "reverse");

	if (!arr)
		return -1;
	for (size_t i = 0, k = arr->len ? arr->len - 1 : 0; i < k; i++, k--) {
		union al_val tmp = arr->items[i];

		arr->items[i] = arr->items[k];
		arr->items[k] = tmp;
	}
	rel(vm, a[0]);
	return 0;
}

static int bi_sort(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_array *arr = array_arg(vm, a[0], "sort");

	if (!arr)
		return -1;
	if (arr->len > 1)
		qsort(arr->items, arr->len, sizeof(*arr->items),
		      arr->h.kind == K_FLOAT ? cmp_float : arr->h.kind == K_STR ? cmp_str : cmp_int);
	rel(vm, a[0]);
	return 0;
}

/* ------------------------------------------------------------ math */

static int bi_abs(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_int(a, a[0].i < 0 ? (int32_t)(0u - (uint32_t)a[0].i) : a[0].i);
}

static int bi_absf(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_float(a, fabsf(a[0].f));
}

static int bi_min(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_int(a, a[0].i < a[1].i ? a[0].i : a[1].i);
}

static int bi_max(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_int(a, a[0].i > a[1].i ? a[0].i : a[1].i);
}

static int bi_minf(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_float(a, a[0].f < a[1].f ? a[0].f : a[1].f);
}

static int bi_maxf(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_float(a, a[0].f > a[1].f ? a[0].f : a[1].f);
}

static int bi_sqrt(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_float(a, sqrtf(a[0].f));
}

static int bi_tan(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_float(a, tanf(a[0].f));
}

static int bi_ln(struct al_vm *vm, union al_val *a, int argc)
{
	if (a[0].f <= 0) {
		al_error(vm, "ln() of %g", (double)a[0].f);
		return -1;
	}
	return ret_float(a, logf(a[0].f));
}

static int bi_exp(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_float(a, expf(a[0].f));
}

static int bi_hypot(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_float(a, hypotf(a[0].f, a[1].f));
}

/*
 * assert(cond) stops the program where the mistake is, rather than
 * letting it carry on with something impossible. A message is optional.
 */
static int bi_assert(struct al_vm *vm, union al_val *a, int argc)
{
	bool ok = a[0].i != 0;
	struct al_str *msg = argc > 1 ? STR(a[1]) : NULL;

	if (!ok)
		al_error(vm, "assertion failed%s%.*s", msg ? ": " : "",
			 msg ? (int)msg->len : 0, msg ? msg->data : "");
	if (argc > 1)
		rel(vm, a[1]);
	return ok ? 0 : -1;
}

static int bi_pow(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_float(a, powf(a[0].f, a[1].f));
}

static int bi_sin(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_float(a, sinf(a[0].f));
}

static int bi_cos(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_float(a, cosf(a[0].f));
}

static int bi_atan2(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_float(a, atan2f(a[0].f, a[1].f));
}

static int bi_floor(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_int(a, al_float_to_int(floorf(a[0].f)));
}

static int bi_ceil(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_int(a, al_float_to_int(ceilf(a[0].f)));
}

static int bi_round(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_int(a, al_float_to_int(roundf(a[0].f)));
}

static int bi_random(struct al_vm *vm, union al_val *a, int argc)
{
	uint32_t x = vm->rng;

	if (a[0].i <= 0) {
		al_error(vm, "random(%d): the limit must be positive", (int)a[0].i);
		return -1;
	}
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	vm->rng = x;
	return ret_int(a, (int32_t)(((uint64_t)x * (uint32_t)a[0].i) >> 32));
}

static int bi_seed(struct al_vm *vm, union al_val *a, int argc)
{
	uint32_t x = (uint32_t)a[0].i * 2654435761u + 0x9e3779b9u;

	vm->rng = x ? x : 1;
	for (int i = 0; i < 4; i++) {
		x = vm->rng;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		vm->rng = x;
	}
	return 0;
}

/* ------------------------------------------------------------ files */

static int bi_open(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_str *name = STR(a[0]), *mode = STR(a[1]);
	const char *p = path(vm, name);
	int m;

	if (!p)
		return -1;
	if (!strcmp(mode->data, "r"))
		m = PORT_O_READ;
	else if (!strcmp(mode->data, "w"))
		m = PORT_O_WRITE;
	else if (!strcmp(mode->data, "a"))
		m = PORT_O_APPEND;
	else {
		al_error(vm, "open() mode must be \"r\", \"w\" or \"a\", not \"%.10s\"", mode->data);
		return -1;
	}
	int fd = port_open(p, m);
	struct al_file *f = NULL;
	if (fd >= 0) {
		f = al_file_new(vm, fd, false);
		if (!f) {
			port_close(fd);
			return oom(vm);
		}
	}
	rel(vm, a[0]);
	rel(vm, a[1]);
	a[0].o = f ? &f->h : NULL;
	return 1;
}

static struct al_file *file_arg(struct al_vm *vm, union al_val v, const char *fn)
{
	struct al_file *f = FILEOBJ(v);

	if (f && f->h.type != OT_FILE)
		al_error(vm, "damaged executable (%s() wants a File)", fn);
	else if (!f)
		al_error(vm, "%s() on a null File (did open() fail?)", fn);
	else if (f->fd < 0)
		al_error(vm, "%s() on a closed File", fn);
	else
		return f;
	return NULL;
}

static int bi_close(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_file *f = file_arg(vm, a[0], "close");

	if (!f)
		return -1;
	if (f->fd == 1)
		al_flush(vm);
	if (!f->std) {
		port_close(f->fd);
		f->fd = -1;
	}
	rel(vm, a[0]);
	return 0;
}

/* Fill the read buffer; false at end of file. */
static bool fill(struct al_vm *vm, struct al_file *f)
{
	if (f->rpos < f->rlen)
		return true;
	if (f->eof)
		return false;
	if (!f->rbuf && !(f->rbuf = al_alloc(vm, RBUF_SIZE)))
		return false;
	if (f->fd == 0)
		al_flush(vm);
	long n = port_read(f->fd, f->rbuf, RBUF_SIZE);
	if (n <= 0) {
		f->eof = true;
		return false;
	}
	f->rpos = 0;
	f->rlen = n;
	return true;
}

static int bi_read(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_file *f = file_arg(vm, a[0], "read");
	int64_t want = argc > 1 ? a[1].i : INT64_MAX;
	struct al_fmt out = { 0 };

	if (!f)
		return -1;
	while (want > 0 && !out.oom && fill(vm, f)) {
		uint32_t n = f->rlen - f->rpos;
		if (n > want)
			n = want;
		al_fmt_puts(vm, &out, f->rbuf + f->rpos, n);
		f->rpos += n;
		want -= n;
		if (interrupted(vm)) {
			port_free(out.buf);
			return -1;
		}
	}
	if (interrupted(vm)) {
		port_free(out.buf);
		return -1;
	}
	rel(vm, a[0]);
	return ret_fmt(vm, a, &out);
}

static int bi_readline(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_file *f = file_arg(vm, a[0], "readline");
	struct al_fmt out = { 0 };

	if (!f)
		return -1;
	while (!out.oom && fill(vm, f)) {
		char *start = f->rbuf + f->rpos;
		size_t avail = f->rlen - f->rpos;
		char *nl = memchr(start, '\n', avail);
		size_t n = nl ? (size_t)(nl - start) + 1 : avail;
		al_fmt_puts(vm, &out, start, n);
		f->rpos += n;
		if (nl)
			break;
	}
	if (interrupted(vm)) {
		port_free(out.buf);
		return -1;
	}
	rel(vm, a[0]);
	return ret_fmt(vm, a, &out);
}

static int bi_write(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_file *f = file_arg(vm, a[0], "write");
	struct al_str *s = STR(a[1]);

	if (!f)
		return -1;
	int r = al_out(vm, f->fd, s->data, s->len);
	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_int(a, r == 0);
}

static int path_op(struct al_vm *vm, union al_val *a, int (*op)(const char *))
{
	const char *p = path(vm, STR(a[0]));

	if (!p)
		return -1;
	int r = op(p);
	rel(vm, a[0]);
	return ret_int(a, r == 0);
}

static int exists(const char *p)
{
	bool dir;
	int64_t size;

	return port_stat(p, &dir, &size);
}

static int is_dir(const char *p)
{
	bool dir;
	int64_t size;

	return port_stat(p, &dir, &size) ? -1 : dir ? 0 : -1;
}

static int bi_exists(struct al_vm *vm, union al_val *a, int argc)
{
	return path_op(vm, a, exists);
}

static int bi_is_dir(struct al_vm *vm, union al_val *a, int argc)
{
	return path_op(vm, a, is_dir);
}

static int bi_remove(struct al_vm *vm, union al_val *a, int argc)
{
	return path_op(vm, a, port_remove);
}

static int bi_mkdir(struct al_vm *vm, union al_val *a, int argc)
{
	return path_op(vm, a, port_mkdir);
}

static int bi_rename(struct al_vm *vm, union al_val *a, int argc)
{
	const char *from = path(vm, STR(a[0])), *to = path(vm, STR(a[1]));

	if (!from || !to)
		return -1;
	int r = port_rename(from, to);
	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_int(a, r == 0);
}

struct listing {
	struct al_vm	*vm;
	struct al_array	*arr;
	bool		 oom;
};

static int list_one(void *ctx, const char *name)
{
	struct listing *l = ctx;

	if (!push_str(l->vm, l->arr, name, strlen(name))) {
		l->oom = true;
		return 1;
	}
	return 0;
}

static int bi_listdir(struct al_vm *vm, union al_val *a, int argc)
{
	const char *p = path(vm, STR(a[0]));
	struct listing l = { vm, NULL, false };

	if (!p)
		return -1;
	if (!(l.arr = al_array_new(vm, K_STR, 0)))
		return oom(vm);
	port_listdir(p, list_one, &l);
	if (l.oom) {
		al_decref(vm, &l.arr->h);
		return oom(vm);
	}
	if (l.arr->len > 1)
		qsort(l.arr->items, l.arr->len, sizeof(*l.arr->items), cmp_str);
	rel(vm, a[0]);
	a[0].o = &l.arr->h;
	return 1;
}

/* ------------------------------------------------------------ processes */

static int bi_exit(struct al_vm *vm, union al_val *a, int argc)
{
	vm->halted = true;
	vm->status = a[0].i;
	al_flush(vm);
	return -1;
}

static int run_argv(struct al_vm *vm, union al_val *items, uint32_t n)
{
	const char **argv = port_alloc((n + 1) * sizeof(*argv));

	if (!argv)
		return oom(vm);
	for (uint32_t i = 0; i < n; i++) {
		if (!items[i].o || !(argv[i] = path(vm, STR(items[i])))) {
			if (!items[i].o)
				al_error(vm, "run() argument is null");
			port_free(argv);
			return -1;
		}
	}
	argv[n] = NULL;
	al_flush(vm);
	int status = n ? port_run(n, argv) : -1;
	port_free(argv);
	if (status == PORT_EINTR) {
		if (vm->raw) {
			port_tty_raw(false);
			vm->raw = false;
		}
		port_die_interrupted();
		vm->failed = true;
		vm->status = 130;
		return -1;
	}
	return status < 0 ? -1 : status;
}

static int bi_run(struct al_vm *vm, union al_val *a, int argc)
{
	int status = run_argv(vm, a, argc);

	if (vm->failed || interrupted(vm))
		return -1;
	for (int i = 0; i < argc; i++)
		rel(vm, a[i]);
	return ret_int(a, status);
}

static int bi_runa(struct al_vm *vm, union al_val *a, int argc)
{
	struct al_array *arr = array_arg(vm, a[0], "run");

	if (!arr)
		return -1;
	if (!arr->len) {
		al_error(vm, "run() needs a command");
		return -1;
	}
	int status = run_argv(vm, arr->items, arr->len);
	if (vm->failed || interrupted(vm))
		return -1;
	rel(vm, a[0]);
	return ret_int(a, status);
}

static int bi_getenv(struct al_vm *vm, union al_val *a, int argc)
{
	const char *v = port_getenv(STR(a[0])->data);

	rel(vm, a[0]);
	return ret_str(vm, a, al_str_new(vm, v ? v : "", v ? strlen(v) : 0));
}

/* ------------------------------------------------------------ time */

static int bi_sleep_ms(struct al_vm *vm, union al_val *a, int argc)
{
	al_flush(vm);
	if (a[0].i > 0)
		port_sleep_ms(a[0].i);
	return interrupted(vm);
}

static int bi_uptime_ms(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_int(a, (int32_t)(port_uptime_us() / 1000));
}

static int bi_time(struct al_vm *vm, union al_val *a, int argc)
{
	return ret_int(a, (int32_t)port_time());
}

static int bi_date(struct al_vm *vm, union al_val *a, int argc)
{
	char buf[32];

	port_date(port_time(), buf, sizeof(buf));
	return ret_str(vm, a, al_str_new(vm, buf, strlen(buf)));
}

/* ------------------------------------------------------------ terminal */

static int bi_raw_mode(struct al_vm *vm, union al_val *a, int argc)
{
	al_flush(vm);
	port_tty_raw(a[0].i);
	vm->raw = a[0].i;
	return 0;
}

static int final_key(int c)
{
	switch (c) {
	case 'A': return AL_KEY_UP;
	case 'B': return AL_KEY_DOWN;
	case 'C': return AL_KEY_RIGHT;
	case 'D': return AL_KEY_LEFT;
	case 'H': return AL_KEY_HOME;
	case 'F': return AL_KEY_END;
	case 'P': return AL_KEY_F1;
	case 'Q': return AL_KEY_F2;
	case 'R': return AL_KEY_F3;
	case 'S': return AL_KEY_F4;
	}
	return AL_KEY_UNKNOWN;
}

static int tilde_key(int n)
{
	switch (n) {
	case 1: case 7: return AL_KEY_HOME;
	case 2: return AL_KEY_INSERT;
	case 3: return AL_KEY_DELETE;
	case 4: case 8: return AL_KEY_END;
	case 5: return AL_KEY_PGUP;
	case 6: return AL_KEY_PGDN;
	case 11: return AL_KEY_F1;
	case 12: return AL_KEY_F2;
	case 13: return AL_KEY_F3;
	case 14: return AL_KEY_F4;
	}
	return AL_KEY_UNKNOWN;
}

/* One key press: a byte, or AL_KEY_* for escape sequences and timeouts. */
int al_readkey(struct al_vm *vm, int timeout_ms)
{
	int c = port_readbyte(timeout_ms);

	if (c != 0x1b)
		return c;
	c = port_readbyte(40);
	if (c < 0)
		return AL_KEY_ESC;
	if (c == 'O')
		return final_key(port_readbyte(40));
	if (c != '[')
		return AL_KEY_UNKNOWN;
	int num = 0;
	for (int i = 0; i < 8; i++) {
		c = port_readbyte(40);
		if (c >= '0' && c <= '9')
			num = num * 10 + c - '0';
		else if (c != ';')
			break;
	}
	return c == '~' ? tilde_key(num) : final_key(c);
}

static int bi_readkey(struct al_vm *vm, union al_val *a, int argc)
{
	al_flush(vm);
	int k = al_readkey(vm, argc > 0 ? (a[0].i < 0 ? -1 : a[0].i) : -1);
	if (interrupted(vm))
		return -1;
	return ret_int(a, k);
}

static int bi_term_cols(struct al_vm *vm, union al_val *a, int argc)
{
	int cols, rows;

	port_tty_size(&cols, &rows);
	return ret_int(a, cols);
}

static int bi_term_rows(struct al_vm *vm, union al_val *a, int argc)
{
	int cols, rows;

	port_tty_size(&cols, &rows);
	return ret_int(a, rows);
}

const al_builtin_fn al_builtin_fns[B_COUNT] = {
#define X(cname, name, sig) bi_##cname,
	AL_BUILTINS(X)
#undef X
};
