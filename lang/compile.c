/*
 * Compiler: source to executable image.
 *
 * No syntax tree. Four passes over the tokens, each cheap:
 *
 *   1. struct names, so types can be used before their declaration
 *   2. struct fields, function signatures, globals
 *   3. function bodies, straight to bytecode
 *   4. global initialisers, as a hidden function run before main
 *
 * Expressions return their static type while they emit code, the way Lua's
 * and clox's single-pass compilers work. Memory is a handful of growable
 * arrays; C stack use is bounded by a nesting limit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "al.h"
#include "lex.h"
#include "port.h"

#define MAX_ERRORS	5
#define MAX_LOCALS	250
#define MAX_NEST	24
#define MAX_FIELDS	255
#define NO_POS		0xffffffffu

enum {
	TY_ERROR,
	TY_NONE,	/* no expectation */
	TY_VOID,
	TY_INT,
	TY_FLOAT,
	TY_BOOL,
	TY_STR,
	TY_FILE,
	TY_STRUCT,
	TY_NULL,
	TY_ASSIGNED,	/* an assignment statement, no value */
};

struct type {
	uint8_t		base;
	uint8_t		dims;	/* array nesting */
	uint16_t	sid;
};

struct buf {
	uint8_t		*p;
	uint32_t	 len;
	uint32_t	 cap;
};

struct cstruct {
	uint32_t	name;
	uint32_t	len;
	uint32_t	first;		/* into fields */
	uint16_t	nfields;
	uint16_t	sname;
};

struct cfield {
	uint32_t	name;
	uint32_t	len;
	struct type	type;
	uint16_t	sname;
	uint16_t	desc;
};

struct cfunc {
	uint32_t	name;
	uint32_t	len;
	struct type	ret;
	uint32_t	first;		/* into params */
	uint8_t		nparams;
	uint8_t		nlocals;
	uint16_t	sname;
	uint16_t	max_stack;
	uint32_t	code;
	uint32_t	code_len;
	uint32_t	lines;
	uint32_t	nlines;
};

struct cglobal {
	uint32_t	name;
	uint32_t	len;
	struct type	type;
	bool		is_const;
	bool		folded;
	union {
		int32_t		i;
		float		f;
		uint16_t	s;
	} value;
};

struct clocal {
	uint32_t	name;
	uint32_t	len;
	struct type	type;
	uint8_t		slot;
	bool		is_const;
	uint16_t	depth;
};

struct cstring {
	uint32_t	off;
	uint32_t	len;
};

struct loop {
	struct loop	*outer;
	uint16_t	 depth;
	uint32_t	 breaks;	/* first entry in c->breaks */
	uint32_t	 conts;
	bool		 has_break;
};

/* Code cut out of the stream to be emitted later (loop conditions, steps). */
struct capture {
	uint8_t		*code;
	uint32_t	 len;
	uint8_t		*lines;
	uint32_t	 nlines;
	int32_t		 last_op;
	int32_t		 label;
	int		 delta;
};

struct comp {
	const char	*file;
	const char	*src;
	uint32_t	 srclen;
	struct lexer	 lx;
	struct token	 prev, tok, peek;
	int		 errors;
	bool		 panic;
	bool		 stop;
	bool		 oom;
	int		 nest;
	int		 prefix_op;	/* ++x statement: the pending operator */

	struct cstruct	*structs;
	uint32_t	 nstructs, cap_structs;
	struct cfield	*fields;
	uint32_t	 nfields, cap_fields;
	struct cfunc	*funcs;
	uint32_t	 nfuncs, cap_funcs;
	struct type	*params;
	uint32_t	 nparams, cap_params;
	struct cglobal	*globals;
	uint32_t	 nglobals, cap_globals;
	struct cstring	*strings;
	uint32_t	 nstrings, cap_strings;
	struct buf	 strbuf;

	struct buf	 code;
	struct buf	 lines;		/* u16 pc, u16 line */
	struct buf	 scratch;
	uint32_t	*breaks;
	uint32_t	 nbreaks, cap_breaks;
	uint32_t	*conts;
	uint32_t	 nconts, cap_conts;
	uint32_t	*ends;		/* jumps out of an if/else if chain */
	uint32_t	 nends, cap_ends;

	/* the function being compiled */
	struct clocal	 locals[MAX_LOCALS];
	uint32_t	 nlocals;
	uint32_t	 max_locals;
	uint16_t	 depth;
	int		 sp;
	int		 max_sp;
	struct type	 ret;
	struct loop	*loop;
	uint32_t	 fn_start;
	uint32_t	 fn_lines;
	uint32_t	 last_line;
	uint32_t	 last_op;
	uint32_t	 label;
	uint16_t	 empty_str;
	uint16_t	 main;
	uint16_t	 init;

	char		 tn[4][80];
	int		 tn_next;
};

static const struct type T_ERROR = { TY_ERROR, 0, 0 };
static const struct type T_NONE = { TY_NONE, 0, 0 };
static const struct type T_VOID = { TY_VOID, 0, 0 };
static const struct type T_INT = { TY_INT, 0, 0 };
static const struct type T_FLOAT = { TY_FLOAT, 0, 0 };
static const struct type T_BOOL = { TY_BOOL, 0, 0 };
static const struct type T_STR = { TY_STR, 0, 0 };
static const struct type T_FILE = { TY_FILE, 0, 0 };
static const struct type T_NULL = { TY_NULL, 0, 0 };
static const struct type T_ASSIGNED = { TY_ASSIGNED, 0, 0 };
static const struct type T_STRS = { TY_STR, 1, 0 };

static struct type expr(struct comp *c, struct type want);
static bool statement(struct comp *c);

/* ------------------------------------------------------------ errors */

static void print_source_line(struct comp *c, const struct token *t)
{
	uint32_t start = t->pos, end = t->pos;

	if (t->pos > c->srclen)
		return;
	while (start > 0 && c->src[start - 1] != '\n')
		start--;
	while (end < c->srclen && c->src[end] != '\n' && c->src[end] != '\r')
		end++;
	if (end - start > 200)
		return;
	char line[512];
	uint32_t n = 0;
	for (uint32_t i = start; i < end; i++)
		line[n++] = c->src[i];
	line[n++] = '\n';
	for (uint32_t i = start; i < t->pos; i++)
		line[n++] = c->src[i] == '\t' ? '\t' : ' ';
	line[n++] = '^';
	line[n++] = '\n';
	port_write(2, line, n);
}

static void verror_at(struct comp *c, const struct token *t, const char *fmt, va_list ap)
{
	char msg[300];

	if (c->panic || c->stop)
		return;
	c->panic = true;
	vsnprintf(msg, sizeof(msg), fmt, ap);
	al_eprintf("%s:%u:%u: error: %s\n", c->file, (unsigned)t->line, (unsigned)t->col, msg);
	if (t->kind != TK_EOF)
		print_source_line(c, t);
	if (++c->errors >= MAX_ERRORS) {
		al_eprintf("%s: too many errors, stopping\n", c->file);
		c->stop = true;
		c->tok.kind = TK_EOF;
		c->peek.kind = TK_EOF;
	}
}

__attribute__((format(printf, 3, 4)))
static void error_at(struct comp *c, const struct token *t, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	verror_at(c, t, fmt, ap);
	va_end(ap);
}

static void out_of_memory(struct comp *c)
{
	if (!c->oom) {
		c->oom = true;
		c->panic = false;
		error_at(c, &c->tok, "out of memory");
		c->stop = true;
		c->tok.kind = TK_EOF;
		c->peek.kind = TK_EOF;
	}
}

/* ------------------------------------------------------------ memory */

static void *grow_array(struct comp *c, void *p, uint32_t *cap, uint32_t need, size_t size)
{
	if (need <= *cap)
		return p;
	uint32_t n = *cap ? *cap : 16;
	while (n < need)
		n *= 2;
	void *q = port_realloc(p, (size_t)n * size);
	if (!q) {
		out_of_memory(c);
		return p;
	}
	*cap = n;
	return q;
}

#define GROW(c, arr, cap, need) ((arr) = grow_array(c, arr, &(cap), need, sizeof(*(arr))), !(c)->oom)

static bool buf_add(struct comp *c, struct buf *b, const void *data, uint32_t n)
{
	if (c->oom)
		return false;
	if (b->len + n > b->cap) {
		b->p = grow_array(c, b->p, &b->cap, b->len + n, 1);
		if (c->oom)
			return false;
	}
	memcpy(b->p + b->len, data, n);
	b->len += n;
	return true;
}

static void buf_free(struct buf *b)
{
	port_free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
}

/* ------------------------------------------------------------ tokens */

static void advance(struct comp *c)
{
	c->prev = c->tok;
	c->tok = c->peek;
	if (c->stop) {
		c->tok.kind = TK_EOF;
		c->peek.kind = TK_EOF;
		return;
	}
	lex_next(&c->lx, &c->peek);
}

static bool check(struct comp *c, int kind)
{
	return c->tok.kind == kind;
}

static bool match(struct comp *c, int kind)
{
	if (c->tok.kind != kind)
		return false;
	advance(c);
	return true;
}

static bool expect(struct comp *c, int kind, const char *what)
{
	if (match(c, kind))
		return true;
	if (c->tok.kind == TK_EOF)
		error_at(c, &c->tok, "expected %s at end of file", what);
	else
		error_at(c, &c->tok, "expected %s, found %s", what, tok_name(c->tok.kind));
	return false;
}

static void start_pass(struct comp *c)
{
	lex_init(&c->lx, c->src, c->srclen);
	c->tok.kind = TK_EOF;
	lex_next(&c->lx, &c->peek);
	advance(c);
	c->panic = false;
}

static bool same_name(struct comp *c, uint32_t pos, uint32_t len, const struct token *t)
{
	return len == t->len && !memcmp(c->src + pos, c->src + t->pos, len);
}

static bool tok_is(struct comp *c, const struct token *t, const char *s)
{
	size_t n = strlen(s);

	return t->kind == TK_IDENT && t->len == n && !memcmp(c->src + t->pos, s, n);
}

static bool enter(struct comp *c)
{
	if (++c->nest > MAX_NEST) {
		error_at(c, &c->tok, "nested too deeply (the limit is %d levels)", MAX_NEST);
		c->stop = true;
		c->tok.kind = TK_EOF;
		c->peek.kind = TK_EOF;
		return false;
	}
	return true;
}

static void leave(struct comp *c)
{
	c->nest--;
}

/* ------------------------------------------------------------ types */

static bool is_ref(struct type t)
{
	return t.dims || t.base == TY_STR || t.base == TY_FILE || t.base == TY_STRUCT || t.base == TY_NULL;
}

/* May hold null: arrays, structs, File. */
static bool is_nullable(struct type t)
{
	return t.dims || t.base == TY_FILE || t.base == TY_STRUCT;
}

static bool is_scalar(struct type t, int base)
{
	return !t.dims && t.base == base;
}

static bool same_type(struct type a, struct type b)
{
	return a.base == b.base && a.dims == b.dims && (a.base != TY_STRUCT || a.sid == b.sid);
}

static struct type elem_type(struct type t)
{
	t.dims--;
	return t;
}

static struct type array_of(struct type t)
{
	t.dims++;
	return t;
}

static int kind_of(struct type t)
{
	if (t.dims)
		return K_REF;
	switch (t.base) {
	case TY_INT: return K_INT;
	case TY_FLOAT: return K_FLOAT;
	case TY_BOOL: return K_BOOL;
	case TY_STR: return K_STR;
	}
	return K_REF;
}

static const char *type_name(struct comp *c, struct type t)
{
	char *out = c->tn[c->tn_next++ % 4];
	const char *base;
	int n;

	switch (t.base) {
	case TY_VOID: base = "void"; break;
	case TY_INT: base = "int"; break;
	case TY_FLOAT: base = "float"; break;
	case TY_BOOL: base = "bool"; break;
	case TY_STR: base = "str"; break;
	case TY_FILE: base = "File"; break;
	case TY_NULL: base = "null"; break;
	case TY_STRUCT: base = NULL; break;
	default: base = "?"; break;
	}
	if (base) {
		n = snprintf(out, 80, "%s", base);
	} else {
		const struct cstruct *s = &c->structs[t.sid];
		n = snprintf(out, 80, "%.*s", (int)(s->len > 60 ? 60 : s->len), c->src + s->name);
	}
	for (int i = 0; i < t.dims && n < 76; i++)
		n += snprintf(out + n, 80 - n, "[]");
	return out;
}

static bool type_start(struct comp *c, const struct token *t);

static int find_struct(struct comp *c, const struct token *t)
{
	for (uint32_t i = 0; i < c->nstructs; i++)
		if (same_name(c, c->structs[i].name, c->structs[i].len, t))
			return i;
	return -1;
}

