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

#include "pico.h"
#include "port.h"

#define RBUF_SIZE	512

#define STR(v)		((struct pico_str *)(v).o)
#define ARR(v)		((struct pico_array *)(v).o)
#define FILEOBJ(v)	((struct pico_file *)(v).o)

static void rel(struct pico_vm *vm, union pico_val v)
{
	pico_decref(vm, v.o);
}

static int oom(struct pico_vm *vm)
{
	pico_error(vm, "out of memory");
	return -1;
}

static int ret_int(union pico_val *a, int32_t v)
{
	a[0].i = v;
	return 1;
}

static int ret_float(union pico_val *a, float v)
{
	a[0].f = v;
	return 1;
}

static int ret_str(struct pico_vm *vm, union pico_val *a, struct pico_str *s)
{
	if (!s)
		return oom(vm);
	a[0].o = &s->h;
	return 1;
}

static int ret_fmt(struct pico_vm *vm, union pico_val *a, struct pico_fmt *f)
{
	struct pico_str *s = f->oom ? NULL : pico_str_new(vm, f->buf ? f->buf : "", f->len);

	port_free(f->buf);
	return ret_str(vm, a, s);
}

/* A signal arrived while we were blocked: end like the VM does. */
static int interrupted(struct pico_vm *vm)
{
	if (!port_interrupted())
		return 0;
	pico_flush(vm);
	if (vm->raw) {
		port_tty_raw(false);
		vm->raw = false;
	}
	port_die_interrupted();
	vm->failed = true;
	vm->status = 130;
	return -1;
}

static const char *path(struct pico_vm *vm, struct pico_str *s)
{
	if (strlen(s->data) != s->len) {
		pico_error(vm, "file name contains a NUL byte");
		return NULL;
	}
	return s->data;
}

/* ------------------------------------------------------------ printing */

__attribute__((format(printf, 3, 4)))
static void fmt_printf(struct pico_vm *vm, struct pico_fmt *out, const char *spec, ...)
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
		pico_fmt_puts(vm, out, small, n);
	} else {
		char *big = port_alloc(n + 1);
		if (!big) {
			out->oom = true;
		} else {
			vsnprintf(big, n + 1, spec, copy);
			pico_fmt_puts(vm, out, big, n);
			port_free(big);
		}
	}
	va_end(copy);
}

/* Literal text of a format, with %% turned into %. */
static void copy_literal(struct pico_vm *vm, struct pico_fmt *out, const char *s, const char *end)
{
	while (s < end) {
		const char *pct = memchr(s, '%', end - s);
		if (!pct) {
			pico_fmt_puts(vm, out, s, end - s);
			return;
		}
		pico_fmt_puts(vm, out, s, pct + 1 - s);
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
static void release_desc(struct pico_vm *vm, union pico_val *a, int from, int to, struct pico_str *desc)
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
static bool str_args(struct pico_vm *vm, union pico_val *a, int argc, bool fmt)
{
	if ((fmt && (!a[0].o || a[0].o->type != OT_STR)) ||
	    !a[argc - 1].o || a[argc - 1].o->type != OT_STR) {
		pico_error(vm, "damaged executable (string expected)");
		return false;
	}
	return true;
}

/* printf-style formatting of a[1..argc-2]; a[0] is the format, a[argc-1] the descriptors. */
static bool format_args(struct pico_vm *vm, struct pico_fmt *out, union pico_val *a, int argc)
{
	struct pico_str *fmt = STR(a[0]), *desc = STR(a[argc - 1]);
	const char *f = fmt->data, *end = f + fmt->len, *lit = f;
	const char *d = desc->data, *dend = d + desc->len;
	int arg = 1, last = argc - 2;

	for (;;) {
		const char *spec = NULL;
		int conv = pico_fmt_next(&f, end, &spec);
		copy_literal(vm, out, lit, conv > 0 ? spec : end);
		if (conv < 0) {
			pico_error(vm, "bad conversion in format string \"%.40s\"", fmt->data);
			return false;
		}
		if (conv == 0)
			break;
		lit = f;
		const char *dnext = d ? desc_next(d, dend) : NULL;
		if (arg > last || !dnext) {
			pico_error(vm, "format string \"%.40s\" wants more arguments", fmt->data);
			return false;
		}
		char sp[24];
		size_t sl = f - spec - 1;
		if (sl > sizeof(sp) - 4)
			sl = sizeof(sp) - 4;
		memcpy(sp, spec, sl);
		union pico_val v = a[arg];
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
				pico_fmt_puts(vm, out, num, pico_fmt_int(num, v.i));
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
			struct pico_fmt tmp = { 0 };
			const char *sub = d;
			if (*d == 's' && v.o && v.o->type == OT_STR)
				pico_fmt_puts(vm, &tmp, STR(v)->data, STR(v)->len);
			else
				pico_fmt_value(vm, &tmp, v, &sub, 0, false);
			if (sl == 1 || !tmp.buf || strlen(tmp.buf) != tmp.len)
				pico_fmt_puts(vm, out, tmp.buf ? tmp.buf : "", tmp.len);
			else
				fmt_printf(vm, out, sp, tmp.buf);
			if (tmp.oom)
				out->oom = true;
			port_free(tmp.buf);
			break;
		}
		}
		if (!fits) {
			pico_error(vm, "%%%c does not fit argument %d of \"%.40s\"", conv, arg, fmt->data);
			return false;
		}
		arg++;
		d = dnext;
	}
	if (arg <= last) {
		pico_error(vm, "more arguments than %% conversions in \"%.40s\"", fmt->data);
		return false;
	}
	if (out->oom) {
		oom(vm);
		return false;
	}
	return true;
}

