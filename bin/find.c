/*
 * Walking the tree: find du xargs
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pt/match.h"
#include "regex.h"
#include "util.h"

/* `s` with every `what` in it made `with`, on the heap. */
static char *replace_all(const char *s, const char *what, const char *with)
{
	size_t wl = strlen(what), rl = strlen(with), n = 0, size;
	char *out, *o;

	for (const char *p = s; (p = strstr(p, what)); p += wl)
		n++;
	size = strlen(s) + n * rl - n * wl + 1;
	if (!(out = o = pt_malloc(size)))
		return NULL;
	for (const char *p = s, *hit; *p; p = hit + wl) {
		if (!(hit = strstr(p, what))) {
			strcpy(o, p);
			return out;
		}
		memcpy(o, p, hit - p);
		o += hit - p;
		memcpy(o, with, rl);
		o += rl;
	}
	*o = '\0';
	return out;
}

/* ------------------------------------------------------------ walking */

/*
 * A walk through a tree, without recursion: a program's stack is 8 KB,
 * and a tree can be deeper than a frame per level allows, so the
 * directories being walked are a stack on the heap, and the path grows and
 * shrinks in one buffer. Each directory is read whole on the way in, so
 * that none is held open while its contents are visited: they may be
 * removed (find -delete) or renamed, which FAT's readdir does not like.
 *
 * A file's size and time cost a lookup on the card, so they are fetched
 * only when asked for, by walk_stat(): find -name never needs them.
 */
enum { WALK_FILE, WALK_DIR_PRE, WALK_DIR_POST };

struct walk {
	char		 path[PT_PATH_MAX];
	int		 depth;		/* 0 for where the walk started */
	bool		 is_dir;
	bool		 have_st;
	struct pt_stat	 st;
	struct pt_dirent ent;
};

/*
 * Called for each file, and for each directory before (WALK_DIR_PRE) and
 * after (WALK_DIR_POST) its contents. Returns 0 to go on, > 0 from
 * WALK_DIR_PRE to leave the directory's contents out, < 0 to stop.
 */
typedef int (*walk_fn)(void *ctx, struct walk *w, int phase);

static const struct pt_stat *walk_stat(struct walk *w)
{
	if (!w->have_st) {
		if (pt_stat(w->path, &w->st)) {
			memset(&w->st, 0, sizeof(w->st));
			w->st.is_dir = w->is_dir;
		}
		w->have_st = true;
	}
	return &w->st;
}

struct walk_level {
	char		*names;		/* each a type byte, the name and a NUL */
	size_t		 used, at;
	size_t		 len;		/* of the directory's path */
	bool		 have_st;
	struct pt_stat	 st;
};

/* Into the directory at w->path: a level pushed, and its names read. */
static int walk_push(struct walk *w, struct walk_level **levels, int *top, int *cap)
{
	struct walk_level *lv;
	size_t size = 0;
	pt_dir_t *d;
	int r;

	if (*top + 1 == *cap) {
		int grown = *cap ? *cap * 2 : 8;

		if (!(lv = pt_realloc(*levels, grown * sizeof(*lv))))
			return -ENOMEM;
		*levels = lv;
		*cap = grown;
	}
	lv = &(*levels)[++*top];
	memset(lv, 0, sizeof(*lv));
	lv->len = strlen(w->path);
	lv->have_st = w->have_st;
	lv->st = w->st;
	if ((r = pt_opendir(w->path, &d)))
		return r;
	while ((r = pt_readdir(d, &w->ent)) == 1) {
		size_t n = strlen(w->ent.name) + 2;

		if (lv->used + n > size) {
			size_t grown = size ? size * 2 : 512;	/* 512 > any one name */
			char *g = pt_realloc(lv->names, grown);

			if (!g) {
				r = -ENOMEM;
				break;
			}
			lv->names = g;
			size = grown;
		}
		lv->names[lv->used] = w->ent.is_dir ? 'd' : 'f';
		memcpy(lv->names + lv->used + 1, w->ent.name, n - 1);
		lv->used += n;
	}
	pt_closedir(d);
	return r;
}

