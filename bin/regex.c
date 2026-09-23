/*
 * Regular expressions. See regex.h for what they take.
 *
 * The pattern is parsed into a tree, and the tree compiled into a program
 * for a Pike VM (Thompson's construction, as in Russ Cox's articles): every
 * way the pattern could be matching is a thread, all of them move through
 * the text one character at a time together, and two threads that reach
 * the same instruction at the same place are one thread. So there is no
 * backtracking, and a match costs at most the program's length for each
 * character of text.
 *
 * Leftmost-longest: threads started earlier keep priority over later ones,
 * and a match is only replaced by one that starts earlier or, starting at
 * the same place, ends later. The groups are those of the thread that got
 * there first -- POSIX's own rules for which of two equally long matches
 * sets a group are a famously tangled corner, and nothing here depends on
 * them.
 *
 * The VM steps over characters, not bytes: text is decoded as UTF-8 as it
 * goes, which is what lets . and [^x] take a whole é. A byte that is not
 * UTF-8 is a character of its own that only . and [^...] match.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pt/match.h"
#include "pt/sys.h"
#include "regex.h"

#define MAX_INST	8000	/* a program bigger than this is refused */
#define MAX_REPEAT	255	/* RE_DUP_MAX */
#define BAD_BYTE	0x110000	/* + the byte: text that is not UTF-8 */

/* ------------------------------------------------------------ characters */

/* The character at s[pos]: its length in bytes, and it, in *cp. */
static int decode(const unsigned char *s, size_t len, size_t pos, uint32_t *cp)
{
	unsigned c = s[pos];
	int n;

	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if (c >= 0xc2 && c <= 0xdf)
		n = 2;
	else if (c >= 0xe0 && c <= 0xef)
		n = 3;
	else if (c >= 0xf0 && c <= 0xf4)
		n = 4;
	else
		goto bad;
	if (pos + n > len)
		goto bad;
	uint32_t v = c & (0x7f >> n);

	for (int i = 1; i < n; i++) {
		unsigned k = s[pos + i];

		if ((k & 0xc0) != 0x80)
			goto bad;
		v = v << 6 | (k & 0x3f);
	}
	/* too long an encoding, a surrogate or past the end of Unicode */
	if ((n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > 0x10ffff)) ||
	    (v >= 0xd800 && v <= 0xdfff))
		goto bad;
	*cp = v;
	return n;
bad:
	*cp = BAD_BYTE + c;
	return 1;
}

/* Letters of Latin-1 fold too, so -i finds "É" for "é". */
static uint32_t fold(uint32_t c)
{
	if (c >= 'A' && c <= 'Z')
		return c + 32;
	if (c >= 0xc0 && c <= 0xde && c != 0xd7)
		return c + 32;
	return c;
}

static uint32_t unfold(uint32_t c)
{
	if (c >= 'a' && c <= 'z')
		return c - 32;
	if (c >= 0xe0 && c <= 0xfe && c != 0xf7)
		return c - 32;
	return c;
}

static bool is_word(uint32_t c)
{
	return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_' ||
	       (c >= 0xc0 && c <= 0xff && c != 0xd7 && c != 0xf7);
}

/* ------------------------------------------------------------ classes */

/*
 * A bracket expression: which of the first 256 characters are in it, as a
 * bitmap, and ranges for anything above -- then whether it is negated.
 */
struct range {
	uint32_t	lo, hi;
};

struct cls {
	uint8_t		map[32];
	struct range	*ranges;
	int		nranges;
	bool		negate;
};

static bool cls_has(const struct cls *k, uint32_t c)
{
	bool in = false;

	if (c < 256) {
		in = k->map[c >> 3] >> (c & 7) & 1;
	} else {
		for (int i = 0; i < k->nranges && !in; i++)
			in = c >= k->ranges[i].lo && c <= k->ranges[i].hi;
	}
	return in != k->negate;
}

/* ------------------------------------------------------------ the tree */

enum {
	N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_WBEG, N_WEND,
	N_CAT, N_ALT, N_REPEAT, N_GROUP,
};

/*
 * A concatenation or an alternation holds its parts as a list, a -> next
 * -> next, rather than as a tree two at a time: a pattern a thousand
 * characters long is then a list a thousand long, not a tree a thousand
 * deep for the code generator to recurse through on a small stack. Only
 * real nesting -- groups inside groups -- recurses, and that is limited.
 */
struct node {
	int		 type;
	uint32_t	 c;		/* N_CHAR */
	int		 cls;		/* N_CLASS: index into classes */
	int		 min, max;	/* N_REPEAT; max -1 is unbounded */
	int		 group;		/* N_GROUP */
	struct node	*a;		/* the first part, or the one inside */
	struct node	*next;		/* the part after this one in its list */
};

#define MAX_DEPTH	32		/* groups inside groups */

enum {
	I_CHAR, I_ANY, I_CLASS, I_BOL, I_EOL, I_WORDB, I_NWORDB, I_WBEG, I_WEND,
	I_SPLIT, I_JMP, I_SAVE, I_MATCH,
};

