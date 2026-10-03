/*
 * Sound: the I2S link to the codec, a mixer, and /dev/audio.
 *
 * The device is raw 16-bit mono, the way /dev/dsp was: write to it and
 * the speaker plays, read from it and the microphone records. The frame
 * on the wire is stereo because the codec expects two slots, so mono
 * samples are doubled on the way out and every second sample is taken
 * on the way in. Programs that have stereo (music, clips) keep it: the
 * mix is stereo, folded to one for the speaker, whole for headphones.
 *
 * Several programs can play at once -- music on one terminal and a game
 * on another -- so nothing writes to the codec directly. Each writer has
 * a stream: a ring of its samples in PSRAM, at its own rate. A mixer task
 * on the kernel's core takes a block from every stream, brings each to
 * the rate on the wire (the highest any of them wants) by straight-line
 * interpolation, adds them up and hands the block to the I2S driver,
 * whose DMA paces it. A writer waits while its ring is full, so a program
 * that times itself by its sound (the NES, a clip) still runs at exactly
 * the rate it plays.
 *
 * How much a ring holds is the stream's latency: a second for music, so
 * a busy SD card -- a screenshot being written, say -- never runs the
 * speaker dry; a few frames for a game or a clip, whose sound has to keep
 * up with the picture.
 *
 * The amplifier is only switched on while something is playing: it hisses
 * softly otherwise, and it is the biggest draw on the battery here. The
 * codec goes into standby after a few seconds of silence.
 *
 * While an alarm rings the speaker is its task's: what anything else
 * plays goes nowhere, at the pace it would have played, so a song does
 * not garble the alarm and its player does not notice.
 *
 * A headphone jack, where there is one, is a DAC of its own (a PCM5102A)
 * on a second I2S controller, fed the same mix in stereo and 32 bits.
 * Sound goes to the jack or to the speaker, never both: to the jack while
 * a plug is in, if a switch in the socket says so, or wherever `volume
 * jack` or `volume speaker` sent it -- but an alarm always rings on the
 * speaker. The DAC has no volume of its own and plays at line level, so
 * the jack's volume is a multiplier here, applied to the 16-bit mix on
 * its way into the DAC's 32 bits so that turning it down loses nothing.
 * The jack and the speaker keep a volume each. Its clock stops with the
 * speaker's, and the DAC powers itself down when it does.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "es8311.h"
#include "levels.h"
#include "pt/kernel.h"

#if CONFIG_PT_AUDIO

#define MCLK_MULTIPLE	256		/* what es8311.c's clock table assumes */
#define BLOCK		160		/* frames the mixer makes at a time */
#define DMA_BUFS	4		/* the ring the DMA plays from */
#define MAX_STREAMS	4
#define LATENCY_MS	100		/* a stream's ring, unless it asks */
#define QUIET_BLOCKS	40		/* silence before the output stops: ~150 ms */
#define STANDBY_MS	5000		/* and before the codec sleeps */
#define ONE		65536		/* the resampler's fixed point */
#define JACK_VOLUME	40		/* headphones, until they are given one */

struct stream {
	TaskHandle_t	  task;		/* the writer */
	int		  pid;		/* its process, 0 for a kernel task */
	int		  rate;
	int		  latency_ms;
	int16_t		 *ring;		/* left and right, side by side */
	size_t		  size;		/* frames the ring holds */
	size_t		  head, tail;	/* written and read, counting up for ever */
	SemaphoreHandle_t space;	/* given as the mixer makes room */
	uint32_t	  phase;	/* between frames a and b, in 1/65536ths */
	int16_t		  a[2], b[2];
	bool		  primed;	/* a and b hold samples */
	bool		  closing;	/* play what is left, then go */
	bool		  used;
};

static i2s_chan_handle_t	tx, rx;
static SemaphoreHandle_t	lock;		/* the table, and the hardware */
static struct stream		streams[MAX_STREAMS];
static TaskHandle_t		mixer;
static int32_t			*mixer_acc;
static int16_t			*mixer_wire;
static bool			ready;
static int			rate = CONFIG_PT_AUDIO_RATE;	/* on the wire */
static int			volume = CONFIG_PT_AUDIO_VOLUME;
static int			mic_gain = CONFIG_PT_AUDIO_MIC_GAIN;
static int			alc_max;
/*
 * Both directions share one I2S controller, and the receiver takes its
 * clock from the transmitter -- that is what "rx switched from master to
 * slave for full-duplex" means at start-up. So recording needs the
 * transmitter running even when nothing is being played: without it the
 * microphone never gets a clock and every read times out.
 */
