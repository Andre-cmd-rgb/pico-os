/*
 * FLAC: the free lossless audio codec, decoding only.
 *
 * Written from the format specification. The shape of a FLAC file:
 *
 *	"fLaC"  metadata blocks (the first is STREAMINFO)  frames...
 *
 * and each frame holds one block of samples for every channel. A channel
 * is not stored as samples but as a prediction plus the error the
 * prediction made: either a fixed polynomial (the first four differences)
 * or a linear predictor whose coefficients the encoder chose and wrote
 * down. Those errors are small, so they are Rice coded -- a few bits each,
 * with the low bits kept verbatim and the high bits in unary.
 *
 * So decoding is: read the predictor, read the errors, run the predictor
 * forward. Nothing is lost anywhere, which is why the output can be
 * checked against any other decoder bit for bit.
 */
#include <stdlib.h>
#include <string.h>

#include "codec.h"
#include <unistd.h>

#include "pt/sys.h"

#define BUF_BYTES	2048
#define MAX_CHANNELS	8
#define MAX_ORDER	32

/* ------------------------------------------------------------ bits */

/*
 * A bit reader over a file, with the two check sums the format uses
 * folded in as bytes arrive: CRC-8 over a frame header, CRC-16 over a
 * whole frame. Every field in the stream is read most-significant bit
 * first.
 */
struct bits {
	int	 fd;
	uint8_t	 buf[BUF_BYTES];
	int	 len, pos;
	uint64_t acc;		/* the next bits, right-aligned in `have` */
	int	 have;
	uint8_t	 crc8;
	uint16_t crc16;
	bool	 eof;
};

static uint8_t crc8_table[256];
static uint16_t crc16_table[256];

static void crc_tables(void)
{
	if (crc8_table[1])
		return;
	for (int i = 0; i < 256; i++) {
		uint8_t c8 = i;
		uint16_t c16 = i << 8;

		for (int b = 0; b < 8; b++) {
			c8 = c8 & 0x80 ? (c8 << 1) ^ 0x07 : c8 << 1;	/* x^8+x^2+x+1 */
			c16 = c16 & 0x8000 ? (c16 << 1) ^ 0x8005 : c16 << 1;
		}
		crc8_table[i] = c8;
		crc16_table[i] = c16;
	}
}

static int next_byte(struct bits *b)
{
	if (b->pos == b->len) {
		ssize_t n = pt_read(b->fd, b->buf, sizeof(b->buf));

		if (n <= 0) {
			b->eof = true;
			return -1;
		}
		b->len = n;
		b->pos = 0;
	}
	uint8_t byte = b->buf[b->pos++];

	b->crc8 = crc8_table[b->crc8 ^ byte];
	b->crc16 = (b->crc16 << 8) ^ crc16_table[(b->crc16 >> 8) ^ byte];
	return byte;
}

/* Keeps at least `n` bits ready; n is never more than 57. */
static bool fill(struct bits *b, int n)
{
	while (b->have < n) {
		int byte = next_byte(b);

		if (byte < 0)
			return false;
		b->acc = (b->acc << 8) | (unsigned)byte;
		b->have += 8;
	}
	return true;
}

static uint32_t get(struct bits *b, int n)
{
	if (!n)
		return 0;
	if (!fill(b, n))
		return 0;
	b->have -= n;
	return (uint32_t)((b->acc >> b->have) & (n == 32 ? 0xffffffffu : (1u << n) - 1));
}

static int32_t get_signed(struct bits *b, int n)
{
	uint32_t v = get(b, n);

	if (n < 32 && (v & (1u << (n - 1))))
		v |= ~((1u << n) - 1);
	return (int32_t)v;
}

/* Zeros before the next 1 bit: the high half of a Rice code. */
static int get_unary(struct bits *b)
{
	int zeros = 0;

	for (;;) {
		int chunk;
		uint32_t peek;

		if (!fill(b, 1))
			return -1;
		chunk = b->have < 32 ? b->have : 32;
		peek = (uint32_t)(b->acc >> (b->have - chunk));
		if (chunk < 32)
			peek &= (1u << chunk) - 1;
		if (peek) {
			int lead = __builtin_clz(peek) - (32 - chunk);

			b->have -= lead + 1;
			return zeros + lead;
		}
		zeros += chunk;
		b->have -= chunk;
	}
}

