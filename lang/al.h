/*
 * a: a small statically typed language.
 *
 * Declarations shared by the compiler, the executable loader and the virtual
 * machine. Nothing here knows about the platform; that lives behind port.h,
 * so the same core builds into the firmware and into the PC tools.
 *
 * No mutable static state anywhere in the core: on PocketType every process
 * shares one address space, so two programs running at once must not see
 * each other's data.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AL_VERSION		2	/* executable format version */
#define AL_HEADER_SIZE		32
#define AL_TRAILER_SIZE		4	/* CRC-32 of everything before it */
#define AL_MAX_CALL_DEPTH	10000
#define AL_MAX_STACK		(1u << 20)	/* values */
#define AL_MAX_SOURCE		(1u << 20)	/* bytes */
#define AL_MAX_IMAGE		(4u << 20)

/* The first bytes of every executable: DEL 'A' 'L', then the version. */
#define AL_MAGIC		"\x7f" "AL"

/* ------------------------------------------------------------ values */

struct al_obj;

/*
 * Values carry no type tag: the compiler knows every type, so the bytecode
 * uses typed instructions instead. 4 bytes on the ESP32-S3.
 */
union al_val {
	int32_t		 i;
	float		 f;
	struct al_obj	*o;
};

enum al_otype {
	OT_STR,
	OT_ARRAY,
	OT_STRUCT,
	OT_FILE,
};

/* What an array element or a struct field holds, for defaults and freeing. */
enum al_kind {
	K_INT,
	K_FLOAT,
	K_BOOL,
	K_STR,
	K_REF,		/* array, struct or File: may be null */
};

#define KIND_IS_REF(k)	((k) >= K_STR)

struct al_obj {
	uint32_t	refs;
	uint8_t		type;		/* enum al_otype */
	uint8_t		kind;		/* arrays: element kind */
	uint16_t	sid;		/* structs: index into the struct table */
};

struct al_str {
	struct al_obj	h;
	uint32_t	len;
	uint32_t	cap;		/* bytes available in data, excluding the NUL */
	char		data[];		/* always NUL-terminated */
};

struct al_array {
	struct al_obj	 h;
	uint32_t	 len;
	uint32_t	 cap;
	union al_val	*items;
	struct al_obj	*dead;		/* link while being freed */
};

struct al_struct {
	struct al_obj	 h;
	struct al_obj	*dead;
	union al_val	 fields[];
};

struct al_file {
	struct al_obj	 h;
	int		 fd;		/* -1 once closed */
	bool		 std;		/* stdin, stdout, stderr: never really closed */
	bool		 eof;
	uint16_t	 rpos;
	uint16_t	 rlen;
	char		*rbuf;
};

/* ------------------------------------------------------------ keys */

/* readkey() results besides plain bytes; the same numbers as pt/keys.h. */
enum {
	AL_KEY_NONE = -4,	/* timeout */
	AL_KEY_INTR = -3,
	AL_KEY_ERROR = -2,
	AL_KEY_EOF = -1,
	AL_KEY_UP = 0x100,
	AL_KEY_DOWN,
	AL_KEY_LEFT,
	AL_KEY_RIGHT,
	AL_KEY_HOME,
	AL_KEY_END,
	AL_KEY_PGUP,
	AL_KEY_PGDN,
	AL_KEY_INSERT,
	AL_KEY_DELETE,
	AL_KEY_ESC,
	AL_KEY_F1,
	AL_KEY_F2,
	AL_KEY_F3,
	AL_KEY_F4,
	AL_KEY_UNKNOWN,
};

/* ------------------------------------------------------------ bytecode */

/*
 * X(name, operands, pops, pushes). Operands: b u8, c i8, w u16, j i16 jump
 * offset from the end of the instruction, i i32, f f32. -1: depends on the
 * operands (calls). Jumps whose name starts with LOOP go backwards and poll
 * for Ctrl-C.
 */
