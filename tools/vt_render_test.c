#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_PT_VT_COUNT 2
#define CONFIG_PT_STATUS_LINE 1
#include "../drivers/tty/font5x8.h"
#define SCALE 1
#define CELL_W (6 * SCALE)
#define CELL_H (10 * SCALE)
#define HIST_LINES 500
#define MAX_PARAMS 8
#define CLEAN_LO 0xffff
#define COLOR(fg, bg) ((fg) | (bg) << 8)
#define FG_OF(color) ((color) & 0xff)
#define BG_OF(color) ((color) >> 8)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) MIN(MAX(v, lo), hi)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) ((unsigned)(ms))
#define PANEL_LINES 64
enum vt_color { VT_FG = 16, VT_BG, VT_DIM, VT_BOLD, VT_BAR_FG, VT_BAR_BG, VT_CURSOR, VT_COLORS };
#define FG_DEFAULT VT_FG
#define BG_DEFAULT VT_BG
enum esc_state { ESC_NONE, ESC_START, ESC_CSI };
enum vt_cursor { VT_CURSOR_BLOCK, VT_CURSOR_UNDERLINE, VT_CURSOR_BAR };
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
struct cell { uint8_t glyph; uint16_t color; };

#include "vt_screen_under_test.h"

static int cols = 3, rows = 4, origin_x, origin_y, text_y, bar_y = 4 * CELL_H, active;
static int panel_width = 3 * CELL_W, panel_height = 5 * CELL_H;
static bool bar_top, display = true;
static struct screen screens[CONFIG_PT_VT_COUNT], *cur = &screens[0];
static SemaphoreHandle_t lock = (void *)1;
static TaskHandle_t renderer = (void *)2;
static uint8_t palette[VT_COLORS][2];
static enum vt_cursor cursor_shape = VT_CURSOR_BLOCK;
static int blink_ms = 530;
static bool locked, panel_held, switch_back, lcd_fail, row_fail;
static int allocations, fail_at, resources, notified, switch_y = -1;
static uint8_t drawn[PANEL_LINES][320 * 2];
static int writes[8], drawn_x[8], drawn_w[8], bar_writes;
static size_t pixel_bytes;
static uint8_t *pixels_given;
static struct cell *row_given;
static int task_deleted;
static jmp_buf delete_point;

int vt_switch(int which);
int vt_active(void);
void vt_redraw(void);
static bool xSemaphoreTake(SemaphoreHandle_t sem, unsigned wait);
static void xSemaphoreGive(SemaphoreHandle_t sem);
static void xTaskNotifyGive(TaskHandle_t task) { assert(task == renderer); notified++; }
static void *heap_caps_malloc(size_t n, int caps)
{
	void *p = malloc(n);

	assert(p);
	resources++;
	return p;
}
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
static void *heap_caps_aligned_alloc(size_t align, size_t n, int caps)
{
	void *p = aligned_alloc(align, n);

	assert(p);
	resources++;
	return p;
}
static void heap_caps_free(void *p) { if (p) { resources--; free(p); } }

#include "vt_grid_under_test.h"
#include "vt_cell_under_test.h"

struct alarm { int kind; uint8_t hour, min; char label[40]; };
#define ALARM_CHIME 1
enum battery_state { BATTERY_NONE, BATTERY_USB, BATTERY_DISCHARGING, BATTERY_CHARGING };
struct battery_status { enum battery_state state; int percent; };
struct wifi_info { bool up; int8_t rssi; };
struct proc { int pid; };
static struct proc program = { 7 };
static bool program_alive = true, program_stopped;
static bool alarm_ringing(struct alarm *which) { return false; }
static bool proc_stopped(int pid) { return pid == program.pid && program_stopped; }
static bool proc_alive(int pid) { return pid == program.pid && program_alive; }
static struct proc *proc_current(void) { return &program; }
static int tty_of_current(void) { return 0; }
static void power_activity(void) { }

#include "vt_holders_under_test.h"

static bool xSemaphoreTake(SemaphoreHandle_t sem, unsigned wait)
{
	assert(wait == portMAX_DELAY);
	if (sem == panel) {
		assert(!panel_held && !locked);		/* the panel first, then the grid */
		panel_held = true;
	} else {
		assert(sem == lock && !locked);
		locked = true;
	}
	return true;
}
static void xSemaphoreGive(SemaphoreHandle_t sem)
{
	if (sem == panel) {
		assert(panel_held);
		panel_held = false;
	} else {
		assert(sem == lock && locked);
		locked = false;
	}
}

