/*
 * video - play a clip.
 *
 * There is no H.264 decoder on a 240 MHz chip and there is not going to
 * be one, so a clip is converted on a PC into what this machine decodes
 * quickly: one baseline JPEG per frame (jpeg.c), and raw 16-bit mono PCM
 * for the codec. tools/mkvideo.py does the converting.
 *
 * The file is:
 *
 *	 0  "PTV2"
 *	 4  u16 width, u16 height, u16 frames a second, u16 flags (1: sound,
 *	    2: the sound is stereo, left and right side by side)
 *	12  u32 frames, u32 sample rate, u32 sound bytes per frame
 *	24  u16 slices, two bytes kept back, u32 where the index is (0: none)
 *	32  each frame: for each slice u32 length and that many bytes of
 *	    JPEG, then the frame's sound
 *	 .  the index, after the last frame: "PTVI", u32 frames, and a u32
 *	    for each frame, where in the file it starts
 *
 * A frame is cut into horizontal slices, each its own small JPEG, because
 * this chip has two cores and one of them was watching. Slice 0 is decoded
 * by the program itself and the rest by a task on the other core, which
 * very nearly halves the time a frame takes. "PTV1" files are the same
 * thing with one slice.
 *
 * Picture and sound are interleaved so that playing is one pass through
 * the file with no seeking, which is what an SD card is good at. The
 * sound is the clock. Frames are read a few ahead of the one on the
 * screen and their sound queued at once, enough of it to ride over a slow
 * read or a slow frame; each frame notes when its sound will be heard,
 * and its picture goes up at that moment -- not when its sound was
 * queued, which put the picture a queue's length ahead of what was heard.
 * A picture that cannot be ready by then is skipped, so the sound never
 * stops for the picture.
 *
 * On another terminal the clip goes on playing its sound, as music would,
 * and decodes no pictures; back in front, it paints the last one again.
 *
 * Jumping about needs to know where each frame starts, and frames are all
 * sizes. A clip made since the index was added says; for an older one the
 * player notes where each frame it reads starts, and to go further than
 * it has been it hops from one frame's lengths to the next without
 * reading the pictures (`video -i` does that once and writes the index
 * into the clip). Where a clip was left is kept in /etc/resume, frame and
 * place in the file both, so going back to it is one seek.
 */
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "canvas.h"
#include "util.h"

#define HEADER		32
#define MAX_FRAME	(256 * 1024)	/* a sane limit on one frame */
#define MAX_SLICES	4
#define HELPER_CORE	0		/* programs run on core 1 */
#define READ_SIZE	(64 * 1024)	/* the file is taken in pieces this big */
#define SLACK_US	4000		/* kept in hand when judging a frame late */
/*
 * Sound queued ahead of the picture, at most. A slow frame and a read from
 * the card together outlasted 50 ms, and the sound broke up for a moment;
 * 150 ms rides over that, and the picture leads the sound by no more than
 * the eye forgives.
 */
#define CLIP_LATENCY_MS	150
#define MAX_AHEAD	12		/* frames read ahead, at most */
#define SHOW_LEAD_US	8000		/* a frame takes most of a refresh to reach the glass */
#define SECTOR		512
#define RESUME		"/etc/resume"
#define RESUME_KEEP	16		/* clips whose place is kept */
#define RESUME_EDGE_S	10		/* this near the start or the end, none is */
#define RESUME_ASK_MS	8000		/* then it goes on from there by itself */

/*
 * A frame read ahead: its slices, its sound, and when that sound is heard
 * (for a silent clip, when its time comes). The picture's buffer grows to
 * the biggest frame the slot has held.
 */
struct slot {
	uint8_t	*jpeg;
	size_t	 cap;
	size_t	 part[MAX_SLICES + 1];
	uint8_t	*pcm;
	int64_t	 play_at;
};

struct clip {
	int	 w, h, fps, frames;
	int	 rate, audio_bytes;	/* per frame; 0 when silent */
	int	 channels;		/* of the sound: 1 or 2 */
	int	 slices;
	uint32_t index_at;		/* where its index is, or 0 */
};

/* Where each frame starts in the file, as far as that is known; 0 is not known. */
struct index {
	uint32_t *off;			/* frames + 1: the last is the end */
};

/*
 * The file, read in big pieces. A frame is four or five small reads -- a
 * length, a slice, a length, a slice, the sound -- and the card charges
 * its access time, most of a millisecond, for each command whatever its
 * size. Taking 64 KB at a time is one command for every seven frames.
 * Starting at the top of the file keeps every piece on a sector boundary,
 * which is what lets the filesystem hand it to the card in one go.
 */
struct reader {
	int	 fd;
	uint8_t	*buf;
	size_t	 len, at;
	uint32_t base;			/* where in the file buf[0] came from */
};

static int reader_get(struct reader *r, void *dst, size_t n)
{
	uint8_t *p = dst;

	while (n) {
		size_t k;

		if (r->at == r->len) {
			int got = pt_read(r->fd, r->buf, READ_SIZE);

			if (got <= 0)
				return -EIO;
			r->base += r->len;
			r->len = got;
			r->at = 0;
		}
		k = r->len - r->at < n ? r->len - r->at : n;
		memcpy(p, r->buf + r->at, k);
		r->at += k;
		p += k;
		n -= k;
	}
	return 0;
}

static uint32_t reader_tell(const struct reader *r)
{
	return r->base + r->at;
}

/*
 * Carries on from `off`. The read starts on the sector before it, so that
 * the pieces after it are whole sectors again, as they are from the top.
 */
static int reader_seek(struct reader *r, uint32_t off)
{
	uint32_t from = off & ~(uint32_t)(SECTOR - 1);
	int got;

	if (pt_lseek(r->fd, from, SEEK_SET) < 0 || (got = pt_read(r->fd, r->buf, READ_SIZE)) < 0)
		return -EIO;
	r->base = from;
	r->len = got;
	r->at = off - from;
	return r->at <= r->len ? 0 : -EIO;
}