static int walk(const char *root, int maxdepth, walk_fn fn, void *ctx, const char *prog)
{
	struct walk *w = pt_malloc(sizeof(*w));
	struct walk_level *levels = NULL, *lv;
	int top = -1, cap = 0, status = 0, r;

	if (!w)
		return fail(prog, root, -ENOMEM);
	if ((r = pt_stat(root, &w->st)) || strlen(root) >= sizeof(w->path)) {
		status = fail(prog, root, r ? r : -ENAMETOOLONG);
		goto out;
	}
	strcpy(w->path, root);
	w->depth = 0;
	w->is_dir = w->st.is_dir;
	w->have_st = true;
	for (;;) {
		/* w is the next thing to visit */
		if (!w->is_dir) {
			r = fn(ctx, w, WALK_FILE);
		} else if ((r = fn(ctx, w, WALK_DIR_PRE)) == 0 && (maxdepth < 0 || w->depth < maxdepth)) {
			int before = top, err = walk_push(w, &levels, &top, &cap);

			if (err)
				status = fail(prog, w->path, err);
			if (top == before)
				r = fn(ctx, w, WALK_DIR_POST);
		} else if (r > 0) {
			r = fn(ctx, w, WALK_DIR_POST);
		}
		if (r < 0 || pt_interrupted())
			break;
		/* after it, the next name in the innermost directory with any left */
		for (;;) {
			if (top < 0)
				goto out;
			lv = &levels[top];
			w->path[lv->len] = '\0';
			if (lv->at == lv->used) {
				/* a directory is done: itself comes after its contents */
				w->depth = top;
				w->is_dir = true;
				w->have_st = lv->have_st;
				w->st = lv->st;
				pt_free(lv->names);
				top--;
				if (fn(ctx, w, WALK_DIR_POST) < 0)
					goto out;
				continue;
			}
			const char *name = lv->names + lv->at + 1;
			size_t n = strlen(name);
			bool slash = lv->len && w->path[lv->len - 1] != '/';

			lv->at += n + 2;
			if (lv->len + slash + n >= sizeof(w->path)) {
				status = fail(prog, name, -ENAMETOOLONG);
				continue;
			}
			if (slash)
				w->path[lv->len] = '/';
			memcpy(w->path + lv->len + slash, name, n + 1);
			w->depth = top + 1;
			w->is_dir = name[-1] == 'd';
			w->have_st = false;
			break;
		}
	}
out:
	for (; top >= 0; top--)
		pt_free(levels[top].names);
	pt_free(levels);
	pt_free(w);
	return status;
}

/* ------------------------------------------------------------ find */

enum node_type {
	F_TRUE, F_FALSE, F_NOT, F_AND, F_OR, F_COMMA,
	F_NAME, F_PATH, F_REGEX, F_TYPE, F_SIZE, F_EMPTY, F_NEWER, F_MTIME, F_MMIN,
	/* the actions */
	F_PRINT, F_PRINT0, F_DELETE, F_EXEC, F_QUIT,
	F_PRUNE,		/* not an action, as far as a -print is implied */
};

struct fnode {
	enum node_type	 type;
	struct fnode	*a, *b;
	const char	*arg;
	int		 flags;			/* PT_GLOB_FOLD for -iname and -ipath */
	char		 cmp;			/* '+', '-' or '=' for a number */
	long long	 num;
	struct regex	*re;
	char		**argv;			/* -exec's command */
	int		 argc;
	bool		 batch;			/* -exec ... {} + */
	char		**pending;		/* the paths a batch has gathered */
	int		 npending;
};

#define FIND_NODES	128
#define FIND_BATCH	64			/* paths for one -exec ... {} + */

struct find {
	struct fnode	*root;
	char		**args;
	int		 n, at;
	int		 mindepth, maxdepth;
	int		 regex_flags;		/* -regextype */
	bool		 depth_first, has_action, quit, error, prune;
	int		 status;
	time_t		 now;
	const char	*base;			/* the last part of the path, for -name */
	char		 basebuf[PT_NAME_MAX];
	struct fnode	*nodes[FIND_NODES];	/* all of them, to be freed */
	int		 nnodes;
};

static struct fnode *fnew(struct find *f, enum node_type type)
{
	struct fnode *n;

	if (f->nnodes == FIND_NODES) {
		pt_dprintf(PT_STDERR, "find: the expression is too long\n");
		f->error = true;
		return NULL;
	}
	if (!(n = pt_calloc(1, sizeof(*n)))) {
		fail("find", NULL, -ENOMEM);
		f->error = true;
		return NULL;
	}
	n->type = type;
	f->nodes[f->nnodes++] = n;
	return n;
}

static const char *fpeek(struct find *f)
{
	return f->at < f->n ? f->args[f->at] : NULL;
}

static bool fpeek_is(struct find *f, const char *a, const char *b)
{
	const char *s = fpeek(f);

	return s && (!strcmp(s, a) || (b && !strcmp(s, b)));
}

static bool starts_expression(const char *s)
{
	return (s[0] == '-' && s[1]) || !strcmp(s, "(") || !strcmp(s, "!");
}

