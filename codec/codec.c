/*
 * Which decoder a file needs, and the plain one for files that are
 * nothing but samples.
 *
 * Every decoder is asked in turn whether it recognises the first bytes,
 * the way the kernel asks each executable loader whether it claims a
 * file. A decoder that does not answers -ENOTSUP.
 */
#include "codec.h"
#include <unistd.h>

#include "pt/sys.h"

#define SNIFF_BYTES	16

struct raw {
	struct codec base;
};

static ssize_t raw_read(struct codec *c, int16_t *pcm, size_t frames)
{
	ssize_t n = pt_read(c->fd, pcm, frames * sizeof(*pcm));

	return n < 0 ? n : n / (ssize_t)sizeof(*pcm);
}

static void raw_close(struct codec *c)
{
	pt_free(c);
}

static const struct codec_ops raw_ops = {
	.name = "raw",
	.read = raw_read,
	.close = raw_close,
};

int codec_open_raw(int fd, int rate, struct codec **out)
{
	struct raw *r = pt_malloc(sizeof(*r));

	if (!r)
		return -ENOMEM;
	*r = (struct raw){ { .ops = &raw_ops, .fd = fd, .rate = rate, .channels = 1 } };
	*out = &r->base;
	return 0;
}

int codec_open(int fd, struct codec **out)
{
	static int (*const openers[])(int, const uint8_t *, size_t, struct codec **) = {
		wav_open, flac_open, mp3_open,
	};
	uint8_t head[SNIFF_BYTES] = { 0 };
	ssize_t n;

	if (pt_lseek(fd, 0, SEEK_SET) < 0)
		return -ESPIPE;
	n = pt_read(fd, head, sizeof(head));
	if (n < 0)
		return n;
	for (size_t i = 0; i < sizeof(openers) / sizeof(openers[0]); i++) {
		int ret = openers[i](fd, head, n, out);

		if (ret != -ENOTSUP)
			return ret;
		if (pt_lseek(fd, 0, SEEK_SET) < 0)
			return -ESPIPE;
	}
	return -ENOTSUP;
}

ssize_t codec_read(struct codec *c, int16_t *pcm, size_t frames)
{
	return c->ops->read(c, pcm, frames);
}

void codec_close(struct codec *c)
{
	c->ops->close(c);
}

const char *codec_name(const struct codec *c)
{
	return c->ops->name;
}
