/* The real WAV decoder, with POSIX files on the host: every sample format, read a few frames at a time. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../codec/wav.c"

void *pt_malloc(size_t size) { return malloc(size); }
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

/* argv: file, channels, bits, then the samples expected, as 24 bits; "bad" for a refused file. */
int main(int argc, char **argv)
{
	struct codec *c = NULL;
	uint8_t head[16];
	int32_t pcm[7 * 2];
	int fd = open(argv[1], O_RDONLY), status, want = argc - 4, seen = 0;
	ssize_t got;

	assert(fd >= 0 && argc >= 4);
	got = read(fd, head, sizeof(head));
	status = wav_open(fd, head, got, &c);
	if (!strcmp(argv[3], "bad")) {
		assert(status == -EINVAL && !c);
		return 0;
	}
	assert(!status && c->channels == atoi(argv[2]) && c->bits == atoi(argv[3]));
	while ((got = c->ops->read(c, pcm, 7)) > 0)
		for (ssize_t i = 0; i < got * c->channels; i++, seen++) {
			assert(seen < want);
			if (pcm[i] != atoi(argv[4 + seen])) {
				fprintf(stderr, "sample %d: %d, not %s\n", seen, pcm[i], argv[4 + seen]);
				return 1;
			}
		}
	assert(!got && seen == want);
	c->ops->close(c);
	close(fd);
	return 0;
}
