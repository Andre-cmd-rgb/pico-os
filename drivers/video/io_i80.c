/*
 * The panel on the ESP32-S3's Intel 8080 (i80) parallel bus: eight data
 * lines, RS (data/command), WR and CS. The 2.4" Arduino Uno shield is wired
 * this way. RD is not used for drawing; tie it to 3.3 V or give the driver
 * its pin, but never leave it floating.
 *
 * On jumper wires this bus is delicate, so the probe half of this file can
 * drive every pin by hand and read the controller back: see lcdprobe and
 * lcdreg, and the story in docs/WIRING.md.
 */
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "ili9341.h"
#include "lcd_io.h"

#if CONFIG_PT_LCD_ILI9341_I80

#define MAX_TRANSFER	16384

static esp_lcd_panel_io_handle_t bus_io;	/* for the lcdreg bus steps */
static esp_lcd_i80_bus_handle_t bus;

int lcd_io_open(esp_lcd_panel_io_handle_t *io, esp_lcd_panel_io_color_trans_done_cb_t done)
{
	esp_lcd_i80_bus_config_t bus_cfg = {
		.dc_gpio_num = CONFIG_PT_LCD_RS,
		.wr_gpio_num = CONFIG_PT_LCD_WR,
		.clk_src = LCD_CLK_SRC_DEFAULT,
		.data_gpio_nums = {
			CONFIG_PT_LCD_D0, CONFIG_PT_LCD_D1, CONFIG_PT_LCD_D2, CONFIG_PT_LCD_D3,
			CONFIG_PT_LCD_D4, CONFIG_PT_LCD_D5, CONFIG_PT_LCD_D6, CONFIG_PT_LCD_D7,
		},
		.bus_width = 8,
		.max_transfer_bytes = MAX_TRANSFER,
		.dma_burst_size = 64,
	};
	esp_lcd_panel_io_i80_config_t io_cfg = {
		.cs_gpio_num = CONFIG_PT_LCD_CS,
		.pclk_hz = CONFIG_PT_LCD_PCLK_HZ,
		.trans_queue_depth = 4,
		.on_color_trans_done = done,
		.lcd_cmd_bits = 8,
		.lcd_param_bits = 8,
		.dc_levels = {
			.dc_idle_level = 0,
			.dc_cmd_level = 0,
			.dc_dummy_level = 0,
			.dc_data_level = 1,
		},
	};
	esp_err_t err;

	if (bus || bus_io || *io)
		return -EBUSY;
	err = esp_lcd_new_i80_bus(&bus_cfg, &bus);

	if (!err)
		err = esp_lcd_new_panel_io_i80(bus, &io_cfg, io);
	if (err) {
		klog("lcd: i80 bus setup failed (%s)", esp_err_to_name(err));
		lcd_io_close(io);
		return -EIO;
	}
	/* RD idles high: low turns the shield's buffer around onto our pins */
	if (CONFIG_PT_LCD_RD >= 0) {
		if ((err = gpio_reset_pin(CONFIG_PT_LCD_RD)) ||
		    (err = gpio_set_direction(CONFIG_PT_LCD_RD, GPIO_MODE_OUTPUT)) ||
		    (err = gpio_set_level(CONFIG_PT_LCD_RD, 1))) {
			klog("lcd: i80 read pin setup failed (%s)", esp_err_to_name(err));
			lcd_io_close(io);
			return -EIO;
		}
	}
	bus_io = *io;
	return 0;
}

int lcd_io_close(esp_lcd_panel_io_handle_t *io)
{
	esp_err_t err;

	if (*io) {
		err = esp_lcd_panel_io_del(*io);
		if (err)
			goto fail;
		*io = bus_io = NULL;
	}
	if (bus) {
		err = esp_lcd_del_i80_bus(bus);
		if (err)
			goto fail;
		bus = NULL;
	}
	return 0;
fail:
	klog("lcd: i80 cleanup failed (%s)", esp_err_to_name(err));
	return -EIO;
}

uint8_t *lcd_io_alloc(size_t bytes)
{
	return bus_io ? esp_lcd_i80_alloc_draw_buffer(bus_io, bytes, 0) : NULL;
}

size_t lcd_io_max_transfer(void)
{
	return MAX_TRANSFER;
}

int lcd_io_reset_gpio(void)
{
	return CONFIG_PT_LCD_RST;
}