/* N, +N or -N, with a unit letter where -size allows one. */
static bool parse_num(const char *s, bool units, char *cmp, long long *n)
{
	char *end;

	*cmp = *s == '+' || *s == '-' ? *s++ : '=';
	if (!isdigit((unsigned char)*s))
		return false;
	*n = strtoll(s, &end, 10);
	return !*end || (units && strchr("bckMG", *end) && !end[1]);
}

static struct fnode *parse_or(struct find *f);

static struct fnode *parse_primary(struct find *f)
{
	static const struct {
		const char	*name;
		enum node_type	 type;
		bool		 has_arg;
		int		 flags;
	} table[] = {
		{ "-name", F_NAME, true, 0 }, { "-iname", F_NAME, true, PT_GLOB_FOLD },
		{ "-path", F_PATH, true, 0 }, { "-ipath", F_PATH, true, PT_GLOB_FOLD },
		{ "-wholename", F_PATH, true, 0 },
		{ "-regex", F_REGEX, true, 0 }, { "-iregex", F_REGEX, true, RE_ICASE },
		{ "-type", F_TYPE, true, 0 }, { "-size", F_SIZE, true, 0 },
		{ "-empty", F_EMPTY, false, 0 }, { "-newer", F_NEWER, true, 0 },
		{ "-mtime", F_MTIME, true, 0 }, { "-mmin", F_MMIN, true, 0 },
		{ "-print", F_PRINT, false, 0 }, { "-print0", F_PRINT0, false, 0 },
		{ "-delete", F_DELETE, false, 0 }, { "-exec", F_EXEC, false, 0 },
		{ "-prune", F_PRUNE, false, 0 }, { "-quit", F_QUIT, false, 0 },
		{ "-true", F_TRUE, false, 0 }, { "-false", F_FALSE, false, 0 },
	};
	const char *tok = fpeek(f), *arg = NULL;
	struct fnode *n;

	if (!tok) {
		pt_dprintf(PT_STDERR, "find: the expression ends too soon\n");
		f->error = true;
		return NULL;
	}
	f->at++;
	if (!strcmp(tok, "(")) {
		n = parse_or(f);
		if (!f->error && !fpeek_is(f, ")", NULL)) {
			pt_dprintf(PT_STDERR, "find: a ( with no )\n");
			f->error = true;
		}
		f->at++;
		return n;
	}
	if (!strcmp(tok, "!") || !strcmp(tok, "-not")) {
		if ((n = fnew(f, F_NOT)))
			n->a = parse_primary(f);
		return n;
	}
	/* options: they hold for the whole walk, and are true where they stand */
	if (!strcmp(tok, "-maxdepth") || !strcmp(tok, "-mindepth")) {
		long v;

		if (!(arg = fpeek(f)) || parse_long(arg, &v) || v < 0 || v > 10000) {
			pt_dprintf(PT_STDERR, "find: %s wants a number\n", tok);
			f->error = true;
			return NULL;
		}
		f->at++;
		*(tok[2] == 'a' ? &f->maxdepth : &f->mindepth) = (int)v;
		return fnew(f, F_TRUE);
	}
	if (!strcmp(tok, "-depth")) {
		f->depth_first = true;
		return fnew(f, F_TRUE);
	}
	if (!strcmp(tok, "-regextype")) {
		arg = fpeek(f);
		if (arg && (!strcmp(arg, "posix-extended") || !strcmp(arg, "egrep")))
			f->regex_flags = RE_EXTENDED;
		else if (arg && (!strcmp(arg, "posix-basic") || !strcmp(arg, "grep")))
			f->regex_flags = 0;
		else {
			pt_dprintf(PT_STDERR, "find: -regextype: posix-basic or posix-extended\n");
			f->error = true;
			return NULL;
		}
		f->at++;
		return fnew(f, F_TRUE);
	}

	size_t i = 0;

