/*
 * /etc on the card too. The flash keeps the system and its settings --
 * the Wi-Fi networks, the user, the time zone, the theme, what the
 * battery has learnt -- and the card keeps a copy of them in .etc (~/.etc,
 * the card being the home directory), so that they outlive the flash
 * being erased. A / that comes up with no user, as an erased one does,
 * takes back what the card has at boot and starts again with it. One that
 * has a user gives the card its /etc: when the card goes in, before it
 * comes out, on `sync`, and within half a minute of anything in /etc
 * changing.
 *
 * Only the files directly in /etc go, and not the logs (power.log is
 * written every few minutes, and is history rather than a setting) nor
 * the .tmp and .new files that become settings by being renamed. The copy
 * follows /etc, deletions included: it is a copy, not an archive. The
 * Wi-Fi passwords are in plain text in /etc/wifi, and so on the card too.
 *
 * Its task's stack is in PSRAM: LittleFS and the card driver are both
 * fine with that, since every flash call is moved off it (internal.c).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include "pt/kernel.h"

#define ETC		"/etc"
#define CARD		"/mnt/sd"
#define CARD_ETC	CARD "/.etc"
#define POLL_S		5		/* the card is looked for this often */
#define CHECK_S		30		/* and /etc looked at for changes */
#define CHUNK		1024
#define VFS_MAX		(PT_PATH_MAX + 16)

/* Paths and buffers, off the stacks: boot's and the task's are small. */
struct work {
	char	etc[VFS_MAX], card[VFS_MAX];	/* the two directories */
	char	from[VFS_MAX], to[VFS_MAX], tmp[VFS_MAX + 4];
	char	a[CHUNK], b[CHUNK];
};

static SemaphoreHandle_t lock;		/* over everything below */
static struct work *work;
static bool ready;			/* boot has had its chance to take the copy back */
static bool card_has;			/* the card's copy is /etc as it looked at `seen` */
static uint32_t seen;
static int last_err;			/* said once, not twice a minute */

/* A setting, rather than a log or half of a rename. */
static bool wanted(const char *name)
{
	size_t n = strlen(name);

	if (name[0] == '.' || strstr(name, ".log"))
		return false;
	return n <= 4 || (strcmp(name + n - 4, ".tmp") && strcmp(name + n - 4, ".new"));
}

static bool join(char *out, const char *dir, const char *name)
{
	return (size_t)snprintf(out, VFS_MAX, "%s/%s", dir, name) < VFS_MAX;
}

/* The two directories as the VFS has them; false when the card is not in. */
static bool paths(struct work *w)
{
	const struct pt_mount *m;

	if (!mount_resolve(ETC, w->etc, sizeof(w->etc)))
		return false;
	/* with the card out, /mnt/sd is the empty directory on the flash */
	m = mount_resolve(CARD_ETC, w->card, sizeof(w->card));
	return m && !strcmp(mount_path(m), CARD);
}

/* Whether w->from and w->to hold the same bytes; a missing file never does. */
static bool same(struct work *w)
{
	int fa = open(w->from, O_RDONLY), fb = open(w->to, O_RDONLY);
	bool eq = fa >= 0 && fb >= 0;

	while (eq) {
		ssize_t na = read(fa, w->a, CHUNK), nb = read(fb, w->b, CHUNK);

		eq = na >= 0 && na == nb && !memcmp(w->a, w->b, na);
		if (na <= 0)
			break;
	}
	if (fa >= 0)
		close(fa);
	if (fb >= 0)
		close(fb);
	return eq;
}

