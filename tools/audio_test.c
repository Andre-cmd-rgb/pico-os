#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../codec/codec.h"
#include "../drivers/audio/levels.h"

#include "half_under_test.h"	/* play's 2:1 filter, cut out of bin/sink.c */

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

	/* The speaker's 16 bits: rounded to the nearest, and held at the ends. */
	assert(speaker_sample(1000 * 256, 32768) == 1000);
	assert(speaker_sample(1000 * 256 + 127, 32768) == 1000);
	assert(speaker_sample(1000 * 256 + 128, 32768) == 1001);
	assert(speaker_sample(-1000 * 256 - 129, 32768) == -1001);
	assert(speaker_sample(MIX_FULL, 32768) == 32767);
	assert(speaker_sample(MIX_OVER, 32768) == 32767 && speaker_sample(-MIX_OVER, 32768) == -32768);

	/* 192 kHz to 96: flat where it is heard, and nothing folded back into it. */
	assert(fabs(through_half(1000)) < 0.01 && fabs(through_half(20000)) < 0.01);
	assert(through_half(76000) < -100 && through_half(90000) < -100);

	puts("audio: full speaker range, unclipped mix, gain recovery, headphone curve, "
	     "24 bits to 32, rounding to 16 and the 2:1 filter passed");
	return 0;
}
