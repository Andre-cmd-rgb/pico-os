/*
 * sh: builtin commands. They run inside the shell process because they
 * change it: its variables, directory, parameters or flow of control.
 */
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt/program.h"
#include "sh.h"

static void print_quoted(const char *s);
static void alias_print(const struct alias *a);

static int b_cd(struct sh *sh, int argc, char **argv)
{
	const char *target = argc > 1 ? argv[1] : pt_getenv("HOME");
	char old[PT_PATH_MAX];

	if (argc > 1 && !strcmp(argv[1], "-"))
		target = pt_getenv("OLDPWD");
	if (!target)
		target = "/";
	strlcpy(old, pt_getcwd(), sizeof(old));
	int err = pt_chdir(target);
	if (err) {
		pt_dprintf(PT_STDERR, "cd: %s: %s\n", target, pt_strerror(err));
		return 1;
	}
	pt_setenv("OLDPWD", old);
	pt_setenv("PWD", pt_getcwd());
	if (argc > 1 && !strcmp(argv[1], "-"))
		pt_printf("%s\n", pt_getcwd());	/* as every shell does for cd - */
	return 0;
}

static int b_exit(struct sh *sh, int argc, char **argv)
{
	/* stopped jobs end with the shell: say so once, as other shells do */
	if (sh->interactive && jobs_stopped(sh) && !sh->warned_stopped) {
		pt_dprintf(PT_STDERR, "There are stopped jobs: `exit` again ends them.\n");
		sh->warned_stopped = true;
		return 1;
	}
	sh->exit_requested = true;
	return argc > 1 ? atoi(argv[1]) & 255 : sh->status;
}

static int b_export(struct sh *sh, int argc, char **argv)
{
	const char *entry;

	if (argc == 1) {
		for (int i = 0; pt_environ(i, &entry); i++)
			pt_printf("%s\n", entry);
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		if (!eq)
			continue;	/* every variable is exported already */
		*eq = '\0';
		int err = pt_setenv(argv[i], eq + 1);
		*eq = '=';
		if (err) {
			pt_dprintf(PT_STDERR, "export: %s: %s\n", argv[i], pt_strerror(err));
			return 1;
		}
	}
	return 0;
}

static int b_unset(struct sh *sh, int argc, char **argv)
{
	bool functions = false;
	int i = 1;

	for (; i < argc && argv[i][0] == '-'; i++)
		functions = !strcmp(argv[i], "-f");
	for (; i < argc; i++) {
		if (functions)
			function_remove(sh, argv[i]);
		else
			pt_unsetenv(argv[i]);
	}
	return 0;
}

static int b_source(struct sh *sh, int argc, char **argv)
{
	if (argc < 2) {
		pt_dprintf(PT_STDERR, "source: file name required\n");
		return 2;
	}
	sh->sourcing++;
	int status = run_file(sh, argv[1]);
	sh->sourcing--;
	sh->returning = false;
	return status;
}

static int b_history(struct sh *sh, int argc, char **argv)
{
	struct history *h = sh_history(sh);

	for (int i = 0; i < h->count; i++)
		pt_printf("%4d  %s\n", i + 1, h->entry[i]);
	return 0;
}

static int b_true(struct sh *sh, int argc, char **argv)
{
	return 0;
}

static int b_false(struct sh *sh, int argc, char **argv)
{
	return 1;
}

static int b_test(struct sh *sh, int argc, char **argv)
{
	return sh_test(argc, argv);
}

/* break [n], continue [n] */
static int b_loop(struct sh *sh, int argc, char **argv)
{
	char *end = "";
	long n = argc > 1 ? strtol(argv[1], &end, 10) : 1;

	if (*end || n < 1) {
		pt_dprintf(PT_STDERR, "%s: %s: loop count must be positive\n", argv[0], argv[1]);
		return 1;
	}
	if (!sh->loops) {
		pt_dprintf(PT_STDERR, "%s: only meaningful in a loop\n", argv[0]);
		return 1;
	}
	if (n > sh->loops)
		n = sh->loops;
	if (argv[0][0] == 'b')
		sh->breaking = n;
	else
		sh->continuing = n;
	return 0;
}

static int b_return(struct sh *sh, int argc, char **argv)
{
	if (!sh->funcs && !sh->sourcing) {
		pt_dprintf(PT_STDERR, "return: not in a function or sourced script\n");
		return 1;
	}
	sh->returning = true;
	return argc > 1 ? atoi(argv[1]) & 255 : sh->status;
}

