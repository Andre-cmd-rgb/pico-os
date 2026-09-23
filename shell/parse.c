/*
 * sh: lexer and parser.
 *
 * Text becomes a tree of struct node allocated from an arena. Words keep
 * their quotes and $: expansion happens when a command runs, so a loop sees
 * its variables change. Running out of text inside a construct (an if with
 * no fi, an open quote, a trailing |) is PARSE_INCOMPLETE rather than an
 * error, so callers can read another line and try again.
 */
#include <ctype.h>
#include <string.h>

#include "sh.h"

#define CHUNK_SIZE	2048
#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------ arena */

struct arena_chunk {
	struct arena_chunk	*prev;
	size_t			 used, size;
	alignas(max_align_t) char data[];
};

void *arena_alloc(struct arena *a, size_t n)
{
	const size_t align = alignof(max_align_t);
	struct arena_chunk *c = a->top;

	n = (n + align - 1) & ~(align - 1);
	if (!c || c->size - c->used < n) {
		size_t size = n > CHUNK_SIZE ? n : CHUNK_SIZE;

		c = pt_malloc(sizeof(*c) + size);
		if (!c)
			return NULL;
		c->prev = a->top;
		c->used = 0;
		c->size = size;
		a->top = c;
	}
	void *p = c->data + c->used;
	c->used += n;
	memset(p, 0, n);
	return p;
}

struct arena_mark arena_mark(struct arena *a)
{
	return (struct arena_mark) { a->top, a->top ? a->top->used : 0 };
}

void arena_release(struct arena *a, struct arena_mark mark)
{
	while (a->top && a->top != mark.chunk) {
		struct arena_chunk *c = a->top;
		a->top = c->prev;
		pt_free(c);
	}
	if (a->top)
		a->top->used = mark.used;
}

/* ------------------------------------------------------------ lexer */

enum tok {
	T_EOF,
	T_NEWLINE,
	T_WORD,
	T_SEMI,
	T_DSEMI,
	T_AMP,
	T_AND,
	T_PIPE,
	T_OR,
	T_LPAREN,
	T_RPAREN,
	T_REDIR,
};

struct token {
	enum tok	 type;
	const char	*start, *end;
	bool		 plain;		/* no quotes or $: may be a reserved word */
	int		 fd;
	enum redir_type	 redir;
};

struct parser {
	struct sh		*sh;
	struct arena		*arena;
	const char		*p;		/* next unread byte */
	const char		*last;		/* end of the last token taken */
	struct token		 tok;
	bool			 peeked;
	struct parse_error	 err;
	/* here-documents on the line being read: their bodies come after it */
	const char		*here_op;	/* the first << on the line */
	const char		*here_start;	/* where its body begins */
	const char		*here_end;	/* past the last body read so far */
};

static struct node *parse_list(struct parser *ps);
static struct node *parse_command(struct parser *ps);

static void *fail(struct parser *ps, int status, const char *msg)
{
	if (!ps->err.status) {
		ps->err.status = status;
		ps->err.msg = msg;
	}
	return NULL;
}

static void *incomplete(struct parser *ps)
{
	return fail(ps, PARSE_INCOMPLETE, "unexpected end of file");
}

bool valid_name(const char *s, size_t len)
{
	if (!len || !(isalpha((unsigned char)s[0]) || s[0] == '_'))
		return false;
	for (size_t i = 1; i < len; i++)
		if (!isalnum((unsigned char)s[i]) && s[i] != '_')
			return false;
	return true;
}

static const char *scan_any(struct parser *ps, const char *p);

static const char *scan_squote(struct parser *ps, const char *p)
{
	const char *q = strchr(p + 1, '\'');

	return q ? q + 1 : incomplete(ps);
}

static const char *scan_bquote(struct parser *ps, const char *p)
{
	for (p++; *p != '`'; p++) {
		if (!*p)
			return incomplete(ps);
		if (*p == '\\' && p[1])
			p++;
	}
	return p + 1;
}

static const char *scan_dquote(struct parser *ps, const char *p)
{
	for (p++; p && *p != '"';) {
		if (!*p)
			return incomplete(ps);
		p = *p == '$' || *p == '`' || *p == '\\' ? scan_any(ps, p) : p + 1;
	}
	return p ? p + 1 : NULL;
}

