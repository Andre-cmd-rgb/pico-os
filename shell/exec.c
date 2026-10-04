/*
 * sh: running the syntax tree.
 *
 * Builtins, functions and compound commands run inside the shell; other
 * commands are spawned. There is no fork, so a pipeline stage, background
 * job or $(...) that needs the shell itself runs in a child `sh -c`, which
 * is handed the definitions of this shell's functions before the command.
 * Variables need no copying: they are all environment variables.
 *
 * Only an interactive shell does job control: each foreground job gets its
 * own process group and the terminal. What a script or a child shell starts
 * stays in the shell's group, so Ctrl-C reaches all of it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt/kernel.h"
#include "sdkconfig.h"
#include "sh.h"

#define MAX_OPENED	8

static void errexit(struct sh *sh);
static char *alias_text(struct sh *sh, const struct node *n);

static void sh_error(const char *what, int err)
{
	pt_dprintf(PT_STDERR, "sh: %s: %s\n", what, pt_strerror(err));
}

bool sh_stack_low(struct sh *sh)
{
	return (uintptr_t)__builtin_frame_address(0) < sh->stack_limit;
}

bool sh_unwinding(struct sh *sh)
{
	return sh->breaking || sh->continuing || sh->returning || sh->exit_requested ||
	       sh->interrupted;
}

/* Set when the process was spawned, and never changed. */
int process_group(void)
{
	struct proc *p = proc_current();

	return p ? p->pgid : pt_getpid();
}

void sh_parse_error(const char *where, int line, const char *text, const struct parse_error *err)
{
	const char *end = err->near ? err->near : text + strlen(text);
	char at[96] = "";

	for (const char *p = text; p < end; p++)
		line += *p == '\n';
	if (where)
		snprintf(at, sizeof(at), "%s: line %d: ", where, line);
	if (!err->near)
		pt_dprintf(PT_STDERR, "sh: %s%s\n", at, err->msg);
	else if (*err->near == '\n')
		pt_dprintf(PT_STDERR, "sh: %s%s near newline\n", at, err->msg);
	else
		pt_dprintf(PT_STDERR, "sh: %s%s near '%.*s'\n", at, err->msg, err->near_len, err->near);
}

int run_text(struct sh *sh, const char *text, const char *where)
{
	struct arena_mark mark = arena_mark(&sh->ast);
	struct parse_error err;
	struct node *root;

	if (parse(sh, &sh->ast, text, &root, &err)) {
		sh_parse_error(where, 1, text, &err);
		sh->status = 2;
	} else {
		exec_node(sh, root);
	}
	arena_release(&sh->ast, mark);
	return sh->status;
}

/* ------------------------------------------------------------ functions */

struct function *function_find(struct sh *sh, const char *name)
{
	for (struct function *f = sh->functions; f; f = f->next)
		if (!strcmp(f->name, name))
			return f;
	return NULL;
}

void function_put(struct function *f)
{
	if (--f->refs)
		return;
	arena_release(&f->arena, (struct arena_mark) { 0 });
	pt_free(f);
}

void function_remove(struct sh *sh, const char *name)
{
	for (struct function **pp = &sh->functions; *pp; pp = &(*pp)->next) {
		if (!strcmp((*pp)->name, name)) {
			struct function *f = *pp;
			*pp = f->next;
			function_put(f);
			return;
		}
	}
}

/* The tree being run is freed afterwards: the function keeps its own copy. */
static int define_function(struct sh *sh, struct node *n)
{
	struct function *f = pt_calloc(1, sizeof(*f) + n->src_len + 1 + n->here_len + 1);
	struct parse_error err;
	struct node *root;

	if (!f) {
		sh_error(n->name, -ENOMEM);
		return 1;
	}
	memcpy(f->src, n->src, n->src_len);
	if (n->here_len) {		/* the bodies of its last line's here-documents */
		f->src[n->src_len] = '\n';
		memcpy(f->src + n->src_len + 1, n->here, n->here_len);
	}
	if (parse(sh, &f->arena, f->src, &root, &err) || !root || root->type != N_FUNCDEF) {
		pt_dprintf(PT_STDERR, "sh: %s: cannot define function\n", n->name);
		arena_release(&f->arena, (struct arena_mark) { 0 });
		pt_free(f);
		return 1;
	}
	f->name = root->name;
	f->body = root->body;
	f->refs = 1;
	function_remove(sh, f->name);
	f->next = sh->functions;
	sh->functions = f;
	return 0;
}

