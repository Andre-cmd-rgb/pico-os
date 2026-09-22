/*
 * edit - a small full-screen text editor.
 *
 *   arrows, Home/End, PgUp/PgDn	move
 *   Backspace, Delete			erase
 *   Enter				new line, keeping the indentation
 *   Tab				spaces to the next multiple of 4
 *
 *   Ctrl-S save   Ctrl-Q quit   Ctrl-F find   Ctrl-G go to line   Ctrl-K cut line
 *
 * Esc opens the same commands as a menu, for keyboards without Ctrl.
 *
 * Only rows whose text changed are sent to the terminal, so editing stays
 * quick over the serial console too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define TAB_WIDTH	4

struct line {
	char	*s;
	int	 len, cap;
};

struct out {
	char	*data;
	size_t	 len, cap;
};

struct editor {
	const char	*path;
	struct line	*lines;
	int		 nlines, cap;
	int		 cy, cx;		/* cursor: line, byte offset */
	int		 want_col;		/* column kept on vertical moves */
	int		 top, left;		/* scroll position */
	int		 cols, rows, text_rows;
	bool		 dirty, quit;
	char		 msg[96];
	char		 find[64];
	char		**shown;		/* last text sent for each row */
	struct out	 out;
};

/* ------------------------------------------------------------ output buffer */

static void out_add(struct out *o, const char *s, size_t n)
{
	if (o->len + n > o->cap) {
		size_t cap = (o->len + n) * 2 + 256;
		char *data = pt_realloc(o->data, cap);
		if (!data)
			return;
		o->data = data;
		o->cap = cap;
	}
	memcpy(o->data + o->len, s, n);
	o->len += n;
}

static void out_printf(struct out *o, const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		out_add(o, buf, n < (int)sizeof(buf) ? n : sizeof(buf) - 1);
}

static void out_flush(struct out *o)
{
	write_all(PT_STDOUT, o->data, o->len);
	o->len = 0;
}

/* ------------------------------------------------------------ lines */

static bool is_cont(char c)
{
	return ((unsigned char)c & 0xc0) == 0x80;
}

static int char_len(const struct line *l, int i)
{
	int n = 1;

	while (i + n < l->len && is_cont(l->s[i + n]))
		n++;
	return n;
}

static int col_at(const struct line *l, int byte)
{
	int col = 0;

	for (int i = 0; i < byte && i < l->len; i++) {
		if (l->s[i] == '\t')
			col = (col / TAB_WIDTH + 1) * TAB_WIDTH;
		else if (!is_cont(l->s[i]))
			col++;
	}
	return col;
}

static int byte_at(const struct line *l, int want)
{
	int col = 0, i = 0;

	while (i < l->len) {
		int next = l->s[i] == '\t' ? (col / TAB_WIDTH + 1) * TAB_WIDTH : col + 1;
		if (next > want)
			break;
		col = next;
		i += char_len(l, i);
	}
	return i;
}

static bool line_insert(struct line *l, int at, const char *s, int n)
{
	if (!n)
		return true;
	if (l->len + n > l->cap) {
		int cap = (l->len + n) * 2 + 16;
		char *grown = pt_realloc(l->s, cap);
		if (!grown)
			return false;
		l->s = grown;
		l->cap = cap;
	}
	memmove(l->s + at + n, l->s + at, l->len - at);
	memcpy(l->s + at, s, n);
	l->len += n;
	return true;
}

static void line_delete(struct line *l, int at, int n)
{
	memmove(l->s + at, l->s + at + n, l->len - at - n);
	l->len -= n;
}

static struct line *lines_insert(struct editor *e, int at)
{
	if (e->nlines == e->cap) {
		int cap = e->cap ? e->cap * 2 : 64;
		struct line *grown = pt_realloc(e->lines, cap * sizeof(*grown));
		if (!grown)
			return NULL;
		e->lines = grown;
		e->cap = cap;
	}
	memmove(e->lines + at + 1, e->lines + at, (e->nlines - at) * sizeof(*e->lines));
	memset(&e->lines[at], 0, sizeof(e->lines[at]));
	e->nlines++;
	return &e->lines[at];
}

