/*
 * PocketType's system calls on the PC, over POSIX, for testing the programs
 * in bin/ that use nothing but those: they are built into one binary with
 * this file, and
 *
 *	host_pt grep -E 'a+b' file
 *
 * runs PocketType's grep. tools/programs_test.py builds it with the
 * sanitizers and compares the programs with GNU's.
 *
 * pt_spawn() runs this same binary again, so xargs and find -exec work.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "pt/keys.h"
#include "pt/program.h"
#include "pt/sys.h"

extern char **environ;

/* ------------------------------------------------------------ programs */

static const struct pt_program *programs[256];
static int nprograms;
static const char *self;

/* In the order registered: nothing here lists them. */
void program_register(const struct pt_program *prog)
{
	if (nprograms < (int)(sizeof(programs) / sizeof(*programs)))
		programs[nprograms++] = prog;
}

/* What Tab completes is the shell's business, and there is none here. */
void completion_register(const struct pt_completion *c)
{
}

const struct pt_program *program_find(const char *name)
{
	for (int i = nprograms - 1; i >= 0; i--)
		if (!strcmp(programs[i]->name, name))
			return programs[i];
	return NULL;
}

const struct pt_program *program_at(int i)
{
	return i >= 0 && i < nprograms ? programs[i] : NULL;
}

int main(int argc, char **argv)
{
	const char *base = strrchr(argv[0], '/');
	const struct pt_program *p = program_find(base ? base + 1 : argv[0]);

	/* the binary itself, for pt_spawn: argv[0] may be a bare name */
	self = realpath("/proc/self/exe", NULL);
	if (!self)
		self = argv[0];
	if (!p && argc > 1 && (p = program_find(argv[1]))) {
		argv++;
		argc--;
	}
	if (!p) {
		fprintf(stderr, "host_pt: which program?\n");
		return 127;
	}
	return p->main(argc, argv);
}

/* ------------------------------------------------------------ files */

/* A POSIX result the board's way: -errno on failure. Called once, as the
 * call it wraps must be. */
static long neg(long r)
{
	return r < 0 ? -errno : r;
}

#define NEG(x)	neg(x)

ssize_t pt_read(int fd, void *buf, size_t n) { return NEG(read(fd, buf, n)); }
ssize_t pt_write(int fd, const void *buf, size_t n)
{
	ssize_t r = write(fd, buf, n);

	if (r < 0 && errno == EPIPE)
		exit(128 + 13);		/* as the board does for SIGPIPE */
	return NEG(r);
}
int pt_open(const char *path, int flags) { return NEG(open(path, flags, 0666)); }
int pt_close(int fd) { return NEG(close(fd)); }
off_t pt_lseek(int fd, off_t off, int whence) { return NEG(lseek(fd, off, whence)); }
int pt_dup(int fd) { return NEG(dup(fd)); }
int pt_dup2(int a, int b) { return NEG(dup2(a, b)); }
int pt_pipe(int fds[2]) { return NEG(pipe(fds)); }
int pt_ioctl(int fd, int req, void *arg) { return -ENOTTY; }
bool pt_isatty(int fd) { return isatty(fd); }

int pt_stat(const char *path, struct pt_stat *st)
{
	struct stat s;

	if (stat(path, &s))
		return -errno;
	st->size = s.st_size;
	st->mtime = s.st_mtime;
	st->is_dir = S_ISDIR(s.st_mode);
	return 0;
}

int pt_mkdir(const char *path) { return NEG(mkdir(path, 0777)); }
int pt_rmdir(const char *path) { return NEG(rmdir(path)); }
int pt_unlink(const char *path) { return NEG(unlink(path)); }
int pt_rename(const char *a, const char *b) { return NEG(rename(a, b)); }

int pt_utime(const char *path, time_t mtime)
{
	struct utimbuf t = { .actime = mtime, .modtime = mtime };

	return NEG(utime(path, mtime ? &t : NULL));
}

struct pt_dir {
	DIR	*d;
	char	 path[PATH_MAX];
};

int pt_opendir(const char *path, pt_dir_t **out)
{
	struct pt_dir *d = calloc(1, sizeof(*d));

	if (!d)
		return -ENOMEM;
	if (!(d->d = opendir(path))) {
		int err = -errno;

		free(d);
		return err;
	}
	snprintf(d->path, sizeof(d->path), "%s", path);
	*out = d;
	return 0;
}

