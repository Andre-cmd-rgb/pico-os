/*
 * JSON, as much as talking to a web API takes: a value found by its path
 * in a text, a string's escapes undone, and text built up with strings
 * escaped. Nothing is parsed into a tree: a path is walked over the text
 * each time, which for an answer of a few kilobytes is quick and costs no
 * memory. See json.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Text built up in pt_malloc'd memory; `oom` says a piece was dropped. */
struct jbuf {
	char	*p;
	size_t	 len, cap;
	bool	 oom;
};

void	jb_add(struct jbuf *b, const char *s, size_t n);
void	jb_puts(struct jbuf *b, const char *s);
void	jb_printf(struct jbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void	jb_str(struct jbuf *b, const char *s);		/* "s", escaped */
void	jb_strn(struct jbuf *b, const char *s, size_t n);
void	jb_free(struct jbuf *b);

/*
 * The value at `path` in the JSON between p and end: names and array
 * indexes joined by dots, "choices.0.delta.content". Returns where the
 * value starts and sets *vend to where it ends, or NULL if it is not
 * there. An empty path is the whole value.
 */
const char *json_get(const char *p, const char *end, const char *path, const char **vend);

/* A string value's text with its escapes undone, into out; its length.
 * Not a string: the value as it is written (a number, true, null). */
size_t	json_text(const char *v, const char *vend, char *out, size_t size);

/* The same, pt_malloc'd whole; NULL if there is no memory. */
char	*json_dup(const char *v, const char *vend);

/* How many items an array has; -1 if it is not one. */
int	json_count(const char *v, const char *vend);

/* A number value; `dflt` if it is not one. */
double	json_num(const char *v, const char *vend, double dflt);

/* One complete JSON value, with bounded nesting and no trailing data. */
bool	json_valid(const char *p, size_t n);