/* w->from to w->to, written aside and renamed, as the drivers write their files. */
static int copy(struct work *w)
{
	int in, out, err = 0;
	ssize_t n;

	snprintf(w->tmp, sizeof(w->tmp), "%s.new", w->to);
	if ((in = open(w->from, O_RDONLY)) < 0)
		return -errno;
	if ((out = open(w->tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
		err = -errno;
		close(in);
		return err;
	}
	while ((n = read(in, w->a, CHUNK)) > 0)
		if (write(out, w->a, n) != n) {
			err = errno ? -errno : -ENOSPC;
			break;
		}
	if (n < 0 && !err)
		err = -errno;
	close(in);
	if (close(out) && !err)
		err = -errno;
	if (!err) {
		unlink(w->to);			/* FAT will not rename over a file */
		if (rename(w->tmp, w->to))
			err = -errno;
	}
	if (err)
		unlink(w->tmp);
	return err;
}

/* What /etc looks like -- its files' names, sizes and times -- and whether the card has its copy. */
static uint32_t look(struct work *w)
{
	uint32_t h = 2166136261u;	/* FNV-1a */
	struct dirent *e;
	struct stat st;
	DIR *d;

#define FOLD(p, n)	for (size_t i_ = 0; i_ < (n); i_++) h = (h ^ ((const uint8_t *)(p))[i_]) * 16777619u
	if (stat(w->card, &st))
		FOLD("-", 1);
	if (!(d = opendir(w->etc)))
		return h;
	while ((e = readdir(d))) {
		if (!wanted(e->d_name) || !join(w->from, w->etc, e->d_name) || stat(w->from, &st))
			continue;
		FOLD(e->d_name, strlen(e->d_name) + 1);
		FOLD(&st.st_size, sizeof(st.st_size));
		FOLD(&st.st_mtime, sizeof(st.st_mtime));
	}
#undef FOLD
	closedir(d);
	return h;
}

/*
 * The card's copy made the same as /etc: what differs is copied, what /etc
 * no longer has is deleted. Not from a / with no user, which is a fresh
 * one: the copy is what it should have been restored from. The number of
 * files written, or an error.
 */
static int save(struct work *w)
{
	struct dirent *e;
	struct stat st;
	int copied = 0, err = 0, e2;
	DIR *d;

	if (!ready || !user_configured() || !paths(w))
		return 0;
	if (mkdir(w->card, 0777) && errno != EEXIST)
		return -errno;
	if (!(d = opendir(w->etc)))
		return -errno;
	while ((e = readdir(d))) {
		if (!wanted(e->d_name) || !join(w->from, w->etc, e->d_name) ||
		    !join(w->to, w->card, e->d_name) || stat(w->from, &st) || !S_ISREG(st.st_mode) ||
		    same(w))
			continue;
		if ((e2 = copy(w)))
			err = e2;
		else
			copied++;
	}
	closedir(d);
	if (!(d = opendir(w->card)))
		return err ? err : copied;
	while ((e = readdir(d))) {
		if (!join(w->to, w->card, e->d_name) || !join(w->from, w->etc, e->d_name) ||
		    stat(w->to, &st) || !S_ISREG(st.st_mode))
			continue;
		if (!wanted(e->d_name) || stat(w->from, &st))
			unlink(w->to);
	}
	closedir(d);
	return err ? err : copied;
}

/* Under the lock: the copy brought up to date if /etc has changed since, or `always`. */
static int update(bool always)
{
	uint32_t now;
	int n;

	if (!ready || !paths(work)) {
		card_has = false;
		return 0;
	}
	now = look(work);
	if (!always && card_has && now == seen)
		return 0;
	n = save(work);
	if (n >= 0) {
		seen = now;
		card_has = true;
		last_err = 0;
	} else if (n != last_err) {
		last_err = n;
		klog("etc: the card's copy of /etc could not be written (%s)", strerror(-n));
	}
	return n;
}

static void etc_task(void *unused)
{
	bool card = false;
	int waited = 0;

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(POLL_S * 1000));
		if (!mount_is_point(CARD)) {
			card = false;
			continue;
		}
		/* a card that has just gone in at once, then every CHECK_S */
		if (card && (waited += POLL_S) < CHECK_S)
			continue;
		card = true;
		waited = 0;
		/* busy means `sync` or the card coming out: it is being done anyway */
		if (xSemaphoreTake(lock, 0) != pdTRUE)
			continue;
		update(false);
		xSemaphoreGive(lock);
	}
}

/*
 * At boot, once the card is mounted. A / with no user is a fresh one: what
 * it lacks is taken from the card's copy, if the card has one, and the
 * number of files taken is returned for the system to start again with
 * them. Until this has run nothing goes to the card, which would get the
 * fresh /etc in place of the copy it should be giving.
 */
int etc_init(void)
{
	struct dirent *e;
	struct stat st;
	int taken = 0;
	DIR *d;

	lock = xSemaphoreCreateMutex();
	work = heap_caps_malloc_prefer(sizeof(*work), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
	if (!lock || !work)
		return 0;
	if (!user_configured() && paths(work) && join(work->from, work->card, "user") &&
	    !stat(work->from, &st) && (d = opendir(work->card))) {
		mkdir(work->etc, 0777);
		while ((e = readdir(d))) {
			if (!wanted(e->d_name) || !join(work->from, work->card, e->d_name) ||
			    !join(work->to, work->etc, e->d_name) || stat(work->from, &st) ||
			    !S_ISREG(st.st_mode) || !stat(work->to, &st))
				continue;
			if (!copy(work))
				taken++;
		}
		closedir(d);
	}
	if (taken) {
		klog("etc: /etc taken back from the card's copy (%d file%s); starting again",
		     taken, taken == 1 ? "" : "s");
		return taken;
	}
	ready = true;
	ktask_create(etc_task, "ketc", 4096, NULL, 1, NULL, 0);
	return 0;
}

/* Now: for `sync`, and the card about to come out. Files written, or an error. */
int etc_save(void)
{
	int n;

	if (!lock || !work)
		return 0;
	xSemaphoreTake(lock, portMAX_DELAY);
	n = update(true);
	xSemaphoreGive(lock);
	return n;
}

/* Nothing written to the card from hold to release: it is being unmounted or formatted. */
void etc_hold(void)
{
	if (lock)
		xSemaphoreTake(lock, portMAX_DELAY);
}

void etc_release(void)
{
	if (lock)
		xSemaphoreGive(lock);
}

/* No more copies until the restart: /etc is about to be thrown away, and the card's with it. */
void etc_stop(void)
{
	etc_hold();
	ready = false;
	etc_release();
}
