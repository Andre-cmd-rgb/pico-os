/* The real FLAC decoder, with POSIX files and allocations on the host. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../codec/flac.c"

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

static void check_file(char **argv)
{
	struct codec *c = NULL;
	uint8_t head[16];
	int16_t samples[16];
	int fd, status, expected, frames;
	ssize_t got;

	expected = atoi(argv[2]);
	frames = atoi(argv[3]);
	fd = open(argv[1], O_RDONLY);
	assert(fd >= 0);
	got = read(fd, head, sizeof(head));
	assert(got >= 0);
	status = flac_open(fd, head, got, &c);
	if (!strcmp(argv[4], "open-error")) {
		assert(status < 0 && !c);
	} else {
		assert(!status && c && c->channels == 1 && c->rate == 16000);
		got = c->ops->read(c, samples, 16);
		if (!strcmp(argv[4], "read-error")) {
			assert(got < 0);
		} else {
			assert(got == frames);
			for (int i = 0; i < frames; i++)
				assert(samples[i] == expected);
			assert(!c->ops->read(c, samples, 16));
		}
		c->ops->close(c);
	}
	assert(!close(fd));
}

static pthread_barrier_t start;

static void *concurrent_reader(void *arg)
{
	int err = pthread_barrier_wait(&start);

	assert(!err || err == PTHREAD_BARRIER_SERIAL_THREAD);
	for (int i = 0; i < 8; i++)
		check_file(arg);
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t readers[8];

	assert(argc == 5);
	assert(!pthread_barrier_init(&start, NULL, 8));
	for (int i = 0; i < 8; i++)
		assert(!pthread_create(&readers[i], NULL, concurrent_reader, argv));
	for (int i = 0; i < 8; i++)
		assert(!pthread_join(readers[i], NULL));
	assert(!pthread_barrier_destroy(&start));
	return 0;
}
