/*
 * pico: a small statically typed language.
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

#define PICO_VERSION		2	/* executable format version */
#define PICO_HEADER_SIZE		32
#define PICO_TRAILER_SIZE		4	/* CRC-32 of everything before it */
#define PICO_MAX_CALL_DEPTH	10000
#define PICO_MAX_STACK		(1u << 20)	/* values */
#define PICO_MAX_SOURCE		(1u << 20)	/* bytes */
#define PICO_MAX_IMAGE		(4u << 20)

/* The first bytes of every executable: DEL 'A' 'L', then the version. */
#define PICO_MAGIC		"\x7f" "AL"	/* from when the language was called a */

/* ------------------------------------------------------------ values */

struct pico_obj;

/*
 * Values carry no type tag: the compiler knows every type, so the bytecode
 * uses typed instructions instead. 4 bytes on the ESP32-S3.
 */
union pico_val {
	int32_t		 i;
	float		 f;
	struct pico_obj	*o;
};

enum pico_otype {
	OT_STR,
	OT_ARRAY,
	OT_STRUCT,
	OT_FILE,
};

/* What an array element or a struct field holds, for defaults and freeing. */
enum pico_kind {
	K_INT,
	K_FLOAT,
	K_BOOL,
	K_STR,
	K_REF,		/* array, struct or File: may be null */
};

#define KIND_IS_REF(k)	((k) >= K_STR)

struct pico_obj {
	uint32_t	refs;
	uint8_t		type;		/* enum pico_otype */
	uint8_t		kind;		/* arrays: element kind */
	uint16_t	sid;		/* structs: index into the struct table */
};

struct pico_str {
	struct pico_obj	h;
	uint32_t	len;
	uint32_t	cap;		/* bytes available in data, excluding the NUL */
	char		data[];		/* always NUL-terminated */
};

struct pico_array {
	struct pico_obj	 h;
	uint32_t	 len;
	uint32_t	 cap;
	union pico_val	*items;
	struct pico_obj	*dead;		/* link while being freed */
};

struct pico_struct {
	struct pico_obj	 h;
	struct pico_obj	*dead;
	union pico_val	 fields[];
};

struct pico_file {
	struct pico_obj	 h;
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
	PICO_KEY_NONE = -4,	/* timeout */
	PICO_KEY_INTR = -3,
	PICO_KEY_ERROR = -2,
	PICO_KEY_EOF = -1,
	PICO_KEY_UP = 0x100,
	PICO_KEY_DOWN,
	PICO_KEY_LEFT,
	PICO_KEY_RIGHT,
	PICO_KEY_HOME,
	PICO_KEY_END,
	PICO_KEY_PGUP,
	PICO_KEY_PGDN,
	PICO_KEY_INSERT,
	PICO_KEY_DELETE,
	PICO_KEY_ESC,
	PICO_KEY_F1,
	PICO_KEY_F2,
	PICO_KEY_F3,
	PICO_KEY_F4,
	PICO_KEY_UNKNOWN,
};

/* ------------------------------------------------------------ bytecode */

/*
 * X(name, operands, pops, pushes). Operands: b u8, c i8, w u16, j i16 jump
 * offset from the end of the instruction, i i32, f f32. -1: depends on the
 * operands (calls). Jumps whose name starts with LOOP go backwards and poll
 * for Ctrl-C.
 */
#define PICO_OPS(X)							\
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

enum pico_op {
#define X(name, fmt, pop, push) OP_##name,
	PICO_OPS(X)
#undef X
	OP_COUNT
};

struct pico_opinfo {
	const char	*name;
	const char	*fmt;
	int8_t		 pop;
	int8_t		 push;
	uint8_t		 size;		/* opcode plus operands */
};

extern const struct pico_opinfo pico_opinfo[OP_COUNT];

/*
 * Fused instructions. After a program is verified, pico_quicken() rewrites the
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
#define PICO_QCC(X, form, b)						\
	X(form, b, EQ, ==) X(form, b, NE, !=) X(form, b, LT, <)		\
	X(form, b, LE, <=) X(form, b, GT, >) X(form, b, GE, >=)
#define PICO_QCMPS(X)							\
	PICO_QCC(X, J, L) PICO_QCC(X, J, K) PICO_QCC(X, J, W) PICO_QCC(X, J, N)	\
	PICO_QCC(X, P, L) PICO_QCC(X, P, K) PICO_QCC(X, P, W) PICO_QCC(X, P, N)	\
	PICO_QCC(X, I, L) PICO_QCC(X, I, K) PICO_QCC(X, I, W) PICO_QCC(X, I, N)

/* Arithmetic on locals: "LOAD a; LOAD b | CONST8 k; op" pushes the result,
 * and with "; STORE c" after it, stores it. "op; STORE c" pops into c. */