static int b_local(struct sh *sh, int argc, char **argv)
{
	if (!sh->funcs) {
		pt_dprintf(PT_STDERR, "local: only works in a function\n");
		return 1;
	}
	for (int i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		size_t len = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
		struct local_var *v;

		if (!valid_name(argv[i], len)) {
			pt_dprintf(PT_STDERR, "local: %s: not a valid name\n", argv[i]);
			return 1;
		}
		v = pt_malloc(sizeof(*v) + len + 1);
		if (!v)
			return 1;
		memcpy(v->name, argv[i], len);
		v->name[len] = '\0';
		const char *old = pt_getenv(v->name);
		v->old = old ? pt_strdup(old) : NULL;
		v->next = sh->locals;
		sh->locals = v;
		if (eq)
			pt_setenv(v->name, eq + 1);
	}
	return 0;
}

static int b_shift(struct sh *sh, int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 1;

	if (n < 0 || n > sh->argc - 1) {
		pt_dprintf(PT_STDERR, "shift: %d: count out of range\n", n);
		return 1;
	}
	memmove(sh->argv + 1, sh->argv + 1 + n, (sh->argc - n) * sizeof(*sh->argv));
	sh->argc -= n;
	return 0;
}

/*
 * set [-eux] [+eux] [-o NAME] [+o NAME] [--] [args]: the options, then
 * the positional parameters. With nothing, the variables; with -o alone,
 * the options.
 */
static int b_set(struct sh *sh, int argc, char **argv)
{
	static const struct {
		char		 letter;
		const char	*name;
		size_t		 offset;
	} options[] = {
		{ 'e', "errexit", offsetof(struct sh, errexit) },
		{ 'u', "nounset", offsetof(struct sh, nounset) },
		{ 'x', "xtrace", offsetof(struct sh, xtrace) },
		{ 0, "pipefail", offsetof(struct sh, pipefail) },
	};
	const char *entry;
	int i = 1;

	if (argc == 1) {
		for (int k = 0; pt_environ(k, &entry); k++)
			pt_printf("%s\n", entry);
		return 0;
	}
	for (; i < argc && (argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1]; i++) {
		bool on = argv[i][0] == '-';

		if (!strcmp(argv[i], "--")) {
			i++;
			goto params;
		}
		if (!strcmp(argv[i] + 1, "o")) {
			if (i + 1 == argc) {
				for (size_t k = 0; k < sizeof(options) / sizeof(options[0]); k++)
					pt_printf("%-10s %s\n", options[k].name,
						  *(bool *)((char *)sh + options[k].offset) ? "on" : "off");
				return 0;
			}
			const char *name = argv[++i];
			size_t k = 0;

			while (k < sizeof(options) / sizeof(options[0]) && strcmp(options[k].name, name))
				k++;
			if (k == sizeof(options) / sizeof(options[0])) {
				pt_dprintf(PT_STDERR, "set: %s: no such option\n", name);
				return 2;
			}
			*(bool *)((char *)sh + options[k].offset) = on;
			continue;
		}
		for (const char *c = argv[i] + 1; *c; c++) {
			size_t k = 0;

			while (k < sizeof(options) / sizeof(options[0]) && options[k].letter != *c)
				k++;
			if (!*c || k == sizeof(options) / sizeof(options[0])) {
				pt_dprintf(PT_STDERR, "set: -%c: no such option (e u x, -o pipefail)\n", *c);
				return 2;
			}
			*(bool *)((char *)sh + options[k].offset) = on;
		}
	}
	if (i == argc)
		return 0;
params:;
	/* what is left replaces $1 $2 ... */
	int count = argc - i + 1;
	size_t size = (count + 1) * sizeof(char *) + strlen(sh->argv[0]) + 1;

	for (int k = i; k < argc; k++)
		size += strlen(argv[k]) + 1;
	char **block = pt_malloc(size);

	if (!block)
		return 1;
	char *s = (char *)(block + count + 1);

	for (int k = 0; k < count; k++) {
		block[k] = s;
		s = stpcpy(s, k ? argv[i + k - 1] : sh->argv[0]) + 1;
	}
	block[count] = NULL;
	pt_free(sh->params);
	sh->params = block;
	sh->argv = block;
	sh->argc = count;
	return 0;
}

