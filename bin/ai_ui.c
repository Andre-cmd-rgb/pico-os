/*
 * A bounded transcript beneath the system bar, with a prompt that never
 * scrolls away. Keep rendered cells, including their colours: streaming
 * Markdown is laid out once by ai.c, and looking back does not change it.
 */
#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "ai_ui.h"
#include "json.h"
#include "util.h"

#define TRANSCRIPT_ROWS 512
#define INPUT_HISTORY 32
#define BOLD 0x100
#define DIM 0x200

struct chat_cell {
	char text[4];
	uint16_t style;
	uint8_t len;
};

struct ai_ui {
	struct chat_cell *cells;
	int cols, rows, body, first, count, col, back;
	int style, esc, param;
	char utf[4];
	int utf_len, utf_left;
	bool dirty, cancelled;
	int64_t drawn;
	char mode[16], status[80];
	char folder[PT_PATH_MAX], model[128], effort[16];
	double cost;
	uint64_t tokens;
	bool partial;
	char input[AI_INPUT_MAX], draft[AI_INPUT_MAX];
	int pos, len, start;
	char *history[INPUT_HISTORY];
	int nhistory, recalled;
	char prefix[AI_INPUT_MAX];
	int completion;
};

static const char *const commands[] = {
	"/study", "/code", "/web", "/voice", "/model", "/new", "/save",
	"/cost", "/help", "/quit", "/search", "/models", "/v", "/q", "/exit", "/?",
	"/folder", "/effort", "/stop", "/sessions", "/resume", "/stats", "/history",
};

static struct chat_cell *row(struct ai_ui *ui, int n)
{
	return ui->cells + ((ui->first + n) % TRANSCRIPT_ROWS) * ui->cols;
}

static int max_back(const struct ai_ui *ui)
{
	return ui->count > ui->body ? ui->count - ui->body : 0;
}

static void next_row(struct ai_ui *ui)
{
	if (ui->count == TRANSCRIPT_ROWS)
		ui->first = (ui->first + 1) % TRANSCRIPT_ROWS;
	else
		ui->count++;
	memset(row(ui, ui->count - 1), 0, ui->cols * sizeof(*ui->cells));
	ui->col = 0;
	if (ui->back && ui->back < max_back(ui))
		ui->back++;
}

static void glyph(struct ai_ui *ui, const char *s, int n)
{
	struct chat_cell *cell;

	if (ui->col == ui->cols)
		next_row(ui);
	cell = row(ui, ui->count - 1) + ui->col++;
	memcpy(cell->text, s, n);
	cell->len = n;
	cell->style = ui->style;
}

static void sgr(struct ai_ui *ui)
{
	int p = ui->param;

	if (!p)
		ui->style = 0;
	else if (p == 1)
		ui->style |= BOLD;
	else if (p == 2)
		ui->style |= DIM;
	else if (p == 22)
		ui->style &= ~(BOLD | DIM);
	else if (p == 39)
		ui->style &= ~0xff;
	else if ((p >= 30 && p <= 37) || (p >= 90 && p <= 97))
		ui->style = (ui->style & ~0xff) | p;
}

void ai_ui_write(struct ai_ui *ui, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char c = s[i];

		if (ui->esc == 1) {
			ui->esc = c == '[' ? 2 : 0;
			ui->param = 0;
			continue;
		}
		if (ui->esc == 2) {
			if (c >= '0' && c <= '9') {
				if (ui->param < 1000)
					ui->param = ui->param * 10 + c - '0';
			} else if (c == ';') {
				sgr(ui);
				ui->param = 0;
			} else {
				if (c == 'm')
					sgr(ui);
				else if (c == 'K')
					memset(row(ui, ui->count - 1) + ui->col, 0,
					       (ui->cols - ui->col) * sizeof(*ui->cells));
				ui->esc = 0;
			}
			continue;
		}
		if (ui->utf_left) {
			if ((c & 0xc0) == 0x80) {
				ui->utf[ui->utf_len++] = c;
				if (!--ui->utf_left)
					glyph(ui, ui->utf, ui->utf_len);
				continue;
			}
			ui->utf_left = 0;
			glyph(ui, "?", 1);
		}
		if (c == 0x1b)
			ui->esc = 1;
		else if (c == '\n')
			next_row(ui);
		else if (c == '\r')
			ui->col = 0;
		else if (c == '\t') {
			int spaces = 4 - ui->col % 4;

			while (spaces--)
				glyph(ui, " ", 1);
		} else if (c >= 0x20 && c < 0x7f)
			glyph(ui, s + i, 1);
		else if (c >= 0xc2 && c <= 0xf4) {
			ui->utf[0] = c;
			ui->utf_len = 1;
			ui->utf_left = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
		}
	}
	ui->dirty = true;
}