static void restore_vars(struct local_var *v)
{
	while (v) {
		struct local_var *next = v->next;

		if (v->old)
			pt_setenv(v->name, v->old);
		else
			pt_unsetenv(v->name);
		pt_free(v->old);
		pt_free(v);
		v = next;
	}
}

static int call_function(struct sh *sh, struct function *f, int argc, char **argv)
{
	int saved_argc = sh->argc, saved_loops = sh->loops;
	char **saved_argv = sh->argv;
	void *saved_params = sh->params;
	struct local_var *saved_locals = sh->locals;
	char **params;

	if (sh_stack_low(sh)) {
		pt_dprintf(PT_STDERR, "sh: %s: functions nested too deeply\n", f->name);
		return 2;
	}
	params = pt_malloc((argc + 1) * sizeof(*params));
	if (!params) {
		sh_error(f->name, -ENOMEM);
		return 1;
	}
	params[0] = sh->argv[0];
	memcpy(params + 1, argv + 1, argc * sizeof(*params));

	f->refs++;
	sh->funcs++;
	sh->argc = argc;
	sh->argv = params;
	sh->params = NULL;
	sh->loops = 0;
	sh->locals = NULL;

	exec_node(sh, f->body);
	sh->returning = false;

	restore_vars(sh->locals);
	pt_free(sh->params);
	pt_free(params);
	sh->locals = saved_locals;
	sh->loops = saved_loops;
	sh->params = saved_params;
	sh->argv = saved_argv;
	sh->argc = saved_argc;
	sh->funcs--;
	function_put(f);
	return sh->status;
}

/* ------------------------------------------------------------ processes */

static int spawn_status(int err)
{
	return err == -ENOENT ? 127 : 126;
}

static int spawn_argv(int argc, char **argv, int fd[3], int pgid)
{
	const struct pt_spawn req = {
		.cmd = argv[0],
		.argc = argc,
		.argv = argv,
		.fd = { fd[0], fd[1], fd[2] },
		.pgid = pgid,
	};
	int pid = pt_spawn(&req);

	if (pid == -ENOENT && !strchr(argv[0], '/'))
		pt_dprintf(PT_STDERR, "sh: %s: command not found\n", argv[0]);
	else if (pid == -ENOEXEC)
		pt_dprintf(PT_STDERR, "sh: %s: not an executable\n", argv[0]);
	else if (pid < 0)
		sh_error(argv[0], pid);
	return pid;
}

/*
 * `text` in a child shell that knows this shell's functions. Its positional
 * parameters are argv (argv[0] is $0) if given, else this shell's.
 */
static int spawn_shell(struct sh *sh, const char *text, size_t len, int argc, char **argv,
		       int fd[3], int pgid)
{
	struct strbuf script = { 0 };
	char **av;
	int pid = -ENOMEM;

	/* the options and the functions come along, as a fork would bring them */
	if (sh->errexit)
		sb_puts(&script, "set -e\n");
	if (sh->nounset)
		sb_puts(&script, "set -u\n");
	if (sh->xtrace)
		sb_puts(&script, "set -x\n");
	if (sh->pipefail)
		sb_puts(&script, "set -o pipefail\n");
	for (struct function *f = sh->functions; f; f = f->next) {
		sb_puts(&script, f->src);
		sb_putc(&script, '\n');
	}
	sb_add(&script, text, len);
	if (!argv) {
		argc = sh->argc;
		argv = sh->argv;
	}
	av = pt_malloc((argc + 4) * sizeof(*av));
	if (av && !script.oom) {
		av[0] = "sh";
		av[1] = "-c";
		av[2] = script.s ? script.s : "";
		memcpy(av + 3, argv, argc * sizeof(*av));
		av[argc + 3] = NULL;
		pid = spawn_argv(argc + 3, av, fd, pgid);
	} else {
		sh_error("sh", pid);
	}
	pt_free(av);
	sb_free(&script);
	return pid;
}

/* A command from the tree in a child shell: its source, and after it the
 * bodies of its here-documents, which come after the line it is on. */
