/* Startup must either publish a complete driver or release its resources. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "es8311.h"
#include "levels.h"

#define CONFIG_PT_AUDIO 1
static int calls, fail_at, allocations;

static bool fails(void)
{
	return ++calls == fail_at;
}

static void *allocate(size_t bytes)
{
	void *p = malloc(bytes);

	assert(p);
	allocations++;
	return p;
}

static void release(void *p)
{
	if (p) {
		assert(allocations > 0);
		allocations--;
		free(p);
	}
}

static void klog(const char *fmt, ...)
{
	(void)fmt;
}

#ifdef TEST_CODEC

struct i2c_master_bus_t { int unused; };
typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
#define I2C_ADDR_BIT_LEN_7 0
static struct i2c_master_bus_t bus;
static bool bad_id;

static int i2c_bus_get(int sda, int scl, struct i2c_master_bus_t **out)
{
	assert(sda == 16 && scl == 15);
	if (fails())
		return -EIO;
	*out = &bus;
	return 0;
}

static int i2c_master_bus_add_device(i2c_master_bus_handle_t handle,
				     const i2c_device_config_t *cfg, i2c_master_dev_handle_t *out)
{
	assert(handle == &bus && cfg->device_address == 0x18);
	if (fails())
		return -EIO;
	*out = allocate(1);
	return 0;
}

static int i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
	release(handle);
	return 0;
}

static int i2c_master_transmit(i2c_master_dev_handle_t handle, const uint8_t *data,
			     size_t n, int timeout)
{
	assert(handle && data && n == 2 && timeout == 200);
	return fails() ? -EIO : 0;
}

static int i2c_master_transmit_receive(i2c_master_dev_handle_t handle, const uint8_t *reg,
				     size_t tx_n, uint8_t *data, size_t rx_n, int timeout)
{
	assert(handle && tx_n == 1 && rx_n == 1 && timeout == 200);
	if (fails())
		return -EIO;
	*data = *reg == 0xfd ? (bad_id ? 0 : 0x83) : *reg == 0xfe ? 0x11 : 0;
	return 0;
}

#include "codec_under_test.h"

int main(void)
{
	int startup_calls;

	assert(es8311_init(16, 15, 44100) == 0 && es8311_present());
	startup_calls = calls;
	es8311_deinit();
	assert(!es8311_present() && !allocations);
	for (fail_at = 1; fail_at <= startup_calls; fail_at++) {
		calls = 0;
		assert(es8311_init(16, 15, 44100) < 0);
		assert(!es8311_present() && !allocations);
	}
	fail_at = calls = 0;
	bad_id = true;
	assert(es8311_init(16, 15, 44100) == -ENODEV);
	assert(!es8311_present() && !allocations);
	bad_id = false;
	assert(es8311_init(16, 15, 44100) == 0);
	/* Every failed register read returns before deriving a register value. */
	fail_at = 0;
	calls = 0;
	assert(clock_config(44100) == 0);
	startup_calls = calls;
	for (fail_at = 1; fail_at <= startup_calls; fail_at++) {
		calls = 0;
		assert(clock_config(44100) == -EIO);
		if (fail_at == 3 || fail_at == 5 || fail_at == 7 || fail_at == 10)
			assert(calls == fail_at);
	}
	/* ALC setup during audio_init must also report every failed write. */
	for (int on = 0; on <= 1; on++)
		for (fail_at = 1; fail_at <= (on ? 3 : 2); fail_at++) {
			calls = 0;
			assert(es8311_set_alc(on, 18) == -EIO);
		}
	fail_at = 0;
	es8311_deinit();
	es8311_deinit();
	assert(!allocations);
	puts("audio codec: every startup bus failure cleans up; failed reads stop clock programming");
	return 0;
}

#else

#define CONFIG_PT_AUDIO_DIN 6
#define CONFIG_PT_AUDIO_AMP_EN 1
#define CONFIG_PT_AUDIO_I2C_SDA 16
#define CONFIG_PT_AUDIO_I2C_SCL 15
#define CONFIG_PT_AUDIO_MIC_ALC 1
#define CONFIG_PT_AUDIO_MIC_ALC_MAX_DB 18
#define CONFIG_PT_AUDIO_JACK_DETECT 2
#define MALLOC_CAP_SPIRAM 2
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 0
#define I2S_CHANNEL_DEFAULT_CONFIG(a, b) ((i2s_chan_config_t){0})
#define GPIO_MODE_OUTPUT 1
#define ESP_LOG_ERROR 1
#define BLOCK 160
#define DMA_BUFS 4
#define pdPASS 1
typedef void *i2s_chan_handle_t;
typedef struct { bool auto_clear; int dma_desc_num, dma_frame_num; } i2s_chan_config_t;
typedef struct { int unused; } i2s_std_config_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
enum audio_out { AUDIO_OUT_AUTO, AUDIO_OUT_SPEAKER, AUDIO_OUT_JACK };
static i2s_chan_handle_t tx, rx;
static SemaphoreHandle_t lock;
static TaskHandle_t mixer;
static int32_t *mixer_acc;
static int16_t *mixer_wire;
static bool ready, codec_on, codec_live;
static int rate = 44100, volume = 60, mic_gain = 24, registered, task_created;
#if CONFIG_PT_AUDIO_JACK
static bool jack_failed;
static enum audio_out out_wanted;
#endif

