/*
 * Pipes: a stream buffer shared by a read end and a write end. Reads return
 * 0 once every writer is gone; writes fail with -EPIPE once every reader is.
 */
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#include "pt/kernel.h"

#define PIPE_SIZE	2048
#define PIPE_POLL	pdMS_TO_TICKS(proc_poll_ms(50))

struct pipe {
	StreamBufferHandle_t	sb;
	SemaphoreHandle_t	rlock, wlock;
	atomic_int		readers, writers;
	atomic_int		ends;		/* open ends: the last one frees */
	atomic_int		timeout_ms;	/* the reader can poll without blocking */
};

/* One counter decides who frees: with separate reader and writer counts, two
 * ends closing at once on the two cores could both see zero and free twice. */
static void pipe_end_closed(struct pipe *p)
{
	if (atomic_fetch_sub(&p->ends, 1) != 1)
		return;
	vStreamBufferDelete(p->sb);
	vSemaphoreDelete(p->rlock);
	vSemaphoreDelete(p->wlock);
	free(p);
}

static ssize_t pipe_read(struct pt_file *f, void *buf, size_t n)
{
	struct pipe *p = f->priv;
	ssize_t ret;
	int timeout = atomic_load(&p->timeout_ms);
	int64_t deadline = pt_uptime_us() + (int64_t)timeout * 1000;

	if (!n)
		return 0;

	xSemaphoreTake(p->rlock, portMAX_DELAY);
	for (;;) {
		int wait = proc_poll_ms(50);
		int64_t left = deadline - pt_uptime_us();
		size_t got;

		if (timeout >= 0 && left < (int64_t)wait * 1000)
			wait = left > 0 ? (int)((left + 999) / 1000) : 0;
		got = xStreamBufferReceive(p->sb, buf, n, pdMS_TO_TICKS(wait));
		if (got) {
			ret = got;
			break;
		}
		if (!atomic_load(&p->writers)) {
			ret = xStreamBufferReceive(p->sb, buf, n, 0);
			break;
		}
		if (pt_interrupted()) {
			ret = -EINTR;
			break;
		}
		if (timeout >= 0 && pt_uptime_us() >= deadline) {
			ret = -EAGAIN;
			break;
		}
		if (proc_stop_pending()) {	/* stopped, but not holding the pipe */
			xSemaphoreGive(p->rlock);
			proc_stop_point();
			xSemaphoreTake(p->rlock, portMAX_DELAY);
		}
	}
	xSemaphoreGive(p->rlock);
	return ret;
}

static int pipe_ioctl(struct pt_file *f, int req, void *arg)
{
	struct pipe *p = f->priv;

	if (req != PT_PIPE_SETTIMEOUT)
		return -ENOTTY;
	if (!arg || *(int *)arg < -1)
		return -EINVAL;
	atomic_store(&p->timeout_ms, *(int *)arg);
	return 0;
}

static ssize_t pipe_write(struct pt_file *f, const void *buf, size_t n)
{
	struct pipe *p = f->priv;
	size_t done = 0;
	ssize_t ret = 0;

	xSemaphoreTake(p->wlock, portMAX_DELAY);
	while (done < n) {
		if (!atomic_load(&p->readers)) {
			ret = -EPIPE;
			break;
		}
		if (pt_interrupted()) {
			ret = -EINTR;
			break;
		}
		if (proc_stop_pending()) {
			xSemaphoreGive(p->wlock);
			proc_stop_point();
			xSemaphoreTake(p->wlock, portMAX_DELAY);
		}
		done += xStreamBufferSend(p->sb, (const char *)buf + done, n - done, PIPE_POLL);
	}
	xSemaphoreGive(p->wlock);
	return done ? (ssize_t)done : ret;
}

static void pipe_release_read(struct pt_file *f)
{
	struct pipe *p = f->priv;

	atomic_fetch_sub(&p->readers, 1);
	pipe_end_closed(p);
}

static void pipe_release_write(struct pt_file *f)
{
	struct pipe *p = f->priv;

	atomic_fetch_sub(&p->writers, 1);
	pipe_end_closed(p);
}

static const struct pt_file_ops pipe_read_ops = {
	.read = pipe_read,
	.ioctl = pipe_ioctl,
	.release = pipe_release_read,
};

static const struct pt_file_ops pipe_write_ops = {
	.write = pipe_write,
	.release = pipe_release_write,
};

int pipe_create(struct pt_file **rd, struct pt_file **wr)
{
	struct pipe *p = calloc(1, sizeof(*p));

	if (!p)
		return -ENOMEM;
	p->sb = xStreamBufferCreate(PIPE_SIZE, 1);
	p->rlock = xSemaphoreCreateMutex();
	p->wlock = xSemaphoreCreateMutex();
	atomic_init(&p->readers, 1);
	atomic_init(&p->writers, 1);
	atomic_init(&p->ends, 2);
	atomic_init(&p->timeout_ms, -1);
	*rd = p->sb && p->rlock && p->wlock ? file_alloc(&pipe_read_ops, p) : NULL;
	*wr = *rd ? file_alloc(&pipe_write_ops, p) : NULL;
	if (*wr)
		return 0;

	if (*rd)
		free(*rd);
	if (p->sb)
		vStreamBufferDelete(p->sb);
	if (p->rlock)
		vSemaphoreDelete(p->rlock);
	if (p->wlock)
		vSemaphoreDelete(p->wlock);
	free(p);
	return -ENOMEM;
}
