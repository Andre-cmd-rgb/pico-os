/*
 * The panel on an SPI bus: four wires (clock, data, chip select, data/command)
 * plus a backlight, the usual wiring when the display is part of the board.
 * Fast and, unlike the parallel bus, not sensitive to wire length.
 */
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "lcd_io.h"

#if CONFIG_PT_LCD_ILI9341_SPI

/* One screenful of a 53x24 console row is 6 KB; 32 KB leaves room to spare. */
#define MAX_TRANSFER	32768

int lcd_io_open(esp_lcd_panel_io_handle_t *io, esp_lcd_panel_io_color_trans_done_cb_t done)
{
	const spi_bus_config_t bus = {
		.sclk_io_num = CONFIG_PT_LCD_SCLK,
		.mosi_io_num = CONFIG_PT_LCD_MOSI,
		.miso_io_num = -1,		/* the panel is only written to */
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = MAX_TRANSFER,
	};
	const esp_lcd_panel_io_spi_config_t cfg = {
		.dc_gpio_num = CONFIG_PT_LCD_SPI_DC,
		.cs_gpio_num = CONFIG_PT_LCD_SPI_CS,
		.pclk_hz = CONFIG_PT_LCD_SPI_HZ,
		.spi_mode = 0,
		.trans_queue_depth = 8,
		.lcd_cmd_bits = 8,
		.lcd_param_bits = 8,
		.on_color_trans_done = done,
	};
	esp_err_t err = spi_bus_initialize(CONFIG_PT_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);

	if (err) {
		klog("lcd: SPI%d bus setup failed (%s)", CONFIG_PT_LCD_SPI_HOST, esp_err_to_name(err));
		return -EIO;
	}
	err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CONFIG_PT_LCD_SPI_HOST, &cfg, io);
	if (err) {
		klog("lcd: SPI panel setup failed (%s)", esp_err_to_name(err));
		return -EIO;
	}
	return 0;
}

uint8_t *lcd_io_alloc(size_t bytes)
{
	return heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t lcd_io_max_transfer(void)
{
	return MAX_TRANSFER;
}

int lcd_io_reset_gpio(void)
{
	return CONFIG_PT_LCD_SPI_RST;
}

const char *lcd_io_name(void)
{
	static char name[24];

	snprintf(name, sizeof(name), "SPI%d at %d MHz", CONFIG_PT_LCD_SPI_HOST,
		 CONFIG_PT_LCD_SPI_HZ / 1000000);
	return name;
}

#endif /* CONFIG_PT_LCD_ILI9341_SPI */