#define AL_OPS(X)							\
	X(NOP, "", 0, 0)						\
	X(CONST8, "c", 0, 1)						\
	X(CONST32, "i", 0, 1)						\
	X(CONSTF, "f", 0, 1)						\
	X(CONSTS, "w", 0, 1)						\
	X(NULLV, "", 0, 1)						\
	X(STDFILE, "b", 0, 1)						\
	X(LOAD, "b", 0, 1)						\
	X(LOADR, "b", 0, 1)						\
	X(STORE, "b", 1, 0)						\
	X(STORER, "b", 1, 0)						\
	X(CLEARR, "b", 0, 0)						\
	X(INCL, "bc", 0, 0)						\
	X(CATL, "b", 1, 0)						\
	X(GLOAD, "w", 0, 1)						\
	X(GLOADR, "w", 0, 1)						\
	X(GSTORE, "w", 1, 0)						\
	X(GSTORER, "w", 1, 0)						\
	X(POP, "", 1, 0)						\
	X(POPR, "", 1, 0)						\
	X(DUP, "", 1, 2)						\
	X(DUPR, "", 1, 2)						\
	X(DUP2R, "", 2, 4)						\
	X(ADD, "", 2, 1)						\
	X(SUB, "", 2, 1)						\
	X(MUL, "", 2, 1)						\
	X(DIV, "", 2, 1)						\
	X(MOD, "", 2, 1)						\
	X(BAND, "", 2, 1)						\
	X(BOR, "", 2, 1)						\
	X(BXOR, "", 2, 1)						\
	X(SHL, "", 2, 1)						\
	X(SHR, "", 2, 1)						\
	X(NEG, "", 1, 1)						\
	X(BNOT, "", 1, 1)						\
	X(NOT, "", 1, 1)						\
	X(EQ, "", 2, 1)							\
	X(NE, "", 2, 1)							\
	X(LT, "", 2, 1)							\
	X(LE, "", 2, 1)							\
	X(GT, "", 2, 1)							\
	X(GE, "", 2, 1)							\
	X(ADDF, "", 2, 1)						\
	X(SUBF, "", 2, 1)						\
	X(MULF, "", 2, 1)						\
	X(DIVF, "", 2, 1)						\
	X(MODF, "", 2, 1)						\
	X(NEGF, "", 1, 1)						\
	X(EQF, "", 2, 1)						\
	X(NEF, "", 2, 1)						\
	X(LTF, "", 2, 1)						\
	X(LEF, "", 2, 1)						\
	X(GTF, "", 2, 1)						\
	X(GEF, "", 2, 1)						\
	X(CONCAT, "", 2, 1)						\
	X(EQS, "", 2, 1)						\
	X(NES, "", 2, 1)						\
	X(LTS, "", 2, 1)						\
	X(LES, "", 2, 1)						\
	X(GTS, "", 2, 1)						\
	X(GES, "", 2, 1)						\
	X(EQR, "", 2, 1)						\
	X(NER, "", 2, 1)						\
	X(I2F, "", 1, 1)						\
	X(I2F2, "", 2, 2)						\
	X(F2I, "", 1, 1)						\
	X(TOSTR, "b", 1, 1)						\
	X(TOSTR2, "b", 2, 2)						\
	X(TOSTRX, "w", 1, 1)						\
	X(JMP, "j", 0, 0)						\
	X(JF, "j", 1, 0)						\
	X(JT, "j", 1, 0)						\
	X(JFK, "j", 1, 0)						\
	X(JTK, "j", 1, 0)						\
	X(JEQ, "j", 2, 0)						\
	X(JNE, "j", 2, 0)						\
	X(JLT, "j", 2, 0)						\
	X(JLE, "j", 2, 0)						\
	X(JGT, "j", 2, 0)						\
	X(JGE, "j", 2, 0)						\
	X(LOOP, "j", 0, 0)						\
	X(LOOPT, "j", 1, 0)						\
	X(LOOPEQ, "j", 2, 0)						\
	X(LOOPNE, "j", 2, 0)						\
	X(LOOPLT, "j", 2, 0)						\
	X(LOOPLE, "j", 2, 0)						\
	X(LOOPGT, "j", 2, 0)						\
	X(LOOPGE, "j", 2, 0)						\
	X(CALL, "w", -1, -1)						\
	X(CALLB, "bb", -1, -1)						\
	X(RET, "", 0, 0)						\
	X(RETV, "", 1, 0)						\
	X(NEWARR, "bw", 0, 1)						\
	X(APPEND, "", 2, 1)						\
	X(IDX, "", 2, 1)						\
	X(IDXR, "", 2, 1)						\
	X(SETIDX, "", 3, 0)						\
	X(SETIDXR, "", 3, 0)						\
	X(IDXL, "b", 1, 1)						\
	X(IDXLR, "b", 1, 1)						\
	X(SETIDXL, "b", 2, 0)						\
	X(SETIDXLR, "b", 2, 0)						\
	X(STRIDX, "", 2, 1)						\
	X(LENA, "", 1, 1)						\
	X(LENS, "", 1, 1)						\
	X(LENL, "b", 0, 1)						\
	X(PUSHA, "", 2, 0)						\
	X(PUSHL, "b", 1, 0)						\
	X(POPA, "", 1, 1)						\
	X(NEWST, "w", 0, 1)						\
	X(SETFI, "b", 2, 1)						\
	X(SETFIR, "b", 2, 1)						\
	X(GETF, "b", 1, 1)						\
	X(GETFR, "b", 1, 1)						\
	X(SETF, "b", 2, 0)						\
	X(SETFR, "b", 2, 0)						\
	X(GETFL, "bb", 0, 1)						\
	X(GETFLR, "bb", 0, 1)						\
	X(SETFL, "bb", 1, 0)						\
	X(SETFLR, "bb", 1, 0)