/* A type at the current token: int, Point[], str[][] ... */
static bool parse_type(struct comp *c, struct type *t)
{
	*t = T_ERROR;
	switch (c->tok.kind) {
	case TK_VOID: *t = T_VOID; break;
	case TK_INT: *t = T_INT; break;
	case TK_FLOAT: *t = T_FLOAT; break;
	case TK_BOOL: *t = T_BOOL; break;
	case TK_STR: *t = T_STR; break;
	case TK_FILE: *t = T_FILE; break;
	case TK_IDENT: {
		int sid = find_struct(c, &c->tok);
		if (sid < 0) {
			error_at(c, &c->tok, "unknown type '%.*s'", (int)c->tok.len, c->src + c->tok.pos);
			return false;
		}
		t->base = TY_STRUCT;
		t->sid = sid;
		break;
	}
	default:
		error_at(c, &c->tok, "expected a type, found %s", tok_name(c->tok.kind));
		return false;
	}
	advance(c);
	while (check(c, TK_LBRACKET) && c->peek.kind == TK_RBRACKET) {
		advance(c);
		advance(c);
		if (t->base == TY_VOID) {
			error_at(c, &c->prev, "there are no void arrays");
			return false;
		}
		if (t->dims == 100) {
			error_at(c, &c->prev, "too many array dimensions");
			return false;
		}
		t->dims++;
	}
	return true;
}

static bool type_start(struct comp *c, const struct token *t)
{
	switch (t->kind) {
	case TK_VOID:
	case TK_INT:
	case TK_FLOAT:
	case TK_BOOL:
	case TK_STR:
	case TK_FILE:
		return true;
	case TK_IDENT:
		return find_struct(c, t) >= 0;
	}
	return false;
}

/* ------------------------------------------------------------ strings */

static int intern(struct comp *c, const char *s, uint32_t len)
{
	for (uint32_t i = 0; i < c->nstrings; i++)
		if (c->strings[i].len == len && (!len || !memcmp(c->strbuf.p + c->strings[i].off, s, len)))
			return i;
	if (c->nstrings >= 0xffff) {
		error_at(c, &c->tok, "too many strings and names in one program");
		return 0;
	}
	if (!GROW(c, c->strings, c->cap_strings, c->nstrings + 1))
		return 0;
	uint32_t off = c->strbuf.len;
	if (len && !buf_add(c, &c->strbuf, s, len))
		return 0;
	c->strings[c->nstrings].off = off;
	c->strings[c->nstrings].len = len;
	return c->nstrings++;
}

static int intern_tok(struct comp *c, const struct token *t)
{
	return intern(c, c->src + t->pos, t->len);
}

/* Type descriptor, used by str() and printf to print any value. */
static void desc_add(struct comp *c, struct buf *b, struct type t)
{
	char tmp[8];

	for (int i = 0; i < t.dims; i++)
		buf_add(c, b, "[", 1);
	switch (t.base) {
	case TY_INT: buf_add(c, b, "i", 1); break;
	case TY_FLOAT: buf_add(c, b, "f", 1); break;
	case TY_BOOL: buf_add(c, b, "b", 1); break;
	case TY_STR: buf_add(c, b, "s", 1); break;
	case TY_FILE: buf_add(c, b, "F", 1); break;
	case TY_STRUCT:
		snprintf(tmp, sizeof(tmp), "S%04x", t.sid);
		buf_add(c, b, tmp, 5);
		break;
	default: buf_add(c, b, "n", 1); break;
	}
}

static int type_desc(struct comp *c, struct type t)
{
	c->scratch.len = 0;
	desc_add(c, &c->scratch, t);
	return c->oom ? 0 : intern(c, (char *)c->scratch.p, c->scratch.len);
}

/* ------------------------------------------------------------ emitting */

static uint32_t pc(struct comp *c)
{
	return c->code.len;
}

static void stack(struct comp *c, int delta)
{
	c->sp += delta;
	if (c->sp > c->max_sp)
		c->max_sp = c->sp;
}

static void mark_line(struct comp *c)
{
	uint32_t line = c->prev.line > 0xffff ? 0xffff : c->prev.line;
	uint32_t rel = pc(c) - c->fn_start;

	if (line == c->last_line)
		return;
	uint8_t e[4] = { rel, rel >> 8, line, line >> 8 };
	buf_add(c, &c->lines, e, 4);
	c->last_line = line;
}

static void emit_op(struct comp *c, int op)
{
	uint8_t b = op;

	mark_line(c);
	c->last_op = pc(c);
	buf_add(c, &c->code, &b, 1);
	if (al_opinfo[op].pop >= 0)
		stack(c, al_opinfo[op].push - al_opinfo[op].pop);
}

static void emit_u8(struct comp *c, uint8_t v)
{
	buf_add(c, &c->code, &v, 1);
}

static void emit_u16(struct comp *c, uint16_t v)
{
	uint8_t b[2] = { v, v >> 8 };

	buf_add(c, &c->code, b, 2);
}

static void emit_op_b(struct comp *c, int op, uint8_t v)
{
	emit_op(c, op);
	emit_u8(c, v);
}

static void emit_op_w(struct comp *c, int op, uint16_t v)
{
	emit_op(c, op);
	emit_u16(c, v);
}

static void emit_int(struct comp *c, int32_t v)
{
	if (v >= -128 && v <= 127) {
		emit_op_b(c, OP_CONST8, (uint8_t)v);
	} else {
		uint32_t u = v;
		uint8_t b[4] = { u, u >> 8, u >> 16, u >> 24 };
		emit_op(c, OP_CONST32);
		buf_add(c, &c->code, b, 4);
	}
}

static void emit_float(struct comp *c, float f)
{
	uint32_t u;

	memcpy(&u, &f, 4);
	uint8_t b[4] = { u, u >> 8, u >> 16, u >> 24 };
	emit_op(c, OP_CONSTF);
	buf_add(c, &c->code, b, 4);
}

static void emit_str(struct comp *c, const char *s, uint32_t len)
{
	emit_op_w(c, OP_CONSTS, intern(c, s, len));
}

static void emit_default(struct comp *c, struct type t)
{
	if (t.dims || t.base == TY_FILE || t.base == TY_STRUCT)
		emit_op(c, OP_NULLV);
	else if (t.base == TY_STR)
		emit_op_w(c, OP_CONSTS, c->empty_str);
	else if (t.base == TY_FLOAT)
		emit_float(c, 0);
	else
		emit_int(c, 0);
}

static uint32_t emit_jump(struct comp *c, int op)
{
	emit_op(c, op);
	uint32_t at = pc(c);
	emit_u16(c, 0);
	return at;
}

static void patch_jump(struct comp *c, uint32_t at)
{
	int32_t off = pc(c) - (at + 2);

	c->label = pc(c);
	if (c->oom)
		return;
	if (off > 32767) {
		error_at(c, &c->prev, "function too large (jump over 32 KB)");
		return;
	}
	c->code.p[at] = off;
	c->code.p[at + 1] = off >> 8;
}

static void emit_loop(struct comp *c, int op, uint32_t target)
{
	emit_op(c, op);
	int32_t off = (int32_t)target - (int32_t)(pc(c) + 2);
	if (off < -32768) {
		error_at(c, &c->prev, "loop body too large (over 32 KB)");
		off = 0;
	}
	emit_u16(c, (uint16_t)off);
}

/* The last instruction is a one-byte compare we may fuse with a jump. */
static int fusable(struct comp *c)
{
	if (c->oom || c->last_op == NO_POS || c->last_op < c->fn_start ||
	    c->last_op + 1 != pc(c) || c->label == pc(c))
		return -1;
	return c->code.p[c->last_op];
}

/* Pop a bool and jump when false. Returns the offset to patch. */
static uint32_t emit_jump_false(struct comp *c)
{
	int fused = -1;

	switch (fusable(c)) {
	case OP_EQ: fused = OP_JNE; break;
	case OP_NE: fused = OP_JEQ; break;
	case OP_LT: fused = OP_JGE; break;
	case OP_LE: fused = OP_JGT; break;
	case OP_GT: fused = OP_JLE; break;
	case OP_GE: fused = OP_JLT; break;
	case OP_NOT: fused = OP_JT; break;
	}
	if (fused < 0)
		return emit_jump(c, OP_JF);
	int op = c->code.p[c->last_op];
	c->code.len = c->last_op;
	stack(c, op == OP_NOT ? 0 : 1);
	return emit_jump(c, fused);
}

/* Pop a bool and jump back to target when true. */
static void emit_loop_true(struct comp *c, uint32_t target)
{
	int fused = -1;

	switch (fusable(c)) {
	case OP_EQ: fused = OP_LOOPEQ; break;
	case OP_NE: fused = OP_LOOPNE; break;
	case OP_LT: fused = OP_LOOPLT; break;
	case OP_LE: fused = OP_LOOPLE; break;
	case OP_GT: fused = OP_LOOPGT; break;
	case OP_GE: fused = OP_LOOPGE; break;
	}
	if (fused < 0) {
		emit_loop(c, OP_LOOPT, target);
		return;
	}
	c->code.len = c->last_op;
	stack(c, 1);
	emit_loop(c, fused, target);
}

static void capture_begin(struct comp *c, struct capture *cap)
{
	memset(cap, 0, sizeof(*cap));
	cap->len = pc(c);		/* start positions until capture_end */
	cap->nlines = c->lines.len;
	cap->delta = c->sp;
}

static void capture_end(struct comp *c, struct capture *cap)
{
	uint32_t mark = cap->len, lmark = cap->nlines;

	cap->len = pc(c) - mark;
	cap->nlines = (c->lines.len - lmark) / 4;
	cap->delta = c->sp - cap->delta;
	cap->last_op = c->last_op != NO_POS && c->last_op >= mark ? (int32_t)(c->last_op - mark) : -1;
	cap->label = c->label != NO_POS && c->label >= mark ? (int32_t)(c->label - mark) : -1;
	if (c->oom)
		return;
	if (cap->len) {
		cap->code = port_alloc(cap->len);
		cap->lines = port_alloc(cap->nlines * 4 + 1);
		if (!cap->code || !cap->lines) {
			out_of_memory(c);
			return;
		}
		memcpy(cap->code, c->code.p + mark, cap->len);
		memcpy(cap->lines, c->lines.p + lmark, cap->nlines * 4);
		for (uint32_t i = 0; i < cap->nlines; i++) {
			uint8_t *e = cap->lines + i * 4;
			uint32_t rel = al_u16(e) - (mark - c->fn_start);
			e[0] = rel;
			e[1] = rel >> 8;
		}
	}
	c->code.len = mark;
	c->lines.len = lmark;
	c->sp -= cap->delta;
	c->last_op = NO_POS;
	c->last_line = 0;
}

static void capture_emit(struct comp *c, struct capture *cap)
{
	uint32_t base = pc(c);

	if (cap->len && !c->oom) {
		buf_add(c, &c->code, cap->code, cap->len);
		for (uint32_t i = 0; i < cap->nlines; i++) {
			uint8_t *e = cap->lines + i * 4;
			uint32_t rel = al_u16(e) + (base - c->fn_start);
			uint8_t n[4] = { rel, rel >> 8, e[2], e[3] };
			buf_add(c, &c->lines, n, 4);
		}
	}
	stack(c, cap->delta);
	c->last_op = cap->last_op >= 0 ? base + cap->last_op : NO_POS;
	c->label = cap->label >= 0 ? base + cap->label : base;
	c->last_line = 0;
}

static void capture_free(struct capture *cap)
{
	port_free(cap->code);
	port_free(cap->lines);
	cap->code = NULL;
	cap->lines = NULL;
}

/* ------------------------------------------------------------ scopes */

static struct clocal *find_local(struct comp *c, const struct token *t)
{
	for (uint32_t i = c->nlocals; i-- > 0;)
		if (same_name(c, c->locals[i].name, c->locals[i].len, t))
			return &c->locals[i];
	return NULL;
}

static int find_global(struct comp *c, const struct token *t)
{
	for (uint32_t i = 0; i < c->nglobals; i++)
		if (same_name(c, c->globals[i].name, c->globals[i].len, t))
			return i;
	return -1;
}

static int find_func(struct comp *c, const struct token *t)
{
	for (uint32_t i = 0; i < c->nfuncs; i++)
		if (same_name(c, c->funcs[i].name, c->funcs[i].len, t))
			return i;
	return -1;
}

static int find_builtin(struct comp *c, const struct token *t)
{
	for (int i = 0; i < B_COUNT; i++) {
		const char *n = al_builtins[i].name;
		if (n[0] != '@' && strlen(n) == t->len && !memcmp(n, c->src + t->pos, t->len))
			return i;
	}
	return -1;
}

static int add_local(struct comp *c, const struct token *name, struct type t, bool is_const)
{
	for (uint32_t i = c->nlocals; i-- > 0 && c->locals[i].depth == c->depth;) {
		if (same_name(c, c->locals[i].name, c->locals[i].len, name)) {
			error_at(c, name, "'%.*s' is already declared here", (int)name->len, c->src + name->pos);
			break;
		}
	}
	if (find_struct(c, name) >= 0)
		error_at(c, name, "'%.*s' is a struct name", (int)name->len, c->src + name->pos);
	if (c->nlocals >= MAX_LOCALS) {
		error_at(c, name, "too many local variables (the limit is %d)", MAX_LOCALS);
		c->stop = true;
		c->tok.kind = TK_EOF;
		return 0;
	}
	struct clocal *l = &c->locals[c->nlocals];
	l->name = name->pos;
	l->len = name->len;
	l->type = t;
	l->slot = c->nlocals;
	l->is_const = is_const;
	l->depth = c->depth;
	c->nlocals++;
	if (c->nlocals > c->max_locals)
		c->max_locals = c->nlocals;
	return l->slot;
}

