/*
 * A silent command must not hold the chat in a pipe read. Poll its output
 * and wait status together, returning a job the model can check or stop.
 */
#include <string.h>

#include "ai_jobs.h"
#include "json.h"

#define OUTPUT_MAX 6000
#define STOP_GRACE_US 500000

bool ai_job_done(const struct ai_job *job)
{
	return !job->pid || (job->exited && job->fd < 0 && !job->group_alive);
}

int ai_job_start(struct ai_job *job, const char *root, const char *command)
{
	char cwd[PT_PATH_MAX];
	char *argv[] = { "sh", "-c", (char *)command, NULL };
	struct pt_spawn req = { .cmd = "sh", .argc = 3, .argv = argv };
	int fds[2], null_fd, err, poll = 0;

	memset(job, 0, sizeof(*job));
	job->fd = -1;
	if ((err = pt_pipe(fds)))
		return err;
	err = pt_ioctl(fds[0], PT_PIPE_SETTIMEOUT, &poll);
	null_fd = pt_open("/dev/null", O_RDONLY);
	if (!err && null_fd < 0)
		err = null_fd;
	strncpy(cwd, pt_getcwd(), sizeof(cwd) - 1);
	cwd[sizeof(cwd) - 1] = '\0';
	if (!err)
		err = pt_chdir(root);
	if (!err) {
		req.fd[0] = null_fd;
		req.fd[1] = req.fd[2] = fds[1];
		err = pt_spawn(&req);
		pt_chdir(cwd);
	}
	pt_close(fds[1]);
	if (null_fd >= 0)
		pt_close(null_fd);
	if (err < 0) {
		pt_close(fds[0]);
		return err;
	}
	job->pid = err;
	job->fd = fds[0];
	job->group_alive = true;
	return job->pid;
}

void ai_job_signal(struct ai_job *job, const struct ai_job_io *io)
{
	if (ai_job_done(job) || job->stopped)
		return;
	io->signal(io->ctx, job->pid, PT_SIGTERM);
	job->stopped = true;
	job->stop_at = pt_uptime_us() + STOP_GRACE_US;
}

void ai_job_collect(struct ai_job *job, int wait_ms, const struct ai_job_io *io, struct jbuf *out)
{
	char buf[256];
	int64_t deadline = pt_uptime_us() + (int64_t)wait_ms * 1000;
	bool cut = false;

	for (;;) {
		/* Bound each drain too: `yes` must still leave time for the keys. */
		for (int i = 0; job->fd >= 0 && i < 16; i++) {
			ssize_t n = pt_read(job->fd, buf, sizeof(buf));

			if (n == -EAGAIN || n == -EINTR)
				break;
			if (n <= 0) {
				pt_close(job->fd);
				job->fd = -1;
				break;
			}
			io->write(io->ctx, buf, n);
			if (out->len < OUTPUT_MAX) {
				size_t keep = OUTPUT_MAX - out->len;

				jb_add(out, buf, (size_t)n < keep ? (size_t)n : keep);
			}
			cut |= out->len >= OUTPUT_MAX;
		}
		if (!job->exited && pt_wait(job->pid, &job->status, PT_WNOHANG) > 0)
			job->exited = true;
		if (job->exited)
			job->group_alive = io->alive(io->ctx, job->pid);
		if (ai_job_done(job))
			break;
		if (!job->stopped && io->cancelled(io->ctx)) {
			ai_job_signal(job, io);
			deadline = pt_uptime_us() + 2000000;
		}
		if (job->stop_at && pt_uptime_us() >= job->stop_at) {
			io->signal(io->ctx, job->pid, PT_SIGKILL);
			job->stop_at = 0;
		}
		if (pt_uptime_us() >= deadline)
			break;
		pt_sleep_ms(10);
	}
	if (cut)
		jb_puts(out, "\n(the rest of the output was cut)");
	if (ai_job_done(job))
		jb_printf(out, "\njob %d: %sexit status %d", job->pid,
			  job->stopped ? "stopped; " : "", job->status);
	else
		jb_printf(out, "\njob %d: %s; use command_status or stop_command with this job id",
			  job->pid, job->stopped ? "stopping" : "still running");
}

void ai_job_close(struct ai_job *job, const struct ai_job_io *io)
{
	struct jbuf out = { 0 };

	if (!job->pid)
		return;
	if (!ai_job_done(job)) {
		io->signal(io->ctx, job->pid, PT_SIGKILL);
		job->stopped = true;
		job->stop_at = 0;
		ai_job_collect(job, 2000, io, &out);
	}
	if (job->fd >= 0)
		pt_close(job->fd);
	job->fd = -1;
	jb_free(&out);
}
