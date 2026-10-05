/*
 * Where decoded sound goes to be heard, for play and music: 24-bit frames
 * as a decoder hands them over, at the file's own rate -- halved past what
 * the codec takes -- with a second of them queued.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "pt/kernel.h"
#include "sink.h"
#include "util.h"

#if CONFIG_PT_AUDIO

#define MUSIC_LATENCY_MS 1000
#define RATE_MAX	(2 * AUDIO_RATE_MAX)	/* over AUDIO_RATE_MAX, a file is halved */

/*
 * 176.4 and 192 kHz files play at half that: two frames into one, through
 * a half-band low-pass (47 taps, Kaiser beta 10) flat to 0.0001 dB below
 * 20 kHz, with whatever would fold back below 20 kHz 105 dB down. Every
 * other tap of a half-band filter is nought and the rest are symmetric
 * about the middle, so a frame costs 13 multiplications a side.
 */
#define HALF_TAPS	47
#define HALF_MID	23

static const float half_mid = 0.499998347f;
static const float half_odd[12] = {
	3.154663788e-01f, -9.784546391e-02f, 5.074820199e-02f, -2.901235634e-02f,
	1.662952581e-02f, -9.154037385e-03f, 4.696079299e-03f, -2.178689259e-03f,
	8.781318778e-04f, -2.871515223e-04f, 6.512216305e-05f, -4.915094928e-06f,
};

struct half {
	float	x[2][2 * HALF_TAPS];	/* a side's last frames, twice over to read in one run */
	int	pos;
	bool	odd;
};

/* `frames` in, half as many out, in place. */
static size_t half_run(struct half *h, int32_t *pcm, size_t frames, int channels)
{
	size_t out = 0;

	for (size_t i = 0; i < frames; i++) {
		for (int c = 0; c < channels; c++)
			h->x[c][h->pos] = h->x[c][h->pos + HALF_TAPS] = pcm[i * channels + c];
		h->pos = (h->pos + 1) % HALF_TAPS;
		if ((h->odd = !h->odd))
			continue;
		for (int c = 0; c < channels; c++) {
			const float *w = &h->x[c][h->pos];	/* the oldest first */
			float y = half_mid * w[HALF_MID];

			for (int k = 0; k < 12; k++)
				y += half_odd[k] * (w[HALF_MID - 1 - 2 * k] + w[HALF_MID + 1 + 2 * k]);
			pcm[out * channels + c] = y >= CODEC_OVER ? CODEC_OVER :
						  y <= -CODEC_OVER ? -CODEC_OVER : codec_round(y);
		}
		out++;
	}
	return out;
}

int sink_open(struct sink *s, bool dry)
{
	*s = (struct sink){ .dry = dry, .started = pt_uptime_us() };
	if (!dry)
		return 0;		/* only the checksum wants the mono copy */
	s->mono = pt_malloc(SINK_PASS * sizeof(*s->mono));
	return s->mono ? 0 : -ENOMEM;
}

void sink_close(struct sink *s)
{
	if (!s->dry)
		audio_stop();
	pt_free(s->half);
	pt_free(s->mono);
}

int sink_format(struct sink *s, const char *prog, const char *name, int rate, int channels)
{
	bool halve = rate > AUDIO_RATE_MAX;

	if (rate == s->rate && channels == s->channels)
		return 0;
	if (rate < 8000 || rate > RATE_MAX) {
		pt_dprintf(PT_STDERR, "%s: %s: %d Hz is outside 8000-%d\n", prog, name, rate, RATE_MAX);
		return -EINVAL;
	}
	s->rate = rate;
	s->channels = channels;
	pt_free(s->half);
	s->half = NULL;
	if (s->dry)
		return 0;		/* the decoder's own output is what is checked */
	if (halve && !(s->half = pt_calloc(1, sizeof(*s->half))))
		return -ENOMEM;
	/* a second queued: a busy card or a screenshot never runs it dry */
	return audio_set_rate(halve ? rate / 2 : rate) ? -EIO : audio_set_latency(MUSIC_LATENCY_MS);
}

int sink_play(struct sink *s, int32_t *pcm, size_t frames, int channels)
{
	if (s->half)
		frames = half_run(s->half, pcm, frames, channels);
	while (frames) {
		size_t n = frames > SINK_PASS ? SINK_PASS : frames;
		ssize_t wrote;

		if (s->dry) {
			for (size_t i = 0; i < n; i++)
				s->mono[i] = channels == 2 ? (pcm[2 * i] + pcm[2 * i + 1]) / 2 / 256
							   : pcm[i] / 256;
			s->crc = crc32_of(s->crc, s->mono, n * sizeof(*s->mono));
		}
		s->frames += n;
		if (s->dry && s->frames / SINK_PASS % 8 == 0)
			pt_sleep_ms(1);		/* the idle task's turn */
		if (!s->dry && (wrote = audio_write24(pcm, n * channels * sizeof(*pcm), channels)) < 0)
			return wrote;
		pcm += n * channels;
		frames -= n;
		if (pt_interrupted())
			return -EINTR;
	}
	return 0;
}

void sink_report(const struct sink *s, const char *name, const struct codec *c)
{
	int64_t us = pt_uptime_us() - s->started;
	double seconds = s->rate ? (double)s->frames / s->rate : 0;
	char bits[12] = "";

	if (c->bits)
		snprintf(bits, sizeof(bits), " %d-bit", c->bits);
	pt_printf("%s: %s, %d Hz%s %s, %.2f s decoded in %.2f s (%.1fx), crc %08lx\n",
		  name, codec_name(c), s->rate, bits, s->channels == 2 ? "stereo" : "mono",
		  seconds, us / 1e6, us ? seconds * 1e6 / us : 0, (unsigned long)s->crc);
}

#endif /* CONFIG_PT_AUDIO */
