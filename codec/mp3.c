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
	/* for seeking: where the frames are, and how the bytes are spread */
	off_t		start, bytes;
	int		bitrate;	/* the first frame's, bits a second */
	bool		have_toc;
	uint8_t		toc[100];	/* Xing: each percent's place in 256ths */
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
		const float *from;
		int32_t *to;
		int n, samples;

		if (m->taken == m->have) {
			if (!next_frame(m))
				break;
		}
		n = m->have - m->taken;
		if ((size_t)n > frames - done)
			n = frames - done;
		/* in locals: a store through pcm could otherwise be m's fields */
		samples = n * c->channels;
		from = m->pcm + m->taken * c->channels;
		to = pcm + done * c->channels;
		for (int i = 0; i < samples; i++) {
			float v = from[i] * (CODEC_FULL + 1);

			to[i] = v >= CODEC_OVER ? CODEC_OVER :
				v <= -CODEC_OVER ? -CODEC_OVER : codec_round(v);
		}
		m->taken += n;
		done += n;
	}
	return done ? (ssize_t)done : m->error;
}

/* ------------------------------------------------------------ seeking */

static const short kbps[2][16] = {	/* layer III: MPEG-1, then 2 and 2.5 */
	{ 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
	{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
};

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/*
 * The first frame's header says the bitrate; a VBR file's first frame is
 * usually no music at all but a Xing (or "Info") table: how many frames
 * there are, how many bytes, and where each percent of the time begins.
 */
static void read_layout(struct mp3 *m, off_t start)
{
	uint8_t *buf = pt_malloc(4096);
	off_t size = pt_lseek(m->base.fd, 0, SEEK_END);
	ssize_t n;

	m->start = start;
	m->bytes = size > start ? size - start : 0;
	if (!buf || pt_lseek(m->base.fd, start, SEEK_SET) < 0 || (n = pt_read(m->base.fd, buf, 4096)) < 0)
		n = 0;
	for (ssize_t p = 0; p + 4 < n; p++) {
		int version, side, x;
		bool mono, mpeg1;

		if (buf[p] != 0xff || (buf[p + 1] & 0xe0) != 0xe0 || ((buf[p + 1] >> 1) & 3) != 1)
			continue;			/* not a layer III frame */
		version = (buf[p + 1] >> 3) & 3;
		if (version == 1 || (buf[p + 2] >> 4) == 15 || ((buf[p + 2] >> 2) & 3) == 3)
			continue;
		mpeg1 = version == 3;
		mono = (buf[p + 3] >> 6) == 3;
		m->start = start + p;
		m->bytes = size > m->start ? size - m->start : 0;
		m->bitrate = kbps[!mpeg1][buf[p + 2] >> 4] * 1000;
		side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
		x = p + 4 + side;
		if (x + 8 <= n && (!memcmp(buf + x, "Xing", 4) || !memcmp(buf + x, "Info", 4))) {
			uint32_t flags = be32(buf + x + 4);
			int at = x + 8;

			if ((flags & 1) && at + 4 <= n) {
				m->base.frames = (uint64_t)be32(buf + at) * (mpeg1 ? 1152 : 576);
				at += 4;
			}
			if ((flags & 2) && at + 4 <= n) {
				if (be32(buf + at) && be32(buf + at) <= m->bytes)
					m->bytes = be32(buf + at);
				at += 4;
			}
			if ((flags & 4) && at + 100 <= n) {
				memcpy(m->toc, buf + at, 100);
				m->have_toc = true;
			}
		}
		break;
	}
	pt_free(buf);
}

static int64_t mp3_seek(struct codec *c, uint64_t frame)
{
	struct mp3 *m = (struct mp3 *)c;
	off_t at;

	if (c->frames) {
		double part = frame >= c->frames ? 1.0 : (double)frame / (double)c->frames;

		if (m->have_toc) {
			double percent = part * 100;
			int i = percent >= 99 ? 99 : (int)percent;
			double a = m->toc[i], b = i < 99 ? m->toc[i + 1] : 256;

			part = (a + (b - a) * (percent - i)) / 256;
		}
		at = (off_t)(part * (double)m->bytes);
		if (frame > c->frames)
			frame = c->frames;
	} else if (m->bitrate && c->rate) {
		at = (off_t)((double)frame * m->bitrate / 8 / c->rate);
	} else {
		return -ENOTSUP;
	}
	if (at > m->bytes)
		at = m->bytes;
	if (pt_lseek(c->fd, m->start + at, SEEK_SET) < 0)
		return -EIO;
	/* minimp3 finds the next frame itself; the reservoir starts again */
	m->filled = m->pos = m->have = m->taken = 0;
	m->eof = false;
	m->error = 0;
	mp3dec_init(&m->dec);
	return frame;
}

static void mp3_close(struct codec *c)
{
	pt_free(c);
}

static const struct codec_ops mp3_ops = {
	.name = "MP3",
	.read = mp3_read,
	.seek = mp3_seek,
	.close = mp3_close,
};

int mp3_open(int fd, const uint8_t *head, size_t n, struct codec **out)
{
	struct mp3 *m;
	off_t start = 0;

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
	/* An ID3v2 tag says how long it is: straight past it, and its cover. */
	if (n >= 10 && !memcmp(head, "ID3", 3) && !((head[6] | head[7] | head[8] | head[9]) & 0x80))
		start = 10 + ((off_t)head[6] << 21 | head[7] << 14 | head[8] << 7 | head[9]) +
			(head[5] & 0x10 ? 10 : 0);	/* and a footer */
	read_layout(m, start);
	if (pt_lseek(fd, m->start, SEEK_SET) < 0 || !next_frame(m)) {
		/* the tag lied or the guess missed: from the top, as it was */
		*m = (struct mp3){ .base = { .ops = &mp3_ops, .fd = fd } };
		mp3dec_init(&m->dec);
		read_layout(m, 0);
		if (pt_lseek(fd, 0, SEEK_SET) < 0 || !next_frame(m)) {
			pt_free(m);
			return -EINVAL;
		}
	}
	*out = &m->base;
	return 0;
}
