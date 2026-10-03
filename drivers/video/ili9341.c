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

void lcd_bus_lock(bool take)
{
	if (take)
		xSemaphoreTake(bus_lock, portMAX_DELAY);
	else
		xSemaphoreGive(bus_lock);
}

void lcd_wait_done(int ms)
{
	xSemaphoreTake(done, pdMS_TO_TICKS(ms));
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
void lcd_light(int percent) { }

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
 * The bus clock. The panel is rated for far less than the 80 MHz this
 * board runs it at; if pictures come out with streaks or wrong colours,
 * a slower clock tells whether the wires are why.
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
 * going in, and the old one on the other: in a landscape frame, several
 * slanting seams that move (a frame takes about 16 ms to send).
 * The panel's TE pin, which would say when a refresh starts, is not
 * wired on this board; its data line is, and the panel can be asked
 * which line it is refreshing (Get Scanline, 0x45).
 *
 * So a frame goes out in the panel's own order, in bands of rows, and a
 * band only once the refresh has passed it. With the slower panel clock,
 * an early start and aligned 32-row bands, bands reach the panel before
 * the next refresh reads them. An unusually late transfer finishes its frame
 * and reports the missed window, rather than leaving half an old image.
 *
 * Two things this panel does that it had to be shown: the row order bit
 * (MY) turns the refresh round as well as the writing, so it is never
 * changed between frames, only the row/column exchange is; and with MY
 * set the refresh runs from the last row of the frame to the first.
 */
#define NATIVE_W	240		/* the panel as it is built */
#define NATIVE_H	320
#define CMD_SCANLINE	0x45
#define PORCH_LINES	4		/* sync and back porch, before row 0 */
#define START_LINE	48		/* leave a whole refresh's headroom for preemption */
#define BEHIND		4		/* rows kept between the refresh and a band */
#define SCAN_GAP_US	10000		/* a long pause can hide a refresh wrap */
/*
 * The actual refresh rate depends on the panel. A wait longer than this
 * many rows sleeps instead of taking CPU time from the decoder; shorter
 * waits poll so that a millisecond tick cannot skip their whole window.
 */
#define SLEEP_ROWS	30

static bool scan_down = true;		/* with MY set; `lcdtest dir` turns it */

void lcd_native_order(bool upwards)
{
	scan_down = upwards;
}

bool lcd_native_upwards(void)
{
	return scan_down;
}

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

bool lcd_native_ok(void)
{
	bool ok;

	if (!width || !io)
		return false;
	lcd_bus_lock(true);
	ok = width == NATIVE_H && height == NATIVE_W && scan_line() >= 0;
	lcd_bus_lock(false);
	return ok;
}

/*
 * The refresh's rows [first, first + n) of a rectangle: panel rows p0 to
 * p0 + ph - 1, each `cw` pixels (the columns CASET has set), laid out in
 * the buffer by panel row. Bus held.
 */
static int send_band(const uint8_t *buf, int p0, int cw, int first, int n)
{
	int lo = scan_down ? NATIVE_H - first - n : first, hi = lo + n - 1;
	const uint8_t row[4] = { lo >> 8, lo, hi >> 8, hi };

	if (esp_lcd_panel_io_tx_param(io, CMD_RASET, row, 4))
		return -EIO;
	lcd_wait_done(0);			/* clear a stale completion */
	if (esp_lcd_panel_io_tx_color(io, CMD_RAMWR, buf + (size_t)(lo - p0) * cw * 2,
				    (size_t)n * cw * 2))
		return -EIO;
	return xSemaphoreTake(done, pdMS_TO_TICKS(100)) ? 0 : -ETIMEDOUT;
}

/*
 * A rectangle of the panel in its own portrait order -- columns c0 to
 * c0 + cw - 1 of rows p0 to p0 + ph - 1, as MADCTL addresses them, the
 * buffer holding it row after row -- sent behind the refresh. Only the
 * picture is sent: whatever is round it was painted once and stays.
 * Returns -ENOTSUP where the refresh cannot be read, -EAGAIN for a frame
 * completed after its refresh window. A late frame is finished rather
 * than leaving the panel with a mixture of two frames until the next one.
 */
int lcd_draw_native(const uint8_t *rgb565be, int c0, int cw, int p0, int ph)
{
	const uint8_t col[4] = { c0 >> 8, c0, (c0 + cw - 1) >> 8, (c0 + cw - 1) & 0xff };
	uint8_t back = ili9341_madctl(), mode = back & (MADCTL_MY | MADCTL_BGR);
	/* the rectangle's rows, counted in the order the refresh meets them */
	int done = scan_down ? NATIVE_H - p0 - ph : p0, end = done + ph;
	int line, last, ret = 0;
	int64_t deadline, polled;
	bool lapped = false, late = false;

	if (!width || !io)
		return -ENODEV;
	if (c0 < 0 || cw < 1 || c0 + cw > NATIVE_W || p0 < 0 || ph < 1 || p0 + ph > NATIVE_H)
		return -EINVAL;
	lcd_bus_lock(true);
	line = scan_line();
	if (line < 0) {
		lcd_bus_lock(false);
		return -ENOTSUP;
	}
	/* A late start left only a few milliseconds for a busy Wi-Fi task
	 * or a DMA bounce. Always start near the top of a refresh instead. */
	deadline = esp_timer_get_time() + 100000;
	while (line > START_LINE) {
		if (esp_timer_get_time() >= deadline) {
			ret = -ETIMEDOUT;
			goto out;
		}
		if (line < NATIVE_H - SLEEP_ROWS)
			vTaskDelay(1);
		else
			esp_rom_delay_us(30);
		line = scan_line();
		if (line < 0) {
			ret = -EIO;
			goto out;
		}
	}
	if (esp_lcd_panel_io_tx_param(io, CMD_MADCTL, &mode, 1) ||
	    esp_lcd_panel_io_tx_param(io, CMD_CASET, col, 4)) {
		ret = -EIO;
		goto restore;
	}
	last = line;
	polled = esp_timer_get_time();
	while (done < end) {
		int passed, n = scanout_band_rows(cw, end - done, scan_down);
		int64_t now;

		line = scan_line();
		now = esp_timer_get_time();
		if (line < 0 || now >= deadline) {
			ret = line < 0 ? -EIO : -ETIMEDOUT;
			break;
		}
		if (now - polled >= SCAN_GAP_US)
			late = true;		/* line alone cannot reveal a skipped whole sweep */
		polled = now;
		if (line < last) {
			if (lapped)
				late = true;
			lapped = true;		/* a new refresh: all of the last one is passed */
		}
		last = line;
		passed = lapped ? NATIVE_H : line - PORCH_LINES - BEHIND;
		/* A whole aligned band, after its last row has been scanned.
		 * Variable row counts made the next PSRAM DMA pointer unaligned,
		 * allocating a bounce buffer in scarce internal RAM mid-frame. */
		if (passed < done + n) {
			if (done + n - passed > SLEEP_ROWS)
				vTaskDelay(1);
			else
				esp_rom_delay_us(50);
			continue;
		}
		/* The second sweep must not have reached rows still unwritten. */
		if (lapped && line - PORCH_LINES + BEHIND >= done)
			late = true;
		if ((ret = send_band(rgb565be, p0, cw, done, n)))
			break;
		done += n;
		/* There is no next iteration to check the final DMA completion. */
		if (done == end) {
			line = scan_line();
			if (line < 0) {
				ret = -EIO;
				break;
			}
			if (esp_timer_get_time() - polled >= SCAN_GAP_US)
				late = true;
			if (line < last)
				lapped = true;
			if (lapped && line - PORCH_LINES + BEHIND >= done - n)
				late = true;
		}
	}
	if (!ret && late)
		ret = -EAGAIN;
restore:
	if (esp_lcd_panel_io_tx_param(io, CMD_MADCTL, &back, 1) && (!ret || ret == -EAGAIN))
		ret = -EIO;
out:
	lcd_bus_lock(false);
	return ret;
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

void lcd_draw(int x, int y, int w, int h, const uint8_t *rgb565be)
{
	const uint8_t col[4] = { x >> 8, x, (x + w - 1) >> 8, x + w - 1 };
	const uint8_t row[4] = { y >> 8, y, (y + h - 1) >> 8, y + h - 1 };

	if (!width || !io)
		return;
	lcd_bus_lock(true);
	if (capture)
		capture_rect(x, y, w, h, rgb565be);
	esp_lcd_panel_io_tx_param(io, CMD_CASET, col, 4);
	esp_lcd_panel_io_tx_param(io, CMD_RASET, row, 4);
	lcd_wait_done(0);			/* clear a stale completion */
	esp_lcd_panel_io_tx_color(io, CMD_RAMWR, rgb565be, (size_t)w * h * 2);
	lcd_wait_done(1000);
	lcd_bus_lock(false);
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
bool lcd_native_ok(void) { return false; }
void lcd_native_order(bool upwards) { }
bool lcd_native_upwards(void) { return false; }
int lcd_draw_native(const uint8_t *rgb565be, int c0, int cw, int p0, int ph) { return -ENODEV; }
int lcd_capture_begin(void) { return -ENODEV; }
const uint8_t *lcd_capture_pixels(int *w, int *h) { *w = *h = 0; return NULL; }
void lcd_capture_end(void) { }
int lcd_capture_try_end(void) { return -ENODEV; }

#endif