static int b_eval(struct sh *sh, int argc, char **argv)
{
	struct strbuf text = { 0 };

	for (int i = 1; i < argc; i++) {
		if (i > 1)
			sb_putc(&text, ' ');
		sb_puts(&text, argv[i]);
	}
	int status = text.oom ? 1 : text.s ? run_text(sh, text.s, NULL) : 0;
	sb_free(&text);
	return status;
}

static bool is_ifs(const char *ifs, char c)
{
	return c && strchr(ifs, c);
}

/*
 * Where `read` takes its line from: a file gives a line in one call
 * (PT_FILE_READLINE) rather than a call a byte; a pipe or a terminal is
 * read a byte at a time, since what follows the line is the next reader's.
 */
struct line_in {
	char	buf[128];
	int	len, pos;
	bool	bytes;
};

static ssize_t next_byte(struct line_in *in, char *c)
{
	if (in->pos == in->len) {
		struct pt_readline rl = { in->buf, sizeof(in->buf) };
		int n = in->bytes ? -ENOTTY : pt_ioctl(PT_STDIN, PT_FILE_READLINE, &rl);

		if (n < 0 && n != -EINTR) {
			in->bytes = true;
			return pt_read(PT_STDIN, c, 1);
		}
		if (n <= 0)
			return n;
		in->len = n;
		in->pos = 0;
	}
	*c = in->buf[in->pos++];
	return 1;
}

/* read [-r] [-p prompt] [name...]: one line from stdin, split among the names */
static int b_read(struct sh *sh, int argc, char **argv)
{
	const char *ifs = pt_getenv("IFS"), *prompt = NULL;
	struct strbuf line = { 0 };
	struct line_in in = { .len = 0 };
	bool raw = false, eof = false;
	int i = 1;
	char c;

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "-r")) {
			raw = true;
		} else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
			prompt = argv[++i];
		} else if (!strcmp(argv[i], "--")) {
			i++;
			break;
		} else {
			pt_dprintf(PT_STDERR, "usage: read [-r] [-p prompt] [name...]\n");
			return 2;
		}
	}
	for (int k = i; k < argc; k++) {
		if (!valid_name(argv[k], strlen(argv[k]))) {
			pt_dprintf(PT_STDERR, "read: %s: not a valid name\n", argv[k]);
			return 2;
		}
	}
	if (prompt && pt_isatty(PT_STDIN))
		pt_dprintf(PT_STDERR, "%s", prompt);
	if (!ifs)
		ifs = " \t\n";

	/* whatever follows the line stays for the next reader */
	for (;;) {
		ssize_t n = next_byte(&in, &c);

		if (n == -EINTR) {
			sh_caught(sh);
			sb_free(&line);
			return 130;
		}
		if (n <= 0) {
			eof = true;
			break;
		}
		if (c == '\n')
			break;
		if (c == '\\' && !raw) {
			if (next_byte(&in, &c) <= 0) {
				eof = true;
				break;
			}
			if (c == '\n')
				continue;
			sb_putc(&line, CTLESC);
		} else if (c == CTLESC) {
			sb_putc(&line, CTLESC);
		}
		sb_putc(&line, c);
	}
	if (line.oom) {
		sb_free(&line);
		return 1;
	}

	const char *p = line.s ? line.s : "";
	if (i == argc) {
		char *reply = pt_strdup(p);
		char *o = reply;

		for (const char *q = p; reply && *q; q++)
			*o++ = *q == CTLESC && q[1] ? *++q : *q;
		if (reply)
			*o = '\0';
		pt_setenv("REPLY", reply ? reply : "");
		pt_free(reply);
	}
	for (int k = i; k < argc; k++) {
		struct strbuf value = { 0 };
		size_t keep = 0;	/* up to the last byte that is not a separator */

		while (is_ifs(ifs, *p))
			p++;
		for (; *p; p++) {
			if (*p == CTLESC && p[1]) {
				p++;
			} else if (is_ifs(ifs, *p)) {
				/* the last name takes the rest of the line */
				if (k < argc - 1)
					break;
				sb_putc(&value, *p);
				continue;
			}
			sb_putc(&value, *p);
			keep = value.len;
		}
		if (value.s)
			value.s[keep] = '\0';
		pt_setenv(argv[k], value.s ? value.s : "");
		sb_free(&value);
	}
	sb_free(&line);
	return eof ? 1 : 0;
}