/* The board's filesystems do not list . and .., so neither does this. */
int pt_readdir(pt_dir_t *d, struct pt_dirent *ent)
{
	struct dirent *e;

	while ((e = readdir(d->d)) && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")))
		;
	if (!e)
		return 0;
	snprintf(ent->name, sizeof(ent->name), "%s", e->d_name);
	if (e->d_type == DT_UNKNOWN) {
		char full[PATH_MAX * 2];
		struct stat s;

		snprintf(full, sizeof(full), "%s/%s", d->path, e->d_name);
		ent->is_dir = !stat(full, &s) && S_ISDIR(s.st_mode);
	} else {
		ent->is_dir = e->d_type == DT_DIR;
	}
	return 1;
}

void pt_closedir(pt_dir_t *d)
{
	if (d) {
		closedir(d->d);
		free(d);
	}
}

int pt_chdir(const char *path) { return NEG(chdir(path)); }

const char *pt_getcwd(void)
{
	static char cwd[PATH_MAX];

	return getcwd(cwd, sizeof(cwd)) ? cwd : "/";
}

/* As the kernel does it: joined onto the working directory, . and .. gone. */
int pt_abspath(const char *path, char *out, size_t size)
{
	char joined[PATH_MAX * 2], *parts[256];
	int depth = 0;
	size_t o = 0;

	snprintf(joined, sizeof(joined), "%s/%s", path[0] == '/' ? "" : pt_getcwd(), path);
	for (char *s = strtok(joined, "/"); s; s = strtok(NULL, "/")) {
		if (!strcmp(s, "."))
			continue;
		if (!strcmp(s, "..")) {
			depth -= depth > 0;
			continue;
		}
		if (depth == 256)
			return -ENAMETOOLONG;
		parts[depth++] = s;
	}
	for (int i = 0; i < depth; i++) {
		int n = snprintf(out + o, size - o, "/%s", parts[i]);

		if (n < 0 || (size_t)n >= size - o)
			return -ENAMETOOLONG;
		o += n;
	}
	if (!depth)
		snprintf(out, size, "/");
	return 0;
}

/* ------------------------------------------------------------ processes */

int pt_spawn(const struct pt_spawn *req)
{
	posix_spawn_file_actions_t fa;
	char **argv = calloc(req->argc + 2, sizeof(*argv));
	pid_t pid;
	int err;

	if (!argv)
		return -ENOMEM;
	argv[0] = (char *)self;
	argv[1] = (char *)req->cmd;
	for (int i = 1; i < req->argc; i++)
		argv[i + 1] = req->argv[i];
	posix_spawn_file_actions_init(&fa);
	for (int i = 0; i < 3; i++)
		if (req->fd[i] >= 0 && req->fd[i] != i)
			posix_spawn_file_actions_adddup2(&fa, req->fd[i], i);
	/* one of ours, or else the PC's own: -ENOENT if there is none */
	if (program_find(req->cmd))
		err = posix_spawn(&pid, self, &fa, NULL, argv, environ);
	else
		err = posix_spawnp(&pid, req->cmd, &fa, NULL, argv + 1, environ);
	posix_spawn_file_actions_destroy(&fa);
	free(argv);
	return err ? -err : pid;
}

int pt_wait(int pid, int *status, int flags)
{
	int st;
	pid_t got = waitpid(pid > 0 ? pid : -1, &st, flags & PT_WNOHANG ? WNOHANG : 0);

	if (got < 0)
		return -errno;
	if (got > 0 && status)
		*status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
	return got;
}

void pt_exit(int status) { exit(status); }
int pt_kill(int pid, int sig) { return NEG(kill(pid, sig)); }
int pt_getpid(void) { return getpid(); }
bool pt_interrupted(void) { return false; }
unsigned pt_sigpending(void) { return 0; }
void pt_sigcatch(bool on) { }
int pt_sleep_ms(uint32_t ms) { usleep(ms * 1000); return 0; }

int64_t pt_uptime_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

const char *pt_getenv(const char *name) { return getenv(name); }

/* kernel/sys.c's, over the PC's own calls: "$HOME/dir/name", dir made, "$HOME/old" moved in. */
int pt_home_file(const char *dir, const char *name, const char *old, char *out, size_t size)
{
	const char *home = getenv("HOME");
	char from[PT_PATH_MAX];
	struct stat st;

	if (!home || !*home)
		return -ENOENT;
	if ((size_t)snprintf(out, size, "%s/%s", home, dir) >= size)
		return -ENAMETOOLONG;
	mkdir(out, 0777);
	if ((size_t)snprintf(out, size, "%s/%s/%s", home, dir, name) >= size)
		return -ENAMETOOLONG;
	if (old && (size_t)snprintf(from, sizeof(from), "%s/%s", home, old) < sizeof(from) &&
	    !stat(from, &st) && S_ISREG(st.st_mode) && stat(out, &st))
		rename(from, out);
	return 0;
}
int pt_setenv(const char *name, const char *value) { return NEG(setenv(name, value, 1)); }
int pt_unsetenv(const char *name) { return NEG(unsetenv(name)); }

int pt_environ(int index, const char **entry)
{
	for (int i = 0; environ[i]; i++)
		if (i == index) {
			*entry = environ[i];
			return 1;
		}
	return 0;
}

/* ------------------------------------------------------------ memory */

void *pt_malloc(size_t n) { return malloc(n ? n : 1); }
void *pt_calloc(size_t count, size_t n) { return calloc(count ? count : 1, n ? n : 1); }
void *pt_realloc(void *p, size_t n) { return realloc(p, n ? n : 1); }
void pt_free(void *p) { free(p); }
char *pt_strdup(const char *s) { return strdup(s); }

/* ------------------------------------------------------------ output */

int pt_vdprintf(int fd, const char *fmt, va_list ap) { return vdprintf(fd, fmt, ap); }

int pt_dprintf(int fd, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vdprintf(fd, fmt, ap);
	va_end(ap);
	return r;
}

int pt_printf(const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vdprintf(1, fmt, ap);
	va_end(ap);
	return r;
}

int pt_puts(const char *s) { return pt_write(1, s, strlen(s)); }

int pt_getc(int fd)
{
	unsigned char c;
	ssize_t r = read(fd, &c, 1);

	return r == 1 ? c : r == 0 ? -1 : -errno;
}

const char *pt_strerror(int err) { return strerror(err < 0 ? -err : err); }

/* ------------------------------------------------------------ terminal */

int pt_readkey(int fd) { int c = pt_getc(fd); return c < 0 ? PT_KEY_EOF : c; }
int pt_readkey_timeout(int fd, int ms) { return PT_KEY_NONE; }
int pt_tty_raw(int fd, bool raw) { return 0; }

void pt_tty_size(int fd, int *cols, int *rows)
{
	*cols = 80;
	*rows = 24;
}
