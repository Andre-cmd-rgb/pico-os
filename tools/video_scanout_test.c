/*
 * The real native scanout (ili9341.c) against a simulated panel and bus:
 * a refresh running at a set rate, Get Scanline reads that take their
 * time, and a queued stream whose commands cost a random few us each,
 * with stalls thrown in. Every frame the driver says went out clean must
 * have been seen whole by every refresh; one that may not have been must
 * be reported (-EAGAIN), never passed off as clean. At 40 MHz the driver
 * slows the refresh with porch lines, and the panel here follows.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scanout.h"

#define CMD_MADCTL	0x36
#define CMD_CASET	0x2a
#define CMD_RASET	0x2b
#define CMD_RAMWR	0x2c
#define MADCTL_MY	0x80
#define MADCTL_BGR	0x08
#define pdMS_TO_TICKS(ms) (ms)

struct lcd_io_step {
	uint8_t		 cmd;
	const uint8_t	*data;
	size_t		 len;
};

/* ------------------------------------------------------------ the bench */

static int width = 320, height = 240;
static void *io = (void *)1;
static bool locked;

static double now;			/* us */
static double period_us = 13979;	/* a refresh */
static int sim_lines = 324, sim_porch = 4;
static double phase;			/* where the refresh was at 0 */
static double bytes_per_us = 10;	/* 80 MHz */
static int read_fails, stream_fails;
static double stall_us;			/* once, before band stall_band */
static int stall_band = -1;
static unsigned seed = 1;

/* where each row (in the refresh's order) was written, its first and last pixel */
static double row_from[320], row_to[320];
static bool row_written[320];
static bool scan_down_sim;

static double jitter(double lo, double hi)
{
	seed = seed * 1103515245 + 12345;
	return lo + (hi - lo) * ((seed >> 8) & 0xffff) / 65535.0;
}

static uint8_t ili9341_madctl(void) { return 0xe8; }
static int64_t esp_timer_get_time(void) { return (int64_t)now; }
static void vTaskDelay(int ticks) { now += ticks * 1000 + jitter(0, 400); }
static void lcd_bus_lock(bool on) { assert(on != locked); locked = on; }
static bool bus_take(bool wait) { (void)wait; lcd_bus_lock(true); return true; }
static int lcd_io_stream_clock(void) { return (int)(bytes_per_us * 8e6); }
static void klog(const char *fmt, ...) { (void)fmt; }
typedef unsigned UBaseType_t;
#define configMAX_PRIORITIES 25
static UBaseType_t sim_prio = 4;
static UBaseType_t uxTaskPriorityGet(void *task) { (void)task; return sim_prio; }
static void vTaskPrioritySet(void *task, UBaseType_t p) { (void)task; sim_prio = p; }

/*
 * Blanking Porch Control: the front porch's lines after the last row,
 * the back porch's before the first, after `sim_back`, which is how far
 * the count is into a refresh when the back porch begins.
 */
static int sim_back;

static int esp_lcd_panel_io_tx_param(void *h, int cmd, const void *param, size_t n)
{
	const uint8_t *p = param;
	double line_us = period_us / sim_lines;

	assert(h == io && locked && cmd == 0xb5 && n == 4 && p[0] >= 2 && p[1] >= 2);
	assert(p[0] <= 127 && p[1] <= 127);
	sim_lines = 320 + p[0] + p[1];
	sim_porch = sim_back + p[1];
	period_us = line_us * sim_lines;
	return 0;
}

static double line_at(double t)
{
	return fmod(t + phase, period_us) * sim_lines / period_us;
}

static int lcd_io_read(uint8_t cmd, uint8_t *b, int n)
{
	int line;

	assert(locked && cmd == 0x45 && n == 3);
	if (read_fails && !--read_fails)
		return -5;
	line = (int)line_at(now + 40);	/* the count is taken mid-read */
	now += 85;
	b[0] = 0;
	b[1] = line >> 1;
	b[2] = (line & 1) << 7;
	return 0;
}

