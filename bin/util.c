#include <stdio.h>
#include <string.h>

#include "util.h"

int fail(const char *prog, const char *what, int err)
{
	if (what)
		pt_dprintf(PT_STDERR, "%s: %s: %s\n", prog, what, pt_strerror(err));
	else
		pt_dprintf(PT_STDERR, "%s: %s\n", prog, pt_strerror(err));
	return 1;
}

int parse_flags(const char *prog, int argc, char **argv, const char *allowed, uint32_t *flags)
{
	int i = 1;

	*flags = 0;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "--"))
			return i + 1;
		for (const char *c = argv[i] + 1; *c; c++) {
			if (*c < 'a' || *c > 'z' || !strchr(allowed, *c)) {
				pt_dprintf(PT_STDERR, "%s: unknown option -%c (try 'help %s')\n", prog, *c, prog);
				return -1;
			}
			*flags |= 1u << (*c - 'a');
		}
	}
	return i;
}

int write_all(int fd, const char *s, size_t n)
{
	while (n) {
		ssize_t r = pt_write(fd, s, n);
		if (r <= 0)
			return r ? (int)r : -EIO;
		s += r;
		n -= r;
	}
	return 0;
}

uint32_t crc32_of(uint32_t crc, const void *data, size_t n)
{
	const uint8_t *p = data;

	crc = ~crc;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
	}
	return ~crc;
}

ssize_t copy_fd(int in, int out)
{
	char *buf = pt_malloc(BUF_SIZE);
	ssize_t total = 0, n;

	if (!buf)
		return -ENOMEM;
	while ((n = pt_read(in, buf, BUF_SIZE)) > 0) {
		int err = write_all(out, buf, n);
		if (err) {
			n = err;
			break;
		}
		total += n;
	}
	pt_free(buf);
	return n < 0 ? n : total;
}

void human_size(uint64_t bytes, char *out, size_t size)
{
	static const char units[] = "BKMGT";
	double v = bytes;
	int u = 0;

	while (v >= 1024 && u < 4) {
		v /= 1024;
		u++;
	}
	if (u == 0)
		snprintf(out, size, "%lluB", (unsigned long long)bytes);
	else
		snprintf(out, size, v < 10 ? "%.1f%c" : "%.0f%c", v, units[u]);
}

const char *join_path(const char *dir, const char *name, char *out, size_t size)
{
	size_t len = strlen(dir);
	int n = snprintf(out, size, "%s%s%s", dir, len && dir[len - 1] == '/' ? "" : "/", name);

	return n >= 0 && (size_t)n < size ? out : NULL;
}
