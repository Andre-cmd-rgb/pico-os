#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_PT_VT_COUNT 2
#include "../drivers/tty/font5x8.h"
#define SCALE 1
#define CELL_W (6 * SCALE)
#define CELL_H (10 * SCALE)
#define FG_DEFAULT 7
#define BG_DEFAULT 0
#define HIST_LINES 500
#define MAX_PARAMS 8
#define CLEAN_LO 0xffff
#define COLOR(fg, bg) ((fg) | (bg) << 8)
#define FG_OF(color) ((color) & 0xff)
#define BG_OF(color) ((color) >> 8)
#define VT_CURSOR 22
#define VT_COLORS 23
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define portMAX_DELAY 0xffffffffu
enum esc_state { ESC_NONE, ESC_START, ESC_CSI };
enum vt_cursor { VT_CURSOR_BLOCK, VT_CURSOR_UNDERLINE, VT_CURSOR_BAR };
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
struct cell { uint16_t glyph; uint16_t color; };

#include "vt_screen_under_test.h"

static int cols = 3, rows = 4, origin_x, text_y, active;
static int panel_width = 3 * CELL_W;
static struct screen screens[CONFIG_PT_VT_COUNT], *cur = &screens[0];
static SemaphoreHandle_t lock = (void *)1;
static TaskHandle_t renderer = (void *)2;
static struct { bool held; unsigned gen; } holder[CONFIG_PT_VT_COUNT];
static uint8_t palette[VT_COLORS][2];
static enum vt_cursor cursor_shape = VT_CURSOR_BLOCK;
static bool repaint_all, locked, switch_back, lcd_fail, row_fail;
static int allocations, fail_at, resources, notified, writes[4], switch_y = -1;
static uint8_t drawn[4 * CELL_H][320 * 2];
static int drawn_x[4], drawn_w[4];
static size_t pixel_bytes;
static int task_deleted;
static jmp_buf delete_point;

int vt_switch(int which);
static void mark_all(void)
{
	assert(cur->cells && cur->dirty_lo && cur->dirty_hi);
	for (int y = 0; y < rows; y++) {
		cur->dirty_lo[y] = 0;
		cur->dirty_hi[y] = cols - 1;
	}
}
static bool xSemaphoreTake(SemaphoreHandle_t sem, unsigned wait)
{ assert(sem == lock && !locked && wait == portMAX_DELAY); locked = true; return true; }
static void xSemaphoreGive(SemaphoreHandle_t sem)
{ assert(sem == lock && locked); locked = false; }
static void xTaskNotifyGive(TaskHandle_t task) { assert(task == renderer); notified++; }
static void *heap_caps_malloc_prefer(size_t n, int count, ...)
{
	void *p;

	assert(count == 2);
	if (++allocations == fail_at)
		return NULL;
	p = malloc(n);
	assert(p);
	memset(p, 0xa5, n);
	resources++;
	return p;
}
static void heap_caps_free(void *p) { if (p) { resources--; free(p); } }
static int lcd_width(void) { return panel_width; }
#include "vt_draw_cell_under_test.h"
static void lcd_draw(int x, int y, int w, int h, const uint8_t *px)
{
	assert(!locked && x >= 0 && x + w <= panel_width);
	assert(y >= 0 && y + h <= rows * CELL_H && h == CELL_H);
	for (int py = 0; py < h; py++)
		memcpy(drawn[y + py] + x * 2, px + py * w * 2, w * 2);
	writes[y / CELL_H]++;
	drawn_x[y / CELL_H] = x;
	drawn_w[y / CELL_H] = w;
	if (y / CELL_H == switch_y) {
		switch_y = -1;
		assert(!vt_switch(1));
		if (switch_back)
			assert(!vt_switch(0));
	}
}
static uint8_t *lcd_alloc_buffer(size_t n)
{ pixel_bytes = n; return lcd_fail ? NULL : heap_caps_malloc_prefer(n, 2); }
static void *renderer_malloc(size_t n) { return row_fail ? NULL : malloc(n); }
static void klog(const char *text) { (void)text; }
static void vTaskDelete(TaskHandle_t task)
{ assert(!task); task_deleted++; longjmp(delete_point, 1); }

#include "vt_render_under_test.h"
#define malloc renderer_malloc
#include "vt_render_alloc_under_test.h"
#undef malloc