static volatile bool		tx_on, rx_on, amp_on, alc_on, codec_on = true;
static int			alc_held;	/* audio_mic_alc_hold()s */
static TaskHandle_t		owner;		/* an alarm ringing */
static int			tx_rate = CONFIG_PT_AUDIO_RATE;	/* the codec's clock */

#if CONFIG_PT_AUDIO_JACK
#define PLUG_BLOCKS	24		/* a plug seen for this long: ~100 ms or more */

static i2s_chan_handle_t	jack;		/* the headphone DAC's controller */
static bool			jack_failed;	/* no controller was left for it */
static volatile bool		jack_on;	/* its clock running */
static int			jack_rate;
static enum audio_out		out_wanted = AUDIO_OUT_AUTO;
static bool			to_jack;	/* where the last block went */
static bool			plugged;	/* the socket's switch, settled */
static int			plug_seen;	/* blocks it has said otherwise */
static int			jack_percent = JACK_VOLUME;
static int32_t			jack_gain;	/* jack_percent as a multiplier */
#endif

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
	amp_on = on;
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

bool audio_present(void)
{
	return ready;
}

/* Playing or recording: the transmitter's clock runs for both. */
bool audio_busy(void)
{
#if CONFIG_PT_AUDIO_JACK
	if (jack_on)
		return true;
#endif
	return tx_on;
}

/* ------------------------------------------------------------ the hardware */

/* The codec out of standby, told again what it had been told. Locked. */
static void codec_wake(void)
{
	if (codec_on)
		return;
	es8311_power(true);
	es8311_set_volume(volume);
	es8311_set_mic_gain(mic_gain);
	if (alc_on && !alc_held)
		es8311_set_alc(true, alc_max);
	codec_on = true;
}

/* The clock running at `hz`, changing it first if it is not. Locked. */
static int clock_at(int hz)
{
	i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);

	codec_wake();
	if (tx_on && hz == tx_rate)
		return 0;
	if (tx_on && rx_on)
		return 0;			/* a recording has the clock */
	if (tx_on) {
		i2s_channel_disable(tx);
		tx_on = false;
	}
	if (hz != tx_rate) {
		clk.mclk_multiple = MCLK_MULTIPLE;
		if (i2s_channel_reconfig_std_clock(tx, &clk) ||
		    (rx && i2s_channel_reconfig_std_clock(rx, &clk)) || es8311_set_rate(hz))
			return -EIO;
		tx_rate = hz;
	}
	if (i2s_channel_enable(tx))
		return -EIO;
	tx_on = true;
	return 0;
}

#if CONFIG_PT_AUDIO_JACK

/*
 * 32-bit samples, the PCM5102A's widest: the bit clock at 64 times the
 * rate, from which it makes its own system clock (its SCK pin tied low).
 */
static i2s_std_config_t jack_config(int hz)
{
	i2s_std_config_t cfg = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(hz),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
							       I2S_SLOT_MODE_STEREO),
		.gpio_cfg = {
			.mclk = I2S_GPIO_UNUSED,
			.bclk = CONFIG_PT_AUDIO_JACK_BCLK,
			.ws = CONFIG_PT_AUDIO_JACK_WS,
			.dout = CONFIG_PT_AUDIO_JACK_DOUT,
			.din = I2S_GPIO_UNUSED,
		},
	};

	return cfg;
}

static int jack_clock_at(int hz)
{
	i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);

	if (jack_on && hz == jack_rate)
		return 0;
	if (jack_on) {
		i2s_channel_disable(jack);
		jack_on = false;
	}
	if (hz != jack_rate) {
		if (i2s_channel_reconfig_std_clock(jack, &clk))
			return -EIO;
		jack_rate = hz;
	}
	if (i2s_channel_enable(jack))
		return -EIO;
	jack_on = true;
	return 0;
}

/* The jack's clock stopped, once what the DMA holds has played. Locked. */
static void jack_stop(void)
{
	if (!jack_on)
		return;
	vTaskDelay(pdMS_TO_TICKS(DMA_BUFS * BLOCK * 1000 / rate + 10));
	i2s_channel_disable(jack);
	jack_on = false;
}

/* The socket's switch now, if it has one. */
static bool plug_now(void)
{
#if CONFIG_PT_AUDIO_JACK_DETECT >= 0
#ifdef CONFIG_PT_AUDIO_JACK_DETECT_LOW
	return gpio_get_level(CONFIG_PT_AUDIO_JACK_DETECT) == 0;
#else
	/* a switch that opens from ground, or one that closes to 3.3 V */
	return gpio_get_level(CONFIG_PT_AUDIO_JACK_DETECT) == 1;
#endif
#else
	return false;
#endif
}

/*
 * Whether this block goes to the jack. The switch is believed once it
 * has said the same for PLUG_BLOCKS blocks, since a plug going in makes
 * and breaks the contact a few times; `fresh` takes it at its word, as
 * the output starts after a silence.
 */
