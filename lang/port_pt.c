/*
 * Port layer for PocketType: the picoc and pico commands, the executable loader,
 * and the platform calls mapped onto pt/sys.h.
 */
#include <stdio.h>
#include <time.h>

#include "pt/keys.h"
#include "pt/program.h"
#include "pt/sys.h"

#include "pico.h"
#include "driver.h"
#include "port.h"

_Static_assert((int)PICO_KEY_UP == (int)PT_KEY_UP && (int)PICO_KEY_F4 == (int)PT_KEY_F4 &&
	       (int)PICO_KEY_UNKNOWN == (int)PT_KEY_UNKNOWN && (int)PICO_KEY_EOF == (int)PT_KEY_EOF,
	       "key codes must match pt/keys.h");

PT_COMPLETE(picoc, ": -o -d -t <file:.pico.al>\n-t: <file>\n*: <file:.pico.al>\n")

PT_PROGRAM_STACK(picoc, 16, "compile a pico program\n"
		 "usage: picoc [-o program] [-d] file.pico\n"
		 "       picoc -t program\n"
		 "  -o  name of the program (default: the file without\n"
		 "      .pico)\n"
		 "  -d  print the bytecode instead of writing a program\n"
		 "  -t  check a program compiled elsewhere: 0 if this\n"
		 "      system would run it, as pkg does with packages\n"
		 "Run the result with ./program. See `man pico`.")
{
	return pico_main_compile(argc, argv);
}

PT_COMPLETE(pico, ": <file:.pico.al>\n")

PT_PROGRAM_STACK(pico, 16, "run a pico program\n"
		 "usage: pico file.pico [args...]   compile in memory and run\n"
		 "       pico program [args...]     run a program made by picoc")
{
	return pico_main_run(argc, argv);
}

static bool loader_probe(const char *path, const uint8_t *head, size_t n)
{
	return pico_probe(head, n);
}

static int loader_exec(const char *path, int argc, char **argv)
{
	return pico_exec(path, argc, argv);
}

static struct pt_loader pico_loader = {
	.name = "pico",
	.stack_kb = 12,
	.probe = loader_probe,
	.exec = loader_exec,
};

__attribute__((constructor)) static void pico_loader_register(void)
{
	loader_register(&pico_loader);
}

void *port_alloc(size_t n)
{
	return pt_malloc(n);
}

void *port_realloc(void *p, size_t n)
{
	return pt_realloc(p, n);
}

void port_free(void *p)
{
	pt_free(p);
}

long port_read(int fd, void *buf, size_t n)
{
	return pt_read(fd, buf, n);
}

long port_write(int fd, const void *buf, size_t n)
{
	return pt_write(fd, buf, n);
}

int port_open(const char *path, int mode)
{
	int flags = mode == PORT_O_READ ? O_RDONLY :
		    mode == PORT_O_WRITE ? O_WRONLY | O_CREAT | O_TRUNC : O_WRONLY | O_CREAT | O_APPEND;

	return pt_open(path, flags);
}

int port_close(int fd)
{
	return pt_close(fd);
}

bool port_isatty(int fd)
{
	return pt_isatty(fd);
}

int port_stat(const char *path, bool *is_dir, int64_t *size)
{
	struct pt_stat st;
	int err = pt_stat(path, &st);

	if (err)
		return err;
	*is_dir = st.is_dir;
	*size = st.size;
	return 0;
}

int port_mkdir(const char *path)
{
	return pt_mkdir(path);
}

int port_remove(const char *path)
{
	struct pt_stat st;
	int err = pt_stat(path, &st);

	if (err)
		return err;
	return st.is_dir ? pt_rmdir(path) : pt_unlink(path);
}

int port_rename(const char *from, const char *to)
{
	return pt_rename(from, to);
}

int port_listdir(const char *path, int (*fn)(void *ctx, const char *name), void *ctx)
{
	struct pt_dirent ent;
	pt_dir_t *dir;
	int err = pt_opendir(path, &dir);

	if (err)
		return err;
	while (pt_readdir(dir, &ent) == 1) {
		if ((ent.name[0] == '.' && !ent.name[1]) ||
		    (ent.name[0] == '.' && ent.name[1] == '.' && !ent.name[2]))
			continue;
		if (fn(ctx, ent.name))
			break;
	}
	pt_closedir(dir);
	return 0;
}

const char *port_strerror(int err)
{
	return pt_strerror(err);
}

/*
 * The child gets its own process group, so Ctrl-C reaches only us: catch
 * it while waiting, pass it on, and let the program end afterwards.
 */