static void fill_terminal(int which, uint8_t glyph)
{
	for (int k = 0; k < rows * cols; k++)
		screens[which].cells[k].glyph = glyph;
	cur = &screens[which];
	mark_all();
}

static void assert_cell(int x, int y, uint8_t glyph, uint16_t color)
{
	uint8_t expected[CELL_W * CELL_H * 2];

	draw_cell(expected, CELL_W, 0, glyph, color, false);
	for (int py = 0; py < CELL_H; py++)
		assert(!memcmp(drawn[y * CELL_H + py] + (origin_x + x * CELL_W) * 2,
			       expected + py * CELL_W * 2, CELL_W * 2));
}

static void assert_edge(int x, int y, unsigned bg)
{
	for (int py = 0; py < CELL_H; py++)
		assert(!memcmp(drawn[y * CELL_H + py] + x * 2, palette[bg], 2));
}

static void check_span(uint8_t *pixels, struct cell *row, int y, int lo, int hi)
{
	uint8_t before[4 * CELL_H][320 * 2];
	int first = lo ? origin_x + lo * CELL_W : 0;
	int end = hi == cols - 1 ? panel_width : origin_x + (hi + 1) * CELL_W;

	memcpy(before, drawn, sizeof(drawn));
	memset(writes, 0, sizeof(writes));
	screens[0].dirty_lo[y] = lo;
	screens[0].dirty_hi[y] = hi;
	render_rows(pixels, row, 0, false, -1, -1);
	assert(writes[y] == 1 && drawn_x[y] == first && drawn_w[y] == end - first);
	for (int line = 0; line < rows; line++) {
		if (line != y)
			assert(!writes[line]);
		for (int py = 0; py < CELL_H; py++)
			for (int x = 0; x < panel_width; x++)
				if (line != y || x < first || x >= end)
					assert(!memcmp(drawn[line * CELL_H + py] + x * 2,
						       before[line * CELL_H + py] + x * 2, 2));
	}
	for (int x = lo; x <= hi; x++) {
		struct cell *c = &screens[0].cells[y * cols + x];

		assert_cell(x, y, c->glyph, c->color);
	}
	if (!lo)
		assert_edge(0, y, BG_OF(screens[0].cells[y * cols].color));
	if (hi == cols - 1)
		assert_edge(panel_width - 1, y, BG_OF(screens[0].cells[y * cols + hi].color));
}

