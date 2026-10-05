/*
 * music - the music player: ~/music as a library of albums, the cover
 * beside the song, and its lyrics moving with it.
 *
 * The library is every FLAC, MP3 and WAV under ~/music. Its tags are read
 * once and kept in ~/.config/music/index, so a start reads only what is
 * new or changed. A song picked from a list plays on into the rest of it.
 * The cover is the one in the file, or a cover.jpg (folder.jpg) beside it;
 * the lyrics are the file's own, or a .lrc of the same name. With stamps
 * they follow the song a line at a time, and a word at a time where the
 * file times each word.
 *
 * Playing holds nothing else up: it goes on while another terminal is in
 * front, and the screen may go dark meanwhile -- a key lights it again.
 * Where it was is kept in ~/.config/music/state for the next time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sdkconfig.h"

#include "codec.h"
#include "drivers/drivers.h"
#include "esp_random.h"
#include "jpeg.h"
#include "lrc.h"
#include "pt/kernel.h"
#include "sink.h"
#include "tags.h"
#include "util.h"

#if CONFIG_PT_AUDIO

#define MUSIC_STACK_KB	24		/* minimp3's scratch is on the stack, as in play */
#define SEEK_MS		10000
#define VOLUME_STEP	5
#define TICK_MS		125		/* how often the time and the lyrics move on */
#define COVER_ROWS	10		/* the cover's height, in lines */
#define DRAIN_US	60000		/* a song's end once this little is left to hear */
#define INDEX_HEAD	"music index 1"
#define FIELDS		12

/* ------------------------------------------------------------ the library */

struct song {
	uint32_t path, title, artist, album, album_artist;	/* in the strings */
	uint32_t ms;
	uint64_t size;
	int64_t	 mtime;
	uint16_t track, disc, year;
	bool	 seen;			/* found on the card in this scan */
};

struct album {
	int	 first, n;		/* its songs: lib->order[first] on */
	int	 year;
};

struct library {
	struct song *songs;
	int	 n, cap;
	char	*str;			/* every string, one after another */
	size_t	 len, room;
	int	*order;			/* by album artist, album, disc, track */
	struct album *albums;
	int	 nalbums;
	bool	 changed;		/* the index wants writing again */
};

#define S(lib, off)	((lib)->str + (off))

typedef int (*cmp_fn)(const void *a, const void *b, void *ctx);

/* A stable merge sort that passes `ctx` on (qsort_r comes in two incompatible kinds). */
static int sort(void *base, size_t n, size_t size, cmp_fn cmp, void *ctx)
{
	char *a = base, *tmp;

	if (n < 2)
		return 0;
	if (!(tmp = pt_malloc(n * size)))
		return -ENOMEM;
	for (size_t width = 1; width < n; width *= 2) {
		for (size_t lo = 0; lo < n; lo += 2 * width) {
			size_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
			size_t i = lo, j = mid, k = lo;

			while (i < mid && j < hi)
				memcpy(tmp + k++ * size, cmp(a + j * size, a + i * size, ctx) < 0 ?
				       a + j++ * size : a + i++ * size, size);
			while (i < mid)
				memcpy(tmp + k++ * size, a + i++ * size, size);
			while (j < hi)
				memcpy(tmp + k++ * size, a + j++ * size, size);
		}
		memcpy(a, tmp, n * size);
	}
	pt_free(tmp);
	return 0;
}

static uint32_t intern(struct library *l, const char *s)
{
	size_t n = strlen(s) + 1;

	if (l->len + n > l->room) {
		size_t room = (l->room ? l->room * 2 : 16384) + n;
		char *str = pt_realloc(l->str, room);

		if (!str)
			return 0;		/* offset 0 is always "" */
		l->str = str;
		l->room = room;
	}
	memcpy(l->str + l->len, s, n);
	l->len += n;
	return l->len - n;
}

static struct song *new_song(struct library *l)
{
	if (l->n == l->cap) {
		int cap = l->cap ? l->cap * 2 : 256;
		struct song *s = pt_realloc(l->songs, cap * sizeof(*s));

		if (!s)
			return NULL;
		l->songs = s;
		l->cap = cap;
	}
	memset(&l->songs[l->n], 0, sizeof(l->songs[0]));
	return &l->songs[l->n++];
}

static void library_free(struct library *l)
{
	pt_free(l->songs);
	pt_free(l->str);
	pt_free(l->order);
	pt_free(l->albums);
	memset(l, 0, sizeof(*l));
}

static bool is_music(const char *name)
{
	const char *dot = strrchr(name, '.');

	return dot && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".flac") || !strcasecmp(dot, ".wav"));
}

/* tags_read()'s way into a file */
static int read_at(void *ctx, uint64_t off, void *buf, size_t len)
{
	int fd = *(int *)ctx;
	size_t done = 0;

	if (pt_lseek(fd, (off_t)off, SEEK_SET) < 0)
		return -EIO;
	while (done < len) {
		ssize_t n = pt_read(fd, (char *)buf + done, len - done);

		if (n < 0)
			return (int)n;
		if (!n)
			break;
		done += n;
	}
	return (int)done;
}

/* A song's tags from its file into `s`; false if it is not music after all. */
static bool read_song(struct library *l, struct song *s, const char *path, const struct pt_stat *st)
{
	struct tags t;
	char title[TAG_TEXT];
	int fd = pt_open(path, O_RDONLY);

	if (fd < 0)
		return false;
	if (tags_read(&t, read_at, &fd, st->size)) {
		pt_close(fd);
		return false;
	}
	pt_close(fd);
	if (!t.title[0]) {
		tags_title_from_name(path, title, sizeof(title));
		strlcpy(t.title, title, sizeof(t.title));
	}
	s->path = intern(l, path);
	s->title = intern(l, t.title);
	s->artist = intern(l, t.artist);
	s->album = intern(l, t.album);
	s->album_artist = intern(l, t.album_artist[0] ? t.album_artist : t.artist);
	s->ms = t.ms;
	s->size = st->size;
	s->mtime = st->mtime;
	s->track = t.track;
	s->disc = t.disc;
	s->year = t.year;
	s->seen = true;
	return true;
}

/* A field of an index line, tabs being the separators: it is cut off there. */
static char *field(char **p)
{
	char *f = *p, *tab = strchr(f, '\t');

	if (tab) {
		*tab = '\0';
		*p = tab + 1;
	} else {
		*p = f + strlen(f);
	}
	return f;
}

/* What tags may hold that the index cannot: tabs and newlines become spaces. */
static void put_field(int fd, const char *s, bool last)
{
	char buf[TAG_TEXT * 2 + 2];
	size_t n = 0;

	for (; *s && n < sizeof(buf) - 2; s++)
		buf[n++] = *s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s;
	buf[n++] = last ? '\n' : '\t';
	write_all(fd, buf, n);
}

static int by_path(const void *a, const void *b, void *ctx)
{
	const struct library *l = ctx;

	return strcmp(S(l, ((const struct song *)a)->path), S(l, ((const struct song *)b)->path));
}

