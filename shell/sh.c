/*
 * sh - the PocketType shell, a practical subset of POSIX sh.
 *
 *   words        cmd arg "double $QUOTED" 'single' \escaped ~/path *.txt [a-z]?
 *   variables    $NAME ${NAME} $? $$ $! $# $0..$9 ${10} $@ "$@" $*   NAME=value
 *   parameters   ${#NAME} ${NAME:-word} ${NAME:=word} ${NAME:+word} ${NAME:?word}
 *                ${NAME%suffix} ${NAME%%suffix} ${NAME#prefix} ${NAME##prefix}
 *   substitution $(command) `command` $((arithmetic))
 *   redirection  < file   > file   >> file   2> file   2>&1   >&2
 *                <<WORD here-documents (<<-WORD strips tabs, <<'WORD' expands nothing)
 *   pipelines    a | b | c    ! a
 *   lists        a ; b    a && b    a || b    a &    a newline is a ;
 *   compound     if a; then b; elif c; then d; else e; fi
 *                while a; do b; done    until a; do b; done
 *                for NAME in words; do a; done    for NAME; do a; done
 *                case word in pat|pat) a;; *) b;; esac
 *                { a; b; }    ( a; b )
 *   functions    name() { a; }   with $1..., $#, $@, return [n], local
 *   builtins     cd exit export unset source . history break continue return
 *                local shift read eval set wait test [ true false : type command
 *                alias unalias trap jobs fg bg
 *   options      set -e (stop on failure) -u (unset is an error) -x (trace)
 *                -o pipefail
 *   traps        trap 'cmd' EXIT INT TERM    trap - INT    trap '' INT
 *   jobs         Ctrl-Z stops the job in front; jobs, fg %n, bg %n; kill %n
 *
 * Every variable is an environment variable, so child processes see them.
 * Commands are parsed into a tree and expanded only when they run, so
 * `cd /mnt/sd; ls $PWD` sees the new directory and loops see variables
 * change. A line that leaves a construct open (if without fi, a trailing |,
 * an open quote) continues at a "> " prompt; Ctrl-C there drops it all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt/keys.h"
#include "pt/program.h"
#include "sh.h"

#define HISTORY_FILE	".sh_history"
#define STACK_RESERVE	3072	/* for the deepest leaf calls, printf among them */

enum {
	CHUNK_RAN,
	CHUNK_MORE,
	CHUNK_BAD,
};

struct history *sh_history(struct sh *sh)
{
	return &sh->history;
}

static void sb_clear(struct strbuf *b)
{
	b->len = 0;
	if (b->s)
		b->s[0] = '\0';
}

/* ------------------------------------------------------------ input */

/*
 * Runs what `text` holds once it forms complete commands, then empties it.
 * *line is the line number `text` starts at, for error messages.
 */
static int run_chunk(struct sh *sh, struct strbuf *text, const char *where, int *line,
		     bool at_end)
{
	struct arena_mark mark = arena_mark(&sh->ast);
	const char *s = text->s ? text->s : "";
	struct parse_error err;
	struct node *root;
	int result = CHUNK_RAN;

	switch (parse(sh, &sh->ast, s, &root, &err)) {
	case PARSE_INCOMPLETE:
		if (!at_end) {
			result = CHUNK_MORE;
			break;
		}
		/* fall through */
	case PARSE_ERROR:
		sh_parse_error(where, *line, s, &err);
		sh->status = 2;
		result = CHUNK_BAD;
		break;
	default:
		exec_node(sh, root);
		break;
	}
	arena_release(&sh->ast, mark);
	if (result == CHUNK_MORE)
		return result;
	for (; *s; s++)
		*line += *s == '\n';
	sb_clear(text);
	return result;
}

static bool stopped(struct sh *sh)
{
	return sh->exit_requested || sh->interrupted || sh->returning;
}

static int run_fd(struct sh *sh, int fd, const char *where)
{
	struct strbuf text = { 0 };
	char *buf = pt_malloc(512);
	int line = 1;
	ssize_t n = 0;

	if (!buf)
		return 1;
	while (!stopped(sh) && (n = pt_read(fd, buf, 512)) > 0) {
		for (ssize_t i = 0; i < n && !stopped(sh); i++) {
			sb_putc(&text, buf[i]);
			if (buf[i] == '\n' && run_chunk(sh, &text, where, &line, false) == CHUNK_BAD)
				goto out;
		}
	}
	if (n == -EINTR) {
		sh_caught(sh);
		if (sh->interrupted)
			sh->status = 130;
	}
	if (text.len && !stopped(sh))
		run_chunk(sh, &text, where, &line, true);
out:
	sb_free(&text);
	pt_free(buf);
	return sh->status;
}

int run_file(struct sh *sh, const char *path)
{
	int fd = pt_open(path, O_RDONLY);

	if (fd < 0) {
		pt_dprintf(PT_STDERR, "sh: %s: %s\n", path, pt_strerror(fd));
		return sh->status = 127;
	}
	int status = run_fd(sh, fd, path);
	pt_close(fd);
	return status;
}