struct inst {
	uint8_t		op;
	uint32_t	arg;		/* the character, the class, or the slot */
	int		x, y;		/* jump targets */
};

struct regex {
	int		 flags, groups, nslots;
	struct inst	*prog;
	int		 n, cap;
	struct cls	*classes;
	int		 nclasses;
	struct node	*nodes;
	int		 nnodes, capnodes;
	/* a pattern that is only ordinary characters is searched for as such */
	char		*literal;
	size_t		 literal_len;
	/*
	 * The bytes a match can start with, so that the VM is only started
	 * where one could: the rest of a line is skipped a byte at a time,
	 * which is most of the work on a line that does not match. Not used
	 * when the pattern can match nothing at all, or starts with anything.
	 */
	uint8_t		 first[32];		/* ASCII, and 0x80 for everything above */
	bool		 skip;
	/* the VM's working memory, sized when the program is built */
	int		*pcs[2];
	long		*caps[2];
	int		 counts[2];
	unsigned	*mark;
	unsigned	 gen;
	long		*scratch;
	struct frame {
		int	 pc;		/* -1: put caps[slot] back to old */
		int	 slot;
		long	 old;
	}		*stack;
};

struct parser {
	struct regex	*re;
	const char	*p, *end;
	int		 flags;
	char		*err;
	size_t		 errlen;
	bool		 failed;
	int		 depth;
};

static void fail(struct parser *ps, const char *fmt, ...)
{
	va_list ap;

	if (ps->failed)
		return;
	ps->failed = true;
	va_start(ap, fmt);
	if (ps->err && ps->errlen)
		vsnprintf(ps->err, ps->errlen, fmt, ap);
	va_end(ap);
}

static struct node *node(struct parser *ps, int type)
{
	struct regex *re = ps->re;

	if (ps->failed)
		return NULL;
	if (re->nnodes == re->capnodes) {
		int cap = re->capnodes ? re->capnodes * 2 : 64;
		struct node *grown = pt_realloc(re->nodes, cap * sizeof(*grown));

		if (!grown) {
			fail(ps, "out of memory");
			return NULL;
		}
		/* the tree points into the array: move the pointers with it */
		for (int i = 0; i < re->nnodes; i++) {
			if (grown[i].a)
				grown[i].a = grown + (grown[i].a - re->nodes);
			if (grown[i].next)
				grown[i].next = grown + (grown[i].next - re->nodes);
		}
		re->nodes = grown;
		re->capnodes = cap;
	}
	struct node *n = &re->nodes[re->nnodes++];

	memset(n, 0, sizeof(*n));
	n->type = type;
	return n;
}

/* Nodes are referred to by index while the array may still move. */
#define IDX(ps, n)	((n) ? (int)((n) - (ps)->re->nodes) : -1)
#define AT(ps, i)	((i) >= 0 ? &(ps)->re->nodes[i] : NULL)

/* A node of `type` around the one at index `inner`: its index, or -1. */
static int wrap(struct parser *ps, int type, int inner)
{
	struct node *n = node(ps, type);

	if (!n)
		return -1;
	n->a = AT(ps, inner);
	return IDX(ps, n);
}

/* Adds `part` to the list node `list` (N_CAT or N_ALT); `*last` is its end. */
static void append(struct parser *ps, int list, int *last, int part)
{
	if (ps->failed || list < 0 || part < 0)
		return;
	if (*last < 0)
		ps->re->nodes[list].a = AT(ps, part);
	else
		ps->re->nodes[*last].next = AT(ps, part);
	*last = part;
}

static int new_class(struct parser *ps)
{
	struct regex *re = ps->re;
	struct cls *grown = pt_realloc(re->classes, (re->nclasses + 1) * sizeof(*grown));

	if (!grown) {
		fail(ps, "out of memory");
		return -1;
	}
	re->classes = grown;
	memset(&grown[re->nclasses], 0, sizeof(grown[0]));
	return re->nclasses++;
}

static void cls_add(struct parser *ps, int k, uint32_t lo, uint32_t hi)
{
	struct cls *c = &ps->re->classes[k];

	for (uint32_t x = lo; x <= hi && x < 256; x++)
		c->map[x >> 3] |= 1 << (x & 7);
	if (hi >= 256) {
		struct range *r = pt_realloc(c->ranges, (c->nranges + 1) * sizeof(*r));

		if (!r) {
			fail(ps, "out of memory");
			return;
		}
		c->ranges = r;
		r[c->nranges++] = (struct range){ lo < 256 ? 256 : lo, hi };
	}
}

/* Both cases of everything in the class, for -i. */
static void cls_fold(struct parser *ps, int k)
{
	struct cls *c = &ps->re->classes[k];

	for (uint32_t x = 0; x < 256; x++)
		if (c->map[x >> 3] >> (x & 7) & 1) {
			uint32_t y = fold(x) == x ? unfold(x) : fold(x);

			c->map[y >> 3] |= 1 << (y & 7);
		}
}