enum al_op {
#define X(name, fmt, pop, push) OP_##name,
	AL_OPS(X)
#undef X
	OP_COUNT
};

struct al_opinfo {
	const char	*name;
	const char	*fmt;
	int8_t		 pop;
	int8_t		 push;
	uint8_t		 size;		/* opcode plus operands */
};

extern const struct al_opinfo al_opinfo[OP_COUNT];

/*
 * Fused instructions. After a program is verified, al_quicken() rewrites the
 * first opcode of common sequences into one of these, which does the whole
 * sequence in one dispatch. Only that opcode byte changes: operands are read
 * where the compiler put them, and a jump into the middle of a sequence still
 * finds the original instructions. They never appear in a file: the verifier
 * refuses any opcode from OP_COUNT on.
 *
 * Compare and branch, "LOAD a; <b>; Jcc or LOOPcc", named Q_<form><b><cc>:
 *   form	J  a forward Jcc          P  a LOOPcc (polls for Ctrl-C)
 *		I  INCL before a LOOPcc: the step and test of a for loop
 *   b		L  LOAD b   K  CONST8   W  CONST32   N  LENL (length of a local)
 */
#define AL_QCC(X, form, b)						\
	X(form, b, EQ, ==) X(form, b, NE, !=) X(form, b, LT, <)		\
	X(form, b, LE, <=) X(form, b, GT, >) X(form, b, GE, >=)
#define AL_QCMPS(X)							\
	AL_QCC(X, J, L) AL_QCC(X, J, K) AL_QCC(X, J, W) AL_QCC(X, J, N)	\
	AL_QCC(X, P, L) AL_QCC(X, P, K) AL_QCC(X, P, W) AL_QCC(X, P, N)	\
	AL_QCC(X, I, L) AL_QCC(X, I, K) AL_QCC(X, I, W) AL_QCC(X, I, N)

/* Arithmetic on locals: "LOAD a; LOAD b | CONST8 k; op" pushes the result,
 * and with "; STORE c" after it, stores it. "op; STORE c" pops into c. */
#define AL_QINT(X)	X(ADD) X(SUB) X(MUL)
#define AL_QFLOAT(X)	X(ADDF) X(SUBF) X(MULF)

