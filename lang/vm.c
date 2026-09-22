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

#include "al.h"
#include "port.h"

#define POLL_EVERY	1024	/* backward jumps and calls between Ctrl-C checks */

struct al_vm *al_vm_new(void)
{
	struct al_vm *vm = port_alloc(sizeof(*vm));

	if (!vm)
		return NULL;
	memset(vm, 0, sizeof(*vm));
	vm->rng = (uint32_t)port_uptime_us() * 2654435761u | 1;
	vm->line_buffered = port_isatty(1);
	vm->empty = al_str_new(vm, "", 0);
	for (int i = 0; i < 3; i++)
		vm->std[i] = al_file_new(vm, i, true);
	if (!vm->empty || !vm->std[0] || !vm->std[1] || !vm->std[2]) {
		al_heap_release(vm);
		port_free(vm);
		return NULL;
	}
	return vm;
}

void al_vm_free(struct al_vm *vm, bool leak_check)
{
	struct al_prog *p = &vm->prog;

	al_flush(vm);
	if (vm->globals) {
		for (uint32_t i = 0; i < p->nglobals; i++)
			if (KIND_IS_REF(p->global_kinds[i]))
				al_decref(vm, vm->globals[i].o);
		port_free(vm->globals);
	}
	if (p->strings) {
		/* loading may have stopped part way: some entries can be NULL */
		for (uint32_t i = 0; i < p->nstrings; i++)
			if (p->strings[i])
				al_decref(vm, &p->strings[i]->h);
		port_free(p->strings);
	}
	for (int i = 0; i < 3; i++)
		if (vm->std[i])
			al_decref(vm, &vm->std[i]->h);
	if (vm->empty)
		al_decref(vm, &vm->empty->h);
	if (leak_check && vm->heap.objects)
		al_eprintf("leak check: %zu objects still allocated (reference cycles?)\n", vm->heap.objects);
	port_free(p->structs);
	port_free(p->fields);
	port_free(p->global_kinds);
	port_free(p->funcs);
	port_free(p->image);
	port_free(vm->stack);
	port_free(vm->frames);
	al_heap_release(vm);
	port_free(vm);
}

/*
 * Room for `need` more values above the first `used`. The stack may move, so
 * the caller turns its pointers into offsets first: taking the address of sp
 * and bp instead would keep them out of registers in the whole interpreter.
 */