static int spawn_node(struct sh *sh, const struct node *n, int fd[3], int pgid)
{
	struct strbuf text = { 0 };
	int pid;

	if (!n->here_len)
		return spawn_shell(sh, n->src, n->src_len, 0, NULL, fd, pgid);
	sb_add(&text, n->src, n->src_len);
	sb_putc(&text, '\n');
	sb_add(&text, n->here, n->here_len);
	pid = text.oom ? -ENOMEM : spawn_shell(sh, text.s, text.len, 0, NULL, fd, pgid);
	if (text.oom)
		sh_error("sh", pid);
	sb_free(&text);
	return pid;
}

/*
 * Waits for what a command line started: in front of an interactive shell
 * a job, which Ctrl-Z can stop (jobs.c); anywhere else just processes. The
 * status is the last process's, or with -o pipefail the last that failed.
 */
static int wait_job(struct sh *sh, const int *pids, int n, int pgid, bool foreground,
		    const char *cmd, size_t len)
{
	int status = 0, failed = 0, sig = 0;

	if (foreground && sh->interactive) {
		struct job j = { .pgid = pgid, .n = n };
		char *copy = pt_malloc(len + 1);

		memcpy(j.pid, pids, n * sizeof(*pids));
		if (copy) {
			memcpy(copy, cmd, len);
			copy[len] = '\0';
		}
		j.cmd = copy ? copy : "";
		status = job_wait(sh, &j, false);
		pt_free(copy);
		return status;
	}
	for (int i = 0; i < n; i++) {
		int st = 0;

		while (pt_wait(pids[i], &st, false) == -EINTR) {
			/* a signal reached the shell: make sure the job has it too */
			sig = sh_take_signal();
			for (int k = i; k < n; k++)
				pt_kill(pids[k], sig);
		}
		status = st;
		if (st)
			failed = st;
	}
	/* set -o pipefail: the last stage to fail speaks for the pipeline */
	if (sh->pipefail && failed)
		status = failed;
	/* the job died of Ctrl-C: stop whatever loop or script ran it */
	if (status == 128 + PT_SIGINT && !sig)
		sig = PT_SIGINT;
	if (sig)
		sh_signal(sh, sig);	/* the shell's own trap, now the job is over */
	return status;
}

int capture(struct sh *sh, const char *text, struct strbuf *out)
{
	int pipefd[2], fd[3] = { PT_STDIN, -1, PT_STDERR };
	char *buf = pt_malloc(512);
	int err = buf ? pt_pipe(pipefd) : -ENOMEM;
	ssize_t n;

	if (err) {
		sh_error("$(...)", err);
		pt_free(buf);
		return 1;
	}
	fd[1] = pipefd[1];
	int pid = spawn_shell(sh, text, strlen(text), 0, NULL, fd, sh->pgid);
	pt_close(pipefd[1]);
	while (pid > 0 && (n = pt_read(pipefd[0], buf, 512)) != 0) {
		if (n == -EINTR) {
			pt_kill(pid, PT_SIGINT);
			sh_caught(sh);
		} else if (n < 0) {
			break;
		} else {
			sb_add(out, buf, n);
		}
	}
	pt_close(pipefd[0]);
	pt_free(buf);
	return pid < 0 ? spawn_status(pid) : wait_job(sh, &pid, 1, pid, false, NULL, 0);
}

/* ------------------------------------------------------------ redirection */

/*
 * A here-document's text: as it stands if its delimiter was quoted,
 * otherwise with $ and ` expanded as inside double quotes -- which is what
 * it becomes for expand_word, with its own double quotes escaped, and the
 * backslash of a \" kept, as a here-document keeps it.
 */
static char *here_text(struct sh *sh, const struct redir *r)
{
	struct strbuf word = { 0 };
	char *text;

	if (r->quoted)
		return pt_strdup(r->body);
	sb_putc(&word, '"');
	for (const char *p = r->body; *p; p++) {
		if (*p == '"')
			sb_putc(&word, '\\');
		else if (*p == '\\' && p[1] == '"')
			sb_putc(&word, '\\');
		sb_putc(&word, *p);
	}
	sb_putc(&word, '"');
	text = word.oom ? NULL : expand_word(sh, word.s, 0);
	sb_free(&word);
	return text;
}

