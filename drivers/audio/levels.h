/* Output levels, shared with the host checks; no hardware access here. */
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* The speaker uses the codec's full volume range, independent of the jack. */
static inline int speaker_volume_reg(int percent)
{
	if (percent <= 0)
		return 0;
	if (percent > 100)
		percent = 100;
	return 0xbf - (100 - percent) * 4 / 5;
}

/*
 * Look ahead over the block already mixed. Turn the whole block down
 * before it clips, keeping its waveform, then recover over about 125 ms.
 * gain is Q15 (32768 is unity); an ordinary single stream passes unchanged.
 * The samples are `frames` of `channels` each, side by side.
 */
static inline int32_t mix_gain(const int32_t *samples, size_t frames, int channels, int rate,
			       int32_t gain)
{
	int32_t peak = 0, target = 32768;

	for (size_t i = 0; i < frames * channels; i++) {
		int32_t v = samples[i] < 0 ? -samples[i] : samples[i];

		if (v > peak)
			peak = v;
	}
	if (peak > 32767)
		target = (int32_t)((32767LL * 32768) / peak);
	if (target < gain)
		return target;
	if (target > gain) {
		int32_t step = (int32_t)((int64_t)(target - gain) * frames * 8 / rate);

		gain += step ? step : 1;
	}
	return gain > target ? target : gain;
}

/*
 * The headphones' volume as a Q15 multiplier: 0.6 dB a percent, so 100 is
 * the DAC's full line level and 50 is 30 dB under it. Headphones want far
 * less than a line input does, and a wide range puts the levels they are
 * listened at in the middle of the scale rather than in its bottom tenth.
 */
static inline int32_t jack_gain_for(int percent)
{
	if (percent <= 0)
		return 0;
	if (percent >= 100)
		return 32768;
	return (int32_t)(32768.0f * powf(10.0f, -(100 - percent) * 0.6f / 20.0f));
}

/*
 * A mixed sample, with the limiter's gain and the headphones' volume, as
 * the DAC's 32 bits. Both gains are applied at once and nothing is
 * rounded off: turned down 40 dB, a 16-bit sample keeps all its bits
 * where a 16-bit output would keep nine.
 */
static inline int32_t jack_sample(int32_t mixed, int32_t gain, int32_t volume)
{
	int64_t v = ((int64_t)mixed * gain * volume) >> 14;

	return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}
