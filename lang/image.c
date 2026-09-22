/*
 * Executable images: the instruction and built-in tables, the loader with
 * its verifier, and a disassembler.
 *
 * File layout, all integers little-endian:
 *
 *	header	 32 bytes: 7f 'A' 'L' version, u16 strings structs globals
 *		 functions main init source reserved, u32 fields lines code
 *	strings	 u32 length, bytes
 *	structs	 u16 name, u16 field count
 *	fields	 u16 name, u16 type descriptor
 *	globals	 u8 kind
 *	funcs	 u32 code offset, length, first line, line count;
 *		 u16 name, max stack; u8 params, locals, returns, 0
 *	lines	 u16 pc, u16 line
 *	code
 *	crc	 u32 CRC-32 of everything before it
 *
 * The loader checks the CRC first, so a file damaged on its way to the
 * device is refused. Then it checks every count, offset and instruction, and
 * tracks the operand stack depth through each function, so that even a file
 * with a valid CRC cannot run off the end of something.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "al.h"
#include "port.h"

#define OPND(f, k) ((f "\0\0")[k] == 'w' || (f "\0\0")[k] == 'j' ? 2 :		\
		    (f "\0\0")[k] == 'i' || (f "\0\0")[k] == 'f' ? 4 :		\
		    (f "\0\0")[k] ? 1 : 0)

const struct al_opinfo al_opinfo[OP_COUNT] = {
#define X(name, fmt, pop, push) { #name, fmt, pop, push, 1 + OPND(fmt, 0) + OPND(fmt, 1) },
	AL_OPS(X)
#undef X
};

const struct al_builtin_info al_builtins[B_COUNT] = {
#define X(cname, name, sig) { name, sig },
	AL_BUILTINS(X)
#undef X
};

/* Argument count range and whether a value comes back. */
static void builtin_shape(int id, int *min, int *max, bool *value)
{
	const char *sig = al_builtins[id].sig;

	if (sig[0] != '*') {
		*min = *max = 0;
		for (; *sig && *sig != ':'; sig++) {
			if (*sig == '?')
				(*min)--;
			else
				(*min)++, (*max)++;
		}
		*value = sig[0] == ':' && sig[1] != 'v';
		return;
	}
	*value = true;
	switch (id) {
	case B_printf:
	case B_print:
	case B_println:
		*value = false;
		*min = id == B_printf ? 2 : 1;
		*max = 255;
		return;
	case B_format:
		*min = 2;
		*max = 255;
		return;
	case B_finda:
	case B_remove_at:
	case B_min:
	case B_max:
		*min = *max = 2;
		return;
	case B_insert:
	case B_slice:
		*min = *max = 3;
		*value = id == B_slice;
		return;
	case B_resize:
		*min = *max = 2;
		*value = false;
		return;
	case B_sort:
	case B_reverse:
		*min = *max = 1;
		*value = false;
		return;
	case B_abs:
		*min = *max = 1;
		return;
	case B_run:
		*min = 1;
		*max = 255;
		return;
	}
	/* find, len-like names the compiler resolves to other ids */
	*min = 1;
	*max = 0;
}

/* ------------------------------------------------------------ reading */

struct reader {
	const uint8_t	*p;
	size_t		 left;
	bool		 bad;
};

static const uint8_t *take(struct reader *r, size_t n)
{
	if (r->bad || n > r->left) {
		r->bad = true;
		return NULL;
	}
	const uint8_t *p = r->p;
	r->p += n;
	r->left -= n;
	return p;
}

static uint32_t rd16(struct reader *r)
{
	const uint8_t *p = take(r, 2);

	return p ? al_u16(p) : 0;
}

static uint32_t rd8(struct reader *r)
{
	const uint8_t *p = take(r, 1);

	return p ? *p : 0;
}

static uint32_t rd32(struct reader *r)
{
	const uint8_t *p = take(r, 4);

	return p ? al_u32(p) : 0;
}

