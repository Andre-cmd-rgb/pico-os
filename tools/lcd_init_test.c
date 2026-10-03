/* Exercise production startup; an optional SPI reader may fail independently. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ili9341.h"

typedef int esp_err_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_panel_io_color_trans_done_cb_t;
typedef void *SemaphoreHandle_t;
#define GPIO_MODE_OUTPUT 1
#define pdMS_TO_TICKS(n) (n)
#define portMAX_DELAY -1
#ifndef CONFIG_PT_LCD_INIT_ALT
#define CONFIG_PT_LCD_INIT_ALT 1
#endif
#define CONFIG_PT_LCD_INVERT 1
static int calls, fail_at, resources, optional_reader_call, backlights, delays;
static bool fail_delete, fail_bus_delete, fail_command, locked;
#ifndef TEST_I80
static bool fail_reader_delete;
#endif
static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t done, bus_lock;
static int width, height;
static bool panel_awake;
static int rotation = 3;
static void *on_color_done = (void *)1;

static bool fails(void) { return ++calls == fail_at; }
static void *allocate(void)
{
	void *p = malloc(1);

	assert(p);
	resources++;
	return p;
}
static void release(void *p)
{
	assert(p && resources > 0);
	resources--;
	free(p);
}
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "injected error"; }
static void klog(const char *fmt, ...) { (void)fmt; }
static void vTaskDelay(int ticks) { delays += ticks; }
static int gpio_set_direction(int gpio, int mode)
{
	assert(gpio >= 0 && mode == GPIO_MODE_OUTPUT);
	return fails() ? 1 : 0;
}
static int gpio_set_level(int gpio, int level)
{
	assert(gpio >= 0 && (level == 0 || level == 1));
	return fails() ? 1 : 0;
}
static void *xSemaphoreCreateBinary(void) { return fails() ? NULL : allocate(); }
static void *xSemaphoreCreateMutex(void) { return fails() ? NULL : allocate(); }
static void vSemaphoreDelete(void *sem) { assert(!io); release(sem); }
void lcd_bus_lock(bool take)
{
	assert(bus_lock && take != locked);
	locked = take;
}
static int esp_lcd_panel_io_del(void *panel)
{
	assert(panel);
	if (fail_delete)
		return 1;
	release(panel);
	return 0;
}
static int lcd_io_close(esp_lcd_panel_io_handle_t *panel);

#ifdef TEST_I80

#define MAX_TRANSFER 16384
#define LCD_CLK_SRC_DEFAULT 1
#define CONFIG_PT_LCD_RS 1
#define CONFIG_PT_LCD_WR 42
#define CONFIG_PT_LCD_D0 4
#define CONFIG_PT_LCD_D1 5
#define CONFIG_PT_LCD_D2 6
#define CONFIG_PT_LCD_D3 7
#define CONFIG_PT_LCD_D4 15
#define CONFIG_PT_LCD_D5 16
#define CONFIG_PT_LCD_D6 17
#define CONFIG_PT_LCD_D7 18
#define CONFIG_PT_LCD_CS 2
#define CONFIG_PT_LCD_PCLK_HZ 2000000
#ifndef CONFIG_PT_LCD_RD
#define CONFIG_PT_LCD_RD -1
#endif
typedef void *esp_lcd_i80_bus_handle_t;
typedef struct {
	int dc_gpio_num, wr_gpio_num, clk_src, data_gpio_nums[8], bus_width;
	int max_transfer_bytes, dma_burst_size;
} esp_lcd_i80_bus_config_t;
typedef struct {
	int cs_gpio_num, pclk_hz, trans_queue_depth, lcd_cmd_bits, lcd_param_bits;
	void *on_color_trans_done;
	struct { int dc_idle_level, dc_cmd_level, dc_dummy_level, dc_data_level; } dc_levels;
} esp_lcd_panel_io_i80_config_t;
static esp_lcd_i80_bus_handle_t bus;
static esp_lcd_panel_io_handle_t bus_io;
static int esp_lcd_new_i80_bus(const esp_lcd_i80_bus_config_t *cfg, void **out)
{
	assert(!bus && cfg->bus_width == 8 && cfg->dma_burst_size == 64);
	if (fails())
		return 1;
	*out = allocate();
	return 0;
}
static int esp_lcd_new_panel_io_i80(void *handle, const esp_lcd_panel_io_i80_config_t *cfg,
				  void **out)
{
	assert(handle == bus && cfg->on_color_trans_done == on_color_done);
	if (fails())
		return 1;
	*out = allocate();
	return 0;
}
static int esp_lcd_del_i80_bus(void *handle)
{
	assert(handle == bus);
	if (fail_bus_delete)
		return 1;
	release(handle);
	return 0;
}
static int gpio_reset_pin(int gpio) { assert(gpio >= 0); return fails() ? 1 : 0; }
#include "i80_under_test.h"
static int lcd_io_reset_gpio(void) { return 41; }
static int lcd_io_set_clock(void **panel, int hz) { (void)panel; (void)hz; return -ENOTSUP; }

#else

#define MAX_TRANSFER 32768
#define HOST 1
#define SPI_DMA_CH_AUTO 0
#define SPI_DEVICE_HALFDUPLEX 1
#define CONFIG_PT_LCD_SCLK 12
#define CONFIG_PT_LCD_MOSI 11
#ifndef CONFIG_PT_LCD_MISO
#define CONFIG_PT_LCD_MISO 13
#endif
#define CONFIG_PT_LCD_SPI_DC 46
#define CONFIG_PT_LCD_SPI_CS 10
#define CONFIG_PT_LCD_SPI_HOST 2
#define CONFIG_PT_LCD_SPI_HZ 80000000
#ifndef CONFIG_PT_LCD_SPI_RST
#define CONFIG_PT_LCD_SPI_RST -1
#endif
typedef int esp_lcd_spi_bus_handle_t;
typedef void *spi_device_handle_t;
typedef struct {
	int sclk_io_num, mosi_io_num, miso_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz;
} spi_bus_config_t;
typedef struct {
	int dc_gpio_num, cs_gpio_num, pclk_hz, spi_mode, trans_queue_depth, lcd_cmd_bits, lcd_param_bits;
	void *on_color_trans_done;
	struct { unsigned psram_dma_direct; } flags;
} esp_lcd_panel_io_spi_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num, queue_size, flags; } spi_device_interface_config_t;
static esp_lcd_panel_io_spi_config_t panel_cfg;
static spi_device_handle_t reader;
static bool bus_open, bus_resource;
static int clock_hz = 40000000;
static int spi_bus_initialize(int host, const spi_bus_config_t *cfg, int dma)
{
	assert(host == HOST && !bus_resource && cfg->max_transfer_sz == MAX_TRANSFER && dma == SPI_DMA_CH_AUTO);
	if (fails())
		return 1;
	resources++;
	bus_resource = true;
	return 0;
}
static int spi_bus_free(int host)
{
	assert(host == HOST && bus_resource && !io && !reader);
	if (fail_bus_delete)
		return 1;
	resources--;
	bus_resource = false;
	return 0;
}
static int esp_lcd_new_panel_io_spi(int host, const esp_lcd_panel_io_spi_config_t *cfg, void **out)
{
	assert(host == HOST && bus_resource && cfg->pclk_hz == 80000000 && cfg->flags.psram_dma_direct);
	assert(cfg->on_color_trans_done == on_color_done);
	if (fails())
		return 1;
	*out = allocate();
	return 0;
}
static void gpio_sleep_sel_dis(int gpio) { (void)gpio; }
#if CONFIG_PT_LCD_MISO >= 0
static int spi_bus_add_device(int host, const spi_device_interface_config_t *cfg, void **out)
{
	assert(host == HOST && cfg->clock_speed_hz == 4000000 && cfg->spics_io_num == -1);
	optional_reader_call = calls + 1;
	if (fails())
		return 1;
	*out = allocate();
	return 0;
}
#endif
static int spi_bus_remove_device(void *handle)
{
	assert(handle == reader);
	if (fail_reader_delete)
		return 1;
	release(handle);
	return 0;
}
#define READ_HZ 4000000
/* lcd_io_stream()'s device is optional and not exercised here. */
static spi_device_handle_t streamer;
static void __attribute__((unused)) stream_add(void) { }
#include "spi_under_test.h"
static int lcd_io_reset_gpio(void) { return CONFIG_PT_LCD_SPI_RST; }
static int lcd_io_set_clock(void **panel, int hz) { assert(panel && hz); return 0; }
#endif

