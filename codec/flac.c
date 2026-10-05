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

#define BUF_BYTES	16384		/* a few frames: one card read a tenth of a second */
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

/* CRC-8 polynomial 0x07 and CRC-16 polynomial 0x8005. Immutable tables
 * avoid publishing a partially initialized table to another player. */
static const uint8_t crc8_table[256] = {
	0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15, 0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d,
	0x70, 0x77, 0x7e, 0x79, 0x6c, 0x6b, 0x62, 0x65, 0x48, 0x4f, 0x46, 0x41, 0x54, 0x53, 0x5a, 0x5d,
	0xe0, 0xe7, 0xee, 0xe9, 0xfc, 0xfb, 0xf2, 0xf5, 0xd8, 0xdf, 0xd6, 0xd1, 0xc4, 0xc3, 0xca, 0xcd,
	0x90, 0x97, 0x9e, 0x99, 0x8c, 0x8b, 0x82, 0x85, 0xa8, 0xaf, 0xa6, 0xa1, 0xb4, 0xb3, 0xba, 0xbd,
	0xc7, 0xc0, 0xc9, 0xce, 0xdb, 0xdc, 0xd5, 0xd2, 0xff, 0xf8, 0xf1, 0xf6, 0xe3, 0xe4, 0xed, 0xea,
	0xb7, 0xb0, 0xb9, 0xbe, 0xab, 0xac, 0xa5, 0xa2, 0x8f, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9d, 0x9a,
	0x27, 0x20, 0x29, 0x2e, 0x3b, 0x3c, 0x35, 0x32, 0x1f, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0d, 0x0a,
	0x57, 0x50, 0x59, 0x5e, 0x4b, 0x4c, 0x45, 0x42, 0x6f, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7d, 0x7a,
	0x89, 0x8e, 0x87, 0x80, 0x95, 0x92, 0x9b, 0x9c, 0xb1, 0xb6, 0xbf, 0xb8, 0xad, 0xaa, 0xa3, 0xa4,
	0xf9, 0xfe, 0xf7, 0xf0, 0xe5, 0xe2, 0xeb, 0xec, 0xc1, 0xc6, 0xcf, 0xc8, 0xdd, 0xda, 0xd3, 0xd4,
	0x69, 0x6e, 0x67, 0x60, 0x75, 0x72, 0x7b, 0x7c, 0x51, 0x56, 0x5f, 0x58, 0x4d, 0x4a, 0x43, 0x44,
	0x19, 0x1e, 0x17, 0x10, 0x05, 0x02, 0x0b, 0x0c, 0x21, 0x26, 0x2f, 0x28, 0x3d, 0x3a, 0x33, 0x34,
	0x4e, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5c, 0x5b, 0x76, 0x71, 0x78, 0x7f, 0x6a, 0x6d, 0x64, 0x63,
	0x3e, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2c, 0x2b, 0x06, 0x01, 0x08, 0x0f, 0x1a, 0x1d, 0x14, 0x13,
	0xae, 0xa9, 0xa0, 0xa7, 0xb2, 0xb5, 0xbc, 0xbb, 0x96, 0x91, 0x98, 0x9f, 0x8a, 0x8d, 0x84, 0x83,
	0xde, 0xd9, 0xd0, 0xd7, 0xc2, 0xc5, 0xcc, 0xcb, 0xe6, 0xe1, 0xe8, 0xef, 0xfa, 0xfd, 0xf4, 0xf3,
};