/*
 * The other core, decoding the slices this one is not. It waits on `go`,
 * decodes what it is pointed at, and says so on `done`.
 */
struct helper {
	TaskHandle_t	  task;
	SemaphoreHandle_t go, done, exited;
	struct canvas	  c;
	const uint8_t	 *data;
	size_t		  len;
	volatile bool	  quit;
	volatile int	  ret;
};

static void helper_task(void *arg)
{
	struct helper *h = arg;

	for (;;) {
		xSemaphoreTake(h->go, portMAX_DELAY);
		if (__atomic_load_n(&h->quit, __ATOMIC_ACQUIRE))
			break;
		h->ret = canvas_jpeg_mem(&h->c, h->data, h->len);
		xSemaphoreGive(h->done);
	}
	xSemaphoreGive(h->exited);
	vTaskSuspend(NULL);
}

/*
 * The panel, filled from one page while the next frame is decoded into
 * the other. Sending a frame is 15 ms of DMA, and there is no reason for
 * the decoder to sit through it.
 */
struct blitter {
	TaskHandle_t	  task;
	SemaphoreHandle_t go, done, exited;
	int		  vt;		/* the terminal the player holds: this is no program */
	struct canvas	 *c;
	const uint8_t	 *native;	/* its turned copy, when the panel can be read */
	volatile bool	  quit, pending;
	int		  missed;
	int		  late;
	int64_t		  transfer_us, transfer_max_us;
};

static void blit_task(void *arg)
{
	struct blitter *b = arg;

	for (;;) {
		xSemaphoreTake(b->go, portMAX_DELAY);
		if (__atomic_load_n(&b->quit, __ATOMIC_ACQUIRE))
			break;
		if (vt_screen_begin_on(b->vt)) {
			int64_t started = esp_timer_get_time(), elapsed;
			int ret = b->native ? canvas_send_native(b->c, b->native) : -ENOTSUP;

			/* A failed synchronised write must not become a tearing
			 * landscape blit. The next frame tries the refresh again. */
			if (ret == -ENOTSUP && !b->native)
				canvas_blit_fit(b->c);
			else if (ret == -EAGAIN)
				b->late++;
			else if (ret)
				b->missed++;
			elapsed = esp_timer_get_time() - started;
			b->transfer_us += elapsed;
			if (elapsed > b->transfer_max_us)
				b->transfer_max_us = elapsed;
		}
		vt_screen_end();
		xSemaphoreGive(b->done);
	}
	xSemaphoreGive(b->exited);
	vTaskSuspend(NULL);
}

static void blit_finish(struct blitter *b)
{
	if (b->pending) {
		xSemaphoreTake(b->done, portMAX_DELAY);
		b->pending = false;
	}
}

static void blit_start(struct blitter *b, struct canvas *c, const uint8_t *native)
{
	blit_finish(b);
	b->c = c;
	b->native = native;
	b->pending = true;
	xSemaphoreGive(b->go);
}

static int task_start(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
		      TaskHandle_t *task, SemaphoreHandle_t *go, SemaphoreHandle_t *done,
		      SemaphoreHandle_t *exited)
{
	*go = xSemaphoreCreateBinary();
	*done = xSemaphoreCreateBinary();
	*exited = xSemaphoreCreateBinary();
	if (!*go || !*done || !*exited ||
	    xTaskCreatePinnedToCore(fn, name, stack, arg, 4, task, HELPER_CORE) != pdPASS) {
		*task = NULL;
		return -ENOMEM;
	}
	return 0;
}

/*
 * Completion of work and exit are separate acknowledgments. Even when
 * killed during a decode or DMA transfer, keep its memory until exit.
 * Polling lets the kernel reaper continue its other chores meanwhile.
 */
static bool task_stop_step(TaskHandle_t *task, SemaphoreHandle_t *go, SemaphoreHandle_t *done,
			   SemaphoreHandle_t *exited, volatile bool *quit)
{
	if (*task) {
		__atomic_store_n(quit, true, __ATOMIC_RELEASE);
		xSemaphoreGive(*go);
		if (!xSemaphoreTake(*exited, 0))
			return false;
		vTaskDelete(*task);
		*task = NULL;
	}
	if (*go)
		vSemaphoreDelete(*go);
	if (*done)
		vSemaphoreDelete(*done);
	if (*exited)
		vSemaphoreDelete(*exited);
	*go = *done = *exited = NULL;
	return true;
}

struct video_cleanup {
	StaticSemaphore_t guard_storage;
	SemaphoreHandle_t guard;
	struct helper *helpers;
	struct blitter *blit;
	struct pt_file *tty;
	bool *boosted, *kept, *screen, *native;
};

static bool video_cleanup_step(void *arg)
{
	struct video_cleanup *c = arg;
	struct blitter *b = c->blit;
	bool done;

	/* A force-kill must not re-enter a half-published startup or cleanup
	 * update. The kernel lets a task holding this mutex release it first. */
	if (!xSemaphoreTake(c->guard, 0))
		return false;
	done = task_stop_step(&b->task, &b->go, &b->done, &b->exited, &b->quit);

	for (int i = 0; i < MAX_SLICES - 1; i++) {
		struct helper *h = &c->helpers[i];

		done &= task_stop_step(&h->task, &h->go, &h->done, &h->exited, &h->quit);
	}
	if (!done) {
		xSemaphoreGive(c->guard);
		return false;
	}
	/* the refresh as it was; the blitter, which held the bus, has stopped */
	if (*c->native) {
		if (!lcd_native_end(false)) {
			xSemaphoreGive(c->guard);
			return false;
		}
		*c->native = false;
	}
	if (*c->boosted) {
		if (!cpufreq_try_boost(false)) {
			xSemaphoreGive(c->guard);
			return false;
		}
		*c->boosted = false;
	}
	if (*c->kept) {
		power_keep_screen(false);
		*c->kept = false;
	}
	/* The reaper has no process-relative stdin. Address the held file. */
	if (*c->screen && c->tty && c->tty->ops->ioctl) {
		int raw = 0, off = -1;

		if (c->tty->ops->ioctl(c->tty, PT_TTY_TRYSETRAW, &raw) == -EAGAIN) {
			xSemaphoreGive(c->guard);
			return false;
		}
		/* A kill can interrupt readkey between its timed read and reset. */
		c->tty->ops->ioctl(c->tty, PT_TTY_SETTIMEOUT, &off);
	}
	xSemaphoreGive(c->guard);
	return true;
}