static const char *lcd_io_name(void) { return "mock bus"; }
static int lcd_io_read(uint8_t cmd, uint8_t *out, int n)
{
	assert(locked && io && out && n > 0);
	*out = cmd;
	return 0;
}
uint8_t ili9341_madctl(void) { return 0xe8; }
static int esp_lcd_panel_io_tx_param(void *panel, int cmd, const void *data, int n)
{
	assert(panel == io && io && done && bus_lock);
	assert(cmd > 0 && (n == 0 || data));
	if (fail_command)
		return 1;
	return fails() ? 1 : 0;
}
static void backlight_init(void)
{
	assert(width == 320 && height == 240 && io && panel_awake);
	backlights++;
}

#include "lcd_init_under_test.h"

static void dispose(void)
{
	fail_delete = fail_bus_delete = false;
#ifndef TEST_I80
	fail_reader_delete = false;
#endif
	assert(!lcd_io_close(&io));
	if (done)
		vSemaphoreDelete(done);
	if (bus_lock)
		vSemaphoreDelete(bus_lock);
	done = bus_lock = NULL;
	width = height = 0;
	panel_awake = false;
	backlights = 0;
	assert(!resources && !locked);
}

static void failed_bus_cleanup(void)
{
	fail_command = fail_bus_delete = true;
	assert(lcd_init() == -EIO && !io && done && bus_lock && !width && !panel_awake);
	fail_command = false;
	assert(lcd_init() == -EIO && done && bus_lock);
	fail_bus_delete = false;
	assert(lcd_init() == 0 && backlights == 1);
	dispose();
}

