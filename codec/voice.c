/*
 * A recorded voice made easier to listen to: the rumble cut, the steady
 * hiss taken out, the level evened out, the peaks held under full scale.
 * What `rec` does to a lesson recorded from a desk, where the teacher is
 * a few metres off and the microphone's hiss is nearly as loud: on the
 * user's recordings the voice stood 7 dB above the pauses before and 17
 * to 22 dB after, and nothing clipped.
 *
 * Every 8 ms, the last 16 ms of sound goes through an FFT. The noise in
 * each band is the minimum its power falls to, which speech, moving from
 * band to band, never holds for long; the band is then scaled by how far
 * it stands above that (Ephraim and Malah's decision-directed estimate,
 * which keeps the "musical noise" of plain spectral subtraction down),
 * but never by less than `reduce_db`, so what is left of the room sounds
 * like a quieter room and not like water. The leveller after it follows
 * the speech's level and not the pauses', and a limiter holds the rest.
 *
 * About 9% of a core at 16 kHz, measured on the board.
 */
#include <math.h>
#include <string.h>

#include "codec.h"
#include "pt/sys.h"

#define N	256		/* 16 ms at 16 kHz */
#define HOP	(N / 2)
#define BINS	(N / 2 + 1)
#define SUB	24		/* frames in a part of the noise's window */
#define SUBS	8		/* parts: the window is 1.5 s at 16 kHz */
#define BIAS	2.5f		/* how far such a minimum sits under the mean */

struct voice {
	float	win[N], cos_t[N / 2], sin_t[N / 2];
	short	bitrev[N];
	float	in[N], ola[N], out[HOP], re[N], im[N];
	int	fill, taken, frames;
	float	smooth[BINS], prev[BINS];
	float	least[SUBS][BINS], now_least[BINS];	/* the noise's window */
	int	sub_frames, sub_at;
	float	floor_gain;
	float	b0, b1, b2, a1, a2, x1, x2, y1, y2;	/* the high-pass */
	float	level, gain, target, env;		/* the leveller */
	int	heard;					/* frames of speech so far */
	bool	speech;
};