/* One type descriptor starting at *pos; false if malformed. */
static bool desc_ok(const struct al_prog *p, const struct al_str *s, uint32_t *pos)
{
	int dims = 0;

	while (*pos < s->len && s->data[*pos] == '[') {
		if (++dims > 100)
			return false;
		(*pos)++;
	}
	if (*pos >= s->len)
		return false;
	char c = s->data[(*pos)++];
	if (strchr("ifbsFn", c))
		return true;
	if (c != 'S' || *pos + 4 > s->len)
		return false;
	unsigned id = 0;
	for (int k = 0; k < 4; k++) {
		char h = s->data[(*pos)++];
		id = id << 4 | (h >= 'a' ? h - 'a' + 10 : h - '0');
		if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f')))
			return false;
	}
	return id < p->nstructs;
}

static int desc_kind(const struct al_str *s)
{
	switch (s->data[0]) {
	case 'i': return K_INT;
	case 'f': return K_FLOAT;
	case 'b': return K_BOOL;
	case 's': return K_STR;
	}
	return K_REF;
}

/* Reflected CRC-32 (the zlib polynomial), four bits at a time. */
uint32_t al_crc32(const uint8_t *data, size_t len)
{
	static const uint32_t nibble[16] = {
		0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
		0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
		0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
	};
	uint32_t crc = 0xffffffff;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		crc = (crc >> 4) ^ nibble[crc & 15];
		crc = (crc >> 4) ^ nibble[crc & 15];
	}
	return ~crc;
}

struct span {
	uint32_t start, end;
};

static int compare_spans(const void *a, const void *b)
{
	const struct span *x = a, *y = b;

	return x->start < y->start ? -1 : x->start > y->start;
}

/*
 * Functions must not share code. The compiler never makes them overlap; if a
 * file did, rewriting one function's opcodes when fusing them (quicken.c)
 * could change another function's verified operands.
 */
static bool functions_apart(const struct al_prog *p)
{
	struct span *s = port_alloc(p->nfuncs * sizeof(*s));
	bool apart = s != NULL;

	for (uint32_t i = 0; apart && i < p->nfuncs; i++)
		s[i] = (struct span){ p->funcs[i].code, p->funcs[i].code + p->funcs[i].len };
	if (apart)
		qsort(s, p->nfuncs, sizeof(*s), compare_spans);
	for (uint32_t i = 1; apart && i < p->nfuncs; i++)
		apart = s[i - 1].end <= s[i].start;
	port_free(s);
	return apart;
}

__attribute__((format(printf, 3, 4)))
static int fail(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
	return -1;
}

