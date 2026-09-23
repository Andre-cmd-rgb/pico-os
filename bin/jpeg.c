/*
 * Baseline JPEG, decoded to RGB565. See jpeg.h for what it takes.
 *
 * Why not the decompressor in the ROM, which is what canvas.c called at
 * first: it is TJpgDec from 2011, which finds each Huffman code a bit at a
 * time and hands every MCU over as RGB888 for a callback to turn into
 * something else. A 320x240 frame took it 50 ms on one core; this takes
 * 21, and 12 split over both.
 *
 * Where the time goes, measured on the board, and what was done about it:
 *
 *   - Huffman codes are looked up JPEG_FAST bits at a time, and most AC
 *     coefficients -- a short code followed by a small value -- come out
 *     of one lookup with the value already sign-extended;
 *   - two blocks in three in video are flat, nothing but a DC, and are a
 *     fill of words (not memset(), whose calls cost more than the fill);
 *   - the IDCT is AAN, not libjpeg's accurate one: five multiplies a pass
 *     instead of twelve, and few enough live values for the sixteen
 *     registers this core has. It is within one step of RGB565 of the
 *     accurate one. A block with nothing outside its top-left 4x4 uses
 *     the same transform with the zeros folded away;
 *   - colour is table lookups: each chroma sample sets where its pixels
 *     look, and a pixel is its luma and three loads, written as RGB565 in
 *     the panel's byte order, two to a word;
 *   - the IDCT is in IRAM, clear of the flash cache.
 *
 * Damaged data never reads or writes outside what it was given: a bad
 * code or a run past the end of a block is caught, arithmetic that could
 * overflow wraps instead, and the rest decodes as whatever it decodes as.
 */
#include <errno.h>
#include <string.h>

#include "jpeg.h"
/*
 * The IDCT runs from internal RAM rather than through the flash cache:
 * 10% quicker, measured, for under 2 KB. The rest of the decode loop made
 * no difference either way and stays in flash.
 */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define HOT IRAM_ATTR
#else
#define HOT
#endif

#define M_SOF0		0xc0
#define M_SOF1		0xc1
#define M_DHT		0xc4
#define M_SOI		0xd8
#define M_EOI		0xd9
#define M_SOS		0xda
#define M_DQT		0xdb
#define M_DRI		0xdd

/* The natural position of each coefficient in zigzag order, with room for a
 * damaged run to go past the end and land harmlessly on the last one. */
static const uint8_t dezigzag[64 + 16] = {
	 0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
	63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
};

/* ------------------------------------------------------------ bytes */

static int refill(struct jpeg *j)
{
	if (j->refill && j->refill(j->ctx, &j->p, &j->end) && j->p < j->end)
		return 1;
	j->p = j->end;
	j->eof = 1;
	return 0;
}

static inline int get8(struct jpeg *j)
{
	if (j->p == j->end && !refill(j))
		return -1;
	return *j->p++;
}

static int get16(struct jpeg *j)
{
	int hi = get8(j), lo = get8(j);

	return hi < 0 || lo < 0 ? -1 : hi << 8 | lo;
}

static int skip(struct jpeg *j, int n)
{
	while (n > 0) {
		size_t k;

		if (j->p == j->end && !refill(j))
			return -1;
		k = (size_t)(j->end - j->p) < (size_t)n ? (size_t)(j->end - j->p) : (size_t)n;
		j->p += k;
		n -= (int)k;
	}
	return 0;
}

/* ------------------------------------------------------------ headers */

static int build_huff(struct jpeg_huff *h, const uint8_t count[16])
{
	uint16_t code[256];
	int k = 0, c = 0;

	for (int len = 1; len <= 16; len++) {
		h->delta[len] = k - c;
		for (int i = 0; i < count[len - 1]; i++) {
			h->size[k] = (uint8_t)len;
			code[k++] = (uint16_t)c++;
		}
		if (c > 1 << len)
			return -EINVAL;		/* more codes than the length has room for */
		h->maxcode[len] = (uint32_t)c << (16 - len);
		c <<= 1;
	}
	h->maxcode[17] = 0xffffffff;		/* stops the search for a length */
	memset(h->fast, 255, sizeof(h->fast));
	for (int i = 0; i < k; i++) {
		int len = h->size[i];

		if (len <= JPEG_FAST) {
			int first = code[i] << (JPEG_FAST - len);

			memset(h->fast + first, i, (size_t)1 << (JPEG_FAST - len));
		}
	}
	return 0;
}

