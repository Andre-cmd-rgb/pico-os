/*
 * The ILI9341 panel: the start-up sequence, drawing, and the backlight.
 *
 * The bus underneath is a separate file (lcd_io.h): SPI on boards with the
 * display soldered on, an 8-bit parallel bus on Arduino shields. Nothing
 * here knows which one it is talking to.
 */
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "ili9341.h"
#include "lcd_io.h"
#include "scanout.h"

#if CONFIG_PT_LCD

/*
 * Panels sold as ILI9341 are not all the same silicon, and each wants its
 * own power, VCOM and gamma settings; with the wrong ones the controller
 * answers and the screen stays blank or washed out. Two tables, as in
 * TFT_eSPI: the usual Adafruit one, and the one Freenove ships for this
 * board (TFT_eSPI's ILI9341_2, from its issue #1172).
 */
#if CONFIG_PT_LCD_INIT_ALT

#define INIT_NAME	" (alt init)"

const uint8_t ili9341_init_seq[] = {
	CMD_SWRESET, DELAY, 150,
	0xcf, 3, 0x00, 0xc1, 0x30,
	0xed, 4, 0x64, 0x03, 0x12, 0x81,
	0xe8, 3, 0x85, 0x00, 0x78,
	0xcb, 5, 0x39, 0x2c, 0x00, 0x34, 0x02,
	0xf7, 1, 0x20,
	0xea, 2, 0x00, 0x00,
	0xc0, 1, 0x10,				/* power control 1, lower drive */
	0xc1, 1, 0x00,				/* power control 2 */
	0xc5, 2, 0x30, 0x30,			/* VCOM control 1 */
	0xc7, 1, 0xb7,				/* VCOM control 2 */
	0x3a, 1, 0x55,				/* 16 bits per pixel */
	0x36, 1, 0x08,				/* BGR, no mirroring yet */
	0xb1, 2, 0x00, 0x1f,			/* retain the board's established frame-clock setting */
	0xb6, 3, 0x08, 0x82, 0x27,		/* display function control */
	0xf2, 1, 0x00,				/* 3-gamma off */
	0x26, 1, 0x01,				/* gamma curve 1 */
	0xe0, 15, 0x0f, 0x2a, 0x28, 0x08, 0x0e, 0x08, 0x54, 0xa9,
		  0x43, 0x0a, 0x0f, 0x00, 0x00, 0x00, 0x00,
	0xe1, 15, 0x00, 0x15, 0x17, 0x07, 0x11, 0x06, 0x2b, 0x56,
		  0x3c, 0x05, 0x10, 0x0f, 0x3f, 0x3f, 0x0f,
	CMD_RASET, 4, 0x00, 0x00, 0x01, 0x3f,
	CMD_CASET, 4, 0x00, 0x00, 0x00, 0xef,
	CMD_SLPOUT, DELAY, 120,
	CMD_DISPON, DELAY, 20,
};

#else

#define INIT_NAME	""

/* The sequence the Adafruit and Bodmer drivers use for this controller. */
const uint8_t ili9341_init_seq[] = {
	CMD_SWRESET, DELAY, 150,
	0xef, 3, 0x03, 0x80, 0x02,
	0xcf, 3, 0x00, 0xc1, 0x30,
	0xed, 4, 0x64, 0x03, 0x12, 0x81,
	0xe8, 3, 0x85, 0x00, 0x78,
	0xcb, 5, 0x39, 0x2c, 0x00, 0x34, 0x02,
	0xf7, 1, 0x20,
	0xea, 2, 0x00, 0x00,
	0xc0, 1, 0x23,				/* power control 1 */
	0xc1, 1, 0x10,				/* power control 2 */
	0xc5, 2, 0x3e, 0x28,			/* VCOM control 1 */
	0xc7, 1, 0x86,				/* VCOM control 2 */
	0x37, 1, 0x00,				/* vertical scroll start */
	0x3a, 1, 0x55,				/* 16 bits per pixel */
	0xb1, 2, 0x00, 0x1f,			/* frame clock; the actual rate depends on the panel */
	0xb6, 3, 0x08, 0x82, 0x27,		/* display function control */
	0xf2, 1, 0x00,				/* 3-gamma off */
	0x26, 1, 0x01,				/* gamma curve 1 */
	0xe0, 15, 0x0f, 0x31, 0x2b, 0x0c, 0x0e, 0x08, 0x4e, 0xf1,
		  0x37, 0x07, 0x10, 0x03, 0x0e, 0x09, 0x00,
	0xe1, 15, 0x00, 0x0e, 0x14, 0x03, 0x11, 0x07, 0x31, 0xc1,
		  0x48, 0x08, 0x0f, 0x0c, 0x31, 0x36, 0x0f,
	CMD_SLPOUT, DELAY, 120,
	CMD_DISPON, DELAY, 20,
};

#endif

const size_t ili9341_init_len = sizeof(ili9341_init_seq);

static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t	 done;
static SemaphoreHandle_t	 bus_lock;	/* renderer and lcdtest may draw at once */
static int			 width, height;
static int			 brightness = 100;	/* what was asked for */
static int			 lamp = 100;		/* what it is at: dimmed, or dark */
static int			 dimmer = -1;		/* the idle dimmer's level; -1 none */
static bool			 panel_awake;
static int			 rotation = CONFIG_PT_LCD_ROTATION;

uint8_t ili9341_madctl(void)
{
	static const uint8_t modes[4] = {
		MADCTL_MX, MADCTL_MV, MADCTL_MY, MADCTL_MX | MADCTL_MY | MADCTL_MV,
	};
	uint8_t mode = modes[rotation];

#ifdef CONFIG_PT_LCD_BGR
	mode |= MADCTL_BGR;
#endif
	return mode;
}