static int verify(struct al_vm *vm, uint16_t index, char *err, size_t errlen)
{
	const struct al_prog *p = &vm->prog;
	const struct al_func *f = &p->funcs[index];
	const uint8_t *code = p->code + f->code;
	uint32_t len = f->len;
	int32_t *depth = port_alloc(len * sizeof(int32_t));
	uint32_t *work = port_alloc(len * sizeof(uint32_t));
	uint32_t nwork = 0, pc = 0;
	int status = -1;

	if (!depth || !work) {
		fail(err, errlen, "out of memory");
		goto done;
	}
	/* -2: not an instruction start, -1: start not reached yet */
	for (uint32_t i = 0; i < len; i++)
		depth[i] = -2;
	for (pc = 0; pc < len;) {
		if (code[pc] >= OP_COUNT) {
			fail(err, errlen, "bad instruction %u at %u", code[pc], (unsigned)pc);
			goto done;
		}
		uint32_t size = al_opinfo[code[pc]].size;
		if (pc + size > len) {
			fail(err, errlen, "instruction runs past the end at %u", (unsigned)pc);
			goto done;
		}
		depth[pc] = -1;
		pc += size;
	}

	depth[0] = 0;
	work[nwork++] = 0;
	while (nwork) {
		pc = work[--nwork];
		int d = depth[pc];
		const uint8_t *ip = code + pc;
		int op = ip[0];
		const struct al_opinfo *info = &al_opinfo[op];
		int pops = info->pop, pushes = info->push;
		uint32_t next = pc + info->size;
		bool falls = true;
		int64_t target = -1;
		int tdepth;

		/* operands */
		switch (op) {
		case OP_LOAD: case OP_LOADR: case OP_STORE: case OP_STORER: case OP_CLEARR:
		case OP_INCL: case OP_CATL: case OP_IDXL: case OP_IDXLR: case OP_SETIDXL:
		case OP_SETIDXLR: case OP_LENL: case OP_PUSHL: case OP_GETFL: case OP_GETFLR:
		case OP_SETFL: case OP_SETFLR:
			if (ip[1] >= f->nlocals)
				goto bad_operand;
			break;
		case OP_STDFILE:
		case OP_TOSTR:
		case OP_TOSTR2:
			if (ip[1] > 2)
				goto bad_operand;
			break;
		case OP_CONSTS:
			if (al_u16(ip + 1) >= p->nstrings)
				goto bad_operand;
			break;
		case OP_TOSTRX: {
			uint32_t pos = 0;
			if (al_u16(ip + 1) >= p->nstrings)
				goto bad_operand;
			const struct al_str *s = p->strings[al_u16(ip + 1)];
			if (!desc_ok(p, s, &pos) || pos != s->len)
				goto bad_operand;
			break;
		}
		case OP_GLOAD: case OP_GLOADR: case OP_GSTORE: case OP_GSTORER:
			if (al_u16(ip + 1) >= p->nglobals)
				goto bad_operand;
			break;
		case OP_NEWARR:
			if (ip[1] > K_REF)
				goto bad_operand;
			break;
		case OP_NEWST:
			if (al_u16(ip + 1) >= p->nstructs)
				goto bad_operand;
			break;
		case OP_CALL: {
			uint16_t fn = al_u16(ip + 1);
			if (fn >= p->nfuncs)
				goto bad_operand;
			pops = p->funcs[fn].nparams;
			pushes = p->funcs[fn].returns;
			break;
		}
		case OP_CALLB: {
			int min, max;
			bool value;
			if (ip[1] >= B_COUNT)
				goto bad_operand;
			builtin_shape(ip[1], &min, &max, &value);
			if (ip[2] < min || ip[2] > max)
				goto bad_operand;
			pops = ip[2];
			pushes = value;
			break;
		}
		case OP_RET:
		case OP_RETV:
			if (f->returns != (op == OP_RETV)) {
				fail(err, errlen, "wrong return at %u", (unsigned)pc);
				goto done;
			}
			falls = false;
			break;
		}
		if (info->fmt[0] == 'j') {
			target = (int64_t)next + (int16_t)al_u16(ip + 1);
			if (target < 0 || target >= len || depth[target] == -2) {
				fail(err, errlen, "bad jump at %u", (unsigned)pc);
				goto done;
			}
			falls = op != OP_JMP && op != OP_LOOP;
		}

		if (d < pops || d - pops + pushes > f->max_stack) {
			fail(err, errlen, "stack out of bounds at %u", (unsigned)pc);
			goto done;
		}
		tdepth = d - pops + pushes;
		if (op == OP_JFK || op == OP_JTK)
			tdepth = d;		/* the value stays when jumping */

		for (int edge = 0; edge < 2; edge++) {
			int64_t to = edge ? target : (falls ? (int64_t)next : -1);
			int nd = edge ? tdepth : d - pops + pushes;
			if (to < 0)
				continue;
			if (to >= len) {
				fail(err, errlen, "runs off the end at %u", (unsigned)pc);
				goto done;
			}
			if (depth[to] == -1) {
				depth[to] = nd;
				work[nwork++] = to;
			} else if (depth[to] != nd) {
				fail(err, errlen, "inconsistent stack at %u", (unsigned)to);
				goto done;
			}
		}
	}
	status = 0;
	goto done;

bad_operand:
	fail(err, errlen, "bad operand at %u", (unsigned)pc);
done:
	port_free(depth);
	port_free(work);
	if (status && err[0] && strlen(err) + 32 < errlen) {
		char name[64];
		const struct al_str *n = p->strings[f->name];
		snprintf(name, sizeof(name), "%.40s", n->data);
		size_t used = strlen(err);
		snprintf(err + used, errlen - used, " in %s", name);
	}
	return status;
}

