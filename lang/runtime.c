/*
 * Runtime: memory, objects, strings, formatting, output and errors.
 */
#include <stdio.h>
#include <string.h>

#include "pico.h"
#include "port.h"

/* ------------------------------------------------------------ memory */

/*
 * Small blocks come from per-program pools: strings, arrays and structs are
 * mostly tiny and short-lived, and a free list is much cheaper than the
 * system allocator (on PocketType every pt_malloc also links the block into
 * the process's allocation list).
 */
#define POOL_MAX	192
#define CHUNK_SIZE	4096

static const uint8_t class_size[PICO_POOL_CLASSES] = { 16, 24, 32, 48, 64, 96, 128, 192 };

/*
 * The class of every size up to POOL_MAX by the 8-byte units it takes (the
 * sizes above are all multiples of 8), so that an allocation or a free
 * finds its class in one look rather than a search.
 */
static const uint8_t class_of[POOL_MAX / 8 + 1] = {
	0, 0, 0, 1, 2, 3, 3, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7,
};

/* The pool class for n bytes, or -1 for the system allocator. */
static int size_class(size_t n)
{
	return n <= POOL_MAX ? class_of[(n + 7) / 8] : -1;
}

struct chunk {
	struct chunk	*next;
};

void *pico_alloc(struct pico_vm *vm, size_t n)
{
	struct pico_heap *h = &vm->heap;
	void *p;

#ifdef PICO_NO_POOL
	p = port_alloc(n);
#else
	int cls = size_class(n);

	if (cls < 0) {
		p = port_alloc(n);
	} else if (h->free[cls]) {
		p = h->free[cls];
		h->free[cls] = *(void **)p;
	} else {
		size_t size = class_size[cls];
		if (!h->bump || (size_t)(h->bump_end - h->bump) < size) {
			struct chunk *c = port_alloc(CHUNK_SIZE);
			if (!c)
				return NULL;
			c->next = h->chunks;
			h->chunks = c;
			h->bump = (char *)c + ((sizeof(*c) + 15) & ~15u);
			h->bump_end = (char *)c + CHUNK_SIZE;
		}
		p = h->bump;
		h->bump += size;
	}
#endif
	if (p) {
		h->bytes += n;
		if (h->bytes > h->peak)
			h->peak = h->bytes;
	}
	return p;
}

void pico_free(struct pico_vm *vm, void *p, size_t n)
{
	if (!p)
		return;
	vm->heap.bytes -= n;
#ifdef PICO_NO_POOL
	port_free(p);
#else
	int cls = size_class(n);

	if (cls < 0) {
		port_free(p);
		return;
	}
	*(void **)p = vm->heap.free[cls];
	vm->heap.free[cls] = p;
#endif
}

void *pico_grow(struct pico_vm *vm, void *p, size_t old, size_t n)
{
	if (old > POOL_MAX && n > POOL_MAX) {
		void *q = port_realloc(p, n);
		if (q)
			vm->heap.bytes += n - old;
		return q;
	}
	void *q = pico_alloc(vm, n);
	if (!q)
		return NULL;
	if (p)
		memcpy(q, p, old < n ? old : n);
	pico_free(vm, p, old);
	return q;
}

void pico_heap_release(struct pico_vm *vm)
{
	struct chunk *c = vm->heap.chunks;

	while (c) {
		struct chunk *next = c->next;
		port_free(c);
		c = next;
	}
	memset(&vm->heap.free, 0, sizeof(vm->heap.free));
	vm->heap.chunks = NULL;
	vm->heap.bump = vm->heap.bump_end = NULL;
}

/* ------------------------------------------------------------ objects */

static size_t str_size(uint32_t cap)
{
	return sizeof(struct pico_str) + cap + 1;
}

struct pico_str *pico_str_alloc(struct pico_vm *vm, size_t len)
{
	if (len > 0x7ffffff0u)
		return NULL;
	size_t cap = len;
	int cls = size_class(str_size(len));

	/* keep the slack of the size class as room to append */
	if (cls >= 0)
		cap = class_size[cls] - sizeof(struct pico_str) - 1;
	struct pico_str *s = pico_alloc(vm, str_size(cap));
	if (!s)
		return NULL;
	s->h.refs = 1;
	s->h.type = OT_STR;
	s->h.kind = 0;
	s->h.sid = 0;
	s->len = len;
	s->cap = cap;
	s->data[len] = '\0';
	vm->heap.objects++;
	return s;
}

struct pico_str *pico_str_new(struct pico_vm *vm, const char *data, size_t len)
{
	struct pico_str *s = pico_str_alloc(vm, len);