static bool jack_wanted(bool fresh)
{
	bool now = plug_now();

	if (jack_failed)
		return false;
	if (fresh || now == plugged) {
		plugged = now;
		plug_seen = 0;
	} else if (++plug_seen >= PLUG_BLOCKS) {
		plugged = now;
		plug_seen = 0;
		klog("audio: headphones %s", now ? "in" : "out");
	}
	if (owner)
		return false;		/* an alarm: on the speaker, to be heard */
	if (out_wanted == AUDIO_OUT_JACK)
		return true;
	return out_wanted == AUDIO_OUT_AUTO && plugged;
}

/*
 * Its own controller, taken the first time sound goes to the jack: its
 * DMA buffers are internal RAM, which a machine whose jack is never
 * used should not give up. Locked. Without one, the speaker only.
 */
static int jack_open(void)
{
	i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
	i2s_std_config_t std = jack_config(rate);

	cfg.auto_clear = true;
	cfg.dma_desc_num = DMA_BUFS;
	cfg.dma_frame_num = BLOCK;
	/* "controller 0 has been occupied" is the search for a free one */
	esp_log_level_set("i2s_platform", ESP_LOG_ERROR);
	if (i2s_new_channel(&cfg, &jack, NULL) || i2s_channel_init_std_mode(jack, &std)) {
		klog("audio: no I2S controller left for the headphone jack");
		if (jack)
			i2s_del_channel(jack);
		jack = NULL;
		jack_failed = true;
		return -EIO;
	}
	jack_rate = rate;
	return 0;
}

/* The socket's switch, from the start: whether a plug is in is asked before a sound. */
static void jack_init(void)
{
#if CONFIG_PT_AUDIO_JACK_DETECT >= 0
	gpio_config_t io = {
		.pin_bit_mask = 1ULL << CONFIG_PT_AUDIO_JACK_DETECT,
		.mode = GPIO_MODE_INPUT,
#ifdef CONFIG_PT_AUDIO_JACK_DETECT_HIGH
		.pull_down_en = GPIO_PULLDOWN_ENABLE,
#else
		.pull_up_en = GPIO_PULLUP_ENABLE,
#endif
	};

	gpio_config(&io);
	plugged = plug_now();
#endif
}

static void jack_volume(int percent)
{
	jack_percent = percent;
	jack_gain = jack_gain_for(percent);
}

#endif /* CONFIG_PT_AUDIO_JACK */

/* Silence: the DMA played out, the amplifier off, the clock stopped. Locked. */
static void output_stop(void)
{
	if (amp_on) {
		/* let the DMA buffers empty, or the last word is cut off */
		vTaskDelay(pdMS_TO_TICKS(DMA_BUFS * BLOCK * 1000 / rate + 10));
		amp(false);
	}
	if (tx_on && !rx_on) {
		i2s_channel_disable(tx);
		tx_on = false;
	}
#if CONFIG_PT_AUDIO_JACK
	jack_stop();
#endif
}

/* ------------------------------------------------------------ streams */

static size_t filled(struct stream *s)
{
	return __atomic_load_n(&s->head, __ATOMIC_ACQUIRE) -
	       __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE);
}