static const uint16_t crc16_table[256] = {
	0x0000, 0x8005, 0x800f, 0x000a, 0x801b, 0x001e, 0x0014, 0x8011, 0x8033, 0x0036, 0x003c, 0x8039, 0x0028, 0x802d, 0x8027, 0x0022,
	0x8063, 0x0066, 0x006c, 0x8069, 0x0078, 0x807d, 0x8077, 0x0072, 0x0050, 0x8055, 0x805f, 0x005a, 0x804b, 0x004e, 0x0044, 0x8041,
	0x80c3, 0x00c6, 0x00cc, 0x80c9, 0x00d8, 0x80dd, 0x80d7, 0x00d2, 0x00f0, 0x80f5, 0x80ff, 0x00fa, 0x80eb, 0x00ee, 0x00e4, 0x80e1,
	0x00a0, 0x80a5, 0x80af, 0x00aa, 0x80bb, 0x00be, 0x00b4, 0x80b1, 0x8093, 0x0096, 0x009c, 0x8099, 0x0088, 0x808d, 0x8087, 0x0082,
	0x8183, 0x0186, 0x018c, 0x8189, 0x0198, 0x819d, 0x8197, 0x0192, 0x01b0, 0x81b5, 0x81bf, 0x01ba, 0x81ab, 0x01ae, 0x01a4, 0x81a1,
	0x01e0, 0x81e5, 0x81ef, 0x01ea, 0x81fb, 0x01fe, 0x01f4, 0x81f1, 0x81d3, 0x01d6, 0x01dc, 0x81d9, 0x01c8, 0x81cd, 0x81c7, 0x01c2,
	0x0140, 0x8145, 0x814f, 0x014a, 0x815b, 0x015e, 0x0154, 0x8151, 0x8173, 0x0176, 0x017c, 0x8179, 0x0168, 0x816d, 0x8167, 0x0162,
	0x8123, 0x0126, 0x012c, 0x8129, 0x0138, 0x813d, 0x8137, 0x0132, 0x0110, 0x8115, 0x811f, 0x011a, 0x810b, 0x010e, 0x0104, 0x8101,
	0x8303, 0x0306, 0x030c, 0x8309, 0x0318, 0x831d, 0x8317, 0x0312, 0x0330, 0x8335, 0x833f, 0x033a, 0x832b, 0x032e, 0x0324, 0x8321,
	0x0360, 0x8365, 0x836f, 0x036a, 0x837b, 0x037e, 0x0374, 0x8371, 0x8353, 0x0356, 0x035c, 0x8359, 0x0348, 0x834d, 0x8347, 0x0342,
	0x03c0, 0x83c5, 0x83cf, 0x03ca, 0x83db, 0x03de, 0x03d4, 0x83d1, 0x83f3, 0x03f6, 0x03fc, 0x83f9, 0x03e8, 0x83ed, 0x83e7, 0x03e2,
	0x83a3, 0x03a6, 0x03ac, 0x83a9, 0x03b8, 0x83bd, 0x83b7, 0x03b2, 0x0390, 0x8395, 0x839f, 0x039a, 0x838b, 0x038e, 0x0384, 0x8381,
	0x0280, 0x8285, 0x828f, 0x028a, 0x829b, 0x029e, 0x0294, 0x8291, 0x82b3, 0x02b6, 0x02bc, 0x82b9, 0x02a8, 0x82ad, 0x82a7, 0x02a2,
	0x82e3, 0x02e6, 0x02ec, 0x82e9, 0x02f8, 0x82fd, 0x82f7, 0x02f2, 0x02d0, 0x82d5, 0x82df, 0x02da, 0x82cb, 0x02ce, 0x02c4, 0x82c1,
	0x8243, 0x0246, 0x024c, 0x8249, 0x0258, 0x825d, 0x8257, 0x0252, 0x0270, 0x8275, 0x827f, 0x027a, 0x826b, 0x026e, 0x0264, 0x8261,
	0x0220, 0x8225, 0x822f, 0x022a, 0x823b, 0x023e, 0x0234, 0x8231, 0x8213, 0x0216, 0x021c, 0x8219, 0x0208, 0x820d, 0x8207, 0x0202,
};

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
	uint64_t	 first;		/* the stream's sample that block starts at */
	off_t		 audio, end;	/* where the frames start, and the file ends */
	bool		 broken;
	int32_t		*ch[MAX_CHANNELS];
};

static const int rate_code[] = {
	0, 88200, 176400, 192000, 8000, 16000, 22050, 24000,
	32000, 44100, 48000, 96000, 0, 0, 0, 0,
};
static const int bps_code[] = { 0, 8, 12, 0, 16, 20, 24, 32 };

/*
 * The fixed predictors -- the sample, and its first four differences --
 * run forward over the residual in s. In 32 bits, wrapping: the sum is
 * right modulo 2^32, so a sample that fits in 32 bits comes out right
 * however large the terms on the way. A damaged frame comes out as
 * nonsense instead of being stopped here, and its CRC-16 throws it away.
 */