	if (s && len)
		memcpy(s->data, data, len);
	return s;
}

struct pico_str *pico_str_concat(struct pico_vm *vm, struct pico_str *a, struct pico_str *b)
{
	if ((size_t)a->len + b->len > 0x7ffffff0u)
		return NULL;
	struct pico_str *s = pico_str_alloc(vm, (size_t)a->len + b->len);

	if (s) {
		memcpy(s->data, a->data, a->len);
		memcpy(s->data + a->len, b->data, b->len);
	}
	return s;
}

/*
 * Append n bytes at p, which is not inside a, to a, which nobody else
 * references. May move a. When a must grow, room doubles it, for a string
 * built a piece at a time; without room it gets just what it needs, as a
 * new string would.
 */
struct pico_str *pico_str_add(struct pico_vm *vm, struct pico_str *a, const char *p, size_t n, bool room)
{
	size_t need = (size_t)a->len + n;

	if (need > 0x7ffffff0u)
		return NULL;
	if (need > a->cap) {
		size_t cap = room && a->cap * 2 > need ? a->cap * 2 : need;
		if (cap > 0x7ffffff0u)
			cap = need;
		struct pico_str *s = pico_grow(vm, a, str_size(a->cap), str_size(cap));
		if (!s)
			return NULL;
		s->cap = cap;
		a = s;
	}
	memcpy(a->data + a->len, p, n);
	a->len = need;
	a->data[need] = '\0';
	return a;
}

/*
 * Give back the room a string built a piece at a time has left over, once
 * it is whole: it may be kept for a long time. If that fails, the string
 * keeps its room.
 */
struct pico_str *pico_str_fit(struct pico_vm *vm, struct pico_str *s)
{
	if (s->cap - s->len <= s->len / 8 || str_size(s->cap) <= POOL_MAX)
		return s;
	struct pico_str *r = pico_grow(vm, s, str_size(s->cap), str_size(s->len));
	if (!r)
		return s;
	r->cap = r->len;
	return r;
}

int pico_str_cmp(const struct pico_str *a, const struct pico_str *b)
{
	uint32_t n = a->len < b->len ? a->len : b->len;
	int r = memcmp(a->data, b->data, n);

	if (r)
		return r;
	return a->len < b->len ? -1 : a->len > b->len;
}

struct pico_array *pico_array_new(struct pico_vm *vm, int kind, uint32_t cap)
{
	struct pico_array *a = pico_alloc(vm, sizeof(*a));

	if (!a)
		return NULL;
	a->h.refs = 1;
	a->h.type = OT_ARRAY;
	a->h.kind = kind;
	a->h.sid = 0;
	a->len = 0;
	a->cap = 0;
	a->items = NULL;
	a->dead = NULL;
	vm->heap.objects++;
	if (cap) {
		a->items = pico_alloc(vm, (size_t)cap * sizeof(union pico_val));
		if (!a->items) {
			pico_obj_free(vm, &a->h);
			return NULL;
		}
		a->cap = cap;
	}
	return a;
}

bool pico_array_push(struct pico_vm *vm, struct pico_array *a, union pico_val v)
{
	if (a->len == a->cap) {
		uint32_t cap = a->cap < 4 ? 4 : a->cap * 2;
		if (cap > (1u << 26))
			return false;
		union pico_val *items = pico_grow(vm, a->items, (size_t)a->cap * sizeof(*items), (size_t)cap * sizeof(*items));
		if (!items)
			return false;
		a->items = items;
		a->cap = cap;
	}
	a->items[a->len++] = v;
	return true;
}

struct pico_struct *pico_struct_new(struct pico_vm *vm, uint16_t sid)
{
	const struct pico_sdef *def = &vm->prog.structs[sid];
	struct pico_struct *s = pico_alloc(vm, sizeof(*s) + def->nfields * sizeof(union pico_val));

	if (!s)
		return NULL;
	s->h.refs = 1;
	s->h.type = OT_STRUCT;
	s->h.kind = def->nfields;
	s->h.sid = sid;
	s->dead = NULL;
	for (int i = 0; i < def->nfields; i++) {
		if (vm->prog.fields[def->first + i].kind == K_STR) {
			s->fields[i].o = &vm->empty->h;
			vm->empty->h.refs++;
		} else {
			memset(&s->fields[i], 0, sizeof(s->fields[i]));
		}
	}
	vm->heap.objects++;
	return s;
}