static int open_target(struct sh *sh, struct redir *r)
{
	char *path;
	int fd;

	if (r->type == R_HEREDOC) {
		char *text = here_text(sh, r);

		fd = text ? pt_memfd(text, strlen(text)) : -ENOMEM;
		pt_free(text);
		if (fd < 0)
			sh_error("here-document", fd);
		return fd < 0 ? -1 : fd;
	}
	if (!(path = expand_word(sh, r->target->text, 0)))
		return -1;
	if (r->type == R_DUP)
		fd = pt_open("/dev/null", r->fd ? O_WRONLY : O_RDONLY);
	else if (r->type == R_IN)
		fd = pt_open(path, O_RDONLY);
	else
		fd = pt_open(path, O_WRONLY | O_CREAT | (r->type == R_APPEND ? O_APPEND : O_TRUNC));
	if (fd < 0)
		sh_error(path, fd);
	pt_free(path);
	return fd < 0 ? -1 : fd;
}

/* For n>&m: m as 0-2, -1 for "-" (closing means /dev/null), -2 on errors. */
static int dup_source(struct sh *sh, struct redir *r)
{
	char *target = expand_word(sh, r->target->text, 0);
	int fd = -2;

	if (!target)
		return -2;
	if (!strcmp(target, "-"))
		fd = -1;
	else if (target[0] >= '0' && target[0] <= '2' && !target[1])
		fd = target[0] - '0';
	else
		pt_dprintf(PT_STDERR, "sh: %s: bad file descriptor\n", target);
	pt_free(target);
	return fd;
}

static bool bad_fd(struct redir *r)
{
	if (r->fd <= 2)
		return false;
	pt_dprintf(PT_STDERR, "sh: %d: only fds 0-2 can be redirected\n", r->fd);
	return true;
}

/* Redirections for a child: which of our fds become its 0, 1 and 2. */
struct fdmap {
	int	fd[3];
	int	opened[MAX_OPENED];
	int	nopened;
};

static int map_redirs(struct sh *sh, struct redir *r, struct fdmap *m)
{
	for (; r; r = r->next) {
		int fd, src = r->type == R_DUP ? dup_source(sh, r) : -1;

		if (bad_fd(r) || src == -2)
			return -1;
		if (src >= 0) {
			m->fd[r->fd] = m->fd[src];
			continue;
		}
		if (m->nopened == MAX_OPENED) {
			pt_dprintf(PT_STDERR, "sh: too many redirections\n");
			return -1;
		}
		if ((fd = open_target(sh, r)) < 0)
			return -1;
		m->opened[m->nopened++] = fd;
		m->fd[r->fd] = fd;
	}
	return 0;
}

static void unmap_redirs(struct fdmap *m)
{
	while (m->nopened)
		pt_close(m->opened[--m->nopened]);
}

/* Redirections inside the shell: move fds 0-2 aside and put them back later. */
struct saved_fds {
	int	fd[3];
};

static void pop_redirs(struct saved_fds *s)
{
	for (int i = 0; i < 3; i++) {
		if (s->fd[i] >= 0) {
			pt_dup2(s->fd[i], i);
			pt_close(s->fd[i]);
		}
	}
}

static int push_redirs(struct sh *sh, struct redir *r, struct saved_fds *s)
{
	s->fd[0] = s->fd[1] = s->fd[2] = -1;
	for (; r; r = r->next) {
		int fd, src = r->type == R_DUP ? dup_source(sh, r) : -1;

		if (bad_fd(r) || src == -2)
			goto fail;
		if (s->fd[r->fd] < 0 && (s->fd[r->fd] = pt_dup(r->fd)) < 0) {
			sh_error("redirection", s->fd[r->fd]);
			goto fail;
		}
		if (src >= 0) {
			pt_dup2(src, r->fd);
			continue;
		}
		if ((fd = open_target(sh, r)) < 0)
			goto fail;
		pt_dup2(fd, r->fd);
		pt_close(fd);
	}
	return 0;
fail:
	pop_redirs(s);
	return -1;
}

/* ------------------------------------------------------------ simple commands */

static bool is_assignment(const char *word)
{
	const char *eq = strchr(word, '=');

	return eq && valid_name(word, eq - word);
}