	while (i < sizeof(table) / sizeof(table[0]) && strcmp(tok, table[i].name))
		i++;
	if (i == sizeof(table) / sizeof(table[0])) {
		pt_dprintf(PT_STDERR, "find: %s: not a test or an action (try 'help find')\n", tok);
		f->error = true;
		return NULL;
	}
	if (table[i].has_arg && !(arg = fpeek(f))) {
		pt_dprintf(PT_STDERR, "find: %s wants an argument\n", tok);
		f->error = true;
		return NULL;
	}
	if (!(n = fnew(f, table[i].type)))
		return NULL;
	if (arg)
		f->at++;
	n->arg = arg;
	n->flags = table[i].flags;
	switch (n->type) {
	case F_TYPE:
		if (strcmp(arg, "f") && strcmp(arg, "d")) {
			pt_dprintf(PT_STDERR, "find: -type %s: there are only f and d\n", arg);
			f->error = true;
		}
		break;
	case F_SIZE: case F_MTIME: case F_MMIN:
		if (!parse_num(arg, n->type == F_SIZE, &n->cmp, &n->num)) {
			pt_dprintf(PT_STDERR, "find: %s %s: not a number\n", tok, arg);
			f->error = true;
		}
		break;
	case F_REGEX: {
		char err[96];

		if (re_compile(&n->re, arg, n->flags | f->regex_flags | RE_NOSUB, err, sizeof(err))) {
			pt_dprintf(PT_STDERR, "find: %s: %s\n", arg, err);
			f->error = true;
		}
		break;
	}
	case F_NEWER: {
		struct pt_stat st;
		int err = pt_stat(arg, &st);

		if (err) {
			fail("find", arg, err);
			f->error = true;
		}
		n->num = err ? 0 : st.mtime;
		break;
	}
	case F_EXEC:
		/* the command, up to a ; or a {} + */
		n->argv = f->args + f->at;
		while (f->at < f->n && strcmp(f->args[f->at], ";") &&
		       !(!strcmp(f->args[f->at], "+") && !strcmp(f->args[f->at - 1], "{}")))
			f->at++;
		n->argc = (int)(f->args + f->at - n->argv);
		if (f->at == f->n || !n->argc) {
			pt_dprintf(PT_STDERR, "find: -exec wants a command, then ; or {} +\n");
			f->error = true;
			break;
		}
		n->batch = f->args[f->at++][0] == '+';
		if (n->batch)
			n->argc--;		/* the {}, which stands for all of them */
		break;
	default:
		break;
	}
	if (n->type >= F_PRINT && n->type < F_PRUNE)
		f->has_action = true;
	if (n->type == F_DELETE)
		f->depth_first = true;		/* a directory's contents go first */
	return n;
}

static struct fnode *parse_and(struct find *f)
{
	struct fnode *left = parse_primary(f), *n;

	while (!f->error && fpeek(f) && !fpeek_is(f, ")", NULL) && !fpeek_is(f, "-o", "-or") &&
	       !fpeek_is(f, ",", NULL)) {
		if (fpeek_is(f, "-a", "-and"))
			f->at++;
		if (!(n = fnew(f, F_AND)))
			return NULL;
		n->a = left;
		n->b = parse_primary(f);
		left = n;
	}
	return left;
}

static struct fnode *parse_or(struct find *f)
{
	struct fnode *left = parse_and(f), *n;

	while (!f->error && (fpeek_is(f, "-o", "-or") || fpeek_is(f, ",", NULL))) {
		bool comma = *f->args[f->at] == ',';

		f->at++;
		if (!(n = fnew(f, comma ? F_COMMA : F_OR)))
			return NULL;
		n->a = left;
		n->b = parse_and(f);
		left = n;
	}
	return left;
}

/* -exec's command, run on one path, or on all of a batch's. */
static int exec_node(struct fnode *n, const char *path)
{
	int argc = 0, extra = n->batch ? n->npending : 0, status;
	char **argv = pt_malloc((n->argc + extra + 1) * sizeof(*argv));
	bool *made = pt_calloc(n->argc + 1, sizeof(*made));

	if (!argv || !made) {
		pt_free(argv);
		pt_free(made);
		return fail("find", NULL, -ENOMEM);
	}
	for (int i = 0; i < n->argc; i++) {
		const char *a = n->argv[i];
		char *s;

		if (!n->batch && strstr(a, "{}") && (s = replace_all(a, "{}", path))) {
			made[argc] = true;
			argv[argc++] = s;
		} else {
			argv[argc++] = (char *)a;
		}
	}
	for (int k = 0; k < extra; k++)
		argv[argc++] = n->pending[k];
	argv[argc] = NULL;
	status = run_command(argc, argv);
	if (status == -ENOENT)
		pt_dprintf(PT_STDERR, "find: %s: no such command\n", argv[0]);
	else if (status < 0)
		fail("find", argv[0], status);
	for (int i = 0; i < n->argc; i++)
		if (made[i])
			pt_free(argv[i]);
	pt_free(made);
	pt_free(argv);
	return status;
}

static void flush_batch(struct find *f, struct fnode *n)
{
	if (!n->npending)
		return;
	if (exec_node(n, NULL))
		f->status = 1;
	for (int k = 0; k < n->npending; k++)
		pt_free(n->pending[k]);
	n->npending = 0;
}

static bool compare(char cmp, long long have, long long want)
{
	return cmp == '+' ? have > want : cmp == '-' ? have < want : have == want;
}

