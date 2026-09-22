/*
 * System calls: fd table, filesystem namespace, formatted output.
 *
 * The namespace: /dev and /proc are served by the kernel; every other path
 * goes through the mount table to a file under an ESP-IDF VFS prefix, so
 * "/etc/motd" on the root filesystem is "/rootfs/etc/motd" underneath.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"

#include "pt/kernel.h"

/* ------------------------------------------------------------ fd table */

static struct pt_file *fd_file(struct proc *p, int fd)
{
	return p && fd >= 0 && fd < PT_MAX_FDS ? p->fd[fd] : NULL;
}

static int fd_install(struct proc *p, struct pt_file *f)
{
	for (int i = 0; i < PT_MAX_FDS; i++) {
		if (!p->fd[i]) {
			p->fd[i] = f;
			return i;
		}
	}
	return -EMFILE;
}

ssize_t pt_read(int fd, void *buf, size_t n)
{
	proc_check_signals();
	struct pt_file *f = file_get(fd_file(proc_current(), fd));

	if (!f)
		return -EBADF;
	ssize_t r = f->ops->read ? f->ops->read(f, buf, n) : -EBADF;
	file_put(f);
	return r;
}

ssize_t pt_write(int fd, const void *buf, size_t n)
{
	proc_check_signals();
	struct pt_file *f = file_get(fd_file(proc_current(), fd));

	if (!f)
		return -EBADF;
	ssize_t r = f->ops->write ? f->ops->write(f, buf, n) : -EBADF;
	file_put(f);

	/* Like SIGPIPE: `cat big | head` ends cat quietly instead of an error. */
	struct proc *p = proc_current();
	if (r == -EPIPE && p && !p->sigcatch)
		pt_exit(128 + PT_SIGPIPE);
	return r;
}

off_t pt_lseek(int fd, off_t off, int whence)
{
	proc_check_signals();
	struct pt_file *f = fd_file(proc_current(), fd);

	if (!f)
		return -EBADF;
	return f->ops->lseek ? f->ops->lseek(f, off, whence) : -ESPIPE;
}

int pt_ioctl(int fd, int req, void *arg)
{
	proc_check_signals();
	struct pt_file *f = fd_file(proc_current(), fd);

	if (!f)
		return -EBADF;
	return f->ops->ioctl ? f->ops->ioctl(f, req, arg) : -ENOTTY;
}

bool pt_isatty(int fd)
{
	struct pt_file *f = fd_file(proc_current(), fd);

	return f && f->is_tty;
}

/* Like close(2), reports a failure to write out buffered data (a full disk). */
int pt_close(int fd)
{
	struct proc *p = proc_current();
	struct pt_file *f = fd_file(p, fd);

	if (!f)
		return -EBADF;
	p->fd[fd] = NULL;
	int err = f->ops->flush ? f->ops->flush(f) : 0;
	file_put(f);
	return err;
}

int pt_dup(int fd)
{
	struct proc *p = proc_current();
	struct pt_file *f = fd_file(p, fd);

	if (!f)
		return -EBADF;
	int nfd = fd_install(p, file_get(f));
	if (nfd < 0)
		file_put(f);
	return nfd;
}

int pt_dup2(int oldfd, int newfd)
{
	struct proc *p = proc_current();
	struct pt_file *f = fd_file(p, oldfd);

	if (!f || newfd < 0 || newfd >= PT_MAX_FDS)
		return -EBADF;
	if (oldfd != newfd) {
		struct pt_file *old = p->fd[newfd];
		p->fd[newfd] = file_get(f);
		file_put(old);
	}
	return newfd;
}

int pt_pipe(int fds[2])
{
	struct proc *p = proc_current();
	struct pt_file *rd, *wr;

	if (!p)
		return -EPERM;
	int err = pipe_create(&rd, &wr);
	if (err)
		return err;
	fds[0] = fd_install(p, rd);
	fds[1] = fds[0] >= 0 ? fd_install(p, wr) : -EMFILE;
	if (fds[1] >= 0)
		return 0;
	if (fds[0] >= 0)
		p->fd[fds[0]] = NULL;
	file_put(rd);
	file_put(wr);
	return -EMFILE;
}