static i2s_std_config_t std_config(void) { return (i2s_std_config_t){0}; }
static void *xSemaphoreCreateMutex(void) { return fails() ? NULL : allocate(1); }
static void vSemaphoreDelete(void *p) { release(p); }
static int kmem_caps(void) { return MALLOC_CAP_SPIRAM; }
static void *heap_caps_malloc(size_t n, int caps)
{
	assert(caps == MALLOC_CAP_SPIRAM);
	return fails() ? NULL : allocate(n);
}
static void heap_caps_free(void *p) { release(p); }
static void esp_log_level_set(const char *name, int level) { (void)name; (void)level; }
static void gpio_hold_dis(int pin) { (void)pin; }
static void gpio_set_direction(int pin, int mode) { (void)pin; (void)mode; }
static void gpio_sleep_sel_dis(int pin) { (void)pin; }
static void amp(bool on) { assert(!on); }
static int i2s_new_channel(const i2s_chan_config_t *cfg, void **out_tx, void **out_rx)
{
	assert(cfg->auto_clear && cfg->dma_desc_num == 4 && cfg->dma_frame_num == 160);
	if (fails())
		return -EIO;
	*out_tx = allocate(1);
	if (out_rx)
		*out_rx = allocate(1);
	return 0;
}
static int i2s_channel_init_std_mode(void *channel, const i2s_std_config_t *cfg)
{
	assert(channel && cfg);
	return fails() ? -EIO : 0;
}
static void i2s_del_channel(void *channel) { release(channel); }
int es8311_init(int sda, int scl, int hz)
{
	assert(sda == 16 && scl == 15 && hz == 44100);
	if (fails())
		return -EIO;
	codec_live = true;
	return 0;
}
void es8311_deinit(void) { codec_live = false; }
int es8311_set_volume(int percent) { assert(percent == volume); return fails() ? -EIO : 0; }
int es8311_set_mic_gain(int db) { assert(db == mic_gain); return fails() ? -EIO : 0; }
static int audio_set_mic_alc(bool on, int max)
{
	assert(on && max == 18);
	return fails() ? -EIO : 0;
}
#if CONFIG_PT_AUDIO_JACK
static void jack_init(void) { }
static int jack_percent = 40;
static void jack_volume(int percent) { assert(percent == jack_percent); }
static void xTaskNotifyGive(void *task) { assert(ready && task == mixer && task_created); }
#endif
static void mixer_task(void *arg) { (void)arg; }
static int ktask_create(void (*fn)(void *), const char *name, int stack, void *arg,
			int priority, void **out, int core)
{
	assert(fn == mixer_task && !strcmp(name, "kaudio") && stack == 3072 && !arg);
	assert(priority == 18 && core == 0);
	assert(!ready && lock && tx && rx && codec_live && codec_on && mixer_acc && mixer_wire);
	if (fails())
		return 0;
	task_created = 1;
	*out = &task_created;
	return pdPASS;
}
static void audio_dev_register(void) { assert(ready && task_created); registered++; }
bool audio_has_jack(void);

#include "audio_init_under_test.h"

int main(void)
{
	int startup_calls;

	assert(!audio_present() && !audio_has_jack());
#if CONFIG_PT_AUDIO_JACK
	assert(audio_set_output(AUDIO_OUT_JACK) == -ENODEV);
#endif
	assert(audio_init() == 0 && audio_present() && registered == 1);
	startup_calls = calls;
	assert(audio_init() == 0 && calls == startup_calls && registered == 1);
	/* The host task does not run; release its successful startup for the next case. */
	ready = false;
	task_created = registered = 0;
	es8311_deinit();
	i2s_del_channel(tx);
	i2s_del_channel(rx);
	heap_caps_free(mixer_acc);
	heap_caps_free(mixer_wire);
	vSemaphoreDelete(lock);
	tx = rx = lock = mixer = NULL;
	mixer_acc = NULL;
	mixer_wire = NULL;
	for (fail_at = 1; fail_at <= startup_calls; fail_at++) {
		calls = 0;
		assert(audio_init() < 0);
		assert(!audio_present() && !audio_has_jack() && !registered && !task_created);
		assert(!allocations && !codec_live && !tx && !rx && !lock && !mixer);
		assert(!mixer_acc && !mixer_wire);
#if CONFIG_PT_AUDIO_JACK
		assert(audio_set_output(AUDIO_OUT_JACK) == -ENODEV);
#endif
	}
	puts("audio startup: allocation, I2S, codec and task failures leave no published driver or resources");
	return 0;
}
#endif