static int lcd_io_stream(const struct lcd_io_step *s, int n)
{
	int cw = 0, lo = 0, hi = -1, band = 0;

	if (!s)
		return 0;
	assert(locked);
	if (stream_fails && !--stream_fails)
		return -5;
	now += jitter(30, 160);			/* queueing starts it */
	for (int i = 0; i < n; i++) {
		now += 8 / bytes_per_us + jitter(2, 12);	/* the command */
		if (s[i].cmd == CMD_CASET) {
			cw = (s[i].data[2] << 8 | s[i].data[3]) - (s[i].data[0] << 8 | s[i].data[1]) + 1;
		} else if (s[i].cmd == CMD_RASET) {
			lo = s[i].data[0] << 8 | s[i].data[1];
			hi = s[i].data[2] << 8 | s[i].data[3];
		} else if (s[i].cmd == CMD_RAMWR) {
			double row_us = cw * 2 / bytes_per_us;

			assert(cw > 0 && s[i].len == (size_t)(hi - lo + 1) * cw * 2);
			if (band++ == stall_band)
				now += stall_us;
			now += jitter(2, 12);
			for (int a = lo; a <= hi; a++) {
				int r = scan_down_sim ? 319 - a : a;

				assert(!row_written[r]);
				row_written[r] = true;
				row_from[r] = now;
				now += row_us;
				row_to[r] = now;
			}
			continue;
		}
		if (s[i].len)
			now += s[i].len / bytes_per_us + jitter(2, 12);
	}
	return 0;
}

#include "scanout_under_test.h"

/* ------------------------------------------------------------ the check */

/* How many times row r has begun to be refreshed by time t. */
static long scans_by(int r, double t)
{
	double at = (r + sim_porch) * period_us / sim_lines;	/* within a refresh */

	return (long)floor((t + phase - at) / period_us) + 1;
}

/* Every row written between the same two refreshes of it, none while it was read. */
static bool whole(int first, int rows)
{
	long pass = -1;
	double slot = period_us / sim_lines;

	for (int r = first; r < first + rows; r++) {
		long a = scans_by(r, row_from[r] - slot), b = scans_by(r, row_to[r]);

		assert(row_written[r]);
		if (a != b)
			return false;		/* read while being written */
		if (pass < 0)
			pass = a;
		else if (a != pass)
			return false;		/* one refresh saw it old, another new */
	}
	return true;
}

static int frame(int cw, int p0, int ph, bool upwards, int *torn)
{
	int first = upwards ? 320 - p0 - ph : p0, ret;

	scan_down = scan_down_sim = upwards;
	memset(row_written, 0, sizeof(row_written));
	ret = lcd_draw_native((const uint8_t *)0x3fc00000, (240 - cw) / 2, cw, p0, ph);
	assert(!locked && sim_prio == 4);	/* the priority given back, whatever happened */
	if (ret == 0 || ret == -EAGAIN) {
		bool ok = whole(first, ph);

		if (!ok && ret == 0) {
			fprintf(stderr, "torn but reported clean: cw=%d p0=%d ph=%d up=%d phase=%.0f "
				"period=%.0f porch=%d stall=%.0f@%d\n", cw, p0, ph, upwards, phase,
				period_us, sim_porch, stall_us, stall_band);
			assert(0);
		}
		*torn += !ok;
	}
	return ret;
}

/* A panel of its own: its refresh, where row 0 is in the count; nothing slowed. Measured. */
static void panel(double period, int porch)
{
	locked = true;
	assert(!slow_by(0));
	locked = false;
	sim_back = porch - 2;
	sim_porch = porch;
	sim_lines = 324;
	period_us = period;
	assert(lcd_native_begin(0, 0, 0, 0));
}