static void lines_delete(struct editor *e, int at)
{
	pt_free(e->lines[at].s);
	memmove(e->lines + at, e->lines + at + 1, (e->nlines - at - 1) * sizeof(*e->lines));
	e->nlines--;
}

/* ------------------------------------------------------------ files */

static int load(struct editor *e)
{
	char buf[512];
	int cur = 0;
	ssize_t n;

	if (!lines_insert(e, 0))
		return -ENOMEM;
	int fd = pt_open(e->path, O_RDONLY);
	if (fd == -ENOENT) {
		snprintf(e->msg, sizeof(e->msg), "new file");
		return 0;
	}
	if (fd < 0)
		return fd;

	while ((n = pt_read(fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n;) {
			char *nl = memchr(buf + i, '\n', n - i);
			ssize_t end = nl ? nl - buf : n;
			ssize_t len = end - i;
			if (len && buf[end - 1] == '\r')
				len--;
			if (len && !line_insert(&e->lines[cur], e->lines[cur].len, buf + i, len)) {
				pt_close(fd);
				return -ENOMEM;
			}
			if (nl && !lines_insert(e, ++cur)) {
				pt_close(fd);
				return -ENOMEM;
			}
			i = end + (nl != NULL);
		}
	}
	pt_close(fd);
	if (e->nlines > 1 && !e->lines[e->nlines - 1].len)
		lines_delete(e, e->nlines - 1);
	snprintf(e->msg, sizeof(e->msg), "%d lines", e->nlines);
	return n < 0 ? (int)n : 0;
}

/*
 * Writes path.tmp and renames it over the file, so a power cut leaves either
 * the old version or the new one. LittleFS replaces the file in that one
 * step; FAT will not rename over a file, so there the old one goes first.
 */
static void save(struct editor *e)
{
	char tmp[PT_PATH_MAX];
	struct pt_stat st;
	int err = 0;

	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", e->path) >= sizeof(tmp)) {
		snprintf(e->msg, sizeof(e->msg), "NOT SAVED: %s", pt_strerror(-ENAMETOOLONG));
		return;
	}
	int fd = pt_open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
	if (fd < 0) {
		snprintf(e->msg, sizeof(e->msg), "NOT SAVED: %s", pt_strerror(fd));
		return;
	}
	for (int i = 0; i < e->nlines && !err; i++) {
		err = write_all(fd, e->lines[i].s, e->lines[i].len);
		if (!err)
			err = write_all(fd, "\n", 1);
	}
	int closed = pt_close(fd);		/* the last block is written here */
	if (!err)
		err = closed;
	if (!err) {
		err = pt_rename(tmp, e->path);
		if (err == -EEXIST && !pt_stat(e->path, &st) && !st.is_dir) {
			err = pt_unlink(e->path);
			if (!err)
				err = pt_rename(tmp, e->path);
		}
	}
	if (err) {
		pt_unlink(tmp);
		snprintf(e->msg, sizeof(e->msg), "NOT SAVED: %s", pt_strerror(err));
		return;
	}
	e->dirty = false;
	snprintf(e->msg, sizeof(e->msg), "saved %d lines", e->nlines);
}

/* ------------------------------------------------------------ screen */

static void scroll_to_cursor(struct editor *e)
{
	int col = col_at(&e->lines[e->cy], e->cx);
	int width = e->cols - 1;

	if (e->cy < e->top)
		e->top = e->cy;
	if (e->cy >= e->top + e->text_rows)
		e->top = e->cy - e->text_rows + 1;
	if (col < e->left)
		e->left = col;
	if (col >= e->left + width)
		e->left = col - width + 1;
}

/* The visible part of a line: tabs expanded, control characters as '?'. */
static int visible_text(struct editor *e, const struct line *l, char *out)
{
	int width = e->cols - 1;
	int col = 0, o = 0;

	for (int i = 0; i < l->len && col < e->left + width;) {
		if (l->s[i] == '\t') {
			int next = (col / TAB_WIDTH + 1) * TAB_WIDTH;
			for (; col < next && col < e->left + width; col++)
				if (col >= e->left)
					out[o++] = ' ';
			i++;
			continue;
		}
		int n = char_len(l, i);
		if (col >= e->left) {
			if ((unsigned char)l->s[i] < 0x20) {
				out[o++] = '?';
			} else {
				memcpy(out + o, l->s + i, n);
				o += n;
			}
		}
		col++;
		i += n;
	}
	out[o] = '\0';
	return o;
}