static void begin_scope(struct comp *c)
{
	c->depth++;
}

static void end_scope(struct comp *c)
{
	while (c->nlocals > 0 && c->locals[c->nlocals - 1].depth >= c->depth) {
		struct clocal *l = &c->locals[c->nlocals - 1];
		if (is_ref(l->type))
			emit_op_b(c, OP_CLEARR, l->slot);
		c->nlocals--;
	}
	c->depth--;
}

/* Release references held by locals deeper than depth, before a jump out. */
static void emit_clears(struct comp *c, uint16_t depth)
{
	for (uint32_t i = c->nlocals; i-- > 0 && c->locals[i].depth > depth;)
		if (is_ref(c->locals[i].type))
			emit_op_b(c, OP_CLEARR, c->locals[i].slot);
}

/* ------------------------------------------------------------ conversions */

/* Make the value on top of the stack a `to`, or report why it cannot be. */
static bool coerce(struct comp *c, struct type from, struct type to, const struct token *at)
{
	if (from.base == TY_ERROR || to.base == TY_ERROR || to.base == TY_NONE)
		return true;
	if (same_type(from, to))
		return true;
	if (is_scalar(from, TY_INT) && is_scalar(to, TY_FLOAT)) {
		emit_op(c, OP_I2F);
		return true;
	}
	if (from.base == TY_NULL && is_nullable(to))
		return true;
	if (from.base == TY_VOID)
		error_at(c, at, "this has no value (it is void)");
	else if (from.base == TY_NULL && to.base == TY_STR)
		error_at(c, at, "a str cannot be null; use \"\"");
	else if (is_scalar(from, TY_FLOAT) && is_scalar(to, TY_INT))
		error_at(c, at, "expected int, got float; convert with int(x)");
	else if (is_scalar(from, TY_STR) && !to.dims && (to.base == TY_INT || to.base == TY_FLOAT))
		error_at(c, at, "expected %s, got str; parse it with to_%s(s)", type_name(c, to), type_name(c, to));
	else if (is_scalar(to, TY_STR) && !from.dims && from.base >= TY_INT && from.base <= TY_BOOL)
		error_at(c, at, "expected str, got %s; convert with str(x)", type_name(c, from));
	else
		error_at(c, at, "expected %s, got %s", type_name(c, to), type_name(c, from));
	return false;
}

static void need_bool(struct comp *c, struct type t, const struct token *at, const char *what)
{
	if (t.base != TY_ERROR && !is_scalar(t, TY_BOOL))
		error_at(c, at, "%s must be bool, got %s", what, type_name(c, t));
}

static int tostr_code(struct type t)
{
	if (t.dims)
		return -1;
	switch (t.base) {
	case TY_INT: return TS_INT;
	case TY_FLOAT: return TS_FLOAT;
	case TY_BOOL: return TS_BOOL;
	}
	return -1;
}

static struct type binop(struct comp *c, int op, struct type l, struct type r, const struct token *at)
{
	if (l.base == TY_ERROR || r.base == TY_ERROR)
		return T_ERROR;
	bool li = is_scalar(l, TY_INT), lf = is_scalar(l, TY_FLOAT);
	bool ri = is_scalar(r, TY_INT), rf = is_scalar(r, TY_FLOAT);
	bool num = (li || lf) && (ri || rf);
	bool ls = is_scalar(l, TY_STR), rs = is_scalar(r, TY_STR);
	bool lb = is_scalar(l, TY_BOOL), rb = is_scalar(r, TY_BOOL);
	int iop = -1, fop = -1, sop = -1;

	switch (op) {
	case TK_PLUS:
		if (ls || rs) {
			if (!ls) {
				if (tostr_code(l) < 0)
					goto bad;
				emit_op_b(c, OP_TOSTR2, tostr_code(l));
			}
			if (!rs) {
				if (tostr_code(r) < 0)
					goto bad;
				emit_op_b(c, OP_TOSTR, tostr_code(r));
			}
			emit_op(c, OP_CONCAT);
			return T_STR;
		}
		iop = OP_ADD, fop = OP_ADDF;
		break;
	case TK_MINUS: iop = OP_SUB, fop = OP_SUBF; break;
	case TK_STAR: iop = OP_MUL, fop = OP_MULF; break;
	case TK_SLASH: iop = OP_DIV, fop = OP_DIVF; break;
	case TK_PERCENT: iop = OP_MOD, fop = OP_MODF; break;
	case TK_AMP:
	case TK_PIPE:
	case TK_CARET:
		iop = op == TK_AMP ? OP_BAND : op == TK_PIPE ? OP_BOR : OP_BXOR;
		if (lb && rb) {
			emit_op(c, iop);
			return T_BOOL;
		}
		if (!(li && ri))
			goto bad;
		emit_op(c, iop);
		return T_INT;
	case TK_SHL:
	case TK_SHR:
		if (!(li && ri))
			goto bad;
		emit_op(c, op == TK_SHL ? OP_SHL : OP_SHR);
		return T_INT;
	case TK_LT: iop = OP_LT, fop = OP_LTF, sop = OP_LTS; goto compare;
	case TK_LE: iop = OP_LE, fop = OP_LEF, sop = OP_LES; goto compare;
	case TK_GT: iop = OP_GT, fop = OP_GTF, sop = OP_GTS; goto compare;
	case TK_GE: iop = OP_GE, fop = OP_GEF, sop = OP_GES; goto compare;
	case TK_EQ:
	case TK_NE:
		iop = op == TK_EQ ? OP_EQ : OP_NE;
		fop = op == TK_EQ ? OP_EQF : OP_NEF;
		sop = op == TK_EQ ? OP_EQS : OP_NES;
		if (lb && rb) {
			emit_op(c, iop);
			return T_BOOL;
		}
		if ((is_nullable(l) || l.base == TY_NULL) && (is_nullable(r) || r.base == TY_NULL) &&
		    (same_type(l, r) || l.base == TY_NULL || r.base == TY_NULL)) {
			emit_op(c, op == TK_EQ ? OP_EQR : OP_NER);
			return T_BOOL;
		}
		goto compare;
	default:
		goto bad;
	}
	/* arithmetic */
	if (!num)
		goto bad;
	if (li && ri) {
		emit_op(c, iop);
		return T_INT;
	}
	if (li)
		emit_op(c, OP_I2F2);
	if (ri)
		emit_op(c, OP_I2F);
	emit_op(c, fop);
	return T_FLOAT;

compare:
	if (ls && rs) {
		emit_op(c, sop);
		return T_BOOL;
	}
	if (!num)
		goto bad;
	if (li && ri) {
		emit_op(c, iop);
		return T_BOOL;
	}
	if (li)
		emit_op(c, OP_I2F2);
	if (ri)
		emit_op(c, OP_I2F);
	emit_op(c, fop);
	return T_BOOL;

bad:
	error_at(c, at, "operator %s does not work on %s and %s", tok_name(op),
		 type_name(c, l), type_name(c, r));
	return T_ERROR;
}

/* ------------------------------------------------------------ expressions */

static int binop_prec(int kind)
{
	switch (kind) {
	case TK_OROR: return 1;
	case TK_ANDAND: return 2;
	case TK_PIPE: return 3;
	case TK_CARET: return 4;
	case TK_AMP: return 5;
	case TK_EQ:
	case TK_NE: return 6;
	case TK_LT:
	case TK_LE:
	case TK_GT:
	case TK_GE: return 7;
	case TK_SHL:
	case TK_SHR: return 8;
	case TK_PLUS:
	case TK_MINUS: return 9;
	case TK_STAR:
	case TK_SLASH:
	case TK_PERCENT: return 10;
	}
	return 0;
}

static bool is_assign_op(int kind)
{
	switch (kind) {
	case TK_ASSIGN:
	case TK_PLUSEQ:
	case TK_MINUSEQ:
	case TK_STAREQ:
	case TK_SLASHEQ:
	case TK_PERCENTEQ:
	case TK_AMPEQ:
	case TK_PIPEEQ:
	case TK_CARETEQ:
	case TK_SHLEQ:
	case TK_SHREQ:
	case TK_INC:
	case TK_DEC:
		return true;
	}
	return false;
}

static int compound_binop(int kind)
{
	switch (kind) {
	case TK_PLUSEQ: return TK_PLUS;
	case TK_MINUSEQ: return TK_MINUS;
	case TK_STAREQ: return TK_STAR;
	case TK_SLASHEQ: return TK_SLASH;
	case TK_PERCENTEQ: return TK_PERCENT;
	case TK_AMPEQ: return TK_AMP;
	case TK_PIPEEQ: return TK_PIPE;
	case TK_CARETEQ: return TK_CARET;
	case TK_SHLEQ: return TK_SHL;
	case TK_SHREQ: return TK_SHR;
	case TK_INC: return TK_PLUS;
	case TK_DEC: return TK_MINUS;
	}
	return TK_PLUS;
}

/*
 * In a statement, is an assignment operator next? `++x` statements set
 * prefix_op and assign at the end of the target instead.
 */
static int assign_here(struct comp *c, bool stmt)
{
	if (!stmt)
		return 0;
	if (c->prefix_op) {
		if (check(c, TK_LBRACKET) || check(c, TK_DOT))
			return 0;
		int op = c->prefix_op;
		c->prefix_op = 0;
		return op;
	}
	if (!is_assign_op(c->tok.kind))
		return 0;
	int op = c->tok.kind;
	advance(c);
	return op;
}

/*
 * The right-hand side of a compound assignment, with the target's current
 * value already on the stack. Leaves the new value.
 */
static void compound_rhs(struct comp *c, int op, struct type target, const struct token *at)
{
	struct type r;

	if (op == TK_INC || op == TK_DEC) {
		if (!is_scalar(target, TY_INT) && target.base != TY_ERROR)
			error_at(c, at, "%s needs an int, got %s", op == TK_INC ? "++" : "--", type_name(c, target));
		emit_int(c, 1);
		r = T_INT;
	} else {
		r = expr(c, T_NONE);
	}
	struct type v = binop(c, compound_binop(op), target, r, at);
	coerce(c, v, target, at);
}

static struct type assign_value(struct comp *c, struct type target)
{
	struct token at = c->tok;
	struct type v = expr(c, target);

	coerce(c, v, target, &at);
	return v;
}

static struct type assign_local(struct comp *c, struct clocal *l, int op, const struct token *at)
{
	bool ref = is_ref(l->type);

	if (l->is_const)
		error_at(c, at, "cannot assign to constant '%.*s'", (int)l->len, c->src + l->name);
	if (op == TK_ASSIGN) {
		assign_value(c, l->type);
		emit_op_b(c, ref ? OP_STORER : OP_STORE, l->slot);
		return T_ASSIGNED;
	}
	if (is_scalar(l->type, TY_INT)) {
		int32_t k = 0;
		bool small = false;
		if (op == TK_INC || op == TK_DEC) {
			k = op == TK_INC ? 1 : -1;
			small = true;
		} else if ((op == TK_PLUSEQ || op == TK_MINUSEQ) && check(c, TK_INTLIT) &&
			   (c->peek.kind == TK_SEMI || c->peek.kind == TK_RPAREN) &&
			   c->tok.v.i >= -127 && c->tok.v.i <= 127) {
			k = op == TK_PLUSEQ ? c->tok.v.i : -c->tok.v.i;
			small = true;
			advance(c);
		}
		if (small) {
			emit_op_b(c, OP_INCL, l->slot);
			emit_u8(c, (uint8_t)(int8_t)k);
			return T_ASSIGNED;
		}
	}
	if (is_scalar(l->type, TY_STR) && op == TK_PLUSEQ) {
		struct token rat = c->tok;
		struct type r = expr(c, T_NONE);
		if (!is_scalar(r, TY_STR) && r.base != TY_ERROR) {
			if (tostr_code(r) < 0)
				error_at(c, &rat, "cannot append %s to a str; convert with str(x)", type_name(c, r));
			else
				emit_op_b(c, OP_TOSTR, tostr_code(r));
		}
		emit_op_b(c, OP_CATL, l->slot);
		return T_ASSIGNED;
	}
	emit_op_b(c, ref ? OP_LOADR : OP_LOAD, l->slot);
	compound_rhs(c, op, l->type, at);
	emit_op_b(c, ref ? OP_STORER : OP_STORE, l->slot);
	return T_ASSIGNED;
}

static struct type assign_global(struct comp *c, int gi, int op, const struct token *at)
{
	struct cglobal *g = &c->globals[gi];
	bool ref = is_ref(g->type);

	if (g->is_const)
		error_at(c, at, "cannot assign to constant '%.*s'", (int)g->len, c->src + g->name);
	if (op != TK_ASSIGN) {
		emit_op_w(c, ref ? OP_GLOADR : OP_GLOAD, gi);
		compound_rhs(c, op, g->type, at);
	} else {
		assign_value(c, g->type);
	}
	emit_op_w(c, ref ? OP_GSTORER : OP_GSTORE, gi);
	return T_ASSIGNED;
}