const char *lcd_io_name(void)
{
	static char name[32];

	snprintf(name, sizeof(name), "i80 bus at %d kHz", CONFIG_PT_LCD_PCLK_HZ / 1000);
	return name;
}

/* One command through the i80 bus, as lcd_init sends them (for lcdreg). */
int lcd_bus_command(uint8_t cmd, const uint8_t *arg, int n)
{
	if (!bus_io)
		return -ENODEV;
	lcd_bus_lock(true);
	esp_err_t err = esp_lcd_panel_io_tx_param(bus_io, cmd, n ? arg : NULL, n);
	lcd_bus_lock(false);
	return err ? -EIO : 0;
}

/*
 * Sends n bytes of 0x55/0xaa through the bus as a RAMWR and counts the rising
 * edges the pulse counter sees on `gpio` meanwhile: proof that the bus really
 * drives that pin (WR should see about n + 1, D0 about n / 2).
 */
int lcd_bus_count_edges(int gpio, int n)
{
	pcnt_unit_config_t ucfg = { .low_limit = -1, .high_limit = 30000 };
	pcnt_chan_config_t ccfg = { .edge_gpio_num = gpio, .level_gpio_num = -1 };
	pcnt_unit_handle_t unit = NULL;
	pcnt_channel_handle_t chan = NULL;
	int count = -1;

	if (!bus_io || n < 1 || n > 20000)
		return -EINVAL;
	uint8_t *buf = lcd_io_alloc(n);
	if (!buf)
		return -ENOMEM;
	for (int i = 0; i < n; i++)
		buf[i] = i & 1 ? 0xaa : 0x55;
	if (pcnt_new_unit(&ucfg, &unit) || pcnt_new_channel(unit, &ccfg, &chan) ||
	    pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
					 PCNT_CHANNEL_EDGE_ACTION_HOLD) ||
	    pcnt_unit_enable(unit) || pcnt_unit_clear_count(unit) || pcnt_unit_start(unit))
		goto out;
	lcd_bus_lock(true);
	lcd_wait_done(0);
	esp_lcd_panel_io_tx_color(bus_io, CMD_RAMWR, buf, n);
	lcd_wait_done(1000);
	lcd_bus_lock(false);
	vTaskDelay(1);
	pcnt_unit_get_count(unit, &count);
	pcnt_unit_stop(unit);
out:
	if (chan)
		pcnt_del_channel(chan);
	if (unit) {
		pcnt_unit_disable(unit);
		pcnt_del_unit(unit);
	}
	heap_caps_free(buf);
	return count;
}

/* ------------------------------------------------------------ probe */

/*
 * A slow wiring test that leaves the i80 peripheral out: every signal is set
 * by hand, a microsecond at a time, so neither the bus setup nor its timing
 * can be the problem. It takes the pins over from the bus; reboot afterwards.
 */
static const int bb_data[8] = {
	CONFIG_PT_LCD_D0, CONFIG_PT_LCD_D1, CONFIG_PT_LCD_D2, CONFIG_PT_LCD_D3,
	CONFIG_PT_LCD_D4, CONFIG_PT_LCD_D5, CONFIG_PT_LCD_D6, CONFIG_PT_LCD_D7,
};
static int bb_rd = -1;

static void bb_output(int pin, int level)
{
	gpio_reset_pin(pin);
	gpio_set_direction(pin, GPIO_MODE_OUTPUT);
	gpio_set_level(pin, level);
}

static void bb_write(uint8_t v)
{
	for (int i = 0; i < 8; i++)
		gpio_set_level(bb_data[i], v >> i & 1);
	gpio_set_level(CONFIG_PT_LCD_WR, 0);
	esp_rom_delay_us(1);
	gpio_set_level(CONFIG_PT_LCD_WR, 1);
}

static void bb_command(uint8_t cmd, const uint8_t *arg, int n)
{
	gpio_set_level(CONFIG_PT_LCD_CS, 0);
	gpio_set_level(CONFIG_PT_LCD_RS, 0);
	bb_write(cmd);
	gpio_set_level(CONFIG_PT_LCD_RS, 1);
	for (int i = 0; i < n; i++)
		bb_write(arg[i]);
	gpio_set_level(CONFIG_PT_LCD_CS, 1);
}