/* ------------------------------------------------------------ namespace */

static int abspath(const char *path, char *out)
{
	struct proc *p = proc_current();

	if (!path || !*path)
		return -ENOENT;
	return path_normalize(p ? p->cwd : "/", path, out, PT_PATH_MAX);
}

int pt_abspath(const char *path, char *out, size_t size)
{
	struct proc *p = proc_current();

	return path_normalize(p ? p->cwd : "/", path, out, size);
}

static const struct pt_kernfs *const kernfs[] = { &devfs, &procfs };

#define N_KERNFS (sizeof(kernfs) / sizeof(kernfs[0]))

/* The kernel filesystem holding `abs`; *entry is the name inside it, "" for
 * the directory itself. */
static const struct pt_kernfs *kernfs_of(const char *abs, const char **entry)
{
	for (size_t i = 0; i < N_KERNFS; i++) {
		size_t n = strlen(kernfs[i]->name);
		if (abs[0] == '/' && !strncmp(abs + 1, kernfs[i]->name, n) &&
		    (abs[n + 1] == '\0' || abs[n + 1] == '/')) {
			*entry = abs[n + 1] ? abs + n + 2 : "";
			return kernfs[i];
		}
	}
	return NULL;
}

#define VFS_PATH_MAX (PT_PATH_MAX + 16)

static int to_vfs(const char *abs, char *vfs)
{
	return mount_resolve(abs, vfs, VFS_PATH_MAX) ? 0 : -ENOENT;
}

/* Paths the user cannot create, remove or rename. */
static int protected(const char *abs)
{
	const char *entry;

	if (kernfs_of(abs, &entry))
		return -EROFS;
	return !strcmp(abs, "/") || mount_is_point(abs) ? -EBUSY : 0;
}

int pt_open(const char *path, int flags)
{
	proc_check_signals();
	struct proc *p = proc_current();
	char abs[PT_PATH_MAX];
	int err = abspath(path, abs);
	struct pt_file *f;

	if (!p)
		return -EPERM;
	if (err)
		return err;
	const char *entry;
	const struct pt_kernfs *fs = kernfs_of(abs, &entry);

	if (fs) {
		if (!*entry)
			return -EISDIR;
		if (!fs->lookup(entry))
			return (flags & O_CREAT) && !strchr(entry, '/') ? -EROFS : -ENOENT;
		if ((flags & O_ACCMODE) != O_RDONLY && !fs->writable)
			return -EROFS;
		f = fs->open(entry, &err);
	} else if (mount_is_point(abs)) {
		return -EISDIR;
	} else {
		char vfs[VFS_PATH_MAX];
		const struct pt_mount *m = mount_resolve(abs, vfs, sizeof(vfs));
		if (!m)
			return -ENOENT;
		f = vfs_file_open(m, vfs, flags, &err);
	}
	if (!f)
		return err;
	int fd = fd_install(p, f);
	if (fd < 0)
		file_put(f);
	return fd;
}

int pt_stat(const char *path, struct pt_stat *st)
{
	proc_check_signals();
	char abs[PT_PATH_MAX];
	int err = abspath(path, abs);

	if (err)
		return err;
	memset(st, 0, sizeof(*st));
	const char *entry;
	const struct pt_kernfs *fs = kernfs_of(abs, &entry);
	if (fs) {
		st->is_dir = !*entry;
		return !*entry || fs->lookup(entry) ? 0 : -ENOENT;
	}
	/* FAT cannot stat its own root directory */
	if (!strcmp(abs, "/") || mount_is_point(abs)) {
		st->is_dir = true;
		return 0;
	}

	char vfs[VFS_PATH_MAX];
	struct stat s;
	if ((err = to_vfs(abs, vfs)))
		return err;
	if (stat(vfs, &s))
		return -errno;
	st->size = s.st_size;
	st->mtime = s.st_mtime;
	st->is_dir = S_ISDIR(s.st_mode);
	return 0;
}