/* Where the picture goes, from one canvas to another with its own pixels. */
static void place(struct canvas *dst, const struct canvas *src)
{
	dst->x0 = src->x0;
	dst->y0 = src->y0;
	dst->dw = src->dw;
	dst->dh = src->dh;
	dst->sw = src->sw;
	dst->sh = src->sh;
	dst->placed = src->placed;
}

static uint16_t le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_header(struct reader *r, struct clip *c)
{
	uint8_t h[HEADER];

	if (reader_get(r, h, sizeof(h)))
		return -EIO;
	if (memcmp(h, "PTV1", 4) && memcmp(h, "PTV2", 4))
		return -ENOTSUP;
	c->w = le16(h + 4);
	c->h = le16(h + 6);
	c->fps = le16(h + 8);
	c->frames = (int)le32(h + 12);
	c->rate = (int)le32(h + 16);
	c->audio_bytes = le16(h + 10) & 1 ? (int)le32(h + 20) : 0;
	c->channels = le16(h + 10) & 2 ? 2 : 1;
	c->slices = memcmp(h, "PTV1", 4) ? le16(h + 24) : 1;
	c->index_at = memcmp(h, "PTV1", 4) ? le32(h + 28) : 0;
	if (c->w <= 0 || c->h <= 0 || c->fps <= 0 || c->fps > 120 || c->frames < 0 ||
	    c->audio_bytes < 0 || c->audio_bytes > 1 << 17 || c->audio_bytes % (2 * c->channels))
		return -EINVAL;
	if (c->slices < 1 || c->slices > MAX_SLICES || c->h % c->slices)
		return -EINVAL;
	/*
	 * Played at exactly one frame's sound per frame. The converter
	 * makes the samples at that rate, but older files rounded the
	 * samples in a frame down, and played at the rate they claim the
	 * sound slid a little further behind the picture every frame --
	 * most of a tenth of a second by the end of a song.
	 */
	if (c->audio_bytes) {
		c->rate = c->audio_bytes / 2 / c->channels * c->fps;
		if (c->rate < 8000 || c->rate > 48000)
			return -ENOTSUP;
	}
	return 0;
}

/* Reads a frame's slices into `f`, noting where each starts; `part[slices]` is the end. */
static int read_frame(struct reader *r, const struct clip *c, struct slot *f)
{
	size_t at = 0;

	for (int s = 0; s < c->slices; s++) {
		uint8_t len[4];
		uint32_t n;

		if (reader_get(r, len, 4))
			return -EIO;
		n = le32(len);
		if (n > MAX_FRAME - at)
			return -EINVAL;
		if (at + n > f->cap) {		/* bigger than any frame it has held */
			size_t cap = at + n > f->cap * 2 ? at + n : f->cap * 2;
			uint8_t *p;

			cap = cap > MAX_FRAME ? MAX_FRAME : cap;
			if (!(p = pt_malloc(cap)))
				return -ENOMEM;
			if (at)
				memcpy(p, f->jpeg, at);
			pt_free(f->jpeg);
			f->jpeg = p;
			f->cap = cap;
		}
		if (reader_get(r, f->jpeg + at, n))
			return -EIO;
		f->part[s] = at;
		at += n;
	}
	f->part[c->slices] = at;
	if (c->audio_bytes && reader_get(r, f->pcm, c->audio_bytes))
		return -EIO;
	return 0;
}

static int read_all(int fd, void *buf, size_t n)
{
	for (size_t done = 0; done < n;) {
		int got = pt_read(fd, (uint8_t *)buf + done, n - done);

		if (got <= 0)
			return -EIO;
		done += got;
	}
	return 0;
}

/*
 * The index: the clip's own if it has one, or else only frame 0, the rest
 * noted as frames are read or passed over. The file is left where the
 * reader expects it.
 */
static int index_open(struct reader *r, const struct clip *c, struct index *x)
{
	uint8_t head[8];

	if (!(x->off = pt_calloc((size_t)c->frames + 1, sizeof(*x->off))))
		return -ENOMEM;
	x->off[0] = HEADER;
	if (!c->index_at || !c->frames)
		return 0;
	if (pt_lseek(r->fd, c->index_at, SEEK_SET) < 0 || read_all(r->fd, head, 8) ||
	    memcmp(head, "PTVI", 4) || le32(head + 4) != (uint32_t)c->frames ||
	    read_all(r->fd, x->off, (size_t)c->frames * 4) || x->off[0] != HEADER) {
		memset(x->off, 0, ((size_t)c->frames + 1) * sizeof(*x->off));
		x->off[0] = HEADER;
	} else {
		x->off[c->frames] = c->index_at;	/* it follows the last frame */
	}
	return pt_lseek(r->fd, r->base + r->len, SEEK_SET) < 0 ? -EIO : 0;
}

/* Onward by `n`: within what has been read, or by reading on from there. */
static int reader_skip(struct reader *r, uint32_t n)
{
	if (n <= r->len - r->at) {
		r->at += n;
		return 0;
	}
	return reader_seek(r, reader_tell(r) + n);
}

/*
 * Goes to frame `t`, from the nearest frame before it whose place is
 * known, hopping from each frame's lengths to the next's. The hops go
 * through the reader's 64 KB pieces and only ever forward: a small read
 * and a seek per frame cost 10 ms each, because FAT finds a place behind
 * the one it is at by walking the file's clusters from the start.
 * Returns the frame it got to, which is short of `t` if the file is.
 */