struct pico_file *pico_file_new(struct pico_vm *vm, int fd, bool std)
{
	struct pico_file *f = pico_alloc(vm, sizeof(*f));

	if (!f)
		return NULL;
	memset(f, 0, sizeof(*f));
	f->h.refs = 1;
	f->h.type = OT_FILE;
	f->fd = fd;
	f->std = std;
	vm->heap.objects++;
	return f;
}

/* Drop one reference held by a dying container; queue what dies with it. */
static void drop(struct pico_vm *vm, struct pico_obj *o, struct pico_obj **pending)
{
	if (!o || --o->refs)
		return;
	if (o->type == OT_ARRAY) {
		((struct pico_array *)o)->dead = *pending;
		*pending = o;
	} else if (o->type == OT_STRUCT) {
		((struct pico_struct *)o)->dead = *pending;
		*pending = o;
	} else {
		pico_obj_free(vm, o);
	}
}

/* Free an object whose count reached zero. Iterative: a list of a million
 * nodes must not recurse a million deep. */
void pico_obj_free(struct pico_vm *vm, struct pico_obj *o)
{
	for (;;) {
		struct pico_obj *next = NULL;

		vm->heap.objects--;
		switch (o->type) {
		case OT_STR: {
			struct pico_str *s = (struct pico_str *)o;
			pico_free(vm, s, str_size(s->cap));
			break;
		}
		case OT_FILE: {
			struct pico_file *f = (struct pico_file *)o;
			if (f->fd == 1 || f->std)
				pico_flush(vm);
			if (f->fd >= 0 && !f->std)
				port_close(f->fd);
			pico_free(vm, f->rbuf, 512);
			pico_free(vm, f, sizeof(*f));
			break;
		}
		case OT_ARRAY: {
			struct pico_array *a = (struct pico_array *)o;
			next = a->dead;
			if (KIND_IS_REF(a->h.kind))
				for (uint32_t i = 0; i < a->len; i++)
					drop(vm, a->items[i].o, &next);
			pico_free(vm, a->items, (size_t)a->cap * sizeof(union pico_val));
			pico_free(vm, a, sizeof(*a));
			break;
		}
		case OT_STRUCT: {
			struct pico_struct *s = (struct pico_struct *)o;
			const struct pico_sdef *def = &vm->prog.structs[s->h.sid];
			next = s->dead;
			for (int i = 0; i < s->h.kind; i++)
				if (KIND_IS_REF(vm->prog.fields[def->first + i].kind))
					drop(vm, s->fields[i].o, &next);
			pico_free(vm, s, sizeof(*s) + s->h.kind * sizeof(union pico_val));
			break;
		}
		}
		/* containers queued by drop() were linked in front of `next` */
		if (!next)
			break;
		o = next;
	}
}

/* ------------------------------------------------------------ formatting */

void pico_fmt_puts(struct pico_vm *vm, struct pico_fmt *f, const char *s, size_t n)
{
	if (f->oom)
		return;
	if (f->len + n + 1 > f->cap) {
		size_t cap = f->cap ? f->cap : 64;
		while (cap < f->len + n + 1)
			cap *= 2;
		if (cap > 0x7ffffff0u) {
			f->oom = true;
			return;
		}
		char *b = port_realloc(f->buf, cap);
		if (!b) {
			f->oom = true;
			return;
		}
		f->buf = b;
		f->cap = cap;
	}
	memcpy(f->buf + f->len, s, n);
	f->len += n;
	f->buf[f->len] = '\0';
}

/* Out-of-range and not-a-number values saturate rather than trap. */
int32_t pico_float_to_int(float f)
{
	if (f != f)
		return 0;
	if (f >= 2147483648.0f)
		return INT32_MAX;
	if (f <= -2147483648.0f)
		return INT32_MIN;
	return (int32_t)f;
}

static void fmt_float(char *out, size_t n, float v)
{
	snprintf(out, n, "%.7g", v);
	if (!strpbrk(out, ".eEn"))		/* "3" -> "3.0"; inf and nan stay */
		strncat(out, ".0", n - strlen(out) - 1);
}

/* Skip one type descriptor; NULL when malformed. */
static const char *desc_skip(const char *d, const char *end)
{
	while (d < end && *d == '[')
		d++;
	if (d >= end)
		return NULL;
	if (*d == 'S')
		return end - d >= 5 ? d + 5 : NULL;
	return strchr("ifbsFn", *d) && *d ? d + 1 : NULL;
}