/* ${...}: to the matching brace */
static const char *scan_param(struct parser *ps, const char *p)
{
	while (p && *p != '}')
		p = *p ? scan_any(ps, p) : incomplete(ps);
	return p ? p + 1 : NULL;
}

/* $((...)): to the )) that closes it */
static const char *scan_arith(struct parser *ps, const char *p)
{
	for (int depth = 0; p;) {
		if (!*p)
			return incomplete(ps);
		if (*p == ')' && !depth) {
			if (p[1] == ')')
				return p + 2;
			return p[1] ? fail(ps, PARSE_ERROR, "missing ))") : incomplete(ps);
		}
		depth += (*p == '(') - (*p == ')');
		p = scan_any(ps, p);
	}
	return NULL;
}

/*
 * $(...): parse the commands inside to find the closing parenthesis, so a
 * case pattern's ) or a quoted one does not end it early.
 */
static const char *scan_subst(struct parser *ps, const char *p)
{
	struct parser sub = { .sh = ps->sh, .arena = ps->arena, .p = p, .last = p };
	struct arena_mark mark = arena_mark(ps->arena);

	parse_list(&sub);
	if (!sub.err.status && sub.tok.type != T_RPAREN) {
		sub.err.status = sub.tok.type == T_EOF ? PARSE_INCOMPLETE : PARSE_ERROR;
		sub.err.msg = sub.tok.type == T_EOF ? "unexpected end of file" : "syntax error";
		sub.err.near = sub.tok.start;
		sub.err.near_len = sub.tok.end - sub.tok.start;
	}
	arena_release(ps->arena, mark);
	if (sub.err.status) {
		if (!ps->err.status)
			ps->err = sub.err;
		return NULL;
	}
	return sub.tok.end;
}

static const char *scan_dollar(struct parser *ps, const char *p)
{
	if (p[1] == '(' && p[2] == '(')
		return scan_arith(ps, p + 3);
	if (p[1] == '(')
		return scan_subst(ps, p + 2);
	if (p[1] == '{')
		return scan_param(ps, p + 2);
	return p + 1;
}

/* One quoted string, substitution or escaped byte; any other byte is one. */
static const char *scan_any(struct parser *ps, const char *p)
{
	switch (*p) {
	case '\'':
		return scan_squote(ps, p);
	case '"':
		return scan_dquote(ps, p);
	case '`':
		return scan_bquote(ps, p);
	case '$':
		return scan_dollar(ps, p);
	case '\\':
		return p[1] ? p + 2 : p + 1;
	}
	return p + 1;
}

const char *parse_skip(struct sh *sh, const char *p)
{
	struct parser ps = { .sh = sh, .arena = &sh->ast, .p = p, .last = p };
	const char *end = scan_any(&ps, p);

	return end ? end : p + strlen(p);
}

static bool is_delim(char c)
{
	return c && strchr(" \t\r\n;&|<>()", c);
}

static const char *scan_word(struct parser *ps, const char *p, bool *plain)
{
	*plain = true;
	while (p && *p && !is_delim(*p)) {
		if (!strchr("\\'\"`$", *p)) {
			p++;
			continue;
		}
		*plain = false;
		/* a backslash before the end continues the line */
		if (p[0] == '\\' && (!p[1] || (p[1] == '\n' && !p[2])))
			return incomplete(ps);
		p = scan_any(ps, p);
	}
	return p;
}

static struct token *peek(struct parser *ps)
{
	struct token *t = &ps->tok;
	const char *p = ps->p;

	if (ps->peeked)
		return t;
	ps->peeked = true;
	for (;;) {
		p += strspn(p, " \t\r");
		if (*p == '#')
			p += strcspn(p, "\n");
		if (p[0] != '\\' || p[1] != '\n')
			break;
		p += 2;
		if (!*p)
			incomplete(ps);
	}
	memset(t, 0, sizeof(*t));
	t->start = p;
	t->end = p + 1;
	if (ps->err.status || !*p) {
		t->end = p;
		return t;
	}

