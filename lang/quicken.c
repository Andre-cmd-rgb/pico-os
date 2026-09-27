/*
 * Quickening: fusing common instruction sequences, once a program is loaded.
 *
 * Every instruction costs a dispatch: fetch the opcode, look up its handler,
 * jump there. On the ESP32-S3 that is most of the time an interpreter spends
 * on simple code, so a for loop's "i++; i < n" (INCL, LOAD, LOAD, LOOPLT)
 * costs four dispatches for one line of work. This pass rewrites the first
 * opcode of such a sequence into a fused instruction (al.h) that does all of
 * it in one.
 *
 * Only the first opcode byte of a sequence changes. The fused handler reads
 * the operands of every instruction it covers from where they were, so no
 * instruction moves, jump offsets stay right, and a jump that lands inside a
 * sequence still finds the original instructions there. Patterns are matched
 * against the original opcodes, kept aside, so sequences may overlap.
 *
 * The rewritten opcodes are LOAD, INCL and the arithmetic ones below; none
 * of their handlers looks at its own opcode byte, and the handlers that do
 * (GETFL and GETFLR, RET and RETV, ...) never have theirs rewritten.
 */
#include "pico.h"
#include "port.h"

/* The b operand of a compare: its kind and the opcode that supplies it. */
static int cmp_source(int op)
{
	switch (op) {
	case OP_LOAD:	 return 0;	/* L */
	case OP_CONST8:	 return 1;	/* K */
	case OP_CONST32: return 2;	/* W */
	case OP_LENL:	 return 3;	/* N */
	}
	return -1;
}

/* Q_<form><b><cc> for form 0 J, 1 P, 2 I; b as above; cc 0 EQ ... 5 GE. */
static int cmp_op(int form, int b, int cc)
{
	return OP_COUNT + (form * 4 + b) * 6 + cc;
}

static int int_op(int op)
{
	switch (op) {
	case OP_ADD: return 0;
	case OP_SUB: return 1;
	case OP_MUL: return 2;
	}
	return -1;
}

static int float_op(int op)
{
	switch (op) {
	case OP_ADDF: return 0;
	case OP_SUBF: return 1;
	case OP_MULF: return 2;
	}
	return -1;
}

static const uint8_t int_fused[3][5] = {
	{ Q_LLADD, Q_LKADD, Q_LLADD_ST, Q_LKADD_ST, Q_ADD_ST },
	{ Q_LLSUB, Q_LKSUB, Q_LLSUB_ST, Q_LKSUB_ST, Q_SUB_ST },
	{ Q_LLMUL, Q_LKMUL, Q_LLMUL_ST, Q_LKMUL_ST, Q_MUL_ST },
};

static const uint8_t float_fused[3][3] = {
	{ Q_LLADDF, Q_LLADDF_ST, Q_ADDF_ST },
	{ Q_LLSUBF, Q_LLSUBF_ST, Q_SUBF_ST },
	{ Q_LLMULF, Q_LLMULF_ST, Q_MULF_ST },
};

static int load_then(int op)
{
	switch (op) {
#define X(name) case OP_##name: return Q_L_##name;
	PICO_QLOAD(X)
#undef X
	}
	return -1;
}

/* The fused opcode for the sequence starting with instruction i, or -1.
 * op[] holds the original opcodes of the function's n instructions. */
static int fuse(const uint8_t *op, int i, int n)
{
	int o0 = op[i];
	int o1 = i + 1 < n ? op[i + 1] : -1;
	int o2 = i + 2 < n ? op[i + 2] : -1;
	int o3 = i + 3 < n ? op[i + 3] : -1;

	/* INCL; LOAD; b; LOOPcc: a for loop's step and test */
	if (o0 == OP_INCL && o1 == OP_LOAD && cmp_source(o2) >= 0 &&
	    o3 >= OP_LOOPEQ && o3 <= OP_LOOPGE)
		return cmp_op(2, cmp_source(o2), o3 - OP_LOOPEQ);
	if (o0 != OP_LOAD) {
		if (int_op(o0) >= 0 && o1 == OP_STORE)
			return int_fused[int_op(o0)][4];
		if (float_op(o0) >= 0 && o1 == OP_STORE)
			return float_fused[float_op(o0)][2];
		return -1;
	}

	/* LOAD a; b; Jcc or LOOPcc */
	if (cmp_source(o1) >= 0 && o2 >= OP_JEQ && o2 <= OP_JGE)
		return cmp_op(0, cmp_source(o1), o2 - OP_JEQ);
	if (cmp_source(o1) >= 0 && o2 >= OP_LOOPEQ && o2 <= OP_LOOPGE)
		return cmp_op(1, cmp_source(o1), o2 - OP_LOOPEQ);

	/* LOAD a; LOAD b or CONST8 k; op [; STORE c] */
	if ((o1 == OP_LOAD || o1 == OP_CONST8) && int_op(o2) >= 0)
		return int_fused[int_op(o2)][(o1 == OP_CONST8) + (o3 == OP_STORE) * 2];
	if (o1 == OP_LOAD && float_op(o2) >= 0)
		return float_fused[float_op(o2)][o3 == OP_STORE];

	return o1 >= 0 ? load_then(o1) : -1;
}

void pico_quicken(struct pico_vm *vm)
{
	const struct pico_prog *p = &vm->prog;
	uint8_t *code = (uint8_t *)p->code;	/* our own copy of the file */

	if (vm->quickened)
		return;			/* fused opcodes have no pico_opinfo entry */
	vm->quickened = true;
	for (uint16_t f = 0; f < p->nfuncs; f++) {
		uint8_t *c = code + p->funcs[f].code;
		uint32_t len = p->funcs[f].len, n = 0;

		/* instruction starts and original opcodes; the verifier has
		 * checked that the instructions tile the function exactly */
		for (uint32_t pc = 0; pc < len; pc += pico_opinfo[c[pc]].size)
			n++;
		uint32_t *start = port_alloc(n * sizeof(*start) + n);
		if (!start)
			return;			/* runs unfused: slower, still right */
		uint8_t *op = (uint8_t *)(start + n);
		n = 0;
		for (uint32_t pc = 0; pc < len; pc += pico_opinfo[c[pc]].size) {
			start[n] = pc;
			op[n++] = c[pc];
		}

		for (uint32_t i = 0; i < n; i++) {
			int q = fuse(op, i, n);
			if (q >= 0)
				c[start[i]] = q;
		}
		port_free(start);
	}
}
