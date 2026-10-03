#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define CMD_CASET 0x2a
#define CMD_RASET 0x2b
#define CMD_RAMWR 0x2c
#define portMAX_DELAY -1
typedef pthread_mutex_t *SemaphoreHandle_t;
static pthread_mutex_t bus = PTHREAD_MUTEX_INITIALIZER;
static SemaphoreHandle_t bus_lock = &bus;
static void *io = (void *)1;
static int width = 4, height = 3;
static int calls, fail_mask;
static atomic_int resources, copies, draw_entered, allow_copy;
static _Thread_local bool owns_bus;
static bool pause_copy;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;

static bool xSemaphoreTake(SemaphoreHandle_t m, int wait)
{
	assert(!owns_bus);
	if (wait ? pthread_mutex_lock(m) : pthread_mutex_trylock(m))
		return false;
	owns_bus = true;
	return true;
}
static void xSemaphoreGive(SemaphoreHandle_t m)
{
	assert(owns_bus);
	owns_bus = false;
	assert(!pthread_mutex_unlock(m));
}
static void lcd_bus_lock(bool on)
{
	if (on)
		assert(xSemaphoreTake(bus_lock, portMAX_DELAY));
	else
		xSemaphoreGive(bus_lock);
}
static void *heap_caps_calloc(size_t n, size_t bytes, int caps)
{
	void *p;

	assert(owns_bus && (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ||
			   caps == MALLOC_CAP_8BIT));
	if (fail_mask & (1 << calls++))
		return NULL;
	p = calloc(n, bytes);
	assert(p);
	atomic_fetch_add(&resources, 1);
	return p;
}
static void heap_caps_free(void *p)
{
	assert(owns_bus);
	if (p) {
		assert(atomic_fetch_sub(&resources, 1) == 1);
		free(p);
	}
}
static int esp_lcd_panel_io_tx_param(void *handle, int command, const void *data, int n)
{ assert(owns_bus && handle == io && data && n == 4 && (command == CMD_CASET || command == CMD_RASET)); return 0; }
static int esp_lcd_panel_io_tx_color(void *handle, int command, const void *data, size_t n)
{ assert(owns_bus && handle == io && command == CMD_RAMWR && data && n); return 0; }
static void lcd_wait_done(int ms) { assert(owns_bus && (ms == 0 || ms == 1000)); }

#include "lcd_capture_state_under_test.h"
#define capture_rect capture_rect_real
#include "lcd_capture_rect_under_test.h"
#undef capture_rect

/* Instrument ownership immediately before the actual production copy.
 * Hold this point so another core can try to retire its borrowed memory. */
static void capture_rect(int x, int y, int w, int h, const uint8_t *pixels)
{
	assert(owns_bus && capture && atomic_load(&resources) == 1);
	if (pause_copy) {
		assert(!pthread_mutex_lock(&gate));
		atomic_store(&draw_entered, 1);
		assert(!pthread_cond_signal(&changed));
		while (!atomic_load(&allow_copy))
			assert(!pthread_cond_wait(&changed, &gate));
		assert(!pthread_mutex_unlock(&gate));
	}
	capture_rect_real(x, y, w, h, pixels);
	atomic_fetch_add(&copies, 1);
}
#include "lcd_draw_under_test.h"

static uint8_t picture[24];
static void *draw_frame(void *arg)
{ (void)arg; lcd_draw(0, 0, 4, 3, picture); return NULL; }
static void *begin_capture(void *arg)
{ *(int *)arg = lcd_capture_begin(); return NULL; }

int main(void)
{
	pthread_t draw, first, second;
	int w, h, one, two;
	const uint8_t *pixels;

	bus_lock = NULL;
	assert(lcd_capture_begin() == -ENODEV && lcd_capture_try_end() == -ENODEV);
	lcd_capture_end();
	bus_lock = &bus;
	fail_mask = 3;
	assert(lcd_capture_begin() == -ENOMEM && !capture && !atomic_load(&resources));
	calls = 0; fail_mask = 1;
	assert(!lcd_capture_begin() && calls == 2 && atomic_load(&resources) == 1);
	assert(lcd_capture_begin() == -EBUSY && calls == 2);
	pixels = lcd_capture_pixels(&w, &h);
	assert(pixels == capture && w == 4 && h == 3);
	for (size_t i = 0; i < sizeof(picture); i++)
		picture[i] = i + 1;
	pause_copy = true;
	assert(!pthread_create(&draw, NULL, draw_frame, NULL));
	assert(!pthread_mutex_lock(&gate));
	while (!atomic_load(&draw_entered))
		assert(!pthread_cond_wait(&changed, &gate));
	assert(!pthread_mutex_unlock(&gate));
	/* Cleanup cannot free pixels while the draw holds the copy/DMA lock. */
	assert(lcd_capture_try_end() == -EAGAIN && atomic_load(&resources) == 1);
	assert(lcd_capture_try_end() == -EAGAIN && !atomic_load(&copies));
	assert(!pthread_mutex_lock(&gate));
	atomic_store(&allow_copy, 1);
	assert(!pthread_cond_signal(&changed));
	assert(!pthread_mutex_unlock(&gate));
	assert(!pthread_join(draw, NULL));
	assert(atomic_load(&copies) == 1 && !memcmp(capture, picture, sizeof(picture)));
	assert(!lcd_capture_try_end() && !capture && !atomic_load(&resources));
	assert(!lcd_capture_try_end());
	/* Two simultaneous owners may allocate only one capture. */
	fail_mask = 0;
	assert(!pthread_create(&first, NULL, begin_capture, &one));
	assert(!pthread_create(&second, NULL, begin_capture, &two));
	assert(!pthread_join(first, NULL) && !pthread_join(second, NULL));
	assert((!one && two == -EBUSY) || (!two && one == -EBUSY));
	assert(atomic_load(&resources) == 1);
	pause_copy = false;
	/* Clipping a rectangle still indexes the original source stride. */
	lcd_draw(-1, -1, 4, 3, picture);
	assert(capture[0] == picture[(4 + 1) * 2]);
	assert(capture[1] == picture[(4 + 1) * 2 + 1]);
	assert(capture[2 * 3] == 0 && capture[2 * (4 * 2)] == 0);
	lcd_capture_end();
	assert(!capture && !atomic_load(&resources));
	assert(!pthread_cond_destroy(&changed));
	assert(!pthread_mutex_destroy(&gate));
	assert(!pthread_mutex_destroy(&bus));
	puts("LCD capture: allocation faults, sole ownership, clipped copies and nonblocking cleanup during concurrent drawing passed");
	return 0;
}