static void emit_global(struct comp *c, int gi)
{
	struct cglobal *g = &c->globals[gi];

	if (!g->folded) {
		emit_op_w(c, is_ref(g->type) ? OP_GLOADR : OP_GLOAD, gi);
		return;
	}
	switch (g->type.base) {
	case TY_FLOAT: emit_float(c, g->value.f); break;
	case TY_STR: emit_op_w(c, OP_CONSTS, g->value.s); break;
	default: emit_int(c, g->value.i); break;
	}
}

static const struct {
	const char	*name;
	int32_t		 value;
} int_consts[] = {
	{ "INT_MAX", INT32_MAX }, { "INT_MIN", INT32_MIN },
	{ "KEY_NONE", AL_KEY_NONE }, { "KEY_EOF", AL_KEY_EOF },
	{ "KEY_UP", AL_KEY_UP }, { "KEY_DOWN", AL_KEY_DOWN },
	{ "KEY_LEFT", AL_KEY_LEFT }, { "KEY_RIGHT", AL_KEY_RIGHT },
	{ "KEY_HOME", AL_KEY_HOME }, { "KEY_END", AL_KEY_END },
	{ "KEY_PGUP", AL_KEY_PGUP }, { "KEY_PGDN", AL_KEY_PGDN },
	{ "KEY_INSERT", AL_KEY_INSERT }, { "KEY_DELETE", AL_KEY_DELETE },
	{ "KEY_ESC", AL_KEY_ESC }, { "KEY_F1", AL_KEY_F1 }, { "KEY_F2", AL_KEY_F2 },
	{ "KEY_F3", AL_KEY_F3 }, { "KEY_F4", AL_KEY_F4 },
};

/* Built-in constants: KEY_*, INT_MAX, PI, stdin... */
static bool builtin_constant(struct comp *c, const struct token *t, struct type *type)
{
	for (size_t i = 0; i < sizeof(int_consts) / sizeof(int_consts[0]); i++) {
		if (tok_is(c, t, int_consts[i].name)) {
			emit_int(c, int_consts[i].value);
			*type = T_INT;
			return true;
		}
	}
	if (tok_is(c, t, "PI")) {
		emit_float(c, 3.14159265f);
		*type = T_FLOAT;
		return true;
	}
	static const char *const files[] = { "stdin", "stdout", "stderr" };
	for (int i = 0; i < 3; i++) {
		if (tok_is(c, t, files[i])) {
			emit_op_b(c, OP_STDFILE, i);
			*type = T_FILE;
			return true;
		}
	}
	return false;
}

static struct type call(struct comp *c, const struct token *name);
static struct type unary(struct comp *c, struct type want);

/* [a, b, c] */
static struct type array_literal(struct comp *c, struct type want)
{
	struct token open = c->tok;
	struct type et = want.dims ? elem_type(want) : T_NONE;
	uint32_t count = 0;

	advance(c);
	emit_op(c, OP_NEWARR);
	uint32_t kind_at = pc(c);
	emit_u8(c, 0);
	emit_u16(c, 0);
	if (!check(c, TK_RBRACKET)) {
		do {
			if (check(c, TK_RBRACKET))
				break;		/* trailing comma */
			struct token at = c->tok;
			struct type v = expr(c, et);
			if (et.base == TY_NONE) {
				if (v.base == TY_NULL || v.base == TY_VOID) {
					error_at(c, &at, "cannot tell the element type; declare the array's type");
					v = T_ERROR;
				}
				et = v;
			} else {
				coerce(c, v, et, &at);
			}
			emit_op(c, OP_APPEND);
			count++;
		} while (match(c, TK_COMMA));
	}
	expect(c, TK_RBRACKET, "']'");
	if (et.base == TY_NONE) {
		error_at(c, &open, "cannot tell the type of []; declare it, as in int[] a = []");
		return T_ERROR;
	}
	if (!c->oom) {
		c->code.p[kind_at] = kind_of(et);
		c->code.p[kind_at + 1] = count > 0xffff ? 0xff : count;
		c->code.p[kind_at + 2] = count > 0xffff ? 0xff : count >> 8;
	}
	return et.base == TY_ERROR ? T_ERROR : array_of(et);
}

static int find_field(struct comp *c, uint16_t sid, const struct token *t)
{
	const struct cstruct *s = &c->structs[sid];

	for (int i = 0; i < s->nfields; i++) {
		const struct cfield *f = &c->fields[s->first + i];
		if (same_name(c, f->name, f->len, t))
			return i;
	}
	return -1;
}

/* Point{x: 1, y: 2} */
static struct type struct_literal(struct comp *c, int sid)
{
	uint8_t seen[32] = { 0 };

	emit_op_w(c, OP_NEWST, sid);
	expect(c, TK_LBRACE, "'{'");
	while (!check(c, TK_RBRACE) && !check(c, TK_EOF)) {
		struct token name = c->tok;
		if (!expect(c, TK_IDENT, "a field name"))
			break;
		int fi = find_field(c, sid, &name);
		if (fi < 0) {
			error_at(c, &name, "%s has no field '%.*s'", type_name(c, (struct type){ TY_STRUCT, 0, sid }),
				 (int)name.len, c->src + name.pos);
			break;
		}
		if (seen[fi / 8] & (1 << fi % 8))
			error_at(c, &name, "field '%.*s' given twice", (int)name.len, c->src + name.pos);
		seen[fi / 8] |= 1 << fi % 8;
		expect(c, TK_COLON, "':'");
		struct type ft = c->fields[c->structs[sid].first + fi].type;
		assign_value(c, ft);
		emit_op_b(c, is_ref(ft) ? OP_SETFIR : OP_SETFI, fi);
		if (!match(c, TK_COMMA))
			break;
	}
	expect(c, TK_RBRACE, "'}'");
	return (struct type){ TY_STRUCT, 0, sid };
}

/* int(x), float(x), str(x) */
static struct type conversion(struct comp *c)
{
	struct token kw = c->tok;

	advance(c);
	expect(c, TK_LPAREN, "'('");
	struct token at = c->tok;
	struct type t = expr(c, T_NONE);
	expect(c, TK_RPAREN, "')'");
	if (t.base == TY_ERROR)
		return kw.kind == TK_INT ? T_INT : kw.kind == TK_FLOAT ? T_FLOAT : T_STR;

	switch (kw.kind) {
	case TK_INT:
		if (is_scalar(t, TY_FLOAT))
			emit_op(c, OP_F2I);
		else if (is_scalar(t, TY_STR))
			error_at(c, &at, "int() does not parse strings; use to_int(s)");
		else if (!is_scalar(t, TY_INT) && !is_scalar(t, TY_BOOL))
			error_at(c, &at, "cannot convert %s to int", type_name(c, t));
		return T_INT;
	case TK_FLOAT:
		if (is_scalar(t, TY_INT))
			emit_op(c, OP_I2F);
		else if (is_scalar(t, TY_STR))
			error_at(c, &at, "float() does not parse strings; use to_float(s)");
		else if (!is_scalar(t, TY_FLOAT))
			error_at(c, &at, "cannot convert %s to float", type_name(c, t));
		return T_FLOAT;
	case TK_STR:
		if (tostr_code(t) >= 0)
			emit_op_b(c, OP_TOSTR, tostr_code(t));
		else if (t.base == TY_VOID)
			error_at(c, &at, "this has no value (it is void)");
		else if (!is_scalar(t, TY_STR))
			emit_op_w(c, OP_TOSTRX, type_desc(c, t));
		return T_STR;
	}
	error_at(c, &kw, "there is no bool() conversion; compare instead, as in x != 0");
	return T_BOOL;
}

/*
 * An operand with its [index] and .field suffixes. With stmt set it may be
 * the target of an assignment, which it compiles, returning TY_ASSIGNED.
 * A local array or struct followed by [ or . is not loaded: the indexed
 * instructions read the local directly and skip two refcount updates.
 */
static struct type operand(struct comp *c, struct type want, bool stmt)
{
	struct token start = c->tok;
	struct type t = T_ERROR;
	int borrowed = -1;
	int op;

	switch (start.kind) {
	case TK_INTLIT:
	case TK_CHARLIT:
		advance(c);
		emit_int(c, start.v.i);
		t = T_INT;
		break;
	case TK_FLOATLIT:
		advance(c);
		emit_float(c, start.v.f);
		t = T_FLOAT;
		break;
	case TK_TRUE:
	case TK_FALSE:
		advance(c);
		emit_int(c, start.kind == TK_TRUE);
		t = T_BOOL;
		break;
	case TK_NULL:
		advance(c);
		emit_op(c, OP_NULLV);
		t = T_NULL;
		break;
	case TK_STRLIT: {
		c->scratch.len = 0;
		while (check(c, TK_STRLIT)) {
			if (GROW(c, c->scratch.p, c->scratch.cap, c->scratch.len + c->tok.len))
				c->scratch.len += lex_decode(c->src, &c->tok, (char *)c->scratch.p + c->scratch.len);
			advance(c);
		}
		emit_str(c, (char *)c->scratch.p, c->scratch.len);
		t = T_STR;
		break;
	}
	case TK_LPAREN:
		advance(c);
		if (type_start(c, &c->tok) && c->peek.kind == TK_RPAREN) {
			error_at(c, &start, "there are no C casts; write %.*s(x) instead",
				 (int)c->tok.len, c->src + c->tok.pos);
			return T_ERROR;
		}
		t = expr(c, want);
		expect(c, TK_RPAREN, "')'");
		break;
	case TK_LBRACKET:
		t = array_literal(c, want);
		break;
	case TK_INT:
	case TK_FLOAT:
	case TK_STR:
	case TK_BOOL:
		if (c->peek.kind == TK_LPAREN) {
			t = conversion(c);
			break;
		}
		error_at(c, &start, "expected an expression, found %s", tok_name(start.kind));
		return T_ERROR;
	case TK_IDENT: {
		advance(c);
		if (check(c, TK_LPAREN)) {
			t = call(c, &start);
			break;
		}
		int sid = find_struct(c, &start);
		if (sid >= 0) {
			if (check(c, TK_LBRACE)) {
				t = struct_literal(c, sid);
				break;
			}
			error_at(c, &start, "'%.*s' is a type, not a value", (int)start.len, c->src + start.pos);
			return T_ERROR;
		}
		struct clocal *l = find_local(c, &start);
		if (l) {
			if ((op = assign_here(c, stmt)))
				return assign_local(c, l, op, &start);
			t = l->type;
			if (is_ref(t) && (t.dims || t.base == TY_STRUCT) &&
			    (check(c, TK_LBRACKET) || check(c, TK_DOT)))
				borrowed = l->slot;
			else
				emit_op_b(c, is_ref(t) ? OP_LOADR : OP_LOAD, l->slot);
			break;
		}
		int gi = find_global(c, &start);
		if (gi >= 0) {
			if ((op = assign_here(c, stmt)))
				return assign_global(c, gi, op, &start);
			emit_global(c, gi);
			t = c->globals[gi].type;
			break;
		}
		if (builtin_constant(c, &start, &t))
			break;
		if (find_func(c, &start) >= 0 || find_builtin(c, &start) >= 0)
			error_at(c, &start, "'%.*s' is a function; call it with ()", (int)start.len, c->src + start.pos);
		else
			error_at(c, &start, "unknown name '%.*s'", (int)start.len, c->src + start.pos);
		return T_ERROR;
	}
	case TK_EOF:
		error_at(c, &start, "expected an expression at end of file");
		return T_ERROR;
	default:
		error_at(c, &start, "expected an expression, found %s", tok_name(start.kind));
		return T_ERROR;
	}

	struct token mark, at;

