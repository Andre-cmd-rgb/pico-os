/*
 * Regular expressions, POSIX basic and extended, for grep, sed and find.
 *
 * Basic (the default) and extended (RE_EXTENDED) syntax as POSIX gives
 * them, with the GNU additions people type without thinking: \+ \? \| in
 * basic, \< \> \b \B \w \W \s \S in both. Matching is leftmost-longest,
 * as POSIX requires, and takes time in proportion to the text, never
 * exponential: a pattern cannot hang the machine. Text is UTF-8: . and
 * [^x] take a whole character, and -i folds accented Latin letters too.
 *
 * Not taken: back-references inside a pattern (\1 in the pattern itself;
 * in sed's replacement they work). They are the one feature that needs a
 * backtracking matcher, and so the one that can run forever.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define RE_EXTENDED	1	/* ( ) | + ? { } are operators without a backslash */
#define RE_ICASE	2	/* ignore case */
#define RE_NOSUB	4	/* only the whole match is wanted, not the groups */
#define RE_MAXSUB	9	/* groups \1 to \9 */

struct re_match {
	long	so[RE_MAXSUB + 1], eo[RE_MAXSUB + 1];	/* -1 when a group took no part */
};

struct regex;

/* 0, or -EINVAL with what is wrong written to err, or -ENOMEM. */
int	re_compile(struct regex **re, const char *pattern, int flags, char *err, size_t errlen);

/* How many ( ) groups the pattern has. */
int	re_groups(const struct regex *re);

/*
 * The leftmost-longest match in s[0..len) that starts at or after `from`.
 * `notbol` says that s does not start a line, so ^ cannot match at 0.
 * Fills m and returns true, or returns false. With m NULL it only says
 * whether there is a match, which is quicker: the first one found will do.
 */
bool	re_search(struct regex *re, const char *s, size_t len, size_t from, bool notbol,
		  struct re_match *m);

/*
 * The longest match that starts exactly at `at` and ends by `limit`: where
 * it ends, or -1. Text after `limit` still counts for $ and \>. grep -w
 * uses it to try shorter matches when the longest does not end a word.
 */
long	re_longest_at(struct regex *re, const char *s, size_t len, size_t at, size_t limit,
		      bool notbol);

void	re_free(struct regex *re);
