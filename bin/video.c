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
 * sound is the clock: each frame's sound goes to the codec before its
 * picture is decoded, and audio_write() returns when the codec has room,
 * which paces everything else. A picture that cannot be ready before the
 * sound runs out is skipped, so the sound never stops for the picture.
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
	volatile bool	  quit, pending;
};

static void blit_task(void *arg)
{
	struct blitter *b = arg;

	for (;;) {
		xSemaphoreTake(b->go, portMAX_DELAY);
		if (b->quit)
			break;
		if (vt_screen_begin())
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

static void blit_start(struct blitter *b, struct canvas *c)
{
	blit_finish(b);
	b->c = c;
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

/* Tells a task started above to finish, and waits until it has. */
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

/* Reads frame's slices into `jpeg`, noting where each starts; `part[slices]` is the end. */
static int read_frame(struct reader *r, const struct clip *c, uint8_t *jpeg,
		      size_t part[MAX_SLICES + 1], uint8_t *pcm)
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
		if (reader_get(r, jpeg + at, n))
			return -EIO;
		part[s] = at;
		at += n;
	}
	part[c->slices] = at;
	if (c->audio_bytes && reader_get(r, pcm, c->audio_bytes))
		return -EIO;
	return 0;
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
	char chosen[PT_PATH_MAX], shot[PT_PATH_MAX];
	const char *path = argc > 1 ? argv[1] : chosen;
	uint8_t *jpeg = NULL, *pcm = NULL;
	int ret, was_rate = 0, was_min = 0, was_max = 0, next = 0;
	int shown_n = 0, dropped = 0, frame_us, buffer_us = 0;
	int64_t read_us = 0, decode_us = 0, blit_us = 0, started, queued = 0, queued_at = 0;
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
	jpeg = pt_malloc(MAX_FRAME);
	pcm = clip.audio_bytes ? pt_malloc(clip.audio_bytes) : NULL;
	/* Two pages: one being sent to the panel while the next is
	 * decoded into the other. */
	if ((ret = canvas_open(&page[0])) || (ret = canvas_open(&page[1])))
		goto done;
	if (!jpeg || (clip.audio_bytes && !pcm)) {
		ret = -ENOMEM;
		goto done;
	}

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
	if ((ret = task_start(blit_task, "vidblit", 3072, &blit, &blit.task, &blit.go, &blit.done)))
		goto done;

	if (clip.audio_bytes) {
		was_rate = audio_rate();
		if ((ret = audio_set_rate(clip.rate)))
			goto done;
		buffer_us = audio_buffer_us();
	}
	/* Nothing here is idle long enough for the governor to be right
	 * about it, and a frame missed while the clock ramps up is a
	 * frame missed. */
	cpufreq_get(&was_min, &was_max);
	cpufreq_set(240, 240);

	vt_hold_screen(true);
	screen = true;
	canvas_clear(&page[0]);
	canvas_clear(&page[1]);
	repaint(&page[0]);
	pt_tty_raw(PT_STDIN, true);
	frame_us = 1000000 / clip.fps;
	started = esp_timer_get_time();

	for (int i = 0; i < clip.frames; i++) {
		size_t part[MAX_SLICES + 1];
		int64_t mark = esp_timer_get_time(), now;
		struct canvas *into = &page[next];
		bool late, hidden = !vt_screen_front();
		int key;

		if (read_frame(&rd, &clip, jpeg, part, pcm))
			break;			/* a short file: stop, not an error */
		now = esp_timer_get_time();
		read_us += now - mark;

		if (clip.audio_bytes) {
			/*
			 * The sound first, then the picture. What is queued
			 * is worked out rather than asked for -- the I2S
			 * driver keeps it to itself -- from what went in and
			 * how long ago: a write that had to wait found the
			 * ring full. If the picture would take longer to
			 * make than the sound lasts, it is skipped, and the
			 * time goes to keeping the sound going.
			 */
			queued -= now - queued_at;
			if (queued < 0)
				queued = 0;
			audio_write(pcm, clip.audio_bytes, 1);
			queued_at = esp_timer_get_time();
			if (queued_at - now > 1000)
				queued = buffer_us - buffer_us / 12;	/* full, less half a buffer */
			else if ((queued += frame_us) > buffer_us)	/* a frame's sound is a frame long */
				queued = buffer_us;
			late = queued < decode_guess + read_us / (i + 1) + SLACK_US;
		} else {
			late = now > started + (int64_t)(i + 1) * frame_us;
		}

		if (!hidden && gen != vt_screen_gen()) {
			gen = vt_screen_gen();
			blit_finish(&blit);
			repaint(shown);
		}
		if (hidden) {
			/* another terminal is in front: the sound goes on alone */
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
				h->data = jpeg + part[s];
				h->len = part[s + 1] - part[s];
				xSemaphoreGive(h->go);
			}
			ret = clip.slices > 1 ? canvas_jpeg_mem(&slice0, jpeg, part[1])
					      : canvas_jpeg_mem(into, jpeg, part[1]);
			for (int s = 1; s < clip.slices; s++) {
				xSemaphoreTake(helpers[s - 1].done, portMAX_DELAY);
				if (helpers[s - 1].ret)
					ret = helpers[s - 1].ret;
			}
			now = esp_timer_get_time();
			decode_us += now - mark;
			/* Guess high: a quick frame lowers it slowly, a slow
			 * one raises it at once. */
			decode_guess = now - mark > decode_guess ? now - mark
				     : decode_guess - (decode_guess - (now - mark)) / 8;

			/* A silent clip keeps time by the clock instead. */
			if (!clip.audio_bytes)
				while (esp_timer_get_time() < started + (int64_t)i * frame_us)
					pt_sleep_ms(1);
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
				blit_start(&blit, into);
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
			if (key == ' ') {		/* paused until the next key */
				audio_stop();
				while ((key = pt_readkey_timeout(PT_STDIN, 200)) == 's' ||
				       key == PT_KEY_NONE) {
					if (key == 's') {
						save_shot(shown, shot, sizeof(shot));
					} else if (gen != vt_screen_gen()) {
						gen = vt_screen_gen();
						repaint(shown);
					}
				}
				if (key == 'q' || key == PT_KEY_ESC || key == PT_CTRL('c') ||
				    key == PT_KEY_EOF || key == PT_KEY_ERROR)
					break;
			} else {
				save_shot(shown, shot, sizeof(shot));
			}
			/* Either way the sound ran dry and starts again. */
			started += esp_timer_get_time() - paused;
			queued = 0;
		}
	}
	ret = 0;
done:
	task_stop(&blit.task, &blit.go, &blit.done, &blit.quit);
	for (int i = 0; i < MAX_SLICES - 1; i++)
		task_stop(&helpers[i].task, &helpers[i].go, &helpers[i].done, &helpers[i].quit);
	if (screen)
		pt_tty_raw(PT_STDIN, false);
	if (was_max)
		cpufreq_set(was_min, was_max);
	if (clip.audio_bytes) {
		audio_stop();
		if (was_rate > 0)
			audio_set_rate(was_rate);
	}
	canvas_close(&page[0]);
	canvas_close(&page[1]);
	pt_free(jpeg);
	pt_free(pcm);
	pt_free(rd.buf);
	pt_close(rd.fd);
	if (screen) {
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