	for (;;) {
		if (check(c, TK_LBRACKET)) {
			mark = c->tok;
			advance(c);
			if (is_scalar(t, TY_STR)) {
				at = c->tok;
				coerce(c, expr(c, T_INT), T_INT, &at);
				expect(c, TK_RBRACKET, "']'");
				if (stmt && is_assign_op(c->tok.kind)) {
					error_at(c, &c->tok, "strings cannot be changed; build a new one");
					return T_ERROR;
				}
				emit_op(c, OP_STRIDX);
				t = T_INT;
				continue;
			}
			if (!t.dims) {
				if (t.base != TY_ERROR)
					error_at(c, &mark, "cannot index %s", type_name(c, t));
				return T_ERROR;
			}
			struct type et = elem_type(t);
			bool ref = is_ref(et);
			at = c->tok;
			coerce(c, expr(c, T_INT), T_INT, &at);
			expect(c, TK_RBRACKET, "']'");
			if ((op = assign_here(c, stmt))) {
				if (op == TK_ASSIGN) {
					assign_value(c, et);
				} else if (borrowed >= 0) {
					emit_op(c, OP_DUP);
					emit_op_b(c, ref ? OP_IDXLR : OP_IDXL, borrowed);
					compound_rhs(c, op, et, &mark);
				} else {
					emit_op(c, OP_DUP2R);
					emit_op(c, ref ? OP_IDXR : OP_IDX);
					compound_rhs(c, op, et, &mark);
				}
				if (borrowed >= 0)
					emit_op_b(c, ref ? OP_SETIDXLR : OP_SETIDXL, borrowed);
				else
					emit_op(c, ref ? OP_SETIDXR : OP_SETIDX);
				return T_ASSIGNED;
			}
			if (borrowed >= 0)
				emit_op_b(c, ref ? OP_IDXLR : OP_IDXL, borrowed);
			else
				emit_op(c, ref ? OP_IDXR : OP_IDX);
			borrowed = -1;
			t = et;
		} else if (check(c, TK_DOT)) {
			at = c->tok;
			advance(c);
			mark = c->tok;
			if (!expect(c, TK_IDENT, "a field name"))
				return T_ERROR;
			if (t.dims || t.base != TY_STRUCT) {
				if (t.base != TY_ERROR)
					error_at(c, &at, "%s has no fields", type_name(c, t));
				return T_ERROR;
			}
			int fi = find_field(c, t.sid, &mark);
			if (fi < 0) {
				error_at(c, &mark, "%s has no field '%.*s'", type_name(c, t), (int)mark.len,
					 c->src + mark.pos);
				return T_ERROR;
			}
			struct type ft = c->fields[c->structs[t.sid].first + fi].type;
			bool ref = is_ref(ft);
			if ((op = assign_here(c, stmt))) {
				if (op == TK_ASSIGN) {
					assign_value(c, ft);
				} else if (borrowed >= 0) {
					emit_op_b(c, ref ? OP_GETFLR : OP_GETFL, borrowed);
					emit_u8(c, fi);
					compound_rhs(c, op, ft, &mark);
				} else {
					emit_op(c, OP_DUPR);
					emit_op_b(c, ref ? OP_GETFR : OP_GETF, fi);
					compound_rhs(c, op, ft, &mark);
				}
				if (borrowed >= 0) {
					emit_op_b(c, ref ? OP_SETFLR : OP_SETFL, borrowed);
					emit_u8(c, fi);
				} else {
					emit_op_b(c, ref ? OP_SETFR : OP_SETF, fi);
				}
				return T_ASSIGNED;
			}
			if (borrowed >= 0) {
				emit_op_b(c, ref ? OP_GETFLR : OP_GETFL, borrowed);
				emit_u8(c, fi);
			} else {
				emit_op_b(c, ref ? OP_GETFR : OP_GETF, fi);
			}
			borrowed = -1;
			t = ft;
		} else {
			break;
		}
	}
	if (stmt && (c->prefix_op || is_assign_op(c->tok.kind))) {
		error_at(c, &start, "cannot assign to this expression");
		c->prefix_op = 0;
		return T_ERROR;
	}
	return t;
}

static struct type unary(struct comp *c, struct type want)
{
	struct token op = c->tok;
	struct type t;

	if (!enter(c))
		return T_ERROR;
	switch (op.kind) {
	case TK_MINUS:
		advance(c);
		t = unary(c, T_NONE);
		if (is_scalar(t, TY_INT)) {
			/* fold -literal so -2147483648 works and costs nothing */
			if (!c->oom && c->last_op != NO_POS && c->label != pc(c) &&
			    c->code.p[c->last_op] == OP_CONST8 && c->last_op + 2 == pc(c) &&
			    (int8_t)c->code.p[c->last_op + 1] != -128) {
				c->code.p[c->last_op + 1] = (uint8_t)-(int8_t)c->code.p[c->last_op + 1];
			} else if (!c->oom && c->last_op != NO_POS && c->label != pc(c) &&
				   c->code.p[c->last_op] == OP_CONST32 && c->last_op + 5 == pc(c)) {
				uint32_t v = -al_u32(c->code.p + c->last_op + 1);
				for (int k = 0; k < 4; k++)
					c->code.p[c->last_op + 1 + k] = v >> (8 * k);
			} else {
				emit_op(c, OP_NEG);
			}
		} else if (is_scalar(t, TY_FLOAT)) {
			emit_op(c, OP_NEGF);
		} else if (t.base != TY_ERROR) {
			error_at(c, &op, "unary - needs a number, got %s", type_name(c, t));
			t = T_ERROR;
		}
		break;
	case TK_BANG:
		advance(c);
		t = unary(c, T_NONE);
		need_bool(c, t, &op, "the operand of !");
		emit_op(c, OP_NOT);
		t = T_BOOL;
		break;
	case TK_TILDE:
		advance(c);
		t = unary(c, T_NONE);
		if (!is_scalar(t, TY_INT) && t.base != TY_ERROR)
			error_at(c, &op, "~ needs an int, got %s", type_name(c, t));
		emit_op(c, OP_BNOT);
		t = T_INT;
		break;
	case TK_INC:
	case TK_DEC:
		error_at(c, &op, "%s is a statement here, not part of an expression", tok_name(op.kind));
		t = T_ERROR;
		break;
	default:
		t = operand(c, want, false);
		break;
	}
	leave(c);
	return t;
}

static struct type binary_rest(struct comp *c, struct type t, int min_prec)
{
	for (;;) {
		int prec = binop_prec(c->tok.kind);
		if (!prec || prec < min_prec)
			return t;
		struct token op = c->tok;
		advance(c);
		if (op.kind == TK_ANDAND || op.kind == TK_OROR) {
			need_bool(c, t, &op, op.kind == TK_ANDAND ? "the left side of &&" : "the left side of ||");
			uint32_t j = emit_jump(c, op.kind == TK_ANDAND ? OP_JFK : OP_JTK);
			struct type r = unary(c, T_NONE);
			if (!enter(c))
				return T_ERROR;
			r = binary_rest(c, r, prec + 1);
			leave(c);
			need_bool(c, r, &op, op.kind == TK_ANDAND ? "the right side of &&" : "the right side of ||");
			patch_jump(c, j);
			t = T_BOOL;
			continue;
		}
		struct type r = unary(c, T_NONE);
		if (!enter(c))
			return T_ERROR;
		r = binary_rest(c, r, prec + 1);
		leave(c);
		t = binop(c, op.kind, t, r, &op);
	}
}

static struct type ternary_rest(struct comp *c, struct type t, struct type want)
{
	if (!check(c, TK_QUESTION))
		return t;
	struct token q = c->tok;
	advance(c);
	need_bool(c, t, &q, "the condition of ?:");
	uint32_t jf = emit_jump_false(c);
	int sp = c->sp;
	struct type a = expr(c, want);
	expect(c, TK_COLON, "':'");
	uint32_t jend = emit_jump(c, OP_JMP);
	patch_jump(c, jf);
	c->sp = sp;
	struct token at_b = c->tok;
	struct type b = expr(c, want.base != TY_NONE ? want : a.base == TY_NULL ? T_NONE : a);

	if (a.base == TY_ERROR || b.base == TY_ERROR) {
		patch_jump(c, jend);
		return T_ERROR;
	}
	if (same_type(a, b)) {
		patch_jump(c, jend);
		return a;
	}
	if (is_scalar(a, TY_INT) && is_scalar(b, TY_FLOAT)) {
		uint32_t jend2 = emit_jump(c, OP_JMP);
		patch_jump(c, jend);
		emit_op(c, OP_I2F);
		patch_jump(c, jend2);
		return T_FLOAT;
	}
	if (is_scalar(a, TY_FLOAT) && is_scalar(b, TY_INT)) {
		emit_op(c, OP_I2F);
		patch_jump(c, jend);
		return T_FLOAT;
	}
	patch_jump(c, jend);
	if (a.base == TY_NULL && is_nullable(b))
		return b;
	if (b.base == TY_NULL && is_nullable(a))
		return a;
	error_at(c, &at_b, "the two sides of ?: differ: %s and %s", type_name(c, a), type_name(c, b));
	return T_ERROR;
}

static struct type expr(struct comp *c, struct type want)
{
	struct type t = unary(c, want);

	t = binary_rest(c, t, 1);
	return ternary_rest(c, t, want);
}

/* ------------------------------------------------------------ calls */

static struct type sig_type(char ch)
{
	switch (ch) {
	case 'i': return T_INT;
	case 'f': return T_FLOAT;
	case 'b': return T_BOOL;
	case 's': return T_STR;
	case 'F': return T_FILE;
	case 'S': return T_STRS;
	}
	return T_VOID;
}

static void emit_callb(struct comp *c, int id, int argc, bool value)
{
	emit_op_b(c, OP_CALLB, id);
	emit_u8(c, argc);
	stack(c, -argc + value);
}

/* ',' between arguments, or complain that there are too few. */
static bool next_arg(struct comp *c, int n, const struct token *name)
{
	if (n == 0) {
		if (check(c, TK_RPAREN)) {
			error_at(c, &c->tok, "too few arguments to %.*s()", (int)name->len, c->src + name->pos);
			return false;
		}
		return true;
	}
	if (check(c, TK_RPAREN)) {
		error_at(c, &c->tok, "too few arguments to %.*s()", (int)name->len, c->src + name->pos);
		return false;
	}
	return expect(c, TK_COMMA, "','");
}

static void end_args(struct comp *c, const struct token *name)
{
	if (check(c, TK_COMMA))
		error_at(c, &c->tok, "too many arguments to %.*s()", (int)name->len, c->src + name->pos);
	expect(c, TK_RPAREN, "')'");
}

static struct type builtin_sig(struct comp *c, int id, const struct token *name)
{
	const char *p = al_builtins[id].sig;
	int argc = 0;

	expect(c, TK_LPAREN, "'('");
	while (*p && *p != ':') {
		struct type want = sig_type(*p++);
		bool optional = *p == '?';
		if (optional)
			p++;
		if (check(c, TK_RPAREN)) {
			if (!optional)
				error_at(c, &c->tok, "too few arguments to %s()", al_builtins[id].name);
			while (*p && *p != ':')
				p++;
			break;
		}
		if (argc > 0 && !expect(c, TK_COMMA, "','"))
			break;
		assign_value(c, want);
		argc++;
	}
	end_args(c, name);
	struct type ret = sig_type(*p == ':' ? p[1] : 'v');
	emit_callb(c, id, argc, ret.base != TY_VOID);
	return ret;
}

/*
 * The next conversion in a printf format: its letter, 0 at the end, or -1
 * when malformed.
 */
static int format_check(struct comp *c, const struct token *fmt_tok, const char **f, const char *end,
			struct type arg, const struct token *at, int argn)
{
	const char *spec;
	int conv = al_fmt_next(f, end, &spec);

	if (conv < 0) {
		error_at(c, fmt_tok, "bad conversion in format string");
		return -1;
	}
	if (conv == 0) {
		error_at(c, at, "argument %d has no %% conversion in the format string", argn);
		return -1;
	}
	if (arg.base == TY_ERROR)
		return conv;
	if (strchr("dixXouc", conv) && !is_scalar(arg, TY_INT) && !is_scalar(arg, TY_BOOL))
		error_at(c, at, "%%%c needs an int, argument %d is %s", conv, argn, type_name(c, arg));
	else if (strchr("feEgG", conv) && !is_scalar(arg, TY_FLOAT) && !is_scalar(arg, TY_INT))
		error_at(c, at, "%%%c needs a float, argument %d is %s", conv, argn, type_name(c, arg));
	return conv;
}

/* printf, format, print, println: any values, described by a hidden argument. */
static struct type builtin_print(struct comp *c, int id, const struct token *name)
{
	bool has_fmt = id == B_printf || id == B_format;
	struct buf desc = { 0 };
	struct token fmt_tok = c->tok;
	char *fmt = NULL;
	const char *f = NULL, *fend = NULL;
	int argc = 0;

	expect(c, TK_LPAREN, "'('");
	if (has_fmt) {
		fmt_tok = c->tok;
		if (check(c, TK_STRLIT) && (c->peek.kind == TK_COMMA || c->peek.kind == TK_RPAREN)) {
			fmt = port_alloc(fmt_tok.len + 1);
			if (!fmt) {
				out_of_memory(c);
				return T_ERROR;
			}
			f = fmt;
			fend = fmt + lex_decode(c->src, &fmt_tok, fmt);
		}
		if (check(c, TK_RPAREN))
			error_at(c, &c->tok, "%s() needs a format string", al_builtins[id].name);
		else
			assign_value(c, T_STR);
		argc = 1;
	}
	if (has_fmt ? match(c, TK_COMMA) : !check(c, TK_RPAREN)) {
		do {
			struct token at = c->tok;
			struct type t = expr(c, T_NONE);
			if (t.base == TY_VOID)
				error_at(c, &at, "this has no value (it is void)");
			desc_add(c, &desc, t);
			if (f && format_check(c, &fmt_tok, &f, fend, t, &at, argc) < 0)
				f = NULL;
			argc++;
		} while (match(c, TK_COMMA) && !c->oom);
	}
	if (f) {
		const char *spec;
		int conv = al_fmt_next(&f, fend, &spec);
		if (conv < 0)
			error_at(c, &fmt_tok, "bad conversion in format string");
		else if (conv > 0)
			error_at(c, &fmt_tok, "the format string wants more arguments");
	}
	expect(c, TK_RPAREN, "')'");
	port_free(fmt);
	emit_op_w(c, OP_CONSTS, c->oom ? 0 : intern(c, (char *)desc.p, desc.len));
	buf_free(&desc);
	argc++;
	emit_callb(c, id, argc, id == B_format);
	return id == B_format ? T_STR : T_VOID;
}

