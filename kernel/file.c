/*
 * Open file objects. A process's fd table points at these; pipes, the
 * terminal, /proc entries and real files all implement the same ops.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "pt/kernel.h"

struct pt_file *file_alloc(const struct pt_file_ops *ops, void *priv)
{
	struct pt_file *f = calloc(1, sizeof(*f));

	if (!f)
		return NULL;
	f->ops = ops;
	f->priv = priv;
	atomic_init(&f->refs, 1);
	return f;
}

struct pt_file *file_get(struct pt_file *f)
{
	if (f)
		atomic_fetch_add(&f->refs, 1);
	return f;
}

void file_put(struct pt_file *f)
{
	if (!f || atomic_fetch_sub(&f->refs, 1) != 1)
		return;
	if (f->ops->release)
		f->ops->release(f);
	free(f);
}

/* ------------------------------------------------------------ VFS files */

/*
 * Files on a real filesystem are buffered, the way stdio buffers: a buffer
 * collects small writes until a whole flash page is ready, and reads come
 * from a block read once. Flash erases before it writes, so a program that
 * writes a line at a time is otherwise paying an erase per line.
 *
 * The buffer holds either unwritten data (dirty) or data read ahead, never
 * both. It is written out on close, on seek and by `sync`; `sync` also asks
 * the filesystem to commit its own caches, so what it has written survives a
 * power cut. A crash or a battery pull loses whatever is still buffered, so
 * anything that must survive should be closed: the editor and the shell's
 * history already do.
 */
struct vfs_file {
	struct vfs_file		*next;		/* every open file, for sync */
	const struct pt_mount	*mount;		/* for umount's busy check */
	int			 fd;
	uint8_t			*buf;
	size_t			 cap;
	size_t			 len;		/* bytes of buf that are valid */
	size_t			 pos;		/* read position inside buf */
	bool			 dirty;		/* buf holds data not yet written */
	SemaphoreHandle_t	 lock;
};

static struct vfs_file	*open_files;
static SemaphoreHandle_t open_lock;

#define LOCK(v)		xSemaphoreTake((v)->lock, portMAX_DELAY)
#define UNLOCK(v)	xSemaphoreGive((v)->lock)

void file_init(void)
{
	open_lock = xSemaphoreCreateMutex();
}

static struct vfs_file *vfs_of(struct pt_file *f)
{
	return f->priv;
}

/*
 * The buffer appears on the first operation small enough to benefit, and it
 * has to be internal RAM: a flash write turns the cache off, so writing from
 * PSRAM makes the flash driver copy the data through a small staging buffer
 * in pieces, which measured three times slower than no buffer at all.
 */