/* NAME=value words; with `saved`, the old values are kept to restore. */
static int assign(struct sh *sh, struct word *w, struct local_var **saved)
{
	for (; w && is_assignment(w->text); w = w->next) {
		const char *eq = strchr(w->text, '=');
		size_t len = eq - w->text;
		struct local_var *v = pt_malloc(sizeof(*v) + len + 1);
		char *value = expand_word(sh, eq + 1, X_ASSIGN);

		if (!v || !value) {
			if (v && value)
				sh_error(w->text, -ENOMEM);
			pt_free(v);
			pt_free(value);
			return 1;
		}
		memcpy(v->name, w->text, len);
		v->name[len] = '\0';
		if (saved) {
			const char *old = pt_getenv(v->name);
			v->old = old ? pt_strdup(old) : NULL;
			v->next = *saved;
			*saved = v;
		}
		int err = pt_setenv(v->name, value);
		if (err)
			sh_error(v->name, err);
		if (!saved)
			pt_free(v);
		pt_free(value);
		if (err)
			return 1;
	}
	return 0;
}

/*
 * The command's words, past its assignments. `local` and `export` take
 * NAME=value arguments like assignments: without splitting or globbing.
 */
static int expand_command(struct sh *sh, struct word *w, struct fields *args)
{
	while (w && is_assignment(w->text))
		w = w->next;
	for (struct word *first = w; w; w = w->next) {
		bool declare = w != first && args->n &&
			       (!strcmp(args->v[0], "local") || !strcmp(args->v[0], "export"));

		if (declare && is_assignment(w->text)) {
			const char *eq = strchr(w->text, '=');
			char *value = expand_word(sh, eq + 1, X_ASSIGN);
			struct strbuf arg = { 0 };

			if (!value)
				return -1;
			sb_add(&arg, w->text, eq + 1 - w->text);
			sb_puts(&arg, value);
			pt_free(value);
			fields_take(args, arg.oom ? NULL : arg.s);
		} else {
			struct word *next = w->next;

			w->next = NULL;
			int err = expand_words(sh, w, args);
			w->next = next;
			if (err)
				return -1;
		}
	}
	if (!args->oom)
		return 0;
	sh_error("sh", -ENOMEM);
	return -1;
}

static bool needs_shell(struct sh *sh, const char *name)
{
	return builtin_find(name) || function_find(sh, name);
}

/* One pipeline stage started as a process: its pid, or 0 with *status set. */
static int spawn_stage(struct sh *sh, struct node *n, int fd[3], int pgid, int *status)
{
	struct fields args = { 0 };
	struct local_var *saved = NULL;
	struct fdmap m = { .fd = { fd[0], fd[1], fd[2] } };
	int pid = 0;

	*status = 0;
	char *alias = n->type == N_SIMPLE ? alias_text(sh, n) : NULL;

	if (alias) {
		pid = spawn_shell(sh, alias, strlen(alias), 0, NULL, fd, pgid);
		pt_free(alias);
	} else if (n->type != N_SIMPLE) {
		pid = spawn_node(sh, n, fd, pgid);
	} else if (expand_command(sh, n->words, &args) || assign(sh, n->words, &saved) ||
		   map_redirs(sh, n->redirs, &m)) {
		*status = 1;
	} else if (args.n && needs_shell(sh, args.v[0])) {
		char **av = pt_malloc((args.n + 1) * sizeof(*av));

		pid = -ENOMEM;
		if (av) {
			av[0] = sh->argv[0];
			memcpy(av + 1, args.v, args.n * sizeof(*av));
			pid = spawn_shell(sh, "\"$@\"", 4, args.n + 1, av, m.fd, pgid);
		}
		pt_free(av);
	} else if (args.n) {
		pid = spawn_argv(args.n, args.v, m.fd, pgid);
	}
	if (pid < 0)
		*status = spawn_status(pid);
	unmap_redirs(&m);
	restore_vars(saved);
	fields_free(&args);
	return pid;
}

static int run_pipeline(struct sh *sh, struct node *first, bool single, bool background,
			const char *src, size_t len)
{
	int pids[MAX_STAGES], n = 0, prev = -1, status = 0, last = 0;
	int pgid = sh->interactive ? 0 : sh->pgid;

	for (struct node *stage = first; stage; stage = single ? NULL : stage->next) {
		int fd[3] = { prev >= 0 ? prev : PT_STDIN, PT_STDOUT, PT_STDERR };
		int pipefd[2] = { -1, -1 };

		if (n == MAX_STAGES) {
			pt_dprintf(PT_STDERR, "sh: pipeline too long\n");
			status = 2;
			last = 0;
			break;
		}
		if (!single && stage->next) {
			int err = pt_pipe(pipefd);
			if (err) {
				sh_error("pipe", err);
				status = 1;
				last = 0;
				break;
			}
			fd[1] = pipefd[1];
		}
		last = spawn_stage(sh, stage, fd, pgid, &status);
		if (last > 0) {
			if (!pgid)
				pgid = last;
			pids[n++] = last;
		}
		if (pipefd[1] >= 0)
			pt_close(pipefd[1]);
		if (prev >= 0)
			pt_close(prev);
		prev = pipefd[0];
	}
	if (prev >= 0)
		pt_close(prev);

	if (background) {
		if (n) {
			struct job *j = job_add(sh, pgid, pids, n, src, len);

			sh->last_bg = pids[n - 1];
			if (sh->interactive)
				pt_printf("[%d] %d\n", j ? j->id : 0, pids[n - 1]);
		}
		return status;
	}
	if (!n)
		return status;
	int job = wait_job(sh, pids, n, pgid, true, src, len);
	return last > 0 ? job : status;
}

