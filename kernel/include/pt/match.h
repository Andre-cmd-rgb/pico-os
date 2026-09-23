/*
 * Shell wildcards: * ? and [...], with the POSIX character classes
 * ([[:digit:]] and the rest). One matcher for the shell's globbing and
 * `case`, and for find -name.
 */
#pragma once

#include <stdbool.h>

#define PT_GLOB_FOLD	1	/* ignore case */
#define PT_GLOB_SLASH	2	/* * ? and [...] do not match '/' */

/*
 * Whether all of `s` matches `pattern`. `escape` quotes the character after
 * it: '\\' for patterns people type, or the shell's own marker for
 * characters that were quoted on its command line; 0 for none.
 */
bool	pt_glob_match(const char *pattern, const char *s, char escape, int flags);

/* Whether `pattern` has a wildcard in it that is not escaped. */
bool	pt_glob_magic(const char *pattern, char escape);

/* Whether byte `c` is in POSIX class `name` ("alpha", "digit", ...): 1, 0, or -1 for no such class. */
int	pt_char_class(const char *name, unsigned char c);