static void index_read(struct library *l, const char *path)
{
	struct lines in;
	char *line;
	size_t len;
	int fd = pt_open(path, O_RDONLY);

	if (fd < 0)
		return;
	lines_init(&in, fd);
	line = lines_next(&in, &len);
	if (!line || strncmp(line, INDEX_HEAD "\n", strlen(INDEX_HEAD) + 1)) {
		lines_free(&in);
		pt_close(fd);
		l->changed = true;		/* another version: read every file again */
		return;
	}
	while ((line = lines_next(&in, &len))) {
		char *f[FIELDS], *p = line;
		struct song *s;

		if (len && line[len - 1] == '\n')
			line[len - 1] = '\0';
		for (int i = 0; i < FIELDS; i++)
			f[i] = field(&p);
		if (!*f[0] || !(s = new_song(l)))
			continue;
		s->path = intern(l, f[0]);
		s->size = strtoull(f[1], NULL, 10);
		s->mtime = strtoll(f[2], NULL, 10);
		s->ms = strtoul(f[3], NULL, 10);
		s->track = atoi(f[4]);
		s->disc = atoi(f[5]);
		s->year = atoi(f[6]);
		s->title = intern(l, f[8]);
		s->artist = intern(l, f[9]);
		s->album = intern(l, f[10]);
		s->album_artist = intern(l, f[11]);
	}
	lines_free(&in);
	pt_close(fd);
}