static void cat_file(const char *path)
{
	char buf[256];
	int fd = pt_open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return;
	while ((n = pt_read(fd, buf, sizeof(buf))) > 0)
		pt_write(PT_STDOUT, buf, n);
	pt_close(fd);
}

static void history_path(char *out, size_t size)
{
	const char *home = pt_getenv("HOME");

	snprintf(out, size, "%s/%s", home ? home : "", HISTORY_FILE);
}

static void history_load(struct sh *sh)
{
	char path[PT_PATH_MAX], buf[256], line[SH_LINE_MAX];
	size_t len = 0;
	int lines = 0;
	ssize_t n;

	history_path(path, sizeof(path));
	int fd = pt_open(path, O_RDONLY);
	if (fd < 0)
		return;
	while ((n = pt_read(fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				line[len] = '\0';
				history_add(&sh->history, line);
				len = 0;
				lines++;
			} else if (len < sizeof(line) - 1) {
				line[len++] = buf[i];
			}
		}
	}
	pt_close(fd);

	/* the file is appended to; rewrite it once it doubles the history size */
	if (lines > 2 * SH_HISTORY && (fd = pt_open(path, O_WRONLY | O_CREAT | O_TRUNC)) >= 0) {
		for (int i = 0; i < sh->history.count; i++)
			pt_dprintf(fd, "%s\n", sh->history.entry[i]);
		pt_close(fd);
	}
}

static void history_append(const char *line)
{
	char path[PT_PATH_MAX];

	history_path(path, sizeof(path));
	int fd = pt_open(path, O_WRONLY | O_CREAT | O_APPEND);
	if (fd < 0)
		return;
	pt_dprintf(fd, "%s\n", line);
	pt_close(fd);
}

#define DEFAULT_PS1	"\\e[1m\\u@\\h\\e[0m:\\e[33m\\w\\e[0m\\$ "

/*
 * $PS1 with bash's escapes: \u user, \h host, \w directory (~ for home),
 * \W its last part, \$ a dollar sign, \e escape, \\ a backslash.
 * A directory too long for half the screen is shortened to its last part.
 */
static void build_prompt(char *out, size_t size)
{
	const char *ps1 = pt_getenv("PS1");
	const char *cwd = pt_getcwd();
	const char *home = pt_getenv("HOME");
	const char *user = pt_getenv("USER");
	const char *host = pt_getenv("HOSTNAME");
	char path[PT_PATH_MAX];
	int cols, rows;
	size_t o = 0;

	pt_tty_size(PT_STDIN, &cols, &rows);
	size_t hlen = home ? strlen(home) : 0;
	if (hlen > 1 && !strncmp(cwd, home, hlen) && (cwd[hlen] == '/' || !cwd[hlen]))
		snprintf(path, sizeof(path), "~%s", cwd + hlen);
	else
		strlcpy(path, cwd, sizeof(path));
	const char *base = strrchr(path, '/');
	base = base && base[1] ? base + 1 : path;
	const char *dir = (int)strlen(path) > cols / 2 ? base : path;

	for (const char *p = ps1 ? ps1 : DEFAULT_PS1; *p && o + 1 < size; p++) {
		const char *insert = NULL;
		char literal[3] = { *p };

		if (*p == '\\' && p[1]) {
			switch (*++p) {
			case 'u': insert = user ? user : "user"; break;
			case 'h': insert = host ? host : "pockettype"; break;
			case 'w': insert = dir; break;
			case 'W': insert = base; break;
			case '$': insert = "$"; break;
			case 'e': insert = "\x1b"; break;
			case '\\': insert = "\\"; break;
			default:
				literal[0] = '\\';
				literal[1] = *p;
				break;
			}
		}
		for (const char *c = insert ? insert : literal; *c && o + 1 < size; c++)
			out[o++] = *c;
	}
	out[o] = '\0';
}

static int interactive(struct sh *sh)
{
	char *line = pt_malloc(SH_LINE_MAX);
	char prompt[PT_PATH_MAX + 64];
	struct strbuf text = { 0 };

	if (!line)
		return 1;
	sh->interactive = true;
	pt_ioctl(PT_STDIN, PT_TTY_SETPGRP, &sh->pgid);
	history_load(sh);
	while (!sh->exit_requested) {
		int first = 1;

		pt_sigcatch(true);
		sh->interrupted = false;
		if (!text.len) {
			jobs_reap(sh);		/* what finished or stopped meanwhile */
			build_prompt(prompt, sizeof(prompt));
		} else {
			const char *ps2 = pt_getenv("PS2");
			strlcpy(prompt, ps2 ? ps2 : "> ", sizeof(prompt));
		}

		int len = lineedit(sh, prompt, line, SH_LINE_MAX);
		if (len == LE_EOF && !text.len)
			break;
		if (len == LE_ERROR) {
			pt_sleep_ms(100);
			continue;
		}
		if (len == LE_EOF || len == LE_INTERRUPT) {
			/* Ctrl-C drops a half-typed construct along with the line */
			if (len == LE_EOF)
				pt_dprintf(PT_STDERR, "sh: syntax error: unexpected end of file\n");
			sb_clear(&text);
			continue;
		}
		if (len == 0 && !text.len)
			continue;
		if (len > 0) {
			history_add(&sh->history, line);
			history_append(line);
		}
		sb_add(&text, line, len);
		sb_putc(&text, '\n');
		if (text.oom) {
			pt_dprintf(PT_STDERR, "sh: out of memory\n");
			sb_free(&text);
			continue;
		}
		run_chunk(sh, &text, NULL, &first, false);
	}
	sb_free(&text);
	pt_free(line);
	return sh->status;
}

