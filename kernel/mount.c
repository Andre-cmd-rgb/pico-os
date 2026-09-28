/*
 * The mount table.
 */
#include <stdio.h>
#include <string.h>

#include "pt/kernel.h"

#define MAX_MOUNTS 8

static const struct pt_mount *mounts[MAX_MOUNTS];
static int n_mounts;

void mount_register(const struct pt_mount *m)
{
	if (n_mounts < MAX_MOUNTS)
		mounts[n_mounts++] = m;
}

int mount_count(void)
{
	return n_mounts;
}

const struct pt_mount *mount_get(int index)
{
	return index >= 0 && index < n_mounts ? mounts[index] : NULL;
}

/* Where it is mounted: a NULL path is the home directory, which moves with the user's name. */
const char *mount_path(const struct pt_mount *m)
{
	return m->path ? m->path : user_home();
}

static bool present(const struct pt_mount *m)
{
	return !m->present || m->present();
}

/* Length of mount path `mp` if it is `abs` or a directory above it, else -1. */
static int covers(const char *mp, const char *abs)
{
	size_t n = strlen(mp);

	if (n == 1)
		return 0;			/* "/" covers everything */
	if (strncmp(abs, mp, n) || (abs[n] && abs[n] != '/'))
		return -1;
	return n;
}

const struct pt_mount *mount_resolve(const char *abs, char *vfs, size_t size)
{
	const struct pt_mount *best = NULL;
	int best_len = -1;

	for (int i = 0; i < n_mounts; i++) {
		int len = covers(mount_path(mounts[i]), abs);
		if (len > best_len && present(mounts[i])) {
			best = mounts[i];
			best_len = len;
		}
	}
	if (!best)
		return NULL;

	const char *rest = abs + best_len;
	if (!strcmp(rest, "/"))
		rest = "";
	if ((size_t)snprintf(vfs, size, "%s%s", best->vfs, rest) >= size)
		return NULL;
	return best;
}

bool mount_is_point(const char *abs)
{
	for (int i = 0; i < n_mounts; i++)
		if (!strcmp(mount_path(mounts[i]), abs) && present(mounts[i]))
			return true;
	return false;
}
