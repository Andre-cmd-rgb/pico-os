/* Real scanout logic with a timed panel and DMA, without the ESP SDK. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scanout.h"
#include "scanout_under_test.h"

#define CMD_MADCTL 0x36
#define CMD_CASET 0x2a
#define MADCTL_MY 0x80
#define MADCTL_BGR 0x08

static bool scan_down, locked;
static int64_t now, pause_us;
static int period_us, bands, sent, sends_before_pause, fail_at, bytes_per_us;
static int rectangle_start, rectangle_end;
static bool written[NATIVE_H];
static int64_t written_at[NATIVE_H], refresh_base;
static uint8_t mode;
static void *io = (void *)1;
static int width = 320;

static uint8_t ili9341_madctl(void) { return 0xe8; }
static int64_t esp_timer_get_time(void) { return now; }
static void vTaskDelay(int ticks) { now += ticks * 1000; }
static void esp_rom_delay_us(int us) { now += us; }
static void lcd_bus_lock(bool on) { assert(on != locked); locked = on; }

static int scan_line(void)
{
	assert(locked);
	now += 25;
	return (now % period_us) * (NATIVE_H + PORCH_LINES) / period_us;
}

static int esp_lcd_panel_io_tx_param(void *handle, int command, const void *data, int n)
{
	(void)handle;
	assert(locked);
	if (command == CMD_MADCTL) {
		assert(n == 1);
		mode = *(const uint8_t *)data;
	} else
		assert(command == CMD_CASET && n == 4);
	now += 25;
	return 0;
}

static int send_band(const uint8_t *buf, int p0, int cw, int first, int n)
{
	int offset = scan_down ? NATIVE_H - first - n - p0 : first - p0;

	(void)buf;
	assert(locked && mode == (ili9341_madctl() & (MADCTL_MY | MADCTL_BGR)));
	assert(first >= rectangle_start && first + n <= rectangle_end);
	assert(n > 0 && n <= 64);
	/* The shortened last band alone may need a bounce buffer. */
	if (first + n < rectangle_end)
		assert((offset * cw * 2) % 64 == 0);
	if (++bands == fail_at)
		return -EIO;
	if (!sent)
		refresh_base = now / period_us * period_us;
	for (int row = first; row < first + n; row++) {
		int order = scan_down ? first + n - row : row - first + 1;

		assert(!written[row]);
		written[row] = true;
		written_at[row] = now + 75 + (order * cw * 2 + bytes_per_us - 1) / bytes_per_us;
	}
	sent += n;
	now += (n * cw * 2 + bytes_per_us - 1) / bytes_per_us + 75;
	if (bands == sends_before_pause)
		now += pause_us;
	return 0;
}

#include "scanout_function.h"

static int play(int phase, int width, int top, int height, bool upwards, int64_t pause)
{
	int ret;

	scan_down = upwards;
	now = phase;
	period_us = 16400;
	bytes_per_us = 8;
	pause_us = pause;
	sends_before_pause = 4;
	bands = sent = 0;
	memset(written, 0, sizeof(written));
	rectangle_start = upwards ? NATIVE_H - top - height : top;
	rectangle_end = rectangle_start + height;
	ret = lcd_draw_native((const uint8_t *)"mock", 0, width, top, height);
	assert(!locked && mode == ili9341_madctl());
	if (!fail_at)
		assert(sent == height);
	if (!ret)
		for (int row = rectangle_start; row < rectangle_end; row++) {
			int64_t scanned_at = refresh_base + (int64_t)(row + PORCH_LINES) * period_us /
						   (NATIVE_H + PORCH_LINES);

			/* Every row is old on one sweep and new on the next. */
			if (written_at[row] >= scanned_at + period_us)
				fprintf(stderr, "phase=%d width=%d top=%d height=%d up=%d pause=%lld row=%d write=%lld scan=%lld\n",
					phase, width, top, height, upwards, (long long)pause, row,
					(long long)written_at[row], (long long)(scanned_at + period_us));
			assert(written_at[row] > scanned_at);
			assert(written_at[row] < scanned_at + period_us);
		}
	return ret;
}

int main(void)
{
	for (int direction = 0; direction < 2; direction++)
		for (int phase = 0; phase < 16400; phase += 250) {
			assert(play(phase, 240, 0, 320, direction, 0) == 0);
			assert(play(phase, 239, 10, 287, direction, 0) == 0);
			assert(play(phase, 160, 80, 160, direction, 0) == 0);
		}
	/* Preemption can miss the window, but must finish the same picture. */
	assert(play(0, 240, 0, 320, true, 12000) == -EAGAIN);
	assert(play(0, 240, 0, 320, true, 25000) == -EAGAIN);
	fail_at = 3;
	assert(play(0, 240, 0, 320, true, 0) == -EIO);
	assert(sent < 320);
	assert(lcd_draw_native(NULL, 0, 241, 0, 320) == -EINVAL);
	puts("video scanout: refresh phases, cropped/reversed DMA, late-frame completion and I/O errors passed");
	return 0;
}
