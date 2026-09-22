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

/* Reflected CRC-32, the gzip / PNG polynomial: what zlib and cksum print. */
uint32_t crc32_of(uint32_t crc, const void *data, size_t n);

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