/* ------------------------------------------------------------ entry points */

/*
 * The shell is ending with `status`: its EXIT trap runs first, whatever
 * brought the end about -- the last line, `exit`, a failure under set -e --
 * and the shell ends with the status it had, unless the trap exits itself.
 */
static int finish(struct sh *sh, int status)
{
	char *trap = sh->traps[TRAP_EXIT];

	if (!trap || !*trap)
		return status;
	sh->traps[TRAP_EXIT] = NULL;		/* once */
	sh->exit_requested = sh->interrupted = sh->returning = false;
	sh->breaking = sh->continuing = 0;
	sh->status = status;
	run_text(sh, trap, "trap");
	pt_free(trap);
	return sh->exit_requested ? sh->status : status;
}

static struct sh *sh_new(int argc, char **argv)
{
	struct sh *sh = pt_calloc(1, sizeof(*sh));
	char **params = pt_malloc((argc + 1) * sizeof(*params));

	if (!sh || !params)
		return NULL;
	memcpy(params, argv, argc * sizeof(*params));
	params[argc] = NULL;
	sh->argc = argc;
	sh->argv = params;
	sh->params = params;
	sh->pgid = process_group();
	sh->stack_limit = (uintptr_t)__builtin_frame_address(0) - (SH_STACK_KB * 1024 - STACK_RESERVE);
	/* Ctrl-C ends what the shell runs, not the shell: see exec_node() */
	pt_sigcatch(true);
	return sh;
}

PT_PROGRAM_STACK(sh, SH_STACK_KB, "command interpreter\n"
		 "usage: sh [-l [-q]] [-c cmd [name args]] [script [args]]\n"
		 "Interactive when input is the terminal. -l prints\n"
		 "/etc/motd, runs /etc/profile and ~/.profile first;\n"
		 "-q leaves the motd out (terminals 2 to 4 do).\n"
		 "Scripts: if, while, until, for, case, functions,\n"
		 "$(cmd), $((1 + 2)), test or [. A line left open\n"
		 "continues at a > prompt.\n"
		 "The prompt is $PS1: \\u user, \\h host, \\w dir, \\W its\n"
		 "last part, \\$ dollar, \\e escape. export PS1='\\W\\$ '")
{
	const char *command = NULL;
	bool login = false, quiet = false;
	int i = 1;

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "-l")) {
			login = true;
		} else if (!strcmp(argv[i], "-q")) {
			quiet = true;
		} else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			command = argv[++i];
		} else {
			pt_dprintf(PT_STDERR, "usage: sh [-l [-q]] [-c cmd [name args]] [script [args]]\n");
			return 2;
		}
	}

	struct sh *sh = sh_new(i < argc ? argc - i : 1, i < argc ? argv + i : argv);
	if (!sh) {
		pt_dprintf(PT_STDERR, "sh: out of memory\n");
		return 1;
	}
	if (command)
		return finish(sh, run_text(sh, command, NULL));
	if (i < argc)
		return finish(sh, run_file(sh, argv[i]));

	if (login) {
		struct pt_stat st;
		char profile[PT_PATH_MAX];
		const char *home = pt_getenv("HOME");

		if (!quiet)
			cat_file("/etc/motd");
		if (!pt_stat("/etc/profile", &st))
			run_file(sh, "/etc/profile");
		snprintf(profile, sizeof(profile), "%s/.profile", home ? home : "");
		if (!pt_stat(profile, &st))
			run_file(sh, profile);
		pt_setenv("PWD", pt_getcwd());
	}
	return finish(sh, pt_isatty(PT_STDIN) ? interactive(sh) : run_fd(sh, PT_STDIN, NULL));
}

/* ------------------------------------------------------------ script loader */

static bool script_probe(const char *path, const uint8_t *head, size_t n)
{
	size_t len = strlen(path);

	return (n >= 2 && head[0] == '#' && head[1] == '!') || (len > 3 && !strcmp(path + len - 3, ".sh"));
}

static int script_exec(const char *path, int argc, char **argv)
{
	struct sh *sh = sh_new(argc, argv);

	if (!sh)
		return 1;
	return finish(sh, run_file(sh, path));
}

static struct pt_loader script_loader = {
	.name = "script",
	.stack_kb = SH_STACK_KB,
	.probe = script_probe,
	.exec = script_exec,
};

__attribute__((constructor)) static void script_loader_register(void)
{
	loader_register(&script_loader);
}
