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
#define M_SOF2		0xc2		/* progressive */
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

		if (tc < 0 || tc >> 4 > 1 || t > 3)
			return -EINVAL;
		if (t > 1 && !j->progressive)
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
		if (t > 1) {			/* a progressive file's extra AC tables: unused here */
			if (skip(j, total))
				return -EINVAL;
			len -= 17 + total;
			continue;
		}
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

/*
 * A progressive scan's header. DC scans are what is read, so they must
 * name tables that exist; the rest are passed over and need nothing.
 */
static int read_scan(struct jpeg *j, int len)
{
	int n = get8(j), a;

	if (n < 1 || n > j->ncomp || len != 4 + 2 * n)
		return -EINVAL;
	for (int i = 0; i < n; i++) {
		int id = get8(j), t = get8(j), k;

		for (k = 0; k < j->ncomp && j->comp[k].id != id; k++)
			;
		if (id < 0 || t < 0 || k == j->ncomp)
			return -EINVAL;
		j->scan[i] = (uint8_t)k;
		j->comp[k].td = t >> 4;
		j->comp[k].pred = 0;
	}
	j->scan_n = n;
	j->ss = get8(j);
	j->se = get8(j);
	a = get8(j);
	if (j->ss < 0 || j->se < 0 || a < 0)
		return -EINVAL;
	j->ah = a >> 4;
	j->al = a & 15;
	if (!j->ss) {
		if (j->se || j->al > 13)
			return -EINVAL;
		for (int i = 0; i < n; i++) {
			const struct jpeg_comp *c = &j->comp[j->scan[i]];

			if (!(j->have_qt >> c->tq & 1) ||
			    (!j->ah && (c->td > 1 || !(j->have_dc >> c->td & 1))))
				return -EINVAL;
		}
	}
	j->bits = 0;
	j->nbits = 0;
	j->marker = 0;
	return 0;
}