static int prev_char(const char *s, int pos)
{
	if (pos)
		pos--;
	while (pos && ((unsigned char)s[pos] & 0xc0) == 0x80)
		pos--;
	return pos;
}

static int next_char(const char *s, int pos)
{
	if (s[pos])
		pos++;
	while (s[pos] && ((unsigned char)s[pos] & 0xc0) == 0x80)
		pos++;
	return pos;
}

static int matches(struct ai_ui *ui, int *found)
{
	const char *s = ui->completion >= 0 ? ui->prefix : ui->input;
	int n = 0;

	if (*s != '/' || strchr(s, ' '))
		return 0;
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
		if (!strncmp(commands[i], s, strlen(s)))
			found[n++] = i;
	return n;
}

static void bar(struct jbuf *b, int y, const char *s, int cols, const char *style)
{
	size_t n = utf8_prefix(s, strlen(s), cols);
	int width = utf8_width(s, n);

	jb_printf(b, "\x1b[%d;1H\x1b[0m\x1b[K%s", y, style ? style : "");
	jb_add(b, s, n);
	for (int i = width; i < cols; i++)
		jb_puts(b, " ");
	/* Erasing after the last column would remove its background too. */
	jb_puts(b, "\x1b[0m");
}

void ai_ui_draw(struct ai_ui *ui, bool force)
{
	struct jbuf b = { 0 };
	char title[256], left[64], right[64];
	int top, cursor;
	int left_len, right_len, pad;
	int64_t now = pt_uptime_us();

	if (!ui || (!force && (!ui->dirty || now - ui->drawn < 50000)))
		return;
	jb_puts(&b, "\x1b[?25l");
	left_len = snprintf(left, sizeof(left), " AI %s ", ui->mode);
	if (ui->tokens >= 1000)
		right_len = snprintf(right, sizeof(right), " %.1fk%s tok  $%.4f%s ",
				     (double)ui->tokens / 1000, ui->partial ? "+" : "",
				     ui->cost, ui->partial ? "+" : "");
	else
		right_len = snprintf(right, sizeof(right), " %llu%s tok  $%.4f%s ",
				     (unsigned long long)ui->tokens, ui->partial ? "+" : "",
				     ui->cost, ui->partial ? "+" : "");
	
	pad = ui->cols - left_len - right_len;
	if (pad < 0)
		pad = 0;
	snprintf(title, sizeof(title), "%s%*s%s", left, pad, "", right);

	bar(&b, 1, title, ui->cols, "\x1b[7m");

	top = max_back(ui) - ui->back;
	for (int y = 0; y < ui->body; y++) {
		int style = -1;

		jb_printf(&b, "\x1b[%d;1H\x1b[0m\x1b[K", y + 2);
		if (top + y >= ui->count)
			continue;
		for (int x = 0; x < ui->cols; x++) {
			struct chat_cell *c = row(ui, top + y) + x;

			if (style != c->style) {
				style = c->style;
				jb_printf(&b, "\x1b[0;%d%sm%s", style & 0xff ? style & 0xff : 39,
					  style & BOLD ? ";1" : "", style & DIM ? "\x1b[2m" : "");
			}
			jb_add(&b, c->len ? c->text : " ", c->len ? c->len : 1);
		}
	}
	
	jb_printf(&b, "\x1b[%d;1H\x1b[0;2m", ui->rows - 1);
	for (int x = 0; x < ui->cols; x++)
		jb_puts(&b, "-");
	jb_puts(&b, "\x1b[0m");

	if (ui->start > ui->pos)
		ui->start = ui->pos;
	while (utf8_width(ui->input + ui->start, ui->pos - ui->start) >= ui->cols - 2)
		ui->start = next_char(ui->input, ui->start);
	cursor = 3 + utf8_width(ui->input + ui->start, ui->pos - ui->start);
	
	jb_printf(&b, "\x1b[%d;1H\x1b[0m\x1b[K\x1b[1m> \x1b[0m", ui->rows);
	
	jb_add(&b, ui->input + ui->start,
	       utf8_prefix(ui->input + ui->start, ui->len - ui->start, ui->cols - 2));
	jb_printf(&b, "\x1b[%d;%dH\x1b[?25h", ui->rows, cursor);
	if (!b.oom) {
		pt_write(PT_STDOUT, b.p, b.len);
		ui->dirty = false;
		ui->drawn = now;
	}
	jb_free(&b);
}