static int64_t now_us;
static time_t wall = 1700000000;
static int battery = 80;
static int64_t esp_timer_get_time(void) { return now_us; }
static time_t alarm_next_any(struct alarm *which) { return 0; }
static bool audio_to_jack(void) { return false; }
static int wifi_state(struct wifi_info *out) { return -1; }
static bool modem_data_up(void) { return false; }
static int modem_signal(void) { return 0; }
static int battery_status(struct battery_status *out)
{
	*out = (struct battery_status) { BATTERY_DISCHARGING, battery };
	return 0;
}
static int lcd_width(void) { return panel_width; }
static int lcd_height(void) { return panel_height; }
static void lcd_draw(int x, int y, int w, int h, const uint8_t *px)
{
	int band;

	assert(!locked && x >= 0 && w > 0 && x + w <= panel_width);
	assert(y >= 0 && h > 0 && y + h <= panel_height);
	for (int py = 0; py < h; py++)
		memcpy(drawn[y + py] + x * 2, px + py * w * 2, w * 2);
	if (y == bar_y) {
		assert(h == CELL_H);
		bar_writes++;
		return;
	}
	band = (y - text_y) / CELL_H;
	assert(h == CELL_H && y >= text_y && (y - text_y) % CELL_H == 0 && band < rows);
	writes[band]++;
	drawn_x[band] = x;
	drawn_w[band] = w;
	if (band == switch_y) {
		switch_y = -1;
		assert(!vt_switch(1));
		if (switch_back)
			assert(!vt_switch(0));
	}
}
static void lcd_fill(int x, int y, int w, int h, uint16_t rgb565)
{
	if (w <= 0 || h <= 0)
		return;				/* as the driver does */
	assert(!locked && x >= 0 && x + w <= panel_width && y >= 0 && y + h <= panel_height);
	for (int py = y; py < y + h; py++)
		for (int px = x; px < x + w; px++) {
			drawn[py][px * 2] = rgb565 >> 8;
			drawn[py][px * 2 + 1] = rgb565;
		}
}
#define time(t) wall
#include "vt_status_under_test.h"
#undef time

/*
 * The renderer's passes: each wait it makes for a notification takes the
 * next step of a script, whose action -- typing, output, a switch -- comes
 * first. When the script is over the renderer is left by a long jump.
 */
struct step {
	void	(*act)(void);
	uint32_t woken;			/* what the wait returns; 0: it ran out */
};
static const struct step *script;
static int script_len, script_at, delays, clears;
static void (*while_waiting)(void);	/* output landing during the 8 ms */
static jmp_buf loop_exit;

static uint32_t ulTaskNotifyTake(int clear, unsigned ticks)
{
	assert(clear == pdTRUE && !locked && !panel_held);
	if (!ticks) {
		clears++;
		return 0;
	}
	if (script_at == script_len)
		longjmp(loop_exit, 1);
	if (script[script_at].act)
		script[script_at].act();
	return script[script_at++].woken;
}
static void vTaskDelay(unsigned ticks)
{
	assert(ticks == 8 && !locked && !panel_held);
	delays++;
	if (while_waiting)
		while_waiting();
}
static void theme_tick(void) { }
static uint8_t *lcd_alloc_buffer(size_t n)
{
	pixel_bytes = n;
	return pixels_given = lcd_fail ? NULL : heap_caps_malloc_prefer(n, 2);
}
static void *renderer_malloc(size_t n) { return row_given = row_fail ? NULL : malloc(n); }
static void klog(const char *fmt, ...) { (void)fmt; }
static void vTaskDelete(TaskHandle_t task)
{ assert(!task); task_deleted++; longjmp(delete_point, 1); }

#define malloc renderer_malloc
#include "vt_render_under_test.h"
#undef malloc
#include "vt_looks_under_test.h"
#include "vt_allocation_under_test.h"
#include "vt_terminals_under_test.h"

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
		assert(!memcmp(drawn[text_y + y * CELL_H + py] + (origin_x + x * CELL_W) * 2,
			       expected + py * CELL_W * 2, CELL_W * 2));
}