static void restore_fixed(int32_t *s, int block, int order)
{
	uint32_t *u = (uint32_t *)s;

	switch (order) {
	case 1:
		for (int i = 1; i < block; i++)
			u[i] += u[i - 1];
		break;
	case 2:
		for (int i = 2; i < block; i++)
			u[i] += 2 * u[i - 1] - u[i - 2];
		break;
	case 3:
		for (int i = 3; i < block; i++)
			u[i] += 3 * (u[i - 1] - u[i - 2]) + u[i - 3];
		break;
	case 4:
		for (int i = 4; i < block; i++)
			u[i] += 4 * (u[i - 1] + u[i - 3]) - 6 * u[i - 2] - u[i - 4];
		break;
	}
}

/*
 * The encoder's predictor: a weighted sum of the samples before, shifted
 * down. The shift means 32 bits only do when the sum cannot leave them,
 * which the weights say: a CD's samples nearly always, 24-bit ones never,
 * and those take the 64-bit sum, a few times slower here.
 */
static bool restore_lpc(int32_t *s, int block, int order, const int32_t *coeff, int shift,
			int bps)
{
	uint64_t weight = 0;

	for (int k = 0; k < order; k++)
		weight += coeff[k] < 0 ? -(int64_t)coeff[k] : coeff[k];
	if ((weight << (bps - 1)) <= INT32_MAX) {
		uint32_t *u = (uint32_t *)s;

		for (int i = order; i < block; i++) {
			uint32_t sum = 0;

			for (int k = 0; k < order; k++)
				sum += (uint32_t)coeff[k] * u[i - 1 - k];
			u[i] += (uint32_t)((int32_t)sum >> shift);
		}
		return true;
	}
	for (int i = order; i < block; i++) {
		int64_t sum = 0;

		for (int k = 0; k < order; k++)
			sum += (int64_t)coeff[k] * s[i - 1 - k];
		sum = s[i] + (sum >> shift);
		if (sum < INT32_MIN || sum > INT32_MAX)
			return false;
		s[i] = sum;
	}
	return true;
}

/*
 * A partition's Rice codes: the decoder's hot loop, nearly all of its time.
 * The reader's state is kept in locals, so that it stays in registers, and
 * bytes are taken straight from the buffer: get() and get_unary() cost a
 * call each, and a call this deep in the stack spills the processor's
 * register window, which came to 370 cycles a sample. Only the CRC-16 is
 * kept up here; the CRC-8 covers the frame's header, which is behind us.
 */
static bool read_rice(struct bits *b, int32_t *out, int n, unsigned param)
{
	const uint8_t *buf = b->buf;
	uint64_t acc = b->acc;
	uint16_t crc = b->crc16;
	int have = b->have, pos = b->pos, len = b->len;
	bool ok = true;

#define TAKE_BYTE()							\
	do {								\
		int byte_;						\
									\
		if (pos < len) {					\
			byte_ = buf[pos++];				\
			crc = (crc << 8) ^ crc16_table[(crc >> 8) ^ byte_]; \
		} else {	/* the buffer refilled, the slow way */	\
			b->pos = pos;					\
			b->crc16 = crc;					\
			byte_ = next_byte(b);				\
			pos = b->pos;					\
			len = b->len;					\
			crc = b->crc16;					\
			if (byte_ < 0) {				\
				ok = false;				\
				goto out;				\
			}						\
		}							\
		acc = acc << 8 | (unsigned)byte_;			\
		have += 8;						\
	} while (0)

	for (int i = 0; i < n; i++) {
		uint32_t q = 0, v;

		for (;;) {			/* zeros, then a one */
			if (have) {
				uint32_t peek = have >= 32 ? (uint32_t)(acc >> (have - 32))
							   : (uint32_t)acc << (32 - have);

				if (peek) {
					int lead = __builtin_clz(peek);

					q += lead;
					have -= lead + 1;
					break;
				}
				q += have >= 32 ? 32 : have;
				have -= have >= 32 ? 32 : have;
			}
			TAKE_BYTE();
		}
		while (have < (int)param)
			TAKE_BYTE();
		v = q << param;
		if (param) {
			have -= param;
			v |= (uint32_t)(acc >> have) & ((1u << param) - 1);
		}
		out[i] = (int32_t)(v >> 1) ^ -(int32_t)(v & 1);		/* zig-zag */
	}
#undef TAKE_BYTE
out:
	b->acc = acc;
	b->have = have;
	b->pos = pos;
	b->crc16 = crc;
	return ok;
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
		if (!read_rice(&f->b, out, n, param))
			return false;
		out += n;
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
		if (order > block)
			return false;
		for (int i = 0; i < order; i++)
			s[i] = get_signed(&f->b, bps);
		if (!read_residual(f, s + order, block, order))
			return false;
		restore_fixed(s, block, order);
	} else if (type >= 32) {		/* the encoder's own predictor */
		int32_t coeff[MAX_ORDER];
		int precision, shift;

		order = type - 31;
		if (order > block)
			return false;
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
		if (!read_residual(f, s + order, block, order) ||
		    !restore_lpc(s, block, order, coeff, shift, bps))
			return false;
	} else {
		return false;			/* reserved */
	}

	if (wasted)
		for (int i = 0; i < block; i++)
			s[i] = (int32_t)((uint32_t)s[i] << wasted);
	return !f->b.eof;
}

