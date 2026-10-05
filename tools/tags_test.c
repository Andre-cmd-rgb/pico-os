/*
 * The tag reader and the LRC parser, on the host.
 *
 *	tags_test FILE          the tags, one "key value" a line; the cover
 *	                        and the lyrics written beside it, as
 *	                        FILE.cover and FILE.lyrics (the lyrics as UTF-8)
 *	tags_test lrc FILE MS.. the lyrics parsed: "ms end text" a line, then
 *	                        each timed word "  ms word"; then where each
 *	                        moment MS falls: "at MS LINE WORDS"
 *	tags_test name NAME     the title made from a file's name
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../bin/lrc.h"
#include "../bin/tags.h"

void *pt_malloc(size_t size) { return malloc(size); }
void pt_free(void *ptr) { free(ptr); }

static int read_at(void *ctx, uint64_t off, void *buf, size_t len)
{
	ssize_t got = pread(*(int *)ctx, buf, len, off);

	return got < 0 ? -errno : (int)got;
}

/* A blob of the file read out, `room` bytes of buffer for it. */
static char *blob(int fd, const struct tag_blob *b, size_t room)
{
	char *buf = malloc(room);

	assert(buf && b->len <= room);
	assert(pread(fd, buf, b->len, b->off) == (ssize_t)b->len);
	return buf;
}

static void save(const char *path, const char *suffix, const void *data, size_t len)
{
	char name[4096];
	FILE *f;

	snprintf(name, sizeof(name), "%s.%s", path, suffix);
	f = fopen(name, "wb");
	assert(f && fwrite(data, 1, len, f) == len && !fclose(f));
}

static int show_tags(const char *path)
{
	struct tags t;
	struct stat st;
	int fd = open(path, O_RDONLY), ret;

	assert(fd >= 0 && !fstat(fd, &st));
	ret = tags_read(&t, read_at, &fd, st.st_size);
	printf("status %d\ntitle %s\nartist %s\nalbum %s\nalbum_artist %s\n", ret, t.title, t.artist,
	       t.album, t.album_artist);
	printf("track %d\ndisc %d\nyear %d\nms %u\nrate %d\nchannels %d\nbits %d\nkbps %d\nflac %d\n",
	       t.track, t.disc, t.year, t.ms, t.rate, t.channels, t.bits, t.kbps, t.flac);
	if (t.cover.len) {
		char *c = blob(fd, &t.cover, t.cover.len);
		size_t n = t.cover.len;

		if (t.cover.unsync) {		/* undone, as music does before decoding */
			n = 0;
			for (size_t i = 0; i < t.cover.len; i++) {
				c[n++] = c[i];
				if ((unsigned char)c[i] == 0xff && i + 1 < t.cover.len && !c[i + 1])
					i++;
			}
		}
		save(path, "cover", c, n);
		free(c);
	}
	if (t.lyrics.len) {
		size_t room = 2 * (size_t)t.lyrics.len + 1;
		char *l = blob(fd, &t.lyrics, room);
		size_t n = tags_utf8(&t.lyrics, l, t.lyrics.len, room);

		assert(n < room && !l[n] && strlen(l) == n);
		save(path, "lyrics", l, n);
		free(l);
	}
	close(fd);
	return 0;
}

static int show_lrc(const char *path, int n, char **moments)
{
	struct lrc l;
	struct stat st;
	char *text;
	FILE *f = fopen(path, "rb");

	assert(f && !stat(path, &st));
	text = malloc(st.st_size + 1);
	assert(text && fread(text, 1, st.st_size, f) == (size_t)st.st_size);
	fclose(f);
	assert(!lrc_parse(&l, text, st.st_size));
	printf("synced %d lines %d\n", l.synced, l.n);
	for (int i = 0; i < l.n; i++) {
		printf("%d %d %s\n", l.lines[i].ms, l.lines[i].end, l.lines[i].text);
		for (int k = 0; k < l.lines[i].nwords; k++)
			printf("  %d %.*s\n", l.lines[i].words[k].ms, l.lines[i].words[k].len,
			       l.lines[i].text + l.lines[i].words[k].at);
	}
	for (int i = 0; i < n; i++) {
		int32_t ms = atoi(moments[i]);
		int line = lrc_line_at(&l, ms);

		printf("at %d %d %d\n", ms, line, line >= 0 ? lrc_words_sung(&l.lines[line], ms) : -2);
	}
	lrc_free(&l);
	free(text);
	return 0;
}

int main(int argc, char **argv)
{
	char out[256];

	if (argc >= 3 && !strcmp(argv[1], "lrc"))
		return show_lrc(argv[2], argc - 3, argv + 3);
	if (argc == 3 && !strcmp(argv[1], "name")) {
		tags_title_from_name(argv[2], out, sizeof(out));
		printf("%s\n", out);
		return 0;
	}
	assert(argc == 2);
	return show_tags(argv[1]);
}