int port_run(int argc, const char **argv)
{
	struct pt_spawn req = {
		.cmd = argv[0],
		.argc = argc,
		.argv = (char *const *)argv,
		.fd = { -1, -1, -1 },
	};
	bool stopped = false;
	int status = 0;

	pt_sigcatch(true);
	int pid = pt_spawn(&req);
	if (pid < 0) {
		pt_sigcatch(false);
		return pid;
	}
	for (;;) {
		int r = pt_wait(pid, &status, false);
		if (r != -EINTR)
			break;
		if (!stopped)
			pt_kill(pid, PT_SIGINT);
		stopped = true;
		pt_sigcatch(true);	/* clears SIGINT/SIGTERM so waiting works */
		if (pt_interrupted())
			pt_sigcatch(false);	/* SIGKILL: gone now */
	}
	pt_sigcatch(false);
	return stopped ? PORT_EINTR : status;
}

/* Keeps what `fd` gives until its end, in *buf; beyond the most, drops it. */
static long take_output(int fd, int pid, char **buf, size_t *n, bool *stopped)
{
	size_t cap = 4096;
	char scrap[256];

	*buf = port_alloc(cap);
	*n = 0;
	for (;;) {
		long got;

		if (*buf && *n + 1 >= cap && cap < PORT_OUTPUT_MAX + 1) {
			char *more = port_realloc(*buf, cap * 2 > PORT_OUTPUT_MAX + 1 ? PORT_OUTPUT_MAX + 1 : cap * 2);

			if (more) {
				*buf = more;
				cap = cap * 2 > PORT_OUTPUT_MAX + 1 ? PORT_OUTPUT_MAX + 1 : cap * 2;
			}
		}
		if (*buf && *n + 1 < cap)
			got = pt_read(fd, *buf + *n, cap - *n - 1);
		else
			got = pt_read(fd, scrap, sizeof(scrap));	/* full: drain it */
		if (got == -EINTR) {
			if (!*stopped)
				pt_kill(pid, PT_SIGINT);
			*stopped = true;
			pt_sigcatch(true);
			if (pt_interrupted())
				pt_sigcatch(false);
			continue;
		}
		if (got <= 0)
			break;
		if (*buf && *n + 1 < cap)
			*n += got;
	}
	if (*buf)
		(*buf)[*n] = '\0';
	return *buf ? 0 : -ENOMEM;
}

int port_run_output(int argc, const char **argv, char **out, size_t *len)
{
	struct pt_spawn req = {
		.cmd = argv[0],
		.argc = argc,
		.argv = (char *const *)argv,
		.fd = { -1, -1, -1 },
	};
	bool stopped = false;
	int fds[2], status = 0, err;

	*out = NULL;
	*len = 0;
	if ((err = pt_pipe(fds)))
		return err;
	req.fd[1] = fds[1];
	pt_sigcatch(true);
	int pid = pt_spawn(&req);
	pt_close(fds[1]);		/* the child's copy is the one that ends it */
	if (pid < 0) {
		pt_close(fds[0]);
		pt_sigcatch(false);
		return pid;
	}
	err = (int)take_output(fds[0], pid, out, len, &stopped);
	pt_close(fds[0]);
	for (;;) {
		int r = pt_wait(pid, &status, false);
		if (r != -EINTR)
			break;
		if (!stopped)
			pt_kill(pid, PT_SIGINT);
		stopped = true;
		pt_sigcatch(true);
		if (pt_interrupted())
			pt_sigcatch(false);
	}
	pt_sigcatch(false);
	if (stopped || err) {
		port_free(*out);
		*out = NULL;
		return stopped ? PORT_EINTR : err;
	}
	return status;
}

const char *port_getenv(const char *name)
{
	return pt_getenv(name);
}

bool port_interrupted(void)
{
	return pt_interrupted();
}

/* Ends the process with the status the pending signal calls for. */
void port_die_interrupted(void)
{
	pt_sigcatch(false);
}

void port_sleep_ms(int ms)
{
	pt_sleep_ms(ms);
}

int64_t port_uptime_us(void)
{
	return pt_uptime_us();
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
	return pt_tty_raw(PT_STDIN, on);
}

int port_readbyte(int timeout_ms)
{
	unsigned char c;
	int off = -1;

	if (timeout_ms >= 0)
		pt_ioctl(PT_STDIN, PT_TTY_SETTIMEOUT, &timeout_ms);
	ssize_t n = pt_read(PT_STDIN, &c, 1);
	if (timeout_ms >= 0)
		pt_ioctl(PT_STDIN, PT_TTY_SETTIMEOUT, &off);
	if (n == 1)
		return c;
	if (n == -EAGAIN)
		return PICO_KEY_NONE;
	if (n == 0)
		return PICO_KEY_EOF;
	return n == -EINTR ? PICO_KEY_INTR : PICO_KEY_ERROR;
}

void port_tty_size(int *cols, int *rows)
{
	pt_tty_size(PT_STDOUT, cols, rows);
}
