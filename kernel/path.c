#include <string.h>

#include "pt/kernel.h"

/* Join `in` onto `cwd` unless absolute, then resolve ".", ".." and "//". */
int path_normalize(const char *cwd, const char *in, char *out, size_t size)
{
	char joined[PT_PATH_MAX * 2];
	const char *parts[64];
	size_t lens[64];
	int depth = 0;

	if ((size_t)snprintf(joined, sizeof(joined), "%s/%s",
			     in[0] == '/' ? "" : (cwd ? cwd : "/"), in) >= sizeof(joined))
		return -ENAMETOOLONG;

	for (const char *s = joined; *s;) {
		while (*s == '/')
			s++;
		size_t len = strcspn(s, "/");
		if (!len)
			break;
		if (len == 1 && s[0] == '.') {
			/* nothing */
		} else if (len == 2 && s[0] == '.' && s[1] == '.') {
			if (depth)
				depth--;
		} else if (depth == 64) {
			return -ENAMETOOLONG;
		} else {
			parts[depth] = s;
			lens[depth++] = len;
		}
		s += len;
	}

	size_t o = 0;
	for (int i = 0; i < depth; i++) {
		if (o + 1 + lens[i] + 1 > size)
			return -ENAMETOOLONG;
		out[o++] = '/';
		memcpy(out + o, parts[i], lens[i]);
		o += lens[i];
	}
	if (!depth) {
		if (size < 2)
			return -ENAMETOOLONG;
		out[o++] = '/';
	}
	out[o] = '\0';
	return 0;
}

const char *path_basename(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash && slash[1] ? slash + 1 : path;
}