static int run_program(struct sh *sh, struct fields *args, struct redir *redirs,
		       const char *src, size_t len)
{
	struct fdmap m = { .fd = { PT_STDIN, PT_STDOUT, PT_STDERR } };

	if (map_redirs(sh, redirs, &m)) {
		unmap_redirs(&m);
		return 1;
	}
	int pid = spawn_argv(args->n, args->v, m.fd, sh->interactive ? 0 : sh->pgid);
	unmap_redirs(&m);
	return pid < 0 ? spawn_status(pid) : wait_job(sh, &pid, 1, pid, true, src, len);
}

/* A program by name, passing over any function of that name: `command`. */
int run_command_argv(struct sh *sh, int argc, char **argv)
{
	struct fields args = { .v = argv, .n = argc };

	return run_program(sh, &args, NULL, argv[0], strlen(argv[0]));
}

/*
 * An alias: the command's first word, if it names one that is not being
 * expanded already, replaced by its text, and the rest of the command as
 * it was written after it. NULL for none. Only an interactive shell uses
 * them, as bash does unless told otherwise.
 */
static char *alias_text(struct sh *sh, const struct node *n)
{
	const struct word *w = n->words;
	struct strbuf text = { 0 };
	struct alias *a;
	size_t len;

	if (!sh->interactive || !w || strpbrk(w->text, "\"'\\$`=") ||
	    !(a = alias_find(sh, w->text)) || sh->nexpanding == 8)
		return NULL;
	len = strlen(w->text);
	if (n->src_len < len || memcmp(n->src, w->text, len))
		return NULL;			/* an assignment or a redirection comes first */
	for (int i = 0; i < sh->nexpanding; i++)
		if (!strcmp(sh->expanding[i], a->name))
			return NULL;
	sb_puts(&text, a->value);
	sb_add(&text, n->src + len, n->src_len - len);
	if (n->here_len) {
		sb_putc(&text, '\n');
		sb_add(&text, n->here, n->here_len);
	}
	if (text.oom) {
		sb_free(&text);
		return NULL;
	}
	return text.s;
}

static int exec_simple(struct sh *sh, struct node *n)
{
	struct fields args = { 0 };
	struct local_var *saved = NULL;
	struct saved_fds fds;
	char *alias = alias_text(sh, n);
	int status;

	if (alias) {
		sh->expanding[sh->nexpanding++] = n->words->text;
		run_text(sh, alias, NULL);
		sh->nexpanding--;
		pt_free(alias);
		return sh->status;
	}
	sh->subst_status = 0;
	if (expand_command(sh, n->words, &args)) {
		status = 1;
	} else if (args.n && (!strcmp(args.v[0], "kill") || !strcmp(args.v[0], "wait")) &&
		   jobs_expand(sh, &args, args.v[0])) {
		status = 1;			/* %N named no job */
	} else if (!args.n) {
		status = assign(sh, n->words, NULL);
		if (!status && n->redirs) {
			status = push_redirs(sh, n->redirs, &fds) ? 1 : 0;
			if (!status)
				pop_redirs(&fds);
		}
		if (!status)
			status = sh->subst_status;
	} else if (assign(sh, n->words, &saved)) {
		status = 1;
	} else {
		const struct builtin *b = builtin_find(args.v[0]);

		if (sh->xtrace) {		/* set -x: what runs, as it runs */
			const char *ps4 = pt_getenv("PS4");
			struct strbuf line = { 0 };

			sb_puts(&line, ps4 ? ps4 : "+ ");
			for (int i = 0; i < args.n; i++) {
				if (i)
					sb_putc(&line, ' ');
				sb_puts(&line, args.v[i]);
			}
			sb_putc(&line, '\n');
			if (line.s)
				pt_write(PT_STDERR, line.s, line.len);
			sb_free(&line);
		}
		struct function *f = b && b->special ? NULL : function_find(sh, args.v[0]);

		if (strcmp(args.v[0], "exit"))
			sh->warned_stopped = false;	/* exit twice in a row: it means it */
		if (!b && !f)
			status = run_program(sh, &args, n->redirs, n->src, n->src_len);
		else if (push_redirs(sh, n->redirs, &fds))
			status = 1;
		else {
			status = f ? call_function(sh, f, args.n, args.v) : b->fn(sh, args.n, args.v);
			pop_redirs(&fds);
		}
	}
	restore_vars(saved);
	fields_free(&args);
	return status;
}