int al_load(struct al_vm *vm, const uint8_t *data, size_t len, char *err, size_t errlen)
{
	struct al_prog *p = &vm->prog;
	struct reader r = { data, len, false };

	err[0] = '\0';
	if (len < AL_HEADER_SIZE || memcmp(data, AL_MAGIC, 3))
		return fail(err, errlen, "not an a executable");
	if (data[3] != AL_VERSION)
		return fail(err, errlen, "executable format version %u, this system runs version %u "
			    "(compile it again with ac)", data[3], AL_VERSION);
	if (len > AL_MAX_IMAGE)
		return fail(err, errlen, "executable too large");
	if (len < AL_HEADER_SIZE + AL_TRAILER_SIZE)
		return fail(err, errlen, "damaged executable (too short)");
	len -= AL_TRAILER_SIZE;
	if (al_crc32(data, len) != al_u32(data + len))
		return fail(err, errlen, "damaged executable (checksum mismatch)");
	r.left = len;
	take(&r, 4);
	p->nstrings = rd16(&r);
	p->nstructs = rd16(&r);
	p->nglobals = rd16(&r);
	p->nfuncs = rd16(&r);
	p->main = rd16(&r);
	p->init = rd16(&r);
	p->source = rd16(&r);
	rd16(&r);
	uint32_t nfields = rd32(&r);
	uint32_t nlines = rd32(&r);
	p->code_len = rd32(&r);

	if (!p->nstrings || p->source >= p->nstrings || p->main >= p->nfuncs ||
	    (p->init != AL_NO_FUNC && p->init >= p->nfuncs) || nfields > len || nlines > len ||
	    p->code_len > len)
		return fail(err, errlen, "damaged executable header");

	p->strings = port_alloc(p->nstrings * sizeof(*p->strings));
	p->structs = port_alloc((p->nstructs + 1) * sizeof(*p->structs));
	p->fields = port_alloc((nfields + 1) * sizeof(*p->fields));
	p->global_kinds = port_alloc(p->nglobals + 1);
	p->funcs = port_alloc(p->nfuncs * sizeof(*p->funcs));
	if (!p->strings || !p->structs || !p->fields || !p->global_kinds || !p->funcs)
		return fail(err, errlen, "out of memory");
	memset(p->strings, 0, p->nstrings * sizeof(*p->strings));

	for (uint32_t i = 0; i < p->nstrings; i++) {
		uint32_t n = rd32(&r);
		const uint8_t *s = take(&r, n);
		if (!s)
			return fail(err, errlen, "damaged string table");
		p->strings[i] = al_str_new(vm, (const char *)s, n);
		if (!p->strings[i])
			return fail(err, errlen, "out of memory");
	}

	uint32_t first = 0;
	for (uint32_t i = 0; i < p->nstructs; i++) {
		struct al_sdef *s = &p->structs[i];
		s->name = rd16(&r);
		s->nfields = rd16(&r);
		s->first = first;
		first += s->nfields;
		if (s->name >= p->nstrings || s->nfields > 255 || first > nfields)
			return fail(err, errlen, "damaged struct table");
	}
	if (first != nfields)
		return fail(err, errlen, "damaged struct table");
	for (uint32_t i = 0; i < nfields; i++) {
		struct al_field *f = &p->fields[i];
		uint32_t pos = 0;
		f->name = rd16(&r);
		f->desc = rd16(&r);
		if (r.bad || f->name >= p->nstrings || f->desc >= p->nstrings ||
		    !desc_ok(p, p->strings[f->desc], &pos) || pos != p->strings[f->desc]->len)
			return fail(err, errlen, "damaged field table");
		f->kind = desc_kind(p->strings[f->desc]);
	}
	for (uint32_t i = 0; i < p->nglobals; i++) {
		const uint8_t *k = take(&r, 1);
		if (!k || *k > K_REF)
			return fail(err, errlen, "damaged global table");
		p->global_kinds[i] = *k;
	}

	for (uint32_t i = 0; i < p->nfuncs; i++) {
		struct al_func *f = &p->funcs[i];
		f->code = rd32(&r);
		f->len = rd32(&r);
		f->lines = rd32(&r);
		f->nlines = rd32(&r);
		f->name = rd16(&r);
		f->max_stack = rd16(&r);
		f->nparams = rd8(&r);
		f->nlocals = rd8(&r);
		f->returns = rd8(&r);
		rd8(&r);
		if (r.bad || !f->len || f->code > p->code_len || f->len > p->code_len - f->code ||
		    f->lines > nlines || f->nlines > nlines - f->lines || f->name >= p->nstrings ||
		    f->nparams > f->nlocals || f->returns > 1)
			return fail(err, errlen, "damaged function table");
	}
	p->lines = take(&r, (size_t)nlines * 4);
	p->code = take(&r, p->code_len);
	if (r.bad || r.left)
		return fail(err, errlen, "damaged executable (size mismatch)");
	if (p->funcs[p->main].nparams > 1 || (p->init != AL_NO_FUNC && p->funcs[p->init].nparams))
		return fail(err, errlen, "damaged executable (entry points)");
	if (!functions_apart(p))
		return fail(err, errlen, "damaged executable (functions overlap)");

	for (uint16_t i = 0; i < p->nfuncs; i++)
		if (verify(vm, i, err, errlen))
			return -1;
	return 0;
}

