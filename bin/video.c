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
 *	 4  u16 width, u16 height, u16 frames a second, u16 flags (1: sound)
 *	12  u32 frames, u32 sample rate, u32 sound bytes per frame
 *	24  u16 slices, then six bytes kept back
 *	32  each frame: for each slice u32 length and that many bytes of
 *	    JPEG, then the frame's sound
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

#if CONFIG_PT_LCD

#define VIDEO		"/home/" CONFIG_PT_USERNAME "/video"
#define PHOTOS		"/home/" CONFIG_PT_USERNAME "/photos"
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
	int	w, h, fps, frames;
	int	rate, audio_bytes;	/* per frame; 0 when silent */
	int	slices;
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

/*
 * The other core, decoding the slices this one is not. It waits on `go`,
 * decodes what it is pointed at, and says so on `done`.
 */
struct helper {
	TaskHandle_t	  task;
	SemaphoreHandle_t go, done;
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
		if (h->quit)
			break;
		h->ret = canvas_jpeg_mem(&h->c, h->data, h->len);
		xSemaphoreGive(h->done);
	}
	xSemaphoreGive(h->done);
	vTaskDelete(NULL);
}

/*
 * The panel, filled from one page while the next frame is decoded into
 * the other. Sending a frame is 15 ms of DMA, and there is no reason for
 * the decoder to sit through it.
 */
struct blitter {
	TaskHandle_t	  task;
	SemaphoreHandle_t go, done;
	struct canvas	 *c;
	const uint8_t	 *native;	/* its turned copy, when the panel can be read */
	volatile bool	  quit, pending;
};

static void blit_task(void *arg)
{
	struct blitter *b = arg;

	for (;;) {
		xSemaphoreTake(b->go, portMAX_DELAY);
		if (b->quit)
			break;
		/* in step with the panel's refresh if it can be, or else as it is */
		if (vt_screen_begin() && (!b->native || canvas_send_native(b->c, b->native)))
			canvas_blit_fit(b->c);
		vt_screen_end();
		xSemaphoreGive(b->done);
	}
	xSemaphoreGive(b->done);
	vTaskDelete(NULL);
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
		      TaskHandle_t *task, SemaphoreHandle_t *go, SemaphoreHandle_t *done)
{
	*go = xSemaphoreCreateBinary();
	*done = xSemaphoreCreateBinary();
	if (!*go || !*done ||
	    xTaskCreatePinnedToCore(fn, name, stack, arg, 4, task, HELPER_CORE) != pdPASS) {
		*task = NULL;
		return -ENOMEM;
	}
	return 0;
}

/*
 * Tells a task started above to finish, and waits until it has. Nothing
 * may be pending on it: the `done` it gives after work would be taken
 * for the one it gives on quitting, and its semaphores deleted under it
 * (a clip stopped with Esc mid-frame crashed the board that way).
 */
static void task_stop(TaskHandle_t *task, SemaphoreHandle_t *go, SemaphoreHandle_t *done,
		      volatile bool *quit)
{
	if (*task) {
		*quit = true;
		xSemaphoreGive(*go);
		xSemaphoreTake(*done, portMAX_DELAY);
		*task = NULL;
	}
	if (*go)
		vSemaphoreDelete(*go);
	if (*done)
		vSemaphoreDelete(*done);
	*go = *done = NULL;
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
	c->slices = memcmp(h, "PTV1", 4) ? le16(h + 24) : 1;
	if (c->w <= 0 || c->h <= 0 || c->fps <= 0 || c->fps > 120 || c->frames < 0 ||
	    c->audio_bytes < 0 || c->audio_bytes > 1 << 16 || c->audio_bytes & 1)
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
		c->rate = c->audio_bytes / 2 * c->fps;
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
			audio_write(f->pcm, c->audio_bytes, 1);
		} else {
			f->play_at += away;
		}
	}
}

static void save_shot(struct canvas *c, char *shot, size_t size)
{
	if (vt_screen_begin())
		canvas_save(c, sd_mounted() ? PHOTOS : "/tmp", shot, size);
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
	if (vt_screen_begin())
		vt_bar_line(lcd_height() - vt_line_height(), line);
	vt_screen_end();
}