static void fft(struct voice *v, bool inverse)
{
	float *re = v->re, *im = v->im;

	for (int i = 0; i < N; i++) {
		int j = v->bitrev[i];

		if (j > i) {
			float t = re[i];

			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}
	for (int len = 2; len <= N; len <<= 1) {
		int step = N / len, half = len / 2;

		for (int i = 0; i < N; i += len)
			for (int k = 0; k < half; k++) {
				float c = v->cos_t[k * step];
				float s = inverse ? v->sin_t[k * step] : -v->sin_t[k * step];
				int a = i + k, b = a + half;
				float tr = re[b] * c - im[b] * s, ti = re[b] * s + im[b] * c;

				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
			}
	}
}

struct voice *voice_new(int rate, float reduce_db, float target_db)
{
	struct voice *v = pt_malloc(sizeof(*v));
	float w0 = 2 * (float)M_PI * 90 / rate, alpha = sinf(w0) / (2 * 0.7071f);
	float a0 = 1 + alpha;
	int bits = 0;

	if (!v)
		return NULL;
	memset(v, 0, sizeof(*v));
	for (int j = 0; j < SUBS; j++)
		for (int k = 0; k < BINS; k++)
			v->least[j][k] = 1e9f;		/* no parts yet: none counts */
	while ((1 << bits) < N)
		bits++;
	for (int i = 0; i < N; i++) {
		int r = 0;

		for (int b = 0; b < bits; b++)
			r |= ((i >> b) & 1) << (bits - 1 - b);
		v->bitrev[i] = r;
		v->win[i] = sinf((float)M_PI * (i + 0.5f) / N);	/* a square-root Hann, both ways */
	}
	for (int k = 0; k < N / 2; k++) {
		v->cos_t[k] = cosf(2 * (float)M_PI * k / N);
		v->sin_t[k] = sinf(2 * (float)M_PI * k / N);
	}
	/* a second-order Butterworth high-pass at 90 Hz: desks, feet, the air */
	v->b0 = (1 + cosf(w0)) / 2 / a0;
	v->b1 = -(1 + cosf(w0)) / a0;
	v->b2 = v->b0;
	v->a1 = -2 * cosf(w0) / a0;
	v->a2 = (1 - alpha) / a0;
	v->floor_gain = powf(10, -reduce_db / 20);
	v->target = target_db;
	v->gain = 1;
	return v;
}

void voice_free(struct voice *v)
{
	pt_free(v);
}

/* The window in v->in, cleaned and added into v->ola. */
static void frame(struct voice *v)
{
	float snr = 0;
	int voiced = 0;

	for (int i = 0; i < N; i++) {
		v->re[i] = v->in[i] * v->win[i];
		v->im[i] = 0;
	}
	fft(v, false);
	for (int k = 0; k < BINS; k++) {
		float p = v->re[k] * v->re[k] + v->im[k] * v->im[k], n, post, prio, g;

		/*
		 * The noise in the band: the least the smoothed power has
		 * been over the last second and a half (Martin's minimum
		 * statistics), which speech, moving from band to band, never
		 * fills. A minimum kept for ever sank to the deepest dip the
		 * hiss ever made, a tenth of its mean, and steady hiss was
		 * then taken for speech, let through and turned up.
		 */
		if (v->frames <= SUB) {
			/*
			 * The first fifth of a second is the start of the
			 * noise: its mean, from the first whole window on
			 * (the very first is half the silence before it).
			 * A minimum started there sat at nothing, and all
			 * of the hiss looked like speech.
			 */
			if (v->frames > 1)
				v->now_least[k] += (p - v->now_least[k]) / (v->frames - 1);
			else if (v->frames == 1)
				v->now_least[k] = p;
			v->smooth[k] = p;
			n = v->now_least[k] + 1e-9f;
			if (v->frames == SUB)
				for (int j = 0; j < SUBS; j++)
					v->least[j][k] = n / BIAS;
		} else {
			v->smooth[k] = 0.85f * v->smooth[k] + 0.15f * p;
			if (!v->sub_frames || v->smooth[k] < v->now_least[k])
				v->now_least[k] = v->smooth[k];
			n = v->now_least[k];
			for (int j = 0; j < SUBS; j++)
				if (v->least[j][k] < n)
					n = v->least[j][k];
			n = n * BIAS + 1e-9f;
		}
		post = p / n;
		prio = 0.98f * v->prev[k] / n + 0.02f * (post > 1 ? post - 1 : 0);
		g = prio / (1 + prio);
		if (g < v->floor_gain)
			g = v->floor_gain;
		v->prev[k] = g * g * p;
		v->re[k] *= g;
		v->im[k] *= g;
		if (k > 0 && k < N / 2) {		/* the mirror half of a real signal */
			v->re[N - k] = v->re[k];
			v->im[N - k] = -v->im[k];
		}
		if (k >= 5 && k <= 54) {		/* 300 to 3400 Hz: speech */
			snr += post;
			voiced++;
		}
	}
	v->speech = v->frames > SUB && voiced && snr / voiced > 3;
	fft(v, true);
	for (int i = 0; i < N; i++)
		v->ola[i] += v->re[i] / N * v->win[i];
	if (v->frames++ < SUB)
		return;
	if (++v->sub_frames == SUB) {
		/* a part of the window done: it replaces the oldest */
		memcpy(v->least[v->sub_at], v->now_least, sizeof(v->now_least));
		v->sub_at = (v->sub_at + 1) % SUBS;
		v->sub_frames = 0;
	}
}

/*
 * The leveller and the limiter over a hop of cleaned samples. The level
 * is followed in decibels, over the speech only: an average of powers is
 * the loud moments' alone, and following the pauses would bring the room
 * up with them. Down in about 50 ms, up in about two seconds, held in the
 * pauses; whatever is still over -1 dB is held there. The first three
 * seconds of speech set it ten times faster: a far voice comes in near
 * -55 dB with the codec's own riding off (audio_mic_alc_hold), and the
 * steady rate took fifteen seconds to bring it up. Up to +30 dB, which is
 * that riding's range and a little more.
 */
static void level(struct voice *v, float *y, int n)
{
	float p = 0, want, from = v->gain, to;

	for (int i = 0; i < n; i++)
		p += y[i] * y[i];
	p = 10 * log10f(p / n + 1e-12f);
	if (v->speech) {
		v->level = v->level ? 0.97f * v->level + 0.03f * p : p;
		v->heard++;
	}
	if (v->level < 0) {
		want = powf(10, (v->target - v->level) / 20);
		if (want > 31.6f)
			want = 31.6f;			/* +30 dB at most */
		if (want < 0.25f)
			want = 0.25f;
		if (want < v->gain)
			v->gain += (want - v->gain) * 0.15f;
		else if (v->speech)
			v->gain += (want - v->gain) * (v->heard < 375 ? 0.04f : 0.004f);
	}
	to = v->gain;
	for (int i = 0; i < n; i++) {
		float s = y[i] * (from + (to - from) * i / n), a = fabsf(s);

		v->env = a > v->env ? a : v->env * 0.9986f;	/* back in 50 ms */
		if (v->env > 0.89f)
			s *= 0.89f / v->env;
		y[i] = s;
	}
}

void voice_run(struct voice *v, int16_t *pcm, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float x = pcm[i] / 32768.0f, y;

		y = v->b0 * x + v->b1 * v->x1 + v->b2 * v->x2 - v->a1 * v->y1 - v->a2 * v->y2;
		v->x2 = v->x1;
		v->x1 = x;
		v->y2 = v->y1;
		v->y1 = y;
		v->in[HOP + v->fill++] = y;
		if (v->fill == HOP) {
			/* a hop in: a frame out, and the window moves on */
			v->fill = 0;
			frame(v);
			memcpy(v->out, v->ola, sizeof(v->out));
			memmove(v->ola, v->ola + HOP, (N - HOP) * sizeof(float));
			memset(v->ola + N - HOP, 0, HOP * sizeof(float));
			memmove(v->in, v->in + HOP, (N - HOP) * sizeof(float));
			level(v, v->out, HOP);
			v->taken = 0;
		}
		y = v->taken < HOP && v->frames ? v->out[v->taken++] : 0;
		y = y > 1 ? 1 : y < -1 ? -1 : y;
		pcm[i] = (int16_t)lrintf(y * 32767);
	}
}