static bool dir_empty(const char *path)
{
	struct pt_dirent ent;
	pt_dir_t *d;
	bool empty;

	if (pt_opendir(path, &d))
		return false;
	empty = pt_readdir(d, &ent) != 1;
	pt_closedir(d);
	return empty;
}

static bool eval(struct find *f, struct fnode *n, struct walk *w)
{
	switch (n->type) {
	case F_TRUE:
		return true;
	case F_FALSE:
		return false;
	case F_NOT:
		return !eval(f, n->a, w);
	case F_AND:
		return eval(f, n->a, w) && eval(f, n->b, w);
	case F_OR:
		return eval(f, n->a, w) || eval(f, n->b, w);
	case F_COMMA:
		eval(f, n->a, w);
		return eval(f, n->b, w);
	case F_NAME:
		return pt_glob_match(n->arg, f->base, '\\', n->flags);
	case F_PATH:
		return pt_glob_match(n->arg, w->path, '\\', n->flags);
	case F_REGEX: {
		struct re_match m;
		size_t len = strlen(w->path);

		/* leftmost-longest: a match of the whole exists if the one at 0 is it */
		return re_search(n->re, w->path, len, 0, false, &m) && m.so[0] == 0 && (size_t)m.eo[0] == len;
	}
	case F_TYPE:
		return w->is_dir == (n->arg[0] == 'd');
	case F_SIZE: {
		/* In units rounded up, as find counts. A folder has no size on
		 * FAT, rather than a block's worth: it never matches. */
		char unit = n->arg[strlen(n->arg) - 1];
		long long per = unit == 'c' ? 1 : unit == 'k' ? 1024 : unit == 'M' ? 1 << 20 :
				unit == 'G' ? 1 << 30 : 512;

		return !w->is_dir && compare(n->cmp, (long long)((walk_stat(w)->size + per - 1) / per), n->num);
	}
	case F_EMPTY:
		return w->is_dir ? dir_empty(w->path) : walk_stat(w)->size == 0;
	case F_NEWER:
		return walk_stat(w)->mtime > (time_t)n->num;
	case F_MTIME:
		return compare(n->cmp, (f->now - walk_stat(w)->mtime) / 86400, n->num);
	case F_MMIN:
		return compare(n->cmp, (f->now - walk_stat(w)->mtime) / 60, n->num);
	case F_PRINT:
		pt_printf("%s\n", w->path);
		return true;
	case F_PRINT0:
		write_all(PT_STDOUT, w->path, strlen(w->path) + 1);
		return true;
	case F_DELETE: {
		int err;

		if (!strcmp(f->base, "."))
			return true;		/* find . -delete: all but . itself */
		err = w->is_dir ? pt_rmdir(w->path) : pt_unlink(w->path);

		if (err) {
			fail("find", w->path, err);
			f->status = 1;
		}
		return !err;
	}
	case F_EXEC:
		if (!n->batch) {
			int st = exec_node(n, w->path);

			if (st < 0)
				f->status = 1;	/* could not run it at all */
			return st == 0;
		}
		if (n->npending == FIND_BATCH)
			flush_batch(f, n);
		if (!(n->pending || (n->pending = pt_malloc(FIND_BATCH * sizeof(*n->pending)))) ||
		    !(n->pending[n->npending] = pt_strdup(w->path))) {
			f->status = fail("find", w->path, -ENOMEM);
			return false;
		}
		n->npending++;
		return true;
	case F_PRUNE:
		f->prune = true;
		return true;
	case F_QUIT:
		f->quit = true;
		return true;
	}
	return false;
}

static int find_visit(void *ctx, struct walk *w, int phase)
{
	struct find *f = ctx;

	/* a directory is looked at before its contents, or after with -depth */
	if (phase == WALK_DIR_PRE ? f->depth_first : phase == WALK_DIR_POST && !f->depth_first)
		return 0;
	if (w->depth >= f->mindepth) {
		/* -name looks at the last part: "dir/" is dir, "/" is itself */
		size_t len = strlen(w->path), end = len, start;

		while (end > 1 && w->path[end - 1] == '/')
			end--;
		for (start = end; start && w->path[start - 1] != '/'; start--)
			;
		if (start == end)
			f->base = w->path;
		else if (end == len)
			f->base = w->path + start;
		else {
			snprintf(f->basebuf, sizeof(f->basebuf), "%.*s", (int)(end - start), w->path + start);
			f->base = f->basebuf;
		}
		f->prune = false;
		if (eval(f, f->root, w) && !f->has_action)
			pt_printf("%s\n", w->path);
	}
	if (f->quit)
		return -1;
	return phase == WALK_DIR_PRE && f->prune;
}

