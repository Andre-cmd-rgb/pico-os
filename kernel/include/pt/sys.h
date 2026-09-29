/*
 * PocketType system calls, ABI version 1.
 *
 * This is the whole interface a program gets. Built-in programs call these
 * functions directly; a loader for compiled programs (bytecode VM, native
 * code) exposes the same set through struct pt_syscalls, so both kinds of
 * program see one system.
 *
 * Conventions, as in the Linux kernel: calls that can fail return a negative
 * errno value (-ENOENT, ...) and never touch the global errno. Paths may be
 * absolute or relative to the calling process's working directory.
 */
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PT_ABI_VERSION	1
#define PT_PATH_MAX	256
#define PT_NAME_MAX	256

#define PT_STDIN	0
#define PT_STDOUT	1
#define PT_STDERR	2

#define PT_SIGINT	2
#define PT_SIGKILL	9
#define PT_SIGPIPE	13
#define PT_SIGTERM	15
#define PT_SIGCONT	18	/* carry on after a stop */
#define PT_SIGSTOP	19	/* stop, and nothing can catch it */
#define PT_SIGTSTP	20	/* stop: Ctrl-Z */

/* ------------------------------------------------------------ files */

ssize_t pt_read(int fd, void *buf, size_t n);
ssize_t pt_write(int fd, const void *buf, size_t n);
int	pt_open(const char *path, int flags);	/* O_RDONLY, O_CREAT, ... */
int	pt_close(int fd);
off_t	pt_lseek(int fd, off_t off, int whence);
int	pt_dup(int fd);
int	pt_dup2(int oldfd, int newfd);
int	pt_pipe(int fds[2]);
int	pt_memfd(const void *data, size_t len);	/* a read-only file of a copy of data */
int	pt_ioctl(int fd, int req, void *arg);
bool	pt_isatty(int fd);

struct pt_stat {
	uint64_t size;
	time_t	 mtime;
	bool	 is_dir;
};

struct pt_dirent {
	char name[PT_NAME_MAX];
	bool is_dir;
};

typedef struct pt_dir pt_dir_t;

int	pt_stat(const char *path, struct pt_stat *st);
int	pt_mkdir(const char *path);
int	pt_rmdir(const char *path);
int	pt_unlink(const char *path);
int	pt_rename(const char *from, const char *to);
int	pt_utime(const char *path, time_t mtime);	/* 0: now */
int	pt_opendir(const char *path, pt_dir_t **dir);
int	pt_readdir(pt_dir_t *dir, struct pt_dirent *ent);	/* 1, 0 at end, <0 */
void	pt_closedir(pt_dir_t *dir);

int	pt_chdir(const char *path);
const char *pt_getcwd(void);
int	pt_abspath(const char *path, char *out, size_t size);

/* ------------------------------------------------------------ processes */

struct pt_spawn {
	const char	*cmd;		/* program name or path to an executable */
	int		 argc;
	char *const	*argv;		/* argv[0] included */
	int		 fd[3];		/* caller fds for the child's 0, 1, 2 */
	int		 pgid;		/* 0: new group led by the child */
};

int	pt_spawn(const struct pt_spawn *req);			/* pid or <0 */
/*
 * pid, 0 (PT_WNOHANG and nothing has finished), or <0. With PT_WUNTRACED a
 * child that has stopped is reported too, once, with a status of
 * PT_WSTOPPED plus the signal that stopped it.
 */
int	pt_wait(int pid, int *status, int flags);
#define PT_WNOHANG	1
#define PT_WUNTRACED	2
#define PT_WSTOPPED	0x10000
void	pt_exit(int status) __attribute__((noreturn));
int	pt_kill(int pid, int sig);
int	pt_getpid(void);
bool	pt_interrupted(void);	/* SIGINT, SIGTERM or SIGKILL pending */
unsigned pt_sigpending(void);	/* which: bit N for signal N */
/*
 * By default SIGINT and SIGTERM end a process at its next system call.
 * A process that catches them keeps running and polls pt_interrupted();
 * turning catching on clears any pending SIGINT/SIGTERM.
 */
void	pt_sigcatch(bool on);
int	pt_sleep_ms(uint32_t ms);	/* -EINTR if interrupted */
int64_t	pt_uptime_us(void);

const char *pt_getenv(const char *name);
/* "$HOME/dir/name", dir made, a file at "$HOME/old" moved in: see sys.c */
int	pt_home_file(const char *dir, const char *name, const char *old, char *out, size_t size);
int	pt_setenv(const char *name, const char *value);
int	pt_unsetenv(const char *name);
int	pt_environ(int index, const char **entry);		/* "NAME=value" */

/* ------------------------------------------------------------ memory */

/* Tracked per process: whatever a program forgets is freed when it exits. */
void	*pt_malloc(size_t n);
void	*pt_calloc(size_t count, size_t n);
void	*pt_realloc(void *p, size_t n);
void	 pt_free(void *p);
char	*pt_strdup(const char *s);

/* ------------------------------------------------------------ terminal */

#define PT_TTY_SETRAW	0x5401	/* int *: 1 raw, 0 cooked */
#define PT_TTY_GETSIZE	0x5402	/* struct pt_winsize * */
#define PT_TTY_SETPGRP	0x5403	/* int *: foreground process group */
#define PT_TTY_SETTIMEOUT 0x5404 /* int *: read timeout in ms, -1 blocks; -EAGAIN on expiry */
#define PT_TTY_GETRAW	0x5405	/* int *: 1 raw, 0 cooked */

struct pt_winsize {
	uint16_t cols;
	uint16_t rows;
};

int	pt_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int	pt_dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int	pt_vdprintf(int fd, const char *fmt, va_list ap);
int	pt_puts(const char *s);		/* no newline added */
int	pt_getc(int fd);		/* byte, -1 at end, <-1 error */
const char *pt_strerror(int err);	/* accepts -ENOENT or ENOENT */

/* ------------------------------------------------------------ table */

struct pt_syscalls {
	uint32_t abi;
	ssize_t	(*read)(int, void *, size_t);
	ssize_t	(*write)(int, const void *, size_t);
	int	(*open)(const char *, int);
	int	(*close)(int);
	off_t	(*lseek)(int, off_t, int);
	int	(*dup2)(int, int);
	int	(*pipe)(int[2]);
	int	(*ioctl)(int, int, void *);
	int	(*stat)(const char *, struct pt_stat *);
	int	(*mkdir)(const char *);
	int	(*rmdir)(const char *);
	int	(*unlink)(const char *);
	int	(*rename)(const char *, const char *);
	int	(*opendir)(const char *, pt_dir_t **);
	int	(*readdir)(pt_dir_t *, struct pt_dirent *);
	void	(*closedir)(pt_dir_t *);
	int	(*chdir)(const char *);
	const char *(*getcwd)(void);
	int	(*spawn)(const struct pt_spawn *);
	int	(*wait)(int, int *, int);
	void	(*exit)(int);
	int	(*kill)(int, int);
	void	(*sigcatch)(bool);
	bool	(*interrupted)(void);
	int	(*getpid)(void);
	int	(*sleep_ms)(uint32_t);
	int64_t	(*uptime_us)(void);
	const char *(*getenv)(const char *);
	int	(*setenv)(const char *, const char *);
	void	*(*malloc)(size_t);
	void	*(*realloc)(void *, size_t);
	void	(*free)(void *);
};

extern const struct pt_syscalls pt_sys;

#ifdef __cplusplus
}
#endif