static bool want_buffer(struct vfs_file *v)
{
	if (v->buf || !CONFIG_PT_FILE_BUF_KB)
		return v->buf != NULL;
	v->cap = (size_t)CONFIG_PT_FILE_BUF_KB * 1024;
	v->buf = heap_caps_malloc(v->cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if (!v->buf)
		v->cap = 0;
	return v->buf != NULL;
}

static int write_out(struct vfs_file *v)
{
	size_t done = 0;

	while (done < v->len) {
		ssize_t r = write(v->fd, v->buf + done, v->len - done);
		if (r <= 0) {
			/* keep what is unwritten: the caller can retry */
			memmove(v->buf, v->buf + done, v->len - done);
			v->len -= done;
			return r < 0 ? -errno : -EIO;
		}
		done += r;
	}
	v->len = v->pos = 0;
	v->dirty = false;
	return 0;
}

/* Empties the buffer: writes out dirty data, or rewinds read-ahead. */
static int drop_buffer(struct vfs_file *v)
{
	if (v->dirty)
		return write_out(v);
	if (v->pos < v->len && lseek(v->fd, (off_t)v->pos - (off_t)v->len, SEEK_CUR) < 0)
		return -errno;
	v->len = v->pos = 0;
	return 0;
}

static ssize_t vfs_read(struct pt_file *f, void *buf, size_t n)
{
	struct vfs_file *v = vfs_of(f);
	ssize_t ret;

	LOCK(v);
	if (v->dirty && (ret = write_out(v)) < 0)
		goto out;
	if (v->pos == v->len && (!want_buffer(v) || n >= v->cap)) {
		ret = read(v->fd, buf, n);		/* large read: no copy */
		ret = ret < 0 ? -errno : ret;
		goto out;
	}
	if (v->pos == v->len) {
		ssize_t got = read(v->fd, v->buf, v->cap);
		if (got < 0) {
			ret = -errno;
			goto out;
		}
		v->len = got;
		v->pos = 0;
	}
	ret = v->len - v->pos < n ? (ssize_t)(v->len - v->pos) : (ssize_t)n;
	memcpy(buf, v->buf + v->pos, ret);
	v->pos += ret;
out:
	UNLOCK(v);
	return ret;
}

static ssize_t vfs_write(struct pt_file *f, const void *buf, size_t n)
{
	struct vfs_file *v = vfs_of(f);
	ssize_t ret;

	LOCK(v);
	if (!v->dirty && v->len && (ret = drop_buffer(v)) < 0)
		goto out;
	if (!want_buffer(v) || n >= v->cap) {
		if (v->dirty && (ret = write_out(v)) < 0)
			goto out;			/* keep the order of the file */
		ret = write(v->fd, buf, n);		/* big enough to go straight out */
		ret = ret < 0 ? -errno : ret;
		goto out;
	}
	if (v->len + n > v->cap && (ret = write_out(v)) < 0)
		goto out;
	memcpy(v->buf + v->len, buf, n);
	v->len += n;
	v->dirty = true;
	ret = n;
out:
	UNLOCK(v);
	return ret;
}

static off_t vfs_lseek(struct pt_file *f, off_t off, int whence)
{
	struct vfs_file *v = vfs_of(f);
	off_t r;

	LOCK(v);
	r = drop_buffer(v);
	if (r == 0) {
		r = lseek(v->fd, off, whence);
		r = r < 0 ? -errno : r;
	}
	UNLOCK(v);
	return r;
}

static int vfs_flush(struct pt_file *f)
{
	struct vfs_file *v = vfs_of(f);
	int err = 0;

	LOCK(v);
	if (v->dirty)
		err = write_out(v);
	UNLOCK(v);
	return err;
}

static void vfs_release(struct pt_file *f)
{
	struct vfs_file *v = vfs_of(f);

	xSemaphoreTake(open_lock, portMAX_DELAY);
	for (struct vfs_file **pp = &open_files; *pp; pp = &(*pp)->next) {
		if (*pp == v) {
			*pp = v->next;
			break;
		}
	}
	xSemaphoreGive(open_lock);

	/* Off the list first: nothing can reach the fd once it is closed,
	 * which matters because its number is soon given to another file. */
	drop_buffer(v);
	close(v->fd);
	vSemaphoreDelete(v->lock);
	heap_caps_free(v->buf);
	free(v);
}

static const struct pt_file_ops vfs_ops = {
	.read = vfs_read,
	.write = vfs_write,
	.lseek = vfs_lseek,
	.flush = vfs_flush,
	.release = vfs_release,
};

struct pt_file *vfs_file_open(const struct pt_mount *m, const char *vfspath, int flags, int *err)
{
	int fd = open(vfspath, flags, 0666);

	if (fd < 0) {
		/* a name FAT could never hold is not there (see sys.c) */
		*err = errno == EINVAL && !(flags & O_CREAT) ? -ENOENT : -errno;
		return NULL;
	}
	struct vfs_file *v = calloc(1, sizeof(*v));
	struct pt_file *f = v ? file_alloc(&vfs_ops, v) : NULL;

	if (f)
		v->lock = xSemaphoreCreateMutex();
	if (!f || !v->lock) {
		free(f);
		free(v);
		close(fd);
		*err = -ENOMEM;
		return NULL;
	}
	v->fd = fd;
	v->mount = m;
	xSemaphoreTake(open_lock, portMAX_DELAY);
	v->next = open_files;
	open_files = v;
	xSemaphoreGive(open_lock);
	return f;
}

int vfs_sync_all(void)
{
	int err = 0;

	xSemaphoreTake(open_lock, portMAX_DELAY);
	for (struct vfs_file *v = open_files; v; v = v->next) {
		LOCK(v);
		int e = v->dirty ? write_out(v) : 0;
		if (!e && fsync(v->fd) < 0 && errno != EINVAL && errno != ENOSYS)
			e = -errno;
		if (e && !err)
			err = e;
		UNLOCK(v);
	}
	xSemaphoreGive(open_lock);
	return err;
}

/*
 * Whether any file is open on m's filesystem -- through m, or through a
 * bind of it elsewhere (the card is /mnt/sd and the home directory both):
 * the same VFS underneath is the same filesystem.
 */
bool vfs_mount_busy(const struct pt_mount *m)
{
	bool busy = false;

	xSemaphoreTake(open_lock, portMAX_DELAY);
	for (struct vfs_file *v = open_files; v && !busy; v = v->next)
		busy = v->mount == m || (v->mount && !strcmp(v->mount->vfs, m->vfs));
	xSemaphoreGive(open_lock);
	return busy;
}

/* ------------------------------------------------------------ memory files */

struct mem_file {
	char	*data;
	size_t	 len;
	size_t	 pos;
};

static ssize_t mem_read(struct pt_file *f, void *buf, size_t n)
{
	struct mem_file *m = f->priv;
	size_t left = m->len - m->pos;

	if (n > left)
		n = left;
	memcpy(buf, m->data + m->pos, n);
	m->pos += n;
	return n;
}

static off_t mem_lseek(struct pt_file *f, off_t off, int whence)
{
	struct mem_file *m = f->priv;
	off_t base = whence == SEEK_END ? (off_t)m->len : whence == SEEK_CUR ? (off_t)m->pos : 0;

	if (base + off < 0 || base + off > (off_t)m->len)
		return -EINVAL;
	m->pos = base + off;
	return m->pos;
}

static void mem_release(struct pt_file *f)
{
	struct mem_file *m = f->priv;

	free(m->data);
	free(m);
}

static const struct pt_file_ops mem_ops = {
	.read = mem_read,
	.lseek = mem_lseek,
	.release = mem_release,
};

struct pt_file *mem_file_open(char *data, size_t len)
{
	struct mem_file *m = calloc(1, sizeof(*m));
	struct pt_file *f = m ? file_alloc(&mem_ops, m) : NULL;

	if (!f) {
		free(m);
		free(data);
		return NULL;
	}
	m->data = data;
	m->len = len;
	return f;
}