PT_PROGRAM(find, "look for files in a tree\n"
	   "usage: find [path...] [expression]\n"
	   "Tests: -name PAT  -iname PAT  -path PAT  -ipath PAT\n"
	   "  -type f|d  -size [+-]N[ckMG]  -empty  -newer FILE\n"
	   "  -mtime [+-]DAYS  -mmin [+-]MIN  -true  -false\n"
	   "  -regex RE  -iregex RE  the whole path, basic as\n"
	   "  grep's, extended after -regextype posix-extended\n"
	   "Actions: -print  -print0  -delete  -prune  -quit\n"
	   "  -exec CMD {} ;   -exec CMD {} +\n"
	   "Options: -maxdepth N  -mindepth N  -depth\n"
	   "Tests join with ! (not), -a (and, or nothing) and\n"
	   "-o (or), in ( ). No action: what passes is printed.\n"
	   "  find ~ -name '*.txt'\n"
	   "  find . -type d -empty -delete\n"
	   "  find ~/music -size +4M -exec ls -l {} ;")
{
	struct find *f = pt_calloc(1, sizeof(*f));
	int first = 1, status = 0;

	if (!f)
		return fail("find", NULL, -ENOMEM);
	f->maxdepth = -1;
	f->now = time(NULL);
	while (first < argc && !starts_expression(argv[first]))
		first++;
	f->args = argv + first;
	f->n = argc - first;
	f->root = f->n ? parse_or(f) : fnew(f, F_TRUE);
	if (!f->error && f->at < f->n) {
		pt_dprintf(PT_STDERR, "find: %s: not expected here\n", f->args[f->at]);
		f->error = true;
	}
	if (f->error || !f->root) {
		status = 1;
		goto out;
	}
	for (int i = first == 1 ? 0 : 1; i < first && !f->quit && !pt_interrupted(); i++)
		status |= walk(i ? argv[i] : ".", f->maxdepth, find_visit, f, "find");
	for (int i = 0; i < f->nnodes; i++)
		flush_batch(f, f->nodes[i]);
	status |= f->status;
out:
	for (int i = 0; i < f->nnodes; i++) {
		struct fnode *n = f->nodes[i];

		re_free(n->re);
		for (int k = 0; k < n->npending; k++)
			pt_free(n->pending[k]);
		pt_free(n->pending);
		pt_free(n);
	}
	pt_free(f);
	return status;
}

/* ------------------------------------------------------------ du */

struct du {
	bool		 all, human, summary;
	long		 maxdepth;
	uint64_t	*sums;			/* bytes so far in each directory being walked */
	int		 nsums;
	bool		 oom;
};

static void du_print(const struct du *d, uint64_t bytes, const char *path)
{
	char size[24];

	if (d->human)
		human_size(bytes, size, sizeof(size));
	else
		snprintf(size, sizeof(size), "%llu", (unsigned long long)((bytes + 1023) / 1024));
	pt_printf("%s\t%s\n", size, path);
}

static int du_visit(void *ctx, struct walk *w, int phase)
{
	struct du *d = ctx;
	bool shown = w->depth == 0 || (!d->summary && (d->maxdepth < 0 || w->depth <= d->maxdepth));

	if (w->depth >= d->nsums) {
		int grown = w->depth + 16;
		uint64_t *g = pt_realloc(d->sums, grown * sizeof(*g));

		if (!g) {
			d->oom = true;
			return -1;
		}
		d->sums = g;
		d->nsums = grown;
	}
	switch (phase) {
	case WALK_FILE:
		d->sums[w->depth] = walk_stat(w)->size;
		if (w->depth)
			d->sums[w->depth - 1] += d->sums[w->depth];
		if (shown && (d->all || w->depth == 0))
			du_print(d, d->sums[w->depth], w->path);
		break;
	case WALK_DIR_PRE:
		d->sums[w->depth] = 0;
		break;
	case WALK_DIR_POST:
		if (w->depth)
			d->sums[w->depth - 1] += d->sums[w->depth];
		if (shown)
			du_print(d, d->sums[w->depth], w->path);
		break;
	}
	return 0;
}

