/*
 * The virtual machine: an operand stack interpreter with typed instructions.
 *
 * Locals live at the bottom of each frame's part of the stack. Every value
 * on the operand stack owns its reference; instructions ending in R move or
 * copy references, the others handle plain numbers. The loop dispatches
 * with computed goto when the compiler has it.
 *
 * On a runtime error nothing is unwound: the error is reported and the
 * program ends, and its memory goes back with the process.
 */
#include <math.h>
#include <string.h>

#include "pico.h"
#include "port.h"

#define POLL_EVERY	1024	/* backward jumps and calls between Ctrl-C checks */

/*
 * CALLB checks that a damaged executable does not hand a built-in a number
 * or the wrong object where its signature wants a str, a File or an array.
 * The signatures are read once, here: vm->bargs has a nibble for each
 * argument from the lowest, the object type it must have plus one, or 0
 * for anything. Eight is plenty: no built-in that checks takes more than
 * three arguments.
 */
static void builtin_args(struct pico_vm *vm)
{
	for (int b = 0; b < B_COUNT; b++) {
		uint32_t want = 0;
		int i = 0;

		for (const char *sig = pico_builtins[b].sig; *sig && *sig != ':'; sig++) {
			int type = *sig == 's' ? OT_STR : *sig == 'F' ? OT_FILE : *sig == 'S' ? OT_ARRAY : -1;

			if (*sig == '?')
				continue;
			if (type >= 0 && i < 8)
				want |= (uint32_t)(type + 1) << (4 * i);
			i++;
		}
		vm->bargs[b] = want;
	}
}

struct pico_vm *pico_vm_new(void)
{
	struct pico_vm *vm = port_alloc(sizeof(*vm));

	if (!vm)
		return NULL;
	memset(vm, 0, sizeof(*vm));
	builtin_args(vm);
	vm->rng = (uint32_t)port_uptime_us() * 2654435761u | 1;
	vm->line_buffered = port_isatty(1);
	vm->empty = pico_str_new(vm, "", 0);
	for (int i = 0; i < 3; i++)
		vm->std[i] = pico_file_new(vm, i, true);
	if (!vm->empty || !vm->std[0] || !vm->std[1] || !vm->std[2]) {
		pico_heap_release(vm);
		port_free(vm);
		return NULL;
	}
	return vm;
}

void pico_vm_free(struct pico_vm *vm, bool leak_check)
{
	struct pico_prog *p = &vm->prog;

	pico_flush(vm);
	if (vm->globals) {
		for (uint32_t i = 0; i < p->nglobals; i++)
			if (KIND_IS_REF(p->global_kinds[i]))
				pico_decref(vm, vm->globals[i].o);
		port_free(vm->globals);
	}
	if (p->strings) {
		/* loading may have stopped part way: some entries can be NULL */
		for (uint32_t i = 0; i < p->nstrings; i++)
			if (p->strings[i])
				pico_decref(vm, &p->strings[i]->h);
		port_free(p->strings);
	}
	for (int i = 0; i < 3; i++)
		if (vm->std[i])
			pico_decref(vm, &vm->std[i]->h);
	if (vm->empty)
		pico_decref(vm, &vm->empty->h);
	if (leak_check && vm->heap.objects)
		pico_eprintf("leak check: %zu objects still allocated (reference cycles?)\n", vm->heap.objects);
	port_free(p->structs);
	port_free(p->fields);
	port_free(p->global_kinds);
	port_free(p->funcs);
	port_free(p->image);
	port_free(vm->stack);
	port_free(vm->frames);
	pico_heap_release(vm);
	port_free(vm);
}

/*
 * Room for `need` more values above the first `used`. The stack may move, so
 * the caller turns its pointers into offsets first: taking the address of sp
 * and bp instead would keep them out of registers in the whole interpreter.
 */
static bool grow_stack(struct pico_vm *vm, size_t used, size_t need)
{
	size_t cap = vm->stack_cap;

	while (cap - used < need) {
		cap *= 2;
		if (cap > PICO_MAX_STACK)
			return false;
	}
	union pico_val *stack = port_alloc(cap * sizeof(*stack));
	if (!stack)
		return false;
	memcpy(stack, vm->stack, used * sizeof(*stack));
	for (uint32_t i = 0; i < vm->nframes; i++)
		vm->frames[i].bp = stack + (vm->frames[i].bp - vm->stack);
	port_free(vm->stack);
	vm->stack = stack;
	vm->stack_cap = cap;
	return true;
}

static bool grow_frames(struct pico_vm *vm)
{
	uint32_t cap = vm->frames_cap * 2;

	if (cap > PICO_MAX_CALL_DEPTH)
		cap = PICO_MAX_CALL_DEPTH;
	if (cap <= vm->frames_cap)
		return false;
	struct pico_frame *frames = port_realloc(vm->frames, cap * sizeof(*frames));
	if (!frames)
		return false;
	vm->frames = frames;
	vm->frames_cap = cap;
	return true;
}

/* len() of a local, for the fused compares: false if it has no length. */
static inline bool qlen(const struct pico_obj *o, int32_t *len)
{
	if (!o || (o->type != OT_STR && o->type != OT_ARRAY))
		return false;
	*len = o->type == OT_STR ? ((const struct pico_str *)o)->len : ((const struct pico_array *)o)->len;
	return true;
}

