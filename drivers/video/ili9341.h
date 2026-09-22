/*
 * What the panel driver and the parallel-bus probe share: the controller's
 * commands and the start-up sequence.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define CMD_SWRESET	0x01
#define CMD_SLPIN	0x10
#define CMD_SLPOUT	0x11
#define CMD_INVOFF	0x20
#define CMD_INVON	0x21
#define CMD_DISPON	0x29
#define CMD_CASET	0x2a
#define CMD_RASET	0x2b
#define CMD_RAMWR	0x2c
#define CMD_MADCTL	0x36

#define MADCTL_MY	0x80
#define MADCTL_MX	0x40
#define MADCTL_MV	0x20
#define MADCTL_BGR	0x08

/*
 * { command, argument count, arguments... }; DELAY in the count means a
 * delay in milliseconds follows the arguments. Every command is followed by
 * a short pause anyway (CMD_GAP_MS): measured on the 2.4" shield, the
 * sequence sent back to back left the controller asleep with its booster
 * off, because "sleep out" was ignored, and nothing was ever displayed.
 */
#define DELAY		0x80
#define CMD_GAP_MS	2

extern const uint8_t ili9341_init_seq[];
extern const size_t ili9341_init_len;

/* The panel's own MADCTL for the configured rotation and colour order. */
uint8_t ili9341_madctl(void);

/* Shared with the probe: the bus mutex and the "transfer finished" wait. */
void	lcd_bus_lock(bool take);
void	lcd_wait_done(int ms);
