/*
 * The clock across power cuts and sleeps.
 *
 * The chip keeps time through deep sleep and restarts, and the network
 * sets it whenever Wi-Fi comes up (drivers/net/wifi.c) and every hour
 * after, but a power cut or the reset button starts it again at 1970.
 * So, as Linux's fake-hwclock does, the time is written to /etc/clock now
 * and then, and a boot that finds its clock behind that puts it forward:
 * late by however long the power was off, but in the right year, and
 * file times stay in order.
 *
 * Deep sleep keeps time on a slow RC oscillator, which runs fast or slow
 * by up to a few percent as the chip cools: a night's sleep could come
 * back minutes out. The oscillator is calibrated against the crystal just
 * before each sleep, which leaves only how far it wanders during one, and
 * that much is learned, like adjtimex's drift file. The moment of going
 * to sleep is kept in RTC memory; on waking, the sleep as the oscillator
 * counted it is shrunk or stretched by what it has been seen to do; and
 * when the network next gives the time, what this sleep got wrong is
 * folded into what is known. Only a sleep that began with the clock just
 * set from the network teaches anything, so an error from before the
 * sleep is not blamed on it.
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

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "pt/kernel.h"

#define CLOCK_FILE	"/etc/clock"
#define SAVE_EVERY_S	3600
#define PLAUSIBLE	1704067200	/* 2024: anything earlier was never set */
#define LEARN_MIN_S	1800		/* shorter sleeps say too little */
#define DRIFT_MAX_PPM	100000		/* ten percent: beyond that, something else broke */
#define SLEEP_MAGIC	0x534c5031	/* "SLP1" */

static atomic_bool	changed;	/* set from outside: save it soon */
static atomic_bool	from_network;	/* and say where it came from */
static bool		synced;		/* set from the network since waking */
static time_t		last_save;
static int		drift_ppm;	/* how fast the sleep's clock runs */
static bool		drift_known;

/* The last sleep, as it began, and what waking made of it. */
static RTC_NOINIT_ATTR struct {
	uint32_t magic;
	int64_t	 at_us;		/* since the epoch */
	bool	 synced;	/* the clock was right when it began */
} sleep_mark;

static struct {
	int64_t	counted_us;	/* the sleep as the RTC counted it */
	int64_t	woke_timer;	/* esp_timer on waking; 0 when nothing to learn */
	int64_t	at_us;		/* when it began */
} woke;

static int64_t now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void set_us(int64_t us)
{
	struct timeval tv = { .tv_sec = us / 1000000, .tv_usec = us % 1000000 };

	settimeofday(&tv, NULL);
}

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
	if (drift_known)
		fprintf(f, "drift %d ppm while asleep\n", drift_ppm);
	fclose(f);
	last_save = now;
}

void clock_sleeping(void)
{
	sleep_mark.magic = SLEEP_MAGIC;
	sleep_mark.at_us = now_us();
	sleep_mark.synced = synced;
}

uint64_t clock_sleep_us(uint64_t us)
{
	/* the RTC counts (1 + drift) of its microseconds to each real one */
	return drift_known ? us + (int64_t)us * drift_ppm / 1000000 : us;
}

/* On waking: the sleep as counted, put right by what is known of it. */
static void woke_up(void)
{
	char when[32];
	int64_t counted = now_us() - sleep_mark.at_us, fix;
	time_t t;

	if (counted <= 0)
		return;
	if (sleep_mark.synced) {
		woke.counted_us = counted;
		woke.woke_timer = esp_timer_get_time();
		woke.at_us = sleep_mark.at_us;
	}
	if (!drift_known)
		return;
	fix = counted - counted * 1000000 / (1000000 + drift_ppm);
	set_us(now_us() - fix);
	t = time(NULL);
	klog("clock: %lld s asleep, the sleep's clock %+d ppm: %.24s",
	     (long long)(counted / 1000000), drift_ppm, ctime_r(&t, when));
}

void clock_restore(void)
{
	char path[64], line[64], when[32];
	struct timeval tv = { 0 };
	FILE *f;
	int ppm;

	if (file(path, sizeof(path)) && (f = fopen(path, "r"))) {
		if (fgets(line, sizeof(line), f))
			tv.tv_sec = (time_t)strtoll(line, NULL, 10);
		if (fgets(line, sizeof(line), f) && sscanf(line, "drift %d", &ppm) == 1 &&
		    abs(ppm) < DRIFT_MAX_PPM) {
			drift_ppm = ppm;
			drift_known = true;
		}
		fclose(f);
	}
	if (esp_reset_reason() == ESP_RST_DEEPSLEEP && sleep_mark.magic == SLEEP_MAGIC)
		woke_up();
	sleep_mark.magic = 0;
	if (tv.tv_sec < PLAUSIBLE || tv.tv_sec <= time(NULL))
		return;			/* the chip kept better time than that */
	settimeofday(&tv, NULL);
	last_save = tv.tv_sec;
	klog("clock: restored to %.24s, the last time it was saved", ctime_r(&tv.tv_sec, when));
}

/*
 * The network has just said what the time is: when the sleep ended, then,
 * is now less what esp_timer, on the crystal, has counted since; and
 * against that, what the RTC counted asleep says how fast it ran.
 */
static void learn(void)
{
	int64_t slept = now_us() - (esp_timer_get_time() - woke.woke_timer) - woke.at_us;
	int64_t ppm;

	woke.woke_timer = 0;
	if (slept < LEARN_MIN_S * 1000000LL)
		return;
	ppm = (woke.counted_us - slept) * 1000000 / slept;
	if (ppm <= -DRIFT_MAX_PPM || ppm >= DRIFT_MAX_PPM)
		return;
	drift_ppm = drift_known ? (drift_ppm + (int)ppm) / 2 : (int)ppm;
	drift_known = true;
	klog("clock: the sleep's clock ran %+lld ppm; now %+d ppm is allowed for", (long long)ppm,
	     drift_ppm);
	atomic_store(&changed, true);		/* to be saved */
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

	if (atomic_exchange(&from_network, false)) {
		synced = true;
		klog("clock: set from the network, %.24s", ctime_r(&now, when));
		if (woke.woke_timer)
			learn();
	}
	if (atomic_exchange(&changed, false) || (now >= PLAUSIBLE && now - last_save >= SAVE_EVERY_S))
		clock_save();
}
