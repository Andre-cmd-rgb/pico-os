/*
 * video - play a clip.
 *
 * There is no H.264 decoder on a 240 MHz chip and there is not going to
 * be one, so a clip is converted on a PC into what this machine can
 * already decode: one JPEG per frame for the ROM's decompressor, and raw
 * 16-bit mono PCM for the codec. tools/mkvideo.py does the converting.
 *
 * The file is:
 *
 *	 0  "PTV1"
 *	 4  u16 width, u16 height, u16 frames a second, u16 flags (1: sound)
 *	12  u32 frames, u32 sample rate, u32 sound bytes per frame
 *	24  eight bytes kept back
 *	32  each frame: u32 length, that many bytes of JPEG, then the
 *	    frame's sound
 *
 * Picture and sound are interleaved so that playing is one pass through
 * the file with no seeking, which is what an SD card is good at. The
 * sound is the clock: audio_write() returns when the codec has taken the
 * samples, which paces everything else, and a frame that would arrive
 * late is read and thrown away rather than held on to.
 */
#include <string.h>

#include "esp_timer.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "canvas.h"
#include "util.h"

#if CONFIG_PT_LCD

#define VIDEO		"/home/" CONFIG_PT_USERNAME "/video"
#define PHOTOS		"/home/" CONFIG_PT_USERNAME "/photos"
#define HEADER		32
#define MAX_FRAME	(256 * 1024)	/* a sane limit on one JPEG */

struct clip {
	int	w, h, fps, frames;
	int	rate, audio_bytes;	/* per frame; 0 when silent */
};

static uint16_t le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_full(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;

	while (n) {
		int got = pt_read(fd, p, n);

		if (got <= 0)
			return -EIO;
		p += got;
		n -= got;
	}
	return 0;
}

static int read_header(int fd, struct clip *c)
{
	uint8_t h[HEADER];

	if (read_full(fd, h, sizeof(h)))
		return -EIO;
	if (memcmp(h, "PTV1", 4))
		return -ENOTSUP;
	c->w = le16(h + 4);
	c->h = le16(h + 6);
	c->fps = le16(h + 8);
	c->frames = (int)le32(h + 12);
	c->rate = (int)le32(h + 16);
	c->audio_bytes = le16(h + 10) & 1 ? (int)le32(h + 20) : 0;
	if (c->w <= 0 || c->h <= 0 || c->fps <= 0 || c->frames < 0 ||
	    c->audio_bytes < 0 || c->audio_bytes > 1 << 16)
		return -EINVAL;
	return 0;
}