static bool class_char(uint32_t c, const char *name)
{
	if (c < 0x80)
		return pt_char_class(name, (unsigned char)c) == 1;
	if (c >= 0xc0 && c <= 0xff && c != 0xd7 && c != 0xf7) {	/* Latin-1 letters */
		if (!strcmp(name, "alpha") || !strcmp(name, "alnum") || !strcmp(name, "graph") ||
		    !strcmp(name, "print"))
			return true;
		if (!strcmp(name, "upper"))
			return c < 0xe0;
		if (!strcmp(name, "lower"))
			return c >= 0xe0;
	}
	return false;
}

/* The next character of the pattern, decoded, advancing past it. */
static uint32_t pat_char(struct parser *ps)
{
	uint32_t c;

	ps->p += decode((const unsigned char *)ps->p, ps->end - ps->p, 0, &c);
	return c;
}

/* [...]: ps->p is just past the '['. */
static int parse_bracket(struct parser *ps)
{
	int k = new_class(ps);
	bool first = true;

	if (k < 0)
		return -1;
	if (ps->p < ps->end && *ps->p == '^') {
		ps->re->classes[k].negate = true;
		ps->p++;
	}
	for (;;) {
		uint32_t lo, hi;

		if (ps->p >= ps->end) {
			fail(ps, "unmatched [");
			return -1;
		}
		if (*ps->p == ']' && !first) {
			ps->p++;
			break;
		}
		first = false;
		if (ps->p[0] == '[' && ps->p + 1 < ps->end &&
		    (ps->p[1] == ':' || ps->p[1] == '=' || ps->p[1] == '.')) {
			char kind = ps->p[1], name[16];
			const char *close = ps->p + 2;

			while (close + 1 < ps->end && !(close[0] == kind && close[1] == ']'))
				close++;
			if (close + 1 >= ps->end) {
				fail(ps, "unmatched [%c", kind);
				return -1;
			}
			size_t n = close - ps->p - 2;

			if (kind == ':') {
				if (n >= sizeof(name)) {
					fail(ps, "unknown class");
					return -1;
				}
				memcpy(name, ps->p + 2, n);
				name[n] = '\0';
				if (pt_char_class(name, 'a') < 0) {
					fail(ps, "unknown class [:%s:]", name);
					return -1;
				}
				for (uint32_t x = 0; x < 256; x++)
					if (class_char(x, name))
						cls_add(ps, k, x, x);
				ps->p = close + 2;
				continue;
			}
			/* [=c=] and [.c.]: one character stands for itself */
			ps->p += 2;
			lo = pat_char(ps);
			if (ps->p != close) {
				fail(ps, "collating elements are single characters here");
				return -1;
			}
			ps->p = close + 2;
		} else {
			lo = pat_char(ps);
		}
		hi = lo;
		if (ps->p + 1 < ps->end && ps->p[0] == '-' && ps->p[1] != ']') {
			ps->p++;
			hi = pat_char(ps);
			if (hi < lo) {
				fail(ps, "invalid range end");
				return -1;
			}
		}
		cls_add(ps, k, lo, hi);
	}
	if (ps->flags & RE_ICASE)
		cls_fold(ps, k);
	struct node *n = node(ps, N_CLASS);

	if (!n)
		return -1;
	n->cls = k;
	return IDX(ps, n);
}

static int class_node(struct parser *ps, const char *name, bool negate, bool underscore)
{
	int k = new_class(ps);
	struct node *n;

	if (k < 0)
		return -1;
	for (uint32_t x = 0; x < 256; x++)
		if (class_char(x, name) || (underscore && x == '_'))
			cls_add(ps, k, x, x);
	ps->re->classes[k].negate = negate;
	if (!(n = node(ps, N_CLASS)))
		return -1;
	n->cls = k;
	return IDX(ps, n);
}

static int char_node(struct parser *ps, uint32_t c)
{
	if (ps->flags & RE_ICASE && fold(c) != unfold(c)) {
		int k = new_class(ps);
		struct node *n;

		if (k < 0)
			return -1;
		cls_add(ps, k, fold(c), fold(c));
		cls_add(ps, k, unfold(c), unfold(c));
		if (!(n = node(ps, N_CLASS)))
			return -1;
		n->cls = k;
		return IDX(ps, n);
	}
	struct node *n = node(ps, N_CHAR);

	if (!n)
		return -1;
	n->c = c;
	return IDX(ps, n);
}

static int parse_alt(struct parser *ps);

static bool ere(const struct parser *ps)
{
	return ps->flags & RE_EXTENDED;
}

/* At p: an operator that ends a concatenation -- | or ), in either syntax. */
static bool at_alt_or_close(const struct parser *ps, const char *p)
{
	if (p >= ps->end)
		return true;
	if (ere(ps))
		return *p == '|' || (*p == ')' && ps->depth > 0);
	return p[0] == '\\' && p + 1 < ps->end && (p[1] == '|' || (p[1] == ')' && ps->depth > 0));
}