/* An argument that must be an array; a bare local is not loaded, *slot gets it. */
static struct type array_arg(struct comp *c, const struct token *name, int *slot, int follow)
{
	struct token at = c->tok;
	struct type t;

	*slot = -1;
	if (slot && check(c, TK_IDENT) && c->peek.kind == follow) {
		struct clocal *l = find_local(c, &c->tok);
		if (l && l->type.dims) {
			advance(c);
			*slot = l->slot;
			return l->type;
		}
	}
	t = expr(c, T_NONE);
	if (t.base != TY_ERROR && !t.dims) {
		error_at(c, &at, "%.*s() needs an array, got %s", (int)name->len, c->src + name->pos, type_name(c, t));
		return T_ERROR;
	}
	return t;
}

static struct type builtin_special(struct comp *c, int id, const struct token *name)
{
	struct token at;
	struct type t, u;
	int slot;

	expect(c, TK_LPAREN, "'('");
	switch (id) {
	case B_find:
		at = c->tok;
		if (!next_arg(c, 0, name))
			break;
		t = expr(c, T_NONE);
		if (is_scalar(t, TY_STR)) {
			expect(c, TK_COMMA, "','");
			assign_value(c, T_STR);
			int argc = 2;
			if (match(c, TK_COMMA)) {
				assign_value(c, T_INT);
				argc++;
			}
			end_args(c, name);
			emit_callb(c, B_finds, argc, true);
			return T_INT;
		}
		if (t.base != TY_ERROR && !t.dims) {
			error_at(c, &at, "find() needs a str or an array, got %s", type_name(c, t));
			break;
		}
		expect(c, TK_COMMA, "','");
		assign_value(c, t.base == TY_ERROR ? T_NONE : elem_type(t));
		end_args(c, name);
		emit_callb(c, B_finda, 2, true);
		return T_INT;

	case B_insert:
	case B_remove_at:
	case B_resize:
	case B_slice:
		if (!next_arg(c, 0, name))
			break;
		t = array_arg(c, name, &slot, -1);
		if (!next_arg(c, 1, name))
			break;
		assign_value(c, T_INT);
		if (id == B_insert) {
			if (!next_arg(c, 2, name))
				break;
			assign_value(c, t.base == TY_ERROR ? T_NONE : elem_type(t));
		} else if (id == B_slice) {
			if (!next_arg(c, 2, name))
				break;
			assign_value(c, T_INT);
		}
		end_args(c, name);
		if (id == B_insert || id == B_slice) {
			emit_callb(c, id, 3, id == B_slice);
			return id == B_slice ? t : T_VOID;
		}
		emit_callb(c, id, 2, id == B_remove_at);
		return id == B_remove_at ? (t.base == TY_ERROR ? t : elem_type(t)) : T_VOID;

	case B_reverse:
		/* Any array can be turned back to front, whatever it holds. */
		at = c->tok;
		if (!next_arg(c, 0, name))
			break;
		t = array_arg(c, name, &slot, -1);
		end_args(c, name);
		emit_callb(c, id, 1, false);
		return T_VOID;

	case B_sort:
		at = c->tok;
		if (!next_arg(c, 0, name))
			break;
		t = array_arg(c, name, &slot, -1);
		if (t.base != TY_ERROR && (t.dims != 1 || !(t.base == TY_INT || t.base == TY_FLOAT || t.base == TY_STR)))
			error_at(c, &at, "sort() works on int[], float[] and str[], not %s", type_name(c, t));
		end_args(c, name);
		emit_callb(c, id, 1, false);
		return T_VOID;

	case B_abs:
		at = c->tok;
		if (!next_arg(c, 0, name))
			break;
		t = expr(c, T_NONE);
		end_args(c, name);
		if (is_scalar(t, TY_FLOAT)) {
			emit_callb(c, B_absf, 1, true);
			return T_FLOAT;
		}
		if (!is_scalar(t, TY_INT) && t.base != TY_ERROR)
			error_at(c, &at, "abs() needs a number, got %s", type_name(c, t));
		emit_callb(c, B_abs, 1, true);
		return T_INT;

	case B_min:
	case B_max:
		at = c->tok;
		if (!next_arg(c, 0, name))
			break;
		t = expr(c, T_NONE);
		if (!next_arg(c, 1, name))
			break;
		struct token at2 = c->tok;
		u = expr(c, T_NONE);
		end_args(c, name);
		if (t.base == TY_ERROR || u.base == TY_ERROR)
			return T_ERROR;
		if (!is_scalar(t, TY_INT) && !is_scalar(t, TY_FLOAT)) {
			error_at(c, &at, "%s() needs numbers, got %s", al_builtins[id].name, type_name(c, t));
			return T_ERROR;
		}
		if (!is_scalar(u, TY_INT) && !is_scalar(u, TY_FLOAT)) {
			error_at(c, &at2, "%s() needs numbers, got %s", al_builtins[id].name, type_name(c, u));
			return T_ERROR;
		}
		if (is_scalar(t, TY_INT) && is_scalar(u, TY_INT)) {
			emit_callb(c, id, 2, true);
			return T_INT;
		}
		if (is_scalar(t, TY_INT))
			emit_op(c, OP_I2F2);
		if (is_scalar(u, TY_INT))
			emit_op(c, OP_I2F);
		emit_callb(c, id == B_min ? B_minf : B_maxf, 2, true);
		return T_FLOAT;

	case B_run: {
		at = c->tok;
		if (!next_arg(c, 0, name))
			break;
		t = expr(c, T_NONE);
		if (same_type(t, T_STRS)) {
			end_args(c, name);
			emit_callb(c, B_runa, 1, true);
			return T_INT;
		}
		coerce(c, t, T_STR, &at);
		int argc = 1;
		while (match(c, TK_COMMA)) {
			assign_value(c, T_STR);
			if (++argc > 64) {
				error_at(c, &c->prev, "too many arguments to run(); pass a str[]");
				break;
			}
		}
		end_args(c, name);
		emit_callb(c, B_run, argc, true);
		return T_INT;
	}
	}
	/* an error was reported: skip to the closing parenthesis */
	while (!check(c, TK_RPAREN) && !check(c, TK_EOF) && !check(c, TK_SEMI))
		advance(c);
	match(c, TK_RPAREN);
	return T_ERROR;
}

static struct type builtin_call(struct comp *c, int id, const struct token *name)
{
	int slot;
	struct type t;
	struct token at;

	if (tok_is(c, name, "len")) {
		expect(c, TK_LPAREN, "'('");
		at = c->tok;
		if (check(c, TK_IDENT) && c->peek.kind == TK_RPAREN) {
			struct clocal *l = find_local(c, &c->tok);
			if (l && (l->type.dims || is_scalar(l->type, TY_STR))) {
				advance(c);
				advance(c);
				emit_op_b(c, OP_LENL, l->slot);
				return T_INT;
			}
		}
		t = expr(c, T_NONE);
		end_args(c, name);
		if (is_scalar(t, TY_STR))
			emit_op(c, OP_LENS);
		else if (t.dims)
			emit_op(c, OP_LENA);
		else if (t.base != TY_ERROR)
			error_at(c, &at, "len() needs a str or an array, got %s", type_name(c, t));
		return T_INT;
	}
	if (tok_is(c, name, "push") || tok_is(c, name, "pop")) {
		bool push = tok_is(c, name, "push");
		expect(c, TK_LPAREN, "'('");
		if (!next_arg(c, 0, name))
			return T_ERROR;
		t = array_arg(c, name, &slot, push ? TK_COMMA : -1);
		if (!push) {
			end_args(c, name);
			emit_op(c, OP_POPA);
			return t.base == TY_ERROR ? t : elem_type(t);
		}
		if (!next_arg(c, 1, name))
			return T_ERROR;
		assign_value(c, t.base == TY_ERROR ? T_NONE : elem_type(t));
		end_args(c, name);
		if (slot >= 0)
			emit_op_b(c, OP_PUSHL, slot);
		else
			emit_op(c, OP_PUSHA);
		return T_VOID;
	}
	switch (id) {
	case B_printf:
	case B_format:
	case B_print:
	case B_println:
		return builtin_print(c, id, name);
	}
	if (al_builtins[id].sig[0] != '*')
		return builtin_sig(c, id, name);
	return builtin_special(c, id, name);
}

static struct type call(struct comp *c, const struct token *name)
{
	int fi = find_func(c, name);

	if (fi < 0) {
		int id = find_builtin(c, name);
		if (id >= 0 || tok_is(c, name, "len") || tok_is(c, name, "push") || tok_is(c, name, "pop"))
			return builtin_call(c, id, name);
		if (find_local(c, name) || find_global(c, name) >= 0)
			error_at(c, name, "'%.*s' is a variable, not a function", (int)name->len, c->src + name->pos);
		else
			error_at(c, name, "unknown function '%.*s'", (int)name->len, c->src + name->pos);
		while (!check(c, TK_RPAREN) && !check(c, TK_EOF) && !check(c, TK_SEMI))
			advance(c);
		match(c, TK_RPAREN);
		return T_ERROR;
	}

	struct cfunc *f = &c->funcs[fi];
	int n = 0;

	expect(c, TK_LPAREN, "'('");
	if (!check(c, TK_RPAREN)) {
		do {
			if (n >= f->nparams) {
				error_at(c, &c->tok, "too many arguments to %.*s() (it takes %d)",
					 (int)name->len, c->src + name->pos, f->nparams);
				break;
			}
			assign_value(c, c->params[f->first + n]);
			n++;
		} while (match(c, TK_COMMA));
	}
	if (n < f->nparams)
		error_at(c, &c->tok, "too few arguments to %.*s() (it takes %d)",
			 (int)name->len, c->src + name->pos, f->nparams);
	expect(c, TK_RPAREN, "')'");
	emit_op_w(c, OP_CALL, fi);
	stack(c, -n + (f->ret.base != TY_VOID));
	return f->ret;
}

/* ------------------------------------------------------------ statements */

static bool at_declaration(struct comp *c)
{
	switch (c->tok.kind) {
	case TK_INT:
	case TK_FLOAT:
	case TK_BOOL:
	case TK_STR:
		return c->peek.kind != TK_LPAREN;
	case TK_FILE:
	case TK_VOID:
		return true;
	case TK_IDENT:
		return find_struct(c, &c->tok) >= 0 && c->peek.kind != TK_LBRACE;
	}
	return false;
}

static void declaration(struct comp *c, bool is_const)
{
	struct type t;

	if (!parse_type(c, &t))
		return;
	if (t.base == TY_VOID) {
		error_at(c, &c->prev, "variables cannot be void");
		return;
	}
	do {
		struct token name = c->tok;
		if (!expect(c, TK_IDENT, "a variable name"))
			return;
		if (match(c, TK_ASSIGN)) {
			assign_value(c, t);
		} else {
			if (is_const)
				error_at(c, &name, "constant '%.*s' needs a value", (int)name.len, c->src + name.pos);
			emit_default(c, t);
		}
		emit_op_b(c, OP_STORE, add_local(c, &name, t, is_const));
	} while (match(c, TK_COMMA));
	expect(c, TK_SEMI, "';'");
}

/* An assignment, x++, or an expression whose value is dropped (a call). */
static void simple_statement(struct comp *c)
{
	struct type t;

	if (check(c, TK_INC) || check(c, TK_DEC)) {
		c->prefix_op = c->tok.kind;
		advance(c);
		struct token at = c->tok;
		t = operand(c, T_NONE, true);
		if (t.base != TY_ASSIGNED && t.base != TY_ERROR)
			error_at(c, &at, "cannot assign to this expression");
		c->prefix_op = 0;
		return;
	}
	t = operand(c, T_NONE, true);
	if (t.base == TY_ASSIGNED)
		return;
	t = binary_rest(c, t, 1);
	t = ternary_rest(c, t, T_NONE);
	if (t.base != TY_VOID && t.base != TY_ERROR)
		emit_op(c, is_ref(t) ? OP_POPR : OP_POP);
}

static bool scoped_statement(struct comp *c)
{
	begin_scope(c);
	if (at_declaration(c) || check(c, TK_CONST))
		error_at(c, &c->tok, "a declaration needs a block here; add { }");
	bool r = statement(c);
	end_scope(c);
	return r;
}

static void synchronize(struct comp *c)
{
	while (!check(c, TK_EOF)) {
		if (c->prev.kind == TK_SEMI)
			break;
		switch (c->tok.kind) {
		case TK_RBRACE:
		case TK_IF:
		case TK_WHILE:
		case TK_FOR:
		case TK_RETURN:
		case TK_BREAK:
		case TK_CONTINUE:
		case TK_CONST:
			goto done;
		}
		advance(c);
	}
done:
	c->panic = false;
}

static bool block_body(struct comp *c)
{
	bool returns = false;

	while (!check(c, TK_RBRACE) && !check(c, TK_EOF)) {
		uint32_t pos = c->tok.pos;
		returns |= statement(c);
		if (c->panic)
			synchronize(c);
		if (c->tok.pos == pos && !check(c, TK_RBRACE) && !check(c, TK_EOF))
			advance(c);
	}
	expect(c, TK_RBRACE, "'}'");
	return returns;
}

