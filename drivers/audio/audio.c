/*
 * Sound: the I2S link to the codec, a mixer, and /dev/audio.
 *
 * The device is raw 16-bit mono, the way /dev/dsp was: write to it and
 * the speaker plays, read from it and the microphone records. The frame
 * on the wire is stereo because the codec expects two slots, so mono
 * samples are doubled on the way out and every second sample is taken
 * on the way in.
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
 */
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "es8311.h"
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

struct stream {
	TaskHandle_t	  task;		/* the writer */
	int		  pid;		/* its process, 0 for a kernel task */
	int		  rate;
	int		  latency_ms;
	int16_t		 *ring;
	size_t		  size;		/* samples the ring holds */
	size_t		  head, tail;	/* written and read, counting up for ever */
	SemaphoreHandle_t space;	/* given as the mixer makes room */
	uint32_t	  phase;	/* between samples a and b, in 1/65536ths */
	int16_t		  a, b;
	bool		  primed;	/* a and b hold samples */
	bool		  closing;	/* play what is left, then go */
	bool		  used;
};

static i2s_chan_handle_t	tx, rx;
static SemaphoreHandle_t	lock;		/* the table, and the hardware */
static struct stream		streams[MAX_STREAMS];
static TaskHandle_t		mixer;
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
static TaskHandle_t		owner;		/* an alarm ringing */

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
	return es8311_present();
}

/* Playing or recording: the transmitter's clock runs for both. */
bool audio_busy(void)
{
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
	if (alc_on)
		es8311_set_alc(true, alc_max);
	codec_on = true;
}

/* The clock running at `hz`, changing it first if it is not. Locked. */
static int clock_at(int hz)
{
	i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);

	codec_wake();
	if (tx_on && hz == rate)
		return 0;
	if (tx_on && rx_on)
		return 0;			/* a recording has the clock */
	if (tx_on) {
		i2s_channel_disable(tx);
		tx_on = false;
	}
	if (hz != rate) {
		clk.mclk_multiple = MCLK_MULTIPLE;
		if (i2s_channel_reconfig_std_clock(tx, &clk) ||
		    (rx && i2s_channel_reconfig_std_clock(rx, &clk)) || es8311_set_rate(hz))
			return -EIO;
		rate = hz;
	}
	if (i2s_channel_enable(tx))
		return -EIO;
	tx_on = true;
	return 0;
}

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
	ring = heap_caps_malloc(size * sizeof(*ring), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!ring)
		ring = heap_caps_malloc(size * sizeof(*ring), MALLOC_CAP_8BIT);
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
 * The stream's next sample at the rate on the wire, or false if it has
 * none just now. Two of its own samples are kept, and the output walks
 * from one to the next by its rate over the wire's.
 */
static bool pull(struct stream *s, uint32_t step, int *out)
{
	while (!s->primed || s->phase >= ONE) {
		size_t tail = __atomic_load_n(&s->tail, __ATOMIC_RELAXED);

		if (tail == __atomic_load_n(&s->head, __ATOMIC_ACQUIRE))
			return false;
		s->a = s->primed ? s->b : s->ring[tail % s->size];
		s->b = s->ring[tail % s->size];
		__atomic_store_n(&s->tail, tail + 1, __ATOMIC_RELEASE);
		if (s->primed)
			s->phase -= ONE;
		s->primed = true;
	}
	*out = s->a + (int)(((int64_t)(s->b - s->a) * s->phase) >> 16);
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
	int32_t *acc = heap_caps_malloc(BLOCK * sizeof(*acc), MALLOC_CAP_INTERNAL);
	int16_t *wire = heap_caps_malloc(BLOCK * 2 * sizeof(*wire), MALLOC_CAP_INTERNAL);
	int quiet = QUIET_BLOCKS, blocks = 0;

	if (!acc || !wire) {
		klog("audio: no memory for the mixer");
		vTaskDelete(NULL);
	}
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
			continue;
		}
		if (clock_at(hz ? hz : rate)) {
			xSemaphoreGive(lock);
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}
		memset(acc, 0, BLOCK * sizeof(*acc));
		for (int i = 0; i < MAX_STREAMS; i++) {
			struct stream *s = &streams[i];
			bool muted = owner && s->task != owner;
			uint32_t step;
			int v;

			if (!s->used)
				continue;
			step = (uint32_t)((uint64_t)s->rate * ONE / rate);
			for (int k = 0; k < BLOCK && pull(s, step, &v); k++) {
				if (!muted)
					acc[k] += v;
				sound = true;
			}
			xSemaphoreGive(s->space);
		}
		xSemaphoreGive(lock);

		for (int k = 0; k < BLOCK; k++) {
			int v = acc[k] > 32767 ? 32767 : acc[k] < -32768 ? -32768 : acc[k];

			wire[2 * k] = wire[2 * k + 1] = (int16_t)v;
		}
		quiet = sound ? 0 : quiet + 1;
		if (sound && !amp_on)
			amp(true);
		/* the DMA takes it when it has room: this is what paces everything */
		i2s_channel_write(tx, wire, BLOCK * 2 * sizeof(*wire), &wrote, pdMS_TO_TICKS(500));
	}
}