/* "LOAD a; X" for other common X: push, then run X's own handler. */
#define AL_QLOAD(X)							\
	X(LOAD) X(CONST8) X(CONST32) X(CONSTF) X(I2F) X(IDXL) X(LENL)	\
	X(GETFL) X(CALL) X(STORE) X(ADD) X(SUB) X(MUL) X(ADDF) X(SUBF)	\
	X(MULF) X(DIV) X(MOD)

enum al_qop {
	Q_FIRST = OP_COUNT - 1,
#define X(form, b, cc, op) Q_##form##b##cc,
	AL_QCMPS(X)
#undef X
#define X(op) Q_LL##op, Q_LK##op, Q_LL##op##_ST, Q_LK##op##_ST, Q_##op##_ST,
	AL_QINT(X)
#undef X
#define X(op) Q_LL##op, Q_LL##op##_ST, Q_##op##_ST,
	AL_QFLOAT(X)
#undef X
#define X(op) Q_L_##op,
	AL_QLOAD(X)
#undef X
	OP_TOTAL
};

_Static_assert(OP_TOTAL <= 256, "opcodes are one byte");

/* TOSTR operand */
enum { TS_INT, TS_FLOAT, TS_BOOL };

/* ------------------------------------------------------------ built-ins */

/*
 * X(c name, a name, signature). Signature "args:result" with i int, f float,
 * b bool, s str, F File, S str[], v void; '?' marks an optional argument.
 * "*" means the compiler checks the call itself. Names starting with '@'
 * are variants the compiler picks; programs cannot name them.
 */
#define AL_BUILTINS(X)							\
	X(printf, "printf", "*")					\
	X(format, "format", "*")					\
	X(print, "print", "*")						\
	X(println, "println", "*")					\
	X(substr, "substr", "sii?:s")					\
	X(find, "find", "*")						\
	X(finds, "@finds", "ssi?:i")					\
	X(finda, "@finda", "*")						\
	X(split, "split", "ss?:S")					\
	X(join, "join", "Ss:s")						\
	X(trim, "trim", "s:s")						\
	X(upper, "upper", "s:s")					\
	X(lower, "lower", "s:s")					\
	X(replace, "replace", "sss:s")					\
	X(starts_with, "starts_with", "ss:b")				\
	X(ends_with, "ends_with", "ss:b")				\
	X(contains, "contains", "ss:b")					\
	X(repeat, "repeat", "si:s")					\
	X(chr, "chr", "i:s")						\
	X(ord, "ord", "s:i")						\
	X(to_int, "to_int", "si?:i")					\
	X(to_float, "to_float", "sf?:f")				\
	X(insert, "insert", "*")					\
	X(remove_at, "remove_at", "*")					\
	X(resize, "resize", "*")					\
	X(slice, "slice", "*")						\
	X(sort, "sort", "*")						\
	X(reverse, "reverse", "*")					\
	X(abs, "abs", "*")						\
	X(absf, "@absf", "f:f")						\
	X(min, "min", "*")						\
	X(minf, "@minf", "ff:f")					\
	X(max, "max", "*")						\
	X(maxf, "@maxf", "ff:f")					\
	X(sqrt, "sqrt", "f:f")						\
	X(pow, "pow", "ff:f")						\
	X(sin, "sin", "f:f")						\
	X(cos, "cos", "f:f")						\
	X(tan, "tan", "f:f")						\
	X(ln, "ln", "f:f")						\
	X(exp, "exp", "f:f")						\
	X(hypot, "hypot", "ff:f")					\
	X(atan2, "atan2", "ff:f")					\
	X(floor, "floor", "f:i")					\
	X(ceil, "ceil", "f:i")						\
	X(round, "round", "f:i")					\
	X(random, "random", "i:i")					\
	X(seed, "seed", "i:v")						\
	X(open, "open", "ss:F")						\
	X(close, "close", "F:v")					\
	X(read, "read", "Fi?:s")					\
	X(readline, "readline", "F:s")					\
	X(write, "write", "Fs:b")					\
	X(exists, "exists", "s:b")					\
	X(is_dir, "is_dir", "s:b")					\
	X(remove, "remove", "s:b")					\
	X(rename, "rename", "ss:b")					\
	X(mkdir, "mkdir", "s:b")					\
	X(listdir, "listdir", "s:S")					\
	X(assert, "assert", "bs?:v")					\
	X(exit, "exit", "i:v")						\
	X(run, "run", "*")						\
	X(runa, "@runa", "S:i")						\
	X(getenv, "getenv", "s:s")					\
	X(sleep_ms, "sleep_ms", "i:v")					\
	X(uptime_ms, "uptime_ms", ":i")					\
	X(time, "time", ":i")						\
	X(date, "date", ":s")						\
	X(raw_mode, "raw_mode", "b:v")					\
	X(readkey, "readkey", "i?:i")					\
	X(term_cols, "term_cols", ":i")					\
	X(term_rows, "term_rows", ":i")