#define PICO_QINT(X)	X(ADD) X(SUB) X(MUL)
#define PICO_QFLOAT(X)	X(ADDF) X(SUBF) X(MULF)

/* "LOAD a; X" for other common X: push, then run X's own handler. */
#define PICO_QLOAD(X)							\
	X(LOAD) X(CONST8) X(CONST32) X(CONSTF) X(I2F) X(IDXL) X(LENL)	\
	X(GETFL) X(CALL) X(STORE) X(ADD) X(SUB) X(MUL) X(ADDF) X(SUBF)	\
	X(MULF) X(DIV) X(MOD)

enum pico_qop {
	Q_FIRST = OP_COUNT - 1,
#define X(form, b, cc, op) Q_##form##b##cc,
	PICO_QCMPS(X)
#undef X
#define X(op) Q_LL##op, Q_LK##op, Q_LL##op##_ST, Q_LK##op##_ST, Q_##op##_ST,
	PICO_QINT(X)
#undef X
#define X(op) Q_LL##op, Q_LL##op##_ST, Q_##op##_ST,
	PICO_QFLOAT(X)
#undef X
#define X(op) Q_L_##op,
	PICO_QLOAD(X)
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
#define PICO_BUILTINS(X)							\
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
	X(term_rows, "term_rows", ":i")					\
	X(chars, "chars", "s:S")					\
	X(output, "output", "*")					\
	X(outputa, "@outputa", "S:s")					\
	X(http_get, "http_get", "s:s")					\
	X(json_get, "json_get", "ss:s")

enum pico_builtin {
#define X(cname, name, sig) B_##cname,
	PICO_BUILTINS(X)
#undef X
	B_COUNT
};

struct pico_builtin_info {
	const char	*name;
	const char	*sig;
};

extern const struct pico_builtin_info pico_builtins[B_COUNT];

/* ------------------------------------------------------------ programs */

struct pico_func {
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

struct pico_field {
	uint16_t	name;
	uint16_t	desc;		/* string index of the type descriptor */
	uint8_t		kind;
};

struct pico_sdef {
	uint16_t	name;
	uint16_t	nfields;
	uint32_t	first;		/* index into fields */
};

struct pico_prog {
	uint16_t	 nstrings;
	uint16_t	 nstructs;
	uint16_t	 nglobals;
	uint16_t	 nfuncs;
	uint16_t	 main;
	uint16_t	 init;		/* 0xffff: no global initialisers */
	uint16_t	 source;	/* string index of the source file name */
	struct pico_str	**strings;
	struct pico_sdef	*structs;
	struct pico_field	*fields;
	uint8_t		*global_kinds;
	struct pico_func	*funcs;
	const uint8_t	*lines;		/* pairs of u16 pc, u16 line */
	const uint8_t	*code;
	uint32_t	 code_len;
	uint8_t		*image;		/* owned copy of the file */
	size_t		 image_len;
};

#define PICO_NO_FUNC	0xffff

/* ------------------------------------------------------------ memory */

#define PICO_POOL_CLASSES	8

struct pico_heap {
	void		*free[PICO_POOL_CLASSES];
	void		*chunks;	/* linked list of pool chunks */
	char		*bump;
	char		*bump_end;
	size_t		 objects;	/* live objects, for the leak check */
	size_t		 bytes;
	size_t		 peak;
};

/* ------------------------------------------------------------ vm */

struct pico_frame {
	const uint8_t	*ip;		/* return address, NULL for the bottom frame */
	union pico_val	*bp;
	uint16_t	 fn;
};

#define PICO_OUTBUF	1024

struct pico_vm {
	struct pico_prog	 prog;
	struct pico_heap	 heap;
	union pico_val	*stack;
	size_t		 stack_cap;
	struct pico_frame	*frames;
	uint32_t	 nframes;
	uint32_t	 frames_cap;
	union pico_val	*globals;
	struct pico_str	*empty;
	struct pico_file	*std[3];
	uint32_t	 rng;
	bool		 raw;
	bool		 halted;	/* exit() called */
	bool		 failed;	/* runtime error reported */
	bool		 line_buffered;
	bool		 quickened;	/* pico_quicken ran: opcodes may be fused */
	int		 status;
	const uint8_t	*ip;		/* where an error happened */
	uint16_t	 fn;
	uint16_t	 outlen;
	char		 out[PICO_OUTBUF];
};

/* ------------------------------------------------------------ compiler */

struct pico_image {
	uint8_t		*data;
	size_t		 len;
};

/* Compile source; errors go to stderr. Returns 0 or -1. */
int	pico_compile(const char *filename, const char *src, size_t len, struct pico_image *out);

/* image.c */
uint32_t pico_crc32(const uint8_t *data, size_t len);
int	pico_load(struct pico_vm *vm, const uint8_t *data, size_t len, char *err, size_t errlen);
void	pico_disasm(struct pico_vm *vm);	/* before pico_quicken */

/* quicken.c: fuse common sequences, once pico_load has verified the program */
void	pico_quicken(struct pico_vm *vm);

/* vm.c */
struct pico_vm *pico_vm_new(void);
int	pico_vm_run(struct pico_vm *vm, int argc, char **argv);
void	pico_vm_free(struct pico_vm *vm, bool leak_check);
int	pico_vm_exec(struct pico_vm *vm, uint16_t fn, union pico_val *result);

/* runtime.c */
void	*pico_alloc(struct pico_vm *vm, size_t n);
void	 pico_free(struct pico_vm *vm, void *p, size_t n);
void	*pico_grow(struct pico_vm *vm, void *p, size_t old, size_t n);
void	 pico_heap_release(struct pico_vm *vm);
void	 pico_obj_free(struct pico_vm *vm, struct pico_obj *o);
struct pico_str *pico_str_new(struct pico_vm *vm, const char *s, size_t len);
struct pico_str *pico_str_alloc(struct pico_vm *vm, size_t len);
struct pico_str *pico_str_concat(struct pico_vm *vm, struct pico_str *a, struct pico_str *b);
struct pico_str *pico_str_append(struct pico_vm *vm, struct pico_str *a, struct pico_str *b);
int	 pico_str_cmp(const struct pico_str *a, const struct pico_str *b);
struct pico_str *pico_tostr(struct pico_vm *vm, union pico_val v, int kind);
int	 pico_fmt_int(char *out, int32_t v);	/* decimal, no NUL; at most 11 bytes */
int32_t	 pico_float_to_int(float f);
struct pico_str *pico_tostr_desc(struct pico_vm *vm, union pico_val v, const struct pico_str *desc);
struct pico_array *pico_array_new(struct pico_vm *vm, int kind, uint32_t cap);
bool	 pico_array_push(struct pico_vm *vm, struct pico_array *a, union pico_val v);
struct pico_struct *pico_struct_new(struct pico_vm *vm, uint16_t sid);
struct pico_file *pico_file_new(struct pico_vm *vm, int fd, bool std);
void	 pico_error(struct pico_vm *vm, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void	 pico_flush(struct pico_vm *vm);
int	 pico_out(struct pico_vm *vm, int fd, const char *s, size_t n);
int	 pico_eprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int	 pico_line_of(const struct pico_prog *p, uint16_t fn, const uint8_t *ip);

struct pico_fmt {
	char	*buf;
	size_t	 len;
	size_t	 cap;
	bool	 oom;
};

int	 pico_fmt_next(const char **f, const char *end, const char **spec);
void	 pico_fmt_puts(struct pico_vm *vm, struct pico_fmt *f, const char *s, size_t n);
void	 pico_fmt_value(struct pico_vm *vm, struct pico_fmt *f, union pico_val v, const char **desc, int depth, bool quote);

/* builtins.c */
typedef int (*pico_builtin_fn)(struct pico_vm *vm, union pico_val *args, int argc);
extern const pico_builtin_fn pico_builtin_fns[B_COUNT];
int	 pico_readkey(struct pico_vm *vm, int timeout_ms);

static inline void pico_incref(struct pico_obj *o)
{
	if (o)
		o->refs++;
}

static inline void pico_decref(struct pico_vm *vm, struct pico_obj *o)
{
	if (o && --o->refs == 0)
		pico_obj_free(vm, o);
}

static inline uint16_t pico_u16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static inline uint32_t pico_u32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
