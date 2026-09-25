/*
 * Virtual terminal.
 *
 * The screen is a grid of cells (glyph + color). Output is interpreted as the
 * VT100 subset programs actually use: cursor movement, erase, insert/delete
 * line, SGR colors and cursor visibility. A renderer task on core 0 redraws
 * only cells whose content changed, one span per row per LCD transfer, and
 * blinks a block cursor.
 *
 * Without a display the grid is 80x24 and simply not drawn; the serial
 * mirror still carries every byte.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "font5x8.h"
#include "pt/kernel.h"

#if CONFIG_PT_FONT_LARGE
#define SCALE		2
#else
#define SCALE		1
#endif
#define CELL_W		(6 * SCALE)
#define CELL_H		(10 * SCALE)
#define HEADLESS_COLS	80
#define HEADLESS_ROWS	24
#define MAX_PARAMS	8
#define FG_DEFAULT	7
#define BG_DEFAULT	0
#define FG_DIM		4
#define CLEAN_LO	0xffff

#ifndef CONFIG_PT_CURSOR_BLINK_MS
#define CONFIG_PT_CURSOR_BLINK_MS 0		/* display disabled */
#endif

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) MIN(MAX(v, lo), hi)

/* ANSI color order: black red green yellow blue magenta cyan white, then bright */
#if CONFIG_PT_THEME_GREEN
static const uint32_t theme[16] = {
	0x030803, 0xff5f3a, 0x33ff66, 0x9cffb5, 0x1f9e45, 0x9cffb5, 0x33ff66, 0x33ff66,
	0x1a5c2e, 0xff7f5a, 0x66ff8c, 0xccffd9, 0x2fc45c, 0xccffd9, 0x66ff8c, 0xe6ffec,
};
#else
/* PIXELTAPE crt-amber: bg #080604, fg #ffb000, dim #a57823, accent #ffd67a,
 * danger #ff5f3a, line #8a5f0c. Everything else is a step of the same hue. */
static const uint32_t theme[16] = {
	0x080604, 0xff5f3a, 0xffb000, 0xffd67a, 0xa57823, 0xffd67a, 0xffb000, 0xffb000,
	0x8a5f0c, 0xff7f5a, 0xffc53d, 0xffe6a8, 0xc99a3f, 0xffe6a8, 0xffc53d, 0xfff1cc,
};
#endif

struct cell {
	uint8_t glyph;
	uint8_t color;		/* bg << 4 | fg */
};

enum esc_state {
	ESC_NONE,
	ESC_START,
	ESC_CSI,
};

static int		 cols = HEADLESS_COLS, rows = HEADLESS_ROWS;
static int		 origin_x, origin_y;	/* the whole area */
static int		 text_y;		/* the text, below the bar */
static bool		 display;
static SemaphoreHandle_t lock;
static TaskHandle_t	 renderer;
static uint8_t		 palette[16][2];

/*
 * One of these per virtual terminal: what is on it, where the cursor is,
 * and how much of it needs repainting. Only the one on screen is drawn;
 * the others keep filling in behind it, so a build or a game carries on
 * while you are looking at something else.
 */
struct screen {
	int		 x, y;
	bool		 wrap;		/* last column written, wrap before next glyph */
	uint8_t		 fg, bg;
	bool		 bold, dim, inverse;
	bool		 cursor;
	int		 saved_x, saved_y;
	enum esc_state	 esc;
	bool		 private;
	int		 params[MAX_PARAMS];
	int		 nparams;
	uint32_t	 utf_cp;
	int		 utf_need;

	struct cell	*cells;
	uint16_t	*dirty_lo, *dirty_hi;
};

static struct screen	 screens[CONFIG_PT_VT_COUNT];
static struct screen	*cur = &screens[0];	/* what vt_write() writes to */
static int		 active;		/* what the renderer paints */

/* ------------------------------------------------------------ grid */

static bool screen_alloc(void);