/* ------------------------------------------------------------ compound commands */

/* After a loop body: take one level of break or continue. True: leave. */
static bool loop_done(struct sh *sh)
{
	if (sh->breaking) {
		sh->breaking--;
		return true;
	}
	if (sh->continuing)
		return --sh->continuing > 0;
	return sh_unwinding(sh);
}

static void exec_loop(struct sh *sh, struct node *n)
{
	int status = 0;

	sh->loops++;
	for (;;) {
		sh->no_errexit++;
		exec_node(sh, n->cond);
		sh->no_errexit--;
		if (sh_unwinding(sh) ? loop_done(sh) : (sh->status == 0) != (n->type == N_WHILE))
			break;
		exec_node(sh, n->body);
		status = sh->status;
		if (loop_done(sh))
			break;
	}
	sh->loops--;
	sh->status = sh->interrupted ? 130 : status;
}

static void exec_for(struct sh *sh, struct node *n)
{
	struct fields words = { 0 };
	int status = 0;

	if (!n->in_list) {
		for (int i = 1; i < sh->argc; i++)
			fields_add(&words, sh->argv[i]);
	} else if (expand_words(sh, n->words, &words)) {
		fields_free(&words);
		sh->status = 1;
		return;
	}
	sh->loops++;
	for (int i = 0; i < words.n; i++) {
		int err = pt_setenv(n->name, words.v[i]);
		if (err) {
			sh_error(n->name, err);
			status = 1;
			break;
		}
		exec_node(sh, n->body);
		status = sh->status;
		if (loop_done(sh))
			break;
	}
	sh->loops--;
	fields_free(&words);
	sh->status = sh->interrupted ? 130 : status;
}

static void exec_case(struct sh *sh, struct node *n)
{
	char *word = expand_word(sh, n->words->text, 0);

	sh->status = word ? 0 : 1;
	for (struct case_item *item = n->items; word && item; item = item->next) {
		for (struct word *p = item->patterns; p; p = p->next) {
			char *pattern = expand_word(sh, p->text, X_PATTERN);
			bool match = pattern && pattern_match(pattern, word);

			pt_free(pattern);
			if (!pattern) {
				sh->status = 1;
				goto out;
			}
			if (match) {
				exec_node(sh, item->body);
				goto out;
			}
		}
	}
out:
	pt_free(word);
}

static void run_subshell(struct sh *sh, struct node *body)
{
	int fd[3] = { PT_STDIN, PT_STDOUT, PT_STDERR };
	int pgid = sh->interactive ? 0 : sh->pgid;
	int pid = spawn_node(sh, body, fd, pgid);

	sh->status = pid < 0 ? spawn_status(pid)
		   : wait_job(sh, &pid, 1, pgid ? pgid : pid, true, body->src, body->src_len);
}

static void exec_compound(struct sh *sh, struct node *n)
{
	struct saved_fds fds;

	if (push_redirs(sh, n->redirs, &fds)) {
		sh->status = 1;
		return;
	}
	switch (n->type) {
	case N_BRACE:
		exec_node(sh, n->body);
		break;
	case N_SUBSHELL:
		run_subshell(sh, n->body);
		errexit(sh);
		break;
	case N_IF:
		sh->no_errexit++;
		exec_node(sh, n->cond);
		sh->no_errexit--;
		if (sh_unwinding(sh))
			break;
		if (sh->status == 0)
			exec_node(sh, n->body);
		else if (n->alt)
			exec_node(sh, n->alt);
		else
			sh->status = 0;
		break;
	case N_WHILE:
	case N_UNTIL:
		exec_loop(sh, n);
		break;
	case N_FOR:
		exec_for(sh, n);
		break;
	case N_CASE:
		exec_case(sh, n);
		break;
	default:
		break;
	}
	pop_redirs(&fds);
}

