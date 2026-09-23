/*
 * The clock across power cuts.
 *
 * The chip keeps time through deep sleep and restarts, and the network
 * sets it whenever Wi-Fi comes up (drivers/net/wifi.c), but a power cut or
 * the reset button starts it again at 1970. So, as Linux's fake-hwclock
 * does, the time is written to /etc/clock now and then, and a boot that
 * finds its clock behind that puts it forward: late by however long the
 * power was off, but in the right year, and file times stay in order.
 *
 * The saves happen on the reaper's task, once a second at most, rather
 * than where the time changes: the network sets it from lwIP's own task,
 * which must not wait for the flash.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "pt/kernel.h"

#define CLOCK_FILE	"/etc/clock"
#define SAVE_EVERY_S	3600
#define PLAUSIBLE	1704067200	/* 2024: anything earlier was never set */

static atomic_bool	changed;	/* set from outside: save it soon */
static atomic_bool	from_network;	/* and say where it came from */
static time_t		last_save;

static const char *file(char *buf, size_t size)
{
	return mount_resolve(CLOCK_FILE, buf, size) ? buf : NULL;
}

void clock_save(void)
{
	char path[64], when[32];
	time_t now = time(NULL);
	struct tm tm;
	FILE *f;

	if (now < PLAUSIBLE || !file(path, sizeof(path)) || !(f = fopen(path, "w")))
		return;
	gmtime_r(&now, &tm);
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", &tm);
	fprintf(f, "%lld %s\n", (long long)now, when);
	fclose(f);
	last_save = now;
}

void clock_restore(void)
{
	char path[64], line[64], when[32];
	struct timeval tv = { 0 };
	FILE *f;

	if (!file(path, sizeof(path)) || !(f = fopen(path, "r")))
		return;
	if (fgets(line, sizeof(line), f))
		tv.tv_sec = (time_t)strtoll(line, NULL, 10);
	fclose(f);
	if (tv.tv_sec < PLAUSIBLE || tv.tv_sec <= time(NULL))
		return;			/* the chip kept better time than that */
	settimeofday(&tv, NULL);
	last_save = tv.tv_sec;
	klog("clock: restored to %.24s, the last time it was saved", ctime_r(&tv.tv_sec, when));
}

void clock_changed(bool network)
{
	if (network)
		atomic_store(&from_network, true);
	atomic_store(&changed, true);
}

void clock_tick(void)
{
	time_t now = time(NULL);
	char when[32];

	if (atomic_exchange(&from_network, false))
		klog("clock: set from the network, %.24s", ctime_r(&now, when));
	if (atomic_exchange(&changed, false) || (now >= PLAUSIBLE && now - last_save >= SAVE_EVERY_S))
		clock_save();
}
