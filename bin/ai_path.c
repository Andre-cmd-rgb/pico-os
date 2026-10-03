/* Normalise first, then check the workspace and the notes as separate roots. */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ai_path.h"
#include "pt/sys.h"

bool ai_path_within(const char *path, const char *root)
{
	size_t n = strlen(root);

	return n && !strncmp(path, root, n) &&
	       ((n == 1 && *root == '/') || !path[n] || path[n] == '/');
}

/* FAT ignores case; /mnt/sd and the real home can name the same notes.
 * Neither spelling may turn an always-confirmed note into a project edit. */
bool ai_path_is_note(const char *path, const char *notes, const char *home, bool sd)
{
	char alias[PT_PATH_MAX];
	size_t n = strlen(notes);

	if (n && !strncasecmp(path, notes, n) && (!path[n] || path[n] == '/'))
		return true;
	if (!sd || !ai_path_within(notes, home) || strncmp(home, "/home/", 6))
		return false;
	if (snprintf(alias, sizeof(alias), "/mnt/sd%s", notes + strlen(home)) >= (int)sizeof(alias))
		return false;
	n = strlen(alias);
	return !strncasecmp(path, alias, n) && (!path[n] || path[n] == '/');
}

bool ai_path_resolve(const char *root, const char *notes, const char *home,
		     const char *given, char *out, size_t size)
{
	char joined[PT_PATH_MAX * 2];
	const char *p;
	size_t len = 1;
	int n;

	if (!given || !size || !*root)
		return false;
	if (*given == '~' && (!given[1] || given[1] == '/'))
		n = snprintf(joined, sizeof(joined), "%s%s", home, given + 1);
	else if (*given == '/')
		n = snprintf(joined, sizeof(joined), "%s", given);
	else
		n = snprintf(joined, sizeof(joined), "%s/%s", root, given);
	if (n < 0 || n >= (int)sizeof(joined) || size < 2)
		return false;
	out[0] = '/';
	out[1] = '\0';
	for (p = joined; *p;) {
		size_t part;

		while (*p == '/')
			p++;
		part = strcspn(p, "/");
		if (!part)
			break;
		if (part == 2 && !strncmp(p, "..", 2)) {
			while (len > 1 && out[len - 1] != '/')
				len--;
			if (len > 1)
				len--;
		} else if (!(part == 1 && *p == '.')) {
			if (len + (len > 1) + part >= size)
				return false;
			if (len > 1)
				out[len++] = '/';
			memcpy(out + len, p, part);
			len += part;
		}
		out[len] = '\0';
		p += part;
	}
	return ai_path_within(out, root) || ai_path_within(out, notes);
}