static int path_op(const char *path, int (*op)(const char *))
{
	proc_check_signals();
	char abs[PT_PATH_MAX];
	char vfs[VFS_PATH_MAX];
	int err = abspath(path, abs);

	if (!err)
		err = protected(abs);
	if (!err)
		err = to_vfs(abs, vfs);
	if (err)
		return err;
	return op(vfs) ? -errno : 0;
}

static int mkdir_default(const char *path)
{
	return mkdir(path, 0777);
}

int pt_mkdir(const char *path)
{
	return path_op(path, mkdir_default);
}

int pt_rmdir(const char *path)
{
	return path_op(path, rmdir);
}

int pt_unlink(const char *path)
{
	return path_op(path, unlink);
}

int pt_rename(const char *from, const char *to)
{
	proc_check_signals();
	char a[PT_PATH_MAX], b[PT_PATH_MAX], va[VFS_PATH_MAX], vb[VFS_PATH_MAX];
	int err = abspath(from, a);

	if (!err)
		err = abspath(to, b);
	if (!err)
		err = protected(a);
	if (!err)
		err = protected(b);
	if (err)
		return err;
	const struct pt_mount *ma = mount_resolve(a, va, sizeof(va));
	const struct pt_mount *mb = mount_resolve(b, vb, sizeof(vb));
	if (!ma || !mb)
		return -ENOENT;
	if (ma != mb)
		return -EXDEV;
	return rename(va, vb) ? -errno : 0;
}

/* ------------------------------------------------------------ directories */

enum dir_kind {
	DIR_ROOT,
	DIR_KERNFS,
	DIR_VFS,
};

struct pt_dir {
	enum dir_kind		 kind;
	const struct pt_kernfs	*fs;
	int			 index;
	DIR		*vfs;
	char		 path[VFS_PATH_MAX];	/* the VFS path, for stat() */
};

int pt_opendir(const char *path, pt_dir_t **out)
{
	proc_check_signals();
	char abs[PT_PATH_MAX];
	int err = abspath(path, abs);

	if (err)
		return err;
	struct pt_dir *d = calloc(1, sizeof(*d));
	if (!d)
		return -ENOMEM;

	const char *entry;
	const struct pt_kernfs *fs = kernfs_of(abs, &entry);

	if (fs && !*entry) {
		d->kind = DIR_KERNFS;
		d->fs = fs;
	} else if (fs) {
		free(d);
		return fs->lookup(entry) ? -ENOTDIR : -ENOENT;
	} else if (to_vfs(abs, d->path)) {
		if (strcmp(abs, "/")) {
			free(d);
			return -ENOENT;
		}
		d->kind = DIR_ROOT;	/* no root filesystem: just /dev and /proc */
	} else {
		d->kind = DIR_VFS;
		d->vfs = opendir(d->path);
		if (!d->vfs) {
			err = errno ? -errno : -ENOENT;
			free(d);
			return err;
		}
	}
	*out = d;
	return 0;
}

int pt_readdir(pt_dir_t *d, struct pt_dirent *ent)
{
	proc_check_signals();
	switch (d->kind) {
	case DIR_ROOT:
		if (d->index < (int)N_KERNFS) {
			strlcpy(ent->name, kernfs[d->index++]->name, sizeof(ent->name));
			ent->is_dir = true;
			return 1;
		}
		return 0;
	case DIR_KERNFS:
		return d->fs->readdir(d->index++, ent);
	case DIR_VFS: {
		struct dirent *e = readdir(d->vfs);
		if (!e)
			return 0;
		strlcpy(ent->name, e->d_name, sizeof(ent->name));
		if (e->d_type == DT_UNKNOWN) {
			char full[VFS_PATH_MAX + PT_NAME_MAX];
			struct stat s;
			snprintf(full, sizeof(full), "%s/%s", d->path, e->d_name);
			ent->is_dir = !stat(full, &s) && S_ISDIR(s.st_mode);
		} else {
			ent->is_dir = e->d_type == DT_DIR;
		}
		return 1;
	}
	}
	return -EINVAL;
}

