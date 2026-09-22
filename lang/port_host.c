/*
 * Port layer for a PC (Linux, macOS): POSIX calls. Builds build-host/ac and
 * build-host/a, which pick their behaviour from the name they run under.
 *
 * AL_STATS=1 runs the command on a painted 1 MB thread stack and reports
 * the peak heap and stack it used, to check what fits on the device.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "al.h"
#include "driver.h"
#include "port.h"

extern char **environ;

static size_t heap_now, heap_peak;
static struct termios saved_termios;
static bool termios_saved;

struct hdr {
	size_t	size;
};

void *port_alloc(size_t n)
{
	struct hdr *h = malloc(sizeof(*h) + n);

	if (!h)
		return NULL;
	h->size = n;
	heap_now += n;
	if (heap_now > heap_peak)
		heap_peak = heap_now;
	return h + 1;
}

void port_free(void *p)
{
	if (!p)
		return;
	struct hdr *h = (struct hdr *)p - 1;
	heap_now -= h->size;
	free(h);
}

void *port_realloc(void *p, size_t n)
{
	if (!p)
		return port_alloc(n);
	struct hdr *h = (struct hdr *)p - 1;
	size_t old = h->size;
	struct hdr *q = realloc(h, sizeof(*q) + n);
	if (!q)
		return NULL;
	q->size = n;
	heap_now = heap_now - old + n;
	if (heap_now > heap_peak)
		heap_peak = heap_now;
	return q + 1;
}

long port_read(int fd, void *buf, size_t n)
{
	ssize_t r;

	do
		r = read(fd, buf, n);
	while (r < 0 && errno == EINTR);
	return r < 0 ? -errno : r;
}

long port_write(int fd, const void *buf, size_t n)
{
	ssize_t r;

	do
		r = write(fd, buf, n);
	while (r < 0 && errno == EINTR);
	return r < 0 ? -errno : r;
}

int port_open(const char *path, int mode)
{
	int flags = mode == PORT_O_READ ? O_RDONLY :
		    mode == PORT_O_WRITE ? O_WRONLY | O_CREAT | O_TRUNC : O_WRONLY | O_CREAT | O_APPEND;
	int fd = open(path, flags | O_CLOEXEC, 0644);

	return fd < 0 ? -errno : fd;
}

int port_close(int fd)
{
	return close(fd) ? -errno : 0;
}

bool port_isatty(int fd)
{
	return isatty(fd);
}

int port_stat(const char *path, bool *is_dir, int64_t *size)
{
	struct stat st;

	if (stat(path, &st))
		return -errno;
	*is_dir = S_ISDIR(st.st_mode);
	*size = st.st_size;
	return 0;
}

int port_mkdir(const char *path)
{
	return mkdir(path, 0755) ? -errno : 0;
}

int port_remove(const char *path)
{
	return remove(path) ? -errno : 0;
}

int port_rename(const char *from, const char *to)
{
	return rename(from, to) ? -errno : 0;
}

int port_listdir(const char *path, int (*fn)(void *ctx, const char *name), void *ctx)
{
	DIR *d = opendir(path);
	struct dirent *e;

	if (!d)
		return -errno;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (fn(ctx, e->d_name))
			break;
	}
	closedir(d);
	return 0;
}

const char *port_strerror(int err)
{
	return strerror(err < 0 ? -err : err);
}

int port_run(int argc, const char **argv)
{
	pid_t pid;
	int status;
	int err = posix_spawnp(&pid, argv[0], NULL, NULL, (char *const *)argv, environ);

	if (err)
		return -err;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -errno;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}

const char *port_getenv(const char *name)
{
	return getenv(name);
}

/* SIGINT's default action already ends the process on a PC. */
bool port_interrupted(void)
{
	return false;
}

void port_die_interrupted(void)
{
}

void port_sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

int64_t port_uptime_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int64_t port_time(void)
{
	return time(NULL);
}

void port_date(int64_t t, char *buf, size_t n)
{
	time_t tt = t;
	struct tm tm;

	localtime_r(&tt, &tm);
	strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

int port_tty_raw(bool on)
{
	if (!isatty(0))
		return -ENOTTY;
	if (on) {
		struct termios t;
		if (tcgetattr(0, &t))
			return -errno;
		if (!termios_saved) {
			saved_termios = t;
			termios_saved = true;
		}
		t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
		t.c_iflag &= ~(IXON | ICRNL);
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
		return tcsetattr(0, TCSAFLUSH, &t) ? -errno : 0;
	}
	if (termios_saved)
		return tcsetattr(0, TCSAFLUSH, &saved_termios) ? -errno : 0;
	return 0;
}

int port_readbyte(int timeout_ms)
{
	struct pollfd p = { .fd = 0, .events = POLLIN };
	unsigned char c;

	if (timeout_ms >= 0) {
		int r = poll(&p, 1, timeout_ms);
		if (r == 0)
			return AL_KEY_NONE;
		if (r < 0)
			return errno == EINTR ? AL_KEY_INTR : AL_KEY_ERROR;
	}
	long n = port_read(0, &c, 1);
	if (n == 1)
		return c;
	return n == 0 ? AL_KEY_EOF : AL_KEY_ERROR;
}

void port_tty_size(int *cols, int *rows)
{
	struct winsize ws;

	if (ioctl(1, TIOCGWINSZ, &ws) || !ws.ws_col || !ws.ws_row) {
		*cols = 80;
		*rows = 24;
		return;
	}
	*cols = ws.ws_col;
	*rows = ws.ws_row;
}

struct job {
	int	(*fn)(int, char **);
	int	  argc;
	char	**argv;
	int	  status;
};

static void *job_main(void *arg)
{
	struct job *j = arg;

	j->status = j->fn(j->argc, j->argv);
	return NULL;
}

int main(int argc, char **argv)
{
	const char *base = strrchr(argv[0], '/');
	struct job job = { al_main_a, argc, argv, 1 };

	base = base ? base + 1 : argv[0];
	if (!strcmp(base, "ac"))
		job.fn = al_main_ac;
	if (!getenv("AL_STATS"))
		return job.fn(argc, argv);

	size_t size = 1 << 20;
	unsigned char *stack = malloc(size);
	pthread_attr_t attr;
	pthread_t thread;

	if (!stack)
		return 1;
	memset(stack, 0xa5, size);
	pthread_attr_init(&attr);
	pthread_attr_setstack(&attr, stack, size);
	if (pthread_create(&thread, &attr, job_main, &job))
		return 1;
	pthread_join(thread, NULL);
	size_t untouched = 0;
	while (untouched < size && stack[untouched] == 0xa5)
		untouched++;
	fprintf(stderr, "stats: stack %zu bytes, heap peak %zu bytes\n", size - untouched, heap_peak);
	return job.status;
}
