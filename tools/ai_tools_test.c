/* Real host processes exercise silent commands and cancellation without an API key. */
#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "../bin/ai_jobs.h"
#include "../bin/ai_models.h"
#include "../bin/ai_path.h"
#include "../drivers/video/scanout.h"

void *pt_malloc(size_t n) { return malloc(n); }
void *pt_realloc(void *p, size_t n) { return realloc(p, n); }
void pt_free(void *p) { free(p); }
ssize_t pt_read(int fd, void *buf, size_t n) { ssize_t r = read(fd, buf, n); return r < 0 ? -errno : r; }
int pt_close(int fd) { return close(fd) ? -errno : 0; }
int pt_pipe(int fds[2]) { return pipe2(fds, O_CLOEXEC) ? -errno : 0; }
int pt_open(const char *p, int flags) { int fd = open(p, flags, 0600); return fd < 0 ? -errno : fd; }
int pt_chdir(const char *p) { return chdir(p) ? -errno : 0; }
const char *pt_getcwd(void) { static char cwd[PT_PATH_MAX]; assert(getcwd(cwd, sizeof(cwd))); return cwd; }
int pt_ioctl(int fd, int req, void *arg)
{
	assert(req == PT_PIPE_SETTIMEOUT && *(int *)arg == 0);
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0 ? -errno : 0;
}
int64_t pt_uptime_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
int pt_sleep_ms(uint32_t ms) { usleep(ms * 1000); return 0; }
int pt_spawn(const struct pt_spawn *req)
{
	pid_t pid = fork();

	if (pid < 0)
		return -errno;
	if (!pid) {
		setpgid(0, 0);
		for (int i = 0; i < 3; i++)
			assert(dup2(req->fd[i], i) >= 0);
		execvp(req->cmd, req->argv);
		_exit(127);
	}
	setpgid(pid, pid);
	return pid;
}
int pt_wait(int pid, int *status, int flags)
{
	int s, got = waitpid(pid, &s, flags & PT_WNOHANG ? WNOHANG : 0);

	if (got > 0)
		*status = WIFEXITED(s) ? WEXITSTATUS(s) : 128 + WTERMSIG(s);
	return got < 0 ? -errno : got;
}
static void output(void *ctx, const char *s, size_t n) { (void)ctx; (void)s; (void)n; }
static bool cancel(void *ctx) { return ctx && *(bool *)ctx; }
static void stop(void *ctx, int pgid, int sig) { (void)ctx; kill(-pgid, sig); }
static bool alive(void *ctx, int pgid)
{
	(void)ctx;
	/* The test adopts shell orphans, as the board's kernel does. */
	while (waitpid(-pgid, NULL, WNOHANG) > 0)
		;
	return kill(-pgid, 0) == 0;
}

static void jobs(void)
{
	struct ai_job job = { 0 };
	bool interrupted = false;
	struct ai_job_io io = { &interrupted, output, cancel, stop, alive };
	struct jbuf out = { 0 };
	char cwd[PT_PATH_MAX];
	int64_t start;

	strcpy(cwd, pt_getcwd());
	assert(!prctl(PR_SET_CHILD_SUBREAPER, 1));
	ai_job_close(&job, &io);
	assert(fcntl(0, F_GETFD) >= 0);
	assert(ai_job_start(&job, "/tmp", "printf hello; exit 7") > 0);
	assert(!strcmp(cwd, pt_getcwd()));
	ai_job_collect(&job, 1000, &io, &out);
	assert(ai_job_done(&job) && job.status == 7);
	assert(strstr(out.p, "hello") && strstr(out.p, "exit status 7"));
	jb_free(&out);
	assert(ai_job_start(&job, "/tmp", "sleep 30 | cat") > 0);
	start = pt_uptime_us();
	ai_job_collect(&job, 50, &io, &out);
	assert(!ai_job_done(&job) && pt_uptime_us() - start < 500000);
	assert(strstr(out.p, "still running"));
	jb_free(&out);
	ai_job_signal(&job, &io);
	ai_job_collect(&job, 2000, &io, &out);
	assert(ai_job_done(&job) && job.stopped);
	jb_free(&out);
	assert(ai_job_start(&job, "/tmp", "trap '' TERM; while :; do sleep 1; done") > 0);
	ai_job_collect(&job, 50, &io, &out);
	jb_free(&out);
	ai_job_signal(&job, &io);
	ai_job_collect(&job, 2000, &io, &out);
	assert(ai_job_done(&job) && job.status == 128 + SIGKILL);
	jb_free(&out);
	assert(ai_job_start(&job, "/tmp", "yes") > 0);
	interrupted = true;
	ai_job_collect(&job, 1000, &io, &out);
	assert(ai_job_done(&job) && job.stopped && out.len < 6200);
	jb_free(&out);
	ai_job_close(&job, &io);
	interrupted = false;
	assert(ai_job_start(&job, "/tmp", "sleep 30 >/dev/null 2>&1 &") > 0);
	ai_job_collect(&job, 100, &io, &out);
	assert(job.exited && job.fd < 0 && !ai_job_done(&job));
	jb_free(&out);
	ai_job_signal(&job, &io);
	ai_job_collect(&job, 2000, &io, &out);
	assert(ai_job_done(&job) && job.stopped);
	jb_free(&out);
	ai_job_close(&job, &io);
	assert(ai_job_start(&job, "/no/such/workspace", "echo bad") == -ENOENT);
	assert(!strcmp(cwd, pt_getcwd()));
}

