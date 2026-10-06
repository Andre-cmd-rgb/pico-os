/*
 * The panel on an SPI bus: four wires (clock, data, chip select, data/command)
 * plus a backlight, the usual wiring when the display is part of the board.
 * Fast and, unlike the parallel bus, not sensitive to wire length.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_private/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
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
 * Video goes out no faster than this, whatever the bus is set to. Past
 * it the panel now and then writes a pixel twice early in a long run,
 * and every pixel after it in that run lands one place on: half the
 * frames sent at 80 MHz came back so (`lcdtest verify`), none at 40,
 * from PSRAM or internal RAM alike, with the SPI controller reporting
 * nothing amiss.
 */
#define STREAM_MAX_HZ	40000000
#define STREAM_MAX	64		/* transactions in one stream: a frame's 14 bands */
/*
 * The board file says "2" or "3" for the controller's name; ESP-IDF numbers
 * them from SPI1, so SPI2 is 1. Passed straight through, a 2 was SPI3,
 * which has no pins of its own and goes through the GPIO matrix -- rated
 * for 40 MHz, and driven at 80. GPIO 10-13 are SPI2's own pins.
 */
#define HOST		(CONFIG_PT_LCD_SPI_HOST == 3 ? SPI3_HOST : SPI2_HOST)

static esp_lcd_panel_io_spi_config_t panel_cfg;
static spi_device_handle_t reader;
static spi_device_handle_t streamer;	/* lcd_io_stream()'s, chip select by hand */
static spi_transaction_t *stream_trans;	/* internal RAM: the interrupt reads them */
static bool bus_open;
static int clock_hz = CONFIG_PT_LCD_SPI_HZ;

/* D/C as the transaction says: low for a command, high for its data. */
static void IRAM_ATTR stream_pre(spi_transaction_t *t)
{
	gpio_ll_set_level(&GPIO, CONFIG_PT_LCD_SPI_DC, (uint32_t)(uintptr_t)t->user);
	gpio_ll_output_enable(&GPIO, CONFIG_PT_LCD_SPI_DC);
}

/*
 * A second writer on the bus, beside esp_lcd's: esp_lcd sends each
 * command synchronously, waiting first for what it queued, so a frame of
 * bands each needing its own row command would wait on a task between
 * every two. This one queues them all. Its chip select is the panel's,
 * taken by hand as for a read.
 */
static void stream_add(void)
{
	const spi_device_interface_config_t dev = {
		.clock_speed_hz = clock_hz < STREAM_MAX_HZ ? clock_hz : STREAM_MAX_HZ,
		.mode = 0,
		.spics_io_num = -1,
		.queue_size = STREAM_MAX,
		.flags = SPI_DEVICE_HALFDUPLEX,
		.pre_cb = stream_pre,
	};

	if (spi_bus_add_device(HOST, &dev, &streamer))
		streamer = NULL;
}

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
	if (streamer) {
		spi_bus_remove_device(streamer);
		streamer = NULL;
		stream_add();
	}
	return 0;
}

int lcd_io_clock(void)
{
	return clock_hz;
}

