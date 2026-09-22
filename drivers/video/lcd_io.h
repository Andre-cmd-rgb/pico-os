/*
 * The bus under the panel. Boards with the display soldered on drive it over
 * SPI (io_spi.c); Arduino shields use an 8-bit parallel bus (io_i80.c). The
 * panel driver above (ili9341.c) is the same either way.
 */
#pragma once

#include "esp_lcd_panel_io.h"

/* Brings the bus up and returns the handle commands and pixels go through. */
int	lcd_io_open(esp_lcd_panel_io_handle_t *io, esp_lcd_panel_io_color_trans_done_cb_t done);

/* A buffer the bus can send by DMA. Free it with heap_caps_free. */
uint8_t *lcd_io_alloc(size_t bytes);

/* The most pixel bytes one transfer may carry. */
size_t	lcd_io_max_transfer(void);

/* The panel's reset pin, or -1 when it follows the board's own reset. */
int	lcd_io_reset_gpio(void);

const char *lcd_io_name(void);