static void put_row(struct editor *e, int row, const char *text)
{
	if (e->shown[row] && !strcmp(e->shown[row], text))
		return;
	pt_free(e->shown[row]);
	e->shown[row] = pt_strdup(text);
	out_printf(&e->out, "\x1b[%d;1H", row + 1);
	out_add(&e->out, text, strlen(text));
	out_add(&e->out, "\x1b[0m\x1b[K", 7);
}

static void status_row(struct editor *e, const char *left)
{
	char right[32], row[256];
	const char *name = strrchr(e->path, '/') ? strrchr(e->path, '/') + 1 : e->path;
	int col = col_at(&e->lines[e->cy], e->cx);

	int rn = snprintf(right, sizeof(right), "L%d/%d C%d ", e->cy + 1, e->nlines, col + 1);
	if (!left) {
		static char title[96];
		snprintf(title, sizeof(title), " %s%s", name, e->dirty ? " *" : "");
		left = title;
	}
	int pad = e->cols - 1 - (int)strlen(left) - rn;
	snprintf(row, sizeof(row), "\x1b[7m%.*s%*s%s", e->cols - 1 - rn, left, pad > 0 ? pad : 0, "", right);
	put_row(e, e->rows - 1, row);
}

static void render(struct editor *e)
{
	char *text = pt_malloc(e->cols * 4 + 16);

	if (!text)
		return;
	scroll_to_cursor(e);
	out_add(&e->out, "\x1b[?25l", 6);
	for (int r = 0; r < e->text_rows; r++) {
		int i = e->top + r;
		if (i < e->nlines)
			visible_text(e, &e->lines[i], text);
		else
			strcpy(text, "\x1b[2m~");
		put_row(e, r, text);
	}
	status_row(e, e->msg[0] ? e->msg : NULL);
	out_printf(&e->out, "\x1b[%d;%dH\x1b[?25h", e->cy - e->top + 1,
		   col_at(&e->lines[e->cy], e->cx) - e->left + 1);
	out_flush(&e->out);
	pt_free(text);
}

/* One-line input on the status row. */
static bool prompt(struct editor *e, const char *label, char *buf, size_t size)
{
	char row[256];
	size_t len = strlen(buf);

	for (;;) {
		snprintf(row, sizeof(row), "\x1b[7m %s %s", label, buf);
		put_row(e, e->rows - 1, row);
		out_printf(&e->out, "\x1b[%d;%zuH", e->rows, strlen(label) + len + 3);
		out_flush(&e->out);

		int k = pt_readkey(PT_STDIN);
		if (k == '\r' || k == '\n')
			return true;
		if (k == PT_KEY_ESC || k == PT_CTRL('c') || k < 0)
			return false;
		if ((k == 0x7f || k == '\b') && len) {
			do
				len--;
			while (len && is_cont(buf[len]));
			buf[len] = '\0';
		} else if (k >= 0x20 && k <= 0xff && k != 0x7f && len + 1 < size) {
			buf[len++] = (char)k;
			buf[len] = '\0';
		}
	}
}

/* ------------------------------------------------------------ commands */

static void move_vertical(struct editor *e, int delta)
{
	int cy = e->cy + delta;

	e->cy = cy < 0 ? 0 : cy >= e->nlines ? e->nlines - 1 : cy;
	e->cx = byte_at(&e->lines[e->cy], e->want_col);
}

static void remember_col(struct editor *e)
{
	e->want_col = col_at(&e->lines[e->cy], e->cx);
}