static int desc_struct(const char *d)
{
	int id = 0;

	for (int k = 1; k <= 4; k++)
		id = id << 4 | (d[k] >= 'a' ? d[k] - 'a' + 10 : d[k] - '0');
	return id;
}

/*
 * Append a printable form of v, described by *desc, and move *desc past
 * its descriptor. Nested strings are quoted when quote is set. Deep or
 * cyclic structures are cut off with "...".
 */
void pico_fmt_value(struct pico_vm *vm, struct pico_fmt *f, union pico_val v, const char **desc, int depth, bool quote)
{
	const char *d = *desc, *end = d + strlen(d);
	char num[48];

	*desc = desc_skip(d, end);
	if (!*desc) {
		*desc = end;
		pico_fmt_puts(vm, f, "?", 1);
		return;
	}
	if (*d == '[') {
		struct pico_array *a = (struct pico_array *)v.o;
		if (!a) {
			pico_fmt_puts(vm, f, "null", 4);
			return;
		}
		if (depth > 8 || a->h.type != OT_ARRAY) {
			pico_fmt_puts(vm, f, "[...]", 5);
			return;
		}
		pico_fmt_puts(vm, f, "[", 1);
		for (uint32_t i = 0; i < a->len && !f->oom; i++) {
			const char *sub = d + 1;
			if (i)
				pico_fmt_puts(vm, f, ", ", 2);
			pico_fmt_value(vm, f, a->items[i], &sub, depth + 1, true);
		}
		pico_fmt_puts(vm, f, "]", 1);
		return;
	}
	switch (*d) {
	case 'i':
		pico_fmt_puts(vm, f, num, pico_fmt_int(num, v.i));
		break;
	case 'b':
		pico_fmt_puts(vm, f, v.i ? "true" : "false", v.i ? 4 : 5);
		break;
	case 'f':
		fmt_float(num, sizeof(num), v.f);
		pico_fmt_puts(vm, f, num, strlen(num));
		break;
	case 's': {
		struct pico_str *s = (struct pico_str *)v.o;
		if (quote)
			pico_fmt_puts(vm, f, "\"", 1);
		if (s && s->h.type == OT_STR)
			pico_fmt_puts(vm, f, s->data, s->len);
		else if (s)
			pico_fmt_puts(vm, f, "?", 1);
		if (quote)
			pico_fmt_puts(vm, f, "\"", 1);
		break;
	}
	case 'F':
		if (v.o && v.o->type != OT_FILE)
			pico_fmt_puts(vm, f, "?", 1);
		else if (v.o)
			pico_fmt_puts(vm, f, num, snprintf(num, sizeof(num), "File(%d)", ((struct pico_file *)v.o)->fd));
		else
			pico_fmt_puts(vm, f, "null", 4);
		break;
	case 'S': {
		struct pico_struct *s = (struct pico_struct *)v.o;
		int sid = desc_struct(d);
		if (!s) {
			pico_fmt_puts(vm, f, "null", 4);
			break;
		}
		if (sid >= vm->prog.nstructs)
			break;
		const struct pico_sdef *def = &vm->prog.structs[sid];
		const struct pico_str *name = vm->prog.strings[def->name];
		pico_fmt_puts(vm, f, name->data, name->len);
		if (depth > 8 || s->h.type != OT_STRUCT) {
			pico_fmt_puts(vm, f, "{...}", 5);
			break;
		}
		pico_fmt_puts(vm, f, "{", 1);
		for (int i = 0; i < def->nfields && i < s->h.kind && !f->oom; i++) {
			const struct pico_field *fd = &vm->prog.fields[def->first + i];
			const struct pico_str *fname = vm->prog.strings[fd->name];
			const char *sub = vm->prog.strings[fd->desc]->data;
			if (i)
				pico_fmt_puts(vm, f, ", ", 2);
			pico_fmt_puts(vm, f, fname->data, fname->len);
			pico_fmt_puts(vm, f, ": ", 2);
			pico_fmt_value(vm, f, s->fields[i], &sub, depth + 1, true);
		}
		pico_fmt_puts(vm, f, "}", 1);
		break;
	}
	default:
		pico_fmt_puts(vm, f, "null", 4);
		break;
	}
}

/* An int in decimal. snprintf does the same through the C library's whole
 * printf machinery, which costs far more than the digits on the ESP32. */
int pico_fmt_int(char *out, int32_t v)
{
	char tmp[11];
	uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
	int n = 0, len = 0;

	do {
		tmp[n++] = '0' + u % 10;
		u /= 10;
	} while (u);
	if (v < 0)
		out[len++] = '-';
	while (n)
		out[len++] = tmp[--n];
	return len;
}

