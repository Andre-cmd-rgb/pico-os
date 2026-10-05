#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../codec/codec.h"
#include "../drivers/audio/levels.h"

#include "half_under_test.h"	/* play's 2:1 filter, cut out of bin/sink.c */

typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#include "mixer_under_test.h"	/* pull() and mix_stream(), from audio.c */

static uint32_t seed = 1;

static uint32_t rnd(void)
{
	seed = seed * 1103515245 + 12345;
	return seed >> 8;
}

/*
 * mix_stream() against pull() a frame at a time, which it stands in for:
 * two copies of one stream fed the same frames -- in bursts, a trickle, or
 * none at all, at the wire's rate and away from it, muted or not -- must
 * give the same blocks and be left in the same state.
 */
static void mixer_matches_pull(void)
{
	static int32_t ring_a[2 * 997], ring_b[2 * 997];
	static int32_t acc_a[2 * BLOCK], acc_b[2 * BLOCK];
	static const uint32_t steps[] = { ONE, ONE, ONE, ONE, ONE / 2, 48000ull * ONE / 44100,
					  44100ull * ONE / 48000, 3 * ONE };

	for (int run = 0; run < 400; run++) {
		struct stream a = { 0 }, b;
		size_t size = 1 + rnd() % 997;
		int32_t value = 0;

		a.ring = ring_a;
		a.size = size;
		a.used = true;
		b = a;
		b.ring = ring_b;
		for (int block = 0; block < 300; block++) {
			size_t room = size - filled(&a), add = rnd() % 4 ? rnd() % (2 * BLOCK) : 0;
			uint32_t step = steps[rnd() % 8];
			bool muted = rnd() % 16 == 0;
			int n = 0, k;

			for (; add && room; add--, room--) {
				size_t at = a.head % size * 2;

				ring_a[at] = ring_b[at] = value += rnd() % 2001 - 1000;
				ring_a[at + 1] = ring_b[at + 1] = -value / 2;
				a.head++;
				b.head++;
			}
			for (int i = 0; i < 2 * BLOCK; i++)
				acc_a[i] = acc_b[i] = (int32_t)(rnd() % 100);
			for (int v[2]; n < BLOCK && pull(&a, step, v); n++) {
				if (!muted) {
					acc_a[2 * n] += v[0];
					acc_a[2 * n + 1] += v[1];
				}
			}
			k = mix_stream(&b, step, acc_b, muted);
			assert(k == n && !memcmp(acc_a, acc_b, sizeof(acc_a)));
			assert(a.tail == b.tail && a.phase == b.phase && a.primed == b.primed);
			assert(!memcmp(a.a, b.a, sizeof(a.a)) && !memcmp(a.b, b.b, sizeof(a.b)));
		}
	}
}

/* A sine's level through half_run(), in dB, after the filter has filled. */
static double through_half(double hz)
{
	static int32_t pcm[192000];
	struct half h = { 0 };
	double sum = 0;
	size_t out;

	for (int i = 0; i < 192000; i++)
		pcm[i] = (int32_t)lrint(MIX_FULL / 2.0 * sin(2 * M_PI * hz * i / 192000));
	out = half_run(&h, pcm, 192000, 1);
	assert(out == 96000);
	for (size_t i = 1000; i < out; i++)
		sum += (double)pcm[i] * pcm[i];
	return 10 * log10(sum / (out - 1000) / (MIX_FULL / 2.0 * MIX_FULL / 2.0 / 2));
}

