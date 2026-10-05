/*
 * The real decoders behind codec_open() and codec_seek(), with POSIX files
 * on the host.
 *
 *	seek_test dump FILE OUT         every frame, as int32s
 *	seek_test FILE OUT FRAME...     for each FRAME in turn, on one open
 *	                                decoder: where codec_seek() says it
 *	                                landed (int64), how many frames were
 *	                                read after it (int32), and those
 *
 * Both print "rate channels frames" first.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../codec/codec.h"

#define AFTER	8192		/* frames read after each seek */

void *pt_malloc(size_t size) { return malloc(size); }
void *pt_calloc(size_t n, size_t size) { return calloc(n, size); }
void pt_free(void *ptr) { free(ptr); }
ssize_t pt_read(int fd, void *buf, size_t size)
{
	ssize_t got = read(fd, buf, size);

	return got < 0 ? -errno : got;
}
off_t pt_lseek(int fd, off_t offset, int whence)
{
	off_t pos = lseek(fd, offset, whence);

	return pos < 0 ? -errno : pos;
}

/* Up to `want` frames, as many reads as it takes. */
static ssize_t read_frames(struct codec *c, int32_t *pcm, size_t want)
{
	size_t done = 0;

	while (done < want) {
		ssize_t got = codec_read(c, pcm + done * c->channels, want - done);

		if (got < 0)
			return got;
		if (!got)
			break;
		done += got;
	}
	return done;
}

int main(int argc, char **argv)
{
	bool dump = argc == 4 && !strcmp(argv[1], "dump");
	const char *in = dump ? argv[2] : argv[1], *out = dump ? argv[3] : argv[2];
	struct codec *c = NULL;
	int fd = open(in, O_RDONLY);
	FILE *f = fopen(out, "wb");
	int32_t *pcm = malloc(AFTER * 2 * sizeof(*pcm));
	ssize_t got;

	assert(argc >= 3 && fd >= 0 && f && pcm);
	assert(!codec_open(fd, &c) && c);
	printf("%d %d %llu\n", c->rate, c->channels, (unsigned long long)c->frames);
	if (dump) {
		while ((got = read_frames(c, pcm, AFTER)) > 0)
			assert(fwrite(pcm, sizeof(*pcm) * c->channels, got, f) == (size_t)got);
		assert(!got);
	} else {
		for (int i = 3; i < argc; i++) {
			int64_t at = codec_seek(c, strtoull(argv[i], NULL, 10));
			int32_t n;

			assert(at >= 0);
			got = read_frames(c, pcm, AFTER);
			assert(got >= 0);
			n = got;
			assert(fwrite(&at, sizeof(at), 1, f) == 1 && fwrite(&n, sizeof(n), 1, f) == 1);
			assert(fwrite(pcm, sizeof(*pcm) * c->channels, n, f) == (size_t)n);
		}
	}
	codec_close(c);
	free(pcm);
	close(fd);
	return fclose(f);
}