/*
 * The AC entries that need no second step: a code and its value both
 * inside JPEG_FAST bits, the value small enough for a byte. Each entry is
 * the value << 8 | the zero run << 4 | the bits the pair takes.
 */
static void build_fast_ac(int16_t *fac, const struct jpeg_huff *h)
{
	for (int i = 0; i < 1 << JPEG_FAST; i++) {
		int k = h->fast[i], rs, run, s, len;

		fac[i] = 0;
		if (k == 255)
			continue;
		rs = h->values[k];
		run = rs >> 4;
		s = rs & 15;
		len = h->size[k];
		if (s && len + s <= JPEG_FAST) {
			int v = ((i << len) & ((1 << JPEG_FAST) - 1)) >> (JPEG_FAST - s);

			if (v < 1 << (s - 1))
				v -= (1 << s) - 1;
			if (v >= -128 && v <= 127)
				fac[i] = (int16_t)(v * 256 + run * 16 + len + s);
		}
	}
}

static int read_dqt(struct jpeg *j, int len)
{
	while (len > 0) {
		int pt = get8(j), wide = pt >> 4, t = pt & 15;

		if (pt < 0 || wide > 1 || t > 3)
			return -EINVAL;
		for (int i = 0; i < 64; i++) {
			int v = wide ? get16(j) : get8(j);

			if (v < 0)
				return -EINVAL;
			j->qt[t][dezigzag[i]] = (uint16_t)v;
		}
		j->have_qt |= 1 << t;
		len -= 1 + 64 * (wide + 1);
	}
	return len ? -EINVAL : 0;
}

static int read_dht(struct jpeg *j, int len)
{
	while (len > 0) {
		int tc = get8(j), t = tc & 15, total = 0;
		uint8_t count[16];
		struct jpeg_huff *h;

		if (tc < 0 || tc >> 4 > 1)
			return -EINVAL;
		if (t > 1)
			return -ENOTSUP;	/* baseline has two of each */
		for (int i = 0; i < 16; i++) {
			int n = get8(j);

			if (n < 0)
				return -EINVAL;
			count[i] = (uint8_t)n;
			total += n;
		}
		if (total > 256)
			return -EINVAL;
		h = tc >> 4 ? &j->ac[t] : &j->dc[t];
		memset(h->values, 0, sizeof(h->values));
		for (int i = 0; i < total; i++) {
			int v = get8(j);

			if (v < 0)
				return -EINVAL;
			h->values[i] = (uint8_t)v;
		}
		if (build_huff(h, count))
			return -EINVAL;
		if (tc >> 4) {
			build_fast_ac(j->fast_ac[t], h);
			j->have_ac |= 1 << t;
		} else {
			j->have_dc |= 1 << t;
		}
		len -= 17 + total;
	}
	return len ? -EINVAL : 0;
}

static int read_sof(struct jpeg *j, int len)
{
	int bits = get8(j);

	j->height = get16(j);
	j->width = get16(j);
	j->ncomp = get8(j);
	if (bits < 0 || j->height < 0 || j->width < 0 || j->ncomp < 0)
		return -EINVAL;
	if (bits != 8 || !j->height)
		return -ENOTSUP;		/* 12-bit samples, or a height given later */
	if (!j->width || len != 6 + 3 * j->ncomp)
		return -EINVAL;
	if (j->ncomp != 1 && j->ncomp != 3)
		return -ENOTSUP;		/* CMYK */
	for (int i = 0; i < j->ncomp; i++) {
		struct jpeg_comp *c = &j->comp[i];
		int hv;

		c->id = get8(j);
		hv = get8(j);
		c->tq = get8(j);
		if (c->id < 0 || hv < 0 || c->tq < 0 || c->tq > 3)
			return -EINVAL;
		c->h = hv >> 4;
		c->v = hv & 15;
		if (c->h < 1 || c->v < 1)
			return -EINVAL;
	}
	if (j->ncomp == 1) {
		j->comp[0].h = j->comp[0].v = 1;	/* one block is the MCU, whatever it says */
	} else if (j->comp[0].h > 2 || j->comp[0].v > 2 ||
		   j->comp[1].h != 1 || j->comp[1].v != 1 ||
		   j->comp[2].h != 1 || j->comp[2].v != 1) {
		return -ENOTSUP;
	}
	j->hmax = j->comp[0].h;
	j->vmax = j->comp[0].v;
	j->mcux = (j->width + 8 * j->hmax - 1) / (8 * j->hmax);
	j->mcuy = (j->height + 8 * j->vmax - 1) / (8 * j->vmax);
	return 0;
}