/*
 * The frame's number in a stream of fixed-size blocks, or its first
 * sample's in one of varying blocks: coded the way UTF-8 codes a
 * character, in up to seven bytes (a sample number has 36 bits).
 */
static bool frame_number(struct bits *b, uint64_t *out)
{
	uint32_t first = get(b, 8);
	int bytes = 0;
	uint64_t v;

	while (bytes < 8 && (first & (0x80 >> bytes)))
		bytes++;
	if (bytes == 1 || bytes == 8)
		return false;			/* a continuation byte cannot start one */
	v = bytes ? first & (0xff >> (bytes + 1)) : first;
	for (int i = 1; i < bytes; i++) {
		uint32_t next = get(b, 8);

		if ((next & 0xc0) != 0x80)
			return false;
		v = v << 6 | (next & 0x3f);
	}
	*out = v;
	return true;
}

static bool decode_frame(struct flac *f)
{
	struct bits *b = &f->b;
	int block_code, rate_sel, channel_mode, bps_sel, channels, block, bps, rate;
	bool varying;
	uint64_t number;
	uint8_t want8;
	uint16_t want16;

	align(b);
	b->crc8 = 0;
	b->crc16 = 0;
	if (get(b, 14) != 0x3ffe || b->eof)
		return false;			/* frame sync */
	get(b, 1);				/* reserved */
	varying = get(b, 1);			/* fixed or variable block size */
	block_code = get(b, 4);
	rate_sel = get(b, 4);
	channel_mode = get(b, 4);
	bps_sel = get(b, 3);
	if (get(b, 1))
		return false;			/* reserved bit */
	if (!frame_number(b, &number))
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
			int64_t mid = 2LL * f->ch[0][i] + (side & 1);

			f->ch[0][i] = (mid + side) >> 1;
			f->ch[1][i] = (mid - side) >> 1;
		}
		break;
	}

	/* Everything above works in the file's own bit depth; we hand out 24. */
	if (bps != 24) {
		int shift = bps - 24;

		for (int c = 0; c < channels; c++)
			for (int i = 0; i < block; i++)
				f->ch[c][i] = shift > 0 ? f->ch[c][i] >> shift
							: f->ch[c][i] * (1 << -shift);
	}
	f->block = block;
	f->taken = 0;
	/* every block but the last is the stream's one size when it is fixed */
	f->first = varying ? number : number * (uint64_t)f->max_block;
	return true;
}

static ssize_t flac_read(struct codec *c, int32_t *pcm, size_t frames)
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

/* ------------------------------------------------------------ seeking */

#define SCAN_BYTES	(256 * 1024)	/* farther than any frame is long */
#define NEAR_SECONDS	2		/* closer than this, decoding on is quicker */

/*
 * The first frame that starts at or after byte `from`, decoded: its
 * samples are ready to hand over and f->first says where it lies. A
 * frame is only believed once both its check sums agree, so the sync
 * code turning up by chance inside a frame's own bytes is passed over.
 */