/* Where `name` is found on $PATH, into `out`: false if nowhere. */
static bool in_path(const char *name, char *out, size_t size)
{
	const char *path = pt_getenv("PATH");
	struct pt_stat st;

	if (strchr(name, '/'))
		return !pt_stat(name, &st) && !st.is_dir && snprintf(out, size, "%s", name) > 0;
	for (const char *dir = path ? path : ""; *dir;) {
		size_t len = strcspn(dir, ":");

		if ((size_t)snprintf(out, size, "%.*s/%s", (int)len, dir, name) < size &&
		    !pt_stat(out, &st) && !st.is_dir)
			return true;
		dir += len + (dir[len] == ':');
	}
	return false;
}

/*
 * What a command name means, in the order the shell looks: a special
 * builtin, a function, a builtin, a program built into the system, a file
 * on $PATH. `verbose` says it as type does; otherwise as command -v does.
 */
static bool describe(struct sh *sh, const char *name, bool verbose)
{
	const struct builtin *b = builtin_find(name);
	struct alias *a = sh->interactive ? alias_find(sh, name) : NULL;
	char path[PT_PATH_MAX];
	const char *what = NULL;

	if (a) {
		if (verbose) {
			pt_printf("%s is aliased to ", name);
			print_quoted(a->value);
			pt_puts("\n");
		} else {
			alias_print(a);
		}
		return true;
	}
	if (b && b->special)
		what = "a special shell builtin";
	else if (function_find(sh, name))
		what = "a function";
	else if (b)
		what = "a shell builtin";
	else if (program_find(name))
		what = "a program built into the system";
	if (what) {
		if (verbose)
			pt_printf("%s is %s\n", name, what);
		else
			pt_printf("%s\n", name);
		return true;
	}
	if (in_path(name, path, sizeof(path))) {
		if (verbose)
			pt_printf("%s is %s\n", name, path);
		else
			pt_printf("%s\n", path);
		return true;
	}
	if (verbose)
		pt_dprintf(PT_STDERR, "%s: not found\n", name);
	return false;
}

static int b_type(struct sh *sh, int argc, char **argv)
{
	int status = 0;

	for (int i = 1; i < argc; i++)
		if (!describe(sh, argv[i], true))
			status = 1;
	return status;
}

/*
 * command -v NAME: what NAME would run. command NAME ARGS: run it, as a
 * builtin or a program, passing over a function of that name.
 */
static int b_command(struct sh *sh, int argc, char **argv)
{
	if (argc > 1 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "-V"))) {
		int status = 0;

		for (int i = 2; i < argc; i++)
			if (!describe(sh, argv[i], argv[1][1] == 'V'))
				status = 1;
		return status;
	}
	if (argc < 2)
		return 0;
	const struct builtin *b = builtin_find(argv[1]);

	if (b)
		return b->fn(sh, argc - 1, argv + 1);
	return run_command_argv(sh, argc - 1, argv + 1);
}

static int b_wait(struct sh *sh, int argc, char **argv)
{
	int status = 0, st, r;

	if (argc == 1) {
		while ((r = pt_wait(-1, &st, false)) > 0 || r == -EINTR) {
			if (r == -EINTR)
				goto interrupted;
			job_ended(sh, r, st, false);
		}
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		if ((r = pt_wait(atoi(argv[i]), &st, false)) == -EINTR)
			goto interrupted;
		if (r > 0)
			job_ended(sh, r, st, false);
		status = r > 0 ? st : 127;
	}
	return status;
interrupted:
	sh_caught(sh);
	return 130;
}

/* ------------------------------------------------------------ trap */

static const char *const trap_names[NTRAPS] = { "EXIT", "INT", "TERM" };

static int trap_index(const char *s)
{
	if (!strncmp(s, "SIG", 3))
		s += 3;
	if (!strcmp(s, "EXIT") || !strcmp(s, "0"))
		return TRAP_EXIT;
	if (!strcmp(s, "INT") || !strcmp(s, "2"))
		return TRAP_INT;
	if (!strcmp(s, "TERM") || !strcmp(s, "15"))
		return TRAP_TERM;
	return -1;
}

/* 'text' as the shell reads it back: quoted, its own quotes made safe. */
static void print_quoted(const char *s)
{
	pt_puts("'");
	for (; *s; s++)
		pt_puts(*s == '\'' ? "'\\''" : (char[]){ *s, 0 });
	pt_puts("'");
}