PT_PROGRAM(du, "how much the files in a tree hold\n"
	   "usage: du [-achs] [-d N] [path...]\n"
	   "  -s  only the total of each path     -a  files too, not only folders\n"
	   "  -h  in K, M and G                   -c  a grand total at the end\n"
	   "  -d N  folders down to N levels only\n"
	   "Sizes are in KB, rounded up, and count what the files hold rather\n"
	   "than the clusters they take up on the card.")
{
	struct du d = { .maxdepth = -1 };
	struct opt o = { .ind = 1 };
	uint64_t total = 0;
	bool grand = false;
	int c, status = 0;

	while ((c = getopt_pt(&o, "du", argc, argv, "achsd:")) != -1) {
		switch (c) {
		case 'a': d.all = true; break;
		case 'c': grand = true; break;
		case 'h': d.human = true; break;
		case 's': d.summary = true; break;
		case 'd':
			if (parse_long(o.arg, &d.maxdepth) || d.maxdepth < 0) {
				pt_dprintf(PT_STDERR, "du: -d %s: not a number\n", o.arg);
				return 1;
			}
			break;
		default:
			return 1;
		}
	}
	for (int i = o.ind; i == o.ind || i < argc; i++) {
		const char *path = i < argc ? argv[i] : ".";

		status |= walk(path, -1, du_visit, &d, "du");
		if (d.oom) {
			status = fail("du", path, -ENOMEM);
			break;
		}
		if (d.sums)
			total += d.sums[0];
		if (pt_interrupted())
			break;
	}
	if (grand)
		du_print(&d, total, "total");
	pt_free(d.sums);
	return status;
}

/* ------------------------------------------------------------ xargs */

/* How much one run may be given: past this, the command runs and
 * the rest go to another run. */
#define XARGS_MAX_ITEMS	1024
#define XARGS_MAX_BYTES	(32 * 1024)

struct xargs {
	char		**cmd;			/* the command and its own arguments */
	int		  ncmd;
	char		**items;		/* for the next run */
	int		  nitems;
	size_t		  bytes;
	long		  per_run;		/* -n */
	const char	 *repl;			/* -I */
	bool		  trace, no_empty, ran, stop;
	int		  status;
	char		 *item;			/* the one being read */
	size_t		  len, size;
};

/* Runs argv, and says what the result means for xargs. */
static void xargs_exec(struct xargs *x, int argc, char **argv)
{
	int st;

	if (x->trace) {
		for (int i = 0; i < argc; i++)
			pt_dprintf(PT_STDERR, "%s%s", i ? " " : "", argv[i]);
		pt_dprintf(PT_STDERR, "\n");
	}
	st = run_command(argc, argv);
	x->ran = true;
	if (st == -ENOENT) {
		pt_dprintf(PT_STDERR, "xargs: %s: no such command\n", argv[0]);
		x->status = 127;
	} else if (st < 0) {
		fail("xargs", argv[0], st);
		x->status = 126;
	} else if (st == 255) {
		pt_dprintf(PT_STDERR, "xargs: %s: exited with status 255; stopping\n", argv[0]);
		x->status = 124;
	} else if (st > 128) {
		pt_dprintf(PT_STDERR, "xargs: %s: stopped by a signal\n", argv[0]);
		x->status = 125;
	} else if (st) {
		x->status = 123;
		return;
	} else {
		return;
	}
	x->stop = true;
}

/* The command with the items gathered so far. */
static void xargs_run(struct xargs *x)
{
	char **argv;

	if (!x->nitems && (x->ran || x->no_empty))
		return;
	if (!(argv = pt_malloc((x->ncmd + x->nitems + 1) * sizeof(*argv)))) {
		x->status = fail("xargs", NULL, -ENOMEM);
		x->stop = true;
		return;
	}
	memcpy(argv, x->cmd, x->ncmd * sizeof(*argv));
	memcpy(argv + x->ncmd, x->items, x->nitems * sizeof(*argv));
	argv[x->ncmd + x->nitems] = NULL;
	xargs_exec(x, x->ncmd + x->nitems, argv);
	pt_free(argv);
	for (int i = 0; i < x->nitems; i++)
		pt_free(x->items[i]);
	x->nitems = 0;
	x->bytes = 0;
}

/* -I: a run for each line, with REPL in the arguments made the line. */
static void xargs_replace(struct xargs *x, const char *line)
{
	char **argv = pt_calloc(x->ncmd + 1, sizeof(*argv));
	int i;

	if (!argv) {
		x->status = fail("xargs", NULL, -ENOMEM);
		x->stop = true;
		return;
	}
	for (i = 0; i < x->ncmd; i++)
		if (!(argv[i] = replace_all(x->cmd[i], x->repl, line)))
			break;
	if (i < x->ncmd) {
		x->status = fail("xargs", NULL, -ENOMEM);
		x->stop = true;
	} else {
		xargs_exec(x, x->ncmd, argv);
	}
	for (i = 0; i < x->ncmd; i++)
		pt_free(argv[i]);
	pt_free(argv);
}

static bool xargs_add(struct xargs *x, char c)
{
	if (x->len == x->size) {
		size_t grown = x->size ? x->size * 2 : 128;
		char *g = pt_realloc(x->item, grown);

		if (!g) {
			x->status = fail("xargs", NULL, -ENOMEM);
			x->stop = true;
			return false;
		}
		x->item = g;
		x->size = grown;
	}
	x->item[x->len++] = c;
	return true;
}