enum al_builtin {
#define X(cname, name, sig) B_##cname,
	AL_BUILTINS(X)
#undef X
	B_COUNT
};

struct al_builtin_info {
	const char	*name;
	const char	*sig;
};

extern const struct al_builtin_info al_builtins[B_COUNT];

/* ------------------------------------------------------------ programs */

struct al_func {
	uint32_t	code;		/* offset into the code section */
	uint32_t	len;
	uint32_t	lines;		/* first line table entry */
	uint32_t	nlines;
	uint16_t	name;		/* string index */
	uint16_t	max_stack;
	uint8_t		nparams;
	uint8_t		nlocals;
	uint8_t		returns;	/* 1 when it returns a value */
};

struct al_field {
	uint16_t	name;
	uint16_t	desc;		/* string index of the type descriptor */
	uint8_t		kind;
};

struct al_sdef {
	uint16_t	name;
	uint16_t	nfields;
	uint32_t	first;		/* index into fields */
};

struct al_prog {
	uint16_t	 nstrings;
	uint16_t	 nstructs;
	uint16_t	 nglobals;
	uint16_t	 nfuncs;
	uint16_t	 main;
	uint16_t	 init;		/* 0xffff: no global initialisers */
	uint16_t	 source;	/* string index of the source file name */
	struct al_str	**strings;
	struct al_sdef	*structs;
	struct al_field	*fields;
	uint8_t		*global_kinds;
	struct al_func	*funcs;
	const uint8_t	*lines;		/* pairs of u16 pc, u16 line */
	const uint8_t	*code;
	uint32_t	 code_len;
	uint8_t		*image;		/* owned copy of the file */
	size_t		 image_len;
};

#define AL_NO_FUNC	0xffff

/* ------------------------------------------------------------ memory */

#define AL_POOL_CLASSES	8

struct al_heap {
	void		*free[AL_POOL_CLASSES];
	void		*chunks;	/* linked list of pool chunks */
	char		*bump;
	char		*bump_end;
	size_t		 objects;	/* live objects, for the leak check */
	size_t		 bytes;
	size_t		 peak;
};

/* ------------------------------------------------------------ vm */

struct al_frame {
	const uint8_t	*ip;		/* return address, NULL for the bottom frame */
	union al_val	*bp;
	uint16_t	 fn;
};

#define AL_OUTBUF	1024

struct al_vm {
	struct al_prog	 prog;
	struct al_heap	 heap;
	union al_val	*stack;
	size_t		 stack_cap;
	struct al_frame	*frames;
	uint32_t	 nframes;
	uint32_t	 frames_cap;
	union al_val	*globals;
	struct al_str	*empty;
	struct al_file	*std[3];
	uint32_t	 rng;
	bool		 raw;
	bool		 halted;	/* exit() called */
	bool		 failed;	/* runtime error reported */
	bool		 line_buffered;
	bool		 quickened;	/* al_quicken ran: opcodes may be fused */
	int		 status;
	const uint8_t	*ip;		/* where an error happened */
	uint16_t	 fn;
	uint16_t	 outlen;
	char		 out[AL_OUTBUF];
};

