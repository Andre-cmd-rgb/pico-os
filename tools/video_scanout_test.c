/*
 * The real native scanout (ili9341.c) against a simulated panel and bus:
 * a refresh running at a set rate, Get Scanline reads that take their
 * time, and a queued stream whose commands cost a random few us each,
 * with stalls thrown in. Every frame the driver says went out clean must
 * have been seen whole by every refresh; one that may not have been must
 * be reported (-EAGAIN), never passed off as clean.
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
static int lcd_io_clock(void) { return (int)(bytes_per_us * 8e6); }
static void klog(const char *fmt, ...) { (void)fmt; }

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
	assert(!locked);
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
		period_us = periods[i];
		phase = 1234;
		locked = false;
		assert(lcd_native_ok());
		assert(refresh.lines == sim_lines);
		assert(fabs(refresh.lines_per_us - sim_lines / period_us) < 0.003 * sim_lines / period_us);
	}

	/* Every start, both ways, every shape and rate: clean, and almost never late. */
	for (int i = 0; i < 3; i++) {
		period_us = periods[i];
		phase = 0;
		assert(lcd_native_ok());
		for (int porch = 1; porch <= 7; porch += 3) {
			sim_porch = porch;
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
	sim_porch = 4;
	printf("video scanout: %d frames, %d late, %d torn\n", frames, late, torn);
	assert(torn == 0);
	assert(late * 1000 < frames);

	/* Stalls mid-frame: whatever tears is reported. */
	period_us = 13979;
	assert(lcd_native_ok());
	for (stall_band = 0; stall_band < 10; stall_band++)
		for (stall_us = 250; stall_us < 16000; stall_us *= 1.7)
			for (double ph = 0; ph < period_us; ph += 397) {
				phase = ph;
				int ret = frame(240, 0, 320, true, &torn);

				assert(ret == 0 || ret == -EAGAIN);
			}
	stall_band = -1;
	stall_us = 0;

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