/* ------------------------------------------------------------ the API */

int audio_init(void)
{
	i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	i2s_std_config_t std = std_config();
	bool duplex = CONFIG_PT_AUDIO_DIN >= 0;

	lock = xSemaphoreCreateMutex();
	if (!lock)
		return -ENOMEM;
	chan_cfg.auto_clear = true;	/* silence, not the last buffer again */
	chan_cfg.dma_desc_num = DMA_BUFS;
	chan_cfg.dma_frame_num = BLOCK;
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
	gpio_hold_dis(CONFIG_PT_AUDIO_AMP_EN);	/* held off through a deep sleep */
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
	es8311_set_mic_gain(mic_gain);
#ifdef CONFIG_PT_AUDIO_MIC_ALC
	audio_set_mic_alc(true, CONFIG_PT_AUDIO_MIC_ALC_MAX_DB);
#endif
	xTaskCreatePinnedToCore(mixer_task, "kaudio", 3072, NULL, 18, &mixer, 0);
	audio_dev_register();
	klog("audio: %d Hz mono, %s; streams mixed", rate,
	     duplex ? "speaker and microphone" : "speaker only");
	return 0;
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

int audio_set_volume(int percent)
{
	if (percent < 0 || percent > 100)
		return -EINVAL;
	volume = percent;
	return codec_on ? es8311_set_volume(percent) : 0;
}

int audio_volume(void)
{
	return volume;
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
	return codec_on ? es8311_set_alc(on, max_db) : 0;
}

bool audio_mic_alc(void)
{
	return alc_on;
}

/*
 * Play 16-bit samples: `channels` says whether the buffer is stereo, and
 * a stereo one is mixed down, the speaker being mono. They go into the
 * caller's ring, waiting while it is full. Returns the bytes taken.
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
		for (size_t k = 0; k < n; k++, done++)
			s->ring[(head + k) % s->size] = channels == 2 ?
				(int16_t)((in[2 * done] + in[2 * done + 1]) / 2) : in[done];
		__atomic_store_n(&s->head, head + n, __ATOMIC_RELEASE);
		xTaskNotifyGive(mixer);
	}
	return (ssize_t)(done * 2 * channels);
}

/*
 * Record 16-bit mono samples, at the rate on the wire: the caller's, if
 * nothing else is playing. The mixer keeps the transmitter's clock
 * running while a recording is on.
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
		if (clock_at(tx_on || !s ? rate : s->rate) || i2s_channel_enable(rx)) {
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
int audio_set_mic_gain(int db) { return -ENODEV; }
int audio_set_mic_alc(bool on, int max_db) { return -ENODEV; }
bool audio_mic_alc(void) { return false; }
ssize_t audio_write(const void *pcm, size_t bytes, int channels) { return -ENODEV; }
ssize_t audio_read(void *pcm, size_t bytes) { return -ENODEV; }

#endif