static bool drawing;		/* lcd_draw_start()'s pixels still going out; bus lock */

void lcd_wait_done(int ms)
{
	xSemaphoreTake(done, pdMS_TO_TICKS(ms));
}

/* Whatever lcd_draw_start() left going, out: the bus's next user has it to itself. */
static void draw_settle(void)
{
	if (drawing) {
		lcd_wait_done(1000);
		drawing = false;
	}
}

void lcd_bus_lock(bool take)
{
	if (take) {
		xSemaphoreTake(bus_lock, portMAX_DELAY);
		draw_settle();
	} else {
		xSemaphoreGive(bus_lock);
	}
}

/* The bus, waiting for it or only if it is free; given back by lcd_bus_lock(false). */
static bool bus_take(bool wait)
{
	if (!xSemaphoreTake(bus_lock, wait ? portMAX_DELAY : 0))
		return false;
	draw_settle();
	return true;
}

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t panel_io,
				    esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
	BaseType_t woken = pdFALSE;

	xSemaphoreGiveFromISR(done, &woken);
#if CONFIG_PT_LCD_ILI9341_SPI
	/* IDF's SPI wrapper ignores our return flag; request the deferred
	 * ISR-exit switch so a waiting renderer need not wait for a tick.
	 * The i80 wrapper consumes the return flag itself. */
	if (woken == pdTRUE)
		portYIELD_FROM_ISR();
#endif
	return woken == pdTRUE;
}

/* ------------------------------------------------------------ backlight */

#if CONFIG_PT_LCD_BACKLIGHT >= 0

#define BL_TIMER	LEDC_TIMER_0
#define BL_CHANNEL	LEDC_CHANNEL_0
#define BL_MAX		255

/*
 * The PWM runs from the chip's own 17.5 MHz RC oscillator rather than the
 * APB clock, and is kept alive through light sleep: from the APB clock it
 * would stop whenever the chip dozed between keys, with the lamp stuck
 * at full or at nothing for as long as the doze lasted.
 */
static void backlight_init(void)
{
	const ledc_timer_config_t timer = {
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.timer_num = BL_TIMER,
		.duty_resolution = LEDC_TIMER_8_BIT,
		.freq_hz = 5000,		/* above hearing, no visible flicker */
		.clk_cfg = LEDC_USE_RC_FAST_CLK,
	};
	const ledc_channel_config_t channel = {
		.gpio_num = CONFIG_PT_LCD_BACKLIGHT,
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.channel = BL_CHANNEL,
		.timer_sel = BL_TIMER,
		.duty = 0,
		.hpoint = 0,
		.sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
	};

	ledc_timer_config(&timer);
	ledc_channel_config(&channel);
	lcd_backlight_set(CONFIG_PT_LCD_BRIGHTNESS);
}

static void lamp_set(int percent)
{
	int duty;

	lamp = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	duty = lamp * BL_MAX / 100;
#ifdef CONFIG_PT_LCD_BACKLIGHT_ACTIVE_LOW
	duty = BL_MAX - duty;
#endif
	ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty);
	ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

/* The brightness asked for: `backlight`, the notes reader, an alarm. */
void lcd_backlight_set(int percent)
{
	brightness = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	lamp_set(dimmer >= 0 && dimmer < brightness ? dimmer : brightness);
}

/* The idle dimmer, over the top of it: -1 gives it back. */
void lcd_light(int percent)
{
	dimmer = percent;
	lamp_set(dimmer >= 0 && dimmer < brightness ? dimmer : brightness);
}

/*
 * The backlight is the largest thing on this board that a sleeping chip
 * cannot switch off by itself: LEDC stops when the cores do, and an
 * unheld pin floats, so the lamp can stay lit through a deep sleep and
 * empty the cell overnight. Stop the channel at the off level and ask
 * the pin to keep it.
 */
static void backlight_sleep(void)
{
	int off = 0;

#ifdef CONFIG_PT_LCD_BACKLIGHT_ACTIVE_LOW
	off = 1;
#endif
	lamp = 0;
	ledc_stop(LEDC_LOW_SPEED_MODE, BL_CHANNEL, off);
}

#else

static void backlight_init(void) { }
static void backlight_sleep(void) { }
void lcd_backlight_set(int percent) { brightness = lamp = percent; }
void lcd_light(int percent) { dimmer = percent; }

#endif

/*
 * Ready for deep sleep: lamp out, panel in its own sleep mode, and the
 * pin states pinned so they survive the cores stopping.
 */
void lcd_sleep(void)
{
	backlight_sleep();
	if (io && width) {
		esp_err_t err;

		lcd_bus_lock(true);
		err = esp_lcd_panel_io_tx_param(io, CMD_SLPIN, NULL, 0);
		if (err)
			klog("lcd: sleep command failed (%s)", esp_err_to_name(err));
		else
			panel_awake = false;
		lcd_bus_lock(false);
	}
	gpio_deep_sleep_hold_en();
}

int lcd_backlight_get(void)
{
	return brightness;
}

int lcd_backlight_now(void)
{
	return lamp;
}

/*
 * The panel's own sleep, while the screen is dark for want of anyone
 * looking: its oscillator and charge pumps stop and it draws a few
 * microamps instead of a few milliamps. What it shows is kept, but
 * nothing may be sent until it is awake again, which takes 5 ms; and it
 * must be awake for 120 ms before it may sleep again.
 */
void lcd_panel_power(bool on)
{
	esp_err_t err;

	if (!io || !width)
		return;
	lcd_bus_lock(true);
	if (on == panel_awake)
		goto out;
	err = esp_lcd_panel_io_tx_param(io, on ? CMD_SLPOUT : CMD_SLPIN, NULL, 0);
	if (err) {
		klog("lcd: %s command failed (%s)", on ? "wake" : "sleep", esp_err_to_name(err));
		goto out;
	}
	vTaskDelay(pdMS_TO_TICKS(on ? 120 : 5));
	panel_awake = on;
out:
	lcd_bus_lock(false);
}