static void assert_edge(int x, int y, unsigned bg)
{
	for (int py = 0; py < CELL_H; py++)
		assert(!memcmp(drawn[text_y + y * CELL_H + py] + x * 2, palette[bg], 2));
}

static void check_span(uint8_t *pixels, struct cell *row, int y, int lo, int hi)
{
	static uint8_t before[PANEL_LINES][320 * 2];
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

/* The renderer, through a script of steps; what it sent is counted afresh. */
static void run(const struct step *steps, int n)
{
	memset(writes, 0, sizeof(writes));
	bar_writes = delays = clears = 0;
	script = steps;
	script_len = n;
	script_at = 0;
	if (!setjmp(loop_exit))
		render_task(NULL);
	assert(script_at == n && !locked && !panel_held);
	heap_caps_free(pixels_given);
	free(row_given);
	pixels_given = NULL;
	row_given = NULL;
}

static void type_key(void) { vt_write("x", 1); }
static void two_rows(void) { vt_write("\x1b[1;1Hd\x1b[2;1He", 15); }
static void three_rows(void) { vt_write("\x1b[1;1Ha\x1b[2;1Hb\x1b[3;1Hc", 21); }
static void scroll_line(void) { vt_write("\x1b[4;1Hlast\n", 11); }
static void more_lines(void) { vt_write("one\ntwo\n", 8); }
static void blink_due(void) { now_us += 530000; }

static int sent_rows(void)
{
	int n = 0;

	for (int y = 0; y < rows; y++)
		n += writes[y];
	return n;
}

int main(void)
{
	uint8_t pixels[320 * CELL_H * 2];
	struct cell row[53];

	for (int i = 0; i < VT_COLORS; i++) {
		palette[i][0] = i;
		palette[i][1] = 255 - i;
	}
	panel = (void *)3;
	setenv("TZ", "UTC0", 1);
	tzset();

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
	lcd_fail = row_fail = false;
	pixels_given = NULL;
	row_given = NULL;

	/* The renderer's passes, on the real width with the bar below the text. */
	renderer = (void *)2;
	active = 0;
	cur = &screens[0];
	assert(screen_alloc());
	const struct step settle[] = { { NULL, 1 } };

	run(settle, 1);
	/* A key's echo is painted at once, without the wait for a burst. */
	const struct step key[] = { { type_key, 1 } };

	run(key, 1);
	assert(!delays && writes[0] == 1 && sent_rows() == 1);
	assert_cell(0, 0, 'x' - FONT_FIRST, COLOR(FG_DEFAULT, BG_DEFAULT));
	const struct step two[] = { { two_rows, 1 } };

	run(two, 1);
	assert(!delays && writes[0] == 1 && writes[1] == 1 && sent_rows() == 2);
	/* Three rows are a burst: the renderer waits for the rest of it. */
	const struct step three[] = { { three_rows, 1 } };

	run(three, 1);
	assert(delays == 1 && clears == 1 && sent_rows() == 3);
	/* So does a scroll, which changes every row, and what lands while it
	 * waits goes out in the same frame: each row is sent once. */
	const struct step scroll[] = { { scroll_line, 1 } };

	while_waiting = more_lines;
	run(scroll, 1);
	while_waiting = NULL;
	assert(delays == 1 && sent_rows() == rows);
	for (int y = 0; y < rows; y++)
		assert(writes[y] == 1);
	assert_cell(0, 1, 'o' - FONT_FIRST, COLOR(FG_DEFAULT, BG_DEFAULT));
	assert_cell(0, 2, 't' - FONT_FIRST, COLOR(FG_DEFAULT, BG_DEFAULT));
	/* A wake for the cursor's blink, the wait run out, never waits: not
	 * even for output that came just after it ran out. */
	const struct step blink[] = {
		{ NULL, 1 }, { blink_due, 0 }, { blink_due, 0 }, { three_rows, 0 },
	};

	run(blink, 4);
	assert(!delays && !clears && writes[3] >= 2 && writes[0] && writes[1] && writes[2]);
	screen_free_all();
	assert(!resources);
	puts("VT renderer: grid faults, switched frames, RGB565 edge/partial spans, 320px allocation "
	     "cleanup and frames without the burst wait passed");
	return 0;
}