static int bi_printf(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_fmt out = { 0 };

	if (!str_args(vm, a, argc, true))
		return -1;
	if (!format_args(vm, &out, a, argc)) {
		port_free(out.buf);
		return -1;
	}
	pico_out(vm, 1, out.buf ? out.buf : "", out.len);
	port_free(out.buf);
	release_desc(vm, a, 1, argc - 1, STR(a[argc - 1]));
	rel(vm, a[0]);
	rel(vm, a[argc - 1]);
	return 0;
}

static int bi_format(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_fmt out = { 0 };

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

static int print_values(struct pico_vm *vm, union pico_val *a, int argc, bool newline)
{
	struct pico_fmt out = { 0 };

	if (!str_args(vm, a, argc, false))
		return -1;
	struct pico_str *desc = STR(a[argc - 1]);
	const char *d = desc->data;

	for (int i = 0; i < argc - 1 && d && *d; i++) {
		if (*d == 's' && a[i].o && a[i].o->type == OT_STR) {
			pico_fmt_puts(vm, &out, STR(a[i])->data, STR(a[i])->len);
			d++;
		} else {
			pico_fmt_value(vm, &out, a[i], &d, 0, false);
		}
	}
	if (newline)
		pico_fmt_puts(vm, &out, "\n", 1);
	if (out.oom) {
		port_free(out.buf);
		return oom(vm);
	}
	pico_out(vm, 1, out.buf ? out.buf : "", out.len);
	port_free(out.buf);
	release_desc(vm, a, 0, argc - 1, desc);
	rel(vm, a[argc - 1]);
	return 0;
}

static int bi_print(struct pico_vm *vm, union pico_val *a, int argc)
{
	return print_values(vm, a, argc, false);
}

static int bi_println(struct pico_vm *vm, union pico_val *a, int argc)
{
	return print_values(vm, a, argc, true);
}

/* ------------------------------------------------------------ strings */

static int bi_substr(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *s = STR(a[0]);
	int32_t start = a[1].i;
	int32_t count = argc > 2 ? a[2].i : INT32_MAX;

	if (start < 0 || (uint32_t)start > s->len) {
		pico_error(vm, "substr() start %d is outside the string (length %u)", (int)start, (unsigned)s->len);
		return -1;
	}
	if (count < 0) {
		pico_error(vm, "substr() count %d is negative", (int)count);
		return -1;
	}
	if ((uint32_t)count > s->len - start)
		count = s->len - start;
	struct pico_str *r = pico_str_new(vm, s->data + start, count);
	rel(vm, a[0]);
	return ret_str(vm, a, r);
}

/*
 * Where needle (n bytes) first is in s at or after from, or -1. Text
 * without the needle's first byte is skipped by memchr, which the C
 * library does a word at a time; its last byte is checked before calling
 * memcmp, so text full of the first byte does not cost a call a byte.
 */
static int32_t search(const struct pico_str *s, const char *needle, uint32_t n, uint32_t from)
{
	const char *p, *last;

	if (n == 0)
		return from <= s->len ? (int32_t)from : -1;
	if (n > s->len || from > s->len - n)
		return -1;
	last = s->data + s->len - n;
	for (p = s->data + from; p <= last; p++) {
		if (*p != needle[0] && !(p = memchr(p, needle[0], last - p + 1)))
			return -1;
		if (p[n - 1] == needle[n - 1] && !memcmp(p + 1, needle + 1, n - 1))
			return p - s->data;
	}
	return -1;
}

/* The compiler always picks @finds or @finda; this keeps the table aligned. */
static int bi_find(struct pico_vm *vm, union pico_val *a, int argc)
{
	pico_error(vm, "damaged executable (find)");
	return -1;
}

static int bi_finds(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *s = STR(a[0]), *sub = STR(a[1]);
	int32_t from = argc > 2 ? a[2].i : 0;

	if (from < 0)
		from = 0;
	int32_t r = (uint32_t)from > s->len ? -1 : search(s, sub->data, sub->len, from);
	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_int(a, r);
}

static int bi_finda(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_array *arr = ARR(a[0]);
	union pico_val v = a[1];
	int32_t r = -1;

	if (!arr) {
		pico_error(vm, "find() in a null array");
		return -1;
	}
	for (uint32_t i = 0; i < arr->len && r < 0; i++) {
		union pico_val x = arr->items[i];
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
			if (x.o && v.o && !pico_str_cmp(STR(x), STR(v)))
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

static bool push_str(struct pico_vm *vm, struct pico_array *arr, const char *s, size_t n)
{
	struct pico_str *p = pico_str_new(vm, s, n);

	if (!p)
		return false;
	if (!pico_array_push(vm, arr, (union pico_val){ .o = &p->h })) {
		pico_decref(vm, &p->h);
		return false;
	}
	return true;
}

static int bi_split(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *s = STR(a[0]), *sep = argc > 1 ? STR(a[1]) : NULL;
	struct pico_array *arr = pico_array_new(vm, K_STR, 0);
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
			pico_decref(vm, &arr->h);
		return oom(vm);
	}
	rel(vm, a[0]);
	if (argc > 1)
		rel(vm, a[1]);
	a[0].o = &arr->h;
	return 1;
}

/* The characters of a string, each its own str: a UTF-8 sequence stays whole. */
static int bi_chars(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *s = STR(a[0]);
	struct pico_array *arr = pico_array_new(vm, K_STR, 0);
	bool ok = arr != NULL;

	for (uint32_t i = 0; ok && i < s->len;) {
		uint32_t n = 1;

		while (i + n < s->len && (s->data[i + n] & 0xc0) == 0x80)
			n++;
		ok = push_str(vm, arr, s->data + i, n);
		i += n;
	}
	if (!ok) {
		if (arr)
			pico_decref(vm, &arr->h);
		return oom(vm);
	}
	rel(vm, a[0]);
	a[0].o = &arr->h;
	return 1;
}

static int bi_join(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_array *arr = ARR(a[0]);
	struct pico_str *sep = STR(a[1]);
	struct pico_fmt out = { 0 };

	if (!arr) {
		pico_error(vm, "join() of a null array");
		return -1;
	}
	for (uint32_t i = 0; i < arr->len; i++) {
		if (i)
			pico_fmt_puts(vm, &out, sep->data, sep->len);
		if (arr->items[i].o)
			pico_fmt_puts(vm, &out, STR(arr->items[i])->data, STR(arr->items[i])->len);
	}
	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_fmt(vm, a, &out);
}

static int bi_trim(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *s = STR(a[0]);
	uint32_t start = 0, end = s->len;

	while (start < end && is_space(s->data[start]))
		start++;
	while (end > start && is_space(s->data[end - 1]))
		end--;
	if (start == 0 && end == s->len)
		return 1;		/* unchanged: hand the same string back */
	struct pico_str *r = pico_str_new(vm, s->data + start, end - start);
	rel(vm, a[0]);
	return ret_str(vm, a, r);
}

static int change_case(struct pico_vm *vm, union pico_val *a, bool up)
{
	struct pico_str *s = STR(a[0]);
	struct pico_str *r = pico_str_new(vm, s->data, s->len);

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

static int bi_upper(struct pico_vm *vm, union pico_val *a, int argc)
{
	return change_case(vm, a, true);
}

static int bi_lower(struct pico_vm *vm, union pico_val *a, int argc)
{
	return change_case(vm, a, false);
}

static int bi_replace(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *s = STR(a[0]), *old = STR(a[1]), *new = STR(a[2]);
	struct pico_fmt out = { 0 };
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
		pico_fmt_puts(vm, &out, s->data + pos, at - pos);
		pico_fmt_puts(vm, &out, new->data, new->len);
		pos = at + old->len;
	}
	pico_fmt_puts(vm, &out, s->data + pos, s->len - pos);
	rel(vm, a[0]);
	rel(vm, a[1]);
	rel(vm, a[2]);
	return ret_fmt(vm, a, &out);
}

static int affix(struct pico_vm *vm, union pico_val *a, bool start)
{
	struct pico_str *s = STR(a[0]), *p = STR(a[1]);
	bool r = p->len <= s->len &&
		 !memcmp(start ? s->data : s->data + s->len - p->len, p->data, p->len);

	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_int(a, r);
}

static int bi_contains(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *s = STR(a[0]), *p = STR(a[1]);
	bool found = search(s, p->data, p->len, 0) >= 0;

	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_int(a, found);
}

static int bi_repeat(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *s = STR(a[0]);
	int32_t times = a[1].i;
	struct pico_str *r;
	char *out;

	if (times < 0)
		times = 0;
	if (times && s->len > (size_t)1 << 20 / (size_t)times) {
		pico_error(vm, "repeat() would make a string of %lu bytes",
			 (unsigned long)(s->len * (size_t)times));
		rel(vm, a[0]);
		return -1;
	}
	out = pico_alloc(vm, s->len * times + 1);
	if (!out) {
		rel(vm, a[0]);
		return -1;
	}
	for (int32_t i = 0; i < times; i++)
		memcpy(out + i * s->len, s->data, s->len);
	r = pico_str_new(vm, out, s->len * times);
	pico_free(vm, out, s->len * times + 1);
	rel(vm, a[0]);
	return ret_str(vm, a, r);
}

static int bi_starts_with(struct pico_vm *vm, union pico_val *a, int argc)
{
	return affix(vm, a, true);
}

static int bi_ends_with(struct pico_vm *vm, union pico_val *a, int argc)
{
	return affix(vm, a, false);
}

static int bi_chr(struct pico_vm *vm, union pico_val *a, int argc)
{
	char buf[4];

	if (a[0].i < 0 || a[0].i > 0x10ffff) {
		pico_error(vm, "chr(%d): not a Unicode code point", (int)a[0].i);
		return -1;
	}
	return ret_str(vm, a, pico_str_new(vm, buf, utf8_put(buf, a[0].i)));
}

static int bi_ord(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *s = STR(a[0]);
	const unsigned char *p = (const unsigned char *)s->data;
	int32_t cp;

	if (!s->len) {
		pico_error(vm, "ord() of an empty string");
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

static int bi_to_int(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *s = STR(a[0]);
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

static int bi_to_float(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *s = STR(a[0]);
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

static struct pico_array *array_arg(struct pico_vm *vm, union pico_val v, const char *fn)
{
	if (!v.o) {
		pico_error(vm, "%s() of a null array", fn);
		return NULL;
	}
	if (v.o->type != OT_ARRAY) {
		pico_error(vm, "damaged executable (%s() wants an array)", fn);
		return NULL;
	}
	return ARR(v);
}


static bool array_room(struct pico_vm *vm, struct pico_array *arr, uint32_t n)
{
	if (n <= arr->cap)
		return true;
	if (n > (1u << 26))
		return false;
	uint32_t cap = arr->cap * 2 > n ? arr->cap * 2 : n;
	union pico_val *items = pico_grow(vm, arr->items, (size_t)arr->cap * sizeof(*items), (size_t)cap * sizeof(*items));
	if (!items)
		return false;
	arr->items = items;
	arr->cap = cap;
	return true;
}

static int bi_insert(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_array *arr = array_arg(vm, a[0], "insert");
	int32_t i = a[1].i;

	if (!arr)
		return -1;
	if (i < 0 || (uint32_t)i > arr->len) {
		pico_error(vm, "insert() index %d out of range (length %u)", (int)i, (unsigned)arr->len);
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

static int bi_remove_at(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_array *arr = array_arg(vm, a[0], "remove_at");
	int32_t i = a[1].i;

	if (!arr)
		return -1;
	if (i < 0 || (uint32_t)i >= arr->len) {
		pico_error(vm, "remove_at() index %d out of range (length %u)", (int)i, (unsigned)arr->len);
		return -1;
	}
	union pico_val v = arr->items[i];
	memmove(arr->items + i, arr->items + i + 1, (arr->len - i - 1) * sizeof(*arr->items));
	arr->len--;
	rel(vm, a[0]);
	a[0] = v;
	return 1;
}

static int bi_resize(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_array *arr = array_arg(vm, a[0], "resize");
	int32_t n = a[1].i;

	if (!arr)
		return -1;
	if (n < 0) {
		pico_error(vm, "resize() to negative length %d", (int)n);
		return -1;
	}
	if ((uint32_t)n > arr->len && !array_room(vm, arr, n))
		return oom(vm);
	while (arr->len > (uint32_t)n) {
		union pico_val v = arr->items[--arr->len];
		if (KIND_IS_REF(arr->h.kind))
			rel(vm, v);
	}
	while (arr->len < (uint32_t)n) {
		union pico_val *v = &arr->items[arr->len++];
		memset(v, 0, sizeof(*v));
		if (arr->h.kind == K_STR) {
			v->o = &vm->empty->h;
			vm->empty->h.refs++;
		}
	}
	rel(vm, a[0]);
	return 0;
}

static int bi_slice(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_array *arr = array_arg(vm, a[0], "slice");
	int32_t start = a[1].i, end = a[2].i;

	if (!arr)
		return -1;
	if (start < 0 || (uint32_t)start > arr->len || end < start) {
		pico_error(vm, "slice(%d, %d) of an array of length %u", (int)start, (int)end, (unsigned)arr->len);
		return -1;
	}
	if ((uint32_t)end > arr->len)
		end = arr->len;
	struct pico_array *r = pico_array_new(vm, arr->h.kind, end - start);
	if (!r)
		return oom(vm);
	for (int32_t i = start; i < end; i++) {
		union pico_val v = arr->items[i];
		if (KIND_IS_REF(arr->h.kind))
			pico_incref(v.o);
		r->items[r->len++] = v;
	}
	rel(vm, a[0]);
	a[0].o = &r->h;
	return 1;
}

static int cmp_int(const void *x, const void *y)
{
	int32_t a = ((const union pico_val *)x)->i, b = ((const union pico_val *)y)->i;

	return a < b ? -1 : a > b;
}

static int cmp_float(const void *x, const void *y)
{
	float a = ((const union pico_val *)x)->f, b = ((const union pico_val *)y)->f;

	return a < b ? -1 : a > b;
}

static int cmp_str(const void *x, const void *y)
{
	const struct pico_str *a = STR(*(const union pico_val *)x), *b = STR(*(const union pico_val *)y);

	if (!a || !b)
		return !a - !b;
	return pico_str_cmp(a, b);
}

/* Turns an array back to front, in place. */
static int bi_reverse(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_array *arr = array_arg(vm, a[0], "reverse");

	if (!arr)
		return -1;
	for (size_t i = 0, k = arr->len ? arr->len - 1 : 0; i < k; i++, k--) {
		union pico_val tmp = arr->items[i];

		arr->items[i] = arr->items[k];
		arr->items[k] = tmp;
	}
	rel(vm, a[0]);
	return 0;
}

static int bi_sort(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_array *arr = array_arg(vm, a[0], "sort");

	if (!arr)
		return -1;
	if (arr->len > 1)
		qsort(arr->items, arr->len, sizeof(*arr->items),
		      arr->h.kind == K_FLOAT ? cmp_float : arr->h.kind == K_STR ? cmp_str : cmp_int);
	rel(vm, a[0]);
	return 0;
}

/* ------------------------------------------------------------ math */

static int bi_abs(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_int(a, a[0].i < 0 ? (int32_t)(0u - (uint32_t)a[0].i) : a[0].i);
}

static int bi_absf(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_float(a, fabsf(a[0].f));
}

static int bi_min(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_int(a, a[0].i < a[1].i ? a[0].i : a[1].i);
}

static int bi_max(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_int(a, a[0].i > a[1].i ? a[0].i : a[1].i);
}

static int bi_minf(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_float(a, a[0].f < a[1].f ? a[0].f : a[1].f);
}

static int bi_maxf(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_float(a, a[0].f > a[1].f ? a[0].f : a[1].f);
}

static int bi_sqrt(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_float(a, sqrtf(a[0].f));
}

static int bi_tan(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_float(a, tanf(a[0].f));
}

static int bi_ln(struct pico_vm *vm, union pico_val *a, int argc)
{
	if (a[0].f <= 0) {
		pico_error(vm, "ln() of %g", (double)a[0].f);
		return -1;
	}
	return ret_float(a, logf(a[0].f));
}

static int bi_exp(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_float(a, expf(a[0].f));
}

static int bi_hypot(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_float(a, hypotf(a[0].f, a[1].f));
}

/*
 * assert(cond) stops the program where the mistake is, rather than
 * letting it carry on with something impossible. A message is optional.
 */
static int bi_assert(struct pico_vm *vm, union pico_val *a, int argc)
{
	bool ok = a[0].i != 0;
	struct pico_str *msg = argc > 1 ? STR(a[1]) : NULL;

	if (!ok)
		pico_error(vm, "assertion failed%s%.*s", msg ? ": " : "",
			 msg ? (int)msg->len : 0, msg ? msg->data : "");
	if (argc > 1)
		rel(vm, a[1]);
	return ok ? 0 : -1;
}

static int bi_pow(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_float(a, powf(a[0].f, a[1].f));
}

static int bi_sin(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_float(a, sinf(a[0].f));
}

static int bi_cos(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_float(a, cosf(a[0].f));
}

static int bi_atan2(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_float(a, atan2f(a[0].f, a[1].f));
}

static int bi_floor(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_int(a, pico_float_to_int(floorf(a[0].f)));
}

static int bi_ceil(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_int(a, pico_float_to_int(ceilf(a[0].f)));
}

static int bi_round(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_int(a, pico_float_to_int(roundf(a[0].f)));
}

static int bi_random(struct pico_vm *vm, union pico_val *a, int argc)
{
	uint32_t x = vm->rng;

	if (a[0].i <= 0) {
		pico_error(vm, "random(%d): the limit must be positive", (int)a[0].i);
		return -1;
	}
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	vm->rng = x;
	return ret_int(a, (int32_t)(((uint64_t)x * (uint32_t)a[0].i) >> 32));
}

static int bi_seed(struct pico_vm *vm, union pico_val *a, int argc)
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

static int bi_open(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *name = STR(a[0]), *mode = STR(a[1]);
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
		pico_error(vm, "open() mode must be \"r\", \"w\" or \"a\", not \"%.10s\"", mode->data);
		return -1;
	}
	int fd = port_open(p, m);
	struct pico_file *f = NULL;
	if (fd >= 0) {
		f = pico_file_new(vm, fd, false);
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

static struct pico_file *file_arg(struct pico_vm *vm, union pico_val v, const char *fn)
{
	struct pico_file *f = FILEOBJ(v);

	if (f && f->h.type != OT_FILE)
		pico_error(vm, "damaged executable (%s() wants a File)", fn);
	else if (!f)
		pico_error(vm, "%s() on a null File (did open() fail?)", fn);
	else if (f->fd < 0)
		pico_error(vm, "%s() on a closed File", fn);
	else
		return f;
	return NULL;
}

static int bi_close(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_file *f = file_arg(vm, a[0], "close");

	if (!f)
		return -1;
	if (f->fd == 1)
		pico_flush(vm);
	if (!f->std) {
		port_close(f->fd);
		f->fd = -1;
	}
	rel(vm, a[0]);
	return 0;
}

/* Fill the read buffer; false at end of file. */
static bool fill(struct pico_vm *vm, struct pico_file *f)
{
	if (f->rpos < f->rlen)
		return true;
	if (f->eof)
		return false;
	if (!f->rbuf && !(f->rbuf = pico_alloc(vm, RBUF_SIZE)))
		return false;
	if (f->fd == 0)
		pico_flush(vm);
	long n = port_read(f->fd, f->rbuf, RBUF_SIZE);
	if (n <= 0) {
		f->eof = true;
		return false;
	}
	f->rpos = 0;
	f->rlen = n;
	return true;
}

/*
 * Add n bytes at p to *s, a string being made that nobody else holds yet:
 * the first piece makes it. False when out of memory, with *s as it was.
 */
static bool build(struct pico_vm *vm, struct pico_str **s, const char *p, size_t n)
{
	struct pico_str *r = *s ? pico_str_add(vm, *s, p, n, true) : pico_str_new(vm, p, n);

	if (!r)
		return false;
	*s = r;
	return true;
}

/* Return a string made by build(), or "" when it had nothing. */
static int ret_built(struct pico_vm *vm, union pico_val *a, struct pico_str *s)
{
	if (!s) {
		vm->empty->h.refs++;
		s = vm->empty;
	} else {
		s = pico_str_fit(vm, s);
	}
	a[0].o = &s->h;
	return 1;
}

static int bi_read(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_file *f = file_arg(vm, a[0], "read");
	int64_t want = argc > 1 ? a[1].i : INT64_MAX;
	struct pico_str *s = NULL;
	bool ok = true;

	if (!f)
		return -1;
	while (want > 0 && ok && fill(vm, f)) {
		uint32_t n = f->rlen - f->rpos;
		if (n > want)
			n = want;
		ok = build(vm, &s, f->rbuf + f->rpos, n);
		f->rpos += n;
		want -= n;
		if (interrupted(vm)) {
			pico_decref(vm, s ? &s->h : NULL);
			return -1;
		}
	}
	if (interrupted(vm)) {
		pico_decref(vm, s ? &s->h : NULL);
		return -1;
	}
	if (!ok) {
		pico_decref(vm, s ? &s->h : NULL);
		return oom(vm);
	}
	rel(vm, a[0]);
	return ret_built(vm, a, s);
}

/* A line that is whole in the read buffer, as most are, is copied once. */
static int bi_readline(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_file *f = file_arg(vm, a[0], "readline");
	struct pico_str *s = NULL;
	bool ok = true;

	if (!f)
		return -1;
	while (ok && fill(vm, f)) {
		char *start = f->rbuf + f->rpos;
		size_t avail = f->rlen - f->rpos;
		char *nl = memchr(start, '\n', avail);
		size_t n = nl ? (size_t)(nl - start) + 1 : avail;
		ok = build(vm, &s, start, n);
		f->rpos += n;
		if (nl)
			break;
	}
	if (interrupted(vm)) {
		pico_decref(vm, s ? &s->h : NULL);
		return -1;
	}
	if (!ok) {
		pico_decref(vm, s ? &s->h : NULL);
		return oom(vm);
	}
	rel(vm, a[0]);
	return ret_built(vm, a, s);
}

static int bi_write(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_file *f = file_arg(vm, a[0], "write");
	struct pico_str *s = STR(a[1]);

	if (!f)
		return -1;
	int r = pico_out(vm, f->fd, s->data, s->len);
	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_int(a, r == 0);
}

static int path_op(struct pico_vm *vm, union pico_val *a, int (*op)(const char *))
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

static int bi_exists(struct pico_vm *vm, union pico_val *a, int argc)
{
	return path_op(vm, a, exists);
}

static int bi_is_dir(struct pico_vm *vm, union pico_val *a, int argc)
{
	return path_op(vm, a, is_dir);
}

static int bi_remove(struct pico_vm *vm, union pico_val *a, int argc)
{
	return path_op(vm, a, port_remove);
}

static int bi_mkdir(struct pico_vm *vm, union pico_val *a, int argc)
{
	return path_op(vm, a, port_mkdir);
}

static int bi_rename(struct pico_vm *vm, union pico_val *a, int argc)
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
	struct pico_vm	*vm;
	struct pico_array	*arr;
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

static int bi_listdir(struct pico_vm *vm, union pico_val *a, int argc)
{
	const char *p = path(vm, STR(a[0]));
	struct listing l = { vm, NULL, false };

	if (!p)
		return -1;
	if (!(l.arr = pico_array_new(vm, K_STR, 0)))
		return oom(vm);
	port_listdir(p, list_one, &l);
	if (l.oom) {
		pico_decref(vm, &l.arr->h);
		return oom(vm);
	}
	if (l.arr->len > 1)
		qsort(l.arr->items, l.arr->len, sizeof(*l.arr->items), cmp_str);
	rel(vm, a[0]);
	a[0].o = &l.arr->h;
	return 1;
}

/* ------------------------------------------------------------ processes */

static int bi_exit(struct pico_vm *vm, union pico_val *a, int argc)
{
	vm->halted = true;
	vm->status = a[0].i;
	pico_flush(vm);
	return -1;
}

static int run_argv(struct pico_vm *vm, union pico_val *items, uint32_t n)
{
	const char **argv = port_alloc((n + 1) * sizeof(*argv));

	if (!argv)
		return oom(vm);
	for (uint32_t i = 0; i < n; i++) {
		if (!items[i].o || !(argv[i] = path(vm, STR(items[i])))) {
			if (!items[i].o)
				pico_error(vm, "run() argument is null");
			port_free(argv);
			return -1;
		}
	}
	argv[n] = NULL;
	pico_flush(vm);
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

static int bi_run(struct pico_vm *vm, union pico_val *a, int argc)
{
	int status = run_argv(vm, a, argc);

	if (vm->failed || interrupted(vm))
		return -1;
	for (int i = 0; i < argc; i++)
		rel(vm, a[i]);
	return ret_int(a, status);
}

static int bi_runa(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_array *arr = array_arg(vm, a[0], "run");

	if (!arr)
		return -1;
	if (!arr->len) {
		pico_error(vm, "run() needs a command");
		return -1;
	}
	int status = run_argv(vm, arr->items, arr->len);
	if (vm->failed || interrupted(vm))
		return -1;
	rel(vm, a[0]);
	return ret_int(a, status);
}

/*
 * What a command prints, as a str: output("date", "+%H:%M"). It runs as
 * run() does, its errors still to the screen; a command that cannot be
 * started gives "". At most a megabyte of it is kept.
 */
static int output_argv(struct pico_vm *vm, union pico_val *items, uint32_t n, union pico_val *res)
{
	const char **argv = port_alloc((n + 1) * sizeof(*argv));
	char *text = NULL;
	size_t len = 0;

	if (!argv)
		return oom(vm);
	for (uint32_t i = 0; i < n; i++) {
		if (!items[i].o || !(argv[i] = path(vm, STR(items[i])))) {
			if (!items[i].o)
				pico_error(vm, "output() argument is null");
			port_free(argv);
			return -1;
		}
	}
	argv[n] = NULL;
	pico_flush(vm);
	int status = n ? port_run_output(n, argv, &text, &len) : -1;
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
	struct pico_str *s = pico_str_new(vm, text ? text : "", text ? len : 0);
	port_free(text);
	if (!s)
		return oom(vm);
	res->o = &s->h;
	return 0;
}

static int bi_output(struct pico_vm *vm, union pico_val *a, int argc)
{
	union pico_val res;

	if (output_argv(vm, a, argc, &res))
		return -1;
	for (int i = 0; i < argc; i++)
		rel(vm, a[i]);
	a[0] = res;
	return 1;
}

static int bi_outputa(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_array *arr = array_arg(vm, a[0], "output");
	union pico_val res;

	if (!arr)
		return -1;
	if (!arr->len) {
		pico_error(vm, "output() needs a command");
		return -1;
	}
	if (output_argv(vm, arr->items, arr->len, &res))
		return -1;
	rel(vm, a[0]);
	a[0] = res;
	return 1;
}

/* A web page, or what an API answers: curl -sL URL. "" if it fails. */
static int bi_http_get(struct pico_vm *vm, union pico_val *a, int argc)
{
	union pico_val argv[3], res;
	struct pico_str *curl = pico_str_new(vm, "curl", 4), *flags = pico_str_new(vm, "-sL", 3);

	if (!curl || !flags) {
		if (curl)
			pico_decref(vm, &curl->h);
		if (flags)
			pico_decref(vm, &flags->h);
		return oom(vm);
	}
	argv[0].o = &curl->h;
	argv[1].o = &flags->h;
	argv[2] = a[0];
	int err = output_argv(vm, argv, 3, &res);
	pico_decref(vm, &curl->h);
	pico_decref(vm, &flags->h);
	if (err)
		return -1;
	rel(vm, a[0]);
	a[0] = res;
	return 1;
}

/* ------------------------------------------------------------ JSON */

static const char *json_ws(const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

/* Past the value at p: a string, a number or word, or a whole { } or [ ]. */
static const char *json_skip(const char *p, const char *e)
{
	int depth = 0;
	bool in_str = false;

	p = json_ws(p, e);
	if (p < e && *p != '{' && *p != '[' && *p != '"') {
		while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' &&
		       *p != '\t' && *p != '\r')
			p++;
		return p;
	}
	for (; p < e; p++) {
		if (in_str) {
			if (*p == '\\')
				p++;
			else if (*p == '"' && (in_str = false, depth == 0))
				return p + 1;
		} else if (*p == '"') {
			in_str = true;
		} else if (*p == '{' || *p == '[') {
			depth++;
		} else if ((*p == '}' || *p == ']') && --depth == 0) {
			return p + 1;
		}
	}
	return e;
}

/* In the object or array at p, the value for `key` (or at index `key`); NULL if none. */
static const char *json_member(const char *p, const char *e, const char *key, size_t klen,
			       long *count)
{
	p = json_ws(p, e);
	if (p >= e || (*p != '{' && *p != '['))
		return NULL;
	bool obj = *p == '{';
	char *end;
	long want = obj ? -1 : strtol(key, &end, 10), n = 0;

	if (!obj && !count && (end != key + klen || !klen || want < 0))
		return NULL;
	for (p = json_ws(p + 1, e); p < e && *p != '}' && *p != ']'; n++) {
		if (obj) {
			const char *k = p + 1, *v;

			if (*p != '"')
				return NULL;
			p = json_skip(p, e);
			v = json_ws(p, e);
			if (v >= e || *v != ':')
				return NULL;
			v = json_ws(v + 1, e);
			if (!count && (size_t)(p - 1 - k) == klen && !memcmp(k, key, klen))
				return v;
			p = json_skip(v, e);
		} else {
			if (!count && n == want)
				return p;
			p = json_skip(p, e);
		}
		p = json_ws(p, e);
		if (p < e && *p == ',')
			p = json_ws(p + 1, e);
	}
	if (count)
		*count = n;
	return NULL;
}

static void put_utf8(struct pico_vm *vm, struct pico_fmt *f, uint32_t cp)
{
	char b[4];
	int n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;

	if (n == 1) {
		b[0] = (char)cp;
	} else {
		for (int i = n - 1; i > 0; i--, cp >>= 6)
			b[i] = (char)(0x80 | (cp & 0x3f));
		b[0] = (char)((0xf0 << (4 - n)) | cp);
	}
	pico_fmt_puts(vm, f, b, n);
}

/*
 * json_get(text, "main.temp"): the value at a path of names and indexes
 * ("list.0.name"), as text: a string without its quotes and escapes, a
 * number or true, false or null as written, an object or array as JSON
 * (for json_get again). "#" as the last step is how many there are. ""
 * when there is no such thing.
 */
static int bi_json_get(struct pico_vm *vm, union pico_val *a, int argc)
{
	struct pico_str *js = STR(a[0]), *path = STR(a[1]);
	const char *p = js->data, *e = js->data + js->len, *q = path->data, *qe = q + path->len;
	struct pico_fmt out = { 0 };

	while (p && q < qe) {
		const char *dot = memchr(q, '.', qe - q);
		size_t klen = (dot ? dot : qe) - q;

		if (klen == 1 && *q == '#' && !dot) {
			long n = -1;

			json_member(p, e, q, klen, &n);
			char num[16];
			int len = snprintf(num, sizeof(num), "%ld", n < 0 ? 0 : n);
			pico_fmt_puts(vm, &out, num, len);
			p = NULL;
			break;
		}
		p = json_member(p, e, q, klen, NULL);
		q = dot ? dot + 1 : qe;
	}
	if (p) {
		p = json_ws(p, e);
		const char *end = json_skip(p, e);

		if (p < e && *p == '"') {
			for (p++; p < end - 1; p++) {
				if (*p != '\\') {
					pico_fmt_puts(vm, &out, p, 1);
					continue;
				}
				if (++p >= end - 1)
					break;
				switch (*p) {
				case 'n': pico_fmt_puts(vm, &out, "\n", 1); break;
				case 't': pico_fmt_puts(vm, &out, "\t", 1); break;
				case 'r': pico_fmt_puts(vm, &out, "\r", 1); break;
				case 'b': pico_fmt_puts(vm, &out, "\b", 1); break;
				case 'f': pico_fmt_puts(vm, &out, "\f", 1); break;
				case 'u': {
					char hex[5] = { 0 };
					uint32_t cp;

					if (end - 1 - p < 5)
						break;
					memcpy(hex, p + 1, 4);
					cp = (uint32_t)strtoul(hex, NULL, 16);
					p += 4;
					if (cp >= 0xd800 && cp < 0xdc00 && end - 1 - p >= 7 && p[1] == '\\' &&
					    p[2] == 'u') {
						memcpy(hex, p + 3, 4);
						uint32_t lo = (uint32_t)strtoul(hex, NULL, 16);

						if (lo >= 0xdc00 && lo < 0xe000) {
							cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
							p += 6;
						}
					}
					put_utf8(vm, &out, cp);
					break;
				}
				default: pico_fmt_puts(vm, &out, p, 1); break;	/* " \ / */
				}
			}
		} else {
			pico_fmt_puts(vm, &out, p, end - p);
		}
	}
	rel(vm, a[0]);
	rel(vm, a[1]);
	return ret_fmt(vm, a, &out);
}

static int bi_getenv(struct pico_vm *vm, union pico_val *a, int argc)
{
	const char *v = port_getenv(STR(a[0])->data);

	rel(vm, a[0]);
	return ret_str(vm, a, pico_str_new(vm, v ? v : "", v ? strlen(v) : 0));
}

/* ------------------------------------------------------------ time */

static int bi_sleep_ms(struct pico_vm *vm, union pico_val *a, int argc)
{
	pico_flush(vm);
	if (a[0].i > 0)
		port_sleep_ms(a[0].i);
	return interrupted(vm);
}

static int bi_uptime_ms(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_int(a, (int32_t)(port_uptime_us() / 1000));
}

static int bi_time(struct pico_vm *vm, union pico_val *a, int argc)
{
	return ret_int(a, (int32_t)port_time());
}

static int bi_date(struct pico_vm *vm, union pico_val *a, int argc)
{
	char buf[32];

	port_date(port_time(), buf, sizeof(buf));
	return ret_str(vm, a, pico_str_new(vm, buf, strlen(buf)));
}

/* ------------------------------------------------------------ terminal */

static int bi_raw_mode(struct pico_vm *vm, union pico_val *a, int argc)
{
	pico_flush(vm);
	port_tty_raw(a[0].i);
	vm->raw = a[0].i;
	return 0;
}

static int final_key(int c)
{
	switch (c) {
	case 'A': return PICO_KEY_UP;
	case 'B': return PICO_KEY_DOWN;
	case 'C': return PICO_KEY_RIGHT;
	case 'D': return PICO_KEY_LEFT;
	case 'H': return PICO_KEY_HOME;
	case 'F': return PICO_KEY_END;
	case 'P': return PICO_KEY_F1;
	case 'Q': return PICO_KEY_F2;
	case 'R': return PICO_KEY_F3;
	case 'S': return PICO_KEY_F4;
	}
	return PICO_KEY_UNKNOWN;
}

static int tilde_key(int n)
{
	switch (n) {
	case 1: case 7: return PICO_KEY_HOME;
	case 2: return PICO_KEY_INSERT;
	case 3: return PICO_KEY_DELETE;
	case 4: case 8: return PICO_KEY_END;
	case 5: return PICO_KEY_PGUP;
	case 6: return PICO_KEY_PGDN;
	case 11: return PICO_KEY_F1;
	case 12: return PICO_KEY_F2;
	case 13: return PICO_KEY_F3;
	case 14: return PICO_KEY_F4;
	}
	return PICO_KEY_UNKNOWN;
}

/* One key press: a byte, or PICO_KEY_* for escape sequences and timeouts. */
int pico_readkey(struct pico_vm *vm, int timeout_ms)
{
	int c = port_readbyte(timeout_ms);

	if (c != 0x1b)
		return c;
	c = port_readbyte(40);
	if (c < 0)
		return PICO_KEY_ESC;
	if (c == 'O')
		return final_key(port_readbyte(40));
	if (c != '[')
		return PICO_KEY_UNKNOWN;
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

static int bi_readkey(struct pico_vm *vm, union pico_val *a, int argc)
{
	pico_flush(vm);
	int k = pico_readkey(vm, argc > 0 ? (a[0].i < 0 ? -1 : a[0].i) : -1);
	if (interrupted(vm))
		return -1;
	return ret_int(a, k);
}

static int bi_term_cols(struct pico_vm *vm, union pico_val *a, int argc)
{
	int cols, rows;

	port_tty_size(&cols, &rows);
	return ret_int(a, cols);
}

static int bi_term_rows(struct pico_vm *vm, union pico_val *a, int argc)
{
	int cols, rows;

	port_tty_size(&cols, &rows);
	return ret_int(a, rows);
}

const pico_builtin_fn pico_builtin_fns[B_COUNT] = {
#define X(cname, name, sig) bi_##cname,
	PICO_BUILTINS(X)
#undef X
};