int main(void)
{
	static const struct { int cw, p0, ph; } shapes[] = {
		{ 240, 0, 320 },	/* full screen */
		{ 176, 0, 320 },	/* letterboxed */
		{ 239, 10, 287 },	/* cropped, odd */
		{ 160, 80, 160 },
	};
	static const double periods[] = { 13979, 16400, 12500 };
	int late = 0, frames = 0, torn = 0;

	/* The refresh is measured, not assumed. */
	for (int i = 0; i < 3; i++) {
		phase = 1234;
		panel(periods[i], 4);
		assert(refresh.lines == sim_lines);
		assert(fabs(refresh.lines_per_us - sim_lines / period_us) < 0.003 * sim_lines / period_us);
	}

	/* Every start, both ways, every shape and rate: clean, and almost never late. */
	for (int i = 0; i < 3; i++) {
		for (int porch = 1; porch <= 7; porch += 3) {
			phase = 0;
			panel(periods[i], porch);
			for (int up = 0; up < 2; up++)
				for (size_t s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++)
					for (double ph = 0; ph < period_us; ph += 61) {
						phase = ph;
						int ret = frame(shapes[s].cw, shapes[s].p0, shapes[s].ph, up, &torn);

						assert(ret == 0 || ret == -EAGAIN);
						late += ret == -EAGAIN;
						frames++;
					}
		}
	}
	printf("video scanout: %d frames, %d late, %d torn\n", frames, late, torn);
	assert(torn == 0);
	assert(late * 1000 < frames);

	/* Stalls mid-frame: whatever tears is reported. */
	panel(13979, 4);
	for (stall_band = 0; stall_band < 10; stall_band++)
		for (stall_us = 250; stall_us < 16000; stall_us *= 1.7)
			for (double ph = 0; ph < period_us; ph += 397) {
				phase = ph;
				int ret = frame(240, 0, 320, true, &torn);

				assert(ret == 0 || ret == -EAGAIN);
			}
	stall_band = -1;
	stall_us = 0;

	/*
	 * At 40 MHz a whole screen takes longer than two refreshes: the
	 * refresh is slowed for it, and then nothing tears and nothing is late.
	 */
	{
		int late40 = 0, frames40 = 0, torn40 = 0, slowest = 0;

		bytes_per_us = 5;
		for (int i = 0; i < 2; i++) {
			phase = 0;
			panel(periods[i], 4);
			for (int up = 0; up < 2; up++)
				for (size_t sh = 0; sh < sizeof(shapes) / sizeof(shapes[0]); sh++)
					for (double ph = 0; ph < period_us; ph += 61) {
						phase = ph;
						int ret = frame(shapes[sh].cw, shapes[sh].p0, shapes[sh].ph,
								up, &torn40);

						assert(ret == 0 || ret == -EAGAIN);
						late40 += ret == -EAGAIN;
						frames40++;
					}
			slowest = sim_lines > slowest ? sim_lines : slowest;
		}
		printf("video scanout at 40 MHz: %d frames, %d late, %d torn, refresh up to %d lines\n",
		       frames40, late40, torn40, slowest);
		assert(torn40 == 0 && late40 * 100 < frames40 && slowest <= 500);

		/*
		 * A 24 fps clip: the refresh set to exactly two a frame from
		 * the start, so no frame is up longer than the next, and the
		 * player told how often frames can go. A 30 fps one is too
		 * quick for that, and is shown as if it were 24.
		 */
		for (int fps = 24; fps <= 30; fps += 6) {
			int every = 0, late = 0, torn = 0, n = 0;

			phase = 0;
			panel(13979, 4);
			assert(lcd_native_begin(240, 0, 320, 1000000 / fps));
			every = lcd_native_every();
			/* to the line, as well as the refresh was measured */
			if (fps == 24)
				assert(fabs(period_us * 2 - 1000000.0 / fps) < period_us / sim_lines * 4 &&
				       abs(every - 1000000 / fps) < period_us / sim_lines * 4);
			else	/* shown as if 24 a second */
				assert(abs(every - 1000000 / 24) < period_us / sim_lines * 4);
			for (double ph = 0; ph < period_us; ph += 61, n++) {
				phase = ph;
				late += frame(240, 0, 320, true, &torn) == -EAGAIN;
			}
			printf("video scanout at %d fps: refresh %d lines, a frame every %d us at most, "
			       "%d of %d late, %d torn\n", fps, sim_lines, every, late, n, torn);
			assert(!torn && !late);
		}
		bytes_per_us = 10;
		panel(13979, 4);
	}

	/* Errors come back, and the bus is let go. */
	read_fails = 1;
	assert(frame(240, 0, 320, true, &torn) == -EIO);
	stream_fails = 1;
	assert(frame(240, 0, 320, true, &torn) == -EIO);
	assert(lcd_draw_native(NULL, 0, 241, 0, 320) == -EINVAL);
	assert(lcd_draw_native(NULL, 0, 240, 10, 320) == -EINVAL);
	assert(!locked);
	puts("video scanout: every start and shape whole, stalls reported, errors passed");
	return 0;
}