bool lcd_panel_on(void)
{
	return panel_awake;
}

/* ------------------------------------------------------------ panel */

int lcd_init(void)
{
	int rst = lcd_io_reset_gpio();
	int ret;
	esp_err_t err;
	uint8_t mode;
	bool landscape;

	if (width && height && io)
		return 0;
	width = height = 0;
	panel_awake = false;
	/* Retry cleanup if an SDK delete failed on an earlier attempt. Its
	 * callback still owns the semaphores until the bus has been released. */
	if ((ret = lcd_io_close(&io)))
		return ret;
	if (done)
		vSemaphoreDelete(done);
	if (bus_lock)
		vSemaphoreDelete(bus_lock);
	done = xSemaphoreCreateBinary();
	bus_lock = xSemaphoreCreateMutex();
	if (!done || !bus_lock) {
		ret = -ENOMEM;
		goto fail;
	}
	if ((ret = lcd_io_open(&io, on_color_done)))
		goto fail;

	if (rst >= 0) {
		if ((err = gpio_set_direction(rst, GPIO_MODE_OUTPUT)) ||
		    (err = gpio_set_level(rst, 0)))
			goto reset_fail;
		vTaskDelay(pdMS_TO_TICKS(20));
		if ((err = gpio_set_level(rst, 1)))
			goto reset_fail;
		vTaskDelay(pdMS_TO_TICKS(120));
	}

	for (size_t i = 0; i < ili9341_init_len;) {
		uint8_t cmd = ili9341_init_seq[i++];
		uint8_t len = ili9341_init_seq[i] & ~DELAY;
		bool delay = ili9341_init_seq[i++] & DELAY;

		err = esp_lcd_panel_io_tx_param(io, cmd, len ? &ili9341_init_seq[i] : NULL, len);
		if (err) {
			klog("lcd: initialization command 0x%02x failed (%s)", cmd, esp_err_to_name(err));
			ret = -EIO;
			goto fail;
		}
		i += len;
		vTaskDelay(pdMS_TO_TICKS(delay ? ili9341_init_seq[i++] : CMD_GAP_MS));
	}

	mode = ili9341_madctl();
	err = esp_lcd_panel_io_tx_param(io, CMD_MADCTL, &mode, 1);
	if (err) {
		klog("lcd: rotation setup failed (%s)", esp_err_to_name(err));
		ret = -EIO;
		goto fail;
	}
#ifdef CONFIG_PT_LCD_INVERT
	err = esp_lcd_panel_io_tx_param(io, CMD_INVON, NULL, 0);
#else
	err = esp_lcd_panel_io_tx_param(io, CMD_INVOFF, NULL, 0);
#endif
	if (err) {
		klog("lcd: inversion setup failed (%s)", esp_err_to_name(err));
		ret = -EIO;
		goto fail;
	}

	landscape = rotation & 1;
	width = landscape ? 320 : 240;
	height = landscape ? 240 : 320;
	panel_awake = true;
	backlight_init();
	klog("lcd: ili9341%s on %s, %dx%d", INIT_NAME, lcd_io_name(), width, height);
	return 0;
reset_fail:
	klog("lcd: reset GPIO failed (%s)", esp_err_to_name(err));
	ret = -EIO;
fail:
	width = height = 0;
	panel_awake = false;
	if (!lcd_io_close(&io)) {
		if (done)
			vSemaphoreDelete(done);
		if (bus_lock)
			vSemaphoreDelete(bus_lock);
		done = bus_lock = NULL;
	}
	return ret;
}

/*
 * A half turn: 1 and 3 are the two ways up in landscape, 0 and 2 in
 * portrait. Only those are offered while running -- a quarter turn would
 * change the width and height the console was laid out for. Nothing is
 * being sent while the bus is held, so the switch falls between frames;
 * whoever owns the screen paints it again afterwards.
 */
int lcd_set_rotation(int r)
{
	uint8_t mode;

	if (!width || !io)
		return -ENODEV;
	if (r < 0 || r > 3 || (r & 1) != (rotation & 1))
		return -EINVAL;
	lcd_bus_lock(true);
	rotation = r;
	mode = ili9341_madctl();
	esp_lcd_panel_io_tx_param(io, CMD_MADCTL, &mode, 1);
	lcd_bus_lock(false);
	return 0;
}

int lcd_rotation(void)
{
	return rotation;
}

/*
 * The bus clock. The panel is rated for far less than the 40 MHz this
 * board runs it at, and past 40 it writes pixels twice now and then; if
 * pictures come out with streaks or wrong colours, a slower clock tells
 * whether the wires are why.
 */
int lcd_set_clock(int hz)
{
	int ret;

	if (!width || !io)
		return -ENODEV;
	if (hz < 10000000 || hz > 80000000)
		return -EINVAL;
	lcd_bus_lock(true);
	ret = lcd_io_set_clock(&io, hz);
	lcd_bus_lock(false);
	if (!ret)
		klog("lcd: bus clock now %d MHz", hz / 1000000);
	return ret;
}

int lcd_clock(void)
{
	return lcd_io_clock();
}