int main(void)
{
	int startup_calls, reader_call;
	uint8_t byte;

	assert(lcd_init() == 0 && width == 320 && height == 240 && lcd_panel_on());
#ifndef TEST_I80
	assert(clock_hz == CONFIG_PT_LCD_SPI_HZ);
#endif
	startup_calls = calls;
	reader_call = optional_reader_call;
	assert(lcd_init() == 0 && calls == startup_calls && backlights == 1);
	dispose();
	for (fail_at = 1; fail_at <= startup_calls; fail_at++) {
		int ret;

		calls = optional_reader_call = 0;
		ret = lcd_init();
		if (fail_at == reader_call) {
			assert(!ret && backlights == 1);
			dispose();
		} else {
			assert(ret < 0 && !io && !width && !height && !panel_awake && !backlights);
			assert(!done && !bus_lock && !resources);
			assert(lcd_read_reg(0x0a, &byte, 1) == -ENODEV);
			assert(lcd_set_rotation(3) == -ENODEV && lcd_set_clock(40000000) == -ENODEV);
		}
	}
	/* A failed SDK deletion keeps its callback resources alive for a retry. */
	fail_at = calls = optional_reader_call = 0;
	fail_command = fail_delete = true;
	assert(lcd_init() == -EIO && io && done && bus_lock && !width && !panel_awake);
	fail_delete = fail_command = false;
	assert(lcd_init() == 0 && backlights == 1);
	/* Power state is published only after a successful command and delay. */
	fail_command = true;
	lcd_panel_power(false);
	assert(lcd_panel_on() && !locked);
	fail_command = false;
	lcd_panel_power(false);
	assert(!lcd_panel_on());
	fail_command = true;
	lcd_panel_power(true);
	assert(!lcd_panel_on() && !locked);
	fail_command = false;
	lcd_panel_power(true);
	assert(lcd_panel_on());
	dispose();
	failed_bus_cleanup();
#if !defined(TEST_I80) && CONFIG_PT_LCD_MISO >= 0
	fail_command = fail_reader_delete = true;
	assert(lcd_init() == -EIO && !io && reader && done && bus_lock && !width);
	fail_command = false;
	assert(lcd_init() == -EIO && reader && done && bus_lock);
	fail_reader_delete = false;
	assert(lcd_init() == 0 && backlights == 1);
	dispose();
#endif
	puts("LCD startup: bus/allocation/reset/command failures unwind, retries and power state passed");
	return 0;
}