static const char *qlen_error(const struct pico_obj *o)
{
	return o ? "damaged executable (len)" : "len() of a null array";
}

/*
 * What an instruction does that may come between a CONCAT and the store of
 * the string it makes, as concat_target needs to know: its size, the local
 * or global it reads (-1 for none) and whether it calls a function of the
 * program. 0 for any other instruction. None of these jumps or reads a
 * variable but its operand; a built-in sees only its arguments, and a
 * function cannot see its caller's locals. Quickened code is read as what
 * it was.
 */
static int concat_step(const uint8_t *ip, int *local, int *global, bool *call)
{
	*local = *global = -1;
	switch (pico_unfused(*ip)) {
	case OP_CONCAT:
	case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
	case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_NEG:
	case OP_ADDF: case OP_SUBF: case OP_MULF: case OP_DIVF: case OP_MODF: case OP_NEGF:
	case OP_I2F: case OP_I2F2: case OP_F2I:
	case OP_IDX: case OP_IDXR: case OP_STRIDX: case OP_LENA: case OP_LENS:
		return 1;
	case OP_CONST8:
	case OP_TOSTR:
	case OP_TOSTR2:
	case OP_GETF:
	case OP_GETFR:
		return 2;
	case OP_CONSTS:
	case OP_TOSTRX:
	case OP_CALLB:
		return 3;
	case OP_CONST32:
	case OP_CONSTF:
		return 5;
	case OP_LOAD:
	case OP_LOADR:
	case OP_IDXL:
	case OP_IDXLR:
	case OP_LENL:
		*local = ip[1];
		return 2;
	case OP_GETFL:
	case OP_GETFLR:
		*local = ip[1];
		return 3;
	case OP_GLOAD:
	case OP_GLOADR:
		*global = pico_u16(ip + 1);
		return 3;
	case OP_CALL:
		*call = true;
		return 3;
	}
	return 0;
}

#define CONCAT_LOOKAHEAD	24	/* instructions */

/*
 * The variable that the string a CONCAT at ip makes, or one made from it,
 * is surely stored in next, with nothing reading the variable before:
 * STORER or GSTORER after a few of the instructions concat_step allows, as
 * in s = s + x or s = s + str(n) + "\n". A call may come before the store
 * of a local, not of a global. NULL if there is none.
 */
static __attribute__((noinline)) union pico_val *concat_target(struct pico_vm *vm, const uint8_t *ip,
								union pico_val *bp)
{
	const uint8_t *p = ip + 1, *end;
	int local = -1, global = -1, l, g, size;
	bool call = false;

	for (int i = 0; i < CONCAT_LOOKAHEAD; i++) {
		if (*p == OP_STORER) {
			local = p[1];
			break;
		}
		if (*p == OP_GSTORER) {
			global = pico_u16(p + 1);
			break;
		}
		if (!(size = concat_step(p, &l, &g, &call)))
			return NULL;
		p += size;
	}
	if ((local < 0 && global < 0) || (global >= 0 && call))
		return NULL;
	end = p;
	for (p = ip + 1; p < end; p += size) {
		size = concat_step(p, &l, &g, &call);
		if ((local >= 0 && l == local) || (global >= 0 && g == global))
			return NULL;
	}
	return local >= 0 ? &bp[local] : &vm->globals[global];
}

/* 0 when two strings are the same; ones of different lengths are not. */
static inline int str_differ(const struct pico_str *a, const struct pico_str *b)
{
	return a->len != b->len || memcmp(a->data, b->data, a->len);
}

#define INCREF(o)	do { struct pico_obj *o_ = (o); if (o_) o_->refs++; } while (0)
#define DECREF(o)	do { struct pico_obj *o_ = (o); if (o_ && --o_->refs == 0) pico_obj_free(vm, o_); } while (0)
#define STR(v)		((struct pico_str *)(v).o)
#define ARR(v)		((struct pico_array *)(v).o)
#define OBJ(v)		((struct pico_struct *)(v).o)
/*
 * Compiled code always has the right type here; a damaged or hand-made
 * executable might not, and this is what keeps it from following a number
 * as if it were a pointer.
 */
#define OK(p, want)	((p) && (p)->h.type == (want))
#define JUMP()		(ip + 3 + (int16_t)pico_u16(ip + 1))

/*
 * Run function fn with its arguments already at the bottom of the stack.
 * Returns 0, or -1 after an error, exit() or an interrupt.
 */