/* Only the one-scan kind: every component, interleaved, in frame order. */
static int read_sos(struct jpeg *j, int len)
{
	int n = get8(j);

	if (n < 0 || !j->ncomp)
		return -EINVAL;
	if (n != j->ncomp)
		return -ENOTSUP;		/* one component per scan */
	if (len != 4 + 2 * n)
		return -EINVAL;
	for (int i = 0; i < n; i++) {
		struct jpeg_comp *c = &j->comp[i];
		int id = get8(j), t = get8(j);

		if (id < 0 || t < 0)
			return -EINVAL;
		if (id != c->id)
			return -ENOTSUP;
		c->td = t >> 4;
		c->ta = t & 15;
		if (c->td > 1 || c->ta > 1)
			return -ENOTSUP;
		if (!(j->have_dc >> c->td & 1) || !(j->have_ac >> c->ta & 1) ||
		    !(j->have_qt >> c->tq & 1))
			return -EINVAL;		/* a table it needs was never given */
		c->pred = 0;
	}
	int ss = get8(j), se = get8(j), a = get8(j);

	if (ss < 0 || se < 0 || a < 0)
		return -EINVAL;
	if (ss != 0 || se != 63 || a != 0)
		return -ENOTSUP;		/* progressive */
	j->bits = 0;
	j->nbits = 0;
	j->marker = 0;
	return 0;
}

int jpeg_open(struct jpeg *j)
{
	j->width = j->height = j->ncomp = 0;
	j->restart = 0;
	j->have_dc = j->have_ac = j->have_qt = 0;
	j->eof = 0;
	if (get8(j) != 0xff || get8(j) != M_SOI)
		return -EINVAL;
	for (;;) {
		int m, len, ret;

		/* the next marker: 0xff, perhaps more of them, then its code */
		if ((m = get8(j)) != 0xff)
			return -EINVAL;
		while ((m = get8(j)) == 0xff)
			;
		if (m < 0 || m == M_EOI)
			return -EINVAL;
		if ((m >= 0xd0 && m <= 0xd7) || m == 0x01)
			continue;		/* markers with nothing after them */
		if ((len = get16(j)) < 2)
			return -EINVAL;
		len -= 2;
		switch (m) {
		case M_SOF0:
		case M_SOF1:
			ret = j->ncomp ? -EINVAL : read_sof(j, len);
			break;
		case M_DHT:
			ret = read_dht(j, len);
			break;
		case M_DQT:
			ret = read_dqt(j, len);
			break;
		case M_DRI:
			j->restart = get16(j);
			ret = len == 2 && j->restart >= 0 ? 0 : -EINVAL;
			break;
		case M_SOS:
			return read_sos(j, len);
		default:
			if (m >= 0xc2 && m <= 0xcf && m != 0xc4 && m != 0xc8)
				return -ENOTSUP;	/* progressive, lossless, arithmetic */
			ret = skip(j, len);	/* APPn, COM, anything else */
			break;
		}
		if (ret)
			return ret;
	}
}

int jpeg_scaled_w(const struct jpeg *j, int scale)
{
	return (j->width + (1 << scale) - 1) >> scale;
}

int jpeg_scaled_h(const struct jpeg *j, int scale)
{
	return (j->height + (1 << scale) - 1) >> scale;
}

size_t jpeg_band_size(const struct jpeg *j, int scale)
{
	return (size_t)j->mcux * ((8 * j->hmax) >> scale) * ((8 * j->vmax) >> scale) * 2;
}

/* ------------------------------------------------------------ entropy */