int lcd_io_stream_clock(void)
{
	return clock_hz < STREAM_MAX_HZ ? clock_hz : STREAM_MAX_HZ;
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
	lcd_io_hold(false);		/* as the last deep sleep left them */
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
	stream_add();
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
	if (streamer) {
		err = spi_bus_remove_device(streamer);
		if (err)
			goto fail;
		streamer = NULL;
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

/* Chip select by hand, low for as long as a stream runs. Bus owned. */
static void cs_take(void)
{
	gpio_func_sel(CONFIG_PT_LCD_SPI_CS, PIN_FUNC_GPIO);
	gpio_set_direction(CONFIG_PT_LCD_SPI_CS, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_PT_LCD_SPI_CS, 0);
}

/* Back to the controller, the way the SPI driver routed it. */
static void cs_give(void)
{
	gpio_set_level(CONFIG_PT_LCD_SPI_CS, 1);
	if (CONFIG_PT_LCD_SPI_CS == spi_periph_signal[HOST].spics0_iomux_pin)
		gpio_iomux_output(CONFIG_PT_LCD_SPI_CS, spi_periph_signal[HOST].func);
	else
		gpio_matrix_output(CONFIG_PT_LCD_SPI_CS, spi_periph_signal[HOST].spics_out[0], false,
				   false);
}

/*
 * A long read after a command -- the panel's memory, for checking what
 * reached it -- in pieces, chip select held low throughout so the panel
 * carries on where it was. Slow (the reader's 4 MHz); the caller holds
 * the bus.
 */
int lcd_io_read_long(uint8_t cmd, uint8_t *out, size_t n)
{
	spi_transaction_t t = { .length = 8, .flags = SPI_TRANS_USE_TXDATA, .tx_data = { cmd } };
	esp_err_t err;

	if (!reader)
		return -ENOTSUP;
	if (spi_device_acquire_bus(reader, portMAX_DELAY))
		return -EIO;
	cs_take();
	gpio_set_direction(CONFIG_PT_LCD_SPI_DC, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_PT_LCD_SPI_DC, 0);
	err = spi_device_polling_transmit(reader, &t);
	gpio_set_level(CONFIG_PT_LCD_SPI_DC, 1);
	while (!err && n) {
		size_t k = n < 4092 ? n : 4092;
		spi_transaction_t r = { .rxlength = k * 8, .rx_buffer = out };

		err = spi_device_polling_transmit(reader, &r);
		out += k;
		n -= k;
	}
	cs_give();
	spi_device_release_bus(reader);
	return err ? -EIO : 0;
}

void lcd_io_hold(bool on)
{
	static const int pins[] = {
		CONFIG_PT_LCD_SPI_CS, CONFIG_PT_LCD_SPI_DC, CONFIG_PT_LCD_SCLK, CONFIG_PT_LCD_MOSI,
	};

	for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		if (pins[i] < 0)
			continue;
		if (on)
			gpio_hold_en(pins[i]);
		else
			gpio_hold_dis(pins[i]);
	}
}

static int stream_fill(spi_transaction_t *t, const uint8_t *data, size_t len, bool dc)
{
	memset(t, 0, sizeof(*t));
	t->user = (void *)(uintptr_t)dc;
	t->length = len * 8;
	if (len <= 4) {
		t->flags = SPI_TRANS_USE_TXDATA;
		memcpy(t->tx_data, data, len);
	} else {
		t->tx_buffer = data;
		if (esp_ptr_external_ram(data))
			t->flags = SPI_TRANS_DMA_USE_PSRAM;
	}
	return 1;
}

int lcd_io_stream(const struct lcd_io_step *steps, int n)
{
	spi_transaction_t *got;
	UBaseType_t prio;
	int count = 0, queued = 0;
	esp_err_t err = ESP_OK;

	if (!streamer)
		return -ENOTSUP;
	if (!n)
		return 0;
	if (!stream_trans) {
		stream_trans = heap_caps_malloc(STREAM_MAX * sizeof(*stream_trans),
						MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		if (!stream_trans)
			return -ENOMEM;
	}
	for (int i = 0; i < n; i++) {
		size_t left = steps[i].len;
		const uint8_t *p = steps[i].data;

		if (count >= STREAM_MAX)
			return -E2BIG;
		count += stream_fill(&stream_trans[count], &steps[i].cmd, 1, false);
		while (left) {
			size_t k = left < MAX_TRANSFER ? left : MAX_TRANSFER;

			if (count >= STREAM_MAX)
				return -E2BIG;
			count += stream_fill(&stream_trans[count], p, k, true);
			p += k;
			left -= k;
		}
	}
	if (spi_device_acquire_bus(streamer, portMAX_DELAY))
		return -EIO;
	cs_take();
	/*
	 * The first band starts while the rest are being queued, and queueing
	 * them all takes a fraction of the time the first one takes to send:
	 * for that moment nothing on this core may come between them.
	 */
	prio = uxTaskPriorityGet(NULL);
	vTaskPrioritySet(NULL, configMAX_PRIORITIES - 2);
	for (; queued < count && !err; queued++)
		err = spi_device_queue_trans(streamer, &stream_trans[queued], 0);
	vTaskPrioritySet(NULL, prio);
	if (err)
		queued--;
	for (int i = 0; i < queued; i++)
		if (spi_device_get_trans_result(streamer, &got, pdMS_TO_TICKS(1000)) && !err)
			err = ESP_ERR_TIMEOUT;
	cs_give();
	spi_device_release_bus(streamer);
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
