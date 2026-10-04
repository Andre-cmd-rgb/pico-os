/*
 * CPU frequency policy.
 *
 * ESP-IDF's power management switches the clock between min and max: the
 * scheduler holds the CPU at max while any task on a core is running and
 * drops to min when both cores are idle. That is Linux's ondemand governor
 * in effect, decided at every idle, not by sampling. performance and
 * powersave pin min and max together.
 *
 * Idle sleep adds light sleep on top: when nothing needs the CPU and no driver
 * holds a lock (a transfer in progress, a USB keyboard, a PC on the native USB
 * console), the chip sleeps with RAM kept until the next timer or interrupt.
 */
#include <stdio.h>
#include <string.h>

#include "esp_private/esp_clk.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static int  cur_min = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
static int  cur_max = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
static bool cur_sleep;
static int  boosts;		/* programs that want the policy's top all the time */
static StaticSemaphore_t policy_storage;
static SemaphoreHandle_t policy_lock;

static void account(void);

#if CONFIG_PT_IDLE_SLEEP
#define IS_IDLE_SLEEP	true
#else
#define IS_IDLE_SLEEP	false
#endif

/*
 * The speeds the chip can be set to, slowest first. The ESP32-S3 runs at
 * 80, 160 or 240 MHz; below 80 its radio stops. The ESP32-P4 divides its
 * PLL by 1, 2 or 4: 400, 200 and 100 MHz, or 360, 180 and 90 on chips
 * before revision 3.
 */
#define FAST_MHZ	CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#if CONFIG_IDF_TARGET_ESP32P4
static const int speeds[] = { FAST_MHZ / 4, FAST_MHZ / 2, FAST_MHZ };
#else
static const int speeds[] = { 80, 160, 240 };
#endif
#define SLOW_MHZ	speeds[0]

static bool valid(int mhz)
{
	for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
		if (mhz == speeds[i])
			return true;
	}
	return false;
}

void cpufreq_speeds(int *slowest, int *middle, int *fastest)
{
	*slowest = speeds[0];
	*middle = speeds[1];
	*fastest = speeds[2];
}

static int apply(int min_mhz, int max_mhz, bool sleep)
{
	if (!valid(min_mhz) || !valid(max_mhz) || min_mhz > max_mhz)
		return -EINVAL;
#if CONFIG_PM_ENABLE
	const esp_pm_config_t cfg = {
		.max_freq_mhz = max_mhz,
		.min_freq_mhz = boosts ? max_mhz : min_mhz,
		.light_sleep_enable = sleep,
	};
	esp_err_t err;

	account();			/* the time so far, at the speeds it was at */
	err = esp_pm_configure(&cfg);
	if (err) {
		klog("cpufreq: %d-%d MHz%s rejected (%s)", min_mhz, max_mhz,
		     sleep ? " with idle sleep" : "", esp_err_to_name(err));
		return err == ESP_ERR_NOT_SUPPORTED ? -ENOTSUP : -EINVAL;
	}
	cur_min = min_mhz;
	cur_max = max_mhz;
	cur_sleep = sleep;
	return 0;
#else
	return min_mhz == cur_min && max_mhz == cur_max && sleep == cur_sleep ? 0 : -ENOTSUP;
#endif
}

int cpufreq_set(int min_mhz, int max_mhz)
{
	int err;

	xSemaphoreTake(policy_lock, portMAX_DELAY);
	err = apply(min_mhz, max_mhz, cur_sleep);
	xSemaphoreGive(policy_lock);
	return err;
}

/*
 * A program never idle long enough for the governor to be right about it
 * -- a clip, whose frame missed while the clock ramps up is a frame
 * missed -- asks for the policy's top speed for as long as it runs. It
 * was a save and a restore in the player: two clips at once, or the
 * policy changed with `cpufreq` while one played, came out wrong. Now it
 * is counted, and only the minimum is lifted: powersave stays at its 80.
 */
/* Policy and effective boost must reach the SDK together on both cores. */
static void boost_locked(bool on)
{
	boosts += on ? 1 : -1;
	if (boosts < 0)
		boosts = 0;
	apply(cur_min, cur_max, cur_sleep);
}

void cpufreq_boost(bool on)
{
	xSemaphoreTake(policy_lock, portMAX_DELAY);
	boost_locked(on);
	xSemaphoreGive(policy_lock);
}

bool cpufreq_try_boost(bool on)
{
	if (!xSemaphoreTake(policy_lock, 0))
		return false;
	boost_locked(on);
	xSemaphoreGive(policy_lock);
	return true;
}

int cpufreq_boosted(void)
{
	int n;

	xSemaphoreTake(policy_lock, portMAX_DELAY);
	n = boosts;
	xSemaphoreGive(policy_lock);
	return n;
}

int cpufreq_set_idle_sleep(bool on)
{
	int err;

	xSemaphoreTake(policy_lock, portMAX_DELAY);
	err = apply(cur_min, cur_max, on);

	if (!err) {
		serial_idle_sleep(on);
		klog("cpufreq: idle sleep %s", on ? "on" : "off");
	}
	xSemaphoreGive(policy_lock);
	return err;
}

bool cpufreq_idle_sleep(void)
{
	bool sleep;

	xSemaphoreTake(policy_lock, portMAX_DELAY);
	sleep = cur_sleep;
	xSemaphoreGive(policy_lock);
	return sleep;
}