static void drop_queued(struct stream *s)
{
	__atomic_store_n(&s->tail, __atomic_load_n(&s->head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

/* The task's stream, one that is not on its way out. Locked. */
static struct stream *find(TaskHandle_t task)
{
	for (int i = 0; i < MAX_STREAMS; i++)
		if (streams[i].used && streams[i].task == task && !streams[i].closing)
			return &streams[i];
	return NULL;
}

static void stream_free(struct stream *s)
{
	heap_caps_free(s->ring);
	if (s->space)
		vSemaphoreDelete(s->space);
	memset(s, 0, sizeof(*s));
}

/* The ring for a rate and a latency; what was queued goes. Locked. */
static int stream_size(struct stream *s, int hz, int latency_ms)
{
	size_t size = (size_t)hz * latency_ms / 1000;
	int16_t *ring;

	if (size < BLOCK * 2)
		size = BLOCK * 2;
	if (s->ring && size == s->size && hz == s->rate)
		return 0;
	ring = heap_caps_malloc(size * 2 * sizeof(*ring), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!ring)
		ring = heap_caps_malloc(size * 2 * sizeof(*ring), MALLOC_CAP_8BIT);
	if (!ring)
		return -ENOMEM;
	heap_caps_free(s->ring);
	s->ring = ring;
	s->size = size;
	s->rate = hz;
	s->latency_ms = latency_ms;
	s->head = s->tail = 0;
	s->primed = false;
	s->phase = 0;
	return 0;
}

/* The calling task's stream, made if it has none. Locked. */
static struct stream *mine(void)
{
	TaskHandle_t self = xTaskGetCurrentTaskHandle();
	struct stream *s = find(self);
	struct proc *p = proc_current();

	if (s)
		return s;
	for (int i = 0; i < MAX_STREAMS && !s; i++)
		if (!streams[i].used)
			s = &streams[i];
	if (!s)
		return NULL;
	memset(s, 0, sizeof(*s));
	s->space = xSemaphoreCreateBinary();
	if (!s->space || stream_size(s, rate, LATENCY_MS)) {
		stream_free(s);
		return NULL;
	}
	s->task = self;
	s->pid = p ? p->pid : 0;
	s->used = true;
	return s;
}

/*
 * The stream's next frame at the rate on the wire, or false if it has
 * none just now. Two of its own frames are kept, and the output walks
 * from one to the next by its rate over the wire's.
 */
static bool pull(struct stream *s, uint32_t step, int out[2])
{
	while (!s->primed || s->phase >= ONE) {
		size_t tail = __atomic_load_n(&s->tail, __ATOMIC_RELAXED);
		const int16_t *f;

		if (tail == __atomic_load_n(&s->head, __ATOMIC_ACQUIRE))
			return false;
		f = &s->ring[tail % s->size * 2];
		for (int c = 0; c < 2; c++) {
			s->a[c] = s->primed ? s->b[c] : f[c];
			s->b[c] = f[c];
		}
		__atomic_store_n(&s->tail, tail + 1, __ATOMIC_RELEASE);
		if (s->primed)
			s->phase -= ONE;
		s->primed = true;
	}
	for (int c = 0; c < 2; c++)
		out[c] = s->a[c] + (int)(((int64_t)(s->b[c] - s->a[c]) * s->phase) >> 16);
	s->phase += step;
	return true;
}

/* ------------------------------------------------------------ the mixer */

/* The rate on the wire: the highest a stream with something queued wants. */
static int wanted_rate(void)
{
	int hz = 0;

	for (int i = 0; i < MAX_STREAMS; i++)
		if (streams[i].used && filled(&streams[i]) && streams[i].rate > hz)
			hz = streams[i].rate;
	return hz;
}

/* Streams played out after their writer stopped, or whose writer is gone. Locked. */
static void reap(bool check_procs)
{
	for (int i = 0; i < MAX_STREAMS; i++) {
		struct stream *s = &streams[i];

		if (!s->used)
			continue;
		if ((s->closing && !filled(s)) || (check_procs && s->pid && !proc_alive(s->pid)))
			stream_free(s);
	}
}

static void mixer_task(void *arg)
{
	int32_t *acc = mixer_acc;
	int16_t *wire = mixer_wire;
	int quiet = QUIET_BLOCKS, blocks = 0;
	int32_t gain = 32768;
	i2s_chan_handle_t out = tx;
#if CONFIG_PT_AUDIO_JACK
	bool fresh = true;		/* the first block after a silence */
#endif

	for (;;) {
		bool sound = false;
		size_t wrote;
		int hz;

		xSemaphoreTake(lock, portMAX_DELAY);
		reap(++blocks % 32 == 0);
		hz = wanted_rate();
		if (!hz && quiet >= QUIET_BLOCKS && !rx_on) {
			/* nothing to play: stop, and wait for something */
			output_stop();
			gain = 32768;
			xSemaphoreGive(lock);
			if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STANDBY_MS))) {
				xSemaphoreTake(lock, portMAX_DELAY);
				if (!tx_on && codec_on) {
					es8311_power(false);	/* a few seconds of silence */
					codec_on = false;
				}
				xSemaphoreGive(lock);
				ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			}
			quiet = 0;
#if CONFIG_PT_AUDIO_JACK
			fresh = true;
#endif
			continue;
		}
#if CONFIG_PT_AUDIO_JACK
		if (jack_wanted(fresh) != to_jack) {
			/* from one output to the other: the one left goes quiet */
			to_jack = !to_jack;
			if (to_jack && !jack && jack_open()) {
				to_jack = false;
			} else if (to_jack) {
				amp(false);
				if (tx_on && !rx_on) {
					i2s_channel_disable(tx);
					tx_on = false;
				}
			} else {
				jack_stop();
			}
		}
		fresh = false;
		if (to_jack) {
			/* the codec's clock only for a recording, the mix at the jack's */
			if (!hz)
				hz = rate;
			if (jack_clock_at(hz) || (rx_on && clock_at(tx_rate))) {
				xSemaphoreGive(lock);
				vTaskDelay(pdMS_TO_TICKS(100));
				continue;
			}
			rate = hz;
			out = jack;
		} else
#endif
		{
			if (clock_at(hz ? hz : rate)) {
				xSemaphoreGive(lock);
				vTaskDelay(pdMS_TO_TICKS(100));
				continue;
			}
			rate = tx_rate;
			out = tx;
		}
		memset(acc, 0, BLOCK * 2 * sizeof(*acc));
		for (int i = 0; i < MAX_STREAMS; i++) {
			struct stream *s = &streams[i];
			bool muted = owner && s->task != owner;
			uint32_t step;
			int v[2];

			if (!s->used)
				continue;
			step = (uint32_t)((uint64_t)s->rate * ONE / rate);
			for (int k = 0; k < BLOCK && pull(s, step, v); k++) {
				if (!muted) {
					acc[2 * k] += v[0];
					acc[2 * k + 1] += v[1];
				}
				sound = true;
			}
			xSemaphoreGive(s->space);
		}
		xSemaphoreGive(lock);
		quiet = sound ? 0 : quiet + 1;

#if CONFIG_PT_AUDIO_JACK
		if (out == jack) {
			/* in place: each sum becomes the DAC's 32-bit sample */
			gain = mix_gain(acc, BLOCK, 2, rate, gain);
			for (int k = 0; k < BLOCK * 2; k++)
				acc[k] = jack_sample(acc[k], gain, jack_gain);
			i2s_channel_write(out, acc, BLOCK * 2 * sizeof(*acc), &wrote,
					  pdMS_TO_TICKS(500));
			continue;
		}
#endif
		/* The speaker is one: the two sides folded, in place. */
		for (int k = 0; k < BLOCK; k++)
			acc[k] = (acc[2 * k] + acc[2 * k + 1]) / 2;
		gain = mix_gain(acc, BLOCK, 1, rate, gain);
		for (int k = 0; k < BLOCK; k++) {
			int v = (int)(((int64_t)acc[k] * gain) / 32768);

			wire[2 * k] = wire[2 * k + 1] = (int16_t)v;
		}
		if (sound && !amp_on)
			amp(true);
		/* the DMA takes it when it has room: this is what paces everything */
		i2s_channel_write(out, wire, BLOCK * 2 * sizeof(*wire), &wrote, pdMS_TO_TICKS(500));
	}
}