/*
 * At least 25 bits in hand afterwards. A marker is where the data stops:
 * from there on the decoder is fed zeros, and whatever the marker is it is
 * kept for restart() to look at.
 */
static HOT void fill(struct jpeg *j)
{
	while (j->nbits <= 24) {
		int c = 0;

		if (!j->marker) {
			c = get8(j);
			if (c == 0xff) {
				int m;

				while ((m = get8(j)) == 0xff)
					;
				if (m) {
					j->marker = m < 0 ? M_EOI : m;
					c = 0;
				}
			} else if (c < 0) {
				j->marker = M_EOI;
				c = 0;
			}
		}
		j->bits |= (uint32_t)c << (24 - j->nbits);
		j->nbits += 8;
	}
}

static inline void consume(struct jpeg *j, int n)
{
	j->bits <<= n;
	j->nbits -= n;
}

static inline int huff_decode(struct jpeg *j, const struct jpeg_huff *h)
{
	uint32_t top;
	int k, len;

	if (j->nbits < 16)
		fill(j);
	k = h->fast[j->bits >> (32 - JPEG_FAST)];
	if (k < 255) {
		consume(j, h->size[k]);
		return h->values[k];
	}
	top = j->bits >> 16;
	for (len = JPEG_FAST + 1; top >= h->maxcode[len]; len++)
		;
	if (len > 16) {
		consume(j, 16);			/* not a code in this table */
		return 0;
	}
	k = (int)(j->bits >> (32 - len)) + h->delta[len];
	consume(j, len);
	return h->values[k & 255];
}

/* `s` more bits, as the signed value they stand for (s is 1 to 16). */
static inline int receive(struct jpeg *j, int s)
{
	uint32_t v;

	if (j->nbits < s)
		fill(j);
	v = j->bits >> (32 - s);
	consume(j, s);
	return (int)v - (v >> (s - 1) ? 0 : (1 << s) - 1);
}

/*
 * One block's coefficients, dequantised, in natural order. Returns 0 when
 * there is nothing but the DC, otherwise one past the zigzag position of
 * the last coefficient.
 */
static int decode_block(struct jpeg *j, struct jpeg_comp *c)
{
	const struct jpeg_huff *ac = &j->ac[c->ta];
	const int16_t *fac = j->fast_ac[c->ta];
	const uint32_t *q = j->qs[c->tq];
	int16_t *out = j->coef;
	int t, k = 1, last = 0;

	for (int i = 0; i < 32; i++)
		((uint32_t *)out)[i] = 0;
	/* Kept to 16 bits, as the coefficients are: real data never needs
	 * more, and damaged data then cannot overflow anything. */
	t = huff_decode(j, &j->dc[c->td]);
	if (t)
		c->pred = (int16_t)(c->pred + receive(j, t > 16 ? 16 : t));
	out[0] = (int16_t)((uint32_t)c->pred * q[0]);
	do {
		int r, z;

		if (j->nbits < 16)
			fill(j);
		r = fac[j->bits >> (32 - JPEG_FAST)];
		if (r) {
			consume(j, r & 15);
			k += (r >> 4) & 15;
			z = dezigzag[k++];
			out[z] = (int16_t)((r >> 8) * q[z]);
			last = k;
		} else {
			int rs = huff_decode(j, ac), s = rs & 15;

			if (!s) {
				if (rs != 0xf0)
					break;	/* end of block */
				k += 16;	/* sixteen zeros */
				continue;
			}
			k += rs >> 4;
			z = dezigzag[k++];
			out[z] = (int16_t)((uint32_t)receive(j, s) * q[z]);
			last = k;
		}
	} while (k < 64);
	return last;
}

/*
 * Between restart intervals the data is byte-aligned and the DC predictors
 * start again. The marker is usually already in hand, found by fill(); if
 * the data was damaged and it is not where it should be, look for it.
 */
static void restart(struct jpeg *j)
{
	while (!j->marker) {
		int c = get8(j);

		if (c < 0) {
			j->marker = M_EOI;
		} else if (c == 0xff) {
			int m;

			while ((m = get8(j)) == 0xff)
				;
			if (m)
				j->marker = m < 0 ? M_EOI : m;
		}
	}
	if (j->marker >= 0xd0 && j->marker <= 0xd7)
		j->marker = 0;			/* RSTn: carry on after it */
	j->bits = 0;
	j->nbits = 0;
	for (int i = 0; i < j->ncomp; i++)
		j->comp[i].pred = 0;
}

