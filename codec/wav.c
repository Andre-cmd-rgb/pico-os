/*
 * WAV: a header and then the samples, nothing else. The header is a chain
 * of chunks; the two that matter say what the samples are ("fmt ") and
 * where they start ("data").
 */
#include <string.h>

#include "codec.h"
#include <unistd.h>

#include "pt/sys.h"

struct wav {
	struct codec	base;
	int		bytes_per_frame;
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

static ssize_t wav_read(struct codec *c, int16_t *pcm, size_t frames)
{
	struct wav *w = (struct wav *)c;
	size_t want = frames * w->bytes_per_frame;
	ssize_t n;

	if (w->left != UINT32_MAX && want > w->left)
		want = w->left;
	if (!want)
		return 0;
	n = pt_read(c->fd, pcm, want);
	if (n < 0)
		return n;
	if (w->left != UINT32_MAX)
		w->left -= n;
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
	uint8_t chunk[8], fmt[16];

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
			if (len < sizeof(fmt) ||
			    pt_read(fd, fmt, sizeof(fmt)) != (ssize_t)sizeof(fmt))
				goto bad;
			if (le16(fmt) != 1 || le16(fmt + 14) != 16)
				goto bad;	/* only plain 16-bit PCM */
			w->base.channels = le16(fmt + 2);
			w->base.rate = le32(fmt + 4);
			if (w->base.channels < 1 || w->base.channels > 2)
				goto bad;
			w->bytes_per_frame = w->base.channels * 2;
			pt_lseek(fd, len - sizeof(fmt), SEEK_CUR);
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