/* ------------------------------------------------------------ the API */

int audio_init(void)
{
	i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	i2s_std_config_t std = std_config();
	bool duplex = CONFIG_PT_AUDIO_DIN >= 0;
	int ret = -EIO;

	if (ready)
		return 0;
	lock = xSemaphoreCreateMutex();
	if (!lock)
		return -ENOMEM;
	/* All resources exist before the task starts, so an allocation
	 * failure cannot leave writers waiting on a mixer that has exited. */
	mixer_acc = heap_caps_malloc(BLOCK * 2 * sizeof(*mixer_acc), MALLOC_CAP_INTERNAL);
	mixer_wire = heap_caps_malloc(BLOCK * 2 * sizeof(*mixer_wire), MALLOC_CAP_INTERNAL);
	if (!mixer_acc || !mixer_wire) {
		ret = -ENOMEM;
		goto fail;
	}
	chan_cfg.auto_clear = true;	/* silence, not the last buffer again */
	chan_cfg.dma_desc_num = DMA_BUFS;
	chan_cfg.dma_frame_num = BLOCK;
	/* "rx switched from master to slave for full-duplex" is a warning
	 * about something we asked for: both directions share one clock. */
	esp_log_level_set("i2s_common", ESP_LOG_ERROR);
	if (i2s_new_channel(&chan_cfg, &tx, duplex ? &rx : NULL)) {
		klog("audio: no free I2S controller");
		goto fail;
	}
	if (i2s_channel_init_std_mode(tx, &std) || (rx && i2s_channel_init_std_mode(rx, &std))) {
		klog("audio: I2S setup failed");
		goto fail;
	}
#if CONFIG_PT_AUDIO_AMP_EN >= 0
	gpio_hold_dis(CONFIG_PT_AUDIO_AMP_EN);	/* held off through a deep sleep */
	gpio_set_direction(CONFIG_PT_AUDIO_AMP_EN, GPIO_MODE_OUTPUT);
	amp(false);
#endif
	ret = es8311_init(CONFIG_PT_AUDIO_I2C_SDA, CONFIG_PT_AUDIO_I2C_SCL, rate);
	if (ret)
		goto fail;
	codec_on = true;
	if ((ret = es8311_set_volume(volume)) || (ret = es8311_set_mic_gain(mic_gain)))
		goto fail;
#ifdef CONFIG_PT_AUDIO_MIC_ALC
	if ((ret = audio_set_mic_alc(true, CONFIG_PT_AUDIO_MIC_ALC_MAX_DB)))
		goto fail;
#endif
#if CONFIG_PT_AUDIO_JACK
	jack_init();
	jack_volume(jack_percent);
#endif
	if (xTaskCreatePinnedToCore(mixer_task, "kaudio", 3072, NULL, 18, &mixer, 0) != pdPASS) {
		ret = -ENOMEM;
		goto fail;
	}
	ready = true;
	audio_dev_register();
	klog("audio: %d Hz, %s%s; streams mixed", rate,
	     duplex ? "speaker and microphone" : "speaker only",
	     audio_has_jack() ? ", stereo headphone jack" : "");
	return 0;
fail:
	es8311_deinit();
	if (tx)
		i2s_del_channel(tx);
	if (rx)
		i2s_del_channel(rx);
	tx = rx = NULL;
	mixer = NULL;
	heap_caps_free(mixer_acc);
	heap_caps_free(mixer_wire);
	mixer_acc = NULL;
	mixer_wire = NULL;
	vSemaphoreDelete(lock);
	lock = NULL;
	return ret;
}

