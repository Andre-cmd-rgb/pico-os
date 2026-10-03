/*
 * The one header the emulator core wants from its usual host.
 *
 * Nofrendo as shipped in Retro-Go asks for <rg_system.h> and two things
 * inside it: a logging call and a CRC. This adapter is ours; it keeps host
 * glue separate from the vendored core and its documented local patches. The functions live in
 * emu/nes_port.c.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_attr.h"		/* the real IRAM_ATTR, not an empty one */

#define RG_LOG_PRINTF	0

void	 rg_system_log(int level, const char *context, const char *format, ...);
uint32_t rg_crc32(uint32_t crc, const uint8_t *buf, size_t len);
