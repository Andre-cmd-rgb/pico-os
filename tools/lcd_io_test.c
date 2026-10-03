/* IDF's LCD colour callback disables D/C before waking the sending task. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#define CONFIG_PT_LCD_SPI_CS 10
#define CONFIG_PT_LCD_SPI_DC 14
#define PIN_FUNC_GPIO 1
#define GPIO_MODE_OUTPUT 1
#define HOST 0
#define SPI_TRANS_USE_TXDATA 1
#define portMAX_DELAY -1
typedef int esp_err_t;
typedef struct { int length, rxlength, flags; uint8_t tx_data[4]; void *rx_buffer; } spi_transaction_t;
static struct { int spics0_iomux_pin, func, spics_out[1]; } spi_periph_signal[] = { { 10, 2, { 3 } } };
static int reader = 1, calls, fail_at, cs_level, dc_level;
static bool owned, cs_manual, dc_enabled, acquire_fail;

static int spi_device_acquire_bus(int device, int wait)
{
	assert(device == reader && wait == portMAX_DELAY && !owned);
	if (acquire_fail)
		return 1;
	owned = true;
	/* Any outstanding colour callback has finished before acquisition. */
	dc_enabled = false;
	return 0;
}

static void spi_device_release_bus(int device)
{
	assert(device == reader && owned && !cs_manual && cs_level == 1);
	owned = false;
}

static void gpio_func_sel(int pin, int function)
{
	assert(owned && pin == CONFIG_PT_LCD_SPI_CS && function == PIN_FUNC_GPIO);
	cs_manual = true;
}

static void gpio_set_direction(int pin, int direction)
{
	assert(owned && direction == GPIO_MODE_OUTPUT);
	if (pin == CONFIG_PT_LCD_SPI_DC)
		dc_enabled = true;
	else
		assert(pin == CONFIG_PT_LCD_SPI_CS);
}

static void gpio_set_level(int pin, int level)
{
	assert(owned);
	if (pin == CONFIG_PT_LCD_SPI_CS)
		cs_level = level;
	else {
		assert(pin == CONFIG_PT_LCD_SPI_DC);
		dc_level = level;
	}
}

static void gpio_iomux_output(int pin, int function)
{
	assert(owned && pin == CONFIG_PT_LCD_SPI_CS && function == 2);
	cs_manual = false;
}

static void gpio_matrix_output(int pin, int signal, bool invert, bool enable_invert)
{
	assert(owned && pin == CONFIG_PT_LCD_SPI_CS && signal == 3 && !invert && !enable_invert);
	cs_manual = false;
}

static int spi_device_polling_transmit(int device, spi_transaction_t *transaction)
{
	assert(device == reader && owned && cs_manual && !cs_level);
	assert(dc_enabled); /* A GPIO level write alone does not enable D/C. */
	if (++calls == 1) {
		assert(!dc_level && transaction->length == 8 && transaction->tx_data[0] == 0x45);
	} else {
		assert(dc_level && transaction->rxlength == 24);
		((uint8_t *)transaction->rx_buffer)[1] = 12;
	}
	return calls == fail_at ? 1 : 0;
}

#include "lcd_read_under_test.h"

int main(void)
{
	uint8_t bytes[3] = { 0 };

	for (int routing = 0; routing < 2; routing++) {
		spi_periph_signal[0].spics0_iomux_pin = routing ? 11 : 10;
		for (fail_at = 0; fail_at <= 2; fail_at++) {
			calls = 0;
			dc_enabled = false;
			assert(lcd_io_read(0x45, bytes, 3) == (fail_at ? -EIO : 0));
			assert(!owned && !cs_manual && cs_level == 1);
			assert(calls == (fail_at == 1 ? 1 : 2));
		}
	}
	acquire_fail = true;
	calls = 0;
	assert(lcd_io_read(0x45, bytes, 3) == -EIO && !calls && !owned);
	assert(lcd_io_read(0x45, bytes, 5) == -ENOTSUP);
	reader = 0;
	assert(lcd_io_read(0x45, bytes, 3) == -ENOTSUP);
	puts("LCD SPI: D/C restored after colour DMA, bus ownership and failure cleanup passed");
	return 0;
}
