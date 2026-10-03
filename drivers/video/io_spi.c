/*
 * The panel on an SPI bus: four wires (clock, data, chip select, data/command)
 * plus a backlight, the usual wiring when the display is part of the board.
 * Fast and, unlike the parallel bus, not sensitive to wire length.
 */
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_private/gpio.h"
#include "soc/spi_periph.h"
#include "esp_lcd_panel_io.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "lcd_io.h"

#if CONFIG_PT_LCD_ILI9341_SPI

/* One screenful of a 53x24 console row is 6 KB; 32 KB leaves room to spare. */
#define MAX_TRANSFER	32768

#define READ_HZ		4000000		/* the panel's reads are slow */
/*
 * The board file says "2" or "3" for the controller's name; ESP-IDF numbers
 * them from SPI1, so SPI2 is 1. Passed straight through, a 2 was SPI3,
 * which has no pins of its own and goes through the GPIO matrix -- rated
 * for 40 MHz, and driven at 80. GPIO 10-13 are SPI2's own pins.
 */
#define HOST		(CONFIG_PT_LCD_SPI_HOST == 3 ? SPI3_HOST : SPI2_HOST)

static esp_lcd_panel_io_spi_config_t panel_cfg;
static spi_device_handle_t reader;
static bool bus_open;
static int clock_hz = CONFIG_PT_LCD_SPI_HZ;

/*
 * The panel's clock, changed while running: the old handle goes and a
 * new one comes on the same bus. Only with nothing being sent (the
 * caller holds the bus).
 */
int lcd_io_set_clock(esp_lcd_panel_io_handle_t *io, int hz)
{
	esp_lcd_panel_io_spi_config_t cfg = panel_cfg;

	cfg.pclk_hz = hz;
	if (esp_lcd_panel_io_del(*io) ||
	    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)HOST, &cfg, io)) {
		*io = NULL;
		return -EIO;
	}
	clock_hz = hz;
	panel_cfg = cfg;
	return 0;
}

int lcd_io_clock(void)
{
	return clock_hz;
}

int lcd_io_open(esp_lcd_panel_io_handle_t *io, esp_lcd_panel_io_color_trans_done_cb_t done)
{
	const spi_bus_config_t bus = {
		.sclk_io_num = CONFIG_PT_LCD_SCLK,
		.mosi_io_num = CONFIG_PT_LCD_MOSI,
		.miso_io_num = CONFIG_PT_LCD_MISO,	/* for reading its registers */
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
		/*
		 * A picture in PSRAM goes to the panel from where it is.
		 * Without this the driver copies every 32 KB of it into a
		 * fresh internal buffer first, and a full screen needs more
		 * of those than there is internal memory: the first piece
		 * arrives and the rest never does. The source must be
		 * aligned to a cache line (canvas.c sees to that).
		 */
		.flags.psram_dma_direct = 1,
	};
	esp_err_t err;

	if (bus_open || reader || *io)
		return -EBUSY;
	err = spi_bus_initialize(HOST, &bus, SPI_DMA_CH_AUTO);

	if (err) {
		klog("lcd: SPI%d bus setup failed (%s)", CONFIG_PT_LCD_SPI_HOST, esp_err_to_name(err));
		return -EIO;
	}
	bus_open = true;
	panel_cfg = cfg;
	err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)HOST, &cfg, io);
	if (err) {
		klog("lcd: SPI panel setup failed (%s)", esp_err_to_name(err));
		lcd_io_close(io);
		return -EIO;
	}
	clock_hz = cfg.pclk_hz;
	/*
	 * The bus keeps its levels through light sleep: isolated, a floating
	 * chip select and clock could clock noise into the panel.
	 */
	gpio_sleep_sel_dis(CONFIG_PT_LCD_SPI_CS);
	gpio_sleep_sel_dis(CONFIG_PT_LCD_SPI_DC);
	gpio_sleep_sel_dis(CONFIG_PT_LCD_SCLK);
	gpio_sleep_sel_dis(CONFIG_PT_LCD_MOSI);
#if CONFIG_PT_LCD_MISO >= 0
	/*
	 * A second device on the bus for reading, slowly: the panel answers
	 * at a few MHz, not at the 80 it is written at. Its chip select is
	 * the writer's pin, taken by hand for the moment of a read.
	 */
	const spi_device_interface_config_t rd = {
		.clock_speed_hz = READ_HZ,
		.mode = 0,
		.spics_io_num = -1,
		.queue_size = 1,
		.flags = SPI_DEVICE_HALFDUPLEX,
	};

	if (spi_bus_add_device(HOST, &rd, &reader))
		reader = NULL;
#endif
	return 0;
}

int lcd_io_close(esp_lcd_panel_io_handle_t *io)
{
	esp_err_t err;

	if (*io) {
		err = esp_lcd_panel_io_del(*io);
		if (err)
			goto fail;
		*io = NULL;
	}
	if (reader) {
		err = spi_bus_remove_device(reader);
		if (err)
			goto fail;
		reader = NULL;
	}
	if (bus_open) {
		err = spi_bus_free(HOST);
		if (err)
			goto fail;
		bus_open = false;
	}
	return 0;
fail:
	klog("lcd: SPI cleanup failed (%s)", esp_err_to_name(err));
	return -EIO;
}

/*
 * A register read: the command with D/C low, then `n` bytes back. The
 * caller holds the bus, so nothing else is being sent; chip select is
 * taken from the SPI controller for the read and given back after.
 */
int lcd_io_read(uint8_t cmd, uint8_t *out, int n)
{
	spi_transaction_t t = { .length = 8, .flags = SPI_TRANS_USE_TXDATA, .tx_data = { cmd } };
	spi_transaction_t r = { .rxlength = n * 8, .rx_buffer = out };
	esp_err_t err;

	if (!reader || n < 1 || n > 4)
		return -ENOTSUP;
	/* The colour callback runs before the SPI ISR releases the writer.
	 * Own the bus before changing either pin, including on another core. */
	if (spi_device_acquire_bus(reader, portMAX_DELAY))
		return -EIO;
	gpio_func_sel(CONFIG_PT_LCD_SPI_CS, PIN_FUNC_GPIO);
	gpio_set_direction(CONFIG_PT_LCD_SPI_CS, GPIO_MODE_OUTPUT);
	/* ESP-IDF disables D/C output after colour DMA. Setting its level
	 * alone then leaves the panel unable to recognise this command. */
	gpio_set_direction(CONFIG_PT_LCD_SPI_DC, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_PT_LCD_SPI_CS, 0);
	gpio_set_level(CONFIG_PT_LCD_SPI_DC, 0);
	err = spi_device_polling_transmit(reader, &t);
	gpio_set_level(CONFIG_PT_LCD_SPI_DC, 1);
	if (!err)
		err = spi_device_polling_transmit(reader, &r);
	gpio_set_level(CONFIG_PT_LCD_SPI_CS, 1);
	/* back to the controller, the way the SPI driver routed it */
	if (CONFIG_PT_LCD_SPI_CS == spi_periph_signal[HOST].spics0_iomux_pin)
		gpio_iomux_output(CONFIG_PT_LCD_SPI_CS, spi_periph_signal[HOST].func);
	else
		gpio_matrix_output(CONFIG_PT_LCD_SPI_CS, spi_periph_signal[HOST].spics_out[0], false,
				   false);
	spi_device_release_bus(reader);
	return err ? -EIO : 0;
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
		 clock_hz / 1000000);
	return name;
}

#endif /* CONFIG_PT_LCD_ILI9341_SPI */
