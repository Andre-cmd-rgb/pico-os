/*
 * The NES on this hardware.
 *
 * Nofrendo (third_party/nofrendo) does the emulating; everything here is
 * the part that makes it a PocketType program: the two hooks the core
 * asks its host for, a palette turned into what the panel wants, the
 * 256x240 picture placed on a 320x240 screen, sound handed to the codec,
 * and the keyboard pretending to be a joypad.
 *
 * Sound is what paces the whole thing: audio_write() blocks while the
 * game's few frames of queued sound are full, and they empty at exactly
 * the rate the NES produces them. Music playing on another terminal is
 * mixed in with it.
 *
 * Switched to another terminal, the game pauses, silent, and carries on
 * where it was when its terminal comes back.
 */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "nes/nes.h"		/* first: it defines the core's own types */
#include "nes/apu.h"
#include "nes/input.h"
#include "nofrendo.h"

#include "drivers/drivers.h"
#include "emu.h"
#include "pt/keys.h"
#include "pt/kernel.h"

#define SAMPLE_RATE	16000
#define SOUND_LATENCY_MS 50	/* three frames */
#define FRAME_US	16639		/* 60.1 frames a second, as on the NES */
#define HOLD_MS		150		/* how long a key counts as held down */
#define ROWS_PER_DRAW	24		/* 256 x 24 x 2 bytes is 12 KB */
#define PX_ALIGN	64		/* a data cache line, for DMA from PSRAM */

static uint16_t	 palette[256];		/* in the order the panel reads them */
static uint8_t	*rowbuf, *rowmem;
static uint8_t	*vidbuf;	/* what the picture unit draws into */
static apu_t	*apu;		/* the core's own mixed output */
static int	 origin_x, origin_y;
static struct nes_stats stats;
static bool	 want_shot;
/*
 * The game running, if any. The core and all of the above are the one
 * copy there is -- programs share an address space -- so a second game
 * on another terminal would run on the first one's memory.
 */
static atomic_int owner;

/* ------------------------------------------------------------ host hooks */

void rg_system_log(int level, const char *context, const char *format, ...)
{
	char line[160];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(line))
		n = sizeof(line) - 1;
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		line[--n] = '\0';
	if (n > 0)
		klog("nes: %s", line);
}

uint32_t rg_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= buf[i];
		for (int b = 0; b < 8; b++)
			crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
	}
	return ~crc;
}

/* ------------------------------------------------------------ the screen */

/*
 * The core hands over one byte per pixel, an index into the palette, in a
 * buffer eight pixels wider than the screen on each side. This turns a
 * band of rows into what the panel expects and sends it.
 */
static void blit(uint8_t *vidbuf)
{
	uint16_t *out = (uint16_t *)rowbuf;
	int lit = 0;

	if (!vt_screen_begin()) {
		vt_screen_end();
		return;			/* another terminal is on the panel */
	}
	stats.blits++;

	for (int y = 0; y < NES_SCREEN_HEIGHT; y += ROWS_PER_DRAW) {
		int rows = NES_SCREEN_HEIGHT - y;

		if (rows > ROWS_PER_DRAW)
			rows = ROWS_PER_DRAW;
		for (int r = 0; r < rows; r++) {
			const uint8_t *in = NES_SCREEN_GETPTR(vidbuf, 0, y + r);
			uint16_t *line = out + r * NES_SCREEN_WIDTH;

			for (int x = 0; x < NES_SCREEN_WIDTH; x++) {
				line[x] = palette[in[x]];
				lit += in[x] != 0;
			}
		}
		lcd_draw(origin_x, origin_y + y, NES_SCREEN_WIDTH, rows, rowbuf);
	}
	vt_screen_end();
	stats.lit_pixels = lit;
}

/* The black around the picture, when the terminal has been painted there. */
static void clear_screen(void)
{
	if (vt_screen_begin())
		lcd_fill(0, 0, lcd_width(), lcd_height(), 0x0000);
	vt_screen_end();
}

/* The core's palette is RGB565 the right way round for a PC; the panel
 * wants each pixel's high byte first. */
static int build_palette(void)
{
	uint16_t *src = nofrendo_buildpalette(0, 16);

	if (!src)
		return -ENOMEM;
	for (int i = 0; i < 256; i++) {
		uint16_t c = src[i];

		palette[i] = (uint16_t)(c << 8 | c >> 8);
	}
	pt_free(src);			/* the core built it for us to keep */
	return 0;
}