PT_COMPLETE(video, ": <file:.ptv>\n")

PT_PROGRAM_STACK(video, 8, "play a clip\n"
		 "usage: video [clip.ptv]\n"
		 "With no file, what is in ~/video is offered as a list.\n"
		 "Space pauses, q or Esc stops, s saves a copy of the screen.\n"
		 "Make a clip on the PC: make video FILE=something.mp4")
{
	static const char *const exts[] = { ".ptv", NULL };
	struct canvas page[2] = { { 0 }, { 0 } }, slice0 = { 0 }, *shown = &page[0];
	struct helper helpers[MAX_SLICES - 1] = { 0 };
	struct blitter blit = { 0 };
	struct reader rd = { .fd = -1 };
	struct clip clip = { 0 };
	struct slot *slots = NULL;
	char chosen[PT_PATH_MAX], shot[PT_PATH_MAX];
	const char *path = argc > 1 ? argv[1] : chosen;
	void *native_mem[2] = { NULL, NULL };
	uint8_t *native[2] = { NULL, NULL };
	int ret, was_min = 0, was_max = 0, next = 0, ahead = 0, nread = 0, end;
	int shown_n = 0, dropped = 0, frame_us, buffer_us = 0;
	int64_t read_us = 0, decode_us = 0, blit_us = 0, started;
	int64_t decode_guess = 0;
	bool keys = true, screen = false;
	unsigned gen = vt_screen_gen();

	if (argc > 2) {
		pt_dprintf(PT_STDERR, "usage: video [clip.ptv]\n");
		return 2;
	}
	if (!vt_has_display()) {
		pt_dprintf(PT_STDERR, "video: there is no screen\n");
		return 1;
	}
	if (argc == 1) {
		ret = pick_file(VIDEO, exts, "clips", chosen, sizeof(chosen));
		if (ret == -ECANCELED)
			return 0;
		if (ret)
			return fail("video", VIDEO, ret);
	}

	if ((rd.fd = pt_open(path, O_RDONLY)) < 0)
		return fail("video", path, rd.fd);
	if (!(rd.buf = pt_malloc(READ_SIZE))) {
		ret = -ENOMEM;
		goto done;
	}
	if ((ret = read_header(&rd, &clip)))
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
			if ((ret = task_start(helper_task, "vidslice", 4096, h,
					      &h->task, &h->go, &h->done)))
				goto done;
		}
	}
	/*
	 * Frames go to the panel turned into its own order, in step with its
	 * refresh, so that nothing that moves tears (canvas_blit_native). The
	 * turning is done here, on this core, while the frame waits for its
	 * sound; the blitter on the other core only sends. A turned copy for
	 * each page, in PSRAM: one is sent while the next is made.
	 */
	for (int k = 0; k < 2 && lcd_native_ok(); k++) {
		native_mem[k] = pt_malloc((size_t)page[0].w * page[0].h * 2 + 63);
		native[k] = native_mem[k] ?
			(uint8_t *)(((uintptr_t)native_mem[k] + 63) & ~(uintptr_t)63) : NULL;
	}
	if (!native[0] || !native[1])
		native[0] = native[1] = NULL;	/* then as it is, tearing or not */
	if ((ret = task_start(blit_task, "vidblit", 3072, &blit, &blit.task, &blit.go, &blit.done)))
		goto done;

	if (clip.audio_bytes) {
		/* a few frames queued at most, or the sound lags the picture */
		if ((ret = audio_set_rate(clip.rate)) || (ret = audio_set_latency(CLIP_LATENCY_MS)))
			goto done;
		buffer_us = audio_buffer_us();
	}
	/* Enough frames ahead to cover the sound queued, and a few more. */
	frame_us = 1000000 / clip.fps;
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
	/* Nothing here is idle long enough for the governor to be right
	 * about it, and a frame missed while the clock ramps up is a
	 * frame missed. */
	cpufreq_get(&was_min, &was_max);
	cpufreq_set(240, 240);

	vt_hold_screen(true);
	power_keep_screen(true);		/* nobody presses keys to watch */
	screen = true;
	canvas_clear(&page[0]);
	canvas_clear(&page[1]);
	repaint(&page[0]);
	pt_tty_raw(PT_STDIN, true);
	/*
	 * The helpers on the other core work in this program's memory: a
	 * kill from another terminal must end the loop and stop them, not
	 * end the program under them.
	 */
	pt_sigcatch(true);
	started = esp_timer_get_time();
	end = clip.frames;

	for (int i = 0; i < end && !pt_interrupted(); i++) {
		int64_t mark, now;
		struct canvas *into = &page[next];
		struct slot *f;
		const uint8_t *turned;
		bool late, hidden;
		int key;

		/*
		 * Read ahead while there is a slot free and the sound has room
		 * for another frame's without waiting. The frame about to be
		 * shown is read whatever: its sound waiting for room is what
		 * paces a clip that nobody is watching.
		 */
		while (nread < end && nread - i < ahead &&
		       (nread == i || !clip.audio_bytes || audio_queued_us() + frame_us <= buffer_us)) {
			struct slot *r = &slots[nread % ahead];

			mark = esp_timer_get_time();
			if ((ret = read_frame(&rd, &clip, r))) {
				if (ret == -ENOMEM)
					goto done;
				ret = 0;
				end = nread;		/* a short file: stop, not an error */
				break;
			}
			now = esp_timer_get_time();
			read_us += now - mark;
			if (clip.audio_bytes) {
				r->play_at = now + audio_queued_us();
				audio_write(r->pcm, clip.audio_bytes, 1);
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
		late = esp_timer_get_time() + decode_guess > f->play_at + frame_us / 2;

		if (!hidden && gen != vt_screen_gen()) {
			gen = vt_screen_gen();
			blit_finish(&blit);
			repaint(shown);
		}
		if (hidden) {
			wait_until(f->play_at);	/* the sound goes on alone, in time */
		} else if (late) {
			dropped++;
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
			turned = !ret && native[next] && !canvas_turn(into, native[next]) ?
				 native[next] : NULL;
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
		if (key == PT_KEY_EOF || key == PT_KEY_ERROR) {
			keys = false;
			continue;
		}
		if (key == 'q' || key == PT_KEY_ESC || key == PT_CTRL('c'))
			break;
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
				    key == PT_KEY_EOF || key == PT_KEY_ERROR)
					break;
			} else {
				save_shot(shown, shot, sizeof(shot));
			}
			/* Either way the sound starts again from the next frame. */
			requeue(slots, ahead, i + 1, nread, &clip, esp_timer_get_time() - paused);
			started += esp_timer_get_time() - paused;
		}
	}
	ret = 0;