int pico_vm_exec(struct pico_vm *vm, uint16_t fn, union pico_val *result)
{
	const struct pico_prog *prog = &vm->prog;
	const struct pico_func *funcs = prog->funcs;
	struct pico_str **strings = prog->strings;
	union pico_val *globals = vm->globals;
	const uint8_t *code = prog->code;
	const struct pico_func *f = &funcs[fn];
	union pico_val *bp = vm->stack, *sp = vm->stack + f->nparams;
	const uint8_t *ip;
	int budget = POLL_EVERY;

	if (vm->stack_cap - f->nparams < (size_t)f->nlocals + f->max_stack &&
	    !grow_stack(vm, f->nparams, f->nlocals + f->max_stack)) {
		pico_eprintf("out of memory\n");
		return -1;
	}
	bp = vm->stack;
	sp = bp + f->nparams;
	for (uint32_t i = f->nparams; i < f->nlocals; i++)
		(sp++)->o = NULL;
	if (vm->nframes == vm->frames_cap && !grow_frames(vm)) {
		pico_eprintf("out of memory\n");
		return -1;
	}
	vm->frames[vm->nframes++] = (struct pico_frame){ .ip = NULL, .bp = bp, .fn = fn };
	ip = code + f->code;

#if defined(__GNUC__)
	static const void *const labels[OP_TOTAL] = {
#define X(name, fmt, pop, push) [OP_##name] = &&L_##name,
		PICO_OPS(X)
#undef X
#define X(form, b, cc, op) [Q_##form##b##cc] = &&L_Q_##form##b##cc,
		PICO_QCMPS(X)
#undef X
#define X(op) [Q_LL##op] = &&L_Q_LL##op, [Q_LK##op] = &&L_Q_LK##op,			\
	      [Q_LL##op##_ST] = &&L_Q_LL##op##_ST, [Q_LK##op##_ST] = &&L_Q_LK##op##_ST,	\
	      [Q_##op##_ST] = &&L_Q_##op##_ST,
		PICO_QINT(X)
#undef X
#define X(op) [Q_LL##op] = &&L_Q_LL##op, [Q_LL##op##_ST] = &&L_Q_LL##op##_ST,		\
	      [Q_##op##_ST] = &&L_Q_##op##_ST,
		PICO_QFLOAT(X)
#undef X
#define X(op) [Q_L_##op] = &&L_Q_L_##op,
		PICO_QLOAD(X)
#undef X
	};
#define CASE(op)	L_##op:
#define QCASE(op)	L_##op:
#define DISPATCH()	goto *labels[*ip]
#else
#define CASE(op)	case OP_##op: L_##op:
#define QCASE(op)	case op: L_##op:
#define DISPATCH()	goto dispatch
#endif
#define NEXT(n)		do { ip += (n); DISPATCH(); } while (0)
#define THROW(...)	do { vm->ip = ip; vm->fn = fn; pico_error(vm, __VA_ARGS__); goto fail; } while (0)
#define POLL()								\
	do {								\
		if (--budget <= 0) {					\
			budget = POLL_EVERY;				\
			if (port_interrupted()) {			\
				vm->ip = ip;				\
				vm->fn = fn;				\
				goto interrupted;			\
			}						\
		}							\
	} while (0)
#define INT_BINOP(op, expr)						\
	CASE(op) {							\
		int32_t a = sp[-2].i, b = sp[-1].i;			\
		(void)a; (void)b;					\
		sp[-2].i = (expr);					\
		sp--;							\
		NEXT(1);						\
	}
#define FLOAT_BINOP(op, expr)						\
	CASE(op) {							\
		float a = sp[-2].f, b = sp[-1].f;			\
		sp[-2].f = (expr);					\
		sp--;							\
		NEXT(1);						\
	}
#define FLOAT_CMP(op, expr)						\
	CASE(op) {							\
		float a = sp[-2].f, b = sp[-1].f;			\
		sp[-2].i = (expr);					\
		sp--;							\
		NEXT(1);						\
	}
#define STR_CMP(op, cmp, expr)						\
	CASE(op) {							\
		struct pico_str *a = STR(sp[-2]), *b = STR(sp[-1]);	\
		if (!OK(a, OT_STR) || !OK(b, OT_STR))			\
			THROW("null string");				\
		int r = cmp(a, b);					\
		sp[-2].i = (expr);					\
		sp--;							\
		DECREF(&a->h);						\
		DECREF(&b->h);						\
		NEXT(1);						\
	}
#define CMP_JUMP(op, cond)						\
	CASE(op) {							\
		sp -= 2;						\
		int32_t a = sp[0].i, b = sp[1].i;			\
		ip = (cond) ? JUMP() : ip + 3;				\
		DISPATCH();						\
	}
#define CMP_LOOP(op, cond)						\
	CASE(op) {							\
		sp -= 2;						\
		int32_t a = sp[0].i, b = sp[1].i;			\
		if (cond) {						\
			ip = JUMP();					\
			POLL();						\
		} else {						\
			ip += 3;					\
		}							\
		DISPATCH();						\
	}

#if defined(__GNUC__)
	DISPATCH();
#else
dispatch:
	switch (*ip) {
#endif

	CASE(NOP)
		NEXT(1);
	CASE(CONST8)
		(sp++)->i = (int8_t)ip[1];
		NEXT(2);
	CASE(CONST32)
		(sp++)->i = (int32_t)pico_u32(ip + 1);
		NEXT(5);
	CASE(CONSTF) {
		uint32_t u = pico_u32(ip + 1);
		memcpy(&sp->f, &u, 4);
		sp++;
		NEXT(5);
	}
	CASE(CONSTS) {
		struct pico_str *s = strings[pico_u16(ip + 1)];
		s->h.refs++;
		(sp++)->o = &s->h;
		NEXT(3);
	}
	CASE(NULLV)
		(sp++)->o = NULL;
		NEXT(1);
	CASE(STDFILE)
		vm->std[ip[1]]->h.refs++;
		(sp++)->o = &vm->std[ip[1]]->h;
		NEXT(2);
	CASE(LOAD)
		*sp++ = bp[ip[1]];
		NEXT(2);
	CASE(LOADR) {
		union pico_val v = bp[ip[1]];
		INCREF(v.o);
		*sp++ = v;
		NEXT(2);
	}
	CASE(STORE)
		bp[ip[1]] = *--sp;
		NEXT(2);
	CASE(STORER) {
		struct pico_obj *old = bp[ip[1]].o;
		bp[ip[1]] = *--sp;
		DECREF(old);
		NEXT(2);
	}
	CASE(CLEARR) {
		struct pico_obj *old = bp[ip[1]].o;
		bp[ip[1]].o = NULL;
		DECREF(old);
		NEXT(2);
	}
	CASE(INCL)
		bp[ip[1]].i = (int32_t)((uint32_t)bp[ip[1]].i + (uint32_t)(int32_t)(int8_t)ip[2]);
		NEXT(3);
	CASE(CATL) {
		struct pico_str *s = STR(bp[ip[1]]), *t = STR(sp[-1]), *r;
		if (!OK(s, OT_STR) || !OK(t, OT_STR))
			THROW(s && t ? "not a string (damaged executable)" : "null string");
		if (s->h.refs == 1) {
			r = pico_str_add(vm, s, t->data, t->len, true);
		} else {
			r = pico_str_concat(vm, s, t);
			if (r)
				s->h.refs--;	/* the local's reference moves to r; s stays alive */
		}
		if (!r)
			THROW("out of memory");
		bp[ip[1]].o = &r->h;
		sp--;
		DECREF(&t->h);
		NEXT(2);
	}
	CASE(GLOAD)
		*sp++ = globals[pico_u16(ip + 1)];
		NEXT(3);
	CASE(GLOADR) {
		union pico_val v = globals[pico_u16(ip + 1)];
		INCREF(v.o);
		*sp++ = v;
		NEXT(3);
	}
	CASE(GSTORE)
		globals[pico_u16(ip + 1)] = *--sp;
		NEXT(3);
	CASE(GSTORER) {
		struct pico_obj *old = globals[pico_u16(ip + 1)].o;
		globals[pico_u16(ip + 1)] = *--sp;
		DECREF(old);
		NEXT(3);
	}
	CASE(POP)
		sp--;
		NEXT(1);
	CASE(POPR)
		sp--;
		DECREF(sp->o);
		NEXT(1);
	CASE(DUP)
		sp[0] = sp[-1];
		sp++;
		NEXT(1);
	CASE(DUPR)
		sp[0] = sp[-1];
		INCREF(sp[0].o);
		sp++;
		NEXT(1);
	CASE(DUP2R)
		sp[0] = sp[-2];
		sp[1] = sp[-1];
		INCREF(sp[0].o);
		sp += 2;
		NEXT(1);

	INT_BINOP(ADD, (int32_t)((uint32_t)a + (uint32_t)b))
	INT_BINOP(SUB, (int32_t)((uint32_t)a - (uint32_t)b))
	INT_BINOP(MUL, (int32_t)((uint32_t)a * (uint32_t)b))
	INT_BINOP(BAND, a & b)
	INT_BINOP(BOR, a | b)
	INT_BINOP(BXOR, a ^ b)
	INT_BINOP(SHL, (int32_t)((uint32_t)a << (b & 31)))
	INT_BINOP(SHR, a >> (b & 31))
	INT_BINOP(EQ, a == b)
	INT_BINOP(NE, a != b)
	INT_BINOP(LT, a < b)
	INT_BINOP(LE, a <= b)
	INT_BINOP(GT, a > b)
	INT_BINOP(GE, a >= b)
	CASE(DIV) {
		int32_t a = sp[-2].i, b = sp[-1].i;
		if (!b)
			THROW("division by zero");
		sp[-2].i = b == -1 ? (int32_t)(0u - (uint32_t)a) : a / b;
		sp--;
		NEXT(1);
	}
	CASE(MOD) {
		int32_t a = sp[-2].i, b = sp[-1].i;
		if (!b)
			THROW("division by zero");
		sp[-2].i = b == -1 ? 0 : a % b;
		sp--;
		NEXT(1);
	}
	CASE(NEG)
		sp[-1].i = (int32_t)(0u - (uint32_t)sp[-1].i);
		NEXT(1);
	CASE(BNOT)
		sp[-1].i = ~sp[-1].i;
		NEXT(1);
	CASE(NOT)
		sp[-1].i = !sp[-1].i;
		NEXT(1);

	FLOAT_BINOP(ADDF, a + b)
	FLOAT_BINOP(SUBF, a - b)
	FLOAT_BINOP(MULF, a * b)
	FLOAT_BINOP(DIVF, a / b)
	FLOAT_BINOP(MODF, fmodf(a, b))
	CASE(NEGF)
		sp[-1].f = -sp[-1].f;
		NEXT(1);
	FLOAT_CMP(EQF, a == b)
	FLOAT_CMP(NEF, a != b)
	FLOAT_CMP(LTF, a < b)
	FLOAT_CMP(LEF, a <= b)
	FLOAT_CMP(GTF, a > b)
	FLOAT_CMP(GEF, a >= b)

	/*
	 * When nobody can see the left string change, b is appended to it
	 * where it is, as CATL does, instead of copying both: when the stack
	 * holds its only reference (the result of a concatenation or a call),
	 * or when the one other is the variable that the result is about to
	 * be stored in (concat_target), as in s = s + x, which would otherwise
	 * copy all of s each time round a loop. That variable's reference
	 * passes to the result and the store finds it empty, so no one ever
	 * holds a string whose length changed under it.
	 */
	CASE(CONCAT) {
		struct pico_str *a = STR(sp[-2]), *b = STR(sp[-1]), *r;
		union pico_val *var = NULL;
		if (!OK(a, OT_STR) || !OK(b, OT_STR))
			THROW(a && b ? "not a string (damaged executable)" : "null string");
		if (a->h.refs == 2 && a->h.kind != PICO_CONST && a != b &&
		    (var = concat_target(vm, ip, bp)) && var->o != &a->h)
			var = NULL;
		if ((a->h.refs == 1 || var) && a != b) {
			if (!(r = pico_str_add(vm, a, b->data, b->len, var != NULL)))
				THROW("out of memory");
			if (var) {
				var->o = NULL;
				r->h.refs--;
			}
		} else {
			if (!(r = pico_str_concat(vm, a, b)))
				THROW("out of memory");
			DECREF(&a->h);
		}
		sp[-2].o = &r->h;
		sp--;
		DECREF(&b->h);
		NEXT(1);
	}
	STR_CMP(EQS, str_differ, r == 0)
	STR_CMP(NES, str_differ, r != 0)
	STR_CMP(LTS, pico_str_cmp, r < 0)
	STR_CMP(LES, pico_str_cmp, r <= 0)
	STR_CMP(GTS, pico_str_cmp, r > 0)
	STR_CMP(GES, pico_str_cmp, r >= 0)
	CASE(EQR)
	CASE(NER) {
		struct pico_obj *a = sp[-2].o, *b = sp[-1].o;
		sp[-2].i = (a == b) == (*ip == OP_EQR);
		sp--;
		DECREF(a);
		DECREF(b);
		NEXT(1);
	}

	CASE(I2F)
		sp[-1].f = (float)sp[-1].i;
		NEXT(1);
	CASE(I2F2)
		sp[-2].f = (float)sp[-2].i;
		NEXT(1);
	CASE(F2I)
		sp[-1].i = pico_float_to_int(sp[-1].f);
		NEXT(1);
	CASE(TOSTR)
	CASE(TOSTR2) {
		union pico_val *v = *ip == OP_TOSTR ? &sp[-1] : &sp[-2];
		struct pico_str *s = pico_tostr(vm, *v, ip[1]);
		if (!s)
			THROW("out of memory");
		v->o = &s->h;
		NEXT(2);
	}
	CASE(TOSTRX) {
		struct pico_str *s = pico_tostr_desc(vm, sp[-1], strings[pico_u16(ip + 1)]);
		if (!s)
			THROW("out of memory");
		DECREF(sp[-1].o);
		sp[-1].o = &s->h;
		NEXT(3);
	}

	CASE(JMP)
		ip = JUMP();
		DISPATCH();
	CASE(JF)
		sp--;
		ip = !sp->i ? JUMP() : ip + 3;
		DISPATCH();
	CASE(JT)
		sp--;
		ip = sp->i ? JUMP() : ip + 3;
		DISPATCH();
	CASE(JFK)
		if (!sp[-1].i) {
			ip = JUMP();
		} else {
			sp--;
			ip += 3;
		}
		DISPATCH();
	CASE(JTK)
		if (sp[-1].i) {
			ip = JUMP();
		} else {
			sp--;
			ip += 3;
		}
		DISPATCH();
	CMP_JUMP(JEQ, a == b)
	CMP_JUMP(JNE, a != b)
	CMP_JUMP(JLT, a < b)
	CMP_JUMP(JLE, a <= b)
	CMP_JUMP(JGT, a > b)
	CMP_JUMP(JGE, a >= b)
	CASE(LOOP)
		ip = JUMP();
		POLL();
		DISPATCH();
	CASE(LOOPT)
		sp--;
		if (sp->i) {
			ip = JUMP();
			POLL();
		} else {
			ip += 3;
		}
		DISPATCH();
	CMP_LOOP(LOOPEQ, a == b)
	CMP_LOOP(LOOPNE, a != b)
	CMP_LOOP(LOOPLT, a < b)
	CMP_LOOP(LOOPLE, a <= b)
	CMP_LOOP(LOOPGT, a > b)
	CMP_LOOP(LOOPGE, a >= b)

	CASE(CALL) {
		uint16_t callee = pico_u16(ip + 1);
		const struct pico_func *cf = &funcs[callee];
		POLL();
		if (vm->nframes == vm->frames_cap && !grow_frames(vm)) {
			if (vm->frames_cap >= PICO_MAX_CALL_DEPTH)
				THROW("call depth limit (%d) reached; runaway recursion?", PICO_MAX_CALL_DEPTH);
			THROW("out of memory");
		}
		size_t need = (size_t)(cf->nlocals - cf->nparams) + cf->max_stack;
		if ((size_t)(vm->stack + vm->stack_cap - sp) < need) {
			size_t sp_at = sp - vm->stack, bp_at = bp - vm->stack;
			if (!grow_stack(vm, sp_at, need))
				THROW("stack overflow");
			sp = vm->stack + sp_at;
			bp = vm->stack + bp_at;
		}
		struct pico_frame *fr = &vm->frames[vm->nframes++];
		fr->ip = ip + 3;
		fr->bp = bp;
		fr->fn = fn;
		bp = sp - cf->nparams;
		for (uint32_t i = cf->nparams; i < cf->nlocals; i++)
			(sp++)->o = NULL;
		fn = callee;
		ip = code + cf->code;
		DISPATCH();
	}
	CASE(RET)
	CASE(RETV) {
		struct pico_frame *fr = &vm->frames[--vm->nframes];
		if (*ip == OP_RETV)
			*bp++ = sp[-1];
		sp = bp;
		ip = fr->ip;
		bp = fr->bp;
		fn = fr->fn;
		if (!ip)
			goto finished;
		DISPATCH();
	}
	CASE(CALLB) {
		union pico_val *args = sp - ip[2];
		uint32_t want = vm->bargs[ip[1]];
		vm->ip = ip;
		vm->fn = fn;
		for (int i = 0; want && i < ip[2]; i++, want >>= 4)
			if ((want & 15) && args[i].o && args[i].o->type != (want & 15) - 1)
				THROW("damaged executable (wrong argument type)");
		int r = pico_builtin_fns[ip[1]](vm, args, ip[2]);
		if (r < 0)
			goto fail;
		sp = args + r;
		NEXT(3);
	}

	CASE(NEWARR) {
		struct pico_array *a = pico_array_new(vm, ip[1], pico_u16(ip + 2));
		if (!a)
			THROW("out of memory");
		(sp++)->o = &a->h;
		NEXT(4);
	}
	CASE(APPEND)
		if (!OK(ARR(sp[-2]), OT_ARRAY))
			THROW("damaged executable (array expected)");
		if (!pico_array_push(vm, ARR(sp[-2]), sp[-1]))
			THROW("out of memory");
		sp--;
		NEXT(1);
	CASE(IDX)
	CASE(IDXR) {
		struct pico_array *a = ARR(sp[-2]);
		int32_t i = sp[-1].i;
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" : "null array");
		if ((uint32_t)i >= a->len)
			THROW("index %d out of range (length %u)", (int)i, (unsigned)a->len);
		union pico_val v = a->items[i];
		if (*ip == OP_IDXR)
			INCREF(v.o);
		sp--;
		sp[-1] = v;
		DECREF(&a->h);
		NEXT(1);
	}
	CASE(SETIDX)
	CASE(SETIDXR) {
		struct pico_array *a = ARR(sp[-3]);
		int32_t i = sp[-2].i;
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" : "null array");
		if ((uint32_t)i >= a->len)
			THROW("index %d out of range (length %u)", (int)i, (unsigned)a->len);
		union pico_val old = a->items[i];
		a->items[i] = sp[-1];
		sp -= 3;
		if (*ip == OP_SETIDXR)
			DECREF(old.o);
		DECREF(&a->h);
		NEXT(1);
	}
	CASE(IDXL) {
		struct pico_array *a = ARR(bp[ip[1]]);
		int32_t i = sp[-1].i;
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" : "null array");
		if ((uint32_t)i >= a->len)
			THROW("index %d out of range (length %u)", (int)i, (unsigned)a->len);
		sp[-1] = a->items[i];
		NEXT(2);
	}
	CASE(IDXLR) {
		struct pico_array *a = ARR(bp[ip[1]]);
		int32_t i = sp[-1].i;
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" : "null array");
		if ((uint32_t)i >= a->len)
			THROW("index %d out of range (length %u)", (int)i, (unsigned)a->len);
		sp[-1] = a->items[i];
		INCREF(sp[-1].o);
		NEXT(2);
	}
	CASE(SETIDXL)
	CASE(SETIDXLR) {
		struct pico_array *a = ARR(bp[ip[1]]);
		int32_t i = sp[-2].i;
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" : "null array");
		if ((uint32_t)i >= a->len)
			THROW("index %d out of range (length %u)", (int)i, (unsigned)a->len);
		union pico_val old = a->items[i];
		a->items[i] = sp[-1];
		sp -= 2;
		if (*ip == OP_SETIDXLR)
			DECREF(old.o);
		NEXT(2);
	}
	CASE(STRIDX) {
		struct pico_str *s = STR(sp[-2]);
		int32_t i = sp[-1].i;
		if (!OK(s, OT_STR))
			THROW(s ? "not a string (damaged executable)" : "null string");
		if ((uint32_t)i >= s->len)
			THROW("string index %d out of range (length %u)", (int)i, (unsigned)s->len);
		sp--;
		sp[-1].i = (uint8_t)s->data[i];
		DECREF(&s->h);
		NEXT(1);
	}
	CASE(LENA)
	CASE(LENS) {
		struct pico_obj *o = sp[-1].o;
		if (!o || (o->type != OT_STR && o->type != OT_ARRAY))
			THROW(o ? "damaged executable (len)" :
			      *ip == OP_LENA ? "len() of a null array" : "null string");
		sp[-1].i = o->type == OT_STR ? ((struct pico_str *)o)->len : ((struct pico_array *)o)->len;
		DECREF(o);
		NEXT(1);
	}
	CASE(LENL) {
		struct pico_obj *o = bp[ip[1]].o;
		if (!o || (o->type != OT_STR && o->type != OT_ARRAY))
			THROW(o ? "damaged executable (len)" : "len() of a null array");
		(sp++)->i = o->type == OT_STR ? ((struct pico_str *)o)->len : ((struct pico_array *)o)->len;
		NEXT(2);
	}
	CASE(PUSHA) {
		struct pico_array *a = ARR(sp[-2]);
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" :
			      "push() to a null array (give it a value first, as in int[] a = [])");
		if (!pico_array_push(vm, a, sp[-1]))
			THROW("out of memory");
		sp -= 2;
		DECREF(&a->h);
		NEXT(1);
	}
	CASE(PUSHL) {
		struct pico_array *a = ARR(bp[ip[1]]);
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" :
			      "push() to a null array (give it a value first, as in int[] a = [])");
		if (!pico_array_push(vm, a, sp[-1]))
			THROW("out of memory");
		sp--;
		NEXT(2);
	}
	CASE(POPA) {
		struct pico_array *a = ARR(sp[-1]);
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" : "pop() from a null array");
		if (!a->len)
			THROW("pop() from an empty array");
		sp[-1] = a->items[--a->len];
		DECREF(&a->h);
		NEXT(1);
	}

	CASE(NEWST) {
		struct pico_struct *s = pico_struct_new(vm, pico_u16(ip + 1));
		if (!s)
			THROW("out of memory");
		(sp++)->o = &s->h;
		NEXT(3);
	}
	CASE(SETFI)
	CASE(SETFIR) {
		struct pico_struct *s = OBJ(sp[-2]);
		if (!OK(s, OT_STRUCT) || ip[1] >= s->h.kind)
			THROW("bad struct field");
		union pico_val old = s->fields[ip[1]];
		s->fields[ip[1]] = sp[-1];
		sp--;
		if (*ip == OP_SETFIR)
			DECREF(old.o);
		NEXT(2);
	}
	CASE(GETF)
	CASE(GETFR) {
		struct pico_struct *s = OBJ(sp[-1]);
		if (!OK(s, OT_STRUCT))
			THROW(s ? "not a struct (damaged executable)" : "null struct (reading a field)");
		if (ip[1] >= s->h.kind)
			THROW("bad struct field");
		union pico_val v = s->fields[ip[1]];
		if (*ip == OP_GETFR)
			INCREF(v.o);
		sp[-1] = v;
		DECREF(&s->h);
		NEXT(2);
	}
	CASE(SETF)
	CASE(SETFR) {
		struct pico_struct *s = OBJ(sp[-2]);
		if (!OK(s, OT_STRUCT))
			THROW(s ? "not a struct (damaged executable)" : "null struct (setting a field)");
		if (ip[1] >= s->h.kind)
			THROW("bad struct field");
		union pico_val old = s->fields[ip[1]];
		s->fields[ip[1]] = sp[-1];
		sp -= 2;
		if (*ip == OP_SETFR)
			DECREF(old.o);
		DECREF(&s->h);
		NEXT(2);
	}
	CASE(GETFL)
	CASE(GETFLR) {
		struct pico_struct *s = OBJ(bp[ip[1]]);
		if (!OK(s, OT_STRUCT))
			THROW(s ? "not a struct (damaged executable)" : "null struct (reading a field)");
		if (ip[2] >= s->h.kind)
			THROW("bad struct field");
		union pico_val v = s->fields[ip[2]];
		if (*ip == OP_GETFLR)
			INCREF(v.o);
		*sp++ = v;
		NEXT(3);
	}
	CASE(SETFL)
	CASE(SETFLR) {
		struct pico_struct *s = OBJ(bp[ip[1]]);
		if (!OK(s, OT_STRUCT))
			THROW(s ? "not a struct (damaged executable)" : "null struct (setting a field)");
		if (ip[2] >= s->h.kind)
			THROW("bad struct field");
		union pico_val old = s->fields[ip[2]];
		s->fields[ip[2]] = sp[-1];
		sp--;
		if (*ip == OP_SETFLR)
			DECREF(old.o);
		NEXT(3);
	}

	/* ------------------------------------------------ fused (quicken.c) */

	/*
	 * Compare and branch. The b operand of "LOAD a; <b>; Jcc" sits at
	 * offset 3 of the sequence (LOAD is 2 bytes), and the jump right
	 * after b's instruction; an I form starts with a 3-byte INCL.
	 */
#define QB_L(v, at)	((v) = bp[ip[at]].i, 2)
#define QB_K(v, at)	((v) = (int8_t)ip[at], 2)
#define QB_W(v, at)	((v) = (int32_t)pico_u32(ip + (at)), 5)
#define QB_N(v, at)	(qlen(bp[ip[at]].o, &(v)) ? 2 : (ip += (at) - 1, -1))
#define QJ_J(taken)	((void)0)
#define QJ_P(taken)	do { if (taken) POLL(); } while (0)
#define QJ_I(taken)	QJ_P(taken)
#define QSTEP_J		0
#define QSTEP_P		0
#define QSTEP_I		3
#define X(form, b, cc, op)						\
	QCASE(Q_##form##b##cc) {					\
		int32_t x, y;						\
		if (QSTEP_##form)					\
			bp[ip[1]].i = (int32_t)((uint32_t)bp[ip[1]].i +	\
				(uint32_t)(int32_t)(int8_t)ip[2]);	\
		const uint8_t *s = ip + QSTEP_##form;			\
		x = bp[s[1]].i;						\
		int size = QB_##b(y, QSTEP_##form + 3);			\
		if (size < 0)						\
			THROW("%s", qlen_error(bp[s[3]].o));		\
		const uint8_t *j = s + 2 + size;			\
		bool taken = x op y;					\
		ip = taken ? j + 3 + (int16_t)pico_u16(j + 1) : j + 3;	\
		QJ_##form(taken);					\
		DISPATCH();						\
	}
	PICO_QCMPS(X)
#undef X

	/* Integer arithmetic: LOAD a; LOAD b or CONST8 k; op [; STORE c] */
#define QI_ADD(a, b)	(int32_t)((uint32_t)(a) + (uint32_t)(b))
#define QI_SUB(a, b)	(int32_t)((uint32_t)(a) - (uint32_t)(b))
#define QI_MUL(a, b)	(int32_t)((uint32_t)(a) * (uint32_t)(b))
#define X(op)								\
	QCASE(Q_LL##op)							\
		(sp++)->i = QI_##op(bp[ip[1]].i, bp[ip[3]].i);		\
		NEXT(5);						\
	QCASE(Q_LK##op)							\
		(sp++)->i = QI_##op(bp[ip[1]].i, (int8_t)ip[3]);	\
		NEXT(5);						\
	QCASE(Q_LL##op##_ST)						\
		bp[ip[6]].i = QI_##op(bp[ip[1]].i, bp[ip[3]].i);	\
		NEXT(7);						\
	QCASE(Q_LK##op##_ST)						\
		bp[ip[6]].i = QI_##op(bp[ip[1]].i, (int8_t)ip[3]);	\
		NEXT(7);						\
	QCASE(Q_##op##_ST)						\
		bp[ip[2]].i = QI_##op(sp[-2].i, sp[-1].i);		\
		sp -= 2;						\
		NEXT(3);
	PICO_QINT(X)
#undef X

	/* Float arithmetic: LOAD a; LOAD b; op [; STORE c] */
#define QF_ADDF(a, b)	((a) + (b))
#define QF_SUBF(a, b)	((a) - (b))
#define QF_MULF(a, b)	((a) * (b))
#define X(op)								\
	QCASE(Q_LL##op)							\
		(sp++)->f = QF_##op(bp[ip[1]].f, bp[ip[3]].f);		\
		NEXT(5);						\
	QCASE(Q_LL##op##_ST)						\
		bp[ip[6]].f = QF_##op(bp[ip[1]].f, bp[ip[3]].f);	\
		NEXT(7);						\
	QCASE(Q_##op##_ST)						\
		bp[ip[2]].f = QF_##op(sp[-2].f, sp[-1].f);		\
		sp -= 2;						\
		NEXT(3);
	PICO_QFLOAT(X)
#undef X

	/* LOAD a; X: push a, then X's own handler, without a dispatch */
#define X(op)								\
	QCASE(Q_L_##op)							\
		*sp++ = bp[ip[1]];					\
		ip += 2;						\
		goto L_##op;
	PICO_QLOAD(X)
#undef X


#if !defined(__GNUC__)
	}
#endif

finished:
	if (result)
		*result = vm->stack[0];
	return 0;

interrupted:
	pico_flush(vm);
	if (vm->raw) {
		port_tty_raw(false);
		vm->raw = false;
	}
	port_die_interrupted();
	vm->failed = true;
	vm->status = 130;
	return -1;

fail:
	return -1;
}

int pico_vm_run(struct pico_vm *vm, int argc, char **argv)
{
	struct pico_prog *p = &vm->prog;
	const struct pico_func *main_fn = &p->funcs[p->main];
	union pico_val result = { 0 };
	int status = 1;

	vm->globals = port_alloc((p->nglobals + 1) * sizeof(*vm->globals));
	vm->stack_cap = 256;
	vm->stack = port_alloc(vm->stack_cap * sizeof(*vm->stack));
	vm->frames_cap = 32;
	vm->frames = port_alloc(vm->frames_cap * sizeof(*vm->frames));
	if (!vm->globals || !vm->stack || !vm->frames) {
		pico_eprintf("out of memory\n");
		return 1;
	}
	for (uint32_t i = 0; i < p->nglobals; i++) {
		vm->globals[i].o = NULL;
		if (p->global_kinds[i] == K_STR) {
			vm->globals[i].o = &vm->empty->h;
			vm->empty->h.refs++;
		} else if (!KIND_IS_REF(p->global_kinds[i])) {
			vm->globals[i].i = 0;
		}
	}

	if (p->init != PICO_NO_FUNC && pico_vm_exec(vm, p->init, NULL))
		goto out;
	vm->nframes = 0;
	if (main_fn->nparams) {
		struct pico_array *args = pico_array_new(vm, K_STR, argc);
		if (!args) {
			pico_eprintf("out of memory\n");
			goto out;
		}
		for (int i = 0; i < argc; i++) {
			struct pico_str *s = pico_str_new(vm, argv[i], strlen(argv[i]));
			if (!s) {
				pico_eprintf("out of memory\n");
				pico_decref(vm, &args->h);
				goto out;
			}
			args->items[args->len++].o = &s->h;
		}
		vm->stack[0].o = &args->h;
	}
	if (pico_vm_exec(vm, p->main, &result))
		goto out;
	status = main_fn->returns ? result.i : 0;
out:
	pico_flush(vm);
	if (vm->raw) {
		port_tty_raw(false);
		vm->raw = false;
	}
	if (vm->halted)
		return vm->status;
	if (vm->failed)
		return vm->status ? vm->status : 1;
	return status;
}