/* {m}, {m,} or {m,n}, with p after the brace: true if it is one. */
static bool parse_interval(struct parser *ps, int *min, int *max)
{
	const char *p = ps->p;
	int a = -1, b;

	if (p < ps->end && *p >= '0' && *p <= '9')
		for (a = 0; p < ps->end && *p >= '0' && *p <= '9'; p++)
			a = a * 10 + (*p - '0') > 1000 ? 1001 : a * 10 + (*p - '0');
	else if (p < ps->end && *p == ',')
		a = 0;				/* {,n}: GNU's, and what people type */
	b = a;
	if (p < ps->end && *p == ',') {
		p++;
		b = -1;
		if (p < ps->end && *p >= '0' && *p <= '9')
			for (b = 0; p < ps->end && *p >= '0' && *p <= '9'; p++)
				b = b * 10 + (*p - '0') > 1000 ? 1001 : b * 10 + (*p - '0');
	}
	if (a < 0)
		return false;
	if (ere(ps)) {
		if (p >= ps->end || *p != '}')
			return false;
		p++;
	} else {
		if (p + 1 >= ps->end || p[0] != '\\' || p[1] != '}')
			return false;
		p += 2;
	}
	if (a > MAX_REPEAT || b > MAX_REPEAT || (b >= 0 && b < a)) {
		fail(ps, "invalid repetition count");
		return false;
	}
	*min = a;
	*max = b;
	ps->p = p;
	return true;
}

/* One atom: a character, a class, an anchor, a group. -1 on an error. */
static int parse_atom(struct parser *ps, bool at_start)
{
	char c = *ps->p;

	if (c == '.') {
		ps->p++;
		return IDX(ps, node(ps, N_ANY));
	}
	if (c == '[') {
		ps->p++;
		return parse_bracket(ps);
	}
	/* ^ is an anchor anywhere in an ERE; in a BRE only at the start */
	if (c == '^' && (ere(ps) || at_start)) {
		ps->p++;
		return IDX(ps, node(ps, N_BOL));
	}
	/* $ likewise: in a BRE only at the end, or before \) or \| */
	if (c == '$' && (ere(ps) || at_alt_or_close(ps, ps->p + 1))) {
		ps->p++;
		return IDX(ps, node(ps, N_EOL));
	}
	if (ere(ps) && c == '(') {
		ps->p++;
		goto group;
	}
	if (c == '\\' && ps->p + 1 < ps->end) {
		char e = ps->p[1];

		ps->p += 2;
		if (!ere(ps) && e == '(')
			goto group;
		switch (e) {
		case 'w': return class_node(ps, "alnum", false, true);
		case 'W': return class_node(ps, "alnum", true, true);
		case 's': return class_node(ps, "space", false, false);
		case 'S': return class_node(ps, "space", true, false);
		case 'b': return IDX(ps, node(ps, N_WORDB));
		case 'B': return IDX(ps, node(ps, N_NWORDB));
		case '<': return IDX(ps, node(ps, N_WBEG));
		case '>': return IDX(ps, node(ps, N_WEND));
		case 'n': return char_node(ps, '\n');
		case 't': return char_node(ps, '\t');
		}
		if (e >= '1' && e <= '9') {
			fail(ps, "back-references (\\%c) are not supported in patterns", e);
			return -1;
		}
		ps->p--;			/* an escaped character is itself */
		return char_node(ps, pat_char(ps));
	}
	if (c == '\\') {
		fail(ps, "trailing backslash");
		return -1;
	}
	return char_node(ps, pat_char(ps));

group:
	/* past \9 a group still groups, but nothing can refer to it: it is
	 * not saved, which also keeps every thread's copy of them small */
	int group = ps->re->groups < RE_MAXSUB ? ++ps->re->groups : 0, inner;

	if (ps->depth == MAX_DEPTH) {
		fail(ps, "groups nested too deeply");
		return -1;
	}
	ps->depth++;
	inner = parse_alt(ps);
	ps->depth--;
	if (ps->failed)
		return -1;
	if (ere(ps) ? (ps->p < ps->end && *ps->p == ')')
		    : (ps->p + 1 < ps->end && ps->p[0] == '\\' && ps->p[1] == ')')) {
		ps->p += ere(ps) ? 1 : 2;
	} else {
		fail(ps, "unmatched %s", ere(ps) ? "(" : "\\(");
		return -1;
	}
	if (!group)
		return inner;
	int g = wrap(ps, N_GROUP, inner);

	if (g >= 0)
		ps->re->nodes[g].group = group;
	return g;
}