void pt_closedir(pt_dir_t *d)
{
	if (!d)
		return;
	if (d->vfs)
		closedir(d->vfs);
	free(d);
}

int pt_chdir(const char *path)
{
	struct proc *p = proc_current();
	struct pt_stat st;
	char abs[PT_PATH_MAX];
	int err = abspath(path, abs);

	if (!p)
		return -EPERM;
	if (!err)
		err = pt_stat(abs, &st);
	if (err)
		return err;
	if (!st.is_dir)
		return -ENOTDIR;
	strlcpy(p->cwd, abs, sizeof(p->cwd));
	return 0;
}

const char *pt_getcwd(void)
{
	struct proc *p = proc_current();

	return p ? p->cwd : "/";
}

/* ------------------------------------------------------------ I/O helpers */

static int write_all(int fd, const char *s, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = pt_write(fd, s + done, n - done);
		if (r <= 0)
			return r ? r : -EIO;
		done += r;
	}
	return done;
}

int pt_vdprintf(int fd, const char *fmt, va_list ap)
{
	char small[256];
	va_list copy;

	va_copy(copy, ap);
	int n = vsnprintf(small, sizeof(small), fmt, ap);
	if (n < 0) {
		va_end(copy);
		return -EINVAL;
	}
	if (n < (int)sizeof(small)) {
		va_end(copy);
		return write_all(fd, small, n);
	}
	char *big = malloc(n + 1);
	if (!big) {
		va_end(copy);
		return -ENOMEM;
	}
	vsnprintf(big, n + 1, fmt, copy);
	va_end(copy);
	int r = write_all(fd, big, n);
	free(big);
	return r;
}

int pt_dprintf(int fd, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	int r = pt_vdprintf(fd, fmt, ap);
	va_end(ap);
	return r;
}

int pt_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	int r = pt_vdprintf(PT_STDOUT, fmt, ap);
	va_end(ap);
	return r;
}

int pt_puts(const char *s)
{
	return write_all(PT_STDOUT, s, strlen(s));
}

int pt_getc(int fd)
{
	unsigned char c;
	ssize_t r = pt_read(fd, &c, 1);

	return r == 1 ? c : r == 0 ? -1 : (int)r;
}

const char *pt_strerror(int err)
{
	return strerror(err < 0 ? -err : err);
}

int pt_sleep_ms(uint32_t ms)
{
	proc_check_signals();
	while (ms) {
		uint32_t step = ms < 50 ? ms : 50;
		vTaskDelay(pdMS_TO_TICKS(step));
		ms -= step;
		if (pt_interrupted()) {
			proc_check_signals();	/* exits unless the process catches */
			return -EINTR;
		}
	}
	return 0;
}

int64_t pt_uptime_us(void)
{
	return esp_timer_get_time();
}

/* ------------------------------------------------------------ table */

static void sys_exit(int status)
{
	pt_exit(status);
}

const struct pt_syscalls pt_sys = {
	.abi = PT_ABI_VERSION,
	.read = pt_read,
	.write = pt_write,
	.open = pt_open,
	.close = pt_close,
	.lseek = pt_lseek,
	.dup2 = pt_dup2,
	.pipe = pt_pipe,
	.ioctl = pt_ioctl,
	.stat = pt_stat,
	.mkdir = pt_mkdir,
	.rmdir = pt_rmdir,
	.unlink = pt_unlink,
	.rename = pt_rename,
	.opendir = pt_opendir,
	.readdir = pt_readdir,
	.closedir = pt_closedir,
	.chdir = pt_chdir,
	.getcwd = pt_getcwd,
	.spawn = pt_spawn,
	.wait = pt_wait,
	.exit = sys_exit,
	.kill = pt_kill,
	.sigcatch = pt_sigcatch,
	.interrupted = pt_interrupted,
	.getpid = pt_getpid,
	.sleep_ms = pt_sleep_ms,
	.uptime_us = pt_uptime_us,
	.getenv = pt_getenv,
	.setenv = pt_setenv,
	.malloc = pt_malloc,
	.realloc = pt_realloc,
	.free = pt_free,
};