/*
 * Tearing, and how it is kept out of moving pictures.
 *
 * The panel refreshes itself from its memory a row at a time along its
 * own rows -- portrait, as it is built, 320 rows of 240 -- whatever way up
 * the picture is sent. A refresh that happens while a frame goes out
 * shows the new frame on one side of the place where it met the frame
 * going in, and the old one on the other: in a landscape frame, a seam
 * across the picture. The panel's TE pin, which would say when a refresh
 * starts, is not wired on this board; its data line is, and the panel can
 * be asked which line it is refreshing (Get Scanline, 0x45).
 *
 * So a frame goes out in the panel's own order, in bands of rows sent in
 * the order the refresh meets them, and only when the refresh is where
 * no band can be met half written (scanout.h works out where that is).
 * The bands are queued on the bus all at once (lcd_io_stream): between
 * two of them nothing waits on a task, which is what had let a refresh
 * catch frames up -- a decoder on the same core, Wi-Fi, a tick's delay
 * before the sender woke, ten times a frame. The refresh is measured
 * when a clip starts, the frame's pace after every frame, and a frame
 * that took longer than it could have is counted.
 *
 * Two things this panel does that it had to be shown: the row order bit
 * (MY) turns the refresh round as well as the writing, so it is never
 * changed between frames, only the row/column exchange is; and with MY
 * set the refresh runs from the last row of the frame to the first. A
 * band's rows go in from its first, so each band has its own row command.
 *
 * Frames go out at 40 MHz at most (io_spi.c says why), and a whole
 * screen then takes 31 ms: longer than two refreshes, so no start would
 * keep every refresh off it. While video is being sent, then, the
 * refresh is slowed as far as a frame needs, by blank lines: after the
 * last row (the front porch) and, past the most it takes, before the
 * first (the back porch, which moves row 0 down the count as far). That
 * leaves each row driven as long as before, unlike halving the panel's
 * clock, which faded the picture.
 */
#define NATIVE_W	240		/* the panel as it is built */
#define NATIVE_H	320
#define CMD_SCANLINE	0x45
#define CMD_PORCH	0xb5		/* blanking porch control */
#define PORCH_LINES	4		/* sync and back porch, before row 0 */
#define MARGIN_LINES	6		/* kept clear of the refresh: a read's length, and more */
#define SLEEP_US	1500		/* a longer wait sleeps; a shorter one reads the line again */
#define MEASURE_US	100000		/* three of the slowest refreshes, and some */
#define PORCH_MAX	127		/* the most lines the panel takes in either porch */
#define SLOW_MAX	500		/* lines a refresh at most: 46 Hz, before it flickers */
#define SLOW_SPARE	6		/* lines added beyond what a frame needs */
#define SLOW_OVERHEAD	2500		/* a frame's overhead counted at most when slowing, us */
#define WINDOW_MIN	3		/* lines: a narrower window can fall between two reads */
#define SLOWEST_GOAL_US	41667		/* a frame time the refresh is kept in step with: 24 fps */

/* The panel's own porches: front, back (lines), then its two in pixels. */
static const uint8_t porches[4] = { 0x02, 0x02, 0x0a, 0x14 };
static int slowed;			/* lines added to the porches */
static int native_users;		/* lcd_native_begin()s not yet ended */
static float native_px_us;		/* the last begun frame's bands, without the overhead */

static bool scan_down = true;		/* with MY set: the refresh runs from the last row */
static struct scanout_panel refresh = {
	.lines_per_us = (NATIVE_H + PORCH_LINES) / 14000.0f,
	.lines = NATIVE_H + PORCH_LINES,
	.porch = PORCH_LINES,
	.margin = MARGIN_LINES,
};
/* What a frame costs besides its pixels -- queueing, commands, the bus's
 * interrupts -- as frames have gone: up at once, down slowly. */
#define OVERHEAD_US	1500		/* what 13 bands at 40 MHz have cost */
static float overhead_us = OVERHEAD_US;

/*
 * The line being refreshed, 0 at the start of vertical sync. The panel
 * sends a dummy bit first, so the count arrives a bit to the right: its
 * bits 8-1 in the second byte, bit 0 at the top of the third.
 */
static int scan_line(void)
{
	uint8_t b[3];

	if (lcd_io_read(CMD_SCANLINE, b, 3))
		return -1;
	return b[1] << 1 | b[2] >> 7;
}

/*
 * How fast the refresh goes and how many lines it counts, read off the
 * panel over two refreshes: it is not the datasheet's, and it drifts
 * with the panel's temperature. Bus held.
 */
static int measure_refresh(void)
{
	int64_t began = esp_timer_get_time(), first = 0, last_wrap = 0;
	int prev = -1, top = 0, wraps = 0, line, lines;

	while (wraps < 3 && esp_timer_get_time() - began < MEASURE_US) {
		if ((line = scan_line()) < 0)
			return -EIO;
		if (prev >= 0 && line < prev) {
			last_wrap = esp_timer_get_time();
			if (!wraps)
				first = last_wrap;
			wraps++;
		}
		if (line > top)
			top = line;
		prev = line;
	}
	if (wraps < 3 || top < NATIVE_H)
		return -EIO;
	/* the reads are two lines apart, and may miss the last */
	lines = NATIVE_H + PORCH_LINES + slowed;
	refresh.lines = top + 1 > lines ? top + 1 : lines;
	refresh.lines_per_us = (float)refresh.lines * (wraps - 1) / (float)(last_wrap - first);
	return 0;
}

/* The refresh `extra` lines longer than the panel's own: the front porch first. Bus held. */
static int slow_by(int extra)
{
	int front = extra < PORCH_MAX - porches[0] ? extra : PORCH_MAX - porches[0];
	uint8_t p[4] = { porches[0] + front, porches[1] + extra - front, porches[2], porches[3] };

	if (esp_lcd_panel_io_tx_param(io, CMD_PORCH, p, 4))
		return -EIO;
	slowed = extra;
	refresh.porch = PORCH_LINES + extra - front;
	return 0;
}

static void paces(int clock, float overhead, struct scanout_pace *fast,
		  struct scanout_pace *slow);