	const char *r = isdigit((unsigned char)p[0]) && (p[1] == '<' || p[1] == '>') ? p + 1 : p;
	switch (*r) {
	case '\n':
		t->type = T_NEWLINE;
		if (ps->here_end)
			t->end = ps->here_end;	/* the bodies are not commands */
		break;
	case ';':
		t->type = p[1] == ';' ? T_DSEMI : T_SEMI;
		t->end = p + 1 + (p[1] == ';');
		break;
	case '&':
		t->type = p[1] == '&' ? T_AND : T_AMP;
		t->end = p + 1 + (p[1] == '&');
		break;
	case '|':
		t->type = p[1] == '|' ? T_OR : T_PIPE;
		t->end = p + 1 + (p[1] == '|');
		break;
	case '(':
		t->type = T_LPAREN;
		break;
	case ')':
		t->type = T_RPAREN;
		break;
	case '<':
	case '>':
		t->type = T_REDIR;
		t->fd = r > p ? *p - '0' : *r == '<' ? 0 : 1;
		t->end = r + 2;
		if (r[0] == '<' && r[1] == '<') {
			t->redir = R_HEREDOC;
			t->end = r + 2 + (r[2] == '-');
		} else if (r[0] == '>' && r[1] == '>') {
			t->redir = R_APPEND;
		} else if (r[1] == '&') {
			t->redir = R_DUP;
		} else {
			t->redir = *r == '<' ? R_IN : R_OUT;
			t->end = r + 1;
		}
		break;
	default:
		t->type = T_WORD;
		t->end = scan_word(ps, p, &t->plain);
		if (!t->end) {
			t->type = T_EOF;
			t->end = p;
		}
		break;
	}
	return t;
}

static struct token *next(struct parser *ps)
{
	struct token *t = peek(ps);

	ps->peeked = false;
	ps->p = ps->last = t->end;
	if (t->type == T_NEWLINE)
		ps->here_op = ps->here_start = ps->here_end = NULL;
	return t;
}

static bool keyword(struct parser *ps, const char *kw)
{
	struct token *t = peek(ps);
	size_t n = strlen(kw);

	return t->type == T_WORD && t->plain && (size_t)(t->end - t->start) == n &&
	       !memcmp(t->start, kw, n);
}

static bool accept(struct parser *ps, const char *kw)
{
	if (!keyword(ps, kw))
		return false;
	next(ps);
	return true;
}

static void *unexpected(struct parser *ps)
{
	struct token *t = peek(ps);

	if (ps->err.status)
		return NULL;
	if (t->type == T_EOF)
		return incomplete(ps);
	ps->err.near = t->start;
	ps->err.near_len = t->end - t->start;
	return fail(ps, PARSE_ERROR, "syntax error");
}

static bool expect(struct parser *ps, const char *kw)
{
	if (accept(ps, kw))
		return true;
	unexpected(ps);
	return false;
}

static void linebreak(struct parser *ps)
{
	while (peek(ps)->type == T_NEWLINE)
		next(ps);
}

/* ------------------------------------------------------------ parser */

static struct node *new_node(struct parser *ps, enum node_type type)
{
	struct node *n = arena_alloc(ps->arena, sizeof(*n));

	if (!n)
		return fail(ps, PARSE_ERROR, "out of memory");
	n->type = type;
	n->src = peek(ps)->start;
	return n;
}

static struct node *end_node(struct parser *ps, struct node *n)
{
	n->src_len = ps->last - n->src;
	/* a here-document of this command's has its body after the line */
	if (ps->here_op && ps->here_op >= n->src) {
		n->here = ps->here_start;
		n->here_len = ps->here_end - ps->here_start;
	}
	return n;
}

static struct word *make_word(struct parser *ps, const struct token *t)
{
	size_t len = t->end - t->start;
	struct word *w = arena_alloc(ps->arena, sizeof(*w) + len + 1);

	if (!w)
		return fail(ps, PARSE_ERROR, "out of memory");
	memcpy(w->text, t->start, len);
	return w;
}

static struct node *nonempty(struct parser *ps, struct node *list)
{
	return list ? list : unexpected(ps);
}

