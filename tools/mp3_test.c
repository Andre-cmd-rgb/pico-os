/* The real MP3 wrapper and minimp3, with POSIX files on the host. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../codec/mp3.c"

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

int main(int argc, char **argv)
{
	struct codec *c = NULL;
	uint8_t head[16];
	int16_t *pcm;
	int fd, channels, rate, chunk;
	ssize_t got;
	bool partial = false;
	size_t decoded = 0;

	assert(argc == 6);
	channels = atoi(argv[2]);
	rate = atoi(argv[3]);
	chunk = atoi(argv[4]);
	assert(chunk > 0 && chunk < 4096);
	fd = open(argv[1], O_RDONLY);
	assert(fd >= 0);
	got = read(fd, head, sizeof(head));
	assert(got >= 0 && !mp3_open(fd, head, got, &c));
	assert(c && c->channels == channels && c->rate == rate);
	pcm = malloc((size_t)chunk * channels * sizeof(*pcm));
	assert(pcm);
	while ((got = c->ops->read(c, pcm, chunk)) > 0) {
		assert(got <= chunk && c->channels == channels && c->rate == rate);
		decoded += got;
		partial |= got < chunk;
	}
	assert(decoded > 0 && c->channels == channels && c->rate == rate);
	if (!strcmp(argv[5], "ok")) {
		assert(!got);
	} else {
		assert(got == -EINVAL);
		if (!strcmp(argv[5], "partial-error"))
			assert(partial);
		else
			assert(!partial);
		assert(c->ops->read(c, pcm, chunk) == -EINVAL);
	}
	c->ops->close(c);
	free(pcm);
	assert(!close(fd));
	return 0;
}
