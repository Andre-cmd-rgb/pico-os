/* Commands belong to a chat, including their output pipe and process group. */
#pragma once

#include "pt/sys.h"

#define AI_JOBS_MAX 3

struct ai_job {
	int pid, fd, status;
	bool exited, stopped, group_alive;
	int64_t stop_at;
};

struct ai_job_io {
	void *ctx;
	void (*write)(void *ctx, const char *s, size_t n);
	bool (*cancelled)(void *ctx);
	void (*signal)(void *ctx, int pgid, int sig);
	bool (*alive)(void *ctx, int pgid);
};

struct jbuf;

int ai_job_start(struct ai_job *job, const char *root, const char *command);
bool ai_job_done(const struct ai_job *job);
void ai_job_signal(struct ai_job *job, const struct ai_job_io *io);
void ai_job_collect(struct ai_job *job, int wait_ms, const struct ai_job_io *io, struct jbuf *out);
void ai_job_close(struct ai_job *job, const struct ai_job_io *io);
