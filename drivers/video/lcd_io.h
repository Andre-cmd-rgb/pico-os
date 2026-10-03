/*
 * The bus under the panel. Boards with the display soldered on drive it over
 * SPI (io_spi.c); Arduino shields use an 8-bit parallel bus (io_i80.c). The
 * panel driver above (ili9341.c) is the same either way.
 */
#pragma once

#include "esp_lcd_panel_io.h"

/* Brings the bus up and returns the handle commands and pixels go through. */
int	lcd_io_open(esp_lcd_panel_io_handle_t *io, esp_lcd_panel_io_color_trans_done_cb_t done);

/* Releases a partial startup too; no drawing may be in progress. */
int	lcd_io_close(esp_lcd_panel_io_handle_t *io);

/* A buffer the bus can send by DMA. Free it with heap_caps_free. */
uint8_t *lcd_io_alloc(size_t bytes);

/* The most pixel bytes one transfer may carry. */
size_t	lcd_io_max_transfer(void);

/* The panel's reset pin, or -1 when it follows the board's own reset. */
int	lcd_io_reset_gpio(void);

const char *lcd_io_name(void);

/* A register of the panel read back, n bytes; -ENOTSUP where it cannot be. */
int	lcd_io_read(uint8_t cmd, uint8_t *out, int n);

/* A long read after a command (the panel's memory), slowly; -ENOTSUP where it cannot be. */
int	lcd_io_read_long(uint8_t cmd, uint8_t *out, size_t n);

/*
 * Commands and their data queued all at once and sent back to back by
 * the bus's interrupt, so that nothing between two of them waits on a
 * task: a whole frame's bands goes out at the wire's own pace. Data over
 * four bytes is sent from where it is (PSRAM too, from a cache line).
 * The caller holds the bus. With no steps it says whether it can be done:
 * 0, or -ENOTSUP.
 */
struct lcd_io_step {
	uint8_t		 cmd;
	const uint8_t	*data;
	size_t		 len;
};

int	lcd_io_stream(const struct lcd_io_step *steps, int n);

/* The bus clock, changed at run time; -ENOTSUP where it cannot be. */
int	lcd_io_set_clock(esp_lcd_panel_io_handle_t *io, int hz);
int	lcd_io_clock(void);
int	lcd_io_stream_clock(void);	/* lcd_io_stream()'s, which may be slower */
