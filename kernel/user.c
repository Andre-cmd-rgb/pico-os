/*
 * Who uses the machine, and where: the user's name, which is also the
 * home directory's (/home/<name>, where the SD card is seen again), and
 * the time zone. `setup` asks for them the first time the machine starts
 * and keeps them in /etc/user and /etc/timezone; until then they are what
 * menuconfig says.
 *
 * The home directory's path is read by the mount table on every lookup,
 * from any task, so a change is written into the other of two copies and
 * the switch between them is one store: a lookup sees the old path or the
 * new one, never half of each.
 *
 * This is kernel code, and the files are reached through the mount table
 * and stdio, as auth.c's are.
 */
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "pt/kernel.h"

#define USER_FILE	"/etc/user"
#define TZ_FILE		"/etc/timezone"

struct who {
	char	name[USER_NAME_MAX + 1];
	char	home[USER_NAME_MAX + 7];	/* "/home/" and the name */
	char	tz[USER_TZ_MAX + 1];		/* the POSIX TZ string */
	char	tz_name[USER_TZ_MAX + 1];	/* "Europe/Rome", or "" */
};

static struct who who[2] = {
	{ CONFIG_PT_USERNAME, "/home/" CONFIG_PT_USERNAME, CONFIG_PT_TZ, "" },
};
static atomic_int now_at;		/* which of the two is in use */
static bool configured;			/* /etc/user is there */
static SemaphoreHandle_t lock;		/* one change at a time */

static const struct who *cur(void)
{
	return &who[atomic_load(&now_at)];
}

/* A copy of what is in use, to change and then put in its place. */
static struct who *begin(void)
{
	int next = !atomic_load(&now_at);

	if (lock)
		xSemaphoreTake(lock, portMAX_DELAY);
	who[next] = *cur();
	return &who[next];
}

static void commit(struct who *w)
{
	atomic_store(&now_at, (int)(w - who));
	if (lock)
		xSemaphoreGive(lock);
}

static void abandon(void)
{
	if (lock)
		xSemaphoreGive(lock);
}

const char *user_name(void)
{
	return cur()->name;
}

const char *user_home(void)
{
	return cur()->home;
}

const char *user_tz(void)
{
	return cur()->tz;
}

const char *user_tz_name(void)
{
	return cur()->tz_name;
}

bool user_configured(void)
{
	return configured;
}

/* As useradd has it: a small letter or _, then letters, digits, _ and -. */
bool user_name_ok(const char *name)
{
	size_t n = strlen(name);

	if (!n || n > USER_NAME_MAX || !((*name >= 'a' && *name <= 'z') || *name == '_'))
		return false;
	for (const char *p = name; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' ||
		      *p == '-'))
			return false;
	return true;
}

/* A line of up to size - 1 bytes, its newline gone; false if there is none. */
static bool read_first_line(const char *file, char *out, size_t size)
{
	char path[PT_PATH_MAX];
	FILE *f;
	bool got;

	if (!mount_resolve(file, path, sizeof(path)) || !(f = fopen(path, "r")))
		return false;
	got = fgets(out, size, f) != NULL;
	fclose(f);
	if (got)
		out[strcspn(out, "\r\n")] = '\0';
	return got && *out;
}

/* The whole file, written aside and renamed over the old one. */
static int write_file(const char *file, const char *text)
{
	char path[PT_PATH_MAX], tmp[PT_PATH_MAX + 8];
	FILE *f;
	int err;

	if (!mount_resolve(file, path, sizeof(path)))
		return -ENOENT;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (!(f = fopen(tmp, "w")))
		return -errno;
	fputs(text, f);
	if (fclose(f)) {
		err = -errno;
		remove(tmp);
		return err;
	}
	return rename(tmp, path) ? -errno : 0;
}

static void apply_tz(const char *tz)
{
	setenv("TZ", tz, 1);
	tzset();
}

/* At boot, once / is mounted, and before the SD card is. */
void user_restore(void)
{
	char line[USER_TZ_MAX * 2 + 4], *space;
	struct who *w;

	lock = xSemaphoreCreateMutex();
	w = begin();
	if (read_first_line(USER_FILE, line, sizeof(line))) {
		configured = true;
		if (user_name_ok(line)) {
			strlcpy(w->name, line, sizeof(w->name));
			snprintf(w->home, sizeof(w->home), "/home/%s", w->name);
		} else {
			klog("user: %s: \"%s\" is not a user name; %s it is", USER_FILE, line,
			     w->name);
		}
	}
	/* "Europe/Rome CET-1CEST,M3.5.0,M10.5.0/3", or only the TZ string */
	if (read_first_line(TZ_FILE, line, sizeof(line))) {
		space = strchr(line, ' ');
		if (space) {
			*space = '\0';
			strlcpy(w->tz_name, line, sizeof(w->tz_name));
			strlcpy(w->tz, space + 1, sizeof(w->tz));
		} else {
			w->tz_name[0] = '\0';
			strlcpy(w->tz, line, sizeof(w->tz));
		}
	}
	commit(w);
	apply_tz(user_tz());
}

/*
 * A new name, kept in /etc/user. The directory on the flash under the
 * old home is renamed with it -- with no card in, that is where the
 * user's files are -- and the card's mount follows the name at once.
 * Programs already running keep the old $HOME.
 */
int user_set_name(const char *name)
{
	char base[PT_PATH_MAX], from[PT_PATH_MAX + USER_NAME_MAX + 2];
	char to[PT_PATH_MAX + USER_NAME_MAX + 2], text[USER_NAME_MAX + 2];
	struct who *w;
	struct stat st;
	int err;

	if (!user_name_ok(name))
		return -EINVAL;
	w = begin();
	if (strcmp(name, w->name)) {
		/* /home is no mount point: this is the flash, under the card */
		if (!mount_resolve("/home", base, sizeof(base))) {
			abandon();
			return -ENOENT;
		}
		snprintf(from, sizeof(from), "%s/%s", base, w->name);
		snprintf(to, sizeof(to), "%s/%s", base, name);
		if (stat(to, &st) && rename(from, to) && mkdir(to, 0777) && errno != EEXIST) {
			err = -errno;
			abandon();
			return err;
		}
		strlcpy(w->name, name, sizeof(w->name));
		snprintf(w->home, sizeof(w->home), "/home/%s", w->name);
	}
	snprintf(text, sizeof(text), "%s\n", name);
	err = write_file(USER_FILE, text);
	if (err) {
		abandon();
		return err;
	}
	configured = true;
	commit(w);
	return 0;
}

/* A time zone: its POSIX TZ string, and a name for it or NULL. */
int user_set_tz(const char *name, const char *tz)
{
	char text[USER_TZ_MAX * 2 + 4];
	struct who *w;
	int err;

	if (!*tz || strlen(tz) > USER_TZ_MAX || strchr(tz, ' ') ||
	    (name && (strlen(name) > USER_TZ_MAX || strchr(name, ' '))))
		return -EINVAL;
	w = begin();
	strlcpy(w->tz, tz, sizeof(w->tz));
	strlcpy(w->tz_name, name ? name : "", sizeof(w->tz_name));
	if (name && *name)
		snprintf(text, sizeof(text), "%s %s\n", name, tz);
	else
		snprintf(text, sizeof(text), "%s\n", tz);
	err = write_file(TZ_FILE, text);
	if (err) {
		abandon();
		return err;
	}
	commit(w);
	apply_tz(tz);
	return 0;
}