static int seek_to(struct reader *r, const struct clip *c, struct index *x, int t)
{
	int k = t;

	while (k > 0 && !x->off[k])
		k--;
	if (k < t && reader_seek(r, x->off[k]))
		return -EIO;
	for (; k < t && !pt_interrupted(); k++) {
		uint8_t len[4];
		int s;

		for (s = 0; s < c->slices; s++)
			if (reader_get(r, len, 4) || le32(len) > MAX_FRAME ||
			    reader_skip(r, le32(len)))
				break;
		if (s < c->slices || reader_skip(r, c->audio_bytes))
			break;
		x->off[k + 1] = reader_tell(r);
	}
	/* the file may have been moved under the reader (frame_at) */
	return reader_seek(r, x->off[k]) ? -EIO : k;
}

/* Whether a frame's first slice starts at `off`: its length, and a JPEG. */
static bool frame_at(int fd, uint32_t off)
{
	uint8_t head[6];

	return pt_lseek(fd, off, SEEK_SET) >= 0 && !read_all(fd, head, 6) &&
	       le32(head) <= MAX_FRAME && head[4] == 0xff && head[5] == 0xd8;
}

/* video -i: every frame's place found, and written into the clip. */
static int index_write(struct reader *r, const struct clip *c, struct index *x)
{
	uint8_t head[8] = { 'P', 'T', 'V', 'I' }, at[4];
	uint32_t end;
	int ret;

	if (c->index_at && x->off[c->frames])
		return -EEXIST;
	if ((ret = seek_to(r, c, x, c->frames)) < 0)
		return ret;
	if (ret != c->frames)
		return pt_interrupted() ? -EINTR : -EIO;
	end = x->off[c->frames];
	memcpy(head + 4, &(uint32_t){ c->frames }, 4);
	memcpy(at, &end, 4);
	/* the index first, then the header that points at it */
	if (pt_lseek(r->fd, end, SEEK_SET) < 0 || (ret = write_all(r->fd, (const char *)head, 8)) ||
	    (ret = write_all(r->fd, (const char *)x->off, (size_t)c->frames * 4)))
		return ret < 0 ? ret : -EIO;
	if (pt_lseek(r->fd, 28, SEEK_SET) < 0 || (ret = write_all(r->fd, (const char *)at, 4)))
		return ret < 0 ? ret : -EIO;
	return 0;
}

/*
 * /etc/resume, a line a clip, the last left first: the frame it was left
 * at, where that frame starts, how many frames the clip has (a clip made
 * again under the same name is not the same clip), and its name.
 */
static bool resume_get(const char *name, const struct clip *c, int *frame, uint32_t *off)
{
	struct lines l;
	char *line;
	size_t len;
	bool found = false;
	int fd = pt_open(RESUME, O_RDONLY);

	if (fd < 0)
		return false;
	lines_init(&l, fd);
	while (!found && (line = lines_next(&l, &len))) {
		unsigned long f, o, n;
		int used = 0;

		if (len && line[len - 1] == '\n')
			line[len - 1] = '\0';
		if (sscanf(line, "%lu %lu %lu %n", &f, &o, &n, &used) == 3 && used &&
		    !strcmp(line + used, name) && n == (unsigned long)c->frames &&
		    f < n && o >= HEADER) {
			*frame = (int)f;
			*off = (uint32_t)o;
			found = true;
		}
	}
	lines_free(&l);
	pt_close(fd);
	return found;
}

/* Keeps where a clip was left, or with frame < 0 forgets it. */
static void resume_put(const char *name, const struct clip *c, int frame, uint32_t off)
{
	size_t cap = RESUME_KEEP * (PT_PATH_MAX + 40), n = 0;
	char *out = pt_malloc(cap), *line;
	struct lines l;
	int fd, kept = 0;
	size_t len;

	if (!out)
		return;
	if (frame >= 0)
		n = snprintf(out, cap, "%d %lu %d %s\n", frame, (unsigned long)off, c->frames,
			     name);
	if ((fd = pt_open(RESUME, O_RDONLY)) >= 0) {
		lines_init(&l, fd);
		while ((line = lines_next(&l, &len)) && kept < RESUME_KEEP - 1) {
			unsigned long f, o, k;
			int used = 0;
			char *nl = len && line[len - 1] == '\n' ? &line[len - 1] : NULL;

			if (nl)
				*nl = '\0';
			if (sscanf(line, "%lu %lu %lu %n", &f, &o, &k, &used) != 3 || !used ||
			    !strcmp(line + used, name) || n + strlen(line) + 2 > cap)
				continue;
			n += snprintf(out + n, cap - n, "%s\n", line);
			kept++;
		}
		lines_free(&l);
		pt_close(fd);
	}
	if ((fd = pt_open(RESUME ".new", O_WRONLY | O_CREAT | O_TRUNC)) >= 0) {
		int err = write_all(fd, out, n);

		pt_close(fd);
		if (err || pt_rename(RESUME ".new", RESUME))
			pt_unlink(RESUME ".new");
	}
	pt_free(out);
}

/*
 * The frame a key goes to from frame `i`: the arrows ten seconds and a
 * minute, the digits a tenth of the way through each. -1 for other keys.
 */
static int seek_key(int key, int i, const struct clip *c)
{
	int step;

	switch (key) {
	case PT_KEY_RIGHT:	step = 10; break;
	case PT_KEY_LEFT:	step = -10; break;
	case PT_KEY_UP:		step = 60; break;
	case PT_KEY_DOWN:	step = -60; break;
	default:
		if (key >= '0' && key <= '9')
			return (int)((int64_t)c->frames * (key - '0') / 10);
		return -1;
	}
	i += step * c->fps;
	return i < 0 ? 0 : i >= c->frames ? c->frames - 1 : i;
}

/* Sleeps until `t` on the microsecond clock, or until a signal. */
static void wait_until(int64_t t)
{
	int64_t now;

	while ((now = esp_timer_get_time()) < t - 1000 && !pt_interrupted())
		pt_sleep_ms((int)((t - now) / 1000));
}