int lcd_probe_begin(int rd_gpio, bool reset)
{
	if (rd_gpio >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(rd_gpio))
		return -EINVAL;
	for (int i = 0; i < 8; i++)
		bb_output(bb_data[i], 0);
	bb_output(CONFIG_PT_LCD_RS, 1);
	bb_output(CONFIG_PT_LCD_WR, 1);
	bb_output(CONFIG_PT_LCD_CS, 1);
	bb_rd = rd_gpio;
	if (bb_rd >= 0)
		bb_output(bb_rd, 1);
	if (reset && CONFIG_PT_LCD_RST >= 0) {
		bb_output(CONFIG_PT_LCD_RST, 0);
		vTaskDelay(pdMS_TO_TICKS(20));
		gpio_set_level(CONFIG_PT_LCD_RST, 1);
		vTaskDelay(pdMS_TO_TICKS(150));
	}
	return 0;
}

/*
 * Writes the index bytes as commands, then reads n bytes back with RD. The
 * data pins get the internal pull-up (pull 1) or pull-down (pull 0): a line
 * the display really drives reads the same either way.
 */
int lcd_probe_read(const uint8_t *index, int nindex, uint8_t *out, int n, int pull)
{
	if (bb_rd < 0)
		return -ENODEV;
	gpio_set_level(CONFIG_PT_LCD_CS, 0);
	gpio_set_level(CONFIG_PT_LCD_RS, 0);
	for (int i = 0; i < nindex; i++)
		bb_write(index[i]);
	gpio_set_level(CONFIG_PT_LCD_RS, 1);
	for (int i = 0; i < 8; i++) {
		gpio_set_direction(bb_data[i], GPIO_MODE_INPUT);
		gpio_set_pull_mode(bb_data[i], pull ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
	}
	esp_rom_delay_us(20);			/* let the pulls settle a floating line */
	for (int k = 0; k < n; k++) {
		gpio_set_level(bb_rd, 0);
		esp_rom_delay_us(2);
		uint8_t v = 0;
		for (int i = 0; i < 8; i++)
			v |= gpio_get_level(bb_data[i]) << i;
		out[k] = v;
		gpio_set_level(bb_rd, 1);
		esp_rom_delay_us(2);
	}
	for (int i = 0; i < 8; i++) {
		gpio_set_pull_mode(bb_data[i], GPIO_FLOATING);
		gpio_set_direction(bb_data[i], GPIO_MODE_OUTPUT);
	}
	gpio_set_level(CONFIG_PT_LCD_CS, 1);
	return 0;
}

void lcd_probe_command(uint8_t cmd, const uint8_t *arg, int n)
{
	bb_command(cmd, arg, n);
}

/* The same ILI9341 set-up lcd_init sends, by hand. */
void lcd_probe_init(void)
{
	for (size_t i = 0; i < ili9341_init_len;) {
		uint8_t cmd = ili9341_init_seq[i++];
		uint8_t len = ili9341_init_seq[i] & ~DELAY;
		bool delay = ili9341_init_seq[i++] & DELAY;

		bb_command(cmd, &ili9341_init_seq[i], len);
		i += len;
		vTaskDelay(pdMS_TO_TICKS(delay ? ili9341_init_seq[i++] : CMD_GAP_MS));
	}
	const uint8_t mode = ili9341_madctl();
	bb_command(CMD_MADCTL, &mode, 1);
	bb_command(CMD_INVOFF, NULL, 0);
}

void lcd_probe_fill(uint16_t rgb565)
{
	static const uint8_t col[4] = { 0, 0, 319 >> 8, 319 & 0xff };
	static const uint8_t row[4] = { 0, 0, 0, 239 };

	bb_command(CMD_CASET, col, 4);
	bb_command(CMD_RASET, row, 4);
	gpio_set_level(CONFIG_PT_LCD_CS, 0);
	gpio_set_level(CONFIG_PT_LCD_RS, 0);
	bb_write(CMD_RAMWR);
	gpio_set_level(CONFIG_PT_LCD_RS, 1);
	for (int i = 0; i < 320 * 240; i++) {
		bb_write(rgb565 >> 8);
		bb_write(rgb565);
	}
	gpio_set_level(CONFIG_PT_LCD_CS, 1);
}

int lcd_io_read(uint8_t cmd, uint8_t *out, int n)
{
	return -ENOTSUP;
}

int lcd_io_set_clock(esp_lcd_panel_io_handle_t *io, int hz)
{
	return -ENOTSUP;
}

int lcd_io_clock(void)
{
	return CONFIG_PT_LCD_PCLK_HZ;
}

#endif /* CONFIG_PT_LCD_ILI9341_I80 */