/*
 * Drops the rest of the current byte. After this the reader holds no
 * loaded bytes at all, which is what makes the check sums line up: every
 * byte folded into them is a byte the frame really used.
 */
static void align(struct bits *b)
{
	b->have = 0;
}

static bool seek_to(struct bits *b, off_t off)
{
	if (pt_lseek(b->fd, off, SEEK_SET) < 0)
		return false;
	b->len = b->pos = b->have = 0;
	b->eof = false;
	return true;
}

/* ------------------------------------------------------------ decoder */

struct flac {
	struct codec	 base;
	struct bits	 b;
	int		 bps;		/* bits per sample in the file */
	int		 max_block;
	int		 channels;	/* in the file; the caller sees at most 2 */
	int		 block;		/* samples in the block just decoded */
	int		 taken;		/* how many of them the caller has */
	bool		 broken;
	int32_t		*ch[MAX_CHANNELS];
};

static const int rate_code[] = {
	0, 88200, 176400, 192000, 8000, 16000, 22050, 24000,
	32000, 44100, 48000, 96000, 0, 0, 0, 0,
};
static const int bps_code[] = { 0, 8, 12, 0, 16, 20, 24, 32 };

/* The fixed predictors: the sample, and the first four differences. */
static int32_t predict_fixed(const int32_t *s, int order)
{
	switch (order) {
	case 0:	 return 0;
	case 1:	 return s[-1];
	case 2:	 return 2 * s[-1] - s[-2];
	case 3:	 return 3 * s[-1] - 3 * s[-2] + s[-3];
	default: return 4 * s[-1] - 6 * s[-2] + 4 * s[-3] - s[-4];
	}
}

/*
 * The residual: the block is cut into partitions and each gets its own
 * Rice parameter, because the error is not equally large everywhere. A
 * parameter of all ones means the encoder gave up and wrote the values
 * out plainly.
 */
static bool read_residual(struct flac *f, int32_t *out, int block, int order)
{
	int method = get(&f->b, 2);
	int partitions, samples;
	int bits = method == 0 ? 4 : 5;
	unsigned escape = method == 0 ? 0xf : 0x1f;

	if (method > 1)
		return false;
	partitions = 1 << get(&f->b, 4);
	samples = block / partitions;
	if (!samples || samples * partitions != block || samples < order)
		return false;

	for (int p = 0; p < partitions; p++) {
		int n = p ? samples : samples - order;
		unsigned param = get(&f->b, bits);

		if (param == escape) {
			int raw = get(&f->b, 5);

			for (int i = 0; i < n; i++)
				*out++ = raw ? get_signed(&f->b, raw) : 0;
			continue;
		}
		for (int i = 0; i < n; i++) {
			int high = get_unary(&f->b);
			uint32_t v;

			if (high < 0)
				return false;
			v = ((uint32_t)high << param) | get(&f->b, param);
			*out++ = (int32_t)(v >> 1) ^ -(int32_t)(v & 1);	/* zig-zag */
		}
	}
	return !f->b.eof;
}

static bool read_subframe(struct flac *f, int32_t *s, int block, int bps)
{
	int type, wasted = 0, order;

	if (get(&f->b, 1))
		return false;			/* the padding bit must be zero */
	type = get(&f->b, 6);
	if (get(&f->b, 1)) {
		int zeros = get_unary(&f->b);

		if (zeros < 0)
			return false;
		wasted = zeros + 1;		/* the encoder dropped low bits */
		bps -= wasted;
	}
	if (bps <= 0 || bps > 32)
		return false;

	if (type == 0) {			/* the whole block is one value */
		int32_t v = get_signed(&f->b, bps);

		for (int i = 0; i < block; i++)
			s[i] = v;
	} else if (type == 1) {			/* stored as it is */
		for (int i = 0; i < block; i++)
			s[i] = get_signed(&f->b, bps);
	} else if (type >= 8 && type <= 12) {	/* fixed polynomial */
		order = type - 8;
		for (int i = 0; i < order; i++)
			s[i] = get_signed(&f->b, bps);
		if (!read_residual(f, s + order, block, order))
			return false;
		for (int i = order; i < block; i++)
			s[i] += predict_fixed(s + i, order);
	} else if (type >= 32) {		/* the encoder's own predictor */
		int32_t coeff[MAX_ORDER];
		int precision, shift;

		order = type - 31;
		for (int i = 0; i < order; i++)
			s[i] = get_signed(&f->b, bps);
		precision = get(&f->b, 4) + 1;
		if (precision == 16)
			return false;
		shift = get_signed(&f->b, 5);
		if (shift < 0)
			return false;
		for (int i = 0; i < order; i++)
			coeff[i] = get_signed(&f->b, precision);
		if (!read_residual(f, s + order, block, order))
			return false;
		for (int i = order; i < block; i++) {
			int64_t sum = 0;

			for (int k = 0; k < order; k++)
				sum += (int64_t)coeff[k] * s[i - 1 - k];
			s[i] += (int32_t)(sum >> shift);
		}
	} else {
		return false;			/* reserved */
	}

	if (wasted)
		for (int i = 0; i < block; i++)
			s[i] = (int32_t)((uint32_t)s[i] << wasted);
	return !f->b.eof;
}

