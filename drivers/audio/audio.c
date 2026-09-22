/*
 * Sound: the I2S link to the codec, and /dev/audio.
 *
 * The device is raw 16-bit mono at the configured sample rate, the way
 * /dev/dsp was: write to it and the speaker plays, read from it and the
 * microphone records. The frame on the wire is stereo because the codec
 * expects two slots, so mono samples are doubled on the way out and every
 * second sample is taken on the way in.
 *
 * The amplifier is only switched on while something is playing: it hisses
 * softly otherwise, and it is the biggest draw on the battery here.
 */
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "es8311.h"

#if CONFIG_PT_AUDIO

#define CHUNK		1024		/* bytes converted at a time */
#define MCLK_MULTIPLE	256		/* what es8311.c's clock table assumes */

static i2s_chan_handle_t	tx, rx;
static SemaphoreHandle_t	lock;
static uint8_t			*scratch;
static int			rate = CONFIG_PT_AUDIO_RATE;
static int			volume = CONFIG_PT_AUDIO_VOLUME;
/*
 * Both directions share one I2S controller, and the receiver takes its
 * clock from the transmitter -- that is what "rx switched from master to
 * slave for full-duplex" means at start-up. So recording needs the
 * transmitter running even when nothing is being played: without it the
 * microphone never gets a clock and every read times out.
 */
static bool			tx_on, rx_on, amp_on, alc_on;

static void audio_dev_register(void);

/*
 * The pin is a shutdown input on most amplifiers, not an enable: the
 * FM8002E on the Freenove board plays when it is low and draws 4 uA when
 * it is high, and it has a pull-up so it stays quiet until we say so.
 */
static void amp(bool on)
{
#if CONFIG_PT_AUDIO_AMP_EN >= 0
#ifdef CONFIG_PT_AUDIO_AMP_ACTIVE_LOW
	gpio_set_level(CONFIG_PT_AUDIO_AMP_EN, !on);
#else
	gpio_set_level(CONFIG_PT_AUDIO_AMP_EN, on);
#endif
#endif
}

static i2s_std_config_t std_config(void)
{
	i2s_std_config_t cfg = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
							       I2S_SLOT_MODE_STEREO),
		.gpio_cfg = {
			.mclk = CONFIG_PT_AUDIO_MCLK,
			.bclk = CONFIG_PT_AUDIO_BCLK,
			.ws = CONFIG_PT_AUDIO_WS,
			.dout = CONFIG_PT_AUDIO_DOUT,
			.din = CONFIG_PT_AUDIO_DIN,
		},
	};

	cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;
	return cfg;
}

int audio_init(void)
{
	i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	i2s_std_config_t std = std_config();
	bool duplex = CONFIG_PT_AUDIO_DIN >= 0;

	lock = xSemaphoreCreateMutex();
	scratch = malloc(CHUNK * 2);
	if (!lock || !scratch)
		return -ENOMEM;
	chan_cfg.auto_clear = true;	/* silence, not the last buffer again */
	/* "rx switched from master to slave for full-duplex" is a warning
	 * about something we asked for: both directions share one clock. */
	esp_log_level_set("i2s_common", ESP_LOG_ERROR);
	if (i2s_new_channel(&chan_cfg, &tx, duplex ? &rx : NULL)) {
		klog("audio: no free I2S controller");
		return -EIO;
	}
	if (i2s_channel_init_std_mode(tx, &std) || (rx && i2s_channel_init_std_mode(rx, &std))) {
		klog("audio: I2S setup failed");
		return -EIO;
	}
#if CONFIG_PT_AUDIO_AMP_EN >= 0
	gpio_set_direction(CONFIG_PT_AUDIO_AMP_EN, GPIO_MODE_OUTPUT);
	amp(false);
#endif
	if (es8311_init(CONFIG_PT_AUDIO_I2C_SDA, CONFIG_PT_AUDIO_I2C_SCL, rate)) {
		i2s_del_channel(tx);		/* give the pins back */
		if (rx)
			i2s_del_channel(rx);
		tx = rx = NULL;
		return -ENODEV;
	}
	es8311_set_volume(volume);
	es8311_set_mic_gain(CONFIG_PT_AUDIO_MIC_GAIN);
#ifdef CONFIG_PT_AUDIO_MIC_ALC
	audio_set_mic_alc(true, CONFIG_PT_AUDIO_MIC_ALC_MAX_DB);
#endif
	audio_dev_register();
	klog("audio: %d Hz mono, %s", rate, duplex ? "speaker and microphone" : "speaker only");
	return 0;
}

bool audio_present(void)
{
	return es8311_present();
}

static int clock_on(void)
{
	if (tx_on)
		return 0;
	if (i2s_channel_enable(tx))
		return -EIO;
	tx_on = true;
	return 0;
}

static void stop_locked(void)
{
	if (amp_on) {
		/* let the DMA buffers empty, or the last word is cut off */
		vTaskDelay(pdMS_TO_TICKS(1440 * 1000 / rate + 20));
		amp(false);
		amp_on = false;
	}
	if (rx_on) {
		i2s_channel_disable(rx);
		rx_on = false;
	}
	if (tx_on) {
		i2s_channel_disable(tx);
		tx_on = false;
	}
}

void audio_stop(void)
{
	if (!audio_present())
		return;
	xSemaphoreTake(lock, portMAX_DELAY);
	stop_locked();
	xSemaphoreGive(lock);
}