static void patch_list(struct comp *c, uint32_t *list, uint32_t from, uint32_t to)
{
	for (uint32_t i = from; i < to; i++)
		patch_jump(c, list[i]);
}

static bool condition(struct comp *c, struct capture *cap)
{
	struct token at = c->tok;

	capture_begin(c, cap);
	struct type t = expr(c, T_BOOL);
	need_bool(c, t, &at, "the condition");
	capture_end(c, cap);
	return true;
}

static bool loop_body(struct comp *c, struct loop *lp)
{
	lp->outer = c->loop;
	lp->depth = c->depth;
	lp->breaks = c->nbreaks;
	lp->conts = c->nconts;
	lp->has_break = false;
	c->loop = lp;
	bool r = scoped_statement(c);
	c->loop = lp->outer;
	return r;
}

static bool while_statement(struct comp *c)
{
	struct capture cond = { 0 };
	struct loop lp;

	advance(c);
	expect(c, TK_LPAREN, "'('");
	bool forever = check(c, TK_TRUE) && c->peek.kind == TK_RPAREN;
	if (forever)
		advance(c);
	else
		condition(c, &cond);
	expect(c, TK_RPAREN, "')'");

	uint32_t jtest = forever ? 0 : emit_jump(c, OP_JMP);
	uint32_t body = pc(c);
	c->label = body;
	loop_body(c, &lp);
	patch_list(c, c->conts, lp.conts, c->nconts);
	c->nconts = lp.conts;
	if (forever) {
		emit_loop(c, OP_LOOP, body);
	} else {
		patch_jump(c, jtest);
		capture_emit(c, &cond);
		emit_loop_true(c, body);
	}
	patch_list(c, c->breaks, lp.breaks, c->nbreaks);
	c->nbreaks = lp.breaks;
	capture_free(&cond);
	return forever && !lp.has_break;
}

static bool for_statement(struct comp *c)
{
	struct capture cond = { 0 }, step = { 0 };
	struct loop lp;

	advance(c);
	expect(c, TK_LPAREN, "'('");
	begin_scope(c);
	if (match(c, TK_SEMI)) {
		/* no initialiser */
	} else if (at_declaration(c)) {
		declaration(c, false);
	} else {
		simple_statement(c);
		expect(c, TK_SEMI, "';'");
	}
	bool forever = check(c, TK_SEMI);
	if (!forever)
		condition(c, &cond);
	expect(c, TK_SEMI, "';'");
	capture_begin(c, &step);
	if (!check(c, TK_RPAREN))
		simple_statement(c);
	capture_end(c, &step);
	expect(c, TK_RPAREN, "')'");

	uint32_t jtest = forever ? 0 : emit_jump(c, OP_JMP);
	uint32_t body = pc(c);
	c->label = body;
	loop_body(c, &lp);
	patch_list(c, c->conts, lp.conts, c->nconts);
	c->nconts = lp.conts;
	capture_emit(c, &step);
	if (forever) {
		emit_loop(c, OP_LOOP, body);
	} else {
		patch_jump(c, jtest);
		capture_emit(c, &cond);
		emit_loop_true(c, body);
	}
	patch_list(c, c->breaks, lp.breaks, c->nbreaks);
	c->nbreaks = lp.breaks;
	end_scope(c);
	capture_free(&cond);
	capture_free(&step);
	return forever && !lp.has_break;
}

/*
 * if / else if / else. The chain is a loop, not recursion: a program with
 * twenty else-ifs must not use twenty levels of the compiler's stack.
 */
static bool if_statement(struct comp *c)
{
	uint32_t first_end = c->nends;
	bool returns = true, has_else = false;

	for (;;) {
		advance(c);		/* 'if' */
		expect(c, TK_LPAREN, "'('");
		struct token at = c->tok;
		struct type t = expr(c, T_BOOL);
		need_bool(c, t, &at, "the condition");
		expect(c, TK_RPAREN, "')'");
		uint32_t jf = emit_jump_false(c);
		returns &= scoped_statement(c);
		if (!check(c, TK_ELSE)) {
			patch_jump(c, jf);
			break;
		}
		advance(c);		/* 'else' */
		if (GROW(c, c->ends, c->cap_ends, c->nends + 1))
			c->ends[c->nends++] = emit_jump(c, OP_JMP);
		patch_jump(c, jf);
		if (check(c, TK_IF))
			continue;
		has_else = true;
		returns &= scoped_statement(c);
		break;
	}
	patch_list(c, c->ends, first_end, c->nends);
	c->nends = first_end;
	return returns && has_else;
}

static bool return_statement(struct comp *c)
{
	struct token kw = c->tok;

	advance(c);
	if (c->ret.base == TY_VOID) {
		if (!check(c, TK_SEMI)) {
			error_at(c, &c->tok, "this function is void and cannot return a value");
			expr(c, T_NONE);
		}
		emit_clears(c, 0);
		emit_op(c, OP_RET);
	} else {
		if (check(c, TK_SEMI))
			error_at(c, &kw, "return needs a %s value", type_name(c, c->ret));
		else
			assign_value(c, c->ret);
		emit_clears(c, 0);
		emit_op(c, OP_RETV);
	}
	expect(c, TK_SEMI, "';'");
	return true;
}

static void jump_statement(struct comp *c)
{
	struct token kw = c->tok;
	bool brk = kw.kind == TK_BREAK;

	advance(c);
	if (!c->loop) {
		error_at(c, &kw, "%s outside a loop", brk ? "break" : "continue");
	} else {
		emit_clears(c, c->loop->depth);
		uint32_t at = emit_jump(c, OP_JMP);
		if (brk) {
			if (GROW(c, c->breaks, c->cap_breaks, c->nbreaks + 1))
				c->breaks[c->nbreaks++] = at;
			c->loop->has_break = true;
		} else if (GROW(c, c->conts, c->cap_conts, c->nconts + 1)) {
			c->conts[c->nconts++] = at;
		}
	}
	expect(c, TK_SEMI, "';'");
}

static bool statement(struct comp *c)
{
	bool returns = false;

	if (!enter(c))
		return false;
	switch (c->tok.kind) {
	case TK_LBRACE:
		advance(c);
		begin_scope(c);
		returns = block_body(c);
		end_scope(c);
		break;
	case TK_IF:
		returns = if_statement(c);
		break;
	case TK_WHILE:
		returns = while_statement(c);
		break;
	case TK_FOR:
		returns = for_statement(c);
		break;
	case TK_RETURN:
		returns = return_statement(c);
		break;
	case TK_BREAK:
	case TK_CONTINUE:
		jump_statement(c);
		break;
	case TK_SEMI:
		advance(c);
		break;
	case TK_CONST:
		advance(c);
		declaration(c, true);
		break;
	case TK_ELSE:
		error_at(c, &c->tok, "else without if");
		advance(c);
		break;
	default:
		if (at_declaration(c)) {
			declaration(c, false);
		} else {
			simple_statement(c);
			expect(c, TK_SEMI, "';'");
		}
		break;
	}
	leave(c);
	return returns;
}

/* ------------------------------------------------------------ declarations */

/* Skip the rest of a top-level item: through its { } block or its ';'. */
static void skip_item(struct comp *c)
{
	int depth = 0;

	while (!check(c, TK_EOF)) {
		if (check(c, TK_LBRACE)) {
			depth++;
		} else if (check(c, TK_RBRACE)) {
			if (--depth <= 0) {
				advance(c);
				break;
			}
		} else if (check(c, TK_SEMI) && depth == 0) {
			advance(c);
			break;
		}
		advance(c);
	}
	c->panic = false;
}

static void pass_structs(struct comp *c)
{
	int depth = 0;

	start_pass(c);
	while (!check(c, TK_EOF)) {
		if (check(c, TK_ERROR)) {
			error_at(c, &c->tok, "%s", c->tok.v.msg);
			c->panic = false;
		} else if (check(c, TK_LBRACE)) {
			depth++;
		} else if (check(c, TK_RBRACE)) {
			depth--;
		} else if (depth == 0 && check(c, TK_STRUCT) && c->peek.kind == TK_IDENT) {
			advance(c);
			if (find_struct(c, &c->tok) >= 0) {
				error_at(c, &c->tok, "struct '%.*s' is already declared", (int)c->tok.len, c->src + c->tok.pos);
				c->panic = false;
			} else if (c->nstructs >= 0xffff) {
				error_at(c, &c->tok, "too many structs");
			} else if (GROW(c, c->structs, c->cap_structs, c->nstructs + 1)) {
				struct cstruct *s = &c->structs[c->nstructs++];
				memset(s, 0, sizeof(*s));
				s->name = c->tok.pos;
				s->len = c->tok.len;
				s->sname = intern_tok(c, &c->tok);
			}
		}
		advance(c);
	}
}

static bool reserved_name(struct comp *c, const struct token *name)
{
	if (find_builtin(c, name) >= 0 || tok_is(c, name, "len") || tok_is(c, name, "push") || tok_is(c, name, "pop")) {
		error_at(c, name, "'%.*s' is a built-in function", (int)name->len, c->src + name->pos);
		return true;
	}
	for (size_t i = 0; i < sizeof(int_consts) / sizeof(int_consts[0]); i++)
		if (tok_is(c, name, int_consts[i].name))
			goto constant;
	if (tok_is(c, name, "PI") || tok_is(c, name, "stdin") || tok_is(c, name, "stdout") || tok_is(c, name, "stderr"))
		goto constant;
	return false;
constant:
	error_at(c, name, "'%.*s' is a built-in constant", (int)name->len, c->src + name->pos);
	return true;
}

static void struct_fields(struct comp *c)
{
	struct token name = c->tok;
	int sid = check(c, TK_IDENT) ? find_struct(c, &name) : -1;

	if (sid < 0) {
		expect(c, TK_IDENT, "a struct name");
		skip_item(c);
		return;
	}
	advance(c);
	struct cstruct *s = &c->structs[sid];
	s->first = c->nfields;
	expect(c, TK_LBRACE, "'{'");
	while (!check(c, TK_RBRACE) && !check(c, TK_EOF)) {
		struct type t;
		if (!parse_type(c, &t)) {
			skip_item(c);
			return;
		}
		if (t.base == TY_VOID)
			error_at(c, &c->prev, "fields cannot be void");
		do {
			struct token fname = c->tok;
			if (!expect(c, TK_IDENT, "a field name"))
				break;
			if (find_field(c, sid, &fname) >= 0)
				error_at(c, &fname, "field '%.*s' is already declared", (int)fname.len, c->src + fname.pos);
			if (s->nfields >= MAX_FIELDS) {
				error_at(c, &fname, "too many fields (the limit is %d)", MAX_FIELDS);
				break;
			}
			if (!GROW(c, c->fields, c->cap_fields, c->nfields + 1))
				return;
			s = &c->structs[sid];
			struct cfield *f = &c->fields[c->nfields++];
			f->name = fname.pos;
			f->len = fname.len;
			f->type = t;
			f->sname = intern_tok(c, &fname);
			f->desc = type_desc(c, t);
			s->nfields++;
		} while (match(c, TK_COMMA));
		if (!expect(c, TK_SEMI, "';'")) {
			skip_item(c);
			return;
		}
	}
	expect(c, TK_RBRACE, "'}'");
	match(c, TK_SEMI);
}

/* A literal initialiser the compiler can substitute: 42, -1.5, "text", true */
static bool fold_constant(struct comp *c, struct cglobal *g)
{
	bool neg = false;
	struct token lit;

	if (check(c, TK_MINUS)) {
		neg = true;
		lit = c->peek;
	} else {
		lit = c->tok;
	}
	struct lexer save = c->lx;
	struct token peek = c->peek, tok = c->tok, prev = c->prev;
	if (neg)
		advance(c);
	advance(c);
	bool end = check(c, TK_SEMI);
	c->lx = save;
	c->peek = peek;
	c->tok = tok;
	c->prev = prev;
	if (!end)
		return false;

	if (is_scalar(g->type, TY_INT) && (lit.kind == TK_INTLIT || lit.kind == TK_CHARLIT)) {
		g->value.i = neg ? (int32_t)(0u - (uint32_t)lit.v.i) : lit.v.i;
	} else if (is_scalar(g->type, TY_FLOAT) && lit.kind == TK_FLOATLIT) {
		g->value.f = neg ? -lit.v.f : lit.v.f;
	} else if (is_scalar(g->type, TY_FLOAT) && lit.kind == TK_INTLIT) {
		g->value.f = neg ? -(float)lit.v.i : (float)lit.v.i;
	} else if (is_scalar(g->type, TY_BOOL) && !neg && (lit.kind == TK_TRUE || lit.kind == TK_FALSE)) {
		g->value.i = lit.kind == TK_TRUE;
	} else if (is_scalar(g->type, TY_STR) && !neg && lit.kind == TK_STRLIT) {
		char *tmp = port_alloc(lit.len + 1);
		if (!tmp) {
			out_of_memory(c);
			return false;
		}
		g->value.s = intern(c, tmp, lex_decode(c->src, &lit, tmp));
		port_free(tmp);
	} else {
		return false;
	}
	g->folded = true;
	return true;
}