struct pico_str *pico_tostr(struct pico_vm *vm, union pico_val v, int kind)
{
	char num[48];

	switch (kind) {
	case TS_INT:
		return pico_str_new(vm, num, pico_fmt_int(num, v.i));
	case TS_FLOAT:
		fmt_float(num, sizeof(num), v.f);
		return pico_str_new(vm, num, strlen(num));
	default:
		return v.i ? pico_str_new(vm, "true", 4) : pico_str_new(vm, "false", 5);
	}
}

struct pico_str *pico_tostr_desc(struct pico_vm *vm, union pico_val v, const struct pico_str *desc)
{
	struct pico_fmt f = { 0 };
	const char *d = desc->data;

	pico_fmt_value(vm, &f, v, &d, 0, false);
	struct pico_str *s = f.oom ? NULL : pico_str_new(vm, f.buf ? f.buf : "", f.len);
	port_free(f.buf);
	return s;
}

/*
 * The next conversion in a printf format. Returns its letter, 0 at the end
 * or -1 when malformed; *spec points at its '%'.
 */
int pico_fmt_next(const char **f, const char *end, const char **spec)
{
	const char *p = *f;

	while (p < end) {
		if (*p != '%') {
			p++;
			continue;
		}
		*spec = p++;
		if (p < end && *p == '%') {
			p++;
			continue;
		}
		while (p < end && *p && strchr("-+ 0#", *p))
			p++;
		int digits = 0;
		while (p < end && *p >= '0' && *p <= '9' && ++digits <= 3)
			p++;
		if (digits > 3)
			return -1;
		if (p < end && *p == '.') {
			p++;
			digits = 0;
			while (p < end && *p >= '0' && *p <= '9' && ++digits <= 3)
				p++;
			if (digits > 3)
				return -1;
		}
		if (p >= end || !*p || !strchr("dixXoucsfeEgG", *p))
			return -1;
		*f = p + 1;
		return *p;
	}
	*f = p;
	return 0;
}

/* ------------------------------------------------------------ output */

static int write_all(int fd, const char *s, size_t n)
{
	while (n) {
		long r = port_write(fd, s, n);
		if (r <= 0)
			return r ? (int)r : -5;
		s += r;
		n -= r;
	}
	return 0;
}

void pico_flush(struct pico_vm *vm)
{
	if (vm->outlen) {
		write_all(1, vm->out, vm->outlen);
		vm->outlen = 0;
	}
}

int pico_out(struct pico_vm *vm, int fd, const char *s, size_t n)
{
	if (fd != 1) {
		pico_flush(vm);
		return write_all(fd, s, n);
	}
	if (vm->outlen + n > PICO_OUTBUF) {
		pico_flush(vm);
		if (n >= PICO_OUTBUF)
			return write_all(1, s, n);
	}
	memcpy(vm->out + vm->outlen, s, n);
	vm->outlen += n;
	if (vm->line_buffered && memchr(s, '\n', n))
		pico_flush(vm);
	return 0;
}

int pico_eprintf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return n;
	if (n >= (int)sizeof(buf))
		n = sizeof(buf) - 1;
	return write_all(2, buf, n);
}

/* ------------------------------------------------------------ errors */

void pico_error(struct pico_vm *vm, const char *fmt, ...)
{
	const struct pico_prog *p = &vm->prog;
	const char *file = p->strings[p->source]->data;
	char msg[300];
	va_list ap;

	if (vm->failed)
		return;
	vm->failed = true;
	pico_flush(vm);
	if (vm->raw) {
		port_tty_raw(false);
		vm->raw = false;
	}
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	pico_eprintf("%s:%d: runtime error: %s\n", file, pico_line_of(p, vm->fn, vm->ip), msg);

	if (vm->nframes < 2)
		return;			/* only main: the line above says it all */
	pico_eprintf("    in %s (%s:%d)\n", p->strings[p->funcs[vm->fn].name]->data, file,
		   pico_line_of(p, vm->fn, vm->ip));
	uint32_t shown = 0;
	for (uint32_t i = vm->nframes; i-- > 0;) {
		const struct pico_frame *fr = &vm->frames[i];
		if (!fr->ip)
			break;
		if (shown++ == 4) {
			pico_eprintf("    ... %u more\n", (unsigned)(i + 1));
			break;
		}
		pico_eprintf("    in %s (%s:%d)\n", p->strings[p->funcs[fr->fn].name]->data, file,
			   pico_line_of(p, fr->fn, fr->ip - 1));
	}
}