int audio_set_rate(int hz)
{
	i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);
	int ret = 0;

	if (!audio_present())
		return -ENODEV;
	if (hz < 8000 || hz > 48000)
		return -EINVAL;
	clk.mclk_multiple = MCLK_MULTIPLE;
	xSemaphoreTake(lock, portMAX_DELAY);
	stop_locked();
	if (i2s_channel_reconfig_std_clock(tx, &clk) ||
	    (rx && i2s_channel_reconfig_std_clock(rx, &clk)))
		ret = -EIO;
	else if (!(ret = es8311_set_rate(hz)))
		rate = hz;
	xSemaphoreGive(lock);
	return ret;
}

int audio_rate(void)
{
	return rate;
}

int audio_set_volume(int percent)
{
	int ret;

	if (percent < 0 || percent > 100)
		return -EINVAL;
	if ((ret = es8311_set_volume(percent)))
		return ret;
	volume = percent;
	return 0;
}

int audio_volume(void)
{
	return volume;
}

int audio_set_mic_gain(int db)
{
	return es8311_set_mic_gain(db);
}

int audio_set_mic_alc(bool on, int max_db)
{
	alc_on = on;
	return es8311_set_alc(on, max_db);
}

bool audio_mic_alc(void)
{
	return alc_on;
}

/*
 * Play 16-bit samples: `channels` says whether the buffer is already stereo.
 * Returns the bytes taken from the buffer, not the bytes put on the wire.
 */
ssize_t audio_write(const void *pcm, size_t bytes, int channels)
{
	const uint8_t *in = pcm;
	size_t done = 0;

	if (!audio_present())
		return -ENODEV;
	if (channels != 1 && channels != 2)
		return -EINVAL;
	if (bytes < 2)
		return 0;
	xSemaphoreTake(lock, portMAX_DELAY);
	if (clock_on()) {
		xSemaphoreGive(lock);
		return -EIO;
	}
	if (!amp_on) {
		amp(true);
		amp_on = true;
	}
	while (done + 2 <= bytes) {
		size_t n = bytes - done, wrote = 0;
		const void *buf;

		if (channels == 2) {
			buf = in + done;
			if (n > CHUNK * 2)
				n = CHUNK * 2;
			n &= ~3u;		/* whole stereo frames */
			if (!n)
				break;
		} else {
			const int16_t *src = (const int16_t *)(in + done);
			int16_t *dst = (int16_t *)scratch;

			if (n > CHUNK)
				n = CHUNK;
			n &= ~1u;
			for (size_t i = 0; i < n / 2; i++)
				dst[2 * i] = dst[2 * i + 1] = src[i];
			buf = scratch;
		}
		if (i2s_channel_write(tx, buf, channels == 2 ? n : n * 2, &wrote, portMAX_DELAY)) {
			xSemaphoreGive(lock);
			return done ? (ssize_t)done : -EIO;
		}
		done += channels == 2 ? wrote : wrote / 2;
	}
	xSemaphoreGive(lock);
	return done;
}

/* Record 16-bit mono samples. */
ssize_t audio_read(void *pcm, size_t bytes)
{
	int16_t *out = pcm;
	size_t done = 0;

	if (!audio_present())
		return -ENODEV;
	if (!rx)
		return -ENODEV;
	xSemaphoreTake(lock, portMAX_DELAY);
	if (!rx_on) {
		/* the transmitter makes the clock the microphone is read with */
		if (clock_on() || i2s_channel_enable(rx)) {
			xSemaphoreGive(lock);
			return -EIO;
		}
		rx_on = true;
	}
	while (done + 2 <= bytes) {
		size_t want = (bytes - done) * 2, got = 0;
		const int16_t *src = (const int16_t *)scratch;

		if (want > CHUNK * 2)
			want = CHUNK * 2;
		want &= ~3u;
		if (i2s_channel_read(rx, scratch, want, &got, pdMS_TO_TICKS(2000))) {
			xSemaphoreGive(lock);
			return done ? (ssize_t)done : -EIO;
		}
		for (size_t i = 0; i < got / 4; i++)	/* the codec speaks on the left slot */
			out[done / 2 + i] = src[2 * i];
		done += got / 2;
	}
	xSemaphoreGive(lock);
	return done;
}

/* ------------------------------------------------------------ /dev/audio */

static ssize_t audio_dev_read(struct pt_file *f, void *buf, size_t n)
{
	return audio_read(buf, n);
}

static ssize_t audio_dev_write(struct pt_file *f, const void *buf, size_t n)
{
	return audio_write(buf, n, 1);
}

static void audio_dev_release(struct pt_file *f)
{
	audio_stop();
}

static const struct pt_file_ops audio_ops = {
	.read = audio_dev_read,
	.write = audio_dev_write,
	.release = audio_dev_release,
};

static void audio_dev_register(void)
{
	dev_register("audio", &audio_ops);
}

#else /* !CONFIG_PT_AUDIO */

int audio_init(void) { return -ENODEV; }
bool audio_present(void) { return false; }
void audio_stop(void) { }
int audio_set_rate(int hz) { return -ENODEV; }
int audio_rate(void) { return 0; }
int audio_set_volume(int percent) { return -ENODEV; }
int audio_volume(void) { return 0; }
int audio_set_mic_gain(int db) { return -ENODEV; }
int audio_set_mic_alc(bool on, int max_db) { return -ENODEV; }
bool audio_mic_alc(void) { return false; }
ssize_t audio_write(const void *pcm, size_t bytes, int channels) { return -ENODEV; }
ssize_t audio_read(void *pcm, size_t bytes) { return -ENODEV; }

#endif