/* ------------------------------------------------------------ IDCT */

/*
 * The AAN IDCT (Arai, Agui and Nakajima; libjpeg's jidctfst.c): five
 * multiplies a pass where the accurate one needs twelve, because the rest
 * of its scaling is folded into the quantisation table (see
 * scale_tables()), and few enough values alive at once for the sixteen
 * registers this core can see. It is a little less exact than libjpeg's
 * default, well inside one step of RGB565.
 *
 * The arithmetic is unsigned, where overflow wraps rather than being
 * undefined: a real picture never comes near overflowing, but damaged
 * coefficients can, and then the block is merely wrong. The instructions
 * are the same; only the shifts, which must be signed, need casts.
 */
#define MUL(v, c)	((uint32_t)((int32_t)((uint32_t)(v) * (uint32_t)(c)) >> 8))
#define ROUND		((128u << 5) + 16)	/* the encoder's 128, and a half, both << 5 */

/*
 * One dimension on s0..s7; `bias` is added to what every output depends
 * on, which is how the rounding costs nothing. Output 0 is t0 + t7, 7 is
 * t0 - t7, 1 and 6 are t1 +- t6, 2 and 5 are t2 +- t5, 4 and 3 t3 +- t4.
 */
#define AAN_1D(s0, s1, s2, s3, s4, s5, s6, s7, bias)				\
	uint32_t t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13;		\
	uint32_t z5, z10, z11, z12, z13;					\
	t10 = (uint32_t)(s0) + (uint32_t)(s4) + (bias);				\
	t11 = (uint32_t)(s0) - (uint32_t)(s4) + (bias);				\
	t13 = (uint32_t)(s2) + (uint32_t)(s6);					\
	t12 = MUL((uint32_t)(s2) - (uint32_t)(s6), 362) - t13;	/* 2 c4 */	\
	t0 = t10 + t13;								\
	t3 = t10 - t13;								\
	t1 = t11 + t12;								\
	t2 = t11 - t12;								\
	z13 = (uint32_t)(s5) + (uint32_t)(s3);					\
	z10 = (uint32_t)(s5) - (uint32_t)(s3);					\
	z11 = (uint32_t)(s1) + (uint32_t)(s7);					\
	z12 = (uint32_t)(s1) - (uint32_t)(s7);					\
	t7 = z11 + z13;								\
	t11 = MUL(z11 - z13, 362);				/* 2 c4 */	\
	z5 = MUL(z10 + z12, 473);				/* 2 c2 */	\
	t10 = MUL(z12, 277) - z5;				/* 2 (c2 - c6) */ \
	t12 = MUL(z10, -669) + z5;				/* -2 (c2 + c6) */ \
	t6 = t12 - t7;								\
	t5 = t11 - t6;								\
	t4 = t10 + t5

#define COLUMN_OUT(w, n)							\
	do {									\
		w[0] = t0 + t7;							\
		w[7 * n] = t0 - t7;						\
		w[n] = t1 + t6;							\
		w[6 * n] = t1 - t6;						\
		w[2 * n] = t2 + t5;						\
		w[5 * n] = t2 - t5;						\
		w[4 * n] = t3 + t4;						\
		w[3 * n] = t3 - t4;						\
	} while (0)

#define ROW_OUT(o)								\
	do {									\
		o[0] = clamp8((int32_t)(t0 + t7) >> 5);				\
		o[7] = clamp8((int32_t)(t0 - t7) >> 5);				\
		o[1] = clamp8((int32_t)(t1 + t6) >> 5);				\
		o[6] = clamp8((int32_t)(t1 - t6) >> 5);				\
		o[2] = clamp8((int32_t)(t2 + t5) >> 5);				\
		o[5] = clamp8((int32_t)(t2 - t5) >> 5);				\
		o[4] = clamp8((int32_t)(t3 + t4) >> 5);				\
		o[3] = clamp8((int32_t)(t3 - t4) >> 5);				\
	} while (0)