done:
	blit_finish(&blit);		/* the frame on its way to the panel first */
	task_stop(&blit.task, &blit.go, &blit.done, &blit.quit);
	pt_free(native_mem[0]);
	pt_free(native_mem[1]);
	for (int i = 0; i < MAX_SLICES - 1; i++)
		task_stop(&helpers[i].task, &helpers[i].go, &helpers[i].done, &helpers[i].quit);
	if (screen)
		pt_tty_raw(PT_STDIN, false);
	if (was_max)
		cpufreq_set(was_min, was_max);
	if (clip.audio_bytes)
		audio_stop();
	canvas_close(&page[0]);
	canvas_close(&page[1]);
	for (int k = 0; slots && k < ahead; k++) {
		pt_free(slots[k].jpeg);
		pt_free(slots[k].pcm);
	}
	pt_free(slots);
	pt_free(rd.buf);
	pt_close(rd.fd);
	if (screen) {
		power_keep_screen(false);
		vt_hold_screen(false);
		vt_redraw();
	}
	if (ret)
		return fail("video", path, ret);

	int seen = shown_n + dropped;

	pt_printf("%dx%d at %d fps: %d frames shown, %d dropped\n",
		  clip.w, clip.h, clip.fps, shown_n, dropped);
	if (seen)
		pt_printf("  read %lld ms, decode %lld ms, draw %lld ms (each frame)\n",
			  read_us / 1000 / seen, shown_n ? decode_us / 1000 / shown_n : 0,
			  shown_n ? blit_us / 1000 / shown_n : 0);
	return 0;
}

#else

PT_PROGRAM(video, "play a clip (no screen on this board)")
{
	pt_dprintf(PT_STDERR, "video: this board has no screen\n");
	return 1;
}

#endif