static bool grow_stack(struct al_vm *vm, size_t used, size_t need)
{
	size_t cap = vm->stack_cap;

	while (cap - used < need) {
		cap *= 2;
		if (cap > AL_MAX_STACK)
			return false;
	}
	union al_val *stack = port_alloc(cap * sizeof(*stack));
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

static bool grow_frames(struct al_vm *vm)
{
	uint32_t cap = vm->frames_cap * 2;

	if (cap > AL_MAX_CALL_DEPTH)
		cap = AL_MAX_CALL_DEPTH;
	if (cap <= vm->frames_cap)
		return false;
	struct al_frame *frames = port_realloc(vm->frames, cap * sizeof(*frames));
	if (!frames)
		return false;
	vm->frames = frames;
	vm->frames_cap = cap;
	return true;
}

/* len() of a local, for the fused compares: false if it has no length. */
static inline bool qlen(const struct al_obj *o, int32_t *len)
{
	if (!o || (o->type != OT_STR && o->type != OT_ARRAY))
		return false;
	*len = o->type == OT_STR ? ((const struct al_str *)o)->len : ((const struct al_array *)o)->len;
	return true;
}

static const char *qlen_error(const struct al_obj *o)
{
	return o ? "damaged executable (len)" : "len() of a null array";
}

#define INCREF(o)	do { struct al_obj *o_ = (o); if (o_) o_->refs++; } while (0)
#define DECREF(o)	do { struct al_obj *o_ = (o); if (o_ && --o_->refs == 0) al_obj_free(vm, o_); } while (0)
#define STR(v)		((struct al_str *)(v).o)
#define ARR(v)		((struct al_array *)(v).o)
#define OBJ(v)		((struct al_struct *)(v).o)
/*
 * Compiled code always has the right type here; a damaged or hand-made
 * executable might not, and this is what keeps it from following a number
 * as if it were a pointer.
 */
#define OK(p, want)	((p) && (p)->h.type == (want))
#define JUMP()		(ip + 3 + (int16_t)al_u16(ip + 1))

/*
 * Run function fn with its arguments already at the bottom of the stack.
 * Returns 0, or -1 after an error, exit() or an interrupt.
 */
int al_vm_exec(struct al_vm *vm, uint16_t fn, union al_val *result)
{
	const struct al_prog *prog = &vm->prog;
	const struct al_func *funcs = prog->funcs;
	struct al_str **strings = prog->strings;
	union al_val *globals = vm->globals;
	const uint8_t *code = prog->code;
	const struct al_func *f = &funcs[fn];
	union al_val *bp = vm->stack, *sp = vm->stack + f->nparams;
	const uint8_t *ip;
	int budget = POLL_EVERY;

	if (vm->stack_cap - f->nparams < (size_t)f->nlocals + f->max_stack &&
	    !grow_stack(vm, f->nparams, f->nlocals + f->max_stack)) {
		al_eprintf("out of memory\n");
		return -1;
	}
	bp = vm->stack;
	sp = bp + f->nparams;
	for (uint32_t i = f->nparams; i < f->nlocals; i++)
		(sp++)->o = NULL;
	if (vm->nframes == vm->frames_cap && !grow_frames(vm)) {
		al_eprintf("out of memory\n");
		return -1;
	}
	vm->frames[vm->nframes++] = (struct al_frame){ .ip = NULL, .bp = bp, .fn = fn };
	ip = code + f->code;

#if defined(__GNUC__)
	static const void *const labels[OP_TOTAL] = {
#define X(name, fmt, pop, push) [OP_##name] = &&L_##name,
		AL_OPS(X)
#undef X
#define X(form, b, cc, op) [Q_##form##b##cc] = &&L_Q_##form##b##cc,
		AL_QCMPS(X)
#undef X
#define X(op) [Q_LL##op] = &&L_Q_LL##op, [Q_LK##op] = &&L_Q_LK##op,			\
	      [Q_LL##op##_ST] = &&L_Q_LL##op##_ST, [Q_LK##op##_ST] = &&L_Q_LK##op##_ST,	\
	      [Q_##op##_ST] = &&L_Q_##op##_ST,
		AL_QINT(X)
#undef X
#define X(op) [Q_LL##op] = &&L_Q_LL##op, [Q_LL##op##_ST] = &&L_Q_LL##op##_ST,		\
	      [Q_##op##_ST] = &&L_Q_##op##_ST,
		AL_QFLOAT(X)
#undef X
#define X(op) [Q_L_##op] = &&L_Q_L_##op,
		AL_QLOAD(X)
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
#define THROW(...)	do { vm->ip = ip; vm->fn = fn; al_error(vm, __VA_ARGS__); goto fail; } while (0)
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
#define STR_CMP(op, expr)						\
	CASE(op) {							\
		struct al_str *a = STR(sp[-2]), *b = STR(sp[-1]);	\
		if (!OK(a, OT_STR) || !OK(b, OT_STR))			\
			THROW("null string");				\
		int r = al_str_cmp(a, b);				\
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
		(sp++)->i = (int32_t)al_u32(ip + 1);
		NEXT(5);
	CASE(CONSTF) {
		uint32_t u = al_u32(ip + 1);
		memcpy(&sp->f, &u, 4);
		sp++;
		NEXT(5);
	}
	CASE(CONSTS) {
		struct al_str *s = strings[al_u16(ip + 1)];
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
		union al_val v = bp[ip[1]];
		INCREF(v.o);
		*sp++ = v;
		NEXT(2);
	}
	CASE(STORE)
		bp[ip[1]] = *--sp;
		NEXT(2);
	CASE(STORER) {
		struct al_obj *old = bp[ip[1]].o;
		bp[ip[1]] = *--sp;
		DECREF(old);
		NEXT(2);
	}
	CASE(CLEARR) {
		struct al_obj *old = bp[ip[1]].o;
		bp[ip[1]].o = NULL;
		DECREF(old);
		NEXT(2);
	}
	CASE(INCL)
		bp[ip[1]].i = (int32_t)((uint32_t)bp[ip[1]].i + (uint32_t)(int32_t)(int8_t)ip[2]);
		NEXT(3);
	CASE(CATL) {
		struct al_str *s = STR(bp[ip[1]]), *t = STR(sp[-1]), *r;
		if (!OK(s, OT_STR) || !OK(t, OT_STR))
			THROW(s && t ? "not a string (damaged executable)" : "null string");
		if (s->h.refs == 1) {
			r = al_str_append(vm, s, t);
		} else {
			r = al_str_concat(vm, s, t);
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
		*sp++ = globals[al_u16(ip + 1)];
		NEXT(3);
	CASE(GLOADR) {
		union al_val v = globals[al_u16(ip + 1)];
		INCREF(v.o);
		*sp++ = v;
		NEXT(3);
	}
	CASE(GSTORE)
		globals[al_u16(ip + 1)] = *--sp;
		NEXT(3);
	CASE(GSTORER) {
		struct al_obj *old = globals[al_u16(ip + 1)].o;
		globals[al_u16(ip + 1)] = *--sp;
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

	CASE(CONCAT) {
		struct al_str *a = STR(sp[-2]), *b = STR(sp[-1]), *r;
		if (!OK(a, OT_STR) || !OK(b, OT_STR))
			THROW(a && b ? "not a string (damaged executable)" : "null string");
		if (!(r = al_str_concat(vm, a, b)))
			THROW("out of memory");
		sp[-2].o = &r->h;
		sp--;
		DECREF(&a->h);
		DECREF(&b->h);
		NEXT(1);
	}
	STR_CMP(EQS, r == 0)
	STR_CMP(NES, r != 0)
	STR_CMP(LTS, r < 0)
	STR_CMP(LES, r <= 0)
	STR_CMP(GTS, r > 0)
	STR_CMP(GES, r >= 0)
	CASE(EQR)
	CASE(NER) {
		struct al_obj *a = sp[-2].o, *b = sp[-1].o;
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
		sp[-1].i = al_float_to_int(sp[-1].f);
		NEXT(1);
	CASE(TOSTR)
	CASE(TOSTR2) {
		union al_val *v = *ip == OP_TOSTR ? &sp[-1] : &sp[-2];
		struct al_str *s = al_tostr(vm, *v, ip[1]);
		if (!s)
			THROW("out of memory");
		v->o = &s->h;
		NEXT(2);
	}
	CASE(TOSTRX) {
		struct al_str *s = al_tostr_desc(vm, sp[-1], strings[al_u16(ip + 1)]);
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
		uint16_t callee = al_u16(ip + 1);
		const struct al_func *cf = &funcs[callee];
		POLL();
		if (vm->nframes == vm->frames_cap && !grow_frames(vm)) {
			if (vm->frames_cap >= AL_MAX_CALL_DEPTH)
				THROW("call depth limit (%d) reached; runaway recursion?", AL_MAX_CALL_DEPTH);
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
		struct al_frame *fr = &vm->frames[vm->nframes++];
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
		struct al_frame *fr = &vm->frames[--vm->nframes];
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
		union al_val *args = sp - ip[2];
		const char *sig = al_builtins[ip[1]].sig;
		vm->ip = ip;
		vm->fn = fn;
		for (int i = 0; *sig && *sig != ':' && i < ip[2]; sig++) {
			if (*sig == '?')
				continue;
			int want = *sig == 's' ? OT_STR : *sig == 'F' ? OT_FILE : *sig == 'S' ? OT_ARRAY : -1;
			if (want >= 0 && args[i].o && args[i].o->type != want)
				THROW("damaged executable (wrong argument type)");
			i++;
		}
		int r = al_builtin_fns[ip[1]](vm, args, ip[2]);
		if (r < 0)
			goto fail;
		sp = args + r;
		NEXT(3);
	}

	CASE(NEWARR) {
		struct al_array *a = al_array_new(vm, ip[1], al_u16(ip + 2));
		if (!a)
			THROW("out of memory");
		(sp++)->o = &a->h;
		NEXT(4);
	}
	CASE(APPEND)
		if (!OK(ARR(sp[-2]), OT_ARRAY))
			THROW("damaged executable (array expected)");
		if (!al_array_push(vm, ARR(sp[-2]), sp[-1]))
			THROW("out of memory");
		sp--;
		NEXT(1);
	CASE(IDX)
	CASE(IDXR) {
		struct al_array *a = ARR(sp[-2]);
		int32_t i = sp[-1].i;
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" : "null array");
		if ((uint32_t)i >= a->len)
			THROW("index %d out of range (length %u)", (int)i, (unsigned)a->len);
		union al_val v = a->items[i];
		if (*ip == OP_IDXR)
			INCREF(v.o);
		sp--;
		sp[-1] = v;
		DECREF(&a->h);
		NEXT(1);
	}
	CASE(SETIDX)
	CASE(SETIDXR) {
		struct al_array *a = ARR(sp[-3]);
		int32_t i = sp[-2].i;
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" : "null array");
		if ((uint32_t)i >= a->len)
			THROW("index %d out of range (length %u)", (int)i, (unsigned)a->len);
		union al_val old = a->items[i];
		a->items[i] = sp[-1];
		sp -= 3;
		if (*ip == OP_SETIDXR)
			DECREF(old.o);
		DECREF(&a->h);
		NEXT(1);
	}
	CASE(IDXL) {
		struct al_array *a = ARR(bp[ip[1]]);
		int32_t i = sp[-1].i;
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" : "null array");
		if ((uint32_t)i >= a->len)
			THROW("index %d out of range (length %u)", (int)i, (unsigned)a->len);
		sp[-1] = a->items[i];
		NEXT(2);
	}
	CASE(IDXLR) {
		struct al_array *a = ARR(bp[ip[1]]);
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
		struct al_array *a = ARR(bp[ip[1]]);
		int32_t i = sp[-2].i;
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" : "null array");
		if ((uint32_t)i >= a->len)
			THROW("index %d out of range (length %u)", (int)i, (unsigned)a->len);
		union al_val old = a->items[i];
		a->items[i] = sp[-1];
		sp -= 2;
		if (*ip == OP_SETIDXLR)
			DECREF(old.o);
		NEXT(2);
	}
	CASE(STRIDX) {
		struct al_str *s = STR(sp[-2]);
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
		struct al_obj *o = sp[-1].o;
		if (!o || (o->type != OT_STR && o->type != OT_ARRAY))
			THROW(o ? "damaged executable (len)" :
			      *ip == OP_LENA ? "len() of a null array" : "null string");
		sp[-1].i = o->type == OT_STR ? ((struct al_str *)o)->len : ((struct al_array *)o)->len;
		DECREF(o);
		NEXT(1);
	}
	CASE(LENL) {
		struct al_obj *o = bp[ip[1]].o;
		if (!o || (o->type != OT_STR && o->type != OT_ARRAY))
			THROW(o ? "damaged executable (len)" : "len() of a null array");
		(sp++)->i = o->type == OT_STR ? ((struct al_str *)o)->len : ((struct al_array *)o)->len;
		NEXT(2);
	}
	CASE(PUSHA) {
		struct al_array *a = ARR(sp[-2]);
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" :
			      "push() to a null array (give it a value first, as in int[] a = [])");
		if (!al_array_push(vm, a, sp[-1]))
			THROW("out of memory");
		sp -= 2;
		DECREF(&a->h);
		NEXT(1);
	}
	CASE(PUSHL) {
		struct al_array *a = ARR(bp[ip[1]]);
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" :
			      "push() to a null array (give it a value first, as in int[] a = [])");
		if (!al_array_push(vm, a, sp[-1]))
			THROW("out of memory");
		sp--;
		NEXT(2);
	}
	CASE(POPA) {
		struct al_array *a = ARR(sp[-1]);
		if (!OK(a, OT_ARRAY))
			THROW(a ? "not an array (damaged executable)" : "pop() from a null array");
		if (!a->len)
			THROW("pop() from an empty array");
		sp[-1] = a->items[--a->len];
		DECREF(&a->h);
		NEXT(1);
	}

	CASE(NEWST) {
		struct al_struct *s = al_struct_new(vm, al_u16(ip + 1));
		if (!s)
			THROW("out of memory");
		(sp++)->o = &s->h;
		NEXT(3);
	}
	CASE(SETFI)
	CASE(SETFIR) {
		struct al_struct *s = OBJ(sp[-2]);
		if (!OK(s, OT_STRUCT) || ip[1] >= s->h.kind)
			THROW("bad struct field");
		union al_val old = s->fields[ip[1]];
		s->fields[ip[1]] = sp[-1];
		sp--;
		if (*ip == OP_SETFIR)
			DECREF(old.o);
		NEXT(2);
	}
	CASE(GETF)
	CASE(GETFR) {
		struct al_struct *s = OBJ(sp[-1]);
		if (!OK(s, OT_STRUCT))
			THROW(s ? "not a struct (damaged executable)" : "null struct (reading a field)");
		if (ip[1] >= s->h.kind)
			THROW("bad struct field");
		union al_val v = s->fields[ip[1]];
		if (*ip == OP_GETFR)
			INCREF(v.o);
		sp[-1] = v;
		DECREF(&s->h);
		NEXT(2);
	}
	CASE(SETF)
	CASE(SETFR) {
		struct al_struct *s = OBJ(sp[-2]);
		if (!OK(s, OT_STRUCT))
			THROW(s ? "not a struct (damaged executable)" : "null struct (setting a field)");
		if (ip[1] >= s->h.kind)
			THROW("bad struct field");
		union al_val old = s->fields[ip[1]];
		s->fields[ip[1]] = sp[-1];
		sp -= 2;
		if (*ip == OP_SETFR)
			DECREF(old.o);
		DECREF(&s->h);
		NEXT(2);
	}
	CASE(GETFL)
	CASE(GETFLR) {
		struct al_struct *s = OBJ(bp[ip[1]]);
		if (!OK(s, OT_STRUCT))
			THROW(s ? "not a struct (damaged executable)" : "null struct (reading a field)");
		if (ip[2] >= s->h.kind)
			THROW("bad struct field");
		union al_val v = s->fields[ip[2]];
		if (*ip == OP_GETFLR)
			INCREF(v.o);
		*sp++ = v;
		NEXT(3);
	}
	CASE(SETFL)
	CASE(SETFLR) {
		struct al_struct *s = OBJ(bp[ip[1]]);
		if (!OK(s, OT_STRUCT))
			THROW(s ? "not a struct (damaged executable)" : "null struct (setting a field)");
		if (ip[2] >= s->h.kind)
			THROW("bad struct field");
		union al_val old = s->fields[ip[2]];
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
#define QB_W(v, at)	((v) = (int32_t)al_u32(ip + (at)), 5)
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
		ip = taken ? j + 3 + (int16_t)al_u16(j + 1) : j + 3;	\
		QJ_##form(taken);					\
		DISPATCH();						\
	}
	AL_QCMPS(X)
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
	AL_QINT(X)
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
	AL_QFLOAT(X)
#undef X

	/* LOAD a; X: push a, then X's own handler, without a dispatch */
#define X(op)								\
	QCASE(Q_L_##op)							\
		*sp++ = bp[ip[1]];					\
		ip += 2;						\
		goto L_##op;
	AL_QLOAD(X)
#undef X


#if !defined(__GNUC__)
	}
#endif

finished:
	if (result)
		*result = vm->stack[0];
	return 0;

interrupted:
	al_flush(vm);
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

int al_vm_run(struct al_vm *vm, int argc, char **argv)
{
	struct al_prog *p = &vm->prog;
	const struct al_func *main_fn = &p->funcs[p->main];
	union al_val result = { 0 };
	int status = 1;

	vm->globals = port_alloc((p->nglobals + 1) * sizeof(*vm->globals));
	vm->stack_cap = 256;
	vm->stack = port_alloc(vm->stack_cap * sizeof(*vm->stack));
	vm->frames_cap = 32;
	vm->frames = port_alloc(vm->frames_cap * sizeof(*vm->frames));
	if (!vm->globals || !vm->stack || !vm->frames) {
		al_eprintf("out of memory\n");
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

	if (p->init != AL_NO_FUNC && al_vm_exec(vm, p->init, NULL))
		goto out;
	vm->nframes = 0;
	if (main_fn->nparams) {
		struct al_array *args = al_array_new(vm, K_STR, argc);
		if (!args) {
			al_eprintf("out of memory\n");
			goto out;
		}
		for (int i = 0; i < argc; i++) {
			struct al_str *s = al_str_new(vm, argv[i], strlen(argv[i]));
			if (!s) {
				al_eprintf("out of memory\n");
				al_decref(vm, &args->h);
				goto out;
			}
			args->items[args->len++].o = &s->h;
		}
		vm->stack[0].o = &args->h;
	}
	if (al_vm_exec(vm, p->main, &result))
		goto out;
	status = main_fn->returns ? result.i : 0;
out:
	al_flush(vm);
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