static bool at_list_end(struct parser *ps)
{
	static const char *const ends[] = { "then", "else", "elif", "fi", "do", "done", "esac", "}" };
	enum tok type = peek(ps)->type;

	if (type == T_EOF || type == T_RPAREN || type == T_DSEMI)
		return true;
	for (size_t i = 0; i < ARRAY_SIZE(ends); i++)
		if (keyword(ps, ends[i]))
			return true;
	return false;
}

/*
 * The body of a here-document whose << is at `op`, with delimiter `word`:
 * the lines after the one being read -- after the bodies of any earlier
 * here-documents on it -- up to a line that is the delimiter. With <<- the
 * tabs that start each line go. The delimiter is taken with its quotes
 * removed, and any quoting in it means the body is not expanded.
 */
static bool here_body(struct parser *ps, struct redir *r, const struct token *op,
		      const struct word *word)
{
	bool strip = op->end[-1] == '-';
	char delim[128];
	size_t n = 0;
	const char *p, *line;
	struct strbuf body = { 0 };

	for (const char *w = word->text; *w; w++) {
		if (*w == '\\' && w[1])
			w++;
		else if (*w == '\\' || *w == '"' || *w == '\'')
			continue;
		if (n + 1 < sizeof(delim))
			delim[n++] = *w;
	}
	delim[n] = '\0';
	r->quoted = strpbrk(word->text, "\"'\\") != NULL;
	if (!ps->here_end) {
		const char *nl = strchr(ps->p, '\n');

		if (!nl) {
			incomplete(ps);
			return false;
		}
		ps->here_op = op->start;
		ps->here_start = ps->here_end = nl + 1;
	}
	for (p = ps->here_end;; p = line) {
		const char *eol = strchr(p, '\n');

		if (!*p) {
			sb_free(&body);
			incomplete(ps);		/* the delimiter has not come yet */
			return false;
		}
		line = eol ? eol + 1 : p + strlen(p);
		if (strip)
			while (*p == '\t')
				p++;
		size_t len = (eol ? eol : line) - p;

		if (len == n && !memcmp(p, delim, n))
			break;
		sb_add(&body, p, line - p);
	}
	ps->here_end = line;
	r->body = arena_alloc(ps->arena, body.len + 1);
	if (r->body)
		memcpy((char *)r->body, body.s ? body.s : "", body.len);
	sb_free(&body);
	if (!r->body)
		fail(ps, PARSE_ERROR, "out of memory");
	return r->body != NULL;
}

static bool parse_redir(struct parser *ps, struct redir ***tail)
{
	const struct token op = *next(ps);
	struct redir *r = arena_alloc(ps->arena, sizeof(*r));

	if (!r) {
		fail(ps, PARSE_ERROR, "out of memory");
		return false;
	}
	if (peek(ps)->type != T_WORD) {
		unexpected(ps);
		return false;
	}
	r->type = op.redir;
	r->fd = op.fd;
	r->target = make_word(ps, peek(ps));
	next(ps);
	if (r->target && r->type == R_HEREDOC && !here_body(ps, r, &op, r->target))
		return false;
	**tail = r;
	*tail = &r->next;
	return r->target;
}

static struct node *parse_funcdef(struct parser *ps, struct node *n, struct word *name)
{
	next(ps);
	if (!valid_name(name->text, strlen(name->text)))
		return fail(ps, PARSE_ERROR, "bad function name");
	if (peek(ps)->type != T_RPAREN)
		return unexpected(ps);
	next(ps);
	linebreak(ps);
	if (peek(ps)->type != T_LPAREN && !keyword(ps, "{") && !keyword(ps, "if") &&
	    !keyword(ps, "while") && !keyword(ps, "until") && !keyword(ps, "for") &&
	    !keyword(ps, "case"))
		return unexpected(ps);
	n->type = N_FUNCDEF;
	n->name = name->text;
	n->body = parse_command(ps);
	return n->body ? end_node(ps, n) : NULL;
}

static struct node *parse_simple(struct parser *ps)
{
	struct node *n = new_node(ps, N_SIMPLE);
	struct word **tail = &n->words;
	struct redir **rtail = &n->redirs;

