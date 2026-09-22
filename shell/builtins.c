/*
 * sh: builtin commands. They run inside the shell process because they
 * change it: its variables, directory, parameters or flow of control.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt/program.h"
#include "sh.h"

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
	return 0;
}

static int b_exit(struct sh *sh, int argc, char **argv)
{
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

/* set -- args: replace the positional parameters */
static int b_set(struct sh *sh, int argc, char **argv)
{
	const char *entry;
	int first = argc > 1 && !strcmp(argv[1], "--") ? 2 : 1;

	if (argc == 1) {
		for (int i = 0; pt_environ(i, &entry); i++)
			pt_printf("%s\n", entry);
		return 0;
	}
	if (first == 1 && (argv[1][0] == '-' || argv[1][0] == '+')) {
		pt_dprintf(PT_STDERR, "set: %s: options are not supported\n", argv[1]);
		return 2;
	}

	int count = argc - first + 1;
	size_t size = (count + 1) * sizeof(char *) + strlen(sh->argv[0]) + 1;
	for (int i = first; i < argc; i++)
		size += strlen(argv[i]) + 1;
	char **block = pt_malloc(size);
	if (!block)
		return 1;
	char *s = (char *)(block + count + 1);
	for (int i = 0; i < count; i++) {
		block[i] = s;
		s = stpcpy(s, i ? argv[first + i - 1] : sh->argv[0]) + 1;
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

/* read [-r] [-p prompt] [name...]: one line from stdin, split among the names */
static int b_read(struct sh *sh, int argc, char **argv)
{
	const char *ifs = pt_getenv("IFS"), *prompt = NULL;
	struct strbuf line = { 0 };
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

	/* a byte at a time: whatever follows the line stays for the next reader */
	for (;;) {
		ssize_t n = pt_read(PT_STDIN, &c, 1);

		if (n == -EINTR) {
			pt_sigcatch(true);
			sh->interrupted = true;
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
			if (pt_read(PT_STDIN, &c, 1) <= 0) {
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

static int b_wait(struct sh *sh, int argc, char **argv)
{
	int status = 0, st, r;

	if (argc == 1) {
		while ((r = pt_wait(-1, &st, false)) > 0 || r == -EINTR)
			if (r == -EINTR)
				goto interrupted;
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		if ((r = pt_wait(atoi(argv[i]), &st, false)) == -EINTR)
			goto interrupted;
		status = r > 0 ? st : 127;
	}
	return status;
interrupted:
	pt_sigcatch(true);
	sh->interrupted = true;
	return 130;
}

static const struct builtin builtins[] = {
	{ ".", b_source, true },
	{ ":", b_true, true },
	{ "[", b_test, false },
	{ "break", b_loop, true },
	{ "cd", b_cd, false },
	{ "continue", b_loop, true },
	{ "eval", b_eval, true },
	{ "exit", b_exit, true },
	{ "export", b_export, true },
	{ "false", b_false, false },
	{ "history", b_history, false },
	{ "local", b_local, true },
	{ "read", b_read, false },
	{ "return", b_return, true },
	{ "set", b_set, true },
	{ "shift", b_shift, true },
	{ "source", b_source, true },
	{ "test", b_test, false },
	{ "true", b_true, false },
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
SHELL_ONLY(set, "list variables, or set $1 $2 ...\nusage: set [-- args ...]")
SHELL_ONLY(wait, "wait for background jobs\nusage: wait [pid ...]")