/*
 * trap 'commands' SIG...: run them when SIG arrives -- INT (Ctrl-C), TERM,
 * or EXIT, when the shell ends. trap '' SIG ignores it, trap - SIG (or
 * trap SIG) puts it back, trap alone lists them.
 */
static int b_trap(struct sh *sh, int argc, char **argv)
{
	int i = 1, status = 0;
	const char *action;

	if (argc == 1) {
		for (int k = 0; k < NTRAPS; k++) {
			if (!sh->traps[k])
				continue;
			pt_puts("trap -- ");
			print_quoted(sh->traps[k]);
			pt_printf(" %s\n", trap_names[k]);
		}
		return 0;
	}
	if (!strcmp(argv[1], "-l")) {
		pt_printf(" 0) EXIT   2) INT   15) TERM\n");
		return 0;
	}
	if (!strcmp(argv[i], "--"))
		i++;
	if (i == argc)
		return 0;
	action = argv[i];
	/* a lone signal, or one first: those go back to what they were */
	if (!strcmp(action, "-"))
		i++;
	else if (trap_index(action) >= 0 && (argc - i == 1 || isdigit((unsigned char)*action)))
		action = "-";
	else
		i++;
	for (; i < argc; i++) {
		int k = trap_index(argv[i]);

		if (k < 0) {
			pt_dprintf(PT_STDERR, "trap: %s: not a signal it knows (trap -l)\n", argv[i]);
			status = 1;
			continue;
		}
		pt_free(sh->traps[k]);
		sh->traps[k] = strcmp(action, "-") ? pt_strdup(action) : NULL;
	}
	return status;
}

/* ------------------------------------------------------------ alias */

struct alias *alias_find(struct sh *sh, const char *name)
{
	for (struct alias *a = sh->aliases; a; a = a->next)
		if (!strcmp(a->name, name))
			return a;
	return NULL;
}

static void alias_remove(struct sh *sh, const char *name)
{
	for (struct alias **pp = &sh->aliases; *pp; pp = &(*pp)->next) {
		if (!strcmp((*pp)->name, name)) {
			struct alias *a = *pp;

			*pp = a->next;
			pt_free(a->value);
			pt_free(a);
			return;
		}
	}
}

static void alias_print(const struct alias *a)
{
	pt_printf("alias %s=", a->name);
	print_quoted(a->value);
	pt_puts("\n");
}

/* alias NAME='text' ...: NAME at the start of a command stands for text. */
static int b_alias(struct sh *sh, int argc, char **argv)
{
	int status = 0;

	if (argc == 1) {
		for (struct alias *a = sh->aliases; a; a = a->next)
			alias_print(a);
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		const char *eq = strchr(argv[i], '=');
		size_t len = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);

		if (!eq) {
			struct alias *a = alias_find(sh, argv[i]);

			if (a) {
				alias_print(a);
			} else {
				pt_dprintf(PT_STDERR, "alias: %s: not found\n", argv[i]);
				status = 1;
			}
			continue;
		}
		if (!len || strcspn(argv[i], "\"'\\$`/ \t") < len) {
			pt_dprintf(PT_STDERR, "alias: %.*s: not a name for an alias\n", (int)len, argv[i]);
			status = 1;
			continue;
		}
		struct alias *a = pt_malloc(sizeof(*a) + len + 1);
		char *value = pt_strdup(eq + 1);

		if (!a || !value) {
			pt_free(a);
			pt_free(value);
			return 1;
		}
		memcpy(a->name, argv[i], len);
		a->name[len] = '\0';
		a->value = value;
		alias_remove(sh, a->name);
		a->next = sh->aliases;
		sh->aliases = a;
	}
	return status;
}

static int b_unalias(struct sh *sh, int argc, char **argv)
{
	int status = 0;

	if (argc > 1 && !strcmp(argv[1], "-a")) {
		while (sh->aliases)
			alias_remove(sh, sh->aliases->name);
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		if (!alias_find(sh, argv[i])) {
			pt_dprintf(PT_STDERR, "unalias: %s: not found\n", argv[i]);
			status = 1;
		}
		alias_remove(sh, argv[i]);
	}
	return status;
}