static void run_background(struct sh *sh, struct node *n)
{
	int fd[3] = { PT_STDIN, PT_STDOUT, PT_STDERR };

	if (n->type == N_PIPELINE || n->type == N_SIMPLE) {
		sh->status = run_pipeline(sh, n->type == N_PIPELINE ? n->body : n,
					  n->type == N_SIMPLE, true, n->src, n->src_len);
		return;
	}
	int pid = spawn_node(sh, n, fd, sh->interactive ? 0 : sh->pgid);
	sh->status = pid < 0 ? spawn_status(pid) : 0;
	if (pid > 0) {
		struct job *j = job_add(sh, pid, &pid, 1, n->src, n->src_len);

		sh->last_bg = pid;
		if (sh->interactive)
			pt_printf("[%d] %d\n", j ? j->id : 0, pid);
	}
}

/*
 * A signal reached the shell, or a job it waited for died of one: its
 * trap runs, or with none the shell stops what it is doing, and a script
 * ends, as usual. An empty trap means the signal is ignored.
 */
void sh_signal(struct sh *sh, int sig)
{
	char *trap = sh->traps[sig == PT_SIGTERM ? TRAP_TERM : TRAP_INT];

	if (!trap) {
		sh->interrupted = true;
		return;
	}
	if (*trap && !sh->in_trap) {
		int status = sh->status;

		sh->in_trap = true;
		run_text(sh, trap, "trap");
		sh->in_trap = false;
		if (!sh->exit_requested)
			sh->status = status;	/* $? is what it was before the trap */
	}
}

/* Which signal is pending for the shell itself, SIGTERM or SIGINT, taken. */
int sh_take_signal(void)
{
	unsigned pending = pt_sigpending();

	pt_sigcatch(true);
	return pending & (1u << PT_SIGTERM) ? PT_SIGTERM : PT_SIGINT;
}

/* A signal is pending for the shell itself: taken, then handled. */
void sh_caught(struct sh *sh)
{
	sh_signal(sh, sh_take_signal());
}

/* set -e: a command that failed, where a failure is not an answer, ends the shell. */
static void errexit(struct sh *sh)
{
	if (sh->errexit && sh->status && !sh->no_errexit && !sh_unwinding(sh))
		sh->exit_requested = true;
}

int exec_node(struct sh *sh, struct node *n)
{
	if (pt_interrupted())
		sh_caught(sh);
	if (sh->interrupted)
		return sh->status = 130;
	if (!n || sh_unwinding(sh))
		return sh->status;
	if (sh_stack_low(sh)) {
		pt_dprintf(PT_STDERR, "sh: commands nested too deeply\n");
		return sh->status = 2;
	}

	switch (n->type) {
	case N_SIMPLE:
		sh->status = exec_simple(sh, n);
		errexit(sh);
		break;
	case N_PIPELINE:
		if (n->negate)
			sh->no_errexit++;	/* ! makes a failure the answer wanted */
		if (n->body->next)
			sh->status = run_pipeline(sh, n->body, false, false, n->src, n->src_len);
		else
			exec_node(sh, n->body);
		if (n->negate) {
			sh->no_errexit--;
			if (!sh->interrupted)
				sh->status = !sh->status;
		} else {
			errexit(sh);
		}
		break;
	case N_AND:
	case N_OR:
		sh->no_errexit++;		/* only the last of an && || list counts */
		exec_node(sh, n->cond);
		sh->no_errexit--;
		if (!sh_unwinding(sh) && (sh->status == 0) == (n->type == N_AND))
			exec_node(sh, n->body);
		break;
	case N_LIST:
		for (struct node *item = n->body; item && !sh_unwinding(sh); item = item->next) {
			if (item->background)
				run_background(sh, item);
			else
				exec_node(sh, item);
		}
		break;
	case N_FUNCDEF:
		sh->status = define_function(sh, n);
		break;
	default:
		exec_compound(sh, n);
		break;
	}
	if (sh->interrupted)
		sh->status = 130;
	return sh->status;
}