/* A quantifier at p, of either syntax: its bounds, or false. */
static bool quantifier(struct parser *ps, int *min, int *max)
{
	const char *p = ps->p;

	if (p >= ps->end)
		return false;
	if (*p == '*') {
		ps->p++;
		*min = 0;
		*max = -1;
		return true;
	}
	if (ere(ps)) {
		if (*p == '+' || *p == '?') {
			ps->p++;
			*min = *p == '+';
			*max = *p == '+' ? -1 : 1;
			return true;
		}
		if (*p == '{') {
			ps->p++;
			if (parse_interval(ps, min, max))
				return true;
			ps->p = p;		/* not an interval: { is a character */
		}
		return false;
	}
	if (p + 1 < ps->end && p[0] == '\\') {
		if (p[1] == '+' || p[1] == '?') {
			ps->p += 2;
			*min = p[1] == '+';
			*max = p[1] == '+' ? -1 : 1;
			return true;
		}
		if (p[1] == '{') {
			ps->p += 2;
			if (parse_interval(ps, min, max))
				return true;
			if (!ps->failed)
				fail(ps, "invalid \\{ interval");
		}
	}
	return false;
}

static int parse_concat(struct parser *ps)
{
	int list = IDX(ps, node(ps, N_CAT)), last = -1;
	bool at_start = true;

	while (!ps->failed && !at_alt_or_close(ps, ps->p)) {
		int atom, min, max;

		/* a quantifier with nothing before it is an ordinary character */
		if (at_start && (*ps->p == '*' || (ere(ps) && (*ps->p == '+' || *ps->p == '?'))))
			atom = char_node(ps, pat_char(ps));
		else
			atom = parse_atom(ps, at_start);
		if (atom < 0)
			return -1;
		/* in a BRE, * straight after a leading ^ is a character too */
		if (!ere(ps) && at_start && ps->re->nodes[atom].type == N_BOL &&
		    ps->p < ps->end && *ps->p == '*') {
			append(ps, list, &last, atom);
			atom = char_node(ps, pat_char(ps));
		}
		for (int stacked = 0; !ps->failed && quantifier(ps, &min, &max); stacked++) {
			/* a** is a*, but a*{2}{3}... nests, and the code generator
			 * recurses once for each: a few are plenty */
			if (stacked == 4) {
				fail(ps, "too many repetitions in a row");
				return -1;
			}
			int r = wrap(ps, N_REPEAT, atom);

			if (r < 0)
				return -1;
			ps->re->nodes[r].min = min;
			ps->re->nodes[r].max = max;
			atom = r;
		}
		if (ps->failed)
			return -1;
		append(ps, list, &last, atom);
		at_start = false;
	}
	return ps->failed ? -1 : list;
}

static int parse_alt(struct parser *ps)
{
	int list = IDX(ps, node(ps, N_ALT)), last = -1;

	append(ps, list, &last, parse_concat(ps));
	while (!ps->failed && ps->p < ps->end) {
		if (ere(ps) && *ps->p == '|')
			ps->p++;
		else if (!ere(ps) && ps->p + 1 < ps->end && ps->p[0] == '\\' && ps->p[1] == '|')
			ps->p += 2;
		else
			break;
		append(ps, list, &last, parse_concat(ps));
	}
	return ps->failed ? -1 : list;
}

/* ------------------------------------------------------------ compiling */

static int emit(struct regex *re, int op, uint32_t arg)
{
	if (re->n == re->cap) {
		int cap = re->cap ? re->cap * 2 : 64;
		struct inst *grown;

		if (re->n >= MAX_INST || !(grown = pt_realloc(re->prog, cap * sizeof(*grown))))
			return -1;
		re->prog = grown;
		re->cap = cap;
	}
	re->prog[re->n] = (struct inst){ .op = op, .arg = arg };
	return re->n++;
}

/* Points every jump in a chain linked through .x (or .y) at `to`. */
static void fix_chain(struct regex *re, int at, int to)
{
	while (at >= 0) {
		int next = re->prog[at].x;

		re->prog[at].x = to;
		at = next;
	}
}

static void fix_chain_y(struct regex *re, int at, int to)
{
	while (at >= 0) {
		int next = re->prog[at].y;

		re->prog[at].y = to;
		at = next;
	}
}