static const struct builtin builtins[] = {
	{ ".", b_source, true },
	{ ":", b_true, true },
	{ "[", b_test, false },
	{ "alias", b_alias, false },
	{ "bg", builtin_bg, false },
	{ "break", b_loop, true },
	{ "cd", b_cd, false },
	{ "command", b_command, false },
	{ "continue", b_loop, true },
	{ "eval", b_eval, true },
	{ "exit", b_exit, true },
	{ "export", b_export, true },
	{ "false", b_false, false },
	{ "fg", builtin_fg, false },
	{ "history", b_history, false },
	{ "jobs", builtin_jobs, false },
	{ "local", b_local, true },
	{ "read", b_read, false },
	{ "return", b_return, true },
	{ "set", b_set, true },
	{ "shift", b_shift, true },
	{ "source", b_source, true },
	{ "test", b_test, false },
	{ "trap", b_trap, true },
	{ "true", b_true, false },
	{ "type", b_type, false },
	{ "unalias", b_unalias, false },
	{ "unset", b_unset, true },
	{ "wait", b_wait, false },
};

const struct builtin *builtin_find(const char *name)
{
	for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
		if (!strcmp(builtins[i].name, name))
			return &builtins[i];
	return NULL;
}

/* Registered as programs so `help` and completion list them. */
#define SHELL_ONLY(name_, help_)						\
	PT_PROGRAM(name_, help_ "\n(shell builtin)")				\
	{									\
		pt_dprintf(PT_STDERR, "%s: only works as a shell command\n", #name_); \
		return 1;							\
	}

SHELL_ONLY(cd, "change the working directory\nusage: cd [dir | -]")
SHELL_ONLY(exit, "leave the shell\nusage: exit [status]")
SHELL_ONLY(export, "set or list environment variables\nusage: export [NAME=value ...]")
SHELL_ONLY(unset, "remove variables or functions\nusage: unset [-f] NAME ...")
SHELL_ONLY(source, "run a script in the current shell\nusage: source file  (or . file)")
SHELL_ONLY(history, "list previous commands")
SHELL_ONLY(break, "leave a for, while or until loop\nusage: break [levels]")
SHELL_ONLY(continue, "start the next turn of a loop\nusage: continue [levels]")
SHELL_ONLY(return, "leave a function or sourced script\nusage: return [status]")
SHELL_ONLY(local, "make function variables local\nusage: local NAME[=value] ...")
SHELL_ONLY(shift, "drop positional parameters: $2 becomes $1\nusage: shift [count]")
SHELL_ONLY(read, "read a line into variables\n"
	   "usage: read [-r] [-p prompt] [NAME ...]\n"
	   "Splits on spaces; the last NAME gets the rest.\n"
	   "No NAME: the line goes to $REPLY. -r keeps \\.")
SHELL_ONLY(eval, "run arguments as a command\nusage: eval text ...")
SHELL_ONLY(set, "set options, or $1 $2 ..., or list variables\n"
	   "usage: set [-eux] [+eux] [-o pipefail] [-- args ...]\n"
	   "  -e  stop at a command that fails (not in if, while, && ||)\n"
	   "  -u  an unset variable is an error   -x  show each command\n"
	   "  -o pipefail  a pipeline fails if any part of it does\n"
	   "+ turns one off; set -o lists them.")
SHELL_ONLY(wait, "wait for background jobs\nusage: wait [pid | %N ...]")
SHELL_ONLY(type, "say what a command name is: builtin, function,\n"
	   "program or file\nusage: type name ...")
SHELL_ONLY(trap, "run commands when a signal comes, or at the end\n"
	   "usage: trap 'commands' INT|TERM|EXIT ...\n"
	   "  trap '' INT    ignore Ctrl-C   trap - INT   as usual again\n"
	   "  trap 'rm -f /tmp/x.$$' EXIT     tidy up at the end\n"
	   "trap alone lists them.")
SHELL_ONLY(alias, "give a command a short name\n"
	   "usage: alias [name='text' ...]\n"
	   "  alias ll='ls -l'   then ll /tmp is ls -l /tmp\n"
	   "Put them in ~/.profile. Alone, lists them.")
SHELL_ONLY(unalias, "forget aliases\nusage: unalias name ... | -a")
SHELL_ONLY(jobs, "list the jobs: what runs in the background or\n"
	   "was stopped with Ctrl-Z")
SHELL_ONLY(fg, "bring a job to the front\nusage: fg [%N]\n"
	   "A stopped one carries on. %N is job N; without it, the\n"
	   "last one stopped or started.")
SHELL_ONLY(bg, "let a stopped job carry on in the background\n"
	   "usage: bg [%N]")
SHELL_ONLY(command, "run a command passing over functions, or say\n"
	   "what it is\nusage: command [-v | -V] name [args ...]")