PT_PROGRAM_STACK(video, 8, "play a clip\n"
		 "usage: video [clip.ptv]\n"
		 "With no file, what is in ~/video is offered as a list.\n"
		 "Space pauses, q or Esc stops, s saves a copy of the screen.\n"
		 "Make a clip on the PC: make video FILE=something.mp4")
{
	static const char *const exts[] = { ".ptv", NULL };
	struct canvas canvas = { 0 };
	struct clip clip;
	char chosen[PT_PATH_MAX], shot[PT_PATH_MAX];
	const char *path = argc > 1 ? argv[1] : chosen;
	uint8_t *jpeg = NULL, *pcm = NULL;
	int fd = -1, ret, was_rate = 0, frame_us;
	int shown = 0, dropped = 0;
	int64_t read_us = 0, decode_us = 0, blit_us = 0;
	bool keys = true;
	int64_t started;

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

	if ((fd = pt_open(path, O_RDONLY)) < 0)
		return fail("video", path, fd);
	if ((ret = read_header(fd, &clip))) {
		pt_close(fd);
		return fail("video", path, ret);
	}
	if ((ret = canvas_open(&canvas))) {
		pt_close(fd);
		return fail("video", NULL, ret);
	}
	jpeg = pt_malloc(MAX_FRAME);
	pcm = clip.audio_bytes ? pt_malloc(clip.audio_bytes) : NULL;
	if (!jpeg || (clip.audio_bytes && !pcm)) {
		ret = -ENOMEM;
		goto done;
	}
	if (clip.audio_bytes) {
		was_rate = audio_rate();
		audio_set_rate(clip.rate);
	}

	canvas_clear(&canvas);
	canvas_blit(&canvas);
	vt_hold_screen(true);
	pt_tty_raw(PT_STDIN, true);
	frame_us = 1000000 / clip.fps;
	started = esp_timer_get_time();

	for (int i = 0; i < clip.frames; i++) {
		uint8_t len[4];
		uint32_t n;
		int64_t mark = esp_timer_get_time();
		int key;

		if (read_full(fd, len, 4))
			break;			/* short file: stop, not an error */
		n = le32(len);
		if (n > MAX_FRAME || read_full(fd, jpeg, n))
			break;
		if (clip.audio_bytes && read_full(fd, pcm, clip.audio_bytes))
			break;
		read_us += esp_timer_get_time() - mark;

		/*
		 * Where this frame is due against where we are. A frame
		 * that would be shown more than a frame late is skipped --
		 * the sound still goes out, so the clip does not drift --
		 * but the clock is moved with it, because a machine that
		 * cannot keep up should show every other frame rather than
		 * fall permanently behind and show none.
		 */
		int64_t due = started + (int64_t)i * frame_us;

		if (esp_timer_get_time() > due + frame_us) {
			dropped++;
			started = esp_timer_get_time() - (int64_t)i * frame_us;
		} else {
			mark = esp_timer_get_time();
			ret = canvas_jpeg_mem(&canvas, jpeg, n);
			decode_us += esp_timer_get_time() - mark;
			if (!ret) {
				mark = esp_timer_get_time();
				canvas_blit_fit(&canvas);
				blit_us += esp_timer_get_time() - mark;
				shown++;
			}
		}
		if (clip.audio_bytes)
			audio_write(pcm, clip.audio_bytes, 1);
		else
			while (esp_timer_get_time() < due)
				pt_sleep_ms(1);

		/* No keyboard -- a clip started from a script -- is not a
		 * reason to stop, only a reason to stop asking. */
		key = keys ? pt_readkey_timeout(PT_STDIN, 0) : PT_KEY_NONE;
		if (key == PT_KEY_EOF || key == PT_KEY_ERROR) {
			keys = false;
			continue;
		}
		if (key == 'q' || key == PT_KEY_ESC || key == PT_CTRL('c'))
			break;
		if (key == 's') {
			canvas_save(&canvas, sd_mounted() ? PHOTOS : "/tmp", shot, sizeof(shot));
			started = esp_timer_get_time() - (int64_t)i * 1000000 / clip.fps;
		}
		if (key == ' ') {			/* paused until the next key */
			int64_t at = esp_timer_get_time();

			audio_stop();
			while ((key = pt_readkey(PT_STDIN)) == 's')
				canvas_save(&canvas, sd_mounted() ? PHOTOS : "/tmp",
					    shot, sizeof(shot));
			if (key == 'q' || key == PT_KEY_ESC || key == PT_KEY_EOF)
				break;
			started += esp_timer_get_time() - at;
		}
	}
	ret = 0;
done:
	pt_tty_raw(PT_STDIN, false);
	if (clip.audio_bytes) {
		audio_stop();
		if (was_rate > 0)
			audio_set_rate(was_rate);
	}
	canvas_close(&canvas);
	pt_free(jpeg);
	pt_free(pcm);
	pt_close(fd);
	vt_hold_screen(false);
	vt_redraw();
	if (ret)
		return fail("video", path, ret);
	int done = shown + dropped;

	pt_printf("%dx%d at %d fps: %d frames shown, %d dropped\n",
		  clip.w, clip.h, clip.fps, shown, dropped);
	if (done)
		pt_printf("  read %lld ms, decode %lld ms, draw %lld ms (each frame)\n",
			  read_us / 1000 / done, shown ? decode_us / 1000 / shown : 0,
			  shown ? blit_us / 1000 / shown : 0);
	return 0;
}

#else

PT_PROGRAM(video, "play a clip (no screen on this board)")
{
	pt_dprintf(PT_STDERR, "video: this board has no screen\n");
	return 1;
}

#endif