/*
 * Asking "what frequency is it at?" from a command is a loaded question:
 * running the command is itself work, so the answer is always the top.
 * What matters is where the time went, which the power manager counts for
 * us -- this digs the per-frequency totals out of its report.
 */
#if CONFIG_PM_ENABLE && CONFIG_PM_PROFILING
/*
 * The power manager counts time per mode -- CPU_MAX, APB_MIN -- and a
 * mode's speed changes with the policy, so its report would put time
 * spent at 240 MHz under 80 after `cpufreq powersave`. What it has
 * counted since last time is added up here per speed, before every change
 * of policy and before every summary.
 */
#define MODES	8

static struct { int mhz; long long us; } at_speed[4];
static long long mode_seen[MODES];

static void account(void)
{
	char *dump = malloc(4096), *line;
	int mode = 0;

	if (!dump)
		return;
	if (cpufreq_stats(dump, 4096) < 0 || !(line = strstr(dump, "Mode stats:"))) {
		free(dump);
		return;
	}
	for (line = strchr(line, '\n'); line && mode < MODES; line = strchr(line, '\n')) {
		char name[16];
		long long us, delta;
		int mhz, i;

		line++;
		/* "APB_MIN   80 M   411580320   68%" -- the megahertz and
		 * its M are sometimes one token and sometimes two. */
		if (sscanf(line, "%15s %d %*[M] %lld", name, &mhz, &us) != 3 || !mhz)
			continue;
		delta = us - mode_seen[mode];
		mode_seen[mode++] = us;
		if (delta <= 0)
			continue;
		for (i = 0; i < 4 && at_speed[i].mhz && at_speed[i].mhz != mhz; i++)
			;
		if (i < 4) {
			at_speed[i].mhz = mhz;
			at_speed[i].us += delta;
		}
	}
	free(dump);
}
#else
static inline void account(void) { }
#endif

/*
 * Asking "what frequency is it at?" from a command is a loaded question:
 * running the command is itself work, so the answer is always the top.
 * What matters is where the time went, which the power manager counts for
 * us -- account() keeps it by speed.
 */
int cpufreq_time_summary(char *buf, size_t size)
{
#if CONFIG_PM_ENABLE && CONFIG_PM_PROFILING
	long long total = 0;
	size_t len = 0;

	if (!size)
		return -EINVAL;
	xSemaphoreTake(policy_lock, portMAX_DELAY);
	account();
	for (int i = 0; i < 4; i++)
		total += at_speed[i].us;
	if (!total) {
		xSemaphoreGive(policy_lock);
		return -ENOTSUP;
	}
	for (int i = 0; i < 4 && at_speed[i].mhz; i++) {
		int n = snprintf(buf + len, size - len, "%s%lld%% at %d MHz",
				 i ? ", " : "", at_speed[i].us * 100 / total, at_speed[i].mhz);

		if (n < 0 || (size_t)n >= size - len) {
			len = size - 1;
			break;
		}
		len += n;
	}
	xSemaphoreGive(policy_lock);
	return len;
#else
	return -ENOTSUP;
#endif
}

int cpufreq_stats(char *buf, size_t size)
{
	if (!size)
		return -EINVAL;
#if CONFIG_PM_ENABLE && CONFIG_PM_PROFILING
	FILE *f = fmemopen(buf, size, "w");

	if (!f)
		return -ENOMEM;
	esp_pm_dump_locks(f);
	fclose(f);
	buf[size - 1] = '\0';
	return strlen(buf);
#else
	return snprintf(buf, size, "power profiling is not built in (CONFIG_PM_PROFILING)\n");
#endif
}

void cpufreq_get(int *min_mhz, int *max_mhz)
{
	xSemaphoreTake(policy_lock, portMAX_DELAY);
	*min_mhz = cur_min;
	*max_mhz = cur_max;
	xSemaphoreGive(policy_lock);
}

int cpufreq_current_mhz(void)
{
	return esp_clk_cpu_freq() / 1000000;
}

const char *cpufreq_policy_name(int min_mhz, int max_mhz)
{
	if (min_mhz == FAST_MHZ && max_mhz == FAST_MHZ)
		return "performance";
	if (min_mhz == SLOW_MHZ && max_mhz == FAST_MHZ)
		return "ondemand";
	if (min_mhz == SLOW_MHZ && max_mhz == SLOW_MHZ)
		return "powersave";
	return "custom";
}

int cpufreq_init(void)
{
	bool sleep = IS_IDLE_SLEEP;

	policy_lock = xSemaphoreCreateMutexStatic(&policy_storage);
#if CONFIG_PT_CPUFREQ_PERFORMANCE
	int err = apply(FAST_MHZ, FAST_MHZ, sleep);
#elif CONFIG_PT_CPUFREQ_POWERSAVE
	int err = apply(SLOW_MHZ, SLOW_MHZ, sleep);
#else
	int err = apply(SLOW_MHZ, FAST_MHZ, sleep);
#endif
	if (!err) {
		if (sleep)
			serial_idle_sleep(true);
		klog("cpufreq: %s, %d-%d MHz, idle sleep %s", cpufreq_policy_name(cur_min, cur_max),
		     cur_min, cur_max, cur_sleep ? "on" : "off");
	}
	return err;
}