/* The windows wide enough to be found by reading the line; how many. */
static int wide(int nw, float lo[2], float hi[2])
{
	int n = 0;

	for (int i = 0; i < nw; i++)
		if (hi[i] - lo[i] >= WINDOW_MIN) {
			lo[n] = lo[i];
			hi[n] = hi[i];
			n++;
		}
	return n;
}

/* A frame's bands, in the order the refresh meets them; their number, -1 if too many. */
static int frame_bands(int cw, int p0, int ph, struct scanout_band *band)
{
	return scanout_bands(cw, scan_down ? NATIVE_H - p0 - ph : p0, ph, scan_down, band);
}

/*
 * The fewest lines a refresh can count and leave frames of these bands,
 * costing `overhead` besides their pixels, a window wide enough to find;
 * -1 if not even the slowest refresh allowed does.
 */
static int lines_for(const struct scanout_band *band, int nb, int cw, float overhead)
{
	struct scanout_panel want = refresh;
	struct scanout_pace fast, slow;
	int front = PORCH_MAX - porches[0];
	float lo[2], hi[2];

	paces(lcd_io_stream_clock(), overhead * 1.25f + 100, &fast, &slow);
	for (int lines = NATIVE_H + PORCH_LINES; lines <= SLOW_MAX; lines++) {
		int extra = lines - NATIVE_H - PORCH_LINES;

		want.lines = lines;
		want.porch = PORCH_LINES + (extra > front ? extra - front : 0);
		if (wide(scanout_window(band, nb, cw, &want, &fast, &slow, lo, hi), lo, hi))
			return lines;
	}
	return -1;
}

/*
 * The refresh `lines` long. Not measured again: blank lines leave each
 * line as long as it was, and a measurement made while a clip is being
 * decoded can miss a refresh between two reads and come out double.
 * Bus held.
 */
static int slow_to(int lines)
{
	int extra = lines - NATIVE_H - PORCH_LINES, was = slowed;

	extra = extra < 0 ? 0 : extra > SLOW_MAX - NATIVE_H - PORCH_LINES ?
		SLOW_MAX - NATIVE_H - PORCH_LINES : extra;
	if (extra == was)
		return 0;
	if (slow_by(extra))
		return -EIO;
	refresh.lines += extra - was;
	klog("lcd: refresh slowed to %d lines in %d us for video (%d us a frame besides pixels)",
	     refresh.lines, (int)(refresh.lines / refresh.lines_per_us), (int)overhead_us);
	return 0;
}

/*
 * Before video is sent: whether frames of cw x ph, from row p0, can be
 * sent in step with the refresh. The refresh is measured, then slowed as
 * far as such a frame needs; and, when `frame_us` says how often the
 * clip has one, to a whole number of refreshes a frame where that is
 * slow enough, so that every frame is up as long as the last (48 Hz for
 * 24 frames a second). lcd_native_every() then says how often such
 * frames can go at most. Each yes is ended with lcd_native_end(), which
 * puts the refresh back once the last has; cw 0 asks only whether.
 */
bool lcd_native_begin(int cw, int p0, int ph, int frame_us)
{
	struct scanout_band band[SCANOUT_MAX_BANDS];
	struct scanout_pace fast, slow;
	int nb, lines = 0;
	float line_us, start, span;
	bool ok;

	native_px_us = 0;
	if (!width || !io || lcd_io_stream(NULL, 0))
		return false;
	lcd_bus_lock(true);
	ok = width == NATIVE_H && height == NATIVE_W && !measure_refresh();
	native_users += ok;
	overhead_us = OVERHEAD_US;	/* not what the last clip's stalls left */
	if (ok)
		klog("lcd: refresh %d lines in %d us", refresh.lines,
		     (int)(refresh.lines / refresh.lines_per_us));
	else
		klog("lcd: video not in step with the refresh (%dx%d, refresh unread)", width, height);
	nb = ok && cw > 0 ? frame_bands(cw, p0, ph, band) : -1;
	if (nb > 0 && (lines = lines_for(band, nb, cw, overhead_us)) > 0) {
		line_us = 1 / refresh.lines_per_us;
		paces(lcd_io_stream_clock(), overhead_us * 1.25f + 100, &fast, &slow);
		scanout_band_times(band, nb - 1, cw, &slow, &start, &span);
		native_px_us = span - slow.start_us;
		if (lines > NATIVE_H + PORCH_LINES)
			lines += SLOW_SPARE;
		/*
		 * As many refreshes a frame as are slow enough, each as long.
		 * A clip quicker than that is shown as if it had 24 frames a
		 * second, every frame two refreshes: the player leaves the
		 * rest out (lcd_native_every()).
		 */
		for (int goal = frame_us, m = 4; goal > 0 && m >= 1; m--) {
			int l = (int)(goal / m / line_us + 0.5f);

			if (l >= lines && l <= SLOW_MAX && span < m * l * line_us) {
				lines = l;
				break;
			}
			if (m == 1 && goal < SLOWEST_GOAL_US) {
				goal = SLOWEST_GOAL_US;
				m = 5;
			}
		}
		/* never faster than another clip being sent needs */
		if (lines < NATIVE_H + PORCH_LINES + slowed && native_users > 1)
			lines = NATIVE_H + PORCH_LINES + slowed;
		slow_to(lines);
	}
	lcd_bus_lock(false);
	return ok;
}

/*
 * How often frames of the shape last begun can go at most, as the
 * refresh and their cost are now: the whole refreshes one takes, from
 * one start to the next. 0 if no shape was given.
 */
int lcd_native_every(void)
{
	float period = refresh.lines / refresh.lines_per_us;
	float span = native_px_us + overhead_us * 1.25f + 100;
	int n = (int)(span / period);

	return native_px_us > 0 ? (int)((n + (n * period < span)) * period) : 0;
}