static void newline(struct editor *e)
{
	struct line *cur = &e->lines[e->cy];
	int indent = 0;

	while (indent < e->cx && (cur->s[indent] == ' ' || cur->s[indent] == '\t'))
		indent++;
	struct line *next = lines_insert(e, e->cy + 1);
	if (!next)
		return;
	cur = &e->lines[e->cy];			/* lines may have moved */
	if (!line_insert(next, 0, cur->s, indent) ||
	    !line_insert(next, indent, cur->s + e->cx, cur->len - e->cx))
		return;
	cur->len = e->cx;
	e->cy++;
	e->cx = indent;
	e->dirty = true;
}

static void backspace(struct editor *e)
{
	struct line *cur = &e->lines[e->cy];

	if (e->cx > 0) {
		int start = e->cx - 1;
		while (start && is_cont(cur->s[start]))
			start--;
		line_delete(cur, start, e->cx - start);
		e->cx = start;
	} else if (e->cy > 0) {
		struct line *prev = &e->lines[e->cy - 1];
		int at = prev->len;
		if (!line_insert(prev, at, cur->s, cur->len))
			return;
		lines_delete(e, e->cy);
		e->cy--;
		e->cx = at;
	} else {
		return;
	}
	e->dirty = true;
}

static void delete_forward(struct editor *e)
{
	struct line *cur = &e->lines[e->cy];

	if (e->cx < cur->len) {
		line_delete(cur, e->cx, char_len(cur, e->cx));
	} else if (e->cy + 1 < e->nlines) {
		struct line *next = &e->lines[e->cy + 1];
		if (!line_insert(cur, cur->len, next->s, next->len))
			return;
		lines_delete(e, e->cy + 1);
	} else {
		return;
	}
	e->dirty = true;
}

static void cut_line(struct editor *e)
{
	if (e->nlines > 1) {
		lines_delete(e, e->cy);
		if (e->cy >= e->nlines)
			e->cy = e->nlines - 1;
	} else {
		e->lines[0].len = 0;
	}
	e->cx = 0;
	e->dirty = true;
}

static void find(struct editor *e)
{
	if (!prompt(e, "Find:", e->find, sizeof(e->find)) || !e->find[0])
		return;
	size_t n = strlen(e->find);

	for (int step = 0; step <= e->nlines; step++) {
		int i = (e->cy + step) % e->nlines;
		struct line *l = &e->lines[i];
		int from = step == 0 ? e->cx + 1 : 0;
		for (int x = from; x + (int)n <= l->len; x++) {
			if (!memcmp(l->s + x, e->find, n)) {
				e->cy = i;
				e->cx = x;
				remember_col(e);
				return;
			}
		}
	}
	snprintf(e->msg, sizeof(e->msg), "\"%s\" not found", e->find);
}

static void go_to_line(struct editor *e)
{
	char number[16] = "";

	if (!prompt(e, "Go to line:", number, sizeof(number)))
		return;
	int n = atoi(number);
	if (n >= 1) {
		e->cy = n > e->nlines ? e->nlines - 1 : n - 1;
		e->cx = 0;
		remember_col(e);
	}
}

static void request_quit(struct editor *e)
{
	if (!e->dirty) {
		e->quit = true;
		return;
	}
	status_row(e, " Unsaved changes!  S save and quit  Q quit anyway  Esc cancel");
	out_flush(&e->out);
	int k = pt_readkey(PT_STDIN);
	if (k == 's' || k == 'S') {
		save(e);
		e->quit = !e->dirty;
	} else if (k == 'q' || k == 'Q') {
		e->quit = true;
	}
}

static void menu(struct editor *e)
{
	status_row(e, " S save  Q quit  X save+quit  F find  G line  K cut  Esc");
	out_flush(&e->out);

	switch (pt_readkey(PT_STDIN)) {
	case 's': case 'S':
		save(e);
		break;
	case 'q': case 'Q':
		request_quit(e);
		break;
	case 'x': case 'X':
		save(e);
		e->quit = !e->dirty;
		break;
	case 'f': case 'F':
		find(e);
		break;
	case 'g': case 'G':
		go_to_line(e);
		break;
	case 'k': case 'K':
		cut_line(e);
		break;
	}
}