static inline uint8_t clamp8(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* The one sample a block with nothing but a DC is made of. */
static inline uint8_t flat(int16_t dc)
{
	return clamp8((int32_t)((uint32_t)dc + ROUND) >> 5);
}

/*
 * The AAN scale factors, cos(k pi / 16) sqrt 2 for row and column, in
 * 14-bit fixed point. Multiplied into the quantisation table they turn
 * the dequantising multiply, which has to happen anyway, into the first
 * step of the transform; the extra 2 bits of the result are the headroom
 * the column pass keeps.
 */
static const uint16_t aan_scale[64] = {
	16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
	22725, 31521, 29692, 26722, 22725, 17855, 12299,  6270,
	21407, 29692, 27969, 25172, 21407, 16819, 11585,  5906,
	19266, 26722, 25172, 22654, 19266, 15137, 10426,  5315,
	16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
	12873, 17855, 16819, 15137, 12873, 10114,  6967,  3552,
	 8867, 12299, 11585, 10426,  8867,  6967,  4799,  2446,
	 4520,  6270,  5906,  5315,  4520,  3552,  2446,  1247,
};

static void scale_tables(struct jpeg *j)
{
	for (int t = 0; t < 4; t++)
		if (j->have_qt >> t & 1)
			for (int i = 0; i < 64; i++)
				j->qs[t][i] = ((uint32_t)j->qt[t][i] * aan_scale[i] + (1 << 11)) >> 12;
}

/*
 * Nothing outside the top-left 4x4 of coefficients, which is where the
 * zigzag has been by its tenth -- a block with a little detail. The same
 * transform with half its inputs known to be zero, which the compiler
 * folds away, and the four columns on the right not there to do at all.
 */
static HOT void idct_4x4(const int16_t *in, uint8_t *out, int stride)
{
	uint32_t ws[32];

	for (int x = 0; x < 4; x++) {
		const int16_t *c = in + x;
		uint32_t *w = ws + x;

		if (!(c[8] | c[16] | c[24])) {
			w[0] = w[4] = w[8] = w[12] = w[16] = w[20] = w[24] = w[28] = (uint32_t)c[0];
			continue;
		}
		AAN_1D(c[0], c[8], c[16], c[24], 0, 0, 0, 0, 0);
		COLUMN_OUT(w, 4);
	}
	for (int y = 0; y < 8; y++) {
		const uint32_t *r = ws + y * 4;
		uint8_t *o = out + y * stride;
		AAN_1D(r[0], r[1], r[2], r[3], 0, 0, 0, 0, ROUND);
		ROW_OUT(o);
	}
}

/* j->coef to 8x8 samples at `out`, rows `stride` apart. */
static HOT void idct(const int16_t *in, int last, uint8_t *out, int stride)
{
	uint32_t ws[64];

	if (!last) {
		uint32_t v = flat(in[0]) * 0x01010101u;

		for (int y = 0; y < 8; y++) {
			uint32_t *row = (uint32_t *)(out + y * stride);

			row[0] = row[1] = v;
		}
		return;
	}
	if (last <= 10) {
		idct_4x4(in, out, stride);
		return;
	}
	for (int x = 0; x < 8; x++) {
		const int16_t *c = in + x;
		uint32_t *w = ws + x;

		if (!(c[8] | c[16] | c[24] | c[32] | c[40] | c[48] | c[56])) {
			w[0] = w[8] = w[16] = w[24] = w[32] = w[40] = w[48] = w[56] = (uint32_t)c[0];
			continue;
		}
		AAN_1D(c[0], c[8], c[16], c[24], c[32], c[40], c[48], c[56], 0);
		COLUMN_OUT(w, 8);
	}
	for (int y = 0; y < 8; y++) {
		const uint32_t *r = ws + y * 8;
		uint8_t *o = out + y * stride;

		if (!(r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) {
			uint32_t v = clamp8((int32_t)(r[0] + ROUND) >> 5) * 0x01010101u;

			((uint32_t *)o)[0] = ((uint32_t *)o)[1] = v;
			continue;
		}
		AAN_1D(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], ROUND);
		ROW_OUT(o);
	}
}

/* ------------------------------------------------------------ colour */

/* The chroma terms of ITU-R BT.601, full range, as JFIF has it. */
#define CHROMA(cb, cr, rv, gv, bv)						\
	do {									\
		int cb_ = (cb) - 128, cr_ = (cr) - 128;				\
		rv = (cr_ * 91881 + 32768) >> 16;				\
		gv = (cb_ * -22554 + cr_ * -46802 + 32768) >> 16;		\
		bv = (cb_ * 116130 + 32768) >> 16;				\
	} while (0)

/*
 * The pixel tables. A pixel goes to the panel as RGB565 with the high byte
 * first, which as a little-endian halfword is GGGBBBBB RRRRRGGG; each table
 * holds its colour's bits of that for every value a channel can reach
 * before clamping, so the clamping costs nothing either.
 */
static void build_rgb(struct jpeg *j)
{
	for (int i = 0; i < 768; i++) {
		unsigned c = clamp8(i - 256);

		j->rgb[0][i] = (uint16_t)(c & 0xf8);
		j->rgb[1][i] = (uint16_t)(c >> 5 | (c & 0x1c) << 11);
		j->rgb[2][i] = (uint16_t)((c & 0xf8) << 5);
	}
	for (int i = 0; i < 256; i++) {
		int rv, gv, bv;

		CHROMA(128, i, rv, gv, bv);		/* Cr's part */
		j->red[i] = j->rgb[0] + 256 + rv;
		j->gcr[i] = (int16_t)gv;
		CHROMA(i, 128, rv, gv, bv);		/* Cb's */
		j->blue[i] = j->rgb[2] + 256 + bv;
		j->gcb[i] = (int16_t)gv;
	}
}

#define PIXEL(lum, rv, gv, bv)	(tr[(lum) + (rv)] | tg[(lum) + (gv)] | tb[(lum) + (bv)])

/*
 * The common case, and the one video is: 4:2:0 at full size. Each chroma
 * sample sets where in the tables its four pixels look, and each pixel is
 * then its luma and three loads; two pixels go out as one word.
 */
static void put_420(const struct jpeg *j, uint8_t *dst, size_t stride)
{
	for (int cy = 0; cy < 8; cy++) {
		const uint8_t *y0 = j->y + cy * 32, *y1 = y0 + 16;
		const uint8_t *cb = j->cb + cy * 8, *cr = j->cr + cy * 8;
		uint32_t *d0 = (uint32_t *)(dst + 2 * cy * stride);
		uint32_t *d1 = (uint32_t *)(dst + (2 * cy + 1) * stride);

		for (int cx = 0; cx < 8; cx++, y0 += 2, y1 += 2) {
			const uint16_t *r = j->red[cr[cx]], *b = j->blue[cb[cx]];
			const uint16_t *g = j->rgb[1] + 256 + j->gcb[cb[cx]] + j->gcr[cr[cx]];

			d0[cx] = (uint32_t)(r[y0[0]] | g[y0[0]] | b[y0[0]]) |
				 (uint32_t)(r[y0[1]] | g[y0[1]] | b[y0[1]]) << 16;
			d1[cx] = (uint32_t)(r[y1[0]] | g[y1[0]] | b[y1[0]]) |
				 (uint32_t)(r[y1[1]] | g[y1[1]] | b[y1[1]]) << 16;
		}
	}
}

/*
 * Anything else: other samplings, greyscale, and the smaller scales. Every
 * plane is already at the size it is needed at (see set_scale()), so a
 * pixel is one luma sample and the chroma sample that covers it.
 */
static void put_any(const struct jpeg *j, int mw, int mh, uint8_t *dst, size_t stride)
{
	const uint16_t *tr = j->rgb[0] + 256, *tg = j->rgb[1] + 256, *tb = j->rgb[2] + 256;
	const struct jpeg_comp *c = &j->comp[1];

	for (int oy = 0; oy < mh; oy++) {
		const uint8_t *lum = j->y + oy * mw;
		uint16_t *d = (uint16_t *)(dst + oy * stride);

		if (j->ncomp == 1) {
			for (int ox = 0; ox < mw; ox++)
				d[ox] = PIXEL(lum[ox], 0, 0, 0);
			continue;
		}
		const uint8_t *cb = j->cb + (oy >> c->uy) * c->bw, *cr = j->cr + (oy >> c->uy) * c->bw;

		for (int ox = 0; ox < mw; ox++) {
			int rv, gv, bv;

			CHROMA(cb[ox >> c->ux], cr[ox >> c->ux], rv, gv, bv);
			d[ox] = PIXEL(lum[ox], rv, gv, bv);
		}
	}
}

/* ------------------------------------------------------------ decode */

/*
 * A block made smaller by averaging: the box filter that scaling by a
 * power of two wants, rather than keeping every other sample and letting
 * the rest alias. An eighth in both directions is exactly the DC.
 */
static void idct_scaled(const int16_t *in, int last, uint8_t *out, int stride, int rx, int ry)
{
	int w = 8 >> rx, h = 8 >> ry, n = rx + ry;
	uint8_t full[64];

	if (!n) {
		idct(in, last, out, stride);
		return;
	}
	if (!last || n == 6) {
		uint8_t v = flat(in[0]);

		for (int y = 0; y < h; y++)
			memset(out + y * stride, v, (size_t)w);
		return;
	}
	idct(in, last, full, 8);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int sum = 0;

			for (int dy = 0; dy < 1 << ry; dy++)
				for (int dx = 0; dx < 1 << rx; dx++)
					sum += full[((y << ry) + dy) * 8 + (x << rx) + dx];
			out[y * stride + x] = (uint8_t)((sum + (1 << n >> 1)) >> n);
		}
	}
}