static int gen(struct regex *re, const struct node *n)
{
	int at, split, pending;

	switch (n->type) {
	case N_CHAR:	return emit(re, I_CHAR, n->c) < 0 ? -1 : 0;
	case N_ANY:	return emit(re, I_ANY, 0) < 0 ? -1 : 0;
	case N_CLASS:	return emit(re, I_CLASS, n->cls) < 0 ? -1 : 0;
	case N_BOL:	return emit(re, I_BOL, 0) < 0 ? -1 : 0;
	case N_EOL:	return emit(re, I_EOL, 0) < 0 ? -1 : 0;
	case N_WORDB:	return emit(re, I_WORDB, 0) < 0 ? -1 : 0;
	case N_NWORDB:	return emit(re, I_NWORDB, 0) < 0 ? -1 : 0;
	case N_WBEG:	return emit(re, I_WBEG, 0) < 0 ? -1 : 0;
	case N_WEND:	return emit(re, I_WEND, 0) < 0 ? -1 : 0;
	case N_CAT:
		for (const struct node *part = n->a; part; part = part->next)
			if (gen(re, part))
				return -1;
		return 0;
	case N_GROUP:
		if (emit(re, I_SAVE, 2 * n->group) < 0 || gen(re, n->a) ||
		    emit(re, I_SAVE, 2 * n->group + 1) < 0)
			return -1;
		return 0;
	case N_ALT:
		/*
		 * split a, next; a; jmp end; next: split b, next2; b; jmp end;
		 * ... the last alternative on its own. The jumps still to be
		 * pointed at the end are chained through their own targets.
		 */
		pending = -1;
		for (const struct node *alt = n->a; alt; alt = alt->next) {
			if (!alt->next)
				return gen(re, alt) ? -1 : (fix_chain(re, pending, re->n), 0);
			if ((split = emit(re, I_SPLIT, 0)) < 0)
				return -1;
			re->prog[split].x = re->n;
			if (gen(re, alt) || (at = emit(re, I_JMP, 0)) < 0)
				return -1;
			re->prog[at].x = pending;
			pending = at;
			re->prog[split].y = re->n;
		}
		return 0;
	case N_REPEAT:
		for (int i = 0; i < n->min; i++)
			if (gen(re, n->a))
				return -1;
		if (n->max < 0) {		/* L: split body, out; body; jmp L */
			if ((at = emit(re, I_SPLIT, 0)) < 0)
				return -1;
			re->prog[at].x = re->n;
			if (gen(re, n->a) || emit(re, I_JMP, 0) < 0)
				return -1;
			re->prog[re->n - 1].x = at;
			re->prog[at].y = re->n;
			return 0;
		}
		/* up to max - min more, each optional, all leaving to the end */
		pending = -1;
		for (int i = n->min; i < n->max; i++) {
			if ((split = emit(re, I_SPLIT, 0)) < 0)
				return -1;
			re->prog[split].x = re->n;
			re->prog[split].y = pending;
			pending = split;
			if (gen(re, n->a))
				return -1;
		}
		fix_chain_y(re, pending, re->n);
		return 0;
	}
	return 0;				/* N_EMPTY */
}

/* ------------------------------------------------------------ the VM */

static void first_add(struct regex *re, unsigned b)
{
	re->first[b >> 3] |= 1 << (b & 7);
}

static bool first_has(const struct regex *re, unsigned b)
{
	return re->first[b >> 3] >> (b & 7) & 1;
}

static bool word_at(const unsigned char *s, size_t len, size_t pos)
{
	uint32_t c;

	return pos < len && (decode(s, len, pos, &c), is_word(c));
}

static bool word_before(const unsigned char *s, size_t pos)
{
	size_t start = pos;
	uint32_t c;

	if (!pos)
		return false;
	/* back to the start of the character before */
	while (start > 0 && pos - start < 4 && (s[start - 1] & 0xc0) == 0x80)
		start--;
	if (start > 0)
		start--;
	decode(s, pos, start, &c);
	return is_word(c);
}

/*
 * Adds the thread at `pc`, following jumps, splits, saves and assertions
 * to the instructions that consume a character (or match), in priority
 * order, each at most once. `caps` is the thread's groups, borrowed and
 * given back as found.
 */
static void add(struct regex *re, int list, int pc0, long *caps, const unsigned char *s,
		size_t len, size_t pos, bool notbol)
{
	struct frame *st = re->stack;
	int top = 0;

	st[top++] = (struct frame){ .pc = pc0 };
	while (top) {
		struct frame f = st[--top];

		if (f.pc < 0) {
			caps[f.slot] = f.old;
			continue;
		}
		int pc = f.pc;

		if (re->mark[pc] == re->gen)
			continue;
		re->mark[pc] = re->gen;

		const struct inst *in = &re->prog[pc];
		bool pass;

		switch (in->op) {
		case I_JMP:
			st[top++] = (struct frame){ .pc = in->x };
			continue;
		case I_SPLIT:
			st[top++] = (struct frame){ .pc = in->y };
			st[top++] = (struct frame){ .pc = in->x };	/* first */
			continue;
		case I_SAVE:
			if ((int)in->arg < re->nslots) {
				st[top++] = (struct frame){ .pc = -1, .slot = in->arg, .old = caps[in->arg] };
				caps[in->arg] = (long)pos;
			}
			st[top++] = (struct frame){ .pc = pc + 1 };
			continue;
		case I_BOL:
			pass = pos == 0 && !notbol;
			break;
		case I_EOL:
			pass = pos == len;
			break;
		case I_WORDB:
		case I_NWORDB:
			pass = (word_before(s, pos) != word_at(s, len, pos)) == (in->op == I_WORDB);
			break;
		case I_WBEG:
			pass = !word_before(s, pos) && word_at(s, len, pos);
			break;
		case I_WEND:
			pass = word_before(s, pos) && !word_at(s, len, pos);
			break;
		default: {			/* consumes a character, or matches */
			int k = re->counts[list]++;

			re->pcs[list][k] = pc;
			memcpy(&re->caps[list][k * re->nslots], caps, re->nslots * sizeof(long));
			continue;
		}
		}
		if (pass)
			st[top++] = (struct frame){ .pc = pc + 1 };
	}
}

