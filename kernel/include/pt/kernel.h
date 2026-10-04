/*
 * Kernel-internal interfaces: shared by the kernel, drivers and boot code.
 * Programs use pt/sys.h instead.
 */
#pragma once

#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pt/sys.h"

#define PT_OS_NAME	"pico-os"
#define PT_VERSION	"1.0-beta3"
#define PT_MAX_FDS	16

/* ------------------------------------------------------------ klog */

void	klog_init(void);
void	klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void	klog_set_console(void (*write)(const char *s, size_t n));
size_t	klog_size(void);
size_t	klog_read(size_t off, char *buf, size_t n);	/* oldest byte is off 0 */
void	klog_clear(void);

/* ------------------------------------------------------------ files */

struct pt_file;

struct pt_file_ops {
	ssize_t	(*read)(struct pt_file *f, void *buf, size_t n);
	ssize_t	(*write)(struct pt_file *f, const void *buf, size_t n);
	off_t	(*lseek)(struct pt_file *f, off_t off, int whence);
	int	(*ioctl)(struct pt_file *f, int req, void *arg);
	int	(*flush)(struct pt_file *f);	/* on every close: write out buffers */
	void	(*release)(struct pt_file *f);	/* last reference dropped */
};

struct pt_file {
	const struct pt_file_ops *ops;
	void		*priv;
	atomic_int	 refs;
	bool		 is_tty;
};

struct pt_file *file_alloc(const struct pt_file_ops *ops, void *priv);
struct pt_file *file_get(struct pt_file *f);
void		file_put(struct pt_file *f);

struct pt_mount;

void		file_init(void);
struct pt_file *vfs_file_open(const struct pt_mount *m, const char *vfspath, int flags, int *err);
int		vfs_sync_all(void);	/* write out and fsync every open file */
bool		vfs_mount_busy(const struct pt_mount *m);	/* files open on it */
struct pt_file *mem_file_open(char *data, size_t len);	/* takes ownership */
int		pipe_create(struct pt_file **rd, struct pt_file **wr);

/* ------------------------------------------------------------ processes */

enum proc_state {
	PROC_FREE = 0,
	PROC_RUNNING,
	PROC_ZOMBIE,
};

struct proc {
	int			 pid;
	int			 ppid;
	int			 pgid;
	enum proc_state		 state;
	int			 status;
	char			 name[16];
	TaskHandle_t		 task;
	SemaphoreHandle_t	 exited;
	SemaphoreHandle_t	 cont;		/* given to wake it from a stop */
	bool			 stopped;	/* by SIGSTOP or SIGTSTP */
	bool			 stop_reported;	/* and its parent knows */
	int			 stop_sig;	/* which of them */
	struct pt_file		*fd[PT_MAX_FDS];
	char			 cwd[PT_PATH_MAX];
	char			*env;		/* "A=1\0B=2\0\0" */
	size_t			 env_len;
	char			*args;		/* argv block */
	struct alloc_hdr	*allocs;
	struct pt_dir		*dirs;		/* open, closed at exit if the program did not */
	atomic_uint		 sigpending;
	bool			 sigcatch;
	atomic_bool		 exiting;
	bool			 forced_cleanup;	/* suspended while helpers finish */
	bool			 (*cleanup)(void *arg);
	void			 *cleanup_arg;
	int64_t			 kill_deadline_us;
	bool			 kill_waiting;	/* past it, but holding a lock */
	int64_t			 start_us;
	uint32_t		 stack_kb;
	const struct pt_program	*prog;
	const struct pt_loader	*loader;
	int			 argc;
	char			**argv;
	const char		*exec_path;
};

struct pt_procinfo {
	int	 pid, ppid, pgid;
	char	 state;			/* R, T (stopped), Z */
	char	 name[16];
	uint32_t stack_kb;
	uint32_t stack_free;
	int64_t	 start_us;
	uint64_t cpu_us;		/* processor time used, both cores */
};

void	proc_init(void);
/*
 * Process stacks are in PSRAM, which the flash and sleep code cannot use
 * (internal.c): this runs fn(arg) on a kernel task with an internal stack
 * and returns what it returns, or calls it directly when the caller's
 * stack is internal already.
 */
void	internal_init(void);
int	on_internal_stack(int (*fn)(void *arg), void *arg);
void	restart_now(void);		/* esp_restart(), from any stack */
struct proc *proc_current(void);	/* NULL for kernel tasks */
int	proc_spawn_console(int argc, char **argv, struct pt_file *console);
int	proc_wait_orphan(int pid);	/* for kernel code: wait for a pid it spawned */
void	proc_signal_group(int pgid, int sig);
int	proc_list(struct pt_procinfo *out, int max);
int	proc_count(void);
bool	proc_alive(int pid);		/* whether that process is still running */
bool	proc_group_alive(int pgid);	/* includes children after the leader exits */
bool	proc_stopped(int pid);		/* stopped by SIGSTOP or SIGTSTP */
/* Called before process memory or its stack is freed, including SIGKILL.
 * Return false to retry later; never block or use caller-relative syscalls.
 * The context may live on the process stack. NULL removes the hook. */
int	proc_set_cleanup(bool (*fn)(void *arg), void *arg);
/*
 * How often a wait looks for signals: every `ms` normally, once a second
 * while nobody is looking (the screen is dark), so the chip can sleep.
 */
