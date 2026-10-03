/*
 * JSON for web APIs: see json.h. The reader only ever skips over values
 * to find the one a path names, so it needs no memory and accepts what a
 * server sends without judging it; the writer escapes what goes into a
 * string and nothing else.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "pt/sys.h"

/* ------------------------------------------------------------ writing */

void jb_add(struct jbuf *b, const char *s, size_t n)
{
	if (b->oom)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 256;
		char *p;

		while (cap < b->len + n + 1)
			cap *= 2;
		if (!(p = pt_realloc(b->p, cap))) {
			b->oom = true;
			return;
		}
		b->p = p;
		b->cap = cap;
	}
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = '\0';
}

void jb_puts(struct jbuf *b, const char *s)
{
	jb_add(b, s, strlen(s));
}

void jb_printf(struct jbuf *b, const char *fmt, ...)
{
	char small[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(small, sizeof(small), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n < sizeof(small)) {
		jb_add(b, small, n);
		return;
	}
	char *big = pt_malloc(n + 1);

	if (!big) {
		b->oom = true;
		return;
	}
	va_start(ap, fmt);
	vsnprintf(big, n + 1, fmt, ap);
	va_end(ap);
	jb_add(b, big, n);
	pt_free(big);
}

void jb_strn(struct jbuf *b, const char *s, size_t n)
{
	size_t run = 0;

	jb_add(b, "\"", 1);
	for (size_t i = 0; i < n; i++) {
		unsigned char c = s[i];
		char esc[8];

		if (c >= 0x20 && c != '"' && c != '\\') {
			run++;
			continue;
		}
		jb_add(b, s + i - run, run);	/* the plain stretch before it */
		run = 0;
		switch (c) {
		case '"':	jb_add(b, "\\\"", 2); break;
		case '\\':	jb_add(b, "\\\\", 2); break;
		case '\n':	jb_add(b, "\\n", 2); break;
		case '\r':	jb_add(b, "\\r", 2); break;
		case '\t':	jb_add(b, "\\t", 2); break;
		default:
			snprintf(esc, sizeof(esc), "\\u%04x", c);
			jb_add(b, esc, 6);
		}
	}
	jb_add(b, s + n - run, run);
	jb_add(b, "\"", 1);
}

void jb_str(struct jbuf *b, const char *s)
{
	jb_strn(b, s, strlen(s));
}

void jb_free(struct jbuf *b)
{
	pt_free(b->p);
	*b = (struct jbuf){ 0 };
}

/* ------------------------------------------------------------ reading */

static const char *ws(const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

/* Past the value at p: a string, a number, a word, or a whole object or array. */
static const char *skip(const char *p, const char *e)
{
	int depth = 0;

	p = ws(p, e);
	do {
		if (p >= e)
			return e;
		if (*p == '"') {
			for (p++; p < e && *p != '"'; p++)
				if (*p == '\\' && p + 1 < e)
					p++;
			if (p < e)
				p++;
		} else if (*p == '{' || *p == '[') {
			depth++;
			p++;
		} else if (*p == '}' || *p == ']') {
			depth--;
			p++;
		} else if (depth) {
			p++;			/* , : and what is between them */
		} else {
			while (p < e && !strchr(",}] \t\r\n", *p))
				p++;
		}
	} while (depth > 0);
	return p;
}

/* In the object or array at p: the value under `key` (or at index `key`). */
static const char *member(const char *p, const char *e, const char *key, size_t klen)
{
	bool array;
	long want;

	if (p >= e || (*p != '{' && *p != '['))
		return NULL;
	array = *p == '[';
	want = array ? strtol(key, NULL, 10) : 0;
	for (long n = 0;; n++) {
		const char *name = NULL, *nend = NULL;

		p = ws(p + 1, e);
		if (p >= e || *p == '}' || *p == ']')
			return NULL;
		if (!array) {
			if (*p != '"')
				return NULL;
			name = p + 1;
			nend = skip(p, e) - 1;
			p = ws(nend + 1, e);
			if (p >= e || *p != ':')
				return NULL;
			p = ws(p + 1, e);
		}
		if (array ? n == want : (size_t)(nend - name) == klen && !memcmp(name, key, klen))
			return p;
		p = ws(skip(p, e), e);
		if (p >= e || *p != ',')
			return NULL;
	}
}

const char *json_get(const char *p, const char *end, const char *path, const char **vend)
{
	p = ws(p, end);
	while (*path && p) {
		size_t klen = strcspn(path, ".");

		p = member(p, end, path, klen);
		path += klen + (path[klen] == '.');
	}
	if (!p || p >= end)
		return NULL;
	*vend = skip(p, end);
	return p;
}

/* Four hex digits at p: their value, or -1. */
static long hex4(const char *p, const char *e)
{
	char h[5];

	if (e - p < 4)
		return -1;
	memcpy(h, p, 4);
	h[4] = '\0';
	return strspn(h, "0123456789abcdefABCDEF") == 4 ? strtol(h, NULL, 16) : -1;
}

static size_t put_utf8(char *out, unsigned long cp)
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
		out[1] = 0x80 | (cp >> 6 & 0x3f);
		out[2] = 0x80 | (cp & 0x3f);
		return 3;
	}
	out[0] = 0xf0 | cp >> 18;
	out[1] = 0x80 | (cp >> 12 & 0x3f);
	out[2] = 0x80 | (cp >> 6 & 0x3f);
	out[3] = 0x80 | (cp & 0x3f);
	return 4;
}

