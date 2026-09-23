/*
 * Small helpers shared by the built-in programs.
 */
#pragma once

#include "pt/keys.h"
#include "pt/program.h"
#include "pt/sys.h"

#define BUF_SIZE	4096

/* "prog: what: No such file or directory", returns 1 */
int	fail(const char *prog, const char *what, int err);

/*
 * Parse clustered short flags ("-la") from argv[1...]. Letters must appear in
 * `allowed`; each sets bit (letter - 'a') in *flags. Stops at "--" or the
 * first non-flag. Returns the index of the first operand, or -1 after
 * printing an error.
 */
int	parse_flags(const char *prog, int argc, char **argv, const char *allowed, uint32_t *flags);
#define FLAG(f, c)	((f) & (1u << ((c) - 'a')))

/*
 * POSIX getopt for options with arguments: `spec` lists the letters, and a
 * ':' after one means it takes an argument ("n:" gives -n 5 and -n5).
 * Returns the letter, -1 at the first operand or after "--", or '?' after
 * saying what was wrong. o->arg is the argument; o->ind the index of the
 * next argv entry. Start with struct opt o = { .ind = 1 }.
 */
struct opt {
	int		 ind, pos;
	const char	*arg;
};

int	getopt_pt(struct opt *o, const char *prog, int argc, char **argv, const char *spec);

/* Reflected CRC-32, the gzip / PNG polynomial: what zlib and cksum print. */
uint32_t crc32_of(uint32_t crc, const void *data, size_t n);

/*
 * Lines of any length from an fd. lines_next() returns the next one, with
 * its '\n' if it has one (the last line of a file may not), and its length
 * in *len; NULL at the end, or on a read error, which is then in l->err.
 * A line is valid until the next call.
 */
struct lines {
	int	 fd;
	char	*buf;
	size_t	 cap, start, end;
	bool	 eof;
	int	 err;
};

void	lines_init(struct lines *l, int fd);
char	*lines_next(struct lines *l, size_t *len);
void	lines_free(struct lines *l);

/*
 * Runs fn on each named file, or on standard input when there are none; "-"
 * is standard input too, as POSIX has it. The status is 1 if a file could
 * not be opened, otherwise whatever fn returned, or-ed together.
 */
int	for_each_input(const char *prog, int argc, char **argv, int first,
		       int (*fn)(int fd, const char *name, void *ctx), void *ctx);

/* A whole decimal number, sign allowed: 0, or -EINVAL for anything else. */
int	parse_long(const char *s, long *out);

/*
 * The escape at *s, which is just past a backslash: \n \t \\ \a \b \f \r
 * \v \e, \NNN octal and \xHH hex. Advances *s and returns the byte; an
 * escape it does not know stands for the character itself.
 */
int	escape_char(const char **s);

/* The same, but octal is \0NNN -- a 0, then up to three digits -- as echo
 * -e and printf's %b write it. */
int	escape_char_0(const char **s);

/*
 * Runs a command -- a program name or a path -- and waits for it: its exit
 * status, or -errno if it could not be started (-ENOENT: no such command).
 * Ctrl-C while it runs is passed on to it.
 */
int	run_command(int argc, char *const argv[]);

ssize_t	copy_fd(int in, int out);
int	write_all(int fd, const char *s, size_t n);
void	human_size(uint64_t bytes, char *out, size_t size);
const char *join_path(const char *dir, const char *name, char *out, size_t size);	/* NULL if too long */

/*
 * Choose a file out of `dir`, with arrows and Enter. `exts` is a
 * NULL-terminated list of lower-case suffixes to show (NULL shows
 * everything); directories are always shown and can be walked into.
 * `title` heads the list, or NULL for the directory's own name.
 * Returns 0 and writes the path to `out`, -ECANCELED if the user backed
 * out, or another negative error.
 */
int	pick_file(const char *dir, const char *const *exts, const char *title,
		  char *out, size_t size);