static void handle_key(struct editor *e, int k)
{
	struct line *cur = &e->lines[e->cy];

	switch (k) {
	case PT_KEY_UP:
		move_vertical(e, -1);
		return;
	case PT_KEY_DOWN:
		move_vertical(e, 1);
		return;
	case PT_KEY_PGUP:
		move_vertical(e, -e->text_rows);
		return;
	case PT_KEY_PGDN:
		move_vertical(e, e->text_rows);
		return;
	case PT_KEY_LEFT:
		if (e->cx > 0) {
			do
				e->cx--;
			while (e->cx && is_cont(cur->s[e->cx]));
		} else if (e->cy > 0) {
			e->cy--;
			e->cx = e->lines[e->cy].len;
		}
		break;
	case PT_KEY_RIGHT:
		if (e->cx < cur->len) {
			e->cx += char_len(cur, e->cx);
		} else if (e->cy + 1 < e->nlines) {
			e->cy++;
			e->cx = 0;
		}
		break;
	case PT_KEY_HOME:
		e->cx = 0;
		break;
	case PT_KEY_END:
		e->cx = cur->len;
		break;
	case '\r':
	case '\n':
		newline(e);
		break;
	case 0x7f:
	case '\b':
		backspace(e);
		break;
	case PT_KEY_DELETE:
		delete_forward(e);
		break;
	case '\t': {
		int spaces = TAB_WIDTH - col_at(cur, e->cx) % TAB_WIDTH;
		if (line_insert(cur, e->cx, "    ", spaces)) {
			e->cx += spaces;
			e->dirty = true;
		}
		break;
	}
	case PT_CTRL('s'):
		save(e);
		return;
	case PT_CTRL('q'):
		request_quit(e);
		return;
	case PT_CTRL('f'):
		find(e);
		return;
	case PT_CTRL('g'):
		go_to_line(e);
		return;
	case PT_CTRL('k'):
		cut_line(e);
		break;
	case PT_KEY_ESC:
	case PT_CTRL('c'):
		menu(e);
		return;
	default:
		if (k >= 0x20 && k <= 0xff && k != 0x7f) {
			char ch[4] = { (char)k };
			int n = 1;
			int need = (k & 0xe0) == 0xc0 ? 1 : (k & 0xf0) == 0xe0 ? 2 : (k & 0xf8) == 0xf0 ? 3 : 0;
			while (need-- > 0) {
				int more = pt_readkey(PT_STDIN);
				if (more < 0x80 || more > 0xbf)
					break;
				ch[n++] = (char)more;
			}
			if (line_insert(cur, e->cx, ch, n)) {
				e->cx += n;
				e->dirty = true;
			}
		}
		break;
	}
	remember_col(e);
}

PT_PROGRAM_STACK(edit, 12, "edit a text file\n"
		 "usage: edit file\n"
		 "Ctrl-S save, Ctrl-Q quit, Ctrl-F find, Ctrl-G go to line,\n"
		 "Ctrl-K cut line. Esc opens a menu with the same commands.")
{
	struct editor e = { 0 };

	if (argc != 2) {
		pt_dprintf(PT_STDERR, "usage: edit file\n");
		return 2;
	}
	if (!pt_isatty(PT_STDIN) || !pt_isatty(PT_STDOUT)) {
		pt_dprintf(PT_STDERR, "edit: needs a terminal\n");
		return 1;
	}
	e.path = argv[1];
	pt_tty_size(PT_STDOUT, &e.cols, &e.rows);
	e.text_rows = e.rows - 1;
	e.shown = pt_calloc(e.rows, sizeof(*e.shown));

	int err = e.shown ? load(&e) : -ENOMEM;
	if (err)
		return fail("edit", e.path, err);

	pt_tty_raw(PT_STDIN, true);
	out_add(&e.out, "\x1b[2J", 4);
	while (!e.quit) {
		render(&e);
		e.msg[0] = '\0';
		int k = pt_readkey(PT_STDIN);
		if (k == PT_KEY_EOF || k == PT_KEY_ERROR)
			break;
		handle_key(&e, k);
	}
	pt_tty_raw(PT_STDIN, false);
	pt_puts("\x1b[2J\x1b[H");
	return 0;
}