/*
 * A frame that no start keeps clear of the refresh, as the refresh is,
 * may be if it is slower: the fewest lines that leave it a window, and a
 * few more for the frames' pace to vary. Only while video is being sent.
 * Bus held.
 */
static bool slow_refresh(const struct scanout_band *band, int nb, int cw)
{
	int now = NATIVE_H + PORCH_LINES + slowed;
	/* not for one frame held up -- the refresh is not sped up again */
	int lines = native_users ? lines_for(band, nb, cw, overhead_us < SLOW_OVERHEAD ?
					       overhead_us : SLOW_OVERHEAD) : -1;

	if (lines <= now)
		return false;
	return !slow_to(lines + SLOW_SPARE);
}

/*
 * The frame's pace as it can be at best, and at worst as frames have
 * gone: the overhead all at the start, so that every band ends as late
 * as it could have.
 */
static void paces(int clock, float overhead, struct scanout_pace *fast,
		  struct scanout_pace *slow)
{
	fast->bytes_per_us = clock / 8e6f;
	fast->band_us = 8;
	fast->start_us = 40;
	*slow = *fast;
	slow->start_us = overhead;
}

/*
 * A rectangle of the panel in its own order: columns c0 to c0 + cw - 1 of
 * rows p0 to p0 + ph - 1, the buffer holding it row after row -- sent so
 * that no refresh meets it half written. Only the picture is sent:
 * whatever is round it was painted once and stays. Returns -ENOTSUP where
 * the refresh cannot be read, -EAGAIN for a frame that went out but took
 * longer than its window allowed (it may have shown a seam).
 */
int lcd_draw_native(const uint8_t *rgb565be, int c0, int cw, int p0, int ph)
{
	const uint8_t col[4] = { c0 >> 8, c0, (c0 + cw - 1) >> 8, c0 + cw - 1 };
	uint8_t back = ili9341_madctl(), mode = back & (MADCTL_MY | MADCTL_BGR);
	/* one frame at a time, under the bus lock: off the caller's small stack */
	static struct scanout_band band[SCANOUT_MAX_BANDS];
	static struct lcd_io_step step[3 + 2 * SCANOUT_MAX_BANDS];
	static uint8_t rows[SCANOUT_MAX_BANDS][4];
	struct scanout_pace fast, slow;
	int nb, ns = 0, line, ret = 0, nw, err;
	UBaseType_t prio;
	float lo[2], hi[2], wait;
	int64_t read_at, took;

	if (!width || !io)
		return -ENODEV;
	if (c0 < 0 || cw < 1 || c0 + cw > NATIVE_W || p0 < 0 || ph < 1 || p0 + ph > NATIVE_H)
		return -EINVAL;
	lcd_bus_lock(true);
	nb = frame_bands(cw, p0, ph, band);
	if (nb < 0) {
		lcd_bus_lock(false);
		return -EINVAL;
	}
	step[ns++] = (struct lcd_io_step){ CMD_MADCTL, &mode, 1 };
	step[ns++] = (struct lcd_io_step){ CMD_CASET, col, 4 };
	for (int k = 0; k < nb; k++) {
		int lo_row = scan_down ? NATIVE_H - band[k].first - band[k].n : band[k].first;
		int hi_row = lo_row + band[k].n - 1;

		rows[k][0] = lo_row >> 8;
		rows[k][1] = lo_row;
		rows[k][2] = hi_row >> 8;
		rows[k][3] = hi_row;
		step[ns++] = (struct lcd_io_step){ CMD_RASET, rows[k], 4 };
		step[ns++] = (struct lcd_io_step){
			CMD_RAMWR, rgb565be + (size_t)(lo_row - p0) * cw * 2, (size_t)band[k].n * cw * 2,
		};
	}
	step[ns++] = (struct lcd_io_step){ CMD_MADCTL, &back, 1 };

	paces(lcd_io_stream_clock(), overhead_us * 1.25f + 100, &fast, &slow);
	nw = wide(scanout_window(band, nb, cw, &refresh, &fast, &slow, lo, hi), lo, hi);
	if (!nw && slow_refresh(band, nb, cw))
		nw = wide(scanout_window(band, nb, cw, &refresh, &fast, &slow, lo, hi), lo, hi);
	if (!nw) {
		/* Slower than the refresh allows at all: go when the refresh
		 * has left the first band, and count it. */
		lo[0] = band[0].first + band[0].n + PORCH_LINES + MARGIN_LINES;
		if (lo[0] > refresh.lines - 1 - MARGIN_LINES)
			lo[0] = refresh.lines - 1 - MARGIN_LINES;
		hi[0] = lo[0] + MARGIN_LINES;
		nw = 1;
		ret = -EAGAIN;
	}
	/*
	 * From the line read to the first band queued nothing else on this
	 * core may come in: a decoder sharing it at the same priority took a
	 * tick there now and then, and the frame went out late. Above the
	 * programs' tasks, then, till the frame is on its way; it sleeps
	 * through most of the wait.
	 */
	prio = uxTaskPriorityGet(NULL);
	vTaskPrioritySet(NULL, configMAX_PRIORITIES - 4);
	for (int64_t give_up = esp_timer_get_time() + 100000;;) {
		if ((line = scan_line()) < 0) {
			err = -EIO;
			break;
		}
		read_at = esp_timer_get_time();
		wait = scanout_wait(line, &refresh, nw, lo, hi);
		if (wait <= 0) {
			err = lcd_io_stream(step, ns) ? -EIO : 0;
			break;
		}
		if (read_at > give_up) {
			err = -ETIMEDOUT;
			break;
		}
		/* Asleep for most of the way, then the line read until it is time. */
		if (wait / refresh.lines_per_us > SLEEP_US)
			vTaskDelay(pdMS_TO_TICKS((int)(wait / refresh.lines_per_us - 1000) / 1000) ?: 1);
	}
	vTaskPrioritySet(NULL, prio);
	took = esp_timer_get_time() - read_at;
	lcd_bus_lock(false);
	if (err)
		return err;

	/*
	 * What it cost besides the pixels, for the next frame; and whether
	 * this one kept to its window, had all of that come before its first
	 * band: the line it started at, still a line it could have started at.
	 */
	{
		float extra = took - (float)cw * ph * 2 / fast.bytes_per_us;

		if (extra < 0)
			extra = 0;
		overhead_us = extra > overhead_us ? extra : overhead_us * 0.95f + extra * 0.05f;
		if (!ret) {
			paces(lcd_io_stream_clock(), extra, &fast, &slow);
			nw = scanout_window(band, nb, cw, &refresh, &fast, &slow, lo, hi);
			if (scanout_wait(line, &refresh, nw, lo, hi) != 0)
				ret = -EAGAIN;
		}
	}
	return ret;
}