void	proc_set_quiet(bool quiet);
int	proc_poll_ms(int ms);
void	mem_release_all(struct proc *p);
/*
 * pt_malloc from particular memory -- MALLOC_CAP_INTERNAL for a buffer the
 * card or the flash wants, say -- and freed with pt_free, or at exit like
 * the rest. The header keeps the heap's alignment, which DMA is happy with.
 * Not for pt_realloc, which would move it to wherever pt_malloc prefers.
 */
void	*pt_malloc_caps(size_t n, uint32_t caps);
/*
 * PSRAM as the chip has it, in bytes: the whole of it, and what is not
 * free -- the heap's blocks, and what the heap was never given: the
 * program itself, copied there at boot to run from (XIP), and what the
 * SDK set aside. Zero and zero without PSRAM.
 */
void	mem_psram(size_t *total, size_t *used);
void	dir_release_all(struct proc *p);	/* sys.c: what the program left open */

/* clock.c: the time kept in /etc/clock, for a boot after the power was cut */
void	clock_restore(void);		/* at boot, once / is mounted */
void	clock_save(void);		/* now: the power is about to go */
void	clock_sleeping(void);		/* deep sleep is next: mark when it began */
uint64_t clock_sleep_us(uint64_t us);	/* a timer wake-up, allowing for drift */
void	clock_changed(bool network);	/* it was set: save it soon */
void	clock_tick(void);		/* once a second, from a task that may write files */

/*
 * user.c: who uses the machine and where -- the name, and so the home
 * directory, and the time zone -- kept in /etc/user and /etc/timezone.
 */
#define USER_NAME_MAX	31
#define USER_TZ_MAX	47
void	user_restore(void);		/* at boot, once / is mounted */
bool	user_configured(void);		/* /etc/user is there: setup has run */
bool	user_name_ok(const char *name);
const char *user_name(void);
const char *user_home(void);		/* "/home/<name>" */
const char *user_tz(void);		/* the POSIX TZ string */
const char *user_tz_name(void);		/* "Europe/Rome", or "" */
int	user_set_name(const char *name);	/* -EINVAL: not a user name */
int	user_set_tz(const char *name, const char *tz);

/*
 * etc.c: /etc kept on the card too, in .etc there, so the settings outlive
 * the flash being erased: a / with no user takes them back at boot.
 */
int	etc_init(void);			/* at boot, once the card is mounted: files taken back */
int	etc_save(void);			/* the card's copy up to date now: files written */
void	etc_hold(void);			/* nothing written to the card until etc_release() */
void	etc_release(void);
void	etc_stop(void);			/* nothing more until the restart: / is being wiped */

/* auth.c: the password the network shell asks for, kept hashed in /etc/shadow */
bool	auth_is_set(void);
int	auth_check(const char *password);	/* 0, -EACCES, or -ENOENT with none set */
int	auth_set(const char *password);		/* NULL removes it */

/* Signals are delivered at safe points: every syscall checks on entry. */
void	proc_check_signals(void);
/*
 * Where a system call that waits a long time -- for the terminal, a pipe,
 * a sleep -- lets a stop happen: it holds no locks when it calls this.
 */
void	proc_stop_point(void);
bool	proc_stop_pending(void);

/* ------------------------------------------------------------ paths */

int	path_normalize(const char *cwd, const char *in, char *out, size_t size);
const char *path_basename(const char *path);

/* ------------------------------------------------------------ mounts */

/*
 * Filesystems attached to the namespace. Users see `path` ("/", "/tmp",
 * "/mnt/sd"); the files live under the ESP-IDF VFS prefix `vfs`. A path
 * belongs to the mount with the longest matching `path`, as on Linux, and
 * a mount that is not present exposes the directory underneath it.
 */
struct pt_mount {
	const char *path;	/* NULL: the user's home, whatever it is now */
	const char *vfs;
	const char *type;
	const char *source;
	bool	    bind;	/* the same filesystem, seen again elsewhere */
	bool	    (*present)(void);
	int	    (*info)(uint64_t *total, uint64_t *free);
};

void	mount_register(const struct pt_mount *m);
const char *mount_path(const struct pt_mount *m);
int	mount_count(void);
const struct pt_mount *mount_get(int index);

/* Mount holding `abs` and the VFS path it maps to; NULL if nothing is mounted. */
const struct pt_mount *mount_resolve(const char *abs, char *vfs, size_t size);

/* `abs` is the mount point of a present mount (or "/"). */
bool	mount_is_point(const char *abs);

/*
 * Filesystems the kernel serves itself, each a flat directory at /<name>.
 * `entry` is the file name inside it.
 */
struct pt_kernfs {
	const char	*name;
	bool		 writable;
	bool		(*lookup)(const char *entry);
	struct pt_file	*(*open)(const char *entry, int *err);
	int		(*readdir)(int index, struct pt_dirent *ent);	/* 1, 0 at end */
};

extern const struct pt_kernfs procfs;	/* procfs.c */
extern const struct pt_kernfs devfs;	/* devfs.c */

/* A driver adds its own character device to /dev, at boot. */
int	dev_register(const char *name, const struct pt_file_ops *ops);

/* ... and its own text file to /proc, written when someone reads it. */
int	proc_register(const char *name, int (*gen)(char *buf, size_t size));
