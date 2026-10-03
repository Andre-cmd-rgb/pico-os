#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef int nespal_t;
#define NES_PALETTE_COUNT 2
#define MESSAGE_WARN(...) ((void)0)
static uint8 nes_palettes[NES_PALETTE_COUNT][64 * 3];
static uint8 gui_pal[8 * 3];
static bool fail_alloc;
static int allocations;

static void *test_calloc(size_t n, size_t size)
{
	allocations++;
	return fail_alloc ? NULL : calloc(n, size);
}

#define calloc test_calloc
#include "nes_palette_under_test.c"
#undef calloc

int main(void)
{
	nes_palettes[0][0] = 255;
	nes_palettes[0][4] = 255;
	gui_pal[2] = 255;
	for (int depth = 15; depth <= 24; depth++) {
		if (depth != 15 && depth != 16 && depth != 24)
			continue;
		fail_alloc = true;
		allocations = 0;
		assert(!nofrendo_buildpalette(0, depth));
		assert(allocations == 1);
		fail_alloc = false;
		void *p = nofrendo_buildpalette(-1, depth);
		assert(p);
		if (depth == 24) {
			uint8 *v = p;
			assert(v[0] == 255 && v[1] == 0 && v[2] == 0);
			assert(!memcmp(v, v + 64 * 3, 64 * 3));
			assert(!memcmp(v, v + 128 * 3, 64 * 3));
			assert(v[192 * 3 + 2] == 255);
		} else {
			uint16 *v = p;
			assert(v[0] == (depth == 16 ? 0xf800 : 0x7c00));
			assert(v[1] == (depth == 16 ? 0x07e0 : 0x03e0));
			assert(v[64] == v[0] && v[128] == v[0]);
			assert(v[192] == 0x001f);
		}
		free(p);
	}
	assert(!nofrendo_buildpalette(0, 32));
	puts("NES palette: allocation failures and 15/16/24-bit palette output passed");
	return 0;
}