bool audio_has_jack(void)
{
#if CONFIG_PT_AUDIO_JACK
	return ready && !jack_failed;
#else
	return false;
#endif
}

bool audio_jack_switch(void)
{
#if CONFIG_PT_AUDIO_JACK && CONFIG_PT_AUDIO_JACK_DETECT >= 0
	return ready && !jack_failed;
#else
	return false;
#endif
}

int audio_set_output(enum audio_out o)
{
#if CONFIG_PT_AUDIO_JACK
	if (!ready || jack_failed)
		return -ENODEV;
	out_wanted = o;
	xTaskNotifyGive(mixer);
	return 0;
#else
	return -ENODEV;
#endif
}

enum audio_out audio_output(void)
{
#if CONFIG_PT_AUDIO_JACK
	return jack_failed ? AUDIO_OUT_SPEAKER : out_wanted;
#else
	return AUDIO_OUT_SPEAKER;
#endif
}

bool audio_to_jack(void)
{
#if CONFIG_PT_AUDIO_JACK
	return ready && !jack_failed && !owner &&
	       (out_wanted == AUDIO_OUT_JACK || (out_wanted == AUDIO_OUT_AUTO && plug_now()));
#else
	return false;
#endif
}

void audio_claim(bool mine)
{
	owner = mine ? xTaskGetCurrentTaskHandle() : NULL;
}

/* What the caller has queued goes, unplayed: a pause, or Ctrl-C. */
void audio_discard(void)
{
	struct stream *s;

	if (!audio_present())
		return;
	xSemaphoreTake(lock, portMAX_DELAY);
	if ((s = find(xTaskGetCurrentTaskHandle())))
		drop_queued(s);
	xSemaphoreGive(lock);
}

/*
 * The caller is done: what it queued plays out first -- this waits for
 * that, unless it is interrupted, when the rest is dropped -- then its
 * stream goes, and a recording ends. Whatever the others play goes on.
 */
void audio_stop(void)
{
	struct stream *s;
	bool proc = proc_current() != NULL;

	if (!audio_present())
		return;
	xSemaphoreTake(lock, portMAX_DELAY);
	s = find(xTaskGetCurrentTaskHandle());
	xSemaphoreGive(lock);
	while (s && filled(s) && !(proc && pt_interrupted()))
		xSemaphoreTake(s->space, pdMS_TO_TICKS(50));
	xSemaphoreTake(lock, portMAX_DELAY);
	if (s && s->used && s->task == xTaskGetCurrentTaskHandle()) {
		drop_queued(s);
		s->closing = true;
	}
	if (rx_on) {
		i2s_channel_disable(rx);
		rx_on = false;
	}
	xSemaphoreGive(lock);
	xTaskNotifyGive(mixer);
}

/* The caller's rate: its samples are brought to the wire's by the mixer. */
int audio_set_rate(int hz)
{
	struct stream *s;
	int ret = -ENOMEM;

	if (!audio_present())
		return -ENODEV;
	if (hz < 8000 || hz > 48000)
		return -EINVAL;
	xSemaphoreTake(lock, portMAX_DELAY);
	if ((s = mine()))
		ret = stream_size(s, hz, s->latency_ms ? s->latency_ms : LATENCY_MS);
	xSemaphoreGive(lock);
	return ret;
}

/* How far behind the writing the playing may run: its ring's size. */
int audio_set_latency(int ms)
{
	struct stream *s;
	int ret = -ENOMEM;

	if (!audio_present())
		return -ENODEV;
	if (ms < 10 || ms > 5000)
		return -EINVAL;
	xSemaphoreTake(lock, portMAX_DELAY);
	if ((s = mine()))
		ret = stream_size(s, s->rate, ms);
	xSemaphoreGive(lock);
	return ret;
}