/* ------------------------------------------------------------ the joypad */

/*
 * A terminal says when a key goes down and never when it comes up, so a
 * press counts as held for a moment and then lets go by itself. It is
 * enough for menus and slow games; a real pad over Bluetooth is the
 * answer for anything that needs a held direction.
 */
struct pad {
	int64_t until[8];	/* one deadline per button, in microseconds */
};

static const int button_bit[8] = {
	NES_PAD_UP, NES_PAD_DOWN, NES_PAD_LEFT, NES_PAD_RIGHT,
	NES_PAD_A, NES_PAD_B, NES_PAD_START, NES_PAD_SELECT,
};

static int button_of_key(int key)
{
	switch (key) {
	case PT_KEY_UP:					return 0;
	case PT_KEY_DOWN:				return 1;
	case PT_KEY_LEFT:				return 2;
	case PT_KEY_RIGHT:				return 3;
	case 'x': case 'X': case 'l': case 'L':		return 4;	/* A */
	case 'z': case 'Z': case 'k': case 'K':		return 5;	/* B */
	case '\r': case '\n':				return 6;	/* start */
	case ' ': case '\t':				return 7;	/* select */
	/* the four keys a CardKB has in a cluster, for the D-pad */
	case 'w': case 'W':				return 0;
	case 's': case 'S':				return 1;
	case 'a': case 'A':				return 2;
	case 'd': case 'D':				return 3;
	}
	return -1;
}

/* Returns false when the player asked to stop. */
static bool read_pad(struct pad *pad, int64_t now, int *state)
{
	int key;

	while ((key = pt_readkey_timeout(PT_STDIN, 0)) != PT_KEY_NONE) {
		int button;

		if (key == 'q' || key == 'Q' || key == PT_KEY_ESC || key == PT_CTRL('c') || key < 0)
			return false;
		if (key == 'p' || key == 'P') {
			want_shot = true;	/* saved after the next frame */
			continue;
		}
		button = button_of_key(key);
		if (button >= 0)
			pad->until[button] = now + HOLD_MS * 1000;
	}
	*state = 0;
	for (int i = 0; i < 8; i++)
		if (pad->until[i] > now)
			*state |= button_bit[i];
	return !pt_interrupted();
}

/* ------------------------------------------------------------ running */

struct nes_cleanup {
	StaticSemaphore_t guard_storage;
	SemaphoreHandle_t guard;
	struct pt_file *tty;
	int pid;
	bool core, capture, raw, done;
};

static bool nes_cleanup_step(void *arg)
{
	struct nes_cleanup *c = arg;
	int pid = c->pid;

	/* Ownership can pass to another game before this hook is removed. */
	if (c->done)
		return true;
	/* A syscall can exit the owner while it holds this guard. That
	 * operation will never resume, so cleanup retires its lock itself. */
	if (xSemaphoreGetMutexHolder(c->guard) != xTaskGetCurrentTaskHandle() &&
	    !xSemaphoreTake(c->guard, 0))
		return false;
	if (c->capture) {
		if (lcd_capture_try_end() == -EAGAIN) {
			xSemaphoreGive(c->guard);
			return false;
		}
		c->capture = false;
	}
	if (c->core) {
		/* The core's static ROM pointers must be cleared before exit
		 * releases its tracked allocations, including from the reaper. */
		nes_shutdown();
		c->core = false;
	}
	pt_free(rowmem);
	pt_free(vidbuf);
	rowbuf = rowmem = vidbuf = NULL;
	apu = NULL;
	want_shot = false;
	if (c->raw && c->tty && c->tty->ops->ioctl) {
		int raw = 0, off = -1;

		if (c->tty->ops->ioctl(c->tty, PT_TTY_TRYSETRAW, &raw) == -EAGAIN) {
			xSemaphoreGive(c->guard);
			return false;
		}
		/* readkey's reset syscall may have delivered the terminating kill. */
		c->tty->ops->ioctl(c->tty, PT_TTY_SETTIMEOUT, &off);
	}
	c->raw = false;
	c->done = true;
	atomic_compare_exchange_strong(&owner, &pid, 0);
	xSemaphoreGive(c->guard);
	return true;
}