struct ai_ui *ai_ui_open(int cols, int rows)
{
	struct ai_ui *ui;
	int app_scroll = 1;

	if (cols < 12 || cols > 240 || rows < 5 || rows > 100)
		return NULL;
	ui = pt_calloc(1, sizeof(*ui));
	if (!ui)
		return NULL;
	ui->cells = pt_calloc((size_t)TRANSCRIPT_ROWS * cols, sizeof(*ui->cells));
	if (!ui->cells) {
		pt_free(ui);
		return NULL;
	}
	ui->cols = cols;
	ui->rows = rows;
	ui->body = rows - 3;
	ui->count = 1;
	ui->completion = -1;
	pt_tty_raw(PT_STDIN, true);
	pt_ioctl(PT_STDIN, PT_TTY_SETSCROLL, &app_scroll);
	pt_puts("\x1b[0m\x1b[2J\x1b[H");
	return ui;
}

void ai_ui_close(struct ai_ui *ui)
{
	int app_scroll = 0;

	if (!ui)
		return;
	pt_ioctl(PT_STDIN, PT_TTY_SETSCROLL, &app_scroll);
	pt_puts("\x1b[0m\x1b[?25h\x1b[2J\x1b[H");
	pt_tty_raw(PT_STDIN, false);
	for (int i = 0; i < ui->nhistory; i++)
		pt_free(ui->history[i]);
	pt_free(ui->cells);
	pt_free(ui);
}

void ai_ui_mode(struct ai_ui *ui, const char *mode)
{
	if (ui) {
		strlcpy(ui->mode, mode, sizeof(ui->mode));
		ui->dirty = true;
	}
}

void ai_ui_context(struct ai_ui *ui, const char *folder, const char *model, const char *effort)
{
	if (!ui)
		return;
	strlcpy(ui->folder, folder, sizeof(ui->folder));
	strlcpy(ui->model, model, sizeof(ui->model));
	strlcpy(ui->effort, effort, sizeof(ui->effort));
	ui->dirty = true;
}

void ai_ui_usage(struct ai_ui *ui, double cost, uint64_t tokens, bool partial)
{
	if (ui) {
		ui->cost = cost;
		ui->tokens = tokens;
		ui->partial = partial;
		ui->dirty = true;
	}
}

void ai_ui_reset(struct ai_ui *ui)
{
	if (!ui)
		return;
	memset(ui->cells, 0, (size_t)TRANSCRIPT_ROWS * ui->cols * sizeof(*ui->cells));
	ui->first = ui->col = ui->back = ui->style = ui->esc = ui->param = 0;
	ui->utf_len = ui->utf_left = 0;
	ui->count = 1;
	ui->cancelled = false;
	ui->status[0] = '\0';
	ai_ui_draft(ui, "");
	ui->dirty = true;
}

/* Filtering ignores punctuation, so a short model name is enough. */
static bool choice_matches(const char *item, const char *filter)
{
	char a[256], b[80];
	int n = 0;

	for (; *item && n < (int)sizeof(a) - 1; item++)
		if ((*item == 'v' || *item == 'V') && isdigit((unsigned char)item[1]))
			continue;
		else if (isalnum((unsigned char)*item))
			a[n++] = tolower((unsigned char)*item);
	a[n] = '\0';
	n = 0;
	for (; *filter && n < (int)sizeof(b) - 1; filter++)
		if ((*filter == 'v' || *filter == 'V') && isdigit((unsigned char)filter[1]))
			continue;
		else if (isalnum((unsigned char)*filter))
			b[n++] = tolower((unsigned char)*filter);
	b[n] = '\0';
	return strstr(a, b) != NULL;
}