static void mark_on(struct screen *sc, int x, int y)
{
	if (x < 0 || y < 0 || x >= cols || y >= rows || !sc->cells)
		return;
	sc->dirty_lo[y] = MIN(sc->dirty_lo[y], x);
	sc->dirty_hi[y] = MAX(sc->dirty_hi[y], x);
}

static void mark(int x, int y)
{
	mark_on(cur, x, y);
}

static void mark_all(void)
{
	for (int y = 0; y < rows; y++) {
		cur->dirty_lo[y] = 0;
		cur->dirty_hi[y] = cols - 1;
	}
}

static uint8_t blank_color(void)
{
	return cur->bg << 4 | FG_DEFAULT;
}

static uint8_t pen_color(void)
{
	uint8_t fg = cur->fg;

	if (cur->bold && fg < 8)
		fg += 8;
	else if (cur->dim && fg == FG_DEFAULT)
		fg = FG_DIM;
	return cur->inverse ? fg << 4 | cur->bg : cur->bg << 4 | fg;
}

static void set_cell(int x, int y, uint8_t glyph, uint8_t color)
{
	struct cell *c = &cur->cells[y * cols + x];

	if (c->glyph == glyph && c->color == color)
		return;
	c->glyph = glyph;
	c->color = color;
	mark(x, y);
}

static void clear_span(int y, int from, int to)
{
	for (int x = from; x < to; x++)
		set_cell(x, y, ' ' - FONT_FIRST, blank_color());
}

static void clear_rows(int from, int to)
{
	for (int y = from; y < to; y++)
		clear_span(y, 0, cols);
}

/* Move rows [top, rows) by `n`: positive scrolls up, negative down. */
static void shift_rows(int top, int n)
{
	int count = rows - top;

	if (!n || count <= 0)
		return;
	if (abs(n) >= count) {
		clear_rows(top, rows);
		return;
	}
	if (n > 0) {
		memmove(&cur->cells[top * cols], &cur->cells[(top + n) * cols], (count - n) * cols * sizeof(*cur->cells));
		for (int i = (rows - n) * cols; i < rows * cols; i++)
			cur->cells[i] = (struct cell) { ' ' - FONT_FIRST, blank_color() };
	} else {
		n = -n;
		memmove(&cur->cells[(top + n) * cols], &cur->cells[top * cols], (count - n) * cols * sizeof(*cur->cells));
		for (int i = top * cols; i < (top + n) * cols; i++)
			cur->cells[i] = (struct cell) { ' ' - FONT_FIRST, blank_color() };
	}
	for (int y = top; y < rows; y++) {
		cur->dirty_lo[y] = 0;
		cur->dirty_hi[y] = cols - 1;
	}
}

static void newline(void)
{
	cur->x = 0;
	cur->wrap = false;
	if (cur->y == rows - 1)
		shift_rows(0, 1);
	else
		cur->y++;
}

static void put_glyph(uint8_t glyph)
{
	if (cur->wrap)
		newline();
	set_cell(cur->x, cur->y, glyph, pen_color());
	if (cur->x == cols - 1)
		cur->wrap = true;
	else
		cur->x++;
}

