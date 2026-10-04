/*
 * WAV: a header and then the samples, nothing else. The header is a chain
 * of chunks; the two that matter say what the samples are ("fmt ") and
 * where they start ("data"). Whole numbers of 8, 16, 24 or 32 bits, or
 * 32-bit floating point, as studio exports come; the extensible header
 * says the same in a longer way.
 */
#include <math.h>
#include <string.h>

#include "codec.h"
#include <unistd.h>

#include "pt/sys.h"

#define FORMAT_PCM	1
#define FORMAT_FLOAT	3
#define FORMAT_EXTENSIBLE 0xfffe	/* the real one in its sub-format's first two bytes */

struct wav {
	struct codec	base;
	int		bytes_per_frame;
	int		width;		/* bytes a sample */
	bool		is_float;
	uint32_t	left;		/* bytes of samples still to come */
};

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

void wav_header(uint8_t *out, int rate, int channels, uint32_t bytes)
{
	const uint32_t fields[] = {
		36 + bytes, 16, 0, (uint32_t)rate, (uint32_t)rate * channels * 2, 0, bytes,
	};
	static const int at[] = { 4, 16, 20, 24, 28, 32, 40 };

	memset(out, 0, WAV_HEADER_BYTES);
	memcpy(out, "RIFF", 4);
	memcpy(out + 8, "WAVEfmt ", 8);
	memcpy(out + 36, "data", 4);
	for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++)
		for (int b = 0; b < 4; b++)
			out[at[i] + b] = fields[i] >> (8 * b);
	out[20] = 1;				/* plain PCM */
	out[22] = channels;
	out[32] = channels * 2;			/* bytes per frame */
	out[34] = 16;				/* bits per sample */
}

/* One sample as 24 bits, from its little-endian bytes. */
static int32_t widen(const struct wav *w, const uint8_t *p)
{
	switch (w->width) {
	case 1:
		return (p[0] - 128) * 65536;		/* 8-bit is unsigned */
	case 2:
		return (int16_t)le16(p) * 256;
	case 3:
		return (int32_t)((uint32_t)p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
	default:
		if (w->is_float) {
			uint32_t bits = le32(p);
			float f;

			memcpy(&f, &bits, sizeof(f));
			f *= CODEC_FULL + 1;
			if (f != f)
				return 0;		/* NaN: silence, not whatever it converts to */
			return f >= CODEC_OVER ? CODEC_OVER : f <= -CODEC_OVER ? -CODEC_OVER :
			       (int32_t)lrintf(f);
		}
		return (int32_t)le32(p) >> 8;
	}
}

/* Read into the far end of the buffer, then widened from the front. */
static ssize_t wav_read(struct codec *c, int32_t *pcm, size_t frames)
{
	struct wav *w = (struct wav *)c;
	size_t want = frames * w->bytes_per_frame, samples;
	uint8_t *raw;
	ssize_t n;

	if (w->left != UINT32_MAX && want > w->left)
		want = w->left;
	if (!want)
		return 0;
	/* at the very end, so a sample is read before its widened self lands on it */
	raw = (uint8_t *)pcm + frames * c->channels * sizeof(*pcm) - want;
	n = pt_read(c->fd, raw, want);
	if (n < 0)
		return n;
	if (w->left != UINT32_MAX)
		w->left -= n;
	samples = n / w->bytes_per_frame * c->channels;
	for (size_t i = 0; i < samples; i++)
		pcm[i] = widen(w, raw + i * w->width);
	return n / w->bytes_per_frame;
}

static void wav_close(struct codec *c)
{
	pt_free(c);
}

static const struct codec_ops wav_ops = {
	.name = "WAV",
	.read = wav_read,
	.close = wav_close,
};

int wav_open(int fd, const uint8_t *head, size_t n, struct codec **out)
{
	struct wav *w;
	uint8_t chunk[8], fmt[26];
	int format, bits;

	if (n < 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4))
		return -ENOTSUP;
	w = pt_malloc(sizeof(*w));
	if (!w)
		return -ENOMEM;
	*w = (struct wav){ .base = { .ops = &wav_ops, .fd = fd }, .left = UINT32_MAX };
	if (pt_lseek(fd, 12, SEEK_SET) < 0)
		goto bad;

	while (pt_read(fd, chunk, sizeof(chunk)) == (ssize_t)sizeof(chunk)) {
		uint32_t len = le32(chunk + 4);

		if (!memcmp(chunk, "fmt ", 4)) {
			uint32_t got = len < sizeof(fmt) ? len : sizeof(fmt);

			if (len < 16 || pt_read(fd, fmt, got) != (ssize_t)got)
				goto bad;
			format = le16(fmt);
			if (format == FORMAT_EXTENSIBLE) {
				if (got < 26)
					goto bad;
				format = le16(fmt + 24);
			}
			bits = le16(fmt + 14);
			w->base.channels = le16(fmt + 2);
			w->base.rate = le32(fmt + 4);
			w->base.bits = bits;
			w->width = (bits + 7) / 8;
			w->is_float = format == FORMAT_FLOAT;
			if (!(format == FORMAT_PCM && bits >= 8 && bits <= 32) &&
			    !(w->is_float && bits == 32))
				goto bad;	/* compressed, or 64-bit float */
			if (w->base.channels < 1 || w->base.channels > 2)
				goto bad;
			w->bytes_per_frame = w->base.channels * w->width;
			pt_lseek(fd, len - got + (len & 1), SEEK_CUR);
		} else if (!memcmp(chunk, "data", 4)) {
			if (!w->bytes_per_frame)
				goto bad;	/* samples before their description */
			w->left = len;
			w->base.frames = len / w->bytes_per_frame;
			*out = &w->base;
			return 0;
		} else {
			pt_lseek(fd, len + (len & 1), SEEK_CUR);
		}
	}
bad:
	pt_free(w);
	return -EINVAL;
}