/* ------------------------------------------------------------ disassembler */

int al_line_of(const struct al_prog *p, uint16_t fn, const uint8_t *ip)
{
	const struct al_func *f = &p->funcs[fn];
	uint32_t pc = ip - (p->code + f->code);
	int line = 0;

	for (uint32_t i = 0; i < f->nlines; i++) {
		const uint8_t *e = p->lines + (f->lines + i) * 4;
		if (al_u16(e) > pc)
			break;
		line = al_u16(e + 2);
	}
	return line;
}

void al_disasm(struct al_vm *vm)
{
	const struct al_prog *p = &vm->prog;
	char line[160];

	for (uint16_t fi = 0; fi < p->nfuncs; fi++) {
		const struct al_func *f = &p->funcs[fi];
		const uint8_t *code = p->code + f->code;
		int n = snprintf(line, sizeof(line), "\n%s: params %u, locals %u, stack %u, %u bytes\n",
				 p->strings[f->name]->data, f->nparams, f->nlocals, f->max_stack,
				 (unsigned)f->len);
		port_write(1, line, n);
		int last = -1;
		for (uint32_t pc = 0; pc < f->len; pc += al_opinfo[code[pc]].size) {
			const struct al_opinfo *info = &al_opinfo[code[pc]];
			const uint8_t *o = code + pc + 1;
			int ln = al_line_of(p, fi, code + pc);
			if (ln != last)
				n = snprintf(line, sizeof(line), "%5d %5u  %-9s", ln, (unsigned)pc, info->name);
			else
				n = snprintf(line, sizeof(line), "      %5u  %-9s", (unsigned)pc, info->name);
			last = ln;
			for (const char *fmt = info->fmt; *fmt && n < 120; fmt++) {
				switch (*fmt) {
				case 'b':
					n += snprintf(line + n, sizeof(line) - n, " %u", *o);
					o += 1;
					break;
				case 'c':
					n += snprintf(line + n, sizeof(line) - n, " %d", (int8_t)*o);
					o += 1;
					break;
				case 'w':
					n += snprintf(line + n, sizeof(line) - n, " %u", al_u16(o));
					if (code[pc] == OP_CONSTS || code[pc] == OP_TOSTRX)
						n += snprintf(line + n, sizeof(line) - n, " \"%.40s\"",
							      p->strings[al_u16(o)]->data);
					else if (code[pc] == OP_CALL)
						n += snprintf(line + n, sizeof(line) - n, " %.40s",
							      p->strings[p->funcs[al_u16(o)].name]->data);
					o += 2;
					break;
				case 'j':
					n += snprintf(line + n, sizeof(line) - n, " -> %d",
						      (int)(pc + info->size + (int16_t)al_u16(o)));
					o += 2;
					break;
				case 'i':
					n += snprintf(line + n, sizeof(line) - n, " %d", (int)(int32_t)al_u32(o));
					o += 4;
					break;
				case 'f': {
					float v;
					uint32_t u = al_u32(o);
					memcpy(&v, &u, 4);
					n += snprintf(line + n, sizeof(line) - n, " %g", v);
					o += 4;
					break;
				}
				}
			}
			if (code[pc] == OP_CALLB)
				n += snprintf(line + n, sizeof(line) - n, "  (%s)", al_builtins[code[pc + 1]].name);
			line[n++] = '\n';
			port_write(1, line, n);
		}
	}
}