/*
 * How each component meets the picture at this scale. Luma is averaged
 * down by the scale. Chroma already has half the resolution in each
 * direction it is subsampled in, so it is averaged down by one step less
 * and, at full size, stretched instead -- which is also how libjpeg does
 * it, and keeps a scaled photo's colour as sharp as its detail.
 */
static void set_scale(struct jpeg *j, int scale)
{
	for (int i = 0; i < j->ncomp; i++) {
		struct jpeg_comp *c = &j->comp[i];
		int hx = scale - (j->hmax / c->h - 1), hy = scale - (j->vmax / c->v - 1);

		c->rx = hx > 0 ? hx : 0;
		c->ry = hy > 0 ? hy : 0;
		c->ux = hx < 0 ? -hx : 0;
		c->uy = hy < 0 ? -hy : 0;
		c->bw = c->h * (8 >> c->rx);
	}
}

static void decode_mcu(struct jpeg *j)
{
	for (int i = 0; i < j->ncomp; i++) {
		struct jpeg_comp *c = &j->comp[i];
		uint8_t *plane = i == 0 ? j->y : i == 1 ? j->cb : j->cr;
		int bh = 8 >> c->ry;

		for (int by = 0; by < c->v; by++) {
			for (int bx = 0; bx < c->h; bx++) {
				int last = decode_block(j, c);

				idct_scaled(j->coef, last, plane + by * bh * c->bw + bx * (8 >> c->rx),
					    c->bw, c->rx, c->ry);
			}
		}
	}
}