int main(void)
{
	uint8_t pixels[320 * CELL_H * 2];
	struct cell row[53];

	for (int i = 0; i < VT_COLORS; i++) {
		palette[i][0] = i;
		palette[i][1] = 255 - i;
	}

	for (int fault = 1; fault <= CONFIG_PT_VT_COUNT * 3; fault++) {
		allocations = 0;
		fail_at = fault;
		assert(!screen_alloc() && !resources);
		for (int i = 0; i < CONFIG_PT_VT_COUNT; i++)
			assert(!screens[i].cells && !screens[i].dirty_lo && !screens[i].dirty_hi);
		assert(vt_switch(1) == -ENOMEM);
	}
	fail_at = 0;
	assert(screen_alloc());
	fill_terminal(0, 'A');
	fill_terminal(1, 'B');
	cur = &screens[0];
	switch_y = 1;
	render_rows(pixels, row, 0, false, -1, -1);
	assert(active == 1 && repaint_all && notified == 1);
	for (int y = 0; y < rows; y++) {
		assert_cell(0, y, 'A', COLOR(FG_DEFAULT, BG_DEFAULT));
		assert_cell(2, y, 'A', COLOR(FG_DEFAULT, BG_DEFAULT));
		assert(screens[1].dirty_lo[y] == 0);
	}
	assert(prepare_frame(1));
	memset(drawn, 0, sizeof(drawn));
	render_rows(pixels, row, 1, false, -1, -1);
	for (int y = 0; y < rows; y++) {
		assert_cell(0, y, 'B', COLOR(FG_DEFAULT, BG_DEFAULT));
		assert_cell(2, y, 'B', COLOR(FG_DEFAULT, BG_DEFAULT));
	}
	/* Switching away and back still needs all rows after the panel clear. */
	assert(!vt_switch(0));
	switch_y = 1;
	switch_back = true;
	render_rows(pixels, row, 0, false, -1, -1);
	assert(active == 0 && repaint_all);
	assert(prepare_frame(0));
	memset(drawn, 0, sizeof(drawn));
	render_rows(pixels, row, 0, false, -1, -1);
	for (int y = 0; y < rows; y++) {
		assert_cell(0, y, 'A', COLOR(FG_DEFAULT, BG_DEFAULT));
		assert_cell(2, y, 'A', COLOR(FG_DEFAULT, BG_DEFAULT));
	}
	/* A dirty span starts at row[0], preserving untouched earlier cells. */
	memset(writes, 0, sizeof(writes));
	screens[0].cells[2 * cols + 1].glyph = 'x';
	screens[0].cells[2 * cols + 2].glyph = 'y';
	screens[0].dirty_lo[2] = 1;
	screens[0].dirty_hi[2] = 2;
	render_rows(pixels, row, 0, false, -1, -1);
	assert_cell(0, 2, 'A', COLOR(FG_DEFAULT, BG_DEFAULT));
	assert_cell(1, 2, 'x', COLOR(FG_DEFAULT, BG_DEFAULT));
	assert_cell(2, 2, 'y', COLOR(FG_DEFAULT, BG_DEFAULT));
	assert(writes[2] == 1 && !writes[0] && !writes[1] && !writes[3]);
	screen_free_all();
	assert(!resources);
	/* The real 320px panel has one edge pixel on each side of 53 cells. */
	cols = 53;
	origin_x = 1;
	panel_width = 320;
	assert(screen_alloc());
	fill_terminal(0, 'C');
	for (int y = 0; y < rows; y++) {
		screens[0].cells[y * cols].color = COLOR(FG_DEFAULT, 2);
		screens[0].cells[y * cols + cols - 1].color = COLOR(FG_DEFAULT, 3);
	}
	memset(writes, 0, sizeof(writes));
	render_rows(pixels, row, 0, false, -1, -1);
	for (int y = 0; y < rows; y++) {
		assert(writes[y] == 1 && drawn_x[y] == 0 && drawn_w[y] == 320);
		assert_edge(0, y, 2);
		assert_edge(319, y, 3);
		assert_cell(0, y, 'C', COLOR(FG_DEFAULT, 2));
		assert_cell(52, y, 'C', COLOR(FG_DEFAULT, 3));
	}
	const int spans[][2] = { { 1, 51 }, { 0, 5 }, { 48, 52 }, { 1, 1 }, { 0, 52 } };

	for (size_t i = 0; i < sizeof(spans) / sizeof(spans[0]); i++) {
		int lo = spans[i][0], hi = spans[i][1];

		for (int x = lo; x <= hi; x++) {
			screens[0].cells[cols + x].glyph = 'D' + i;
			screens[0].cells[cols + x].color = COLOR(FG_DEFAULT, x == lo ? 4 : 5);
		}
		check_span(pixels, row, 1, lo, hi);
	}
	/* Invalid cell indices use the same foreground/background fallbacks
	 * for glyphs and spare edges, rather than reading beyond the palette. */
	screens[0].cells[0] = (struct cell) { FONT_BLOCK, COLOR(255, 255) };
	screens[0].cells[cols - 1] = (struct cell) { 0, COLOR(255, 255) };
	screens[0].dirty_lo[0] = 0;
	screens[0].dirty_hi[0] = cols - 1;
	memset(writes, 0, sizeof(writes));
	render_rows(pixels, row, 0, false, -1, -1);
	assert(writes[0] == 1 && drawn_w[0] == 320);
	assert_edge(0, 0, BG_DEFAULT);
	assert_edge(319, 0, BG_DEFAULT);
	assert_cell(0, 0, FONT_BLOCK, COLOR(255, 255));
	assert_cell(52, 0, 0, COLOR(255, 255));
	screen_free_all();
	assert(!resources);
	for (int fault = 1; fault <= 3; fault++) {
		lcd_fail = fault & 1;
		row_fail = fault & 2;
		renderer = (void *)2;
		if (!setjmp(delete_point))
			render_task(NULL);
		assert(!resources && !renderer && task_deleted == fault && pixel_bytes == 6400);
	}
	puts("VT renderer: grid faults, switched frames, RGB565 edge/partial spans and 320px allocation cleanup passed");
	return 0;
}