static uint8_t glyph_of(uint32_t cp)
{
	if (cp >= FONT_FIRST && cp < FONT_FIRST + FONT_ASCII)
		return cp - FONT_FIRST;
	/* the table is sorted by code point */
	size_t lo = 0, hi = sizeof(font_extra) / sizeof(font_extra[0]);

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;

		if (font_extra[mid].cp == cp)
			return font_extra[mid].glyph;
		if (font_extra[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	return FONT_UNKNOWN;
}

static void put_codepoint(uint32_t cp)
{
	put_glyph(glyph_of(cp));
}

static void reset_pen(void)
{
	cur->fg = FG_DEFAULT;
	cur->bg = BG_DEFAULT;
	cur->bold = cur->dim = cur->inverse = false;
}

/* ------------------------------------------------------------ escapes */

static int param(int i, int def)
{
	return i < cur->nparams && cur->params[i] > 0 ? cur->params[i] : def;
}

static void sgr(void)
{
	if (!cur->nparams)
		reset_pen();
	for (int i = 0; i < cur->nparams; i++) {
		int p = cur->params[i];

		if (p == 0)
			reset_pen();
		else if (p == 1)
			cur->bold = true;
		else if (p == 2)
			cur->dim = true;
		else if (p == 22)
			cur->bold = cur->dim = false;
		else if (p == 7)
			cur->inverse = true;
		else if (p == 27)
			cur->inverse = false;
		else if (p >= 30 && p <= 37)
			cur->fg = p - 30;
		else if (p == 39)
			cur->fg = FG_DEFAULT;
		else if (p >= 40 && p <= 47)
			cur->bg = p - 40;
		else if (p == 49)
			cur->bg = BG_DEFAULT;
		else if (p >= 90 && p <= 97)
			cur->fg = p - 90 + 8;
		else if (p >= 100 && p <= 107)
			cur->bg = p - 100 + 8;
	}
}

static void csi(uint8_t final)
{
	int mode = cur->nparams ? cur->params[0] : 0;

	switch (final) {
	case 'A':
		cur->y = MAX(0, cur->y - param(0, 1));
		break;
	case 'B':
		cur->y = MIN(rows - 1, cur->y + param(0, 1));
		break;
	case 'C':
		cur->x = MIN(cols - 1, cur->x + param(0, 1));
		break;
	case 'D':
		cur->x = MAX(0, cur->x - param(0, 1));
		break;
	case 'G':
		cur->x = CLAMP(param(0, 1) - 1, 0, cols - 1);
		break;
	case 'd':
		cur->y = CLAMP(param(0, 1) - 1, 0, rows - 1);
		break;
	case 'H':
	case 'f':
		cur->y = CLAMP(param(0, 1) - 1, 0, rows - 1);
		cur->x = CLAMP(param(1, 1) - 1, 0, cols - 1);
		break;
	case 'J':
		if (mode == 0) {
			clear_span(cur->y, cur->x, cols);
			clear_rows(cur->y + 1, rows);
		} else if (mode == 1) {
			clear_rows(0, cur->y);
			clear_span(cur->y, 0, cur->x + 1);
		} else {
			clear_rows(0, rows);
		}
		break;
	case 'K':
		if (mode == 0)
			clear_span(cur->y, cur->x, cols);
		else if (mode == 1)
			clear_span(cur->y, 0, cur->x + 1);
		else
			clear_span(cur->y, 0, cols);
		return;			/* erasing does not cancel a pending wrap */
	case 'L':
		shift_rows(cur->y, -param(0, 1));
		break;
	case 'M':
		shift_rows(cur->y, param(0, 1));
		break;
	case 'm':
		sgr();
		return;
	case 'h':
	case 'l':
		if (cur->private && mode == 25)
			cur->cursor = final == 'h';
		return;
	case 's':
		cur->saved_x = cur->x;
		cur->saved_y = cur->y;
		return;
	case 'u':
		cur->x = cur->saved_x;
		cur->y = cur->saved_y;
		break;
	default:
		return;
	}
	cur->wrap = false;
}

static void vt_byte(uint8_t c)
{
	switch (cur->esc) {
	case ESC_START:
		cur->esc = ESC_NONE;
		if (c == '[') {
			cur->esc = ESC_CSI;
			cur->private = false;
			cur->nparams = 0;
			memset(cur->params, 0, sizeof(cur->params));
		} else if (c == '7') {
			cur->saved_x = cur->x;
			cur->saved_y = cur->y;
		} else if (c == '8') {
			cur->x = cur->saved_x;
			cur->y = cur->saved_y;
			cur->wrap = false;
		} else if (c == 'c') {
			reset_pen();
			clear_rows(0, rows);
			cur->x = cur->y = 0;
			cur->wrap = false;
			cur->cursor = true;
		}
		return;
	case ESC_CSI:
		if (c == '?') {
			cur->private = true;
		} else if (c >= '0' && c <= '9') {
			if (!cur->nparams)
				cur->nparams = 1;
			int *p = &cur->params[cur->nparams - 1];
			if (*p < 10000)
				*p = *p * 10 + (c - '0');
		} else if (c == ';') {
			if (!cur->nparams)
				cur->nparams = 1;
			if (cur->nparams < MAX_PARAMS)
				cur->params[cur->nparams++] = 0;
		} else {
			cur->esc = ESC_NONE;
			if (c >= 0x40 && c <= 0x7e)
				csi(c);
		}
		return;
	case ESC_NONE:
		break;
	}

	if (cur->utf_need) {
		if ((c & 0xc0) == 0x80) {
			cur->utf_cp = cur->utf_cp << 6 | (c & 0x3f);
			if (!--cur->utf_need)
				put_codepoint(cur->utf_cp);
			return;
		}
		cur->utf_need = 0;
		put_glyph(FONT_UNKNOWN);
	}
	if (c >= 0x80) {
		if ((c & 0xe0) == 0xc0) {
			cur->utf_cp = c & 0x1f;
			cur->utf_need = 1;
		} else if ((c & 0xf0) == 0xe0) {
			cur->utf_cp = c & 0x0f;
			cur->utf_need = 2;
		} else if ((c & 0xf8) == 0xf0) {
			cur->utf_cp = c & 0x07;
			cur->utf_need = 3;
		} else {
			put_glyph(FONT_UNKNOWN);
		}
		return;
	}

	switch (c) {
	case 0x1b:
		cur->esc = ESC_START;
		break;
	case '\r':
		cur->x = 0;
		cur->wrap = false;
		break;
	case '\n':
		newline();
		break;
	case '\b':
		if (cur->wrap)
			cur->wrap = false;
		else if (cur->x)
			cur->x--;
		break;
	case '\t':
		cur->x = MIN((cur->x / 8 + 1) * 8, cols - 1);
		cur->wrap = false;
		break;
	default:
		if (c >= 0x20 && c < 0x7f)
			put_glyph(c - FONT_FIRST);
		break;
	}
}

void vt_write(const char *s, size_t n)
{
	if (!cur->cells)
		return;
	xSemaphoreTake(lock, portMAX_DELAY);
	for (size_t i = 0; i < n; i++)
		vt_byte((uint8_t)s[i]);
	xSemaphoreGive(lock);
	if (renderer)
		xTaskNotifyGive(renderer);
}

void vt_size(int *c, int *r)
{
	*c = cols;
	*r = rows;
}

bool vt_has_display(void)
{
	return display;
}

/* ------------------------------------------------------------ renderer */

static void draw_cell(uint8_t *px, int span_w, int cell_x, uint8_t glyph, uint8_t color)
{
	const uint8_t *fg = palette[color & 0x0f];
	const uint8_t *bg = palette[color >> 4];
	const uint8_t *bits = font5x8[glyph < FONT_GLYPHS ? glyph : FONT_UNKNOWN];
	const bool block = glyph == FONT_BLOCK;

	for (int py = 0; py < CELL_H; py++) {
		int gy = py / SCALE - 1;
		uint8_t rowbits = block ? 0x1f : gy >= 0 && gy < FONT_H ? bits[gy] : 0;
		uint8_t *o = px + (py * span_w + cell_x) * 2;

		for (int x = 0; x < CELL_W; x++) {
			int gx = x / SCALE;
			bool on = block || (gx < FONT_W && ((rowbits >> (FONT_W - 1 - gx)) & 1));
			const uint8_t *c = on ? fg : bg;
			*o++ = c[0];
			*o++ = c[1];
		}
	}
}

/*
 * A program that draws on the panel itself holds the screen, but only for
 * its own terminal: switch to another and the renderer paints that one as
 * usual, while the program draws nothing until its terminal is back in
 * front. `panel` keeps the two from drawing at once.
 */
static volatile bool held_by_program;
static volatile int hold_pid;		/* the program holding it, 0 for none */
static volatile int hold_vt;		/* the terminal it runs on */
static volatile unsigned hold_gen;	/* its terminal's returns to the front */
static SemaphoreHandle_t panel;

static volatile bool repaint_all;	/* the screen changed under us */

#if CONFIG_PT_STATUS_LINE

/* UTF-8 into glyphs, at most `max` of them; how many there were. */
static int to_glyphs(const char *s, uint8_t *out, int max)
{
	int n = 0;

	while (*s && n < max) {
		uint32_t cp = (uint8_t)*s++;
		int more = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;

		if (more)
			cp &= 0x3f >> more;
		while (more-- > 0 && (*s & 0xc0) == 0x80)
			cp = cp << 6 | (*s++ & 0x3f);
		out[n++] = glyph_of(cp);
	}
	return n;
}

/*
 * The line along the bottom: which terminal you are on, the time, the
 * next alarm if it is within a day, what is left in the battery and
 * what the radio is doing. While an alarm rings the whole line is that
 * alarm, flashing. It is painted once a second by the renderer, straight
 * to the panel, and is not part of any terminal's text -- nothing a
 * program writes can disturb it.
 */
static void draw_status(uint8_t *pixels)
{
	uint8_t bar[128], right[48];
	char text[96];
	struct battery_status bat;
	struct wifi_info net;
	struct alarm ring;
	time_t now = time(NULL), next;
	struct tm tm;
	int n, mid_at, right_at;
	uint8_t color = (uint8_t)(FG_DEFAULT << 4 | BG_DEFAULT);	/* inverted */

	memset(bar, ' ' - FONT_FIRST, sizeof(bar));
	if (alarm_ringing(&ring)) {
		const char *keys = ring.kind == ALARM_CHIME ? "any key: ok " :
				   "any key: snooze  esc: stop ";

		n = to_glyphs(keys, right, sizeof(right));
		right_at = cols - n > 4 ? cols - n : 4;
		bar[1] = FONT_ALARM;
		snprintf(text, sizeof(text), "%02d:%02d %s", ring.hour, ring.min,
			 ring.label[0] ? ring.label : "alarm");
		to_glyphs(text, bar + 3, right_at - 4);
		memcpy(bar + right_at, right, cols - right_at);
		if (now & 1)
			color = (uint8_t)(BG_DEFAULT << 4 | FG_DEFAULT);	/* flashing */
	} else {
		localtime_r(&now, &tm);
		snprintf(text, sizeof(text), " %d/%d", vt_active() + 1, CONFIG_PT_VT_COUNT);
		if (!wifi_state(&net) && net.up)
			snprintf(text + strlen(text), sizeof(text) - strlen(text), "  %s", net.ssid);
		n = to_glyphs(text, bar, cols / 2 - 3);
		mid_at = (cols - 5) / 2;
		snprintf(text, sizeof(text), "%02d:%02d", tm.tm_hour, tm.tm_min);
		if (mid_at > n)
			to_glyphs(text, bar + mid_at, 5);

		n = 0;
		next = alarm_next_any(NULL);
		if (next && next - now < 24 * 3600) {
			struct tm at;

			localtime_r(&next, &at);
			right[n++] = FONT_ALARM;
			snprintf(text, sizeof(text), "%02d:%02d  ", at.tm_hour, at.tm_min);
			n += to_glyphs(text, right + n, sizeof(right) - n);
		}
		/* no cell fitted: nothing to show */
		if (!battery_status(&bat) && bat.state != BATTERY_NONE && bat.state != BATTERY_USB) {
			if (bat.state == BATTERY_CHARGING) {
				right[n++] = FONT_BOLT;
				right[n++] = ' ' - FONT_FIRST;
			}
			snprintf(text, sizeof(text), "%d%% ", bat.percent);
			n += to_glyphs(text, right + n, sizeof(right) - n);
		}
		right_at = cols - n;
		if (right_at > mid_at + 5)
			memcpy(bar + right_at, right, n);
	}

	for (int x = 0; x < cols; x++)
		draw_cell(pixels, cols * CELL_W, x * CELL_W, bar[x], color);
	lcd_draw(origin_x, origin_y, cols * CELL_W, CELL_H, pixels);
}

#else
static void draw_status(uint8_t *pixels) { }
#endif

static struct screen *onscreen(void)
{
	return &screens[active];
}

static void render_task(void *arg)
{
	const int64_t blink_us = CONFIG_PT_CURSOR_BLINK_MS * 1000LL;
	uint8_t *pixels = lcd_alloc_buffer((size_t)cols * CELL_W * CELL_H * 2);
	struct cell *row = malloc(cols * sizeof(*row));
	int64_t next_blink = esp_timer_get_time() + blink_us;
	int64_t next_status = 0;
	bool blink_on = true, cur_shown = false;
	int cur_x = -1, cur_y = -1;

	if (!pixels || !row) {
		klog("vt: no memory for the renderer");
		vTaskDelete(NULL);
	}
	for (;;) {
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(blink_us ? CONFIG_PT_CURSOR_BLINK_MS : 1000));
		vTaskDelay(pdMS_TO_TICKS(8));	/* let a burst of output land in one frame */
		ulTaskNotifyTake(pdTRUE, 0);
		if (held_by_program && hold_pid && !proc_alive(hold_pid)) {
			held_by_program = false;	/* it has gone without giving it back */
			hold_pid = 0;
			klog("vt: a program ended holding the screen; taking it back");
			xSemaphoreTake(lock, portMAX_DELAY);
			mark_all();
			xSemaphoreGive(lock);
			repaint_all = true;
		}
		xSemaphoreTake(panel, portMAX_DELAY);
		if (held_by_program && hold_vt == active) {
			xSemaphoreGive(panel);
			continue;		/* the program in front owns the screen */
		}

		int64_t now = esp_timer_get_time();
		if (blink_us && now >= next_blink) {
			blink_on = !blink_on;
			next_blink = now + blink_us;
		}

		struct screen *sc = onscreen();

		if (repaint_all) {
			/* a different terminal: clear once, then draw it */
			repaint_all = false;
			lcd_fill(0, 0, lcd_width(), lcd_height(),
				 palette[BG_DEFAULT][0] << 8 | palette[BG_DEFAULT][1]);
			cur_x = cur_y = -1;
			next_status = 0;
		}
		if (esp_timer_get_time() >= next_status) {
			next_status = esp_timer_get_time() + 1000000;
			draw_status(pixels);
		}
		xSemaphoreTake(lock, portMAX_DELAY);
		bool show = sc->cursor && (blink_on || !blink_us);
		int cx = sc->x, cy = sc->y;
		if (cx != cur_x || cy != cur_y || show != cur_shown) {
			mark_on(sc, cur_x, cur_y);
			mark_on(sc, cx, cy);
			cur_x = cx;
			cur_y = cy;
			cur_shown = show;
		}
		xSemaphoreGive(lock);

		for (int y = 0; y < rows; y++) {
			xSemaphoreTake(lock, portMAX_DELAY);
			sc = onscreen();	/* it may have been switched */
			int lo = sc->dirty_lo[y], hi = sc->dirty_hi[y];
			if (lo <= hi) {
				memcpy(row, &sc->cells[y * cols + lo], (hi - lo + 1) * sizeof(*row));
				sc->dirty_lo[y] = CLEAN_LO;
				sc->dirty_hi[y] = 0;
			}
			xSemaphoreGive(lock);
			if (lo > hi)
				continue;

			int n = hi - lo + 1;
			for (int i = 0; i < n; i++) {
				uint8_t color = row[i].color;
				if (cur_shown && y == cur_y && lo + i == cur_x)
					color = (uint8_t)(color << 4 | color >> 4);
				draw_cell(pixels, n * CELL_W, i * CELL_W, row[i].glyph, color);
			}
			lcd_draw(origin_x + lo * CELL_W, text_y + y * CELL_H, n * CELL_W, CELL_H, pixels);
		}
		xSemaphoreGive(panel);
	}
}

/* ------------------------------------------------------------ setup */

void vt_init(void)
{
	display = lcd_width() > 0;
	if (display) {
		int screen_rows = lcd_height() / CELL_H;

		cols = lcd_width() / CELL_W;
#if CONFIG_PT_STATUS_LINE
		rows = screen_rows - 1;		/* the top line is the status bar */
#else
		rows = screen_rows;
#endif
		origin_x = (lcd_width() - cols * CELL_W) / 2;
		origin_y = (lcd_height() - screen_rows * CELL_H) / 2;
		text_y = origin_y + (screen_rows - rows) * CELL_H;
	}
	for (int i = 0; i < 16; i++) {
		uint32_t rgb = theme[i];
		uint16_t v = ((rgb >> 16) & 0xf8) << 8 | ((rgb >> 8) & 0xfc) << 3 | (rgb & 0xff) >> 3;
		palette[i][0] = v >> 8;
		palette[i][1] = v;
	}

	lock = xSemaphoreCreateMutex();
	panel = xSemaphoreCreateMutex();
	if (!lock || !panel || !screen_alloc()) {
		klog("vt: out of memory");
		return;
	}
	reset_pen();
	cur->cursor = true;
	for (int i = 0; i < cols * rows; i++)
		cur->cells[i] = (struct cell) { ' ' - FONT_FIRST, blank_color() };
	for (int y = 0; y < rows; y++) {
		cur->dirty_lo[y] = CLEAN_LO;
		cur->dirty_hi[y] = 0;
	}
	klog("vt: %dx%d console%s", cols, rows, display ? "" : " (no display)");
}

/*
 * A full-screen program (a game, a picture viewer) owns the panel while
 * it runs. The renderer would otherwise keep repainting the text
 * underneath it -- the blinking cursor alone is enough to do that twice a
 * second.
 */
/*
 * A screen's memory. The text is in PSRAM -- it is read once a frame and
 * never from an interrupt -- so several terminals cost nothing that the
 * scarce internal RAM wants.
 */
static bool screen_alloc(void)
{
	for (int i = 0; i < CONFIG_PT_VT_COUNT; i++) {
		struct screen *sc = &screens[i];

		if (sc->cells)
			continue;
		sc->cells = heap_caps_malloc_prefer((size_t)cols * rows * sizeof(*sc->cells), 2,
						    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
						    MALLOC_CAP_8BIT);
		sc->dirty_lo = heap_caps_malloc_prefer(rows * sizeof(*sc->dirty_lo), 2,
						       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
						       MALLOC_CAP_8BIT);
		sc->dirty_hi = heap_caps_malloc_prefer(rows * sizeof(*sc->dirty_hi), 2,
						       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
						       MALLOC_CAP_8BIT);
		if (!sc->cells || !sc->dirty_lo || !sc->dirty_hi)
			return false;
		for (int k = 0; k < cols * rows; k++)
			sc->cells[k] = (struct cell) { ' ' - FONT_FIRST, 0 };
		for (int y = 0; y < rows; y++) {
			sc->dirty_lo[y] = CLEAN_LO;
			sc->dirty_hi[y] = 0;
		}
		sc->cursor = true;
		sc->fg = FG_DEFAULT;
		sc->bg = BG_DEFAULT;
	}
	return true;
}

/*
 * Writing to a terminal that is not the one on screen: the text lands in
 * its own buffer and nothing is painted until someone switches to it.
 */
void vt_write_on(int which, const char *s, size_t n)
{
	struct screen *was;

	if (which < 0 || which >= CONFIG_PT_VT_COUNT || !screens[which].cells)
		return;
	xSemaphoreTake(lock, portMAX_DELAY);
	was = cur;
	cur = &screens[which];
	for (size_t i = 0; i < n; i++)
		vt_byte((uint8_t)s[i]);
	cur = was;
	xSemaphoreGive(lock);
	if (which == active && renderer)
		xTaskNotifyGive(renderer);
}

int vt_count(void)
{
	return CONFIG_PT_VT_COUNT;
}

int vt_active(void)
{
	return active;
}

/*
 * Put another terminal on the screen.
 *
 * This is called from whatever saw the key, which may be a driver task
 * with a small stack, so it does no drawing: it hands the repaint to the
 * renderer, which is the only task that ever touches the panel for the
 * terminal.
 */
int vt_switch(int which)
{
	if (which < 0 || which >= CONFIG_PT_VT_COUNT)
		return -EINVAL;
	if (which == active)
		return 0;
	xSemaphoreTake(lock, portMAX_DELAY);
	active = which;
	cur = &screens[which];
	mark_all();
	repaint_all = true;
	if (held_by_program && which == hold_vt)
		hold_gen++;		/* the program's turn to paint it all */
	xSemaphoreGive(lock);
	if (renderer)
		xTaskNotifyGive(renderer);
	return 0;
}

/*
 * A program that draws on the panel itself -- a picture, a clip, a game --
 * holds the screen while it does, for the terminal it runs on. The renderer
 * notices if it ends without letting go (killed from another terminal, say)
 * and takes the screen back, rather than leave the console frozen behind a
 * picture.
 */
void vt_hold_screen(bool held)
{
	struct proc *p = proc_current();
	int vt = tty_of_current();

	if (!display)
		return;
	xSemaphoreTake(panel, portMAX_DELAY);	/* not halfway through a repaint */
	hold_vt = vt >= 0 ? vt : active;
	hold_pid = held && p ? p->pid : 0;
	held_by_program = held;
	xSemaphoreGive(panel);
	if (!held)
		vt_redraw();
}

bool vt_screen_front(void)
{
	return held_by_program && hold_vt == active;
}

/*
 * Around each thing the holder draws: whether its terminal is in front --
 * if not, it draws nothing -- with the renderer kept off the panel until
 * vt_screen_end().
 */
bool vt_screen_begin(void)
{
	if (!display)
		return false;
	xSemaphoreTake(panel, portMAX_DELAY);
	return vt_screen_front();
}

void vt_screen_end(void)
{
	if (display)
		xSemaphoreGive(panel);
}

unsigned vt_screen_gen(void)
{
	return hold_gen;
}

void vt_redraw(void)
{
	if (!display || !cur->cells)
		return;
	lcd_fill(0, 0, lcd_width(), lcd_height(), palette[BG_DEFAULT][0] << 8 | palette[BG_DEFAULT][1]);
	xSemaphoreTake(lock, portMAX_DELAY);
	mark_all();
	xSemaphoreGive(lock);
	if (renderer)
		xTaskNotifyGive(renderer);
}

void vt_start_display(void)
{
	if (!display || !cur->cells)
		return;
	lcd_fill(0, 0, lcd_width(), lcd_height(), palette[BG_DEFAULT][0] << 8 | palette[BG_DEFAULT][1]);
	xSemaphoreTake(lock, portMAX_DELAY);
	mark_all();
	xSemaphoreGive(lock);
	xTaskCreatePinnedToCore(render_task, "kvt", 4096, NULL, 4, &renderer, 0);
}