int ai_ui_choose(struct ai_ui *ui, const char *title, const char *const *items, int n, int selected)
{
	char filter[80] = "", line[320];
	int *found = pt_malloc((size_t)n * sizeof(*found));
	int choice = 0, top = 0, result = -1;
	int rows = ui ? ui->rows : 23, cols = ui ? ui->cols : 53;
	int visible = rows - 4;

	if (!found || n < 1) {
		pt_free(found);
		return -1;
	}
	pt_tty_raw(PT_STDIN, true);
	for (;;) {
		struct jbuf b = { 0 };
		int count = 0, k;

		for (int i = 0; i < n; i++)
			if (choice_matches(items[i], filter))
				found[count++] = i;
		if (selected >= 0) {
			for (int i = 0; i < count; i++)
				if (found[i] == selected)
					choice = i;
			selected = -1;
		}
		if (choice >= count)
			choice = count > 0 ? count - 1 : 0;
		if (choice < 0)
			choice = 0;
		if (!count)
			top = 0;
		if (choice < top)
			top = choice;
		if (choice >= top + visible)
			top = choice - visible + 1;
		jb_puts(&b, "\x1b[?25l");
		bar(&b, 1, title, cols, "\x1b[7m");
		snprintf(line, sizeof(line), " %d choices; type to filter", count);
		bar(&b, 2, line, cols, "\x1b[2m");
		for (int y = 0; y < visible; y++) {
			int i = top + y;

			snprintf(line, sizeof(line), "%s%s", i == choice ? "> " : "  ",
				 i < count ? items[found[i]] : "");
			bar(&b, y + 3, line, cols, i == choice && i < count ? "\x1b[7m" : "");
		}
		bar(&b, rows - 1, " Up/Down choose  Enter selects  Esc returns", cols, "\x1b[2m");
		snprintf(line, sizeof(line), "> %s", filter);
		bar(&b, rows, line, cols, "");
		pt_write(PT_STDOUT, b.p ? b.p : "", b.len);
		jb_free(&b);
		k = pt_readkey_timeout(PT_STDIN, -1);
		if (k == PT_KEY_NONE)
			continue;
		if (k < 0 || k == PT_KEY_ESC || k == PT_CTRL('c'))
			break;
		if ((k == '\r' || k == '\n') && count) {
			result = found[choice];
			break;
		}
		if (k == PT_KEY_UP && choice)
			choice--;
		else if (k == PT_KEY_DOWN && choice + 1 < count)
			choice++;
		else if (k == PT_KEY_PGUP)
			choice = choice > visible ? choice - visible : 0;
		else if (k == PT_KEY_PGDN)
			choice = choice + visible < count ? choice + visible : count - 1;
		else if ((k == 127 || k == '\b') && *filter) {
			filter[strlen(filter) - 1] = '\0';
			choice = top = 0;
		} else if (k >= 32 && k < 127 && strlen(filter) < sizeof(filter) - 1) {
			size_t len = strlen(filter);

			filter[len] = k;
			filter[len + 1] = '\0';
			choice = top = 0;
		}
	}
	pt_free(found);
	pt_tty_raw(PT_STDIN, ui != NULL);
	if (ui) {
		ui->dirty = true;
		ai_ui_draw(ui, true);
	}
	return result;
}

void ai_ui_status(struct ai_ui *ui, const char *status)
{
	if (!ui)
		return;
	strlcpy(ui->status, status, sizeof(ui->status));
	if (!*status)
		ui->cancelled = false;
	ui->dirty = true;
	ai_ui_draw(ui, true);
}

int ai_ui_key(struct ai_ui *ui, int timeout_ms)
{
	int k;

	if (ui)
		ai_ui_draw(ui, ui->dirty && timeout_ms != 0);
	k = pt_readkey_timeout(PT_STDIN, timeout_ms);
	if (!ui)
		return k;
	if (k == PT_KEY_PGUP || k == PT_CTRL('p'))
		ui->back += ui->body - 1;
	else if (k == PT_KEY_PGDN || k == PT_CTRL('n'))
		ui->back -= ui->body - 1;
	else if (k == PT_KEY_CTRL_HOME)
		ui->back = max_back(ui);
	else if (k == PT_KEY_CTRL_END)
		ui->back = 0;
	else
		return k;
	if (ui->back < 0)
		ui->back = 0;
	if (ui->back > max_back(ui))
		ui->back = max_back(ui);
	ai_ui_draw(ui, true);
	return PT_KEY_NONE;
}

bool ai_ui_poll(struct ai_ui *ui)
{
	if (!ui)
		return false;
	/* Drain a typing burst without starving the stream. Enter leaves a
	 * draft in place until readline owns submission again. */
	for (int count = 0; count < 64 && !ui->cancelled; count++) {
		int k = ai_ui_key(ui, 0);

		if (k == PT_KEY_NONE)
			break;
		if (k == PT_CTRL('c') || k == PT_KEY_ESC || k < 0)
			ui->cancelled = true;
		else
			ai_ui_edit_key(ui, k);
	}
	ai_ui_draw(ui, false);
	return ui->cancelled;
}

void ai_ui_draft(struct ai_ui *ui, const char *text)
{
	strlcpy(ui->input, text, sizeof(ui->input));
	ui->pos = ui->len = strlen(ui->input);
	ui->start = 0;
	ui->completion = -1;
	ui->dirty = true;
}