/* The caller's rate if it plays, else the wire's. */
int audio_rate(void)
{
	struct stream *s;
	int hz;

	if (!lock)
		return rate;
	xSemaphoreTake(lock, portMAX_DELAY);
	s = find(xTaskGetCurrentTaskHandle());
	hz = s ? s->rate : rate;
	xSemaphoreGive(lock);
	return hz;
}

/* How much sound the caller's ring and the DMA hold when full. */
int audio_buffer_us(void)
{
	struct stream *s;
	int us;

	if (!lock)
		return 0;
	xSemaphoreTake(lock, portMAX_DELAY);
	s = find(xTaskGetCurrentTaskHandle());
	us = (int)((int64_t)DMA_BUFS * BLOCK * 1000000 / rate);
	if (s)
		us += (int)((int64_t)s->size * 1000000 / s->rate);
	xSemaphoreGive(lock);
	return us;
}

/*
 * How long before a sample the caller writes now is heard: its queue as it
 * stands, and the DMA's ring, taken as full (it is, while anything plays).
 * What a player needs to show a picture when its sound comes out.
 */
int audio_queued_us(void)
{
	struct stream *s;
	int64_t us;

	if (!lock)
		return 0;
	xSemaphoreTake(lock, portMAX_DELAY);
	s = find(xTaskGetCurrentTaskHandle());
	us = (int64_t)DMA_BUFS * BLOCK * 1000000 / rate;
	if (s)
		us += (int64_t)(__atomic_load_n(&s->head, __ATOMIC_ACQUIRE) -
				__atomic_load_n(&s->tail, __ATOMIC_ACQUIRE)) * 1000000 / s->rate;
	xSemaphoreGive(lock);
	return (int)us;
}

int audio_set_out_volume(enum audio_out out, int percent)
{
	if (percent < 0 || percent > 100)
		return -EINVAL;
	if (out == AUDIO_OUT_JACK) {
#if CONFIG_PT_AUDIO_JACK
		jack_volume(percent);
		return 0;
#else
		return -ENODEV;
#endif
	}
	volume = percent;
	return codec_on ? es8311_set_volume(percent) : 0;
}

int audio_out_volume(enum audio_out out)
{
#if CONFIG_PT_AUDIO_JACK
	if (out == AUDIO_OUT_JACK)
		return jack_percent;
#endif
	return volume;
}

/* The volume of wherever the sound goes now. */
int audio_set_volume(int percent)
{
	return audio_set_out_volume(audio_to_jack() ? AUDIO_OUT_JACK : AUDIO_OUT_SPEAKER, percent);
}

int audio_volume(void)
{
	return audio_out_volume(audio_to_jack() ? AUDIO_OUT_JACK : AUDIO_OUT_SPEAKER);
}

int audio_set_mic_gain(int db)
{
	mic_gain = db;
	return codec_on ? es8311_set_mic_gain(db) : 0;
}

int audio_set_mic_alc(bool on, int max_db)
{
	alc_on = on;
	alc_max = max_db;
	return codec_on ? es8311_set_alc(on && !alc_held, max_db) : 0;
}

/*
 * The codec's own gain riding held off, counted: rec's voice cleaning
 * levels the sound itself, and the codec's control ramping the hiss up
 * 15 to 20 dB in a recording's first seconds was taken for speech.
 */
void audio_mic_alc_hold(bool hold)
{
	if (!lock)
		return;
	xSemaphoreTake(lock, portMAX_DELAY);
	alc_held += hold ? 1 : -1;
	if (codec_on && alc_on && alc_held == (hold ? 1 : 0))
		es8311_set_alc(!alc_held, alc_max);
	xSemaphoreGive(lock);
}

bool audio_mic_alc(void)
{
	return alc_on;
}

/*
 * Play 16-bit samples: `channels` says whether the buffer is stereo; a
 * mono one is played on both sides. They go into the caller's ring,
 * waiting while it is full. Returns the bytes taken.
 */
ssize_t audio_write(const void *pcm, size_t bytes, int channels)
{
	const int16_t *in = pcm;
	size_t frames, done = 0;
	bool proc = proc_current() != NULL;
	struct stream *s;

	if (!audio_present())
		return -ENODEV;
	if (channels != 1 && channels != 2)
		return -EINVAL;
	frames = bytes / 2 / channels;
	xSemaphoreTake(lock, portMAX_DELAY);
	s = mine();
	xSemaphoreGive(lock);
	if (!s)
		return -ENOMEM;
	while (done < frames) {
		size_t head = __atomic_load_n(&s->head, __ATOMIC_RELAXED);
		size_t room = s->size - (head - __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE));
		size_t n = frames - done < room ? frames - done : room;

		if (!n) {
			if (proc && pt_interrupted())
				break;		/* Ctrl-C: the program is going */
			xTaskNotifyGive(mixer);
			xSemaphoreTake(s->space, pdMS_TO_TICKS(50));
			continue;
		}
		for (size_t k = 0; k < n; k++, done++) {
			int16_t *f = &s->ring[(head + k) % s->size * 2];

			f[0] = in[done * channels];
			f[1] = in[done * channels + channels - 1];
		}
		__atomic_store_n(&s->head, head + n, __ATOMIC_RELEASE);
		xTaskNotifyGive(mixer);
	}
	return (ssize_t)(done * 2 * channels);
}