/*
 * One lcd_native_begin() over; when it was the last, the refresh as the
 * panel had it. Without `wait`, false if the bus is busy (a kill's
 * cleanup, which must not block): try again.
 */
bool lcd_native_end(bool wait)
{
	if (!bus_take(wait))
		return false;
	if (native_users && !--native_users && slowed && io) {
		if (slow_by(0))
			klog("lcd: the refresh could not be put back");
		else
			refresh.lines = NATIVE_H + PORCH_LINES;	/* each line as long */
	}
	lcd_bus_lock(false);
	return true;
}

/* A register read back from the panel, for probing. */
int lcd_read_reg(uint8_t cmd, uint8_t *out, int n)
{
	int ret;

	if (!width || !io)
		return -ENODEV;
	lcd_bus_lock(true);
	ret = lcd_io_read(cmd, out, n);
	lcd_bus_lock(false);
	return ret;
}

/*
 * The panel's memory read back, for checking what reached it: the
 * rectangle as lcd_draw_native() writes it, the reply to Memory Read as
 * it comes (a dummy first, then three bytes a pixel). Slow: a frame takes
 * half a second.
 */
#define CMD_RAMRD	0x2e

int lcd_read_native(uint8_t *raw, size_t n, int c0, int cw, int p0, int ph)
{
	const uint8_t col[4] = { c0 >> 8, c0, (c0 + cw - 1) >> 8, c0 + cw - 1 };
	const uint8_t row[4] = { p0 >> 8, p0, (p0 + ph - 1) >> 8, p0 + ph - 1 };
	uint8_t back = ili9341_madctl(), mode = back & (MADCTL_MY | MADCTL_BGR);
	int ret;

	if (!width || !io)
		return -ENODEV;
	if (c0 < 0 || cw < 1 || c0 + cw > NATIVE_W || p0 < 0 || ph < 1 || p0 + ph > NATIVE_H)
		return -EINVAL;
	lcd_bus_lock(true);
	ret = esp_lcd_panel_io_tx_param(io, CMD_MADCTL, &mode, 1) ||
	      esp_lcd_panel_io_tx_param(io, CMD_CASET, col, 4) ||
	      esp_lcd_panel_io_tx_param(io, CMD_RASET, row, 4) ? -EIO : 0;
	if (!ret)
		ret = lcd_io_read_long(CMD_RAMRD, raw, n);
	if (esp_lcd_panel_io_tx_param(io, CMD_MADCTL, &back, 1) && !ret)
		ret = -EIO;
	lcd_bus_lock(false);
	return ret;
}

int lcd_width(void)
{
	return width;
}

int lcd_height(void)
{
	return height;
}

uint8_t *lcd_alloc_buffer(size_t bytes)
{
	return width && io ? lcd_io_alloc(bytes) : NULL;
}

/*
 * Screen capture. Nothing is kept in memory normally -- the panel is the
 * only copy of the picture, which is what makes the terminal cheap -- so
 * a screenshot is taken by remembering the next thing drawn. While a
 * capture is armed every lcd_draw() also copies into this buffer; when it
 * is not, the cost is one pointer test.
 */
static uint8_t	*capture;
static int	 capture_w, capture_h;

int lcd_capture_begin(void)
{
	if (!bus_lock || !width)
		return -ENODEV;
	lcd_bus_lock(true);
	if (capture) {
		lcd_bus_lock(false);
		return -EBUSY;
	}
	capture_w = width;
	capture_h = height;
	capture = heap_caps_calloc(1, (size_t)capture_w * capture_h * 2,
				   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!capture)
		capture = heap_caps_calloc(1, (size_t)capture_w * capture_h * 2, MALLOC_CAP_8BIT);
	int ret = capture ? 0 : -ENOMEM;

	lcd_bus_lock(false);
	return ret;
}

const uint8_t *lcd_capture_pixels(int *w, int *h)
{
	*w = capture_w;
	*h = capture_h;
	return capture;
}

void lcd_capture_end(void)
{
	if (!bus_lock)
		return;
	lcd_bus_lock(true);
	heap_caps_free(capture);
	capture = NULL;
	lcd_bus_lock(false);
}

int lcd_capture_try_end(void)
{
	if (!bus_lock)
		return -ENODEV;
	if (!xSemaphoreTake(bus_lock, 0))
		return -EAGAIN;
	heap_caps_free(capture);
	capture = NULL;
	xSemaphoreGive(bus_lock);
	return 0;
}

static void capture_rect(int x, int y, int w, int h, const uint8_t *rgb565be)
{
	for (int row = 0; row < h; row++) {
		int dy = y + row;

		if (dy < 0 || dy >= capture_h)
			continue;
		for (int col = 0; col < w; col++) {
			int dx = x + col;
			size_t to, from;

			if (dx < 0 || dx >= capture_w)
				continue;
			to = ((size_t)dy * capture_w + dx) * 2;
			from = ((size_t)row * w + col) * 2;
			capture[to] = rgb565be[from];
			capture[to + 1] = rgb565be[from + 1];
		}
	}
}