int jpeg_open(struct jpeg *j)
{
	j->width = j->height = j->ncomp = 0;
	j->progressive = 0;
	j->blocks = NULL;
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
		case M_SOF2:
			j->progressive = m == M_SOF2;
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
			return j->progressive ? read_scan(j, len) : read_sos(j, len);
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

size_t jpeg_dc_size(const struct jpeg *j)
{
	size_t n = 0;

	if (!j->progressive)
		return 0;
	for (int i = 0; i < j->ncomp; i++)
		n += (size_t)j->mcux * j->comp[i].h * j->mcuy * j->comp[i].v;
	return n * sizeof(*j->blocks);
}

/* ------------------------------------------------------------ entropy */

/*
 * At least 25 bits in hand afterwards. A marker is where the data stops:
 * from there on the decoder is fed zeros, and whatever the marker is it is
 * kept for restart() to look at.
 */
static HOT void fill(struct jpeg *j)
{
	/*
	 * The usual case first, with the bit buffer kept in registers: no
	 * marker yet and the bytes there to read -- four, the most it can
	 * take -- so that only a 0xff needs the careful way below.
	 */
	if (!j->marker && j->end - j->p >= 4) {
		const uint8_t *p = j->p;
		uint32_t bits = j->bits;
		int nbits = j->nbits;

		do {
			uint32_t c = *p;

			if (c == 0xff)
				break;
			p++;
			bits |= c << (24 - nbits);
			nbits += 8;
		} while (nbits <= 24);
		j->p = p;
		j->bits = bits;
		j->nbits = nbits;
	}
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
 * One block's coefficients, dequantised, in natural order, into j->coef,
 * which is all zeros before (see clear_block()). Returns 0 when there is
 * nothing but the DC, otherwise one past the zigzag position of the last
 * coefficient.
 */
static int decode_block(struct jpeg *j, struct jpeg_comp *c)
{
	const struct jpeg_huff *ac = &j->ac[c->ta];
	const int16_t *fac = j->fast_ac[c->ta];
	const uint32_t *q = j->qs[c->tq];
	int16_t *out = j->coef;
	int t, k = 1, last = 0;

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

#define CLEAR_WALK	8	/* zigzag positions put back one at a time, at most */
#define CLEAR4(w, i)	(w[i] = w[i + 1] = w[i + 2] = w[i + 3] = 0)

/*
 * A block's coefficients back to zeros once the IDCT has them, ready for
 * the next. A block that stopped early has only the places it reached put
 * back; any other is cleared a word at a time, written out because as a
 * loop the compiler makes it a memset() and, the builtin being off in the
 * firmware, a loop of bytes, four times the work.
 */
static inline void clear_block(int16_t *coef, int last)
{
	uint32_t *w = (uint32_t *)coef;

	if (last <= CLEAR_WALK) {
		w[0] = 0;			/* the first two places */
		for (int k = 2; k < last; k++)
			coef[dezigzag[k]] = 0;
		return;
	}
	CLEAR4(w, 0);
	CLEAR4(w, 4);
	CLEAR4(w, 8);
	CLEAR4(w, 12);
	CLEAR4(w, 16);
	CLEAR4(w, 20);
	CLEAR4(w, 24);
	CLEAR4(w, 28);
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

#define NEUTRAL_BELOW	32	/* of 255: green steps with red and blue: build_rgb() */

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

		/*
		 * Green's step is half red's and blue's, and a grey whose
		 * green is a step up from theirs is green: on black, a speck.
		 * In the darkest shades it steps with them, so a grey stays
		 * grey. Above, it has its finer steps; its value is taken two
		 * down there because it is dithered by the same pattern as
		 * red and blue, twice its own step (see put_420()).
		 */
		unsigned g = c < NEUTRAL_BELOW ? (c >> 3) << 1 : (c - 2) >> 2;

		j->rgb[0][i] = (uint16_t)(c & 0xf8);
		j->rgb[1][i] = (uint16_t)(g >> 3 | (g & 7) << 13);
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
 *
 * The four are also the cells of a 2x2 ordered dither. RGB565 keeps five
 * bits of red and blue and six of green, and cutting the rest off makes
 * a sky or a dark room into bands of flat colour -- blocky, and the wrong
 * colour at the edges of the bands. Adding an eighth, three, five, seven
 * eighths of a step in a fixed pattern before the cut leaves each
 * pixel's colour as it was on average, and the eye does the averaging.
 * The offsets are in the tables' units, a step of red and blue being 8,
 * and all three colours take the same one: dithered by patterns of their
 * own, each pixel rounded its colours different ways, and a dark grey
 * came out as specks of green and purple. The tables reach far enough
 * past 255 to take the offsets.
 *
 * Apply the same rounding in shadows as everywhere else. Suppressing
 * dither below luma 8 crushed those shades to solid black; stripping
 * their chroma also made dark scenes grey. Preserve the source's colour
 * and shadow detail. Actual black stays black (the largest offset is 7).
 */
#define DOT(i)		(r[i] | g[i] | b[i])
#define DITHER(y, o)	DOT((y) + (o))

static void put_420(const struct jpeg *j, uint8_t *dst, size_t stride)
{
	for (int cy = 0; cy < 8; cy++) {
		const uint8_t *y0 = j->y + cy * 32, *y1 = y0 + 16;
		const uint8_t *cb = j->cb + cy * 8, *cr = j->cr + cy * 8;
		uint32_t *d0 = (uint32_t *)(dst + 2 * cy * stride);
		uint32_t *d1 = (uint32_t *)(dst + (2 * cy + 1) * stride);

		for (int cx = 0; cx < 8; cx++, y0 += 2, y1 += 2) {
			int u = cb[cx], v = cr[cx];
			const uint16_t *r = j->red[v], *b = j->blue[u];
			const uint16_t *g = j->rgb[1] + 256 + j->gcb[u] + j->gcr[v];

			d0[cx] = (uint32_t)DITHER(y0[0], 1) | (uint32_t)DITHER(y0[1], 5) << 16;
			d1[cx] = (uint32_t)DITHER(y1[0], 7) | (uint32_t)DITHER(y1[1], 3) << 16;
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
				clear_block(j->coef, last);
			}
		}
	}
}

/* ------------------------------------------------------------ progressive */

/*
 * The data after a scan, up to the marker that ends it: a scan that is
 * not read, or the last bits of one that was. Restart markers are a part
 * of the scan.
 */
static void to_marker(struct jpeg *j)
{
	for (;;) {
		int c, m;

		if (j->marker && (j->marker < 0xd0 || j->marker > 0xd7))
			return;
		j->marker = 0;
		if ((c = get8(j)) < 0) {
			j->marker = M_EOI;
			return;
		}
		if (c != 0xff)
			continue;
		while ((m = get8(j)) == 0xff)
			;
		if (m < 0)
			j->marker = M_EOI;
		else if (m)
			j->marker = m;		/* 0 is a stuffed 0xff */
	}
}

/* The markers between scans, tables among them: 1 for the next scan, 0 at the end. */
static int next_scan(struct jpeg *j)
{
	for (;;) {
		int m = j->marker, len, ret;

		j->marker = 0;
		if (m == M_EOI)
			return 0;
		if ((len = get16(j)) < 2)
			return -EINVAL;
		len -= 2;
		switch (m) {
		case M_SOS:
			return (ret = read_scan(j, len)) ? ret : 1;
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
		default:
			ret = skip(j, len);
			break;
		}
		if (ret)
			return ret;
		if (get8(j) != 0xff)
			return -EINVAL;
		while ((m = get8(j)) == 0xff)
			;
		if (m <= 0)
			return -EINVAL;
		j->marker = m;
	}
}

/* A block's DC in a DC scan: its first bits, or one more of them. */
static void dc_bits(struct jpeg *j, struct jpeg_comp *c, int16_t *v)
{
	if (!j->ah) {
		int t = huff_decode(j, &j->dc[c->td]);

		if (t)
			c->pred = (int16_t)(c->pred + receive(j, t > 16 ? 16 : t));
		*v = (int16_t)((uint32_t)c->pred * (1u << j->al));
		return;
	}
	if (j->nbits < 1)
		fill(j);
	if (j->bits >> 31)
		*v = (int16_t)(*v | 1 << j->al);
	consume(j, 1);
}

/*
 * A DC scan into the value each block keeps. With one component the scan
 * goes block by block over that component's own picture; with several,
 * MCU by MCU like a baseline scan.
 */
static void dc_scan(struct jpeg *j, int16_t *const dc[3], const int bw[3])
{
	int todo = j->restart;

	if (j->scan_n == 1) {
		int k = j->scan[0];
		struct jpeg_comp *c = &j->comp[k];
		int w = ((j->width * c->h + j->hmax - 1) / j->hmax + 7) / 8;
		int h = ((j->height * c->v + j->vmax - 1) / j->vmax + 7) / 8;

		for (int by = 0; by < h && !j->eof; by++) {
			for (int bx = 0; bx < w; bx++) {
				if (j->restart) {
					if (!todo) {
						restart(j);
						todo = j->restart;
					}
					todo--;
				}
				dc_bits(j, c, &dc[k][by * bw[k] + bx]);
			}
		}
		return;
	}
	for (int my = 0; my < j->mcuy && !j->eof; my++) {
		for (int mx = 0; mx < j->mcux; mx++) {
			if (j->restart) {
				if (!todo) {
					restart(j);
					todo = j->restart;
				}
				todo--;
			}
			for (int s = 0; s < j->scan_n; s++) {
				int k = j->scan[s];
				struct jpeg_comp *c = &j->comp[k];

				for (int by = 0; by < c->v; by++)
					for (int bx = 0; bx < c->h; bx++)
						dc_bits(j, c, &dc[k][(my * c->v + by) * bw[k] + mx * c->h + bx]);
			}
		}
	}
}

/*
 * Every scan in turn: the DC ones read, the rest passed over. Then each
 * block's one value is its pixel at an eighth, and the colour goes as it
 * does at any scale (put_any()).
 */
static int decode_progressive(struct jpeg *j, uint8_t *band, jpeg_band_fn fn, void *ctx)
{
	int mw = j->hmax, mh = j->vmax, sh = jpeg_scaled_h(j, 3), bw[3], ret = 0;
	size_t stride = (size_t)j->mcux * mw * 2;
	int16_t *dc[3], *p = j->blocks;

	memset(j->blocks, 0, jpeg_dc_size(j));
	for (int i = 0; i < j->ncomp; i++) {
		bw[i] = j->mcux * j->comp[i].h;
		dc[i] = p;
		p += (size_t)bw[i] * j->mcuy * j->comp[i].v;
	}
	for (int scans = 0; scans < 256; scans++) {
		int next;

		if (!j->ss)
			dc_scan(j, dc, bw);
		if (j->eof && !j->ss) {
			ret = -EINVAL;		/* cut short in a scan that mattered */
			break;
		}
		to_marker(j);
		if ((next = next_scan(j)) <= 0) {
			ret = next;
			break;
		}
	}
	set_scale(j, 3);
	scale_tables(j);
	build_rgb(j);
	for (int my = 0; my < j->mcuy; my++) {
		int y = my * mh, err;

		for (int mx = 0; mx < j->mcux; mx++) {
			for (int i = 0; i < j->ncomp; i++) {
				struct jpeg_comp *c = &j->comp[i];
				uint8_t *plane = i == 0 ? j->y : i == 1 ? j->cb : j->cr;
				int w = 8 >> c->rx, h = 8 >> c->ry;

				for (int by = 0; by < c->v; by++) {
					for (int bx = 0; bx < c->h; bx++) {
						int16_t v = dc[i][(my * c->v + by) * bw[i] + mx * c->h + bx];
						uint8_t px = flat((int16_t)((uint32_t)v * j->qs[c->tq][0]));

						for (int r = 0; r < h; r++)
							memset(plane + (by * h + r) * c->bw + bx * w, px, (size_t)w);
					}
				}
			}
			put_any(j, mw, mh, band + mx * mw * 2, stride);
		}
		if ((err = fn(ctx, y, sh - y < mh ? sh - y : mh, band, stride)))
			return err;
	}
	return ret;
}

int jpeg_decode(struct jpeg *j, int scale, uint8_t *band, jpeg_band_fn fn, void *ctx)
{
	int mw = (8 * j->hmax) >> scale, mh = (8 * j->vmax) >> scale;
	int sh = jpeg_scaled_h(j, scale), todo = j->restart;
	size_t stride = (size_t)j->mcux * mw * 2;
	int fast = !scale && j->ncomp == 3 && j->hmax == 2 && j->vmax == 2;

	if (scale < 0 || scale > 3 || !j->ncomp)
		return -EINVAL;
	if (j->progressive)
		return scale == 3 && j->blocks ? decode_progressive(j, band, fn, ctx) : -ENOTSUP;
	set_scale(j, scale);
	scale_tables(j);
	build_rgb(j);
	memset(j->coef, 0, sizeof(j->coef));	/* and kept so: clear_block() */
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