size_t json_text(const char *v, const char *vend, char *out, size_t size)
{
	size_t n = 0;

	if (!size)
		return 0;
	if (v >= vend || *v != '"') {
		n = (size_t)(vend - v) < size - 1 ? (size_t)(vend - v) : size - 1;
		memcpy(out, v, n);
		out[n] = '\0';
		return n;
	}
	for (const char *p = v + 1; p < vend - 1 && n + 4 < size; p++) {
		char utf[4];
		size_t k;

		if (*p != '\\') {
			out[n++] = *p;
			continue;
		}
		switch (*++p) {
		case 'n':	out[n++] = '\n'; break;
		case 't':	out[n++] = '\t'; break;
		case 'r':	out[n++] = '\r'; break;
		case 'b':	out[n++] = '\b'; break;
		case 'f':	out[n++] = '\f'; break;
		case 'u': {
			long cp = hex4(p + 1, vend), lo;

			if (cp < 0)
				break;
			p += 4;
			/* a pair of surrogates is one character past the first 65536 */
			if (cp >= 0xd800 && cp < 0xdc00 && p + 2 < vend && p[1] == '\\' &&
			    p[2] == 'u' && (lo = hex4(p + 3, vend)) >= 0xdc00 && lo < 0xe000) {
				cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
				p += 6;
			}
			k = put_utf8(utf, (unsigned long)cp);
			memcpy(out + n, utf, k);
			n += k;
			break;
		}
		default:	out[n++] = *p;	/* \" \\ \/ */
		}
	}
	out[n] = '\0';
	return n;
}

char *json_dup(const char *v, const char *vend)
{
	size_t size = (size_t)(vend - v) + 4;
	char *out = pt_malloc(size);

	if (out)
		json_text(v, vend, out, size);
	return out;
}

int json_count(const char *v, const char *vend)
{
	int n = 0;

	if (v >= vend || *v != '[')
		return -1;
	for (const char *p = ws(v + 1, vend); p < vend && *p != ']'; n++) {
		p = ws(skip(p, vend), vend);
		if (p < vend && *p == ',')
			p = ws(p + 1, vend);
	}
	return n;
}

double json_num(const char *v, const char *vend, double dflt)
{
	char num[40];
	char *end;
	double d;

	if (v >= vend || (size_t)(vend - v) >= sizeof(num))
		return dflt;
	memcpy(num, v, vend - v);
	num[vend - v] = '\0';
	d = strtod(num, &end);
	return end == num ? dflt : d;
}

/* ------------------------------------------------------------ validation */

static const char *json_space(const char *p, const char *end)
{
	while (p < end && strchr(" \t\r\n", *p))
		p++;
	return p;
}

/* The API JSON reader is deliberately permissive. Snapshots and tool arguments
 * need a stricter check before that reader sees incomplete data. */
static const char *json_value_end(const char *p, const char *end, int depth)
{
	char close;
	bool object;

	p = json_space(p, end);
	if (p == end || depth > 32)
		return NULL;
	if (*p == '"') {
		for (p++; p < end; p++) {
			if (*p == '"')
				return p + 1;
			if ((unsigned char)*p < 32)
				return NULL;
			if (*p == '\\') {
				if (++p == end)
					return NULL;
				if (*p == 'u') {
					for (int i = 0; i < 4; i++)
						if (++p == end || !isxdigit((unsigned char)*p))
							return NULL;
				} else if (!strchr("\"\\/bfnrt", *p))
					return NULL;
			}
		}
		return NULL;
	}
	if (*p == '{' || *p == '[') {
		object = *p == '{';
		close = object ? '}' : ']';
		p = json_space(p + 1, end);
		if (p < end && *p == close)
			return p + 1;
		for (;;) {
			if (object) {
				if (p == end || *p != '"' || !(p = json_value_end(p, end, depth + 1)))
					return NULL;
				p = json_space(p, end);
				if (p == end || *p != ':')
					return NULL;
				p++;
			}
			if (!(p = json_value_end(p, end, depth + 1)))
				return NULL;
			p = json_space(p, end);
			if (p == end)
				return NULL;
			if (*p == close)
				return p + 1;
			if (*p != ',')
				return NULL;
			p = json_space(p + 1, end);
		}
	}
	if (end - p >= 4 && (!memcmp(p, "true", 4) || !memcmp(p, "null", 4)))
		return p + 4;
	if (end - p >= 5 && !memcmp(p, "false", 5))
		return p + 5;
	if (*p == '-')
		p++;
	if (p == end || !isdigit((unsigned char)*p))
		return NULL;
	if (*p == '0')
		p++;
	else
		while (p < end && isdigit((unsigned char)*p))
			p++;
	if (p < end && *p == '.') {
		if (++p == end || !isdigit((unsigned char)*p))
			return NULL;
		while (p < end && isdigit((unsigned char)*p))
			p++;
	}
	if (p < end && (*p == 'e' || *p == 'E')) {
		p++;
		if (p < end && (*p == '+' || *p == '-'))
			p++;
		if (p == end || !isdigit((unsigned char)*p))
			return NULL;
		while (p < end && isdigit((unsigned char)*p))
			p++;
	}
	return p;
}

bool json_valid(const char *p, size_t n)
{
	const char *end = p + n, *after = json_value_end(p, end, 0);

	return after && json_space(after, end) == end;
}