/* ------------------------------------------------------------ compiler */

struct al_image {
	uint8_t		*data;
	size_t		 len;
};

/* Compile source; errors go to stderr. Returns 0 or -1. */
int	al_compile(const char *filename, const char *src, size_t len, struct al_image *out);

/* image.c */
uint32_t al_crc32(const uint8_t *data, size_t len);
int	al_load(struct al_vm *vm, const uint8_t *data, size_t len, char *err, size_t errlen);
void	al_disasm(struct al_vm *vm);	/* before al_quicken */

/* quicken.c: fuse common sequences, once al_load has verified the program */
void	al_quicken(struct al_vm *vm);

/* vm.c */
struct al_vm *al_vm_new(void);
int	al_vm_run(struct al_vm *vm, int argc, char **argv);
void	al_vm_free(struct al_vm *vm, bool leak_check);
int	al_vm_exec(struct al_vm *vm, uint16_t fn, union al_val *result);

/* runtime.c */
void	*al_alloc(struct al_vm *vm, size_t n);
void	 al_free(struct al_vm *vm, void *p, size_t n);
void	*al_grow(struct al_vm *vm, void *p, size_t old, size_t n);
void	 al_heap_release(struct al_vm *vm);
void	 al_obj_free(struct al_vm *vm, struct al_obj *o);
struct al_str *al_str_new(struct al_vm *vm, const char *s, size_t len);
struct al_str *al_str_alloc(struct al_vm *vm, size_t len);
struct al_str *al_str_concat(struct al_vm *vm, struct al_str *a, struct al_str *b);
struct al_str *al_str_append(struct al_vm *vm, struct al_str *a, struct al_str *b);
int	 al_str_cmp(const struct al_str *a, const struct al_str *b);
struct al_str *al_tostr(struct al_vm *vm, union al_val v, int kind);
int	 al_fmt_int(char *out, int32_t v);	/* decimal, no NUL; at most 11 bytes */
int32_t	 al_float_to_int(float f);
struct al_str *al_tostr_desc(struct al_vm *vm, union al_val v, const struct al_str *desc);
struct al_array *al_array_new(struct al_vm *vm, int kind, uint32_t cap);
bool	 al_array_push(struct al_vm *vm, struct al_array *a, union al_val v);
struct al_struct *al_struct_new(struct al_vm *vm, uint16_t sid);
struct al_file *al_file_new(struct al_vm *vm, int fd, bool std);
void	 al_error(struct al_vm *vm, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void	 al_flush(struct al_vm *vm);
int	 al_out(struct al_vm *vm, int fd, const char *s, size_t n);
int	 al_eprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int	 al_line_of(const struct al_prog *p, uint16_t fn, const uint8_t *ip);

struct al_fmt {
	char	*buf;
	size_t	 len;
	size_t	 cap;
	bool	 oom;
};

int	 al_fmt_next(const char **f, const char *end, const char **spec);
void	 al_fmt_puts(struct al_vm *vm, struct al_fmt *f, const char *s, size_t n);
void	 al_fmt_value(struct al_vm *vm, struct al_fmt *f, union al_val v, const char **desc, int depth, bool quote);

/* builtins.c */
typedef int (*al_builtin_fn)(struct al_vm *vm, union al_val *args, int argc);
extern const al_builtin_fn al_builtin_fns[B_COUNT];
int	 al_readkey(struct al_vm *vm, int timeout_ms);

static inline void al_incref(struct al_obj *o)
{
	if (o)
		o->refs++;
}

static inline void al_decref(struct al_vm *vm, struct al_obj *o)
{
	if (o && --o->refs == 0)
		al_obj_free(vm, o);
}

static inline uint16_t al_u16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static inline uint32_t al_u32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