/*
 * Record 16-bit mono samples at the caller's rate (audio_set_rate), or the
 * wire's if it set none. The recording has the clock: whatever else plays
 * meanwhile is brought to its rate by the mixer, which keeps the
 * transmitter's clock running while a recording is on. Taking the wire's
 * rate instead would make a recording started during a song say 16 kHz
 * in its header and hold 44.1.
 */
ssize_t audio_read(void *pcm, size_t bytes)
{
	int16_t *out = pcm, *scratch;
	size_t done = 0;

	if (!audio_present() || !rx)
		return -ENODEV;
	scratch = malloc(1024);
	if (!scratch)
		return -ENOMEM;
	xSemaphoreTake(lock, portMAX_DELAY);
	if (!rx_on) {
		struct stream *s = find(xTaskGetCurrentTaskHandle());

		/* the transmitter makes the clock the microphone is read with */
		if (clock_at(s ? s->rate : rate) || i2s_channel_enable(rx)) {
			xSemaphoreGive(lock);
			free(scratch);
			return -EIO;
		}
		rx_on = true;
		xTaskNotifyGive(mixer);
	}
	xSemaphoreGive(lock);
	while (done + 2 <= bytes) {
		size_t want = (bytes - done) * 2, got = 0;

		if (want > 1024)
			want = 1024;
		want &= ~3u;
		if (i2s_channel_read(rx, scratch, want, &got, pdMS_TO_TICKS(2000))) {
			free(scratch);
			return done ? (ssize_t)done : -EIO;
		}
		for (size_t i = 0; i < got / 4; i++)	/* the codec speaks on the left slot */
			out[done / 2 + i] = scratch[2 * i];
		done += got / 2;
	}
	free(scratch);
	return done;
}

/*
 * Before deep sleep: everything stopped and the codec in standby, which
 * it otherwise stays out of for as long as the system is up.
 */
void audio_sleep(void)
{
	if (!audio_present())
		return;
	xSemaphoreTake(lock, portMAX_DELAY);
	for (int i = 0; i < MAX_STREAMS; i++)
		if (streams[i].used)
			drop_queued(&streams[i]);
	if (rx_on) {
		i2s_channel_disable(rx);
		rx_on = false;
	}
	output_stop();
	if (codec_on) {
		es8311_power(false);
		codec_on = false;
	}
#if CONFIG_PT_AUDIO_AMP_EN >= 0
	/* An unheld pin floats in deep sleep, and a floating shutdown pin
	 * can leave the amplifier on all night. */
	gpio_hold_en(CONFIG_PT_AUDIO_AMP_EN);
#endif
	xSemaphoreGive(lock);
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
bool audio_busy(void) { return false; }
void audio_stop(void) { }
void audio_discard(void) { }
void audio_sleep(void) { }
void audio_claim(bool mine) { }
int audio_set_rate(int hz) { return -ENODEV; }
int audio_set_latency(int ms) { return -ENODEV; }
int audio_rate(void) { return 0; }
int audio_buffer_us(void) { return 0; }
int audio_queued_us(void) { return 0; }
int audio_set_volume(int percent) { return -ENODEV; }
int audio_volume(void) { return 0; }
int audio_set_out_volume(enum audio_out out, int percent) { return -ENODEV; }
int audio_out_volume(enum audio_out out) { return 0; }
int audio_set_mic_gain(int db) { return -ENODEV; }
int audio_set_mic_alc(bool on, int max_db) { return -ENODEV; }
void audio_mic_alc_hold(bool hold) { }
bool audio_mic_alc(void) { return false; }
ssize_t audio_write(const void *pcm, size_t bytes, int channels) { return -ENODEV; }
ssize_t audio_read(void *pcm, size_t bytes) { return -ENODEV; }
bool audio_has_jack(void) { return false; }
bool audio_jack_switch(void) { return false; }
int audio_set_output(enum audio_out out) { return -ENODEV; }
enum audio_out audio_output(void) { return AUDIO_OUT_SPEAKER; }
bool audio_to_jack(void) { return false; }

#endif