/* The item read so far is complete. */
static void xargs_item(struct xargs *x)
{
	char *copy;

	if (!xargs_add(x, '\0'))
		return;
	x->len--;
	if (x->repl) {
		/* the whole line, less the blanks it starts with */
		char *s = x->item;

		while (*s == ' ' || *s == '\t')
			s++;
		if (*s)
			xargs_replace(x, s);
		x->len = 0;
		return;
	}
	if (!(copy = pt_malloc(x->len + 1)))
		goto oom;
	memcpy(copy, x->item, x->len + 1);
	if (x->nitems && (x->nitems == XARGS_MAX_ITEMS || x->bytes + x->len + 1 > XARGS_MAX_BYTES))
		xargs_run(x);
	x->items[x->nitems++] = copy;
	x->bytes += x->len + 1;
	x->len = 0;
	if (x->per_run && x->nitems >= x->per_run)
		xargs_run(x);
	return;
oom:
	x->status = fail("xargs", NULL, -ENOMEM);
	x->stop = true;
}

PT_PROGRAM(xargs, "run a command with arguments read from standard input\n"
	   "usage: xargs [-0rt] [-n N] [-I REPL] [command [argument...]]\n"
	   "  find . -name '*.bak' | xargs rm       rm them all\n"
	   "  -n N     at most N arguments to a run  -r  no run if there are none\n"
	   "  -I REPL  a run for each line, with REPL in the arguments made it\n"
	   "  -0       items end with a NUL, as find -print0 writes them\n"
	   "  -t       show each command before it runs\n"
	   "Items are split at blanks and newlines, and quotes and \\ work as in\n"
	   "the shell. The command is echo if none is given.")
{
	static char *echo[] = { "echo", NULL };
	struct xargs x = { 0 };
	struct opt o = { .ind = 1 };
	struct lines l;
	char *line, quote = 0;
	size_t len;
	bool zero = false, unended = false;
	bool quoted = false;		/* '' is an item, if an empty one */
	int c;

	while ((c = getopt_pt(&o, "xargs", argc, argv, "0rtn:I:")) != -1) {
		switch (c) {
		case '0': zero = true; break;
		case 'r': x.no_empty = true; break;
		case 't': x.trace = true; break;
		case 'I': x.repl = o.arg; break;
		case 'n':
			if (parse_long(o.arg, &x.per_run) || x.per_run < 1) {
				pt_dprintf(PT_STDERR, "xargs: -n %s: not a number\n", o.arg);
				return 1;
			}
			break;
		default:
			return 1;
		}
	}
	x.cmd = o.ind < argc ? argv + o.ind : echo;
	x.ncmd = o.ind < argc ? argc - o.ind : 1;
	if (!(x.items = pt_malloc(XARGS_MAX_ITEMS * sizeof(*x.items))))
		return fail("xargs", NULL, -ENOMEM);
	lines_init(&l, PT_STDIN);
	while (!x.stop && !unended && !pt_interrupted() && (line = lines_next(&l, &len))) {
		for (size_t i = 0; i < len && !x.stop && !unended; i++) {
			char ch = line[i];

			if (zero) {
				if (ch)
					xargs_add(&x, ch);
				else
					xargs_item(&x);
			} else if (quote) {
				if (ch == '\n')
					unended = true;
				else if (ch == quote)
					quote = 0;
				else
					xargs_add(&x, ch);
			} else if (ch == '\'' || ch == '"') {
				quote = ch;
				quoted = true;
			} else if (ch == '\\' && i + 1 < len) {
				xargs_add(&x, line[++i]);
			} else if (ch == '\n' || (!x.repl && (ch == ' ' || ch == '\t'))) {
				if (x.len || quoted)
					xargs_item(&x);
				quoted = false;
			} else {
				xargs_add(&x, ch);
			}
		}
	}
	if (l.err && !x.stop) {
		x.status = fail("xargs", NULL, l.err);
		x.stop = true;
	}
	if (quote && !x.stop) {
		/* what came before it still runs, as GNU's does */
		pt_dprintf(PT_STDERR, "xargs: a %c with no end (-0 for names with quotes)\n", quote);
		unended = true;
	} else if (!x.stop && (x.len || quoted)) {
		xargs_item(&x);
	}
	if (!x.stop && !x.repl)
		xargs_run(&x);
	if (unended)
		x.status = 1;
	for (int i = 0; i < x.nitems; i++)
		pt_free(x.items[i]);
	pt_free(x.items);
	pt_free(x.item);
	lines_free(&l);
	return x.status;
}