int main(void)
{
	int32_t samples[160], gain;

	assert(speaker_volume_reg(0) == 0);
	assert(speaker_volume_reg(75) == 0xbf - 20);
	assert(speaker_volume_reg(100) == 0xbf);
	for (int p = 1; p <= 100; p++) {
		assert(speaker_volume_reg(p) >= speaker_volume_reg(p - 1));
		assert(speaker_volume_reg(p) <= speaker_volume_reg(100));
	}
	/* A single stream below full scale is bit-for-bit unchanged. */
	for (int i = 0; i < 160; i++)
		samples[i] = (i % 9 - 4) * 7000 * 256;
	assert(mix_gain(samples, 160, 1, 44100, 32768) == 32768);
	/* Four overlapping streams retain their shape, rather than flat tops. */
	for (int i = 0; i < 160; i++)
		samples[i] *= 4;
	gain = mix_gain(samples, 160, 1, 44100, 32768);
	assert(gain > 0 && gain < 32768);
	for (int i = 0; i < 160; i++) {
		int v = (int)((int64_t)samples[i] * gain / 32768);

		assert(v >= -MIX_FULL && v <= MIX_FULL);
		assert(v == -(int)((int64_t)-samples[i] * gain / 32768));
	}
	/* Releasing attenuation is gradual, monotonic, and never exceeds unity. */
	for (int i = 0; i < 160; i++)
		samples[i] = 0;
	for (int rate = 8000; rate <= 48000; rate += 8000) {
		gain = 8000;
		for (int block = 0; block < rate * 2 / 160; block++) {
			int32_t next = mix_gain(samples, 160, 1, rate, gain);

			assert(next >= gain && next <= 32768);
			assert(next - gain <= (32768 - gain) / 6 + 1);
			gain = next;
		}
		assert(gain == 32768);
	}
	/* Stereo: the peak of either side limits both, released per frame. */
	for (int i = 0; i < 160; i++)
		samples[i] = i % 2 ? 0 : 40000 * 256;
	gain = mix_gain(samples, 80, 2, 44100, 32768);
	assert((int64_t)40000 * 256 * gain / 32768 <= MIX_FULL);
	for (int i = 0; i < 160; i++)
		samples[i] = 0;
	assert(mix_gain(samples, 80, 2, 44100, 16384) == mix_gain(samples, 160, 1, 88200, 16384));

	/* Headphones: 0.6 dB a step, silent at 0, the DAC's full scale at 100. */
	assert(jack_gain_for(0) == 0);
	assert(jack_gain_for(100) == 32768);
	for (int p = 1; p <= 100; p++)
		assert(jack_gain_for(p) > jack_gain_for(p - 1));
	assert(abs(jack_gain_for(50) - 1036) <= 1);		/* -30 dB */
	assert(abs(jack_gain_for(90) - 16422) <= 2);		/* -6 dB */

	/* 32 bits for the DAC: full scale reaches it, nothing wraps, and a
	 * quiet volume keeps every bit of a 24-bit sample. */
	assert(jack_sample(MIX_FULL, 32768, 32768) == MIX_FULL * 256);
	assert(jack_sample(-MIX_FULL - 1, 32768, 32768) == INT32_MIN);
	assert(jack_sample(MIX_FULL * 4, 8192, 32768) == MIX_FULL * 256);
	assert(jack_sample(MIX_OVER * 4, 32768, 32768) == INT32_MAX);
	for (int v = -MIX_FULL - 1; v <= MIX_FULL; v += 100003) {
		int64_t want = ((int64_t)v * 32768 * jack_gain_for(30)) >> 22;

		assert(jack_sample(v, 32768, jack_gain_for(30)) == want);
	}
	assert(jack_sample(1, 32768, jack_gain_for(40)) != jack_sample(2, 32768, jack_gain_for(40)));

	/* Both as their plain arithmetic gives them, over the whole range. */
	for (int i = 0; i < 1000000; i++) {
		int32_t m = (int32_t)(rnd() % (4u * MIX_OVER + 1)) - 2 * MIX_OVER;
		int32_t g = i % 3 ? (int32_t)(rnd() % 32769) : 32768, vol = rnd() % 32769;
		int64_t j = ((int64_t)m * g * vol) >> 22, sp = (((int64_t)m * g >> 15) + 128) >> 8;

		assert(jack_sample(m, g, vol) == (j > INT32_MAX ? INT32_MAX : j < INT32_MIN ?
						  INT32_MIN : j));
		assert(speaker_sample(m, g) == (sp > INT16_MAX ? INT16_MAX : sp < INT16_MIN ?
						 INT16_MIN : sp));
	}

	/* The speaker's 16 bits: rounded to the nearest, and held at the ends. */
	assert(speaker_sample(1000 * 256, 32768) == 1000);
	assert(speaker_sample(1000 * 256 + 127, 32768) == 1000);
	assert(speaker_sample(1000 * 256 + 128, 32768) == 1001);
	assert(speaker_sample(-1000 * 256 - 129, 32768) == -1001);
	assert(speaker_sample(MIX_FULL, 32768) == 32767);
	assert(speaker_sample(MIX_OVER, 32768) == 32767 && speaker_sample(-MIX_OVER, 32768) == -32768);

	mixer_matches_pull();

	/* 192 kHz to 96: flat where it is heard, and nothing folded back into it. */
	assert(fabs(through_half(1000)) < 0.01 && fabs(through_half(20000)) < 0.01);
	assert(through_half(76000) < -100 && through_half(90000) < -100);

	puts("audio: full speaker range, unclipped mix, gain recovery, headphone curve, "
	     "24 bits to 32, rounding to 16, the mixer's block walk and the 2:1 filter passed");
	return 0;
}
