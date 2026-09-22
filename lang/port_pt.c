/*
 * Port layer for PocketType: the ac and a commands, the executable loader,
 * and the platform calls mapped onto pt/sys.h.
 */
#include <stdio.h>
#include <time.h>

#include "pt/keys.h"
#include "pt/program.h"
#include "pt/sys.h"

#include "al.h"
#include "driver.h"
#include "port.h"

_Static_assert((int)AL_KEY_UP == (int)PT_KEY_UP && (int)AL_KEY_F4 == (int)PT_KEY_F4 &&
	       (int)AL_KEY_UNKNOWN == (int)PT_KEY_UNKNOWN && (int)AL_KEY_EOF == (int)PT_KEY_EOF,
	       "key codes must match pt/keys.h");

PT_PROGRAM_STACK(ac, 16, "compile an a program\n"
		 "usage: ac [-o program] [-d] file.al\n"
		 "  -o  name of the program (default: file.al without .al)\n"
		 "  -d  print the bytecode instead of writing a program\n"
		 "Run the result with ./program. See docs/LANGUAGE.md.")
{
	return al_main_ac(argc, argv);
}

PT_PROGRAM_STACK(a, 16, "run an a program\n"
		 "usage: a file.al [args...]   compile in memory and run\n"
		 "       a program [args...]   run a program made by ac")
{
	return al_main_a(argc, argv);
}

static bool loader_probe(const char *path, const uint8_t *head, size_t n)
{
	return al_probe(head, n);
}

static int loader_exec(const char *path, int argc, char **argv)
{
	return al_exec(path, argc, argv);
}

static struct pt_loader al_loader = {
	.name = "a",
	.stack_kb = 12,
	.probe = loader_probe,
	.exec = loader_exec,
};

__attribute__((constructor)) static void al_loader_register(void)
{
	loader_register(&al_loader);
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
		return AL_KEY_NONE;
	if (n == 0)
		return AL_KEY_EOF;
	return n == -EINTR ? AL_KEY_INTR : AL_KEY_ERROR;
}

void port_tty_size(int *cols, int *rows)
{
	pt_tty_size(PT_STDOUT, cols, rows);
}