static bool matches(const struct regex *re, const struct inst *in, uint32_t c)
{
	switch (in->op) {
	case I_CHAR:	return c == in->arg;
	case I_ANY:	return true;
	case I_CLASS:	return cls_has(&re->classes[in->arg], c);
	}
	return false;
}

static void set_match(const struct regex *re, const long *caps, struct re_match *m)
{
	for (int i = 0; i <= RE_MAXSUB; i++) {
		m->so[i] = 2 * i + 1 < re->nslots ? caps[2 * i] : -1;
		m->eo[i] = 2 * i + 1 < re->nslots ? caps[2 * i + 1] : -1;
		if (m->so[i] < 0 || m->eo[i] < 0)
			m->so[i] = m->eo[i] = -1;
	}
}

static bool literal_search(const struct regex *re, const char *s, size_t len, size_t from,
			   struct re_match *m)
{
	size_t n = re->literal_len;

	for (size_t i = from; i + n <= len; i++) {
		const char *hit = memchr(s + i, re->literal[0], len - n + 1 - i);

		if (!hit)
			return false;
		i = hit - s;
		if (!memcmp(hit, re->literal, n)) {
			if (m) {
				memset(m, -1, sizeof(*m));
				m->so[0] = (long)i;
				m->eo[0] = (long)(i + n);
			}
			return true;
		}
	}
	return false;
}

/*
 * The VM. Threads start at every place from `from` on until something has
 * matched -- or, `anchored`, only at `from` -- and a match is only taken if
 * it ends by `limit`: the text past it is still there for $ and \> to see.
 * With no `m`, only whether there is a match is wanted, and the first one
 * found will do.
 */
static bool run(struct regex *re, const unsigned char *s, size_t len, size_t from, size_t limit,
		bool anchored, bool notbol, struct re_match *m)
{
	long *caps = re->scratch;
	long best_start = -1, best_end = -1;
	int cur = 0;

	re->counts[0] = re->counts[1] = 0;
	for (size_t pos = from;;) {
		/* nothing under way: on to where a match could start */
		if (!re->counts[cur] && best_start < 0 && re->skip && !anchored)
			while (pos < len && !first_has(re, s[pos] < 0x80 ? s[pos] : 0x80))
				pos += s[pos] < 0x80 ? 1 : (size_t)decode(s, len, pos, &(uint32_t){ 0 });

		uint32_t c = 0;
		int step = pos < len ? decode(s, len, pos, &c) : 0;

		/* a new thread here, at the lowest priority, until something matched */
		re->gen++;
		for (int i = 0; i < re->counts[cur]; i++)	/* already queued */
			re->mark[re->pcs[cur][i]] = re->gen;
		if (best_start < 0 && (!anchored || pos == from)) {
			for (int i = 0; i < re->nslots; i++)
				caps[i] = -1;
			add(re, cur, 0, caps, s, len, pos, notbol);
		}
		if (!re->counts[cur]) {
			/* nothing alive: done if something matched, else on to the next place */
			if (best_start >= 0 || !step || anchored || pos >= limit)
				break;
			pos += step;
			continue;
		}

		int next = !cur;

		re->counts[next] = 0;
		re->gen++;
		for (int i = 0; i < re->counts[cur]; i++) {
			const struct inst *in = &re->prog[re->pcs[cur][i]];
			long *tc = &re->caps[cur][i * re->nslots];

			if (best_start >= 0 && tc[0] > best_start)
				continue;		/* started too late to matter */
			if (in->op == I_MATCH) {
				if (!m)
					return true;	/* that there is one is enough */
				if (best_start < 0 || tc[0] < best_start ||
				    (tc[0] == best_start && (long)pos > best_end)) {
					best_start = tc[0];
					best_end = (long)pos;
					if (m)
						set_match(re, tc, m);
				}
				continue;
			}
			if (step && pos < limit && matches(re, in, c)) {
				memcpy(caps, tc, re->nslots * sizeof(long));
				add(re, next, re->pcs[cur][i] + 1, caps, s, len, pos + step, notbol);
			}
		}
		if (!step || pos >= limit)
			break;
		cur = next;
		pos += step;
	}
	if (best_start >= 0 && m) {
		m->so[0] = best_start;
		m->eo[0] = best_end;
	}
	return best_start >= 0;
}

bool re_search(struct regex *re, const char *text, size_t len, size_t from, bool notbol,
	       struct re_match *m)
{
	if (from > len)
		return false;
	if (re->literal)
		return literal_search(re, text, len, from, m);
	return run(re, (const unsigned char *)text, len, from, len, false, notbol, m);
}

long re_longest_at(struct regex *re, const char *text, size_t len, size_t at, size_t limit,
		   bool notbol)
{
	struct re_match m;

	if (at > len || limit < at)
		return -1;
	if (limit > len)
		limit = len;
	if (re->literal)
		return at + re->literal_len <= limit && !memcmp(text + at, re->literal, re->literal_len)
		       ? (long)(at + re->literal_len) : -1;
	return run(re, (const unsigned char *)text, len, at, limit, true, notbol, &m) ? m.eo[0] : -1;
}