static void remember(struct ai_ui *ui)
{
	char *s;

	if (!ui->len || (ui->nhistory && !strcmp(ui->history[ui->nhistory - 1], ui->input)))
		return;
	s = pt_strdup(ui->input);
	if (!s)
		return;
	if (ui->nhistory == INPUT_HISTORY) {
		pt_free(ui->history[0]);
		memmove(ui->history, ui->history + 1, (INPUT_HISTORY - 1) * sizeof(s));
		ui->nhistory--;
	}
	ui->history[ui->nhistory++] = s;
}

static void erase(struct ai_ui *ui, int from, int to)
{
	memmove(ui->input + from, ui->input + to, ui->len - to + 1);
	ui->len -= to - from;
	ui->pos = from;
	ui->recalled = ui->nhistory;
}

void ai_ui_edit_key(struct ai_ui *ui, int k)
{
	if (!ui || k < 0 || k == '\r' || k == '\n')
		return;
	if (k != '\t')
		ui->completion = -1;
	if (k == PT_CTRL('c')) {
		ai_ui_draft(ui, "");
		ui->recalled = ui->nhistory;
	} else if (k == PT_KEY_LEFT || k == PT_CTRL('b'))
		ui->pos = prev_char(ui->input, ui->pos);
	else if (k == PT_KEY_RIGHT || k == PT_CTRL('f'))
		ui->pos = next_char(ui->input, ui->pos);
	else if (k == PT_KEY_HOME || k == PT_CTRL('a'))
		ui->pos = 0;
	else if (k == PT_KEY_END || k == PT_CTRL('e'))
		ui->pos = ui->len;
	else if ((k == 127 || k == '\b') && ui->pos)
		erase(ui, prev_char(ui->input, ui->pos), ui->pos);
	else if ((k == PT_KEY_DELETE || k == PT_CTRL('d')) && ui->pos < ui->len)
		erase(ui, ui->pos, next_char(ui->input, ui->pos));
	else if (k == PT_CTRL('u'))
		erase(ui, 0, ui->pos);
	else if (k == PT_CTRL('k'))
		erase(ui, ui->pos, ui->len);
	else if (k == PT_CTRL('w')) {
		int from = ui->pos;

		while (from && ui->input[from - 1] == ' ')
			from--;
		while (from && ui->input[from - 1] != ' ')
			from = prev_char(ui->input, from);
		erase(ui, from, ui->pos);
	} else if (k == PT_KEY_UP || k == PT_KEY_DOWN) {
		if (ui->recalled == ui->nhistory)
			strlcpy(ui->draft, ui->input, sizeof(ui->draft));
		if (k == PT_KEY_UP && ui->recalled)
			ui->recalled--;
		if (k == PT_KEY_DOWN && ui->recalled < ui->nhistory)
			ui->recalled++;
		ai_ui_draft(ui, ui->recalled == ui->nhistory ? ui->draft : ui->history[ui->recalled]);
	} else if (k == '\t') {
		int found[sizeof(commands) / sizeof(commands[0])], n;

		if (ui->completion < 0)
			strlcpy(ui->prefix, ui->input, sizeof(ui->prefix));
		n = matches(ui, found);
		if (n) {
			ui->completion = (ui->completion + 1) % n;
			snprintf(ui->input, sizeof(ui->input), "%s ", commands[found[ui->completion]]);
			ui->pos = ui->len = strlen(ui->input);
			ui->start = 0;
			ui->recalled = ui->nhistory;
		}
	} else if (k >= 32 && k < 256 && k != 127 && ui->len < AI_INPUT_MAX - 1) {
		memmove(ui->input + ui->pos + 1, ui->input + ui->pos, ui->len - ui->pos + 1);
		ui->input[ui->pos++] = k;
		ui->len++;
		ui->recalled = ui->nhistory;
	}
	ui->dirty = true;
}

int ai_ui_readline(struct ai_ui *ui, char *out, size_t size)
{
	ai_ui_status(ui, "");
	ui->recalled = ui->nhistory;
	for (;;) {
		int k;

		k = ai_ui_key(ui, -1);
		if (k == PT_KEY_NONE)
			continue;
		if (k == PT_KEY_ESC)
			return -2;
		if (k == PT_KEY_EOF || k == PT_KEY_ERROR || k == PT_KEY_INTR ||
		    (k == PT_CTRL('d') && !ui->len))
			return -1;
		if (k != '\t')
			ui->completion = -1;
		if (k == '\r' || k == '\n') {
			int n = ui->len;

			strlcpy(out, ui->input, size);
			remember(ui);
			ui->recalled = ui->nhistory;
			ai_ui_draft(ui, "");
			ui->back = 0;
			return n;
		}
		ai_ui_edit_key(ui, k);
		ui->dirty = true;
	}
}