/* The frame number, coded the way UTF-8 codes a character. */
static bool skip_frame_number(struct bits *b)
{
	uint32_t first = get(b, 8);
	int extra = 0;

	while (extra < 6 && (first & (0x80 >> extra)))
		extra++;
	if (extra == 1)
		return false;			/* a continuation byte cannot start one */
	for (int i = 1; i < extra; i++)
		if ((get(b, 8) & 0xc0) != 0x80)
			return false;
	return true;
}

static bool decode_frame(struct flac *f)
{
	struct bits *b = &f->b;
	int block_code, rate_sel, channel_mode, bps_sel, channels, block, bps, rate;
	uint8_t want8;
	uint16_t want16;

	align(b);
	b->crc8 = 0;
	b->crc16 = 0;
	if (get(b, 14) != 0x3ffe || b->eof)
		return false;			/* frame sync */
	get(b, 1);				/* reserved */
	get(b, 1);				/* fixed or variable block size */
	block_code = get(b, 4);
	rate_sel = get(b, 4);
	channel_mode = get(b, 4);
	bps_sel = get(b, 3);
	if (get(b, 1))
		return false;			/* reserved bit */
	if (!skip_frame_number(b))
		return false;

	if (block_code == 1)
		block = 192;
	else if (block_code >= 2 && block_code <= 5)
		block = 576 << (block_code - 2);
	else if (block_code >= 8)
		block = 256 << (block_code - 8);
	else if (block_code == 6)
		block = get(b, 8) + 1;
	else if (block_code == 7)
		block = get(b, 16) + 1;
	else
		return false;

	if (rate_sel == 12)
		rate = get(b, 8) * 1000;
	else if (rate_sel == 13)
		rate = get(b, 16);
	else if (rate_sel == 14)
		rate = get(b, 16) * 10;
	else if (rate_sel == 15)
		return false;
	else
		rate = rate_code[rate_sel];	/* 0 means "as the header said" */
	if (rate && rate != f->base.rate)
		return false;			/* a rate change mid-stream */

	bps = bps_sel ? bps_code[bps_sel] : f->bps;
	channels = channel_mode < 8 ? channel_mode + 1 : 2;
	if (!bps || block > f->max_block || channels != f->channels)
		return false;

	want8 = b->crc8;
	if (get(b, 8) != want8)
		return false;			/* header check sum */

	/*
	 * One of the two channels of a stereo pair carries the difference,
	 * which needs one bit more than the samples themselves.
	 */
	for (int c = 0; c < channels; c++) {
		int extra = (channel_mode == 8 && c == 1) ||
			    (channel_mode == 9 && c == 0) ||
			    (channel_mode == 10 && c == 1);

		if (!read_subframe(f, f->ch[c], block, bps + extra))
			return false;
	}
	align(b);
	want16 = b->crc16;
	if (get(b, 16) != want16)
		return false;			/* frame check sum */

	switch (channel_mode) {
	case 8:					/* left and the difference */
		for (int i = 0; i < block; i++)
			f->ch[1][i] = f->ch[0][i] - f->ch[1][i];
		break;
	case 9:					/* the difference and right */
		for (int i = 0; i < block; i++)
			f->ch[0][i] += f->ch[1][i];
		break;
	case 10:				/* the average and the difference */
		for (int i = 0; i < block; i++) {
			int32_t side = f->ch[1][i];
			int32_t mid = ((uint32_t)f->ch[0][i] << 1) | (side & 1);

			f->ch[0][i] = (mid + side) >> 1;
			f->ch[1][i] = (mid - side) >> 1;
		}
		break;
	}

	/* Everything above works in the file's own bit depth; we hand out 16. */
	if (bps != 16) {
		int shift = bps - 16;

		for (int c = 0; c < channels; c++)
			for (int i = 0; i < block; i++)
				f->ch[c][i] = shift > 0 ? f->ch[c][i] >> shift
							: f->ch[c][i] << -shift;
	}
	f->block = block;
	f->taken = 0;
	return true;
}