	while (n) {
		struct token *t = peek(ps);

		if (t->type == T_REDIR) {
			if (!parse_redir(ps, &rtail))
				return NULL;
		} else if (t->type == T_WORD) {
			struct word *w = make_word(ps, t);
			bool first = !n->words && !n->redirs;

			if (!w)
				return NULL;
			next(ps);
			if (first && peek(ps)->type == T_LPAREN)
				return parse_funcdef(ps, n, w);
			*tail = w;
			tail = &w->next;
		} else {
			break;
		}
	}
	if (!n || (!n->words && !n->redirs))
		return unexpected(ps);
	return end_node(ps, n);
}

static bool do_group(struct parser *ps, struct node *n)
{
	return expect(ps, "do") && (n->body = nonempty(ps, parse_list(ps))) && expect(ps, "done");
}

/* after the if or elif */
static struct node *parse_if(struct parser *ps, struct node *n)
{
	if (!n || !(n->cond = nonempty(ps, parse_list(ps))) || !expect(ps, "then") ||
	    !(n->body = nonempty(ps, parse_list(ps))))
		return NULL;
	if (keyword(ps, "elif")) {
		struct node *elif = new_node(ps, N_IF);
		next(ps);
		n->alt = parse_if(ps, elif);
		return n->alt ? end_node(ps, n) : NULL;
	}
	if (accept(ps, "else") && !(n->alt = nonempty(ps, parse_list(ps))))
		return NULL;
	return expect(ps, "fi") ? end_node(ps, n) : NULL;
}

static struct node *parse_for(struct parser *ps, struct node *n)
{
	struct word **tail = &n->words;
	struct token *t = peek(ps);

	if (t->type != T_WORD || !t->plain || !valid_name(t->start, t->end - t->start))
		return unexpected(ps);
	struct word *name = make_word(ps, t);
	if (!name)
		return NULL;
	n->name = name->text;
	next(ps);
	linebreak(ps);
	if (accept(ps, "in")) {
		n->in_list = true;
		while (peek(ps)->type == T_WORD) {
			if (!(*tail = make_word(ps, peek(ps))))
				return NULL;
			tail = &(*tail)->next;
			next(ps);
		}
		if (peek(ps)->type != T_SEMI && peek(ps)->type != T_NEWLINE)
			return unexpected(ps);
		next(ps);
	} else if (peek(ps)->type == T_SEMI) {
		next(ps);
	}
	linebreak(ps);
	return do_group(ps, n) ? n : NULL;
}

static struct node *parse_case(struct parser *ps, struct node *n)
{
	struct case_item **tail = &n->items;

	if (peek(ps)->type != T_WORD)
		return unexpected(ps);
	if (!(n->words = make_word(ps, peek(ps))))
		return NULL;
	next(ps);
	linebreak(ps);
	if (!expect(ps, "in"))
		return NULL;
	linebreak(ps);
	while (!keyword(ps, "esac")) {
		struct case_item *item = arena_alloc(ps->arena, sizeof(*item));
		struct word **pattern = &item->patterns;

		if (!item)
			return fail(ps, PARSE_ERROR, "out of memory");
		if (peek(ps)->type == T_LPAREN)
			next(ps);
		for (;;) {
			if (peek(ps)->type != T_WORD)
				return unexpected(ps);
			if (!(*pattern = make_word(ps, peek(ps))))
				return NULL;
			pattern = &(*pattern)->next;
			next(ps);
			if (peek(ps)->type != T_PIPE)
				break;
			next(ps);
		}
		if (peek(ps)->type != T_RPAREN)
			return unexpected(ps);
		next(ps);
		item->body = parse_list(ps);
		if (ps->err.status)
			return NULL;
		*tail = item;
		tail = &item->next;
		if (peek(ps)->type != T_DSEMI)
			break;
		next(ps);
		linebreak(ps);
	}
	return expect(ps, "esac") ? n : NULL;
}

static struct node *parse_command(struct parser *ps)
{
	struct node *n;

