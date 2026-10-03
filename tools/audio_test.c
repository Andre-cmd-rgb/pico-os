#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../drivers/audio/levels.h"

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
		samples[i] = (i % 9 - 4) * 7000;
	assert(mix_gain(samples, 160, 1, 44100, 32768) == 32768);
	/* Four overlapping streams retain their shape, rather than flat tops. */
	for (int i = 0; i < 160; i++)
		samples[i] *= 4;
	gain = mix_gain(samples, 160, 1, 44100, 32768);
	assert(gain > 0 && gain < 32768);
	for (int i = 0; i < 160; i++) {
		int v = (int)((int64_t)samples[i] * gain / 32768);

		assert(v >= -32767 && v <= 32767);
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
		samples[i] = i % 2 ? 0 : 40000;
	gain = mix_gain(samples, 80, 2, 44100, 32768);
	assert((int64_t)40000 * gain / 32768 <= 32767);
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
	 * quiet volume keeps every bit of the sample. */
	assert(jack_sample(32767, 32768, 32768) == 32767 * 65536);
	assert(jack_sample(-32768, 32768, 32768) == INT32_MIN);
	assert(jack_sample(131068, 8192, 32768) == 32767 * 65536);
	for (int v = -32768; v <= 32767; v += 1001) {
		int64_t want = ((int64_t)v * 32768 * jack_gain_for(30)) >> 14;

		assert(jack_sample(v, 32768, jack_gain_for(30)) == want);
	}
	assert(jack_sample(1, 32768, jack_gain_for(40)) != jack_sample(2, 32768, jack_gain_for(40)));
	puts("audio: full speaker range, unclipped mix, gain recovery, headphone curve "
	     "and 32-bit samples passed");
	return 0;
}
