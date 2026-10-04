/*
 * MP3 (MPEG-1/2 layer III).
 *
 * The decoding itself is minimp3 (third_party/), the one piece of this
 * firmware written by somebody else: layer III is a large, fiddly piece
 * of signal processing whose Huffman and window tables are the ISO
 * standard itself, and a half-correct one sounds worse than none. This
 * file is the part that belongs to us -- keeping the reader fed, finding
 * frames, and handing samples over like every other decoder here.
 *
 * The samples are taken as floating point, which is what minimp3 works
 * in, rather than rounded to 16 bits and clipped: 24 bits of them are
 * kept, and a loud track's overshoot of full scale goes to the mixer's
 * limiter instead of being cut flat.
 *
 * Note for callers: minimp3 keeps a 17 KB scratch buffer on the stack, so
 * a program that plays MP3 needs a stack bigger than the default.
 */
#include <math.h>
#include <string.h>

#include "codec.h"
#define MINIMP3_FLOAT_OUTPUT		/* as decoders.c builds it */
#include "minimp3.h"
#include <unistd.h>

#include "pt/sys.h"

#define IN_BYTES	16384		/* a frame is at most 1441 bytes */
#define REFILL_AT	4096

struct mp3 {
	struct codec	base;
	mp3dec_t	dec;
	uint8_t		in[IN_BYTES];
	int		filled, pos;
	bool		eof;
	int		error;
	float		pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];	/* full scale is 1 */
	int		have, taken;	/* frames decoded but not handed over */
};

static bool refill(struct mp3 *m)
{
	ssize_t n;

	if (m->eof || m->filled - m->pos >= REFILL_AT)
		return true;
	memmove(m->in, m->in + m->pos, m->filled - m->pos);
	m->filled -= m->pos;
	m->pos = 0;
	n = pt_read(m->base.fd, m->in + m->filled, IN_BYTES - m->filled);
	if (n < 0) {
		m->error = n;
		return false;
	}
	if (n == 0)
		m->eof = true;
	m->filled += n;
	return true;
}

/* Decodes the next frame with samples in it; false at the end of the file. */
static bool next_frame(struct mp3 *m)
{
	if (m->error)
		return false;
	for (;;) {
		mp3dec_frame_info_t info;
		int samples;

		if (!refill(m))
			return false;
		if (m->pos >= m->filled)
			return false;
		samples = mp3dec_decode_frame(&m->dec, m->in + m->pos, m->filled - m->pos,
					      m->pcm, &info);
		if (!info.frame_bytes)
			return false;		/* nothing decodable left */
		m->pos += info.frame_bytes;
		if (!samples)
			continue;		/* a tag, or a frame cut short */
		/* Callers allocate their output once, using the opening format. */
		if (m->base.channels && (m->base.channels != info.channels ||
					m->base.rate != info.hz)) {
			m->error = -EINVAL;
			return false;
		}
		m->base.rate = info.hz;
		m->base.channels = info.channels;
		m->have = samples;
		m->taken = 0;
		return true;
	}
}

static ssize_t mp3_read(struct codec *c, int32_t *pcm, size_t frames)
{
	struct mp3 *m = (struct mp3 *)c;
	size_t done = 0;

	while (done < frames) {
		int n;

		if (m->taken == m->have) {
			if (!next_frame(m))
				break;
		}
		n = m->have - m->taken;
		if ((size_t)n > frames - done)
			n = frames - done;
		for (int i = 0; i < n * c->channels; i++) {
			float v = m->pcm[m->taken * c->channels + i] * (CODEC_FULL + 1);

			pcm[done * c->channels + i] = v >= CODEC_OVER ? CODEC_OVER :
						      v <= -CODEC_OVER ? -CODEC_OVER : (int32_t)lrintf(v);
		}
		m->taken += n;
		done += n;
	}
	return done ? (ssize_t)done : m->error;
}

static void mp3_close(struct codec *c)
{
	pt_free(c);
}

static const struct codec_ops mp3_ops = {
	.name = "MP3",
	.read = mp3_read,
	.close = mp3_close,
};

int mp3_open(int fd, const uint8_t *head, size_t n, struct codec **out)
{
	struct mp3 *m;

	if (n < 3)
		return -ENOTSUP;
	/* An ID3 tag, or the eleven set bits a frame starts with. */
	if (memcmp(head, "ID3", 3) && !(head[0] == 0xff && (head[1] & 0xe0) == 0xe0))
		return -ENOTSUP;
	m = pt_malloc(sizeof(*m));
	if (!m)
		return -ENOMEM;
	*m = (struct mp3){ .base = { .ops = &mp3_ops, .fd = fd } };
	mp3dec_init(&m->dec);
	if (pt_lseek(fd, 0, SEEK_SET) < 0 || !next_frame(m)) {
		pt_free(m);
		return -EINVAL;
	}
	*out = &m->base;
	return 0;
}