/* A rectangle's commands, and its pixels queued to follow. Bus held. */
static void draw_queue(int x, int y, int w, int h, const uint8_t *rgb565be)
{
	const uint8_t col[4] = { x >> 8, x, (x + w - 1) >> 8, x + w - 1 };
	const uint8_t row[4] = { y >> 8, y, (y + h - 1) >> 8, y + h - 1 };

	if (capture)
		capture_rect(x, y, w, h, rgb565be);
	esp_lcd_panel_io_tx_param(io, CMD_CASET, col, 4);
	esp_lcd_panel_io_tx_param(io, CMD_RASET, row, 4);
	lcd_wait_done(0);			/* clear a stale completion */
	esp_lcd_panel_io_tx_color(io, CMD_RAMWR, rgb565be, (size_t)w * h * 2);
	drawing = true;
}

void lcd_draw(int x, int y, int w, int h, const uint8_t *rgb565be)
{
	if (!width || !io)
		return;
	lcd_bus_lock(true);
	draw_queue(x, y, w, h, rgb565be);
	draw_settle();
	lcd_bus_lock(false);
}

/*
 * lcd_draw() that returns as soon as the pixels are on their way: the
 * caller gets on with the next picture meanwhile, and keeps this one's
 * buffer as it is until lcd_draw_wait() says they are out. Anything
 * else that uses the panel waits for them first.
 */
void lcd_draw_start(int x, int y, int w, int h, const uint8_t *rgb565be)
{
	if (!width || !io)
		return;
	lcd_bus_lock(true);
	draw_queue(x, y, w, h, rgb565be);
	lcd_bus_lock(false);
}

/* Whether lcd_draw_start()'s pixels are out, waiting up to `ms` for them. */
bool lcd_draw_wait(int ms)
{
	bool out;

	if (!bus_lock)
		return true;
	if (!xSemaphoreTake(bus_lock, pdMS_TO_TICKS(ms)))
		return false;			/* someone else is drawing */
	if (drawing && xSemaphoreTake(done, pdMS_TO_TICKS(ms)))
		drawing = false;
	out = !drawing;
	xSemaphoreGive(bus_lock);
	return out;
}

/*
 * A rectangle of one colour. The colour is laid out once in PSRAM, on a
 * cache line so the DMA can read it from there, and sent as many times
 * as it takes: a whole screen asked for 32 KB of internal RAM, which a
 * game started while music played did not have, and the fill failed
 * without a word, leaving the terminal showing round the picture.
 */
void lcd_fill(int x, int y, int w, int h, uint16_t rgb565)
{
	if (!width || !io || w <= 0 || h <= 0)
		return;
	int rows_per_chunk = lcd_io_max_transfer() / 2 / w;
	if (rows_per_chunk > h)
		rows_per_chunk = h;
	if (rows_per_chunk < 1)
		return;				/* wider than one transfer */
	size_t bytes = ((size_t)w * rows_per_chunk * 2 + 63) & ~(size_t)63;
	uint8_t *buf = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

	if (!buf)
		buf = lcd_alloc_buffer((size_t)w * rows_per_chunk * 2);
	if (!buf) {
		klog("lcd: no memory to fill %dx%d", w, h);
		return;
	}
	for (int i = 0; i < w * rows_per_chunk; i++) {
		buf[2 * i] = rgb565 >> 8;
		buf[2 * i + 1] = rgb565;
	}
	for (int row = 0; row < h; row += rows_per_chunk) {
		int n = h - row < rows_per_chunk ? h - row : rows_per_chunk;
		lcd_draw(x, y + row, w, n, buf);
	}
	heap_caps_free(buf);
}

#else /* !CONFIG_PT_LCD */

int lcd_init(void) { return -ENODEV; }
int lcd_width(void) { return 0; }
int lcd_height(void) { return 0; }
uint8_t *lcd_alloc_buffer(size_t bytes) { return NULL; }
void lcd_draw(int x, int y, int w, int h, const uint8_t *rgb565be) { }
void lcd_draw_start(int x, int y, int w, int h, const uint8_t *rgb565be) { }
bool lcd_draw_wait(int ms) { return true; }
void lcd_fill(int x, int y, int w, int h, uint16_t rgb565) { }
void lcd_backlight_set(int percent) { }
int lcd_backlight_get(void) { return 0; }
void lcd_sleep(void) { }
int lcd_set_rotation(int r) { return -ENODEV; }
int lcd_backlight_now(void) { return 0; }
void lcd_light(int percent) { }
void lcd_panel_power(bool on) { }
bool lcd_panel_on(void) { return false; }
int lcd_rotation(void) { return 0; }
int lcd_set_clock(int hz) { return -ENODEV; }
int lcd_clock(void) { return 0; }
int lcd_read_reg(uint8_t cmd, uint8_t *out, int n) { return -ENODEV; }
bool lcd_native_begin(int cw, int p0, int ph, int frame_us) { return false; }
int lcd_native_every(void) { return 0; }
bool lcd_native_end(bool wait) { return true; }
int lcd_draw_native(const uint8_t *rgb565be, int c0, int cw, int p0, int ph) { return -ENODEV; }
int lcd_read_native(uint8_t *raw, size_t n, int c0, int cw, int p0, int ph) { return -ENODEV; }
int lcd_capture_begin(void) { return -ENODEV; }
const uint8_t *lcd_capture_pixels(int *w, int *h) { *w = *h = 0; return NULL; }
void lcd_capture_end(void) { }
int lcd_capture_try_end(void) { return -ENODEV; }

#endif