static void pass_decls(struct comp *c)
{
	start_pass(c);
	while (!check(c, TK_EOF)) {
		if (match(c, TK_STRUCT)) {
			struct_fields(c);
		} else if (check(c, TK_CONST) || type_start(c, &c->tok)) {
			bool is_const = match(c, TK_CONST);
			struct type t;
			if (!parse_type(c, &t)) {
				skip_item(c);
				continue;
			}
			struct token name = c->tok;
			if (!expect(c, TK_IDENT, "a name")) {
				skip_item(c);
				continue;
			}
			if (reserved_name(c, &name) || find_func(c, &name) >= 0 || find_global(c, &name) >= 0 ||
			    find_struct(c, &name) >= 0) {
				if (!c->panic)
					error_at(c, &name, "'%.*s' is already declared", (int)name.len, c->src + name.pos);
				skip_item(c);
				continue;
			}
			if (check(c, TK_LPAREN) && !is_const) {
				if (c->nfuncs >= 0xfffe) {
					error_at(c, &name, "too many functions");
					skip_item(c);
					continue;
				}
				if (!GROW(c, c->funcs, c->cap_funcs, c->nfuncs + 1))
					return;
				struct cfunc *f = &c->funcs[c->nfuncs++];
				memset(f, 0, sizeof(*f));
				f->name = name.pos;
				f->len = name.len;
				f->ret = t;
				f->first = c->nparams;
				f->sname = intern_tok(c, &name);
				advance(c);
				if (!check(c, TK_RPAREN)) {
					do {
						struct type pt;
						if (!parse_type(c, &pt))
							break;
						if (pt.base == TY_VOID)
							error_at(c, &c->prev, "parameters cannot be void");
						expect(c, TK_IDENT, "a parameter name");
						if (f->nparams >= MAX_LOCALS) {
							error_at(c, &c->prev, "too many parameters");
							break;
						}
						if (!GROW(c, c->params, c->cap_params, c->nparams + 1))
							return;
						f = &c->funcs[c->nfuncs - 1];
						c->params[c->nparams++] = pt;
						f->nparams++;
					} while (match(c, TK_COMMA));
				}
				expect(c, TK_RPAREN, "')'");
				if (!check(c, TK_LBRACE))
					error_at(c, &c->tok, "expected '{' to start the body of %.*s()",
						 (int)name.len, c->src + name.pos);
				skip_item(c);
				continue;
			}
			if (t.base == TY_VOID) {
				error_at(c, &name, "variables cannot be void");
				skip_item(c);
				continue;
			}
			if (c->nglobals >= 0xffff) {
				error_at(c, &name, "too many globals");
				skip_item(c);
				continue;
			}
			if (!GROW(c, c->globals, c->cap_globals, c->nglobals + 1))
				return;
			struct cglobal *g = &c->globals[c->nglobals++];
			memset(g, 0, sizeof(*g));
			g->name = name.pos;
			g->len = name.len;
			g->type = t;
			g->is_const = is_const;
			if (match(c, TK_ASSIGN)) {
				if (is_const)
					fold_constant(c, g);
			} else if (is_const) {
				error_at(c, &name, "constant '%.*s' needs a value", (int)name.len, c->src + name.pos);
			} else if (!check(c, TK_SEMI)) {
				expect(c, TK_SEMI, "';' or '='");
			}
			skip_item(c);
		} else {
			error_at(c, &c->tok, "expected a function, struct or global declaration, found %s",
				 tok_name(c->tok.kind));
			skip_item(c);
		}
	}
}

static void begin_function(struct comp *c, struct type ret)
{
	c->nlocals = 0;
	c->max_locals = 0;
	c->depth = 1;
	c->sp = 0;
	c->max_sp = 0;
	c->ret = ret;
	c->loop = NULL;
	c->fn_start = pc(c);
	c->fn_lines = c->lines.len;
	c->last_line = 0;
	c->last_op = NO_POS;
	c->label = NO_POS;
}

static void end_function(struct comp *c, struct cfunc *f, const struct token *at)
{
	f->code = c->fn_start;
	f->code_len = pc(c) - c->fn_start;
	f->lines = c->fn_lines / 4;
	f->nlines = (c->lines.len - c->fn_lines) / 4;
	f->max_stack = c->max_sp > 0xffff ? 0xffff : c->max_sp;
	f->nlocals = c->max_locals;
	if (f->code_len > 32767)
		error_at(c, at, "function too large (over 32 KB of code); split it");
	if (c->max_sp > 0xffff)
		error_at(c, at, "expression too large");
}

static void function_body(struct comp *c, struct cfunc *f)
{
	struct token name = c->tok;

	begin_function(c, f->ret);
	advance(c);
	expect(c, TK_LPAREN, "'('");
	if (!check(c, TK_RPAREN)) {
		do {
			struct type pt;
			if (!parse_type(c, &pt))
				break;
			struct token pname = c->tok;
			if (!expect(c, TK_IDENT, "a parameter name"))
				break;
			add_local(c, &pname, pt, false);
		} while (match(c, TK_COMMA));
	}
	expect(c, TK_RPAREN, "')'");
	if (!expect(c, TK_LBRACE, "'{'")) {
		skip_item(c);
		return;
	}
	bool returns = block_body(c);
	if (!returns) {
		if (f->ret.base == TY_VOID) {
			emit_clears(c, 0);
			emit_op(c, OP_RET);
		} else {
			error_at(c, &c->prev, "%.*s() can reach its end without returning a value",
				 (int)name.len, c->src + name.pos);
		}
	}
	end_function(c, f, &name);
}

static void pass_bodies(struct comp *c)
{
	uint32_t fi = 0;

	start_pass(c);
	while (!check(c, TK_EOF)) {
		if (match(c, TK_STRUCT)) {
			skip_item(c);
			continue;
		}
		bool is_const = match(c, TK_CONST);
		struct type t;
		if (!parse_type(c, &t)) {
			skip_item(c);
			continue;
		}
		if (check(c, TK_IDENT) && c->peek.kind == TK_LPAREN && !is_const && fi < c->nfuncs &&
		    same_name(c, c->funcs[fi].name, c->funcs[fi].len, &c->tok)) {
			function_body(c, &c->funcs[fi++]);
			c->panic = false;
		} else {
			skip_item(c);
		}
	}
}

static void pass_globals(struct comp *c)
{
	struct cfunc init = { 0 };
	uint32_t gi = 0;

	init.ret = T_VOID;
	begin_function(c, T_VOID);
	start_pass(c);
	while (!check(c, TK_EOF)) {
		if (match(c, TK_STRUCT)) {
			skip_item(c);
			continue;
		}
		match(c, TK_CONST);
		struct type t;
		if (!parse_type(c, &t)) {
			skip_item(c);
			continue;
		}
		struct token name = c->tok;
		if (!check(c, TK_IDENT) || c->peek.kind == TK_LPAREN || gi >= c->nglobals ||
		    !same_name(c, c->globals[gi].name, c->globals[gi].len, &name)) {
			skip_item(c);
			continue;
		}
		struct cglobal *g = &c->globals[gi++];
		advance(c);
		if (match(c, TK_ASSIGN) && !g->folded) {
			assign_value(c, g->type);
			emit_op_w(c, is_ref(g->type) ? OP_GSTORER : OP_GSTORE, gi - 1);
			if (!expect(c, TK_SEMI, "';'"))
				skip_item(c);
			continue;
		}
		skip_item(c);
	}
	c->init = AL_NO_FUNC;
	if (pc(c) != c->fn_start) {
		emit_op(c, OP_RET);
		end_function(c, &init, &c->tok);
		if (GROW(c, c->funcs, c->cap_funcs, c->nfuncs + 1)) {
			init.sname = intern(c, "<globals>", 9);
			c->init = c->nfuncs;
			c->funcs[c->nfuncs++] = init;
		}
	}
}

static void check_main(struct comp *c)
{
	for (uint32_t i = 0; i < c->nfuncs; i++) {
		struct cfunc *f = &c->funcs[i];
		if (f->len != 4 || memcmp(c->src + f->name, "main", 4))
			continue;
		struct token at = { .kind = TK_IDENT, .pos = f->name, .len = 4 };
		/* find the line for the message */
		at.line = 1;
		at.col = 1;
		for (uint32_t k = 0, ls = 0; k < f->name; k++) {
			if (c->src[k] == '\n') {
				at.line++;
				ls = k + 1;
			}
			at.col = f->name - ls + 1;
		}
		bool ret_ok = f->ret.base == TY_VOID || is_scalar(f->ret, TY_INT);
		bool args_ok = f->nparams == 0 || (f->nparams == 1 && same_type(c->params[f->first], T_STRS));
		if (!ret_ok || !args_ok)
			error_at(c, &at, "main must be int main(str[] args), int main() or void main()");
		c->main = i;
		return;
	}
	struct token end = { .kind = TK_EOF, .line = c->lx.line, .col = 1 };
	error_at(c, &end, "no main function");
}

/* ------------------------------------------------------------ output */

static void put16(uint8_t **p, uint32_t v)
{
	(*p)[0] = v;
	(*p)[1] = v >> 8;
	*p += 2;
}

static void put32(uint8_t **p, uint32_t v)
{
	(*p)[0] = v;
	(*p)[1] = v >> 8;
	(*p)[2] = v >> 16;
	(*p)[3] = v >> 24;
	*p += 4;
}

static int write_image(struct comp *c, struct al_image *out)
{
	int source = intern(c, c->file, strlen(c->file));
	size_t size = AL_HEADER_SIZE;

	if (c->oom)
		return -1;
	size += c->nstrings * 4 + c->strbuf.len;
	size += c->nstructs * 4 + c->nfields * 4;
	size += c->nglobals;
	size += c->nfuncs * 24;
	size += c->lines.len + c->code.len;
	size += AL_TRAILER_SIZE;
	if (size > AL_MAX_IMAGE) {
		al_eprintf("%s: program too large\n", c->file);
		return -1;
	}
	uint8_t *img = port_alloc(size), *p = img;
	if (!img) {
		al_eprintf("%s: out of memory\n", c->file);
		return -1;
	}
	memcpy(p, AL_MAGIC, 3);
	p[3] = AL_VERSION;
	p += 4;
	put16(&p, c->nstrings);
	put16(&p, c->nstructs);
	put16(&p, c->nglobals);
	put16(&p, c->nfuncs);
	put16(&p, c->main);
	put16(&p, c->init);
	put16(&p, source);
	put16(&p, 0);
	put32(&p, c->nfields);
	put32(&p, c->lines.len / 4);
	put32(&p, c->code.len);

	for (uint32_t i = 0; i < c->nstrings; i++) {
		put32(&p, c->strings[i].len);
		memcpy(p, c->strbuf.p + c->strings[i].off, c->strings[i].len);
		p += c->strings[i].len;
	}
	for (uint32_t i = 0; i < c->nstructs; i++) {
		put16(&p, c->structs[i].sname);
		put16(&p, c->structs[i].nfields);
	}
	for (uint32_t i = 0; i < c->nfields; i++) {
		put16(&p, c->fields[i].sname);
		put16(&p, c->fields[i].desc);
	}
	for (uint32_t i = 0; i < c->nglobals; i++)
		*p++ = kind_of(c->globals[i].type);
	for (uint32_t i = 0; i < c->nfuncs; i++) {
		struct cfunc *f = &c->funcs[i];
		put32(&p, f->code);
		put32(&p, f->code_len);
		put32(&p, f->lines);
		put32(&p, f->nlines);
		put16(&p, f->sname);
		put16(&p, f->max_stack);
		*p++ = f->nparams;
		*p++ = f->nlocals;
		*p++ = f->ret.base != TY_VOID;
		*p++ = 0;
	}
	if (c->lines.len)
		memcpy(p, c->lines.p, c->lines.len);
	p += c->lines.len;
	if (c->code.len)
		memcpy(p, c->code.p, c->code.len);
	p += c->code.len;
	put32(&p, al_crc32(img, p - img));
	out->data = img;
	out->len = p - img;
	return 0;
}

int al_compile(const char *filename, const char *src, size_t len, struct al_image *out)
{
	struct comp *c = port_alloc(sizeof(*c));
	int status = -1;

	out->data = NULL;
	out->len = 0;
	if (!c) {
		al_eprintf("%s: out of memory\n", filename);
		return -1;
	}
	memset(c, 0, sizeof(*c));
	c->file = filename;
	c->src = src;
	c->srclen = len;
	if (len > AL_MAX_SOURCE) {
		al_eprintf("%s: source file too large\n", filename);
		goto done;
	}
	c->empty_str = intern(c, "", 0);

	pass_structs(c);
	if (c->errors)
		goto done;
	pass_decls(c);
	if (c->errors)
		goto done;
	check_main(c);
	if (c->errors)
		goto done;
	pass_bodies(c);
	if (c->errors)
		goto done;
	pass_globals(c);
	if (c->errors)
		goto done;
	status = write_image(c, out);
done:
	port_free(c->structs);
	port_free(c->fields);
	port_free(c->funcs);
	port_free(c->params);
	port_free(c->globals);
	port_free(c->strings);
	port_free(c->breaks);
	port_free(c->conts);
	port_free(c->ends);
	buf_free(&c->strbuf);
	buf_free(&c->code);
	buf_free(&c->lines);
	buf_free(&c->scratch);
	port_free(c);
	return status;
}