static void models(void)
{
	const char *json = "{\"data\":["
		"{\"id\":\"deepseek/deepseek-v4.1-flash\",\"description\":\"quoted \\\"}\\\"\","
		"\"architecture\":{\"output_modalities\":[\"text\"]},"
		"\"pricing\":{\"prompt\":\"0.0000003\",\"completion\":\"0.000001\"},"
		"\"reasoning\":{\"supported_efforts\":[\"high\",\"low\"],\"mandatory\":true}},"
		"{\"id\":\"google/gemma-4:free\",\"pricing\":{\"prompt\":\"0\",\"completion\":\"0\"}},"
		"{\"id\":\"audio/voice\",\"architecture\":{\"output_modalities\":[\"audio\"]}},"
		"{\"id\":\"other/reasoner\",\"reasoning\":{\"supported_efforts\":null}}]}";
	int found[8];

	for (size_t chunk = 1; chunk < 31; chunk++) {
		struct ai_catalog c = { 0 };

		for (size_t i = 0; i < strlen(json); i += chunk) {
			size_t n = strlen(json) - i;

			assert(ai_catalog_feed(&c, json + i, n < chunk ? n : chunk));
		}
		assert(c.count == 3 && c.depth == 0);
		assert(strstr(c.models[0].label, "$0.3/1"));
		assert(c.models[0].efforts == ((1U << 0) | (1U << 3) | (1U << 5)));
		assert(c.models[1].efforts == 1 && c.models[2].efforts == 255);
		assert(ai_model_match(&c, "deepseek-4.1-flash", found, 8) == 1 && found[0] == 0);
		assert(ai_model_match(&c, "gemma4", found, 8) == 1 && found[0] == 1);
		assert(!ai_model_match(&c, "made-up", found, 8));
		ai_catalog_free(&c);
	}
}

static void paths(void)
{
	char p[PT_PATH_MAX];
	const char *root = "/home/andre/pico/project", *notes = "/home/andre/notes";

	assert(ai_path_resolve(root, notes, "/home/andre", "src/../main.pico", p, sizeof(p)));
	assert(!strcmp(p, "/home/andre/pico/project/main.pico"));
	assert(ai_path_resolve(root, notes, "/home/andre", "~/notes/history/topic.md", p, sizeof(p)));
	assert(ai_path_resolve(root, notes, "/home/andre", "../../notes/new.md", p, sizeof(p)));
	assert(!ai_path_resolve(root, notes, "/home/andre", "../../../.config/openrouter", p, sizeof(p)));
	assert(!ai_path_resolve(root, notes, "/home/andre", "~/notes/../../etc/passwd", p, sizeof(p)));
	assert(!ai_path_resolve(root, notes, "/home/andre", "/home/andre/pico/project-evil/file", p, sizeof(p)));
	assert(ai_path_resolve("/", notes, "/home/andre", "/tmp/a", p, sizeof(p)));
	assert(!ai_path_resolve(root, notes, "/home/andre", "file", p, 3));
	assert(ai_path_is_note("/home/andre/NOTES/topic.md", notes, "/home/andre", true));
	assert(ai_path_is_note("/mnt/sd/Notes/topic.md", notes, "/home/andre", true));
	assert(!ai_path_is_note("/mnt/sd/notes-extra/topic.md", notes, "/home/andre", true));
	assert(!ai_path_is_note("/mnt/sd/notes/topic.md", notes, "/home/andre", false));
	assert(!ai_path_is_note("/home/andre/notes-extra/topic.md", notes, "/home/andre", true));
	assert(!ai_path_is_note("/mnt/sd/notes/topic.md", "/tmp/test/notes", "/tmp/test", true));
}

static void bands(void)
{
	struct scanout_band b[SCANOUT_MAX_BANDS];

	for (int w = 1; w <= 240; w++)
		for (int h = 1; h <= 320; h++)
			for (int reverse = 0; reverse <= 1; reverse++) {
				int nb = scanout_bands(w, 7, h, reverse, b), done = 0;

				assert(nb > 0 && nb <= SCANOUT_MAX_BANDS);
				for (int k = 0; k < nb; k++) {
					/* where its pixels begin in the picture's buffer */
					int offset = reverse ? h - done - b[k].n : done;

					assert(b[k].first == 7 + done && b[k].n > 0 && b[k].n <= 64);
					assert((offset * w * 2) % 64 == 0);
					done += b[k].n;
				}
				assert(done == h);
				if (h >= 64 && w % 32 == 0)
					assert(b[0].n <= SCANOUT_EDGE_ROWS + 1 &&
					       b[nb - 1].n <= SCANOUT_EDGE_ROWS + 1);
			}
}

int main(void)
{
	paths(); models(); bands(); jobs();
	puts("ai tools: workspace boundaries, streamed models, effort, silent jobs, group stop, DMA bands passed");
	return 0;
}