	if (sh_stack_low(ps->sh))
		return fail(ps, PARSE_ERROR, "too deeply nested");
	if (peek(ps)->type == T_LPAREN || keyword(ps, "{")) {
		bool brace = peek(ps)->type == T_WORD;
		n = new_node(ps, brace ? N_BRACE : N_SUBSHELL);
		next(ps);
		if (!n || !(n->body = nonempty(ps, parse_list(ps))))
			return NULL;
		if (brace ? !expect(ps, "}") : peek(ps)->type != T_RPAREN)
			return unexpected(ps);
		if (!brace)
			next(ps);
	} else if (keyword(ps, "if")) {
		n = new_node(ps, N_IF);
		next(ps);
		n = parse_if(ps, n);
	} else if (keyword(ps, "while") || keyword(ps, "until")) {
		n = new_node(ps, keyword(ps, "while") ? N_WHILE : N_UNTIL);
		next(ps);
		if (n && !((n->cond = nonempty(ps, parse_list(ps))) && do_group(ps, n)))
			n = NULL;
	} else if (keyword(ps, "for")) {
		n = new_node(ps, N_FOR);
		next(ps);
		n = n ? parse_for(ps, n) : NULL;
	} else if (keyword(ps, "case")) {
		n = new_node(ps, N_CASE);
		next(ps);
		n = n ? parse_case(ps, n) : NULL;
	} else {
		return parse_simple(ps);
	}

	struct redir **tail = n ? &n->redirs : NULL;
	while (n && peek(ps)->type == T_REDIR)
		if (!parse_redir(ps, &tail))
			return NULL;
	return n ? end_node(ps, n) : NULL;
}

static struct node *parse_pipeline(struct parser *ps)
{
	const char *start = peek(ps)->start;
	bool negate = accept(ps, "!");
	struct node *first = parse_command(ps);

	if (!first || (!negate && peek(ps)->type != T_PIPE))
		return first;

	struct node *pl = new_node(ps, N_PIPELINE);
	if (!pl)
		return NULL;
	pl->src = start;
	pl->negate = negate;
	pl->body = first;
	for (struct node *stage = first; peek(ps)->type == T_PIPE; stage = stage->next) {
		next(ps);
		linebreak(ps);
		if (!(stage->next = parse_command(ps)))
			return NULL;
	}
	return end_node(ps, pl);
}

static struct node *parse_andor(struct parser *ps)
{
	struct node *left = parse_pipeline(ps);

	while (left && (peek(ps)->type == T_AND || peek(ps)->type == T_OR)) {
		struct node *n = new_node(ps, peek(ps)->type == T_AND ? N_AND : N_OR);

		if (!n)
			return NULL;
		next(ps);
		linebreak(ps);
		n->src = left->src;
		n->cond = left;
		if (!(n->body = parse_pipeline(ps)))
			return NULL;
		left = end_node(ps, n);
	}
	return left;
}

/* Commands up to a closing keyword, ), ;; or the end; NULL when empty. */
static struct node *parse_list(struct parser *ps)
{
	struct node *first = NULL, **tail = &first;
	const char *start;
	bool several = false;

	linebreak(ps);
	start = peek(ps)->start;
	while (!ps->err.status && !at_list_end(ps)) {
		struct node *item = parse_andor(ps);
		enum tok type = peek(ps)->type;

		if (!item)
			return NULL;
		several |= first != NULL;
		*tail = item;
		tail = &item->next;
		if (type == T_AMP)
			item->background = several = true;
		if (type != T_AMP && type != T_SEMI && type != T_NEWLINE)
			break;
		next(ps);
		linebreak(ps);
	}
	if (ps->err.status || !first || !several)
		return ps->err.status ? NULL : first;

	struct node *list = new_node(ps, N_LIST);
	if (!list)
		return NULL;
	list->src = start;
	list->body = first;
	return end_node(ps, list);
}

int parse(struct sh *sh, struct arena *a, const char *text, struct node **out,
	  struct parse_error *err)
{
	struct parser ps = { .sh = sh, .arena = a, .p = text, .last = text };

	*out = parse_list(&ps);
	if (!ps.err.status && peek(&ps)->type != T_EOF)
		unexpected(&ps);
	*err = ps.err;
	return ps.err.status;
}
