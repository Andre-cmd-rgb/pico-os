/*
 * Shell wildcards. See pt/match.h.
 *
 * Plain C with no system calls, so that it also builds on the PC for the
 * tests of the programs that use it.
 */
#include <ctype.h>
#include <string.h>

#include "pt/match.h"

int pt_char_class(const char *name, unsigned char c)
{
	static const struct {
		const char *name;
		int (*is)(int);
	} classes[] = {
		{ "alnum", isalnum }, { "alpha", isalpha }, { "blank", isblank },
		{ "cntrl", iscntrl }, { "digit", isdigit }, { "graph", isgraph },
		{ "lower", islower }, { "print", isprint }, { "punct", ispunct },
		{ "space", isspace }, { "upper", isupper }, { "xdigit", isxdigit },
	};

	for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
		if (!strcmp(classes[i].name, name))
			return classes[i].is(c) != 0;
	return -1;
}

/* The next character of a pattern, and whether it was escaped. */
static const char *next_char(const char *p, char escape, unsigned char *c)
{
	if (escape && *p == escape && p[1])
		p++;
	*c = (unsigned char)*p;
	return p + 1;
}

/*
 * A [...] expression at p, tried against c: returns where the pattern
 * goes on, or NULL if p does not start a well-formed one (then the '['
 * is an ordinary character, as POSIX says).
 */
static const char *bracket(const char *p, unsigned char c, char escape, int flags, bool *hit)
{
	bool negate = false, found = false;
	unsigned char lc = (unsigned char)tolower(c), uc = (unsigned char)toupper(c);
	const char *first;

	p++;
	if (*p == '!' || *p == '^') {
		negate = true;
		p++;
	}
	for (first = p; *p && (*p != ']' || p == first);) {
		unsigned char lo, hi;

		if (p[0] == '[' && p[1] == ':') {
			const char *end = strstr(p + 2, ":]");
			char name[16];
			size_t n = end ? (size_t)(end - p - 2) : 0;

			if (end && n < sizeof(name)) {
				memcpy(name, p + 2, n);
				name[n] = '\0';
				int in = pt_char_class(name, c);

				if (in < 0)
					return NULL;	/* no such class: not a bracket at all */
				if (flags & PT_GLOB_FOLD && (!strcmp(name, "upper") || !strcmp(name, "lower")))
					in = isalpha(c) != 0;
				found |= in;
				p = end + 2;
				continue;
			}
		}
		p = next_char(p, escape, &lo);
		hi = lo;
		if (p[0] == '-' && p[1] && p[1] != ']')
			p = next_char(p + 1, escape, &hi);
		if (flags & PT_GLOB_FOLD)
			found |= (lc >= lo && lc <= hi) || (uc >= lo && uc <= hi);
		else
			found |= c >= lo && c <= hi;
	}
	if (*p != ']')
		return NULL;
	*hit = found != negate;
	return p + 1;
}

static bool same(unsigned char a, unsigned char b, int flags)
{
	return a == b || (flags & PT_GLOB_FOLD && tolower(a) == tolower(b));
}

/*
 * The usual way: on a mismatch, back up to just after the last '*' and let
 * it swallow one more character. Only the last star ever needs to move --
 * whatever an earlier one matched, the later one can take up the slack --
 * so this is linear in practice, never exponential.
 */
bool pt_glob_match(const char *pat, const char *s, char escape, int flags)
{
	const char *star = NULL, *resume = NULL;

	for (;;) {
		if (*pat == '*') {
			while (*pat == '*')
				pat++;
			if (!*pat && !(flags & PT_GLOB_SLASH && strchr(s, '/')))
				return true;
			star = pat;
			resume = s;
			continue;
		}
		if (!*s && !*pat)
			return true;

		const char *next = pat + 1;
		bool ok = false, whole = true;
		unsigned char c = (unsigned char)*s, pc;

		if (!*s || !*pat || (flags & PT_GLOB_SLASH && c == '/' && *pat != '/')) {
			ok = false;
		} else if (*pat == '?') {
			ok = true;
		} else if (*pat == '[' && (next = bracket(pat, c, escape, flags, &ok))) {
			/* matched or not, next is past the brackets */
		} else {
			next = next_char(pat, escape, &pc);
			ok = same(pc, c, flags);
			whole = false;
		}
		if (ok) {
			pat = next;
			s++;
			/* ? and [...] take a whole UTF-8 character: caf? is café */
			while (whole && c >= 0xc0 && (*s & 0xc0) == 0x80)
				s++;
			continue;
		}
		/* a star may not swallow a slash when slashes are special */
		if (!star || !*resume || (flags & PT_GLOB_SLASH && *resume == '/'))
			return false;
		pat = star;
		s = ++resume;
	}
}

bool pt_glob_magic(const char *p, char escape)
{
	bool dummy;

	for (; *p; p++) {
		if (escape && *p == escape && p[1])
			p++;
		else if (*p == '*' || *p == '?' || (*p == '[' && bracket(p, 0, escape, 0, &dummy)))
			return true;
	}
	return false;
}