static ssize_t flac_read(struct codec *c, int16_t *pcm, size_t frames)
{
	struct flac *f = (struct flac *)c;
	size_t done = 0;

	while (done < frames) {
		int n;

		if (f->taken == f->block) {
			if (f->b.eof || f->broken)
				break;
			if (!decode_frame(f)) {
				/* A short read at the end is the end; anything
				 * else is a damaged file and worth saying so. */
				f->broken = !f->b.eof;
				if (f->broken && !done)
					return -EIO;
				break;
			}
		}
		n = f->block - f->taken;
		if ((size_t)n > frames - done)
			n = frames - done;
		if (c->channels == 2) {
			const int32_t *l = f->ch[0] + f->taken, *r = f->ch[1] + f->taken;

			for (int i = 0; i < n; i++) {
				pcm[2 * (done + i)] = l[i];
				pcm[2 * (done + i) + 1] = r[i];
			}
		} else {
			const int32_t *s = f->ch[0] + f->taken;

			for (int i = 0; i < n; i++)
				pcm[done + i] = s[i];
		}
		f->taken += n;
		done += n;
	}
	return done;
}

static void flac_close(struct codec *c)
{
	struct flac *f = (struct flac *)c;

	for (int i = 0; i < MAX_CHANNELS; i++)
		pt_free(f->ch[i]);
	pt_free(f);
}

static const struct codec_ops flac_ops = {
	.name = "FLAC",
	.read = flac_read,
	.close = flac_close,
};

/*
 * The header: STREAMINFO says how big blocks get and how wide the samples
 * are, which is everything we need to allocate. The other metadata blocks
 * (tags, seek points, cover art) are skipped.
 */
int flac_open(int fd, const uint8_t *head, size_t n, struct codec **out)
{
	struct flac *f;
	uint8_t info[34];
	bool last = false;
	int channels;

	if (n < 4 || memcmp(head, "fLaC", 4))
		return -ENOTSUP;
	crc_tables();
	f = pt_malloc(sizeof(*f));
	if (!f)
		return -ENOMEM;
	*f = (struct flac){ .base = { .ops = &flac_ops, .fd = fd }, .b = { .fd = fd } };
	if (!seek_to(&f->b, 4)) {
		pt_free(f);
		return -EIO;
	}

	while (!last) {
		uint32_t type, len;

		last = get(&f->b, 1);
		type = get(&f->b, 7);
		len = get(&f->b, 24);
		if (f->b.eof)
			goto bad;
		if (type == 0) {		/* STREAMINFO */
			if (len < sizeof(info))
				goto bad;
			for (size_t i = 0; i < sizeof(info); i++)
				info[i] = get(&f->b, 8);
			len -= sizeof(info);
		}
		for (uint32_t i = 0; i < len; i++)	/* skip the rest */
			get(&f->b, 8);
	}

	f->max_block = info[2] << 8 | info[3];
	f->base.rate = info[10] << 12 | info[11] << 4 | info[12] >> 4;
	channels = ((info[12] >> 1) & 7) + 1;
	f->bps = (((info[12] & 1) << 4) | (info[13] >> 4)) + 1;
	f->base.frames = ((uint64_t)(info[13] & 0xf) << 32) | (uint32_t)(info[14] << 24) |
			 info[15] << 16 | info[16] << 8 | info[17];
	if (!f->base.rate || !f->max_block || f->max_block > 65535 ||
	    channels < 1 || channels > MAX_CHANNELS || f->bps < 4 || f->bps > 32)
		goto bad;
	f->channels = channels;
	f->base.channels = channels > 2 ? 2 : channels;	/* a mono speaker, at most a pair */

	for (int i = 0; i < channels; i++) {
		f->ch[i] = pt_malloc((size_t)f->max_block * sizeof(*f->ch[i]));
		if (!f->ch[i])
			goto bad;
	}
	*out = &f->base;
	return 0;
bad:
	flac_close(&f->base);
	return -EINVAL;
}