/* ------------------------------------------------------------ setting up */

/* A pattern of nothing but ordinary characters needs no VM. */
static char *as_literal(const char *p, int flags, size_t *n)
{
	static const char special_bre[] = ".[\\*^$";
	static const char special_ere[] = ".[\\*^$+?{}()|";

	if (flags & RE_ICASE || !*p)
		return NULL;
	for (const char *q = p; *q; q++)
		if (strchr(flags & RE_EXTENDED ? special_ere : special_bre, *q))
			return NULL;
	*n = strlen(p);
	return pt_strdup(p);
}

/*
 * Follows every way from the start that consumes nothing -- jumps, splits,
 * saves, and assertions, which may fail but never move -- to the first
 * instructions that take a character, and notes what they take. The VM's
 * own stack and marks are the working memory: each instruction is looked
 * at once and pushes at most two more, which is what the stack holds.
 */
static void first_set(struct regex *re)
{
	struct frame *todo = re->stack;
	unsigned *seen = re->mark;
	int n = 0;

	re->skip = true;
	memset(re->first, 0, sizeof(re->first));
	todo[n++].pc = 0;
	re->gen++;
	while (n && re->skip) {
		int pc = todo[--n].pc;
		const struct inst *in = &re->prog[pc];

		if (seen[pc] == re->gen)
			continue;
		seen[pc] = re->gen;
		switch (in->op) {
		case I_JMP:
			todo[n++].pc = in->x;
			break;
		case I_SPLIT:
			todo[n++].pc = in->x;
			todo[n++].pc = in->y;
			break;
		case I_CHAR:
			first_add(re, in->arg < 0x80 ? in->arg : 0x80);
			break;
		case I_CLASS: {
			const struct cls *k = &re->classes[in->arg];

			for (unsigned b = 0; b < 0x80; b++)
				if (cls_has(k, b))
					first_add(re, b);
			first_add(re, 0x80);	/* whatever is above, the VM decides */
			break;
		}
		case I_ANY:
		case I_MATCH:			/* an empty match: anywhere at all */
			re->skip = false;
			break;
		default:			/* saves and assertions */
			todo[n++].pc = pc + 1;
			break;
		}
	}
}

int re_compile(struct regex **out, const char *pattern, int flags, char *err, size_t errlen)
{
	struct regex *re = pt_calloc(1, sizeof(*re));
	struct parser ps = {
		.re = re, .p = pattern, .end = pattern + strlen(pattern), .flags = flags,
		.err = err, .errlen = errlen,
	};
	int root;

	*out = NULL;
	if (!re)
		return -ENOMEM;
	re->flags = flags;
	if ((re->literal = as_literal(pattern, flags, &re->literal_len))) {
		*out = re;
		return 0;
	}
	root = parse_alt(&ps);
	if (!ps.failed && ps.p < ps.end)
		fail(&ps, "unmatched %s", ere(&ps) ? ")" : "\\)");
	re->nslots = flags & RE_NOSUB ? 2 : 2 * (re->groups + 1);
	if (!ps.failed && (emit(re, I_SAVE, 0) < 0 || gen(re, &re->nodes[root]) ||
			   emit(re, I_SAVE, 1) < 0 || emit(re, I_MATCH, 0) < 0))
		fail(&ps, "pattern too large");
	pt_free(re->nodes);			/* the program is all that is kept */
	re->nodes = NULL;
	if (!ps.failed) {
		for (int i = 0; i < 2; i++) {
			re->pcs[i] = pt_malloc(re->n * sizeof(int));
			re->caps[i] = pt_malloc((size_t)re->n * re->nslots * sizeof(long));
		}
		re->mark = pt_calloc(re->n, sizeof(unsigned));
		re->scratch = pt_malloc(re->nslots * sizeof(long));
		re->stack = pt_malloc((2 * re->n + 2) * sizeof(*re->stack));
		if (!re->pcs[0] || !re->pcs[1] || !re->caps[0] || !re->caps[1] || !re->mark ||
		    !re->scratch || !re->stack)
			fail(&ps, "out of memory");
		else
			first_set(re);
	}
	if (ps.failed) {
		re_free(re);
		return -EINVAL;
	}
	*out = re;
	return 0;
}

int re_groups(const struct regex *re)
{
	return re->groups;
}

void re_free(struct regex *re)
{
	if (!re)
		return;
	for (int i = 0; i < re->nclasses; i++)
		pt_free(re->classes[i].ranges);
	pt_free(re->classes);
	pt_free(re->prog);
	pt_free(re->nodes);
	pt_free(re->literal);
	for (int i = 0; i < 2; i++) {
		pt_free(re->pcs[i]);
		pt_free(re->caps[i]);
	}
	pt_free(re->mark);
	pt_free(re->scratch);
	pt_free(re->stack);
	pt_free(re);
}