void nes_last_stats(struct nes_stats *out)
{
	*out = stats;
}

int nes_run(const char *rom_path, const struct nes_options *opt)
{
	char vfs[PT_PATH_MAX + 16];
	struct pad pad = { 0 };
	struct nes_cleanup cleanup = { 0 };
	struct proc *p = proc_current();
	int64_t started, next_frame, paused_us = 0;
	unsigned gen;
	int frame = 0, ret = 0, frame_us = FRAME_US;
	bool screen = false;

	if (!p)
		return -EPERM;
	if (!vt_has_display())
		return -ENODEV;
	if (!mount_resolve(rom_path, vfs, sizeof(vfs)))
		return -ENOENT;

	int me = pt_getpid(), was = atomic_load(&owner);

	/* one killed outright never gave it back: a pid no longer alive */
	if ((was && proc_alive(was)) || !atomic_compare_exchange_strong(&owner, &was, me))
		return -EBUSY;
	cleanup.guard = xSemaphoreCreateMutexStatic(&cleanup.guard_storage);
	cleanup.tty = p->fd[PT_STDIN];
	cleanup.pid = me;
	pt_sigcatch(true);
	if ((ret = proc_set_cleanup(nes_cleanup_step, &cleanup))) {
		atomic_store(&owner, 0);
		return ret;
	}

	/* Publish startup state under a mutex so force-kill waits for the
	 * core to finish an allocation or ROM-load update before retiring it. */
	xSemaphoreTake(cleanup.guard, portMAX_DELAY);

	/*
	 * Both buffers are in PSRAM. The row buffer goes to the display by
	 * DMA, which reads PSRAM directly if the buffer starts on a cache
	 * line (the panel's driver writes the cache back first); in internal
	 * RAM it took 12 KB that music playing on another terminal leaves no
	 * room for. Both are counted as the program's, so a kill -9 from
	 * another terminal frees them.
	 */
	rowmem = pt_malloc_caps(NES_SCREEN_WIDTH * ROWS_PER_DRAW * 2 + PX_ALIGN - 1,
				MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	rowbuf = rowmem ? (uint8_t *)(((uintptr_t)rowmem + PX_ALIGN - 1) & ~(uintptr_t)(PX_ALIGN - 1))
			: NULL;
	vidbuf = pt_malloc_caps(NES_SCREEN_PITCH * NES_SCREEN_HEIGHT,
				MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!rowbuf || !vidbuf) {
		ret = -ENOMEM;
		goto startup_done;
	}
	cleanup.core = true;
	if (!nes_init(SYS_DETECT, SAMPLE_RATE, false, NULL) ||
	    !nes_getptr()->apu || !nes_getptr()->apu->buffer) {
		ret = -ENOMEM;
		goto startup_done;
	}
	if ((ret = build_palette()))
		goto startup_done;
	if (nes_loadfile(vfs) != 0) {
		klog("nes: %s is not a ROM this can run", rom_path);
		ret = -ENOEXEC;
		goto startup_done;
	}
	/*
	 * After loading, not before: inserting a cartridge resets the
	 * machine, and the reset clears the video buffer pointer. Set it
	 * first and the core quietly skips every frame.
	 */
	nes_getptr()->blit_func = blit;
	nes_setvidbuf(vidbuf);
	apu = nes_getptr()->apu;
	input_connect(0, NES_JOYPAD);
	if (nes_getptr()->refresh_rate == 50)
		frame_us = 20000;		/* PAL stays at 50 Hz without an audio clock */
	cleanup.raw = true;
startup_done:
	xSemaphoreGive(cleanup.guard);
	if (ret)
		goto out;
	pt_tty_raw(PT_STDIN, true);

	origin_x = (lcd_width() - NES_SCREEN_WIDTH) / 2;
	origin_y = (lcd_height() - NES_SCREEN_HEIGHT) / 2;
	if (origin_x < 0)
		origin_x = 0;
	if (origin_y < 0)
		origin_y = 0;
	vt_hold_screen(true);		/* the terminal stops repainting */
	screen = true;
	clear_screen();
	gen = vt_screen_gen();
	if (opt->sound) {
		if ((ret = audio_set_rate(SAMPLE_RATE)) ||
		    (ret = audio_set_latency(SOUND_LATENCY_MS)))
			goto out;		/* a failed codec cannot pace the game */
	}

	memset(&stats, 0, sizeof(stats));
	started = next_frame = esp_timer_get_time();
	for (;;) {
		int64_t now = esp_timer_get_time();
		bool draw = !opt->frameskip || frame % (opt->frameskip + 1) == 0;
		int state;

		if (!vt_screen_front()) {
			/* another terminal in front: the game waits, silent */
			pt_sleep_ms(50);
			if (pt_interrupted())
				break;
			paused_us += esp_timer_get_time() - now;
			continue;
		}
		if (gen != vt_screen_gen()) {
			gen = vt_screen_gen();
			clear_screen();		/* the next frame paints the rest */
			next_frame = esp_timer_get_time();
		}
		if (!read_pad(&pad, now, &state))
			break;
		input_update(0, state);
		/* 'p' takes a picture: arm before the frame, write after it */
		if (want_shot) {
			int captured;

			xSemaphoreTake(cleanup.guard, portMAX_DELAY);
			captured = lcd_capture_begin();
			cleanup.capture = !captured;
			xSemaphoreGive(cleanup.guard);
			if (!captured)
				draw = true;
			else {
				want_shot = false;
				klog("nes: screenshot failed (%d)", captured);
			}
		}
		nes_emulate(draw);
		if (want_shot) {
			char path[64];

			if (sd_mounted()) {	/* bin's shots_dir(), which emu cannot reach */
				snprintf(path, sizeof(path), "%s/photos", user_home());
				pt_mkdir(path);
				strlcat(path, "/screenshots", sizeof(path));
				pt_mkdir(path);
			}
			for (int n = 1; n < 1000; n++) {
				struct pt_stat st;

				if (sd_mounted())
					snprintf(path, sizeof(path), "%s/photos/screenshots/nes-%d.bmp",
						 user_home(), n);
				else
					snprintf(path, sizeof(path), "/tmp/nes-%d.bmp", n);
				if (pt_stat(path, &st))
					break;
			}
			klog("nes: %s", lcd_capture_save(path) ? "screenshot failed" : path);
			xSemaphoreTake(cleanup.guard, portMAX_DELAY);
			lcd_capture_end();
			cleanup.capture = false;
			xSemaphoreGive(cleanup.guard);
			want_shot = false;
		}
		stats.frames++;
		if (!draw)
			stats.skipped++;
		frame++;

		/*
		 * nes_emulate() has already mixed this frame's sound into
		 * the APU's own buffer -- mixing it again here would make a
		 * second frame of it from a state that has moved on.
		 */
		if (opt->sound && apu && apu->buffer) {
			int samples = apu->samples_per_frame;
			ssize_t written;

			for (int s = 0; s < samples; s++) {
				int mag = apu->buffer[s] < 0 ? -apu->buffer[s] : apu->buffer[s];

				if (mag > stats.sound_peak)
					stats.sound_peak = mag;
			}
			written = audio_write(apu->buffer, samples * sizeof(*apu->buffer), 1);
			if (written != (ssize_t)(samples * sizeof(*apu->buffer))) {
				ret = pt_interrupted() ? 0 : written < 0 ? (int)written : -EIO;
				break;
			}
			continue;	/* the codec is the clock */
		}
		next_frame += frame_us;
		int64_t wait = next_frame - esp_timer_get_time();

		if (wait > 1500)
			pt_sleep_ms(wait / 1000);	/* the bulk of it */
		else if (wait <= 0)
			next_frame = esp_timer_get_time();	/* behind: give up the debt */
		while (esp_timer_get_time() < next_frame)
			;			/* the last millisecond, exactly */
	}
	int64_t elapsed = esp_timer_get_time() - started - paused_us;

	stats.seconds = elapsed / 1000000;
	if (elapsed > 0)		/* from microseconds: whole seconds lie */
		stats.fps_tenths = (int)(stats.frames * 10000000LL / elapsed);
out:
	if (screen)
		vt_hold_screen(false);
	if (opt->sound)
		audio_stop();
	while (!nes_cleanup_step(&cleanup))
		vTaskDelay(pdMS_TO_TICKS(10));
	proc_set_cleanup(NULL, NULL);
	return ret;
}