static bool frame_from(struct flac *f, off_t from)
{
	struct bits *b = &f->b;
	int prev = -1;

	if (!seek_to(b, from))
		return false;
	for (off_t at = from; at < from + SCAN_BYTES; at++) {
		int byte = next_byte(b);

		if (byte < 0)
			return false;
		if (prev == 0xff && (byte & 0xfe) == 0xf8) {
			if (seek_to(b, at - 1) && decode_frame(f))
				return true;
			if (!seek_to(b, at + 1))	/* on from the byte after it */
				return false;
			byte = -1;
		}
		prev = byte;
	}
	return false;
}

/*
 * Interpolation between what is known before and after the sample: a
 * guess at its byte, the frame found from there, and the range shrinks
 * to whichever side the frame fell. Close enough, it decodes forward to
 * the block holding the sample and hands out from that sample on.
 */
static int64_t flac_seek(struct codec *c, uint64_t target)
{
	struct flac *f = (struct flac *)c;
	off_t lo = f->audio, hi = f->end;
	uint64_t lo_s = 0, hi_s = c->frames;
	uint64_t near = (uint64_t)c->rate * NEAR_SECONDS;
	bool found;

	f->broken = false;
	if (c->frames && target >= c->frames) {
		f->b.eof = true;		/* the end: nothing more to read */
		f->taken = f->block;
		return c->frames;
	}
	for (int tries = 0; tries < 32 && lo < hi; tries++) {
		off_t guess = lo + (hi - lo) / 2;

		if (hi_s > lo_s) {
			double bytes = (double)(hi - lo) / (double)(hi_s - lo_s);

			/* a block early, so the frame found is the one holding it */
			guess = lo + (off_t)(bytes * ((double)(target - lo_s) - f->max_block));
			if (guess < lo)
				guess = lo;
			if (guess >= hi)
				guess = hi - 1;
		}
		found = frame_from(f, guess);
		if (!found || f->first > target) {
			hi = guess;			/* it starts before here */
			if (found && f->first < hi_s)
				hi_s = f->first;
			continue;
		}
		if (target - f->first >= near) {
			lo = guess + 1;
			lo_s = f->first;
			continue;
		}
		while (target >= f->first + (uint64_t)f->block)
			if (!decode_frame(f))
				return -EIO;
		f->taken = target - f->first;
		return target;
	}
	return -EIO;
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
	.seek = flac_seek,
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
	bool last = false, have_info = false;
	int channels;

	if (n < 4 || memcmp(head, "fLaC", 4))
		return -ENOTSUP;
	f = pt_malloc(sizeof(*f));
	if (!f)
		return -ENOMEM;
	*f = (struct flac){ .base = { .ops = &flac_ops, .fd = fd }, .b = { .fd = fd } };
	if (!seek_to(&f->b, 4)) {
		pt_free(f);
		return -EIO;
	}

	f->audio = 4;
	while (!last) {
		uint32_t type, len;

		last = get(&f->b, 1);
		type = get(&f->b, 7);
		len = get(&f->b, 24);
		if (f->b.eof)
			goto bad;
		f->audio += 4 + len;
		if (type == 0) {		/* STREAMINFO */
			if (have_info || len != sizeof(info))
				goto bad;
			for (size_t i = 0; i < sizeof(info); i++)
				info[i] = get(&f->b, 8);
			have_info = true;
		} else if (!have_info) {
			goto bad;		/* STREAMINFO must be the first block */
		}
		/* past the rest: a cover can be half a megabyte */
		if (!seek_to(&f->b, f->audio))
			goto bad;
	}
	f->end = pt_lseek(fd, 0, SEEK_END);
	if (f->end < f->audio || !seek_to(&f->b, f->audio))
		goto bad;

	f->max_block = info[2] << 8 | info[3];
	f->base.rate = info[10] << 12 | info[11] << 4 | info[12] >> 4;
	channels = ((info[12] >> 1) & 7) + 1;
	f->bps = (((info[12] & 1) << 4) | (info[13] >> 4)) + 1;
	f->base.frames = ((uint64_t)(info[13] & 0xf) << 32) | ((uint32_t)info[14] << 24) |
			 info[15] << 16 | info[16] << 8 | info[17];
	if (!f->base.rate || !f->max_block || f->max_block > 65535 ||
	    channels < 1 || channels > MAX_CHANNELS || f->bps < 4 || f->bps > 32)
		goto bad;
	f->channels = channels;
	f->base.channels = channels > 2 ? 2 : channels;	/* a mono speaker, at most a pair */
	f->base.bits = f->bps;

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
