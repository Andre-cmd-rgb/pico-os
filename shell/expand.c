/*
 * sh: word expansion, done when a command runs.
 *
 * A word goes through tilde, parameter, arithmetic and command substitution,
 * then field splitting and pathname expansion, as in POSIX sh. While a word
 * is built, every byte that came from quotes is preceded by CTLESC, so that
 * globbing and case patterns take it literally; the marks are removed once
 * the word is finished.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh.h"

/* ------------------------------------------------------------ strings */

void sb_add(struct strbuf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 64;

		while (cap < b->len + n + 1)
			cap *= 2;
		char *grown = pt_realloc(b->s, cap);
		if (!grown) {
			b->oom = true;
			return;
		}
		b->s = grown;
		b->cap = cap;
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

void sb_putc(struct strbuf *b, char c)
{
	if (b->len + 1 < b->cap) {
		b->s[b->len++] = c;
		b->s[b->len] = '\0';
	} else {
		sb_add(b, &c, 1);
	}
}

void sb_puts(struct strbuf *b, const char *s)
{
	sb_add(b, s, strlen(s));
}

void sb_free(struct strbuf *b)
{
	pt_free(b->s);
	memset(b, 0, sizeof(*b));
}

void fields_take(struct fields *f, char *s)
{
	if (s && f->n + 1 >= f->cap) {
		int cap = f->cap ? f->cap * 2 : 8;
		char **v = pt_realloc(f->v, cap * sizeof(*v));
		if (!v) {
			pt_free(s);
			s = NULL;
		} else {
			f->v = v;
			f->cap = cap;
		}
	}
	if (!s) {
		f->oom = true;
		return;
	}
	f->v[f->n++] = s;
	f->v[f->n] = NULL;
}

void fields_add(struct fields *f, const char *s)
{
	fields_take(f, pt_strdup(s));
}

void fields_free(struct fields *f)
{
	for (int i = 0; i < f->n; i++)
		pt_free(f->v[i]);
	pt_free(f->v);
	memset(f, 0, sizeof(*f));
}

static void unescape(char *s)
{
	char *o = s;

	for (; *s; s++) {
		if (*s == CTLESC && s[1])
			s++;
		*o++ = *s;
	}
	*o = '\0';
}

/* ------------------------------------------------------------ patterns */

/* Quoted characters are marked with CTLESC: to the matcher, an escape. */
static bool has_magic(const char *s)
{
	return pt_glob_magic(s, CTLESC);
}

/* ------------------------------------------------------------ expansion */

struct xstate {
	struct sh	*sh;
	int		 flags;
	const char	*word;		/* start of the word, for ~ */
	struct strbuf	 buf;
	bool		 field;		/* a field is open, maybe still empty */
	bool		 magic;		/* unquoted * ? [ in it */
	bool		 empty_at;	/* "$@" without parameters */
	struct fields	*out;
	bool		 err;
};

static void expand_span(struct xstate *xs, const char *p, const char *end, bool quoted);

static void add_quoted(struct xstate *xs, char c)
{
	if (c == CTLESC || strchr("*?[]!-^\\", c))
		sb_putc(&xs->buf, CTLESC);
	sb_putc(&xs->buf, c);
	xs->field = true;
}

static void add_plain(struct xstate *xs, char c)
{
	if (c == CTLESC) {
		add_quoted(xs, c);
		return;
	}
	xs->magic |= c == '*' || c == '?' || c == '[';
	sb_putc(&xs->buf, c);
	xs->field = true;
}

/*
 * The next component of a pattern, marks and all, into `out`: where the
 * pattern goes on, past the slashes after it. A slash divides components
 * whether it was quoted or not, as POSIX has it.
 */
static const char *component(const char *p, struct strbuf *out, bool *dir_only)
{
	out->len = 0;
	if (out->s)
		out->s[0] = '\0';
	while (*p && *p != '/' && !(*p == CTLESC && p[1] == '/')) {
		if (*p == CTLESC && p[1])
			sb_putc(out, *p++);
		sb_putc(out, *p++);
	}
	*dir_only = false;
	while (*p == '/' || (*p == CTLESC && p[1] == '/')) {
		p += *p == CTLESC ? 2 : 1;
		*dir_only = true;		/* something follows, or a trailing slash */
	}
	return p;
}

/* Every entry of directory `prefix` ("" for here) that matches `pattern`. */
static void glob_dir(const char *prefix, const char *pattern, bool dir_only,
		     struct fields *out, struct pt_dirent *ent)
{
	bool dot = pattern[0] == '.' || (pattern[0] == CTLESC && pattern[1] == '.');
	pt_dir_t *d;

	if (pt_opendir(*prefix ? prefix : ".", &d))
		return;
	while (pt_readdir(d, ent) == 1) {
		const char *name = ent->name;

		if (!strcmp(name, ".") || !strcmp(name, "..") || (name[0] == '.' && !dot))
			continue;
		if ((dir_only && !ent->is_dir) || !pattern_match(pattern, name))
			continue;
		struct strbuf path = { 0 };

		sb_puts(&path, prefix);
		sb_puts(&path, name);
		if (dir_only)
			sb_putc(&path, '/');
		fields_take(out, path.oom ? NULL : path.s);
	}
	pt_closedir(d);
}

static int compare_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/*
 * Pathname expansion, a component at a time: each one with a wildcard is
 * matched in every directory the components before it found, and each one
 * without is taken as it is, if it exists. `*' + '/' + `*.txt' works, as
 * in any other shell, and a trailing slash asks for directories only.
 * Nothing found and the word stays as it was typed.
 */
static bool glob_field(struct xstate *xs, const char *pattern)
{
	struct fields paths = { 0 }, next = { 0 };
	struct strbuf comp = { 0 };
	struct pt_dirent *ent = pt_malloc(sizeof(*ent));
	const char *p = pattern;
	bool found;

	if (!ent)
		return false;
	fields_add(&paths, *p == '/' ? "/" : "");
	while (*p == '/')
		p++;
	while (*p && paths.n && !paths.oom && !next.oom) {
		bool dir_only;

		p = component(p, &comp, &dir_only);
		if (comp.oom)
			break;
		for (int i = 0; i < paths.n; i++) {
			if (has_magic(comp.s)) {
				glob_dir(paths.v[i], comp.s, dir_only, &next, ent);
				continue;
			}
			/* no wildcard: the name itself, if it is there */
			struct strbuf path = { 0 };
			struct pt_stat st;

			sb_puts(&path, paths.v[i]);
			sb_puts(&path, comp.s);
			if (path.s)
				unescape(path.s + strlen(paths.v[i]));
			if (!path.oom && !pt_stat(path.s, &st) && (!dir_only || st.is_dir)) {
				if (dir_only)
					sb_putc(&path, '/');
				fields_take(&next, path.oom ? NULL : path.s);
			} else {
				sb_free(&path);
			}
		}
		fields_free(&paths);
		paths = next;
		memset(&next, 0, sizeof(next));
	}
	xs->err |= paths.oom || next.oom || comp.oom;
	found = !*p && paths.n > 0 && !xs->err;
	if (found) {
		qsort(paths.v, paths.n, sizeof(paths.v[0]), compare_names);
		for (int i = 0; i < paths.n; i++) {
			fields_take(xs->out, paths.v[i]);
			paths.v[i] = NULL;
		}
		paths.n = 0;
	}
	fields_free(&paths);
	fields_free(&next);
	sb_free(&comp);
	pt_free(ent);
	return found;
}

static void end_field(struct xstate *xs)
{
	char empty[1] = "";

	if (!xs->field)
		return;
	char *s = xs->buf.s ? xs->buf.s : empty;

	if (!(xs->flags & X_GLOB) || !xs->magic || !has_magic(s) || !glob_field(xs, s)) {
		if (!(xs->flags & X_PATTERN))
			unescape(s);
		fields_add(xs->out, s);
	}
	xs->buf.len = 0;
	if (xs->buf.s)
		xs->buf.s[0] = '\0';
	xs->field = xs->magic = false;
}

/* The result of an expansion: split and globbed unless quoted. */
static void add_value(struct xstate *xs, const char *v, bool quoted)
{
	const char *ifs = pt_getenv("IFS");
	bool split = !quoted && (xs->flags & X_SPLIT);
	bool active = !quoted && (xs->flags & (X_SPLIT | X_PATTERN));

	if (!ifs)
		ifs = " \t\n";
	for (; *v; v++) {
		if (split && strchr(ifs, *v))
			end_field(xs);
		else if (active)
			add_plain(xs, *v);
		else
			add_quoted(xs, *v);
	}
}

static void add_number(struct xstate *xs, long long n, bool quoted)
{
	char num[24];

	snprintf(num, sizeof(num), "%lld", n);
	add_value(xs, num, quoted);
}

/* Length of the parameter name at s: a special character, one digit or a name. */
static size_t param_name(const char *s, const char *end, bool brace)
{
	size_t n = 0;

	if (s >= end)
		return 0;
	if (strchr("?$#!-@*", *s) && *s)
		return 1;
	if (isdigit((unsigned char)*s)) {
		while (brace && s + n < end && isdigit((unsigned char)s[n]))
			n++;
		return n ? n : 1;
	}
	while (s + n < end && (isalnum((unsigned char)s[n]) || s[n] == '_'))
		n++;
	return isdigit((unsigned char)*s) ? 0 : n;
}

static const char *param_value(struct sh *sh, const char *name, size_t len, char buf[64])
{
	switch (*name) {
	case '?':
		snprintf(buf, 64, "%d", sh->status);
		return buf;
	case '$':
		snprintf(buf, 64, "%d", pt_getpid());
		return buf;
	case '#':
		snprintf(buf, 64, "%d", sh->argc > 0 ? sh->argc - 1 : 0);
		return buf;
	case '!':
		snprintf(buf, 64, "%d", sh->last_bg);
		return sh->last_bg ? buf : NULL;
	case '-':
		return "";
	}
	if (isdigit((unsigned char)*name)) {
		int n = atoi(name);
		return n < sh->argc ? sh->argv[n] : NULL;
	}
	if (len >= 64)
		return NULL;
	memcpy(buf, name, len);
	buf[len] = '\0';
	return pt_getenv(buf);
}

/* set -u: an unset parameter, used bare, is an error -- and ends a script. */
static bool unset_error(struct xstate *xs, const char *name, size_t len)
{
	if (!xs->sh->nounset)
		return false;
	pt_dprintf(PT_STDERR, "sh: %.*s: parameter not set\n", (int)len, name);
	xs->err = true;
	if (!xs->sh->interactive)
		xs->sh->exit_requested = true;
	return true;
}

/* $@ and $*: one field per parameter where splitting applies. */
static void positional(struct xstate *xs, bool at, bool quoted)
{
	struct sh *sh = xs->sh;
	const char *ifs = pt_getenv("IFS");
	char sep[2] = { ifs ? ifs[0] : ' ' };

	for (int i = 1; i < sh->argc; i++) {
		if (i > 1 && (xs->flags & X_SPLIT) && (at || !quoted)) {
			xs->field |= quoted;
			end_field(xs);
		} else if (i > 1) {
			add_value(xs, sep, true);
		}
		add_value(xs, sh->argv[i], quoted);
		xs->field |= quoted;
	}
	if (sh->argc <= 1 && quoted && at)
		xs->empty_at = true;
}

static char *expand_text(struct sh *sh, const char *p, const char *end, int flags)
{
	struct xstate xs = { .sh = sh, .flags = flags & (X_PATTERN | X_ASSIGN), .word = p };

	expand_span(&xs, p, end, false);
	if (xs.buf.oom)
		pt_dprintf(PT_STDERR, "sh: out of memory\n");
	if (xs.err || xs.buf.oom) {
		sb_free(&xs.buf);
		return NULL;
	}
	char *s = xs.buf.s ? xs.buf.s : pt_strdup("");
	if (s && !(flags & X_PATTERN))
		unescape(s);
	return s;
}

static void bad_substitution(struct xstate *xs, const char *p, const char *end)
{
	pt_dprintf(PT_STDERR, "sh: ${%.*s}: bad substitution\n", (int)(end - p), p);
	xs->err = true;
}

/* ${#name} ${name-word} ${name=word} ${name+word} ${name?word} ${name%pattern}... */
static void param_brace(struct xstate *xs, const char *p, const char *end, bool quoted)
{
	struct sh *sh = xs->sh;
	char buf[64];

	if (*p == '#' && p + 1 < end) {
		size_t len = param_name(p + 1, end, true);
		if (!len || p + 1 + len != end)
			return bad_substitution(xs, p, end);
		const char *v = param_value(sh, p + 1, len, buf);
		add_number(xs, v ? (long long)strlen(v) : 0, quoted);
		return;
	}

	size_t len = param_name(p, end, true);
	const char *op = p + len;
	if (!len)
		return bad_substitution(xs, p, end);
	if (*p == '@' || *p == '*') {
		if (op != end)
			return bad_substitution(xs, p, end);
		return positional(xs, *p == '@', quoted);
	}
	const char *v = param_value(sh, p, len, buf);
	if (op == end) {
		if (v)
			add_value(xs, v, quoted);
		else
			unset_error(xs, p, len);
		return;
	}

	bool colon = *op == ':';
	char kind = op[colon];
	const char *word = op + colon + 1;
	bool unset = !v || (colon && !*v);

	switch (kind) {
	case '-':
		if (unset)
			expand_span(xs, word, end, quoted);
		else
			add_value(xs, v, quoted);
		return;
	case '+':
		if (!unset)
			expand_span(xs, word, end, quoted);
		return;
	case '=':
	case '?': {
		if (!unset) {
			add_value(xs, v, quoted);
			return;
		}
		char *text = expand_text(sh, word, end, 0);
		char name[64];
		if (!text) {
			xs->err = true;
			return;
		}
		snprintf(name, sizeof(name), "%.*s", (int)len, p);
		if (kind == '?') {
			pt_dprintf(PT_STDERR, "sh: %s: %s\n", name, *text ? text : "parameter not set");
			xs->err = true;
		} else if (!valid_name(p, len) || pt_setenv(name, text)) {
			pt_dprintf(PT_STDERR, "sh: %s: cannot assign\n", name);
			xs->err = true;
		} else {
			add_value(xs, text, quoted);
		}
		pt_free(text);
		return;
	}
	case '%':
	case '#': {
		if (colon)
			break;
		bool longest = word < end && *word == kind;
		char *pattern = expand_text(sh, word + longest, end, X_PATTERN);
		char *s = pt_strdup(v ? v : "");
		size_t n = s ? strlen(s) : 0;
		if (!pattern || !s) {
			xs->err = true;
		} else if (kind == '#') {
			/* shortest or longest prefix that matches */
			for (size_t k = 0; k <= n; k++) {
				size_t i = longest ? n - k : k;
				char saved = s[i];
				s[i] = '\0';
				bool m = pattern_match(pattern, s);
				s[i] = saved;
				if (m) {
					memmove(s, s + i, n - i + 1);
					break;
				}
			}
		} else {
			for (size_t k = 0; k <= n; k++) {
				size_t i = longest ? k : n - k;
				if (pattern_match(pattern, s + i)) {
					s[i] = '\0';
					break;
				}
			}
		}
		if (!xs->err)
			add_value(xs, s, quoted);
		pt_free(pattern);
		pt_free(s);
		return;
	}
	}
	bad_substitution(xs, p, end);
}

static void arith(struct xstate *xs, const char *p, const char *end, bool quoted)
{
	char *expr = expand_text(xs->sh, p, end, 0);
	long long value;

	if (!expr || arith_eval(xs->sh, expr, &value))
		xs->err = true;
	else
		add_number(xs, value, quoted);
	pt_free(expr);
}

static void subst(struct xstate *xs, const char *p, const char *end, bool quoted, bool backquote)
{
	struct strbuf text = { 0 }, out = { 0 };

	for (; p < end; p++) {
		/* inside `...` a backslash quotes only $ ` and \ */
		if (backquote && *p == '\\' && p + 1 < end && strchr("$`\\", p[1]))
			p++;
		sb_putc(&text, *p);
	}
	if (text.oom) {
		xs->err = true;
		sb_free(&text);
		return;
	}
	xs->sh->subst_status = capture(xs->sh, text.s ? text.s : "", &out);
	while (out.len && out.s[out.len - 1] == '\n')
		out.s[--out.len] = '\0';
	if (xs->sh->interrupted || out.oom)
		xs->err = true;
	else
		add_value(xs, out.s ? out.s : "", quoted);
	sb_free(&text);
	sb_free(&out);
}

static const char *expand_dollar(struct xstate *xs, const char *p, const char *end, bool quoted)
{
	char buf[64];
	const char *q;

	if (p + 1 < end && (p[1] == '(' || p[1] == '{')) {
		q = parse_skip(xs->sh, p);
		if (q > end)
			q = end;
		if (p[1] == '{')
			param_brace(xs, p + 2, q - 1, quoted);
		else if (p[2] == '(')
			arith(xs, p + 3, q - 2, quoted);
		else
			subst(xs, p + 2, q - 1, quoted, false);
		return q;
	}
	size_t len = param_name(p + 1, end, false);
	if (!len) {
		if (quoted)
			add_quoted(xs, '$');
		else
			add_plain(xs, '$');
		return p + 1;
	}
	if (p[1] == '@' || p[1] == '*') {
		positional(xs, p[1] == '@', quoted);
	} else {
		const char *v = param_value(xs->sh, p + 1, len, buf);
		if (v)
			add_value(xs, v, quoted);
		else if (p[1] != '!')
			unset_error(xs, p + 1, len);
	}
	return p + 1 + len;
}

static void expand_span(struct xstate *xs, const char *p, const char *end, bool quoted)
{
	while (p < end && !xs->err) {
		const char *q;

		switch (*p) {
		case '\\':
			if (p + 1 < end && p[1] == '\n') {
				p += 2;
			} else if (p + 1 >= end || (quoted && !strchr("$`\"\\", p[1]))) {
				add_quoted(xs, *p++);
			} else {
				add_quoted(xs, p[1]);
				p += 2;
			}
			continue;
		case '\'':
			if (quoted)
				break;
			q = parse_skip(xs->sh, p);
			for (p++; p < q - 1 && p < end; p++)
				add_quoted(xs, *p);
			xs->field = true;
			p = q;
			continue;
		case '"':
			if (quoted)
				break;
			q = parse_skip(xs->sh, p);
			xs->empty_at = false;
			expand_span(xs, p + 1, q - 1 < end ? q - 1 : end, true);
			xs->field |= !xs->empty_at;
			xs->empty_at = false;
			p = q;
			continue;
		case '`':
			q = parse_skip(xs->sh, p);
			subst(xs, p + 1, q - 1 < end ? q - 1 : end, quoted, true);
			p = q;
			continue;
		case '$':
			p = expand_dollar(xs, p, end, quoted);
			continue;
		case '~':
			if (quoted || !(p == xs->word || ((xs->flags & X_ASSIGN) && p[-1] == ':')))
				break;
			if (p + 1 == end || p[1] == '/' || ((xs->flags & X_ASSIGN) && p[1] == ':')) {
				const char *home = pt_getenv("HOME");
				add_value(xs, home ? home : "~", true);
				p++;
				continue;
			}
			break;
		}
		if (quoted)
			add_quoted(xs, *p);
		else
			add_plain(xs, *p);
		p++;
	}
}

int expand_words(struct sh *sh, struct word *w, struct fields *out)
{
	for (; w; w = w->next) {
		struct xstate xs = { .sh = sh, .flags = X_SPLIT | X_GLOB, .word = w->text, .out = out };

		expand_span(&xs, w->text, w->text + strlen(w->text), false);
		if (!xs.err)
			end_field(&xs);
		if (!xs.err && (xs.buf.oom || out->oom)) {
			pt_dprintf(PT_STDERR, "sh: out of memory\n");
			xs.err = true;
		}
		sb_free(&xs.buf);		/* after the check: it clears oom */
		if (xs.err)
			return -1;
	}
	return 0;
}

char *expand_word(struct sh *sh, const char *text, int flags)
{
	return expand_text(sh, text, text + strlen(text), flags);
}