static void index_write(const struct library *l, const char *path)
{
	char tmp[PT_PATH_MAX + 8], num[96];
	int fd;

	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if ((fd = pt_open(tmp, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
		return;
	write_all(fd, INDEX_HEAD "\n", strlen(INDEX_HEAD) + 1);
	for (int i = 0; i < l->n; i++) {
		const struct song *s = &l->songs[i];

		put_field(fd, S(l, s->path), false);
		snprintf(num, sizeof(num), "%llu\t%lld\t%lu\t%u\t%u\t%u\t0\t", (unsigned long long)s->size,
			 (long long)s->mtime, (unsigned long)s->ms, s->track, s->disc, s->year);
		write_all(fd, num, strlen(num));
		put_field(fd, S(l, s->title), false);
		put_field(fd, S(l, s->artist), false);
		put_field(fd, S(l, s->album), false);
		put_field(fd, S(l, s->album_artist), true);
	}
	if (pt_close(fd) || pt_rename(tmp, path))
		pt_unlink(tmp);
}

/* The song at `path`, from the index's sorted list (the first `known`). */
static struct song *known_song(struct library *l, int known, const char *path)
{
	int lo = 0, hi = known - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2, c = strcmp(path, S(l, l->songs[mid].path));

		if (!c)
			return &l->songs[mid];
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return NULL;
}

struct walk {
	char	**dirs;
	int	 n, cap;
};

static bool push_dir(struct walk *w, const char *path)
{
	if (w->n == w->cap) {
		int cap = w->cap ? w->cap * 2 : 16;
		char **d = pt_realloc(w->dirs, cap * sizeof(*d));

		if (!d)
			return false;
		w->dirs = d;
		w->cap = cap;
	}
	return (w->dirs[w->n] = pt_strdup(path)) && ++w->n;
}

/*
 * Every music file under `root` (or `root` itself, a file): a known one
 * whose size and time are as they were is kept as it was, anything else
 * is read. `known` songs came from the index, sorted by path.
 */
static void scan(struct library *l, const char *root, int known, bool show)
{
	struct walk w = { 0 };
	struct pt_stat st;
	char path[PT_PATH_MAX];
	int found = 0;

	if (pt_stat(root, &st))
		return;
	if (!st.is_dir) {
		struct song *s = new_song(l);

		if (s && !read_song(l, s, root, &st))
			l->n--;
		return;
	}
	push_dir(&w, root);
	while (w.n && !pt_interrupted()) {
		char *dir = w.dirs[--w.n];
		struct pt_dirent ent;
		pt_dir_t *d;

		if (pt_opendir(dir, &d)) {
			pt_free(dir);
			continue;
		}
		while (pt_readdir(d, &ent) == 1) {
			struct song *s;

			if (ent.name[0] == '.' || (size_t)snprintf(path, sizeof(path), "%s/%s", dir,
								   ent.name) >= sizeof(path))
				continue;
			if (ent.is_dir) {
				push_dir(&w, path);
				continue;
			}
			if (!is_music(ent.name) || pt_stat(path, &st))
				continue;
			s = known_song(l, known, path);
			if (s && s->size == st.size && s->mtime == st.mtime) {
				s->seen = true;
				continue;
			}
			if (s)
				s->seen = false;	/* changed: the old entry goes, a new one comes */
			l->changed = true;
			if ((s = new_song(l)) && !read_song(l, s, path, &st))
				l->n--;
			if (show && ++found % 10 == 0)
				pt_printf("\r reading the library: %d new or changed\x1b[K", found);
		}
		pt_closedir(d);
		pt_free(dir);
	}
	while (w.n)
		pt_free(w.dirs[--w.n]);
	pt_free(w.dirs);
}

/* Songs no longer on the card go, and with them their place in the index. */
static void drop_unseen(struct library *l)
{
	int k = 0;

	for (int i = 0; i < l->n; i++)
		if (l->songs[i].seen)
			l->songs[k++] = l->songs[i];
	l->changed |= k != l->n;
	l->n = k;
}

static int text_cmp(const char *a, const char *b)
{
	return strcasecmp(a, b);
}

static int by_album(const void *pa, const void *pb, void *ctx)
{
	const struct library *l = ctx;
	const struct song *a = &l->songs[*(const int *)pa], *b = &l->songs[*(const int *)pb];
	int c;

	if ((c = text_cmp(S(l, a->album_artist), S(l, b->album_artist))) ||
	    (c = text_cmp(S(l, a->album), S(l, b->album))))
		return c;
	if (a->disc != b->disc)
		return a->disc - b->disc;
	if (a->track != b->track)
		return a->track - b->track;
	return text_cmp(S(l, a->title), S(l, b->title));
}

/* The songs in album order, and the albums they make. */
static int organise(struct library *l)
{
	pt_free(l->order);
	pt_free(l->albums);
	l->order = pt_malloc((l->n + 1) * sizeof(*l->order));
	l->albums = pt_malloc((l->n + 1) * sizeof(*l->albums));
	l->nalbums = 0;
	if (!l->order || !l->albums)
		return -ENOMEM;
	for (int i = 0; i < l->n; i++)
		l->order[i] = i;
	if (sort(l->order, l->n, sizeof(*l->order), by_album, l))
		return -ENOMEM;
	for (int i = 0; i < l->n; i++) {
		const struct song *s = &l->songs[l->order[i]];
		struct album *a = l->nalbums ? &l->albums[l->nalbums - 1] : NULL;
		const struct song *f = a ? &l->songs[l->order[a->first]] : NULL;

		if (!f || text_cmp(S(l, s->album), S(l, f->album)) ||
		    text_cmp(S(l, s->album_artist), S(l, f->album_artist))) {
			a = &l->albums[l->nalbums++];
			*a = (struct album){ .first = i };
		}
		a->n++;
		if (s->year > a->year)
			a->year = s->year;
	}
	return 0;
}

static int load_library(struct library *l, const char *dir, const char *index)
{
	int known;

	index_read(l, index);
	known = l->n;
	if (sort(l->songs, known, sizeof(*l->songs), by_path, l))
		return -ENOMEM;
	scan(l, dir, known, true);
	drop_unseen(l);
	if (l->changed)
		index_write(l, index);
	return organise(l);
}

/* ------------------------------------------------------------ playing */

enum repeat { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };

struct player {
	struct sink	 sink;
	bool		 sink_open;
	struct codec	*c;
	int		 fd;
	int32_t		*pcm;
	int		 song;		/* in the library; -1: none */
	uint64_t	 base, fed;	/* the file's frame where feeding began, frames fed since */
	int		 rate;
	uint32_t	 ms;		/* how long it is */
	bool		 playing;	/* a song is open */
	bool		 paused, ended;
	int64_t		 held_ms;	/* where it stands while paused or stopped */
	char		 format[40];
	struct lrc	 lrc;
	char		*lyrics;	/* what the lrc points into */
	uint8_t		*cover;		/* RGB565, cover_px square */
	int		 cover_px;
};

static int64_t position_ms(const struct player *p)
{
	int64_t frames;

	if (!p->playing || p->paused || !p->rate)
		return p->held_ms;
	frames = (int64_t)(p->base + p->fed) - (int64_t)audio_queued_us() * p->rate / 1000000;
	if (frames < (int64_t)p->base)
		frames = p->base;
	return frames * 1000 / p->rate;
}

static void close_song(struct player *p)
{
	if (p->c)
		codec_close(p->c);
	if (p->fd >= 0)
		pt_close(p->fd);
	p->c = NULL;
	p->fd = -1;
	p->playing = false;
	lrc_free(&p->lrc);
	pt_free(p->lyrics);
	p->lyrics = NULL;
	pt_free(p->cover);
	p->cover = NULL;
}

/* A blob of the file read out whole; `room` at least its length. */
static char *read_blob(int fd, const struct tag_blob *b, size_t room)
{
	char *buf = pt_malloc(room);

	if (buf && read_at(&fd, b->off, buf, b->len) != (int)b->len) {
		pt_free(buf);
		buf = NULL;
	}
	return buf;
}

/* A file beside the song: the same name with `ext`, or `name` in its folder. */
static int beside(const char *song, const char *ext, const char *name, char *out, size_t size)
{
	const char *slash = strrchr(song, '/'), *dot = strrchr(song, '.');

	if (ext && dot && (!slash || dot > slash)) {
		if ((size_t)(dot - song) + strlen(ext) >= size)
			return -ENAMETOOLONG;
		memcpy(out, song, dot - song);
		strcpy(out + (dot - song), ext);
	} else if (name && slash) {
		if ((size_t)(slash - song) + 1 + strlen(name) >= size)
			return -ENAMETOOLONG;
		memcpy(out, song, slash - song + 1);
		strcpy(out + (slash - song) + 1, name);
	} else {
		return -ENOENT;
	}
	return 0;
}

/* A whole small file into memory (a .lrc, a cover.jpg): its length, or <0. */
static ssize_t slurp(const char *path, char **out, size_t extra, size_t limit)
{
	struct pt_stat st;
	struct tag_blob all;
	int fd;

	if (pt_stat(path, &st) || st.is_dir || !st.size || st.size > limit)
		return -ENOENT;
	if ((fd = pt_open(path, O_RDONLY)) < 0)
		return fd;
	all = (struct tag_blob){ .off = 0, .len = (uint32_t)st.size };
	*out = read_blob(fd, &all, st.size * extra + 1);
	pt_close(fd);
	return *out ? (ssize_t)st.size : -ENOMEM;
}

static void load_lyrics(struct player *p, int fd, const struct tags *t, const char *path)
{
	char side[PT_PATH_MAX];
	size_t n = 0;

	if (t->lyrics.len && t->lyrics.len < 1024 * 1024) {
		size_t room = 2 * (size_t)t->lyrics.len + 1;

		if ((p->lyrics = read_blob(fd, &t->lyrics, room)))
			n = tags_utf8(&t->lyrics, p->lyrics, t->lyrics.len, room);
	} else if (!beside(path, ".lrc", NULL, side, sizeof(side))) {
		ssize_t got = slurp(side, &p->lyrics, 1, 256 * 1024);

		if (got > 0) {
			n = got;
			p->lyrics[n] = '\0';
		}
	}
	if (p->lyrics && lrc_parse(&p->lrc, p->lyrics, n))
		memset(&p->lrc, 0, sizeof(p->lrc));
}

/* ------------------------------------------------------------ the cover */

struct picture {
	uint8_t	*px;			/* RGB565 high byte first, w x h */
	int	 w, h;
};

static int cover_band(void *ctx, int y, int rows, const uint8_t *px, size_t stride)
{
	struct picture *pic = ctx;

	for (int r = 0; r < rows && y + r < pic->h; r++)
		memcpy(pic->px + (size_t)(y + r) * pic->w * 2, px + r * stride, (size_t)pic->w * 2);
	return 0;
}

/*
 * A JPEG at the smallest scale that is still at least `size` across (only
 * an eighth for a progressive one), then made `size` square by averaging
 * each output pixel's share of it -- or by repeating, if it is smaller.
 */
static uint8_t *decode_cover(const uint8_t *data, size_t len, int size)
{
	struct jpeg *j = pt_malloc(sizeof(*j));
	struct picture pic = { 0 };
	uint8_t *band = NULL, *out = NULL;
	int scale = 3;

	if (!j)
		return NULL;
	j->p = data;
	j->end = data + len;
	j->refill = NULL;
	if (jpeg_open(j))
		goto done;
	if (!j->progressive)
		while (scale > 0 && jpeg_scaled_w(j, scale) < size && jpeg_scaled_h(j, scale) < size)
			scale--;
	pic.w = jpeg_scaled_w(j, scale);
	pic.h = jpeg_scaled_h(j, scale);
	pic.px = pt_malloc((size_t)pic.w * pic.h * 2);
	band = pt_malloc(jpeg_band_size(j, scale));
	if (j->progressive)
		j->blocks = pt_malloc(jpeg_dc_size(j));
	if (!pic.px || !band || (j->progressive && !j->blocks) ||
	    jpeg_decode(j, scale, band, cover_band, &pic) || !(out = pt_malloc((size_t)size * size * 2)))
		goto done;
	/* the middle square of a picture that is not one */
	{
		int side = pic.w < pic.h ? pic.w : pic.h, x0 = (pic.w - side) / 2, y0 = (pic.h - side) / 2;

		for (int y = 0; y < size; y++) {
			int sy0 = y * side / size, sy1 = (y + 1) * side / size;

			if (sy1 <= sy0)
				sy1 = sy0 + 1;
			for (int x = 0; x < size; x++) {
				int sx0 = x * side / size, sx1 = (x + 1) * side / size, r = 0, g = 0, b = 0, n = 0;

				if (sx1 <= sx0)
					sx1 = sx0 + 1;
				for (int sy = sy0; sy < sy1; sy++)
					for (int sx = sx0; sx < sx1; sx++) {
						const uint8_t *s = pic.px + ((size_t)(y0 + sy) * pic.w + x0 + sx) * 2;
						unsigned v = s[0] << 8 | s[1];

						r += v >> 11;
						g += v >> 5 & 63;
						b += v & 31;
						n++;
					}
				r /= n;
				g /= n;
				b /= n;
				out[((size_t)y * size + x) * 2] = (uint8_t)(r << 3 | g >> 3);
				out[((size_t)y * size + x) * 2 + 1] = (uint8_t)(g << 5 | b);
			}
		}
	}
done:
	pt_free(j->blocks);
	pt_free(j);
	pt_free(band);
	pt_free(pic.px);
	return out;
}

/* ID3 unsynchronisation undone in place: the new length. */
static size_t resync(uint8_t *s, size_t len)
{
	size_t n = 0;

	for (size_t i = 0; i < len; i++) {
		s[n++] = s[i];
		if (s[i] == 0xff && i + 1 < len && !s[i + 1])
			i++;
	}
	return n;
}

static void load_cover(struct player *p, int fd, const struct tags *t, const char *path, int size)
{
	static const char *const names[] = { "cover.jpg", "folder.jpg", "front.jpg", "Cover.jpg", "Folder.jpg" };
	char side[PT_PATH_MAX], *data = NULL;
	size_t len = 0;

	if (size < 16)
		return;
	if (t->cover.len && t->cover.len < 8 * 1024 * 1024 && (data = read_blob(fd, &t->cover, t->cover.len))) {
		len = t->cover.unsync ? resync((uint8_t *)data, t->cover.len) : t->cover.len;
	} else {
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !data; i++) {
			ssize_t got;

			if (!beside(path, NULL, names[i], side, sizeof(side)) &&
			    (got = slurp(side, &data, 1, 4 * 1024 * 1024)) > 0)
				len = got;
		}
	}
	if (data) {
		p->cover = decode_cover((const uint8_t *)data, len, size);
		p->cover_px = size;
	}
	pt_free(data);
}

/* "FLAC, 24-bit, 48 kHz" or "MP3, 320 kb/s, 44.1 kHz" */
static void describe(char *out, size_t size, const struct tags *t, const struct codec *c)
{
	char rate[16];

	if (c->rate % 1000)
		snprintf(rate, sizeof(rate), "%d.%d kHz", c->rate / 1000, c->rate % 1000 / 100);
	else
		snprintf(rate, sizeof(rate), "%d kHz", c->rate / 1000);
	if (t->flac || (c->bits && !t->kbps))
		snprintf(out, size, "%s, %d-bit, %s", codec_name(c), c->bits, rate);
	else if (t->kbps)
		snprintf(out, size, "%s, %d kb/s, %s", codec_name(c), t->kbps, rate);
	else
		snprintf(out, size, "%s, %s", codec_name(c), rate);
}

/* Open a song and get it ready to play from `start_ms`; 0 or -errno. */
static int open_song(struct player *p, const struct library *l, int song, int64_t start_ms, int cover)
{
	const char *path = S(l, l->songs[song].path);
	struct pt_stat st;
	struct tags t;
	int ret;

	close_song(p);
	p->song = song;
	p->held_ms = start_ms;
	if ((p->fd = pt_open(path, O_RDONLY)) < 0)
		return p->fd;
	if (pt_stat(path, &st) || tags_read(&t, read_at, &p->fd, st.size))
		memset(&t, 0, sizeof(t));
	load_lyrics(p, p->fd, &t, path);
	load_cover(p, p->fd, &t, path, cover);
	/* codec_open() starts from the top, wherever the tags left the file */
	if ((ret = codec_open(p->fd, &p->c)))
		return ret;
	if (!p->sink_open) {
		if ((ret = sink_open(&p->sink, false)))
			return ret;
		p->sink_open = true;
	}
	if ((ret = sink_format(&p->sink, "music", path, p->c->rate, p->c->channels)))
		return ret;
	p->rate = p->c->rate;
	p->ms = p->c->frames ? (uint32_t)(p->c->frames * 1000 / p->rate) : t.ms;
	p->base = p->fed = 0;
	if (start_ms > 0) {
		int64_t at = codec_seek(p->c, (uint64_t)start_ms * p->rate / 1000);

		p->base = at > 0 ? at : 0;
	}
	describe(p->format, sizeof(p->format), &t, p->c);
	p->playing = true;
	p->paused = p->ended = false;
	return 0;
}

/* Stops what is queued and starts again from `ms`. */
static void seek_to(struct player *p, int64_t ms)
{
	int64_t at;

	if (!p->c)
		return;
	if (ms < 0)
		ms = 0;
	if (p->ms && ms > p->ms)
		ms = p->ms;
	audio_discard();
	at = codec_seek(p->c, (uint64_t)ms * p->rate / 1000);
	if (at >= 0) {
		p->base = at;
		p->fed = 0;
	}
	p->held_ms = at >= 0 ? at * 1000 / p->rate : ms;
	p->ended = false;
}

/* ------------------------------------------------------------ the queue */

struct queue {
	int	*songs;			/* in the order picked */
	int	*shuffled;		/* the same, mixed, when shuffle is on */
	int	 n, at;
	bool	 shuffle;
	enum repeat repeat;
};

static int queue_song(const struct queue *q, int at)
{
	return q->shuffle ? q->shuffled[at] : q->songs[at];
}

/* The list as it plays from here; shuffled with the picked song first. */
static int queue_set(struct queue *q, const int *list, int n, int first)
{
	pt_free(q->songs);
	pt_free(q->shuffled);
	q->songs = pt_malloc((n + 1) * sizeof(int));
	q->shuffled = pt_malloc((n + 1) * sizeof(int));
	if (!q->songs || !q->shuffled)
		return -ENOMEM;
	memcpy(q->songs, list, n * sizeof(int));
	q->n = n;
	memcpy(q->shuffled, list, n * sizeof(int));
	for (int i = n - 1; i > 0; i--) {
		int k = (int)(esp_random() % (uint32_t)(i + 1)), v = q->shuffled[i];

		q->shuffled[i] = q->shuffled[k];
		q->shuffled[k] = v;
	}
	for (int i = 0; i < n; i++)
		if (q->shuffled[i] == list[first]) {
			q->shuffled[i] = q->shuffled[0];
			q->shuffled[0] = list[first];
		}
	q->at = q->shuffle ? 0 : first;
	return 0;
}

/* Where the song now playing is, in the other order, when shuffle changes. */
static void queue_toggle(struct queue *q)
{
	int song = q->n ? queue_song(q, q->at) : -1;

	q->shuffle = !q->shuffle;
	for (int i = 0; i < q->n; i++)
		if (queue_song(q, i) == song)
			q->at = i;
}

/* ------------------------------------------------------------ the screen */

enum view { ALBUMS, SONGS, NOW };

struct ui {
	struct library	*lib;
	struct player	 p;
	struct queue	 q;
	enum view	 view, back;	/* back: where Esc goes from NOW */
	int		 cols, rows, body;
	int		 sel, top;	/* the list's cursor and first row shown */
	int		 album;		/* the album SONGS shows; -1 all, -2 a search */
	int		*list;		/* SONGS: the songs shown */
	int		 nlist;
	char		 search[64];
	char		 note[64];
	int		 cw, ch;	/* a cell, in pixels; 0 without a screen */
	bool		 inset;		/* the cover is showing */
	int		 last_line;	/* the lyric lit last time */
	int		 last_words;
	enum screen_state screen;
	bool		 full;		/* everything wants drawing again */
};

static void fmt_time(char *out, size_t size, int64_t ms)
{
	int64_t s = ms / 1000;

	if (s >= 3600)
		snprintf(out, size, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
	else
		snprintf(out, size, "%d:%02d", (int)(s / 60), (int)(s % 60));
}

/* `s` cut to `cols` columns, from row `row` column `col`, the rest of the row cleared. */
static void put_at(int row, int col, const char *look, const char *s, int cols)
{
	size_t n = utf8_prefix(s, strlen(s), cols);

	pt_printf("\x1b[%d;%dH%s%.*s\x1b[0m\x1b[K", row, col, look, (int)n, s);
}

static void put_centred(int row, int cols, const char *look, const char *s)
{
	size_t n = utf8_prefix(s, strlen(s), cols - 2);
	int w = utf8_width(s, n);

	pt_printf("\x1b[%d;1H\x1b[0m\x1b[K\x1b[%d;%dH%s%.*s\x1b[0m", row, row, (cols - w) / 2 + 1, look, (int)n, s);
}

/* The last line: the keys, or a note in their place. */
static void key_line(struct ui *u, const char *keys)
{
	const char *s = u->note[0] ? u->note : keys;
	size_t n = utf8_prefix(s, strlen(s), u->cols);
	int pad = u->cols - utf8_width(s, n);

	pt_printf("\x1b[%d;1H\x1b[7m%.*s%*s\x1b[0m", u->rows, (int)n, s, pad > 0 ? pad : 0, "");
	u->note[0] = '\0';
}

static void hide_cover(struct ui *u)
{
	if (u->inset) {
		vt_inset_close();
		u->inset = false;
	}
}

static void show_cover(struct ui *u)
{
	uint8_t *px;
	int size = u->p.cover_px;

	if (!u->cw || !u->p.cover || u->inset)
		return;
	px = vt_inset_open(u->cw / 2, u->ch / 2, size, size);
	if (!px)
		return;
	memcpy(px, u->p.cover, (size_t)size * size * 2);
	vt_inset_show();
	u->inset = true;
}

static int cover_px(const struct ui *u)
{
	return u->cw ? COVER_ROWS * u->ch : 0;
}

/* Where the words beside the cover start. */
static int info_col(const struct ui *u)
{
	return u->cw ? (cover_px(u) + u->cw) / u->cw + 2 : 2;
}

static const char *repeat_name[] = { "", "repeat", "repeat one" };

static void draw_progress(struct ui *u, int64_t ms)
{
	char a[16], b[16], bar[64 * 3 + 16], line[256];
	int col = info_col(u), width = u->cols - col - 12, done;
	struct player *p = &u->p;

	if (width < 4)
		width = 4;
	if (width > 60)
		width = 60;
	fmt_time(a, sizeof(a), ms);
	fmt_time(b, sizeof(b), p->ms);
	done = p->ms ? (int)(ms * width / p->ms) : 0;
	if (done > width)
		done = width;
	bar[0] = '\0';
	for (int i = 0; i < width; i++) {
		if (i == done)
			strcat(bar, "\x1b[2m");
		strcat(bar, "\xe2\x96\x88");		/* a full block */
	}
	snprintf(line, sizeof(line), "%s %s\x1b[0m %s", a, bar, p->ms ? b : "");
	pt_printf("\x1b[7;%dH\x1b[0m%s\x1b[K", col, line);
}

#define WRAP_MAX	3		/* rows one line of the lyrics may take */

struct lyric_row {
	int	 line, piece;		/* which line of the lyrics, which of its rows */
};

/* Where a line of the lyrics breaks to fit `width`: its pieces, at spaces where it can. */
static int wrap(const char *t, int width, size_t off[WRAP_MAX], size_t len[WRAP_MAX])
{
	size_t total = strlen(t), at = 0;
	int n = 0;

	while (at < total && n < WRAP_MAX) {
		size_t take = utf8_prefix(t + at, total - at, width);

		if (at + take < total && t[at + take] != ' ') {
			size_t sp = take;

			while (sp > 0 && t[at + sp] != ' ')
				sp--;
			if (sp > 0)
				take = sp;		/* the word that would not fit goes down */
		}
		off[n] = at;
		len[n++] = take;
		for (at += take; at < total && t[at] == ' '; at++)
			;
	}
	return n;
}

/*
 * The lyrics under the cover: the line being sung in the middle, lit -- a
 * word at a time where the file times them -- what was sung above it, dim,
 * and what comes next below. A long line takes two or three rows.
 */
static void draw_lyrics(struct ui *u, int64_t ms, bool force)
{
	struct player *p = &u->p;
	int top = COVER_ROWS + 3, rows = u->rows - top - 1, mid = top + (rows - 1) / 2;
	int cur, words = -1, width = u->cols - 2, at, k;
	struct lyric_row show[48];
	size_t off[WRAP_MAX], len[WRAP_MAX], lit = 0;

	if (rows < 1)
		return;
	if (rows > 48)
		rows = 48;
	if (!p->lrc.n) {
		if (!force)
			return;
		for (int r = 0; r < rows; r++)
			pt_printf("\x1b[%d;1H\x1b[0m\x1b[K", top + r);
		put_centred(mid, u->cols, "\x1b[2m", p->playing ? "no lyrics" : "");
		return;
	}
	if (p->lrc.synced) {
		cur = lrc_line_at(&p->lrc, (int32_t)ms);
		if (cur >= 0)
			words = lrc_words_sung(&p->lrc.lines[cur], (int32_t)ms);
	} else {
		/* plain lyrics: they go by as the song does */
		cur = p->ms ? (int)(ms * p->lrc.n / p->ms) : 0;
	}
	if (!force && cur == u->last_line && words == u->last_words)
		return;
	u->last_line = cur;
	u->last_words = words;

	/* lay the rows out: the line being sung from the middle, the rest round it */
	for (int r = 0; r < rows; r++)
		show[r].line = -1;
	k = wrap(p->lrc.lines[cur < 0 ? 0 : cur].text, width, off, len);
	at = mid - top - (k - 1) / 2;
	for (int j = 0; j < k && at + j < rows; j++)
		show[at + j] = (struct lyric_row){ cur < 0 ? 0 : cur, j };
	for (int i = (cur < 0 ? 0 : cur) - 1, r = at - 1; i >= 0 && r >= 0; i--) {
		int n = wrap(p->lrc.lines[i].text, width, off, len);

		for (int j = n - 1; j >= 0 && r >= 0; j--, r--)
			show[r] = (struct lyric_row){ i, j };
	}
	for (int i = (cur < 0 ? 0 : cur) + 1, r = at + k; i < p->lrc.n && r < rows; i++) {
		int n = wrap(p->lrc.lines[i].text, width, off, len);

		for (int j = 0; j < n && r < rows; j++, r++)
			show[r] = (struct lyric_row){ i, j };
	}
	if (cur >= 0 && words >= 0 && p->lrc.lines[cur].nwords)
		lit = words ? p->lrc.lines[cur].words[words - 1].at + p->lrc.lines[cur].words[words - 1].len : 0;

	for (int r = 0; r < rows; r++) {
		const struct lrc_line *line;
		const char *t;
		size_t o, n;
		int w;

		pt_printf("\x1b[%d;1H\x1b[0m\x1b[K", top + r);
		if (show[r].line < 0)
			continue;
		line = &p->lrc.lines[show[r].line];
		wrap(line->text, width, off, len);
		o = off[show[r].piece];
		n = len[show[r].piece];
		t = line->text + o;
		w = utf8_width(t, n);
		pt_printf("\x1b[%d;%dH", top + r, (u->cols - w) / 2 + 1);
		if (show[r].line != cur || !p->lrc.synced) {
			pt_printf("%s%.*s\x1b[0m", show[r].line < cur || cur < 0 ? "\x1b[2m" : "", (int)n, t);
		} else if (words < 0) {
			pt_printf("\x1b[1m%.*s\x1b[0m", (int)n, t);
		} else {
			/* the words sung so far lit, the rest still to come */
			size_t on = lit <= o ? 0 : lit - o > n ? n : lit - o;

			pt_printf("\x1b[1m%.*s\x1b[0m%.*s", (int)on, t, (int)(n - on), t + on);
		}
	}
}

static void draw_now(struct ui *u, bool full)
{
	struct player *p = &u->p;
	const struct library *l = u->lib;
	int col = info_col(u), room = u->cols - col;
	int64_t ms = position_ms(p);
	char line[160];

	if (full) {
		pt_puts("\x1b[0m\x1b[2J");
		if (p->song < 0) {
			put_at(2, col, "\x1b[2m", "nothing playing: Tab for the library", room);
		} else {
			const struct song *s = &l->songs[p->song];

			put_at(2, col, "\x1b[1m", S(l, s->title), room);
			put_at(3, col, "", *S(l, s->artist) ? S(l, s->artist) : "unknown artist", room);
			if (s->year)
				snprintf(line, sizeof(line), "%s, %d", *S(l, s->album) ? S(l, s->album) : "no album",
					 s->year);
			else
				snprintf(line, sizeof(line), "%s", *S(l, s->album) ? S(l, s->album) : "no album");
			put_at(4, col, "\x1b[2m", line, room);
			put_at(5, col, "\x1b[2m", p->format, room);
		}
		if (u->q.n && u->q.at + 1 < u->q.n) {
			snprintf(line, sizeof(line), "next: %s", S(l, l->songs[queue_song(&u->q, u->q.at + 1)].title));
			put_at(11, col, "\x1b[2m", line, room);
		}
		key_line(u, " spc pause \xe2\x86\x90\xe2\x86\x92 seek n/p next s shuffle r repeat tab q");
		show_cover(u);
	}
	if (p->song >= 0) {
		snprintf(line, sizeof(line), "%s%s%s%s  vol %d%%",
			 !p->playing ? "stopped" : p->paused ? "paused" : "\xe2\x99\xaa playing",
			 u->q.shuffle ? "  shuffle" : "", u->q.repeat ? "  " : "", repeat_name[u->q.repeat],
			 audio_volume());
		put_at(8, col, "", line, room);
		draw_progress(u, ms);
	}
	draw_lyrics(u, ms, full);
}

static const struct song *row_song(const struct ui *u, int i)
{
	return &u->lib->songs[u->list[i]];
}

static int list_len(const struct ui *u)
{
	return u->view == ALBUMS ? u->lib->nalbums + 1 : u->nlist;
}

/* The song playing, a line above the keys. */
static void draw_playing(struct ui *u)
{
	const struct library *l = u->lib;
	char a[16], b[16], line[160];

	if (u->p.song < 0) {
		pt_printf("\x1b[%d;1H\x1b[0m\x1b[K", u->rows - 1);
		return;
	}
	fmt_time(a, sizeof(a), position_ms(&u->p));
	fmt_time(b, sizeof(b), u->p.ms);
	snprintf(line, sizeof(line), "%s %s  %s/%s", u->p.paused || !u->p.playing ? " " : "\xe2\x99\xaa",
		 S(l, l->songs[u->p.song].title), a, b);
	put_at(u->rows - 1, 1, "\x1b[2m", line, u->cols);
}

static void draw_list(struct ui *u, bool full)
{
	const struct library *l = u->lib;
	char head[96], line[256], t[16];
	int n = list_len(u);

	if (!full) {
		draw_playing(u);
		return;
	}
	if (u->view == ALBUMS)
		snprintf(head, sizeof(head), " Music  %d songs, %d albums", l->n, l->nalbums);
	else if (u->album >= 0)
		snprintf(head, sizeof(head), " %s", *S(l, l->songs[l->order[l->albums[u->album].first]].album) ?
			 S(l, l->songs[l->order[l->albums[u->album].first]].album) : "no album");
	else if (u->album == -2)
		snprintf(head, sizeof(head), " \"%s\": %d", u->search, u->nlist);
	else
		snprintf(head, sizeof(head), " All songs: %d", u->nlist);
	pt_printf("\x1b[1;1H\x1b[0;1m%.*s\x1b[0m\x1b[K", u->cols, head);
	for (int r = 0; r < u->body; r++) {
		int i = u->top + r;
		const char *look = i == u->sel ? "\x1b[0;7m" : "\x1b[0m";

		pt_printf("\x1b[%d;1H%s", r + 2, look);
		if (i >= n) {
			pt_puts("\x1b[0m\x1b[K");
			continue;
		}
		if (u->view == ALBUMS && i == 0) {
			snprintf(line, sizeof(line), " All songs");
		} else if (u->view == ALBUMS) {
			const struct album *a = &l->albums[i - 1];
			const struct song *s = &l->songs[l->order[a->first]];

			snprintf(line, sizeof(line), " %s \x1b[2m%s\x1b[0m%s", *S(l, s->album) ? S(l, s->album) : "no album",
				 S(l, s->album_artist), look);
		} else {
			const struct song *s = row_song(u, i);
			bool on = u->p.song == u->list[i];

			fmt_time(t, sizeof(t), s->ms);
			snprintf(line, sizeof(line), "%s%s \x1b[2m%s\x1b[0m%s", on ? "\xe2\x99\xaa" : " ",
				 S(l, s->title), S(l, s->artist), look);
		}
		/* the escapes take no room: cut by what shows */
		{
			int shown = 0;
			char *p = line;

			while (*p && shown < u->cols - 6) {
				if (*p == '\x1b') {
					char *m = strchr(p, 'm');

					if (!m)
						break;
					p = m + 1;
					continue;
				}
				shown += ((unsigned char)*p & 0xc0) != 0x80;
				p++;
			}
			while (((unsigned char)*p & 0xc0) == 0x80)
				p++;
			*p = '\0';
			/* spaces, not \x1b[K: the highlight runs across the row */
			pt_printf("%s%s%*s", line, look, u->cols - shown, "");
		}
		if (u->view == SONGS) {
			fmt_time(t, sizeof(t), row_song(u, i)->ms);
			pt_printf("\x1b[%d;%dH%s%s", r + 2, u->cols - (int)strlen(t), look, t);
		}
		pt_puts("\x1b[0m");
	}
	draw_playing(u);
	key_line(u, u->view == ALBUMS ? " enter open  / search  spc pause  n next  tab  q quit" :
		 " enter play  / search  esc back  spc pause  tab  q");
}

static void draw(struct ui *u)
{
	if (u->view == NOW) {
		draw_now(u, u->full);
	} else {
		hide_cover(u);
		draw_list(u, u->full);
	}
	u->full = false;
}

static void fix_view(struct ui *u)
{
	int n = list_len(u);

	if (u->sel >= n)
		u->sel = n - 1;
	if (u->sel < 0)
		u->sel = 0;
	if (u->sel < u->top)
		u->top = u->sel;
	if (u->sel >= u->top + u->body)
		u->top = u->sel - u->body + 1;
	if (u->top < 0)
		u->top = 0;
}

/* SONGS for album `a` (-1: every song, in album order). */
static int show_album(struct ui *u, int a)
{
	const struct library *l = u->lib;
	int first = a >= 0 ? l->albums[a].first : 0, n = a >= 0 ? l->albums[a].n : l->n;

	pt_free(u->list);
	if (!(u->list = pt_malloc((n + 1) * sizeof(int))))
		return -ENOMEM;
	memcpy(u->list, l->order + first, n * sizeof(int));
	u->nlist = n;
	u->album = a;
	u->view = SONGS;
	u->sel = u->top = 0;
	return 0;
}

static bool contains(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return true;
	return false;
}

static int search(struct ui *u)
{
	const struct library *l = u->lib;
	int n = 0;

	pt_free(u->list);
	if (!(u->list = pt_malloc((l->n + 1) * sizeof(int))))
		return -ENOMEM;
	for (int i = 0; i < l->n; i++) {
		const struct song *s = &l->songs[l->order[i]];

		if (contains(S(l, s->title), u->search) || contains(S(l, s->artist), u->search) ||
		    contains(S(l, s->album), u->search))
			u->list[n++] = l->order[i];
	}
	u->nlist = n;
	u->album = -2;
	u->view = SONGS;
	u->sel = u->top = 0;
	return 0;
}

/* ------------------------------------------------------------ remembering */

static void state_path(char *out, size_t size)
{
	if (pt_home_file(".config/music", "state", NULL, out, size))
		out[0] = '\0';
}

static void save_state(const struct ui *u)
{
	char path[PT_PATH_MAX], line[PT_PATH_MAX + 64];
	int fd;

	state_path(path, sizeof(path));
	if (!path[0] || u->p.song < 0 || (fd = pt_open(path, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
		return;
	snprintf(line, sizeof(line), "%s\t%lld\t%d\t%d\n", S(u->lib, u->lib->songs[u->p.song].path),
		 (long long)position_ms(&u->p), u->q.shuffle, u->q.repeat);
	write_all(fd, line, strlen(line));
	pt_close(fd);
}

/* The song last played and where, ready but not playing: Space goes on. */
static void restore_state(struct ui *u)
{
	char path[PT_PATH_MAX], *text = NULL, *p, *f[4];
	ssize_t n;

	state_path(path, sizeof(path));
	if (!path[0] || (n = slurp(path, &text, 1, 4096)) <= 0)
		return;
	text[n] = '\0';
	p = text;
	for (int i = 0; i < 4; i++)
		f[i] = field(&p);
	f[3][strcspn(f[3], "\n")] = '\0';
	for (int i = 0; i < u->lib->n; i++) {
		if (strcmp(S(u->lib, u->lib->songs[u->lib->order[i]].path), f[0]))
			continue;
		u->q.shuffle = atoi(f[2]) != 0;
		u->q.repeat = (enum repeat)(atoi(f[3]) % 3);
		queue_set(&u->q, u->lib->order, u->lib->n, i);
		u->p.song = u->lib->order[i];
		u->p.held_ms = strtoll(f[1], NULL, 10);
		u->p.ms = u->lib->songs[u->p.song].ms;
		break;
	}
	pt_free(text);
}

/* ------------------------------------------------------------ the program */

/* Play the song at the queue's place, or the next that will open. */
static void play_queue(struct ui *u, int64_t from_ms)
{
	hide_cover(u);
	for (int tries = 0; tries < u->q.n; tries++) {
		int song = queue_song(&u->q, u->q.at);
		int ret = open_song(&u->p, u->lib, song, tries ? 0 : from_ms, cover_px(u));

		if (!ret) {
			u->last_line = -2;
			u->full = true;
			return;
		}
		snprintf(u->note, sizeof(u->note), " cannot play %.36s", S(u->lib, u->lib->songs[song].title));
		close_song(&u->p);
		if (++u->q.at >= u->q.n)
			u->q.at = 0;
	}
	u->full = true;
}

/* What follows a song: the next, the same again, or nothing. */
static void advance(struct ui *u, int step)
{
	if (!u->q.n)
		return;
	audio_discard();
	if (u->q.repeat == REPEAT_ONE && step > 0 && u->p.ended) {
		play_queue(u, 0);
		return;
	}
	u->q.at += step;
	if (u->q.at >= u->q.n || u->q.at < 0) {
		if (u->q.repeat == REPEAT_OFF && step > 0 && u->p.ended) {
			u->q.at = u->q.n - 1;
			close_song(&u->p);
			u->p.held_ms = 0;
			u->full = true;
			return;			/* the end of the list */
		}
		u->q.at = (u->q.at + u->q.n) % u->q.n;
	}
	play_queue(u, 0);
}

static void toggle_pause(struct ui *u)
{
	struct player *p = &u->p;

	if (!p->playing) {
		if (p->song >= 0 && u->q.n)
			play_queue(u, p->held_ms);
		return;
	}
	if (!p->paused) {
		p->held_ms = position_ms(p);
		audio_discard();
		p->paused = true;
	} else {
		p->paused = false;
		seek_to(p, p->held_ms);
	}
}

static void volume(struct ui *u, int step)
{
	int v = audio_volume() + step;

	audio_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
}

/* A key: false when it is time to go. */
static bool key(struct ui *u, int k)
{
	struct player *p = &u->p;

	switch (k) {
	case 'q':
	case PT_CTRL('c'):
	case PT_KEY_EOF:
	case PT_KEY_ERROR:
		return false;
	case ' ':
		toggle_pause(u);
		u->full = true;
		return true;
	case 'n':
		advance(u, 1);
		return true;
	case 'p':
		/* a few seconds in, back to the start; otherwise the one before */
		if (p->playing && position_ms(p) > 3000)
			seek_to(p, 0);
		else
			advance(u, -1);
		u->full = true;
		return true;
	case 's':
		queue_toggle(&u->q);
		snprintf(u->note, sizeof(u->note), " shuffle %s", u->q.shuffle ? "on" : "off");
		u->full = true;
		return true;
	case 'r':
		u->q.repeat = (enum repeat)((u->q.repeat + 1) % 3);
		snprintf(u->note, sizeof(u->note), " %s", u->q.repeat ? repeat_name[u->q.repeat] : "repeat off");
		u->full = true;
		return true;
	case '+':
	case '=':
		volume(u, VOLUME_STEP);
		return true;
	case '-':
		volume(u, -VOLUME_STEP);
		return true;
	case '\t':
		u->view = u->view == NOW ? u->back : NOW;
		u->full = true;
		return true;
	}
	if (u->view == NOW) {
		switch (k) {
		case PT_KEY_LEFT:
			seek_to(p, position_ms(p) - SEEK_MS);
			break;
		case PT_KEY_RIGHT:
			seek_to(p, position_ms(p) + SEEK_MS);
			break;
		case PT_KEY_UP:
			volume(u, VOLUME_STEP);
			break;
		case PT_KEY_DOWN:
			volume(u, -VOLUME_STEP);
			break;
		case PT_KEY_ESC:
		case 'l':
			u->view = u->back;
			break;
		}
		u->full = true;
		return true;
	}
	switch (k) {
	case PT_KEY_UP: case 'k':	u->sel--; break;
	case PT_KEY_DOWN: case 'j':	u->sel++; break;
	case PT_KEY_PGUP:		u->sel -= u->body; break;
	case PT_KEY_PGDN:		u->sel += u->body; break;
	case PT_KEY_HOME: case 'g':	u->sel = 0; break;
	case PT_KEY_END: case 'G':	u->sel = list_len(u) - 1; break;
	case PT_KEY_LEFT:		seek_to(p, position_ms(p) - SEEK_MS); break;
	case PT_KEY_RIGHT:		seek_to(p, position_ms(p) + SEEK_MS); break;
	case PT_KEY_ESC:
		if (u->view == SONGS) {
			u->view = ALBUMS;
			u->sel = u->album >= 0 ? u->album + 1 : 0;
			u->top = 0;
		}
		break;
	case '/':
		u->search[0] = '\0';
		if (ask_line(u->rows, " search: ", u->search, sizeof(u->search)))
			search(u);
		break;
	case '\r':
	case '\n':
		if (u->view == ALBUMS) {
			show_album(u, u->sel - 1);
		} else if (u->nlist) {
			queue_set(&u->q, u->list, u->nlist, u->sel);
			u->back = SONGS;
			play_queue(u, 0);
			u->view = NOW;
		}
		break;
	}
	u->full = true;
	return true;
}

static void finish(struct ui *u)
{
	save_state(u);
	audio_discard();
	hide_cover(u);
	close_song(&u->p);
	if (u->p.sink_open)
		sink_close(&u->p.sink);
	pt_free(u->p.pcm);
	pt_free(u->q.songs);
	pt_free(u->q.shuffled);
	pt_free(u->list);
}

static int run(struct ui *u)
{
	int64_t tick = 0;
	struct player *p = &u->p;

	p->pcm = pt_malloc(SINK_PASS * 2 * sizeof(*p->pcm));
	if (!p->pcm)
		return -ENOMEM;
	u->full = true;
	for (;;) {
		int64_t now = pt_uptime_us();
		enum screen_state screen = power_screen();
		bool feeding = p->playing && !p->paused && !p->ended;
		int k;

		if (feeding) {
			ssize_t got = codec_read(p->c, p->pcm, SINK_PASS);

			if (got > 0) {
				if (sink_play(&p->sink, p->pcm, got, p->c->channels) == -EINTR)
					return 0;
				p->fed += got;
			} else {
				p->ended = true;	/* or broken: the next one either way */
			}
		}
		if (p->playing && p->ended && audio_queued_us() < DRAIN_US) {
			advance(u, 1);
			continue;
		}
		if (pt_interrupted())
			return 0;
		/* while it plays, sink_play() is the wait; otherwise the keys are */
		k = pt_readkey_timeout(PT_STDIN, feeding ? 0 : p->playing && p->ended ? 20 :
					 power_poll_ms(TICK_MS));
		if (k != PT_KEY_NONE && k != PT_KEY_INTR) {
			if (!key(u, k))
				return 0;
			if (u->view != NOW)
				fix_view(u);
		}
		if (screen != u->screen) {
			u->full |= screen == SCREEN_ON && u->screen == SCREEN_OFF;
			u->screen = screen;
		}
		/* the clock and the lyrics move on; nobody sees a dark screen */
		if (u->full || (screen != SCREEN_OFF && now - tick >= TICK_MS * 1000)) {
			if (u->view != NOW)
				fix_view(u);
			draw(u);
			tick = now;
		}
	}
}

static int add_args(struct library *l, int argc, char **argv, int first)
{
	for (int i = first; i < argc; i++)
		scan(l, argv[i], 0, false);
	return organise(l);
}

PT_COMPLETE(music, ": -s <file:.mp3.flac.wav>\n*: <file:.mp3.flac.wav>\n")

PT_PROGRAM_ANYCORE(music, MUSIC_STACK_KB, "the music player: ~/music, covers and lyrics\n"
	   "usage: music [-s] [file or folder...]\n"
	   "  -s  shuffle\n"
	   "The library is every FLAC, MP3 and WAV under ~/music.\n"
	   "Albums: enter opens one, enter on a song plays it\n"
	   "and the rest, / searches, tab shows what plays.\n"
	   "Playing: space pauses, arrows seek and set the\n"
	   "volume, n and p next and previous, s shuffle, r\n"
	   "repeat, q quits. It plays on while another terminal\n"
	   "is in front, and the screen may go dark.")
{
	struct library lib = { 0 };
	struct ui u = { .lib = &lib, .album = -1, .last_line = -2 };
	char dir[PT_PATH_MAX], index[PT_PATH_MAX];
	uint32_t flags;
	int first = parse_flags("music", argc, argv, "s", &flags), err = 0, tw, th;
	const char *home = pt_getenv("HOME");

	if (first < 0)
		return 2;
	if (!audio_present()) {
		pt_dprintf(PT_STDERR, "music: no audio codec on this board\n");
		return 1;
	}
	if (!pt_isatty(PT_STDIN) || !pt_isatty(PT_STDOUT)) {
		pt_dprintf(PT_STDERR, "music: needs the screen and the keyboard\n");
		return 1;
	}
	u.p.song = -1;
	u.p.fd = -1;
	u.q.shuffle = FLAG(flags, 's');
	pt_tty_size(PT_STDOUT, &u.cols, &u.rows);
	u.body = u.rows - 3;
	if (vt_has_display()) {
		vt_text_size(&tw, &th);
		u.cw = tw / u.cols;
		u.ch = vt_line_height();
	}
	pt_sigcatch(true);
	pt_tty_raw(PT_STDIN, true);
	pt_puts("\x1b[?25l\x1b[2J\x1b[H");
	if (first < argc) {
		err = add_args(&lib, argc, argv, first);
		if (!err && lib.n) {
			queue_set(&u.q, lib.order, lib.n, 0);
			u.back = SONGS;
			show_album(&u, -1);
			play_queue(&u, 0);
			u.view = NOW;
		}
	} else if (!home || snprintf(dir, sizeof(dir), "%s/music", home) >= (int)sizeof(dir) ||
		   pt_home_file(".config/music", "index", NULL, index, sizeof(index))) {
		err = -ENOENT;
	} else {
		pt_printf(" reading the library...");
		err = load_library(&lib, dir, index);
		u.view = u.back = ALBUMS;
		if (!err)
			restore_state(&u);
	}
	if (!err && !lib.n) {
		pt_puts("\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
		pt_tty_raw(PT_STDIN, false);
		pt_printf("music: no songs: put FLAC, MP3 or WAV files in ~/music\n");
		library_free(&lib);
		return 1;
	}
	if (!err)
		err = run(&u);
	finish(&u);
	pt_puts("\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
	pt_tty_raw(PT_STDIN, false);
	library_free(&lib);
	return err ? fail("music", "~/music", err) : 0;
}

#endif /* CONFIG_PT_AUDIO */
