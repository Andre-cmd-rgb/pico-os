/*
 * The platform under the language: port_pt.c on PocketType, port_host.c on
 * a PC. Calls that can fail return a negative errno value, as in pt/sys.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	PORT_O_READ,
	PORT_O_WRITE,		/* create or truncate */
	PORT_O_APPEND,		/* create, write at the end */
};

void	*port_alloc(size_t n);
void	*port_realloc(void *p, size_t n);
void	 port_free(void *p);

long	 port_read(int fd, void *buf, size_t n);
long	 port_write(int fd, const void *buf, size_t n);
int	 port_open(const char *path, int mode);
int	 port_close(int fd);
bool	 port_isatty(int fd);
int	 port_stat(const char *path, bool *is_dir, int64_t *size);
int	 port_mkdir(const char *path);
int	 port_remove(const char *path);		/* file or empty directory */
int	 port_rename(const char *from, const char *to);
/* Calls fn for each entry except . and ..; stops early if fn returns non-zero. */
int	 port_listdir(const char *path, int (*fn)(void *ctx, const char *name), void *ctx);
const char *port_strerror(int err);

#define PORT_EINTR	(-4)

/*
 * Runs a command with our stdin, stdout and stderr: its exit status, <0 if
 * it could not start, or PORT_EINTR if we were interrupted (the child has
 * been told to stop).
 */
int	 port_run(int argc, const char **argv);
const char *port_getenv(const char *name);

/* A signal (Ctrl-C, kill) is waiting: stop the program. */
bool	 port_interrupted(void);
/* Ends the process the way the pending signal asks; returns if it cannot. */
void	 port_die_interrupted(void);
void	 port_sleep_ms(int ms);
int64_t	 port_uptime_us(void);
int64_t	 port_time(void);
void	 port_date(int64_t t, char *buf, size_t n);	/* "YYYY-MM-DD HH:MM:SS" */

int	 port_tty_raw(bool on);
/* A byte from stdin, AL_KEY_EOF, or AL_KEY_NONE after timeout_ms (-1 waits). */
int	 port_readbyte(int timeout_ms);
void	 port_tty_size(int *cols, int *rows);