int jpeg_decode(struct jpeg *j, int scale, uint8_t *band, jpeg_band_fn fn, void *ctx)
{
	int mw = (8 * j->hmax) >> scale, mh = (8 * j->vmax) >> scale;
	int sh = jpeg_scaled_h(j, scale), todo = j->restart;
	size_t stride = (size_t)j->mcux * mw * 2;
	int fast = !scale && j->ncomp == 3 && j->hmax == 2 && j->vmax == 2;

	if (scale < 0 || scale > 3 || !j->ncomp)
		return -EINVAL;
	set_scale(j, scale);
	scale_tables(j);
	build_rgb(j);
	for (int my = 0; my < j->mcuy; my++) {
		int y = my * mh, ret;

		for (int mx = 0; mx < j->mcux; mx++) {
			if (j->restart) {
				if (!todo) {
					restart(j);
					todo = j->restart;
				}
				todo--;
			}
			decode_mcu(j);
			if (fast)
				put_420(j, band + mx * mw * 2, stride);
			else
				put_any(j, mw, mh, band + mx * mw * 2, stride);
		}
		if ((ret = fn(ctx, y, sh - y < mh ? sh - y : mh, band, stride)))
			return ret;
	}
	return j->eof ? -EINVAL : 0;		/* it ran out before the end */
}

