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
		.min_freq_mhz = min_mhz,
		.light_sleep_enable = sleep,
	};
	esp_err_t err = esp_pm_configure(&cfg);
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
	return apply(min_mhz, max_mhz, cur_sleep);
}

int cpufreq_set_idle_sleep(bool on)
{
	int err = apply(cur_min, cur_max, on);

	if (!err) {
		serial_idle_sleep(on);
		klog("cpufreq: idle sleep %s", on ? "on" : "off");
	}
	return err;
}

bool cpufreq_idle_sleep(void)
{
	return cur_sleep;
}

/*
 * Asking "what frequency is it at?" from a command is a loaded question:
 * running the command is itself work, so the answer is always the top.
 * What matters is where the time went, which the power manager counts for
 * us -- this digs the per-frequency totals out of its report.
 */
int cpufreq_time_summary(char *buf, size_t size)
{
#if CONFIG_PM_ENABLE && CONFIG_PM_PROFILING
	char *dump = malloc(4096), *line;
	struct { int mhz; long long us; } bucket[4] = { 0 };
	int n = 0;
	long long total = 0;
	size_t len = 0;

	if (!dump)
		return -ENOMEM;
	if (cpufreq_stats(dump, 4096) < 0) {
		free(dump);
		return -EIO;
	}
	line = strstr(dump, "Mode stats:");
	if (!line) {
		free(dump);
		return -ENOTSUP;
	}
	for (line = strchr(line, '\n'); line; line = strchr(line, '\n')) {
		char name[16];
		long long us;
		int mhz, i;

		line++;
		/* "APB_MIN   80 M   411580320   68%" -- the megahertz and
		 * its M are sometimes one token and sometimes two. */
		if (sscanf(line, "%15s %d %*[M] %lld", name, &mhz, &us) != 3 || !mhz)
			continue;
		for (i = 0; i < n && bucket[i].mhz != mhz; i++)
			;
		if (i == n && n < 4)
			bucket[n++].mhz = mhz;
		if (i < 4) {
			bucket[i].mhz = mhz;
			bucket[i].us += us;
		}
		total += us;
	}
	free(dump);
	if (!total)
		return -ENOTSUP;
	for (int i = 0; i < n; i++)
		len += snprintf(buf + len, size - len, "%s%lld%% at %d MHz",
				i ? ", " : "", bucket[i].us * 100 / total, bucket[i].mhz);
	return len;
#else
	return -ENOTSUP;
#endif
}

int cpufreq_stats(char *buf, size_t size)
{
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
	*min_mhz = cur_min;
	*max_mhz = cur_max;
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
