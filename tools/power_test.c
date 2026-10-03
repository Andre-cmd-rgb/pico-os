#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#define CONFIG_PM_ENABLE 1
#define CONFIG_PM_PROFILING 1
#define CONFIG_PT_IDLE_SLEEP 0
#define CONFIG_PT_CPUFREQ_PERFORMANCE 0
#define CONFIG_PT_CPUFREQ_POWERSAVE 0
#define CONFIG_IDF_TARGET_ESP32P4 0
#define ESP_ERR_NOT_SUPPORTED 1
#define portMAX_DELAY -1
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef int esp_err_t;
typedef struct { int max_freq_mhz, min_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
static esp_pm_config_t configured;
static atomic_int keep;

static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *m)
{ assert(!pthread_mutex_init(m, NULL)); return m; }
static bool xSemaphoreTake(SemaphoreHandle_t m, int wait)
{ return !(wait ? pthread_mutex_lock(m) : pthread_mutex_trylock(m)); }
static void xSemaphoreGive(SemaphoreHandle_t m) { assert(!pthread_mutex_unlock(m)); }
static int esp_pm_configure(const esp_pm_config_t *c) { configured = *c; return 0; }
static const char *esp_err_to_name(int err) { (void)err; return "mock"; }
static void klog(const char *fmt, ...) { (void)fmt; }
static void serial_idle_sleep(bool on) { (void)on; }
static void power_activity(void) {}
static int esp_clk_cpu_freq(void) { return 240000000; }
static void esp_pm_dump_locks(FILE *f)
{
	/* Both frequencies accumulate so small summary buffers need truncation. */
	fputs("Mode stats:\nCPU_MAX 240 M 1000\nAPB_MIN 80 M 1000\n", f);
}
int cpufreq_stats(char *buf, size_t n);
const char *cpufreq_policy_name(int min, int max);
#include "power_under_test.h"

static void *churn(void *arg)
{
	(void)arg;
	for (int i = 0; i < 300; i++) {
		cpufreq_boost(true);
		power_keep_screen(true);
		int low, high;

		cpufreq_get(&low, &high);
		assert(low <= high);
		power_keep_screen(false);
		cpufreq_boost(false);
	}
	return NULL;
}

int main(void)
{
	pthread_t threads[4];
	char summary[128];

	assert(!cpufreq_init());
	assert(configured.min_freq_mhz == 80 && configured.max_freq_mhz == 240);
	assert(cpufreq_set(123, 240) == -EINVAL);
	assert(xSemaphoreTake(policy_lock, 0));
	assert(!cpufreq_try_boost(true) && !boosts);
	xSemaphoreGive(policy_lock);
	cpufreq_boost(true);
	assert(cpufreq_boosted() == 1 && configured.min_freq_mhz == 240);
	assert(!cpufreq_set(80, 80) && configured.min_freq_mhz == 80);
	assert(!cpufreq_set(80, 240) && configured.min_freq_mhz == 240);
	assert(cpufreq_try_boost(false) && configured.min_freq_mhz == 80);
	for (int i = 0; i < 4; i++)
		assert(!pthread_create(&threads[i], NULL, churn, NULL));
	for (int i = 0; i < 4; i++)
		assert(!pthread_join(threads[i], NULL));
	assert(!cpufreq_boosted() && !atomic_load(&keep));
	assert(configured.min_freq_mhz == 80 && configured.max_freq_mhz == 240);
	power_keep_screen(false); assert(!atomic_load(&keep));
	assert(cpufreq_time_summary(summary, sizeof(summary)) > 0);
	for (size_t n = 1; n < 40; n++) {
		char *small = malloc(n);
		int len = cpufreq_time_summary(small, n);

		assert(len >= 0 && (size_t)len < n && small[len] == '\0');
		free(small);
	}
	assert(cpufreq_time_summary(summary, 0) == -EINVAL);
	assert(cpufreq_stats(summary, 0) == -EINVAL);
	assert(!pthread_mutex_destroy(policy_lock));
	puts("power: concurrent policy/boost/screen holds, cleanup retry and bounded summaries passed");
	return 0;
}