/*
 * The frames read ahead, `from` up to `to`, heard from now on: after a
 * pause or a picture saved, when their queued sound has been thrown away
 * (a silent clip's are `away` later instead).
 */
static void requeue(struct slot *slots, int ahead, int from, int to, const struct clip *c,
		    int64_t away)
{
	for (int n = from; n < to; n++) {
		struct slot *f = &slots[n % ahead];

		if (c->audio_bytes) {
			f->play_at = esp_timer_get_time() + audio_queued_us();
			audio_write(f->pcm, c->audio_bytes, c->channels);
		} else {
			f->play_at += away;
		}
	}
}

static void save_shot(struct canvas *c, char *shot, size_t size)
{
	char dir[64];

	shots_dir(dir, sizeof(dir));
	if (vt_screen_begin())
		canvas_save(c, dir, shot, size);
	vt_screen_end();
}

/* All of a frame, bars and all: after the terminal has been on the panel. */
static void repaint(struct canvas *c)
{
	if (vt_screen_begin())
		canvas_blit(c);
	vt_screen_end();
}

static void clock_text(char *out, size_t size, int seconds)
{
	if (seconds >= 3600)
		snprintf(out, size, "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
	else
		snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

static void bar(const char *line)
{
	if (vt_screen_begin())
		vt_bar_line(lcd_height() - vt_line_height(), line);
	vt_screen_end();
}

/*
 * Paused: a line along the bottom, in the status line's colours, saying
 * where in the clip this is -- the time, and a bar filled that far --
 * and which keys do what. The picture is painted again on the way out.
 */
static void pause_bar(const struct clip *clip, int frame)
{
	char now[16], all[16], left[40], right[40], line[200];
	int cols, width, done;
	size_t n;

	clock_text(now, sizeof(now), frame / clip->fps);
	clock_text(all, sizeof(all), clip->frames / clip->fps);
	snprintf(left, sizeof(left), " || %s / %s  ", now, all);
	snprintf(right, sizeof(right), "  space: play  s: save  q: quit");
	vt_size(&cols, &width);
	width = cols - (int)strlen(left) - (int)strlen(right);
	if (width < 4) {		/* a narrow screen: the keys go */
		right[0] = '\0';
		width = cols - (int)strlen(left) - 1;
	}
	done = clip->frames ? (int)((int64_t)frame * width / clip->frames) : 0;
	n = snprintf(line, sizeof(line), "%s", left);
	for (int i = 0; i < width && n + 4 < sizeof(line); i++)
		n += snprintf(line + n, sizeof(line) - n, "%s", i < done ? "\u2588" : "\u00b7");
	snprintf(line + n, sizeof(line) - n, "%s", right);
	bar(line);
}

/* A question or a note on the bottom line, over the picture. */
static void bar_at(const char *what, int seconds, const char *after)
{
	char at[16], line[96];

	clock_text(at, sizeof(at), seconds);
	snprintf(line, sizeof(line), " %s %s%s", what, at, after);
	bar(line);
}

PT_COMPLETE(video, ": -v -i <file:.ptv>\n*: <file:.ptv>\n")

/*
 * One clip played, or indexed (`index_only`); its status, as the program's.
 * `listed`: it was picked from the list, which comes back after it -- when
 * it ends or Esc is pressed, not for q -- and *back says which it was.
 */
static int play_clip(const char *path, bool index_only, bool verbose, bool listed, bool *back)
{
	struct canvas page[2] = { { 0 }, { 0 } }, slice0 = { 0 }, *shown = &page[0];
	struct helper helpers[MAX_SLICES - 1] = { 0 };
	struct blitter blit = { 0 };
	struct reader rd = { .fd = -1 };
	struct clip clip = { 0 };
	struct index idx = { 0 };
	struct slot *slots = NULL;
	char shot[PT_PATH_MAX];
	const char *name;
	void *native_mem[2] = { NULL, NULL };
	uint8_t *native[2] = { NULL, NULL };
	int ret, next = 0, ahead = 0, nread = 0, end;
	int shown_n = 0, dropped = 0, frame_us, buffer_us = 0, i = 0, from = 0, target;
	int thinned = 0, every_us = 0, ncw, np0, nph;	/* every_us: the panel's pace */
	int64_t read_us = 0, decode_us = 0, blit_us = 0, started;
	int64_t decode_guess = 0;
	bool keys = true, screen = false, played = false, step = false, boosted = false;
	bool kept = false, native_ok = false;
	struct video_cleanup cleanup = {
		.helpers = helpers, .blit = &blit, .tty = proc_current()->fd[PT_STDIN],
		.boosted = &boosted, .kept = &kept, .screen = &screen, .native = &native_ok,
	};
	uint32_t from_off = 0;
	unsigned gen = vt_screen_gen();
	int key0;

	*back = false;
	name = path_basename(path);	/* what /etc/resume knows it by */
	if ((rd.fd = pt_open(path, index_only ? O_RDWR : O_RDONLY)) < 0)
		return fail("video", path, rd.fd);
	if (!(rd.buf = pt_malloc(READ_SIZE))) {
		ret = -ENOMEM;
		goto done;
	}
	if ((ret = read_header(&rd, &clip)) || (ret = index_open(&rd, &clip, &idx)))
		goto done;
	if (index_only) {
		pt_printf("%s: finding where its %d frames start\n", path, clip.frames);
		if ((ret = index_write(&rd, &clip, &idx)) == -EEXIST)
			pt_printf("%s has an index already\n", path);
		else if (!ret)
			pt_printf("%s: indexed\n", path);
		ret = ret == -EEXIST ? 0 : ret;
		goto done;
	}
	/* Helpers use this process's memory. Catch interruption before any
	 * of them starts, including system calls during the rest of startup. */
	pt_sigcatch(true);
	cleanup.guard = xSemaphoreCreateMutexStatic(&cleanup.guard_storage);
	if ((ret = proc_set_cleanup(video_cleanup_step, &cleanup)))
		goto done;
	/* Two pages: one being sent to the panel while the next is
	 * decoded into the other. */
	if ((ret = canvas_open(&page[0])) || (ret = canvas_open(&page[1])))
		goto done;

	/*
	 * Fit the whole picture once, then give slice 0 to this task and
	 * the rest to the other core. A one-slice file is decoded the
	 * ordinary way, letting the decoder work out where it goes.
	 */
	page[0].sw = clip.w;
	page[0].sh = clip.h;
	canvas_fit(&page[0]);
	place(&page[1], &page[0]);
	slice0 = page[0];
	if (clip.slices > 1) {
		slice0.sh = clip.h / clip.slices;
		slice0.dh = page[0].dh / clip.slices;
		slice0.placed = true;
		/* Rounding lands on slice boundaries rather than leaving a gap. */
		for (int i = 0; i < clip.slices - 1; i++) {
			struct helper *h = &helpers[i];
			int top = page[0].dh * (i + 1) / clip.slices;
			int bottom = page[0].dh * (i + 2) / clip.slices;

			h->c = slice0;
			h->c.dh = bottom - top;
			h->c.y0 = page[0].y0 + top;
			xSemaphoreTake(cleanup.guard, portMAX_DELAY);
			ret = task_start(helper_task, "vidslice", 4096, h,
					 &h->task, &h->go, &h->done, &h->exited);
			xSemaphoreGive(cleanup.guard);
			if (ret)
				goto done;
		}
	}
	/*
	 * Frames go to the panel turned into its own order, in step with its
	 * refresh, so that nothing that moves tears (canvas_blit_native). The
	 * turning is done here, on this core, while the frame waits for its
	 * sound; the blitter shares the other core with the helper decoder,
	 * leaving this core free to decode and read the next frame. A turned copy for
	 * each page, in PSRAM: one is sent while the next is made.
	 */
	frame_us = 1000000 / clip.fps;
	xSemaphoreTake(cleanup.guard, portMAX_DELAY);
	native_ok = canvas_native_shape(&page[0], &ncw, &np0, &nph) &&
		    lcd_native_begin(ncw, np0, nph, frame_us);
	xSemaphoreGive(cleanup.guard);
	for (int k = 0; k < 2 && native_ok; k++) {
		native_mem[k] = pt_malloc((size_t)page[0].w * page[0].h * 2 + 63);
		native[k] = native_mem[k] ?
			(uint8_t *)(((uintptr_t)native_mem[k] + 63) & ~(uintptr_t)63) : NULL;
	}
	if (native_ok && (!native[0] || !native[1])) {
		ret = -ENOMEM;
		goto done;
	}
	xSemaphoreTake(cleanup.guard, portMAX_DELAY);
	ret = task_start(blit_task, "vidblit", 3072, &blit, &blit.task, &blit.go,
			 &blit.done, &blit.exited);
	xSemaphoreGive(cleanup.guard);
	if (ret)
		goto done;

	if (clip.audio_bytes) {
		/* a few frames queued at most, or the sound lags the picture */
		if ((ret = audio_set_rate(clip.rate)) || (ret = audio_set_latency(CLIP_LATENCY_MS)))
			goto done;
		buffer_us = audio_buffer_us();
	}
	/* Enough frames ahead to cover the sound queued, and a few more. */
	ahead = (buffer_us + frame_us - 1) / frame_us + 3;
	ahead = ahead > MAX_AHEAD ? MAX_AHEAD : ahead;
	if (!(slots = pt_calloc(ahead, sizeof(*slots)))) {
		ret = -ENOMEM;
		goto done;
	}
	for (int k = 0; k < ahead && clip.audio_bytes; k++) {
		if (!(slots[k].pcm = pt_malloc(clip.audio_bytes))) {
			ret = -ENOMEM;
			goto done;
		}
	}
	/* As fast as the policy lets it, all the time (cpufreq_boost()):
	 * nothing here is idle long enough for the governor to be right
	 * about it. Under powersave that is 80 MHz, and frames are dropped. */
	xSemaphoreTake(cleanup.guard, portMAX_DELAY);
	cpufreq_boost(true);
	boosted = true;
	xSemaphoreGive(cleanup.guard);

	vt_hold_screen(true);
	blit.vt = vt_screen_mine();
	xSemaphoreTake(cleanup.guard, portMAX_DELAY);
	power_keep_screen(true);		/* nobody presses keys to watch */
	kept = true;
	screen = true;
	xSemaphoreGive(cleanup.guard);
	canvas_clear(&page[0]);
	canvas_clear(&page[1]);
	repaint(&page[0]);
	pt_tty_raw(PT_STDIN, true);
	/* Left part of the way through last time: on from there, unless 0. */
	if (resume_get(name, &clip, &from, &from_off) && frame_at(rd.fd, from_off)) {
		bar_at("go on from", from / clip.fps, "?  enter: yes  0: from the start");
		switch ((key0 = pt_readkey_timeout(PT_STDIN, RESUME_ASK_MS))) {
		case 'q':
		case PT_KEY_ESC:
		case PT_CTRL('c'):
			*back = listed && key0 == PT_KEY_ESC;
			goto done;
		case '0':
		case PT_KEY_HOME:
			from = 0;
			break;
		default:			/* Enter, anything else, or nobody there */
			idx.off[from] = from_off;
			break;
		}
		repaint(&page[0]);
	} else {
		from = 0;
	}
	if ((ret = seek_to(&rd, &clip, &idx, from)) < 0)
		goto done;
	from = nread = ret;
	ret = 0;
	started = esp_timer_get_time() - (int64_t)from * frame_us;
	end = clip.frames;
	played = true;

	for (i = from; i < end && !pt_interrupted(); i++) {
		int64_t mark, now;
		struct canvas *into = &page[next];
		struct slot *f;
		const uint8_t *turned;
		bool late, hidden, thin;
		int key;

		/*
		 * Read ahead while there is a slot free and the sound has room
		 * for another frame's without waiting. The frame about to be
		 * shown is read whatever: its sound waiting for room is what
		 * paces a clip that nobody is watching.
		 */
		while (nread < end && nread - i < ahead &&
		       (nread == i || (!step && (!clip.audio_bytes ||
						 audio_queued_us() + frame_us <= buffer_us)))) {
			struct slot *r = &slots[nread % ahead];

			mark = esp_timer_get_time();
			idx.off[nread] = reader_tell(&rd);
			if ((ret = read_frame(&rd, &clip, r))) {
				if (ret == -ENOMEM)
					goto done;
				ret = 0;
				end = nread;		/* a short file: stop, not an error */
				break;
			}
			idx.off[nread + 1] = reader_tell(&rd);
			now = esp_timer_get_time();
			read_us += now - mark;
			if (step) {
				r->play_at = now;	/* shown at once, and heard never */
			} else if (clip.audio_bytes) {
				r->play_at = now + audio_queued_us();
				audio_write(r->pcm, clip.audio_bytes, clip.channels);
			} else {
				r->play_at = started + (int64_t)nread * frame_us;
			}
			nread++;
		}
		if (i >= end)
			break;
		f = &slots[i % ahead];
		hidden = !vt_screen_front();
		/* not ready before half its sound is gone: let it go */
		late = !step && esp_timer_get_time() + decode_guess > f->play_at + frame_us / 2;
		/*
		 * No more frames than the panel takes in step with its
		 * refresh (a whole screen at 30 a second is more), the ones
		 * left out spread evenly and not decoded at all.
		 */
		every_us = native_ok ? lcd_native_every() : 0;
		thin = !step && every_us > frame_us && i > from &&
		       (int64_t)i * frame_us / every_us == (int64_t)(i - 1) * frame_us / every_us;

		if (!hidden && gen != vt_screen_gen()) {
			gen = vt_screen_gen();
			blit_finish(&blit);
			repaint(shown);
		}
		if (hidden) {
			wait_until(f->play_at);	/* the sound goes on alone, in time */
		} else if (late) {
			dropped++;
		} else if (thin) {
			thinned++;
		} else {
			mark = esp_timer_get_time();
			slice0.px = into->px;
			/* Hand the other slices over before doing our own,
			 * so both cores are busy at the same time. */
			for (int s = 1; s < clip.slices; s++) {
				struct helper *h = &helpers[s - 1];

				h->c.px = into->px;
				h->data = f->jpeg + f->part[s];
				h->len = f->part[s + 1] - f->part[s];
				xSemaphoreGive(h->go);
			}
			ret = clip.slices > 1 ? canvas_jpeg_mem(&slice0, f->jpeg, f->part[1])
					      : canvas_jpeg_mem(into, f->jpeg, f->part[1]);
			for (int s = 1; s < clip.slices; s++) {
				xSemaphoreTake(helpers[s - 1].done, portMAX_DELAY);
				if (helpers[s - 1].ret)
					ret = helpers[s - 1].ret;
			}
			if (!ret && native[next])
				ret = canvas_turn(into, native[next]);
			turned = !ret ? native[next] : NULL;
			now = esp_timer_get_time();
			decode_us += now - mark;
			/* Guess high: a quick frame lowers it slowly, a slow
			 * one raises it at once. */
			decode_guess = now - mark > decode_guess ? now - mark
				     : decode_guess - (decode_guess - (now - mark)) / 8;

			/* up when its sound is heard */
			wait_until(f->play_at - SHOW_LEAD_US);
			/*
			 * The previous frame has had this whole decode to
			 * reach the panel, and it went from the other page,
			 * so waiting for it here costs nothing -- which is
			 * the point of there being two.
			 */
			mark = esp_timer_get_time();
			blit_finish(&blit);
			blit_us += esp_timer_get_time() - mark;
			if (!ret) {
				blit_start(&blit, into, turned);
				shown = into;
				next ^= 1;
				shown_n++;
			}
			ret = 0;
		}

		/* No keyboard -- a clip started from a script -- is not a
		 * reason to stop, only a reason to stop asking. */
		key = keys ? pt_readkey_timeout(PT_STDIN, 0) : PT_KEY_NONE;
		if (step) {		/* a jump made while paused: paused again there */
			step = false;
			/* unless a key came while it was finding the frame */
			if ((target = seek_key(key, i, &clip)) >= 0) {
				step = true;
				goto jump;
			}
			if (key != 'q' && key != PT_KEY_ESC && key != PT_CTRL('c'))
				key = ' ';
		}
		if (key == PT_KEY_EOF || key == PT_KEY_ERROR) {
			keys = false;
			continue;
		}
		if (key == 'q' || key == PT_KEY_ESC || key == PT_CTRL('c')) {
			*back = listed && key == PT_KEY_ESC;
			break;
		}
		if ((target = seek_key(key, i, &clip)) >= 0)
			goto jump;
		if (key == 's' || key == ' ') {
			int64_t paused = esp_timer_get_time();

			blit_finish(&blit);
			if (clip.audio_bytes)
				audio_discard();	/* the frames ahead go in again after */
			if (key == ' ') {		/* paused until the next key */
				power_keep_screen(false);	/* it may dim while paused */
				pause_bar(&clip, i);
				while ((key = pt_readkey_timeout(PT_STDIN, 200)) == 's' ||
				       key == PT_KEY_NONE) {
					if (key == 's') {
						save_shot(shown, shot, sizeof(shot));
					} else if (gen != vt_screen_gen()) {
						gen = vt_screen_gen();
						repaint(shown);
						pause_bar(&clip, i);
					}
				}
				power_keep_screen(true);
				repaint(shown);		/* the bar goes */
				if (key == 'q' || key == PT_KEY_ESC || key == PT_CTRL('c') ||
				    key == PT_KEY_EOF || key == PT_KEY_ERROR) {
					*back = listed && key == PT_KEY_ESC;
					break;
				}
				if ((target = seek_key(key, i, &clip)) >= 0) {
					step = true;	/* to show where it went */
					goto jump;
				}
			} else {
				save_shot(shown, shot, sizeof(shot));
			}
			/* Either way the sound starts again from the next frame. */
			requeue(slots, ahead, i + 1, nread, &clip, esp_timer_get_time() - paused);
			started += esp_timer_get_time() - paused;
		}
		continue;
jump:
		/* Everything read ahead is let go, and its sound with it. */
		blit_finish(&blit);
		if (clip.audio_bytes)
			audio_discard();
		if (!idx.off[target]) {		/* not been there: a hop at a time */
			bar_at("finding", target / clip.fps, "...");
			if ((ret = seek_to(&rd, &clip, &idx, target)) >= 0)
				repaint(shown);
		} else {
			ret = seek_to(&rd, &clip, &idx, target);
		}
		if (ret < 0)
			goto done;
		i = ret - 1;			/* and the loop's i++ makes it that frame */
		nread = ret;
		ret = 0;
		started = esp_timer_get_time() - (int64_t)nread * frame_us;
		/* A silent seek needs time to decode its first frame before the
		 * new timeline starts, or the retained estimate drops it at once. */
		if (!clip.audio_bytes)
			started += decode_guess + SHOW_LEAD_US;
	}
	ret = 0;
	if (i >= end)
		*back = listed;		/* to the end: the list again */
	/* Where it was left, for next time; played to the end, or all but, is done with. */
	if (played && i < end && i / clip.fps >= RESUME_EDGE_S &&
	    (clip.frames - i) / clip.fps >= RESUME_EDGE_S && idx.off[i])
		resume_put(name, &clip, i, idx.off[i]);
	else if (played)
		resume_put(name, &clip, -1, 0);
done:
	if (cleanup.guard)
		while (!video_cleanup_step(&cleanup))
			vTaskDelay(pdMS_TO_TICKS(10));
	proc_set_cleanup(NULL, NULL);
	pt_free(native_mem[0]);
	pt_free(native_mem[1]);
	if (screen)
		pt_tty_raw(PT_STDIN, false);
	if (clip.audio_bytes)
		audio_stop();
	canvas_close(&page[0]);
	canvas_close(&page[1]);
	for (int k = 0; slots && k < ahead; k++) {
		pt_free(slots[k].jpeg);
		pt_free(slots[k].pcm);
	}
	pt_free(slots);
	pt_free(idx.off);
	pt_free(rd.buf);
	pt_close(rd.fd);
	if (screen) {
		vt_hold_screen(false);
		vt_redraw();
	}
	if (ret)
		return fail("video", path, ret);
	if (index_only || !verbose)
		return 0;

	int seen = shown_n + dropped + thinned;

	pt_printf("%dx%d at %d fps: %d frames shown, %d dropped\n",
		  clip.w, clip.h, clip.fps, shown_n - blit.missed, dropped + blit.missed);
	if (thinned)
		pt_printf("  %d left out: the panel takes %d.%d a second in step\n", thinned,
			  10000000 / every_us / 10, 10000000 / every_us % 10);
	if (blit.missed)
		pt_printf("  %d panel transfers failed\n", blit.missed);
	if (blit.late)
		pt_printf("  %d frames late for the refresh: a seam possible\n", blit.late);
	if (shown_n)
		pt_printf("  panel %lld ms average, %lld ms maximum (including refresh wait)\n",
			  blit.transfer_us / 1000 / shown_n, blit.transfer_max_us / 1000);
	if (seen)
		pt_printf("  read %lld ms, decode %lld ms, draw %lld ms (each frame)\n",
			  read_us / 1000 / seen, shown_n ? decode_us / 1000 / shown_n : 0,
			  shown_n ? blit_us / 1000 / shown_n : 0);
	return 0;
}

PT_PROGRAM_STACK(video, 8, "play a clip\n"
		 "usage: video [-v] [clip.ptv]    video -i clip.ptv\n"
		 "With no file, what is in ~/video is offered as a list.\n"
		 "Space pauses, q stops, s saves the screen; Esc\n"
		 "goes back to the list, if it came from one.\n"
		 "Left/right go 10 s back or on, up/down a minute,\n"
		 "0-9 a tenth of the way in each. A clip goes on from\n"
		 "where it was left, unless 0 is pressed at the start.\n"
		 "-v  at the end, frames shown and dropped, and the\n"
		 "    time each frame took to read, decode and send\n"
		 "-i  write an index into an older clip, so that\n"
		 "    jumping in it is quick (new clips have one)\n"
		 "On the PC: make video FILE=something.mkv")
{
	static const char *const exts[] = { ".ptv", NULL };
	char chosen[PT_PATH_MAX] = "";
	bool index_only = false, verbose = false;
	const char *path = NULL;
	int arg, ret;

	for (arg = 1; arg < argc && argv[arg][0] == '-' && argv[arg][1]; arg++) {
		if (!strcmp(argv[arg], "-i"))
			index_only = true;
		else if (!strcmp(argv[arg], "-v"))
			verbose = true;
		else
			break;
	}
	if (arg < argc && argv[arg][0] != '-')
		path = argv[arg++];
	if (arg < argc || (index_only && (!path || verbose))) {
		pt_dprintf(PT_STDERR, "usage: video [-v] [clip.ptv]    video -i clip.ptv\n");
		return 2;
	}
	if (!index_only && !vt_has_display()) {
		pt_dprintf(PT_STDERR, "video: there is no screen\n");
		return 1;
	}
	if (path) {
		bool back;

		return play_clip(path, index_only, verbose, false, &back);
	}
	/* the list, a clip, and the list again with that clip picked */
	for (;;) {
		char dir[64];
		bool back;

		home_dir(dir, sizeof(dir), "video");
		ret = pick_file(dir, exts, "clips", chosen, sizeof(chosen));
		if (ret == -ECANCELED)
			return 0;
		if (ret)
			return fail("video", dir, ret);
		ret = play_clip(chosen, false, verbose, true, &back);
		if (!back || pt_interrupted())
			return ret;
	}
}
