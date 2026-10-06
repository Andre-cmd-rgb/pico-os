/*
 * Line editor for the interactive shell.
 *
 * The line is drawn on one terminal row. When it is longer than the row, the
 * row scrolls sideways to keep the cursor visible, so nothing ever wraps -
 * wrapping on a 53-column screen is what makes line editors fall apart.
 *
 *   Left/Right, Home/End, Ctrl-A/Ctrl-E	move
 *   Backspace, Delete, Ctrl-W, Ctrl-U, Ctrl-K	erase
 *   Up/Down, Ctrl-P/Ctrl-N			history
 *   Tab					complete commands and paths
 *   Esc					clear the line
 *   Ctrl-L					clear the screen
 *   Ctrl-C					abandon the line
 *   Ctrl-D					exit on an empty line
 */
#include <stdio.h>
#include <string.h>

#include "pt/keys.h"
#include "sh.h"

struct edit {
	char		*buf;
	size_t		 size, len, pos;	/* bytes */
	const char	*prompt;
	size_t		 prompt_w;
	int		 cols;
	size_t		 scroll;		/* first visible code point */
};

static bool is_cont(char c)
{
	return ((unsigned char)c & 0xc0) == 0x80;
}

static size_t cp_count(const char *s, size_t n)
{
	size_t count = 0;

	for (size_t i = 0; i < n; i++)
		count += !is_cont(s[i]);
	return count;
}

static size_t cp_offset(const char *s, size_t len, size_t cps)
{
	size_t i = 0;

	while (i < len && cps) {
		i++;
		while (i < len && is_cont(s[i]))
			i++;
		cps--;
	}
	return i;
}

static size_t visible_width(const char *s)
{
	size_t w = 0;

	while (*s) {
		if (*s == '\x1b') {
			s++;
			if (*s == '[') {
				s++;
				while (*s && !(*s >= 0x40 && *s <= 0x7e))
					s++;
			}
			if (*s)
				s++;
			continue;
		}
		w += !is_cont(*s);
		s++;
	}
	return w;
}

static void out(const char *s, size_t n)
{
	while (n) {
		ssize_t r = pt_write(PT_STDOUT, s, n);
		if (r <= 0)
			return;
		s += r;
		n -= r;
	}
}

static void refresh(struct edit *e)
{
	char tmp[16];
	size_t avail = e->cols > (int)e->prompt_w + 2 ? e->cols - e->prompt_w - 1 : 1;
	size_t cur = cp_count(e->buf, e->pos);
	size_t total = cp_count(e->buf, e->len);

	if (cur < e->scroll)
		e->scroll = cur;
	if (cur - e->scroll > avail)
		e->scroll = cur - avail;

	size_t from = cp_offset(e->buf, e->len, e->scroll);
	size_t end_cp = e->scroll + avail < total ? e->scroll + avail : total;
	size_t to = cp_offset(e->buf, e->len, end_cp);

	/* one write per redraw: less flicker, and far fewer serial bytes */
	size_t plen = strlen(e->prompt);
	char frame[1024];
	size_t col = e->prompt_w + cur - e->scroll;
	int tail = snprintf(tmp, sizeof(tmp), col ? "\x1b[%zuC" : "", col);

	if (1 + plen + (to - from) + 4 + tail > sizeof(frame)) {
		out("\r", 1);
		out(e->prompt, plen);
		out(e->buf + from, to - from);
		out("\x1b[K\r", 4);
		out(tmp, tail);
		return;
	}
	char *o = frame;
	*o++ = '\r';
	memcpy(o, e->prompt, plen);
	o += plen;
	memcpy(o, e->buf + from, to - from);
	o += to - from;
	memcpy(o, "\x1b[K\r", 4);
	o += 4;
	memcpy(o, tmp, tail);
	o += tail;
	out(frame, o - frame);
}

static void set_line(struct edit *e, const char *s)
{
	size_t n = strlen(s);

	if (n >= e->size)
		n = e->size - 1;
	memcpy(e->buf, s, n);
	e->buf[n] = '\0';
	e->len = e->pos = n;
	e->scroll = 0;
}

static void insert(struct edit *e, const char *s, size_t n)
{
	if (e->len + n >= e->size)
		return;
	memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos);
	memcpy(e->buf + e->pos, s, n);
	e->len += n;
	e->pos += n;
	e->buf[e->len] = '\0';
}

static void erase_range(struct edit *e, size_t from, size_t to)
{
	memmove(e->buf + from, e->buf + to, e->len - to);
	e->len -= to - from;
	e->buf[e->len] = '\0';
	if (e->pos > to)
		e->pos -= to - from;
	else if (e->pos > from)
		e->pos = from;
}

static size_t prev_cp(struct edit *e, size_t i)
{
	if (!i)
		return 0;
	do
		i--;
	while (i && is_cont(e->buf[i]));
	return i;
}

static size_t next_cp(struct edit *e, size_t i)
{
	if (i >= e->len)
		return e->len;
	do
		i++;
	while (i < e->len && is_cont(e->buf[i]));
	return i;
}

static void list_candidates(struct edit *e, const struct candidates *c)
{
	size_t width = 0;

	for (int i = 0; i < c->count; i++) {
		size_t w = cp_count(c->name[i], strlen(c->name[i])) + c->is_dir[i];
		if (w > width)
			width = w;
	}
	width += 2;
	int per_row = e->cols / (int)width;
	if (per_row < 1)
		per_row = 1;

	out("\n", 1);
	for (int i = 0; i < c->count; i++) {
		size_t w = cp_count(c->name[i], strlen(c->name[i])) + c->is_dir[i];
		out(c->name[i], strlen(c->name[i]));
		if (c->is_dir[i])
			out("/", 1);
		if ((i + 1) % per_row == 0 || i + 1 == c->count) {
			out("\n", 1);
		} else {
			for (size_t pad = w; pad < width; pad++)
				out(" ", 1);
		}
	}
}

/* Whether a word needs quoting to reach a command as it is. */
static bool needs_quotes(const char *w)
{
	if (*w == '#')
		return true;
	for (; *w; w++)
		if (strchr(" \t'\"\\$`&|;<>()*?[]", *w))
			return true;
	return false;
}

/*
 * The word `w`, written back into the line: as it is if nothing in it is
 * special, or else in the quotes it was begun with -- double ones if it
 * was not -- with a leading ~/ kept outside them, where it still means
 * home. `close` ends the quote, for a word that is finished.
 */
static size_t quote_word(const char *w, char quote, bool close, char *out, size_t size)
{
	size_t n = 0;

	if (!quote && !needs_quotes(w)) {
		strlcpy(out, w, size);
		return strlen(out);
	}
	if (!quote)
		quote = '"';
	if (w[0] == '~' && (w[1] == '/' || !w[1])) {
		for (int k = 0; k < 2 && *w && n + 1 < size; k++)
			out[n++] = *w++;
	}
	if (n + 1 < size)
		out[n++] = quote;
	for (; *w && n + 3 < size; w++) {
		if (quote == '"' && strchr("\"\\$`", *w))
			out[n++] = '\\';
		else if (quote == '\'' && *w == '\'') {
			out[n++] = '\'';		/* 'it'\''s' */
			out[n++] = '\\';
			out[n++] = '\'';
		}
		out[n++] = *w;
	}
	if (close && n + 1 < size)
		out[n++] = quote;
	out[n] = '\0';
	return n;
}

static void complete(struct sh *sh, struct edit *e)
{
	char word[PT_PATH_MAX], raw[PT_PATH_MAX * 2], quote;
	struct candidates c = { 0 };
	size_t start = word_scan(e->buf, e->pos, word, sizeof(word), &quote);

	if (sh_candidates(sh, e->buf, start, word, &c) || !c.count) {
		candidates_free(&c);
		return;
	}

	/* longest common extension past what is already typed */
	size_t common = strlen(c.name[0]);
	for (int i = 1; i < c.count; i++) {
		size_t k = 0;
		while (k < common && c.name[i][k] == c.name[0][k])
			k++;
		common = k;
	}
	while (common > c.typed && is_cont(c.name[0][common]))
		common--;

	if (common > c.typed || c.count == 1) {
		bool unique = c.count == 1, dir = unique && c.is_dir[0];
		size_t len = strlen(word), n;

		/* the word as it will be, then written back quoted as it needs */
		if (len + common - c.typed + 2 < sizeof(word)) {
			memcpy(word + len, c.name[0] + c.typed, common - c.typed);
			len += common - c.typed;
			if (dir)
				word[len++] = '/';
			word[len] = '\0';
		}
		n = quote_word(word, quote, unique && !dir, raw, sizeof(raw));
		if (unique && !dir && n + 1 < sizeof(raw)) {
			raw[n++] = ' ';
			raw[n] = '\0';
		}
		erase_range(e, start, e->pos);
		insert(e, raw, n);
	} else {
		list_candidates(e, &c);
	}
	candidates_free(&c);
}

void history_add(struct history *h, const char *line)
{
	if (!*line || (h->count && !strcmp(h->entry[h->count - 1], line)))
		return;
	if (h->count == SH_HISTORY) {
		pt_free(h->entry[0]);
		memmove(h->entry, h->entry + 1, (SH_HISTORY - 1) * sizeof(h->entry[0]));
		h->count--;
	}
	char *copy = pt_strdup(line);
	if (copy)
		h->entry[h->count++] = copy;
}

int lineedit(struct sh *sh, const char *prompt, char *buf, size_t size)
{
	struct history *h = sh_history(sh);
	struct edit e = { .buf = buf, .size = size, .prompt = prompt, .prompt_w = visible_width(prompt) };
	char draft[SH_LINE_MAX] = "";
	int hist = h->count;
	int rows, result;

	pt_tty_size(PT_STDIN, &e.cols, &rows);
	buf[0] = '\0';
	pt_tty_raw(PT_STDIN, true);
	refresh(&e);
	history_save(sh);		/* the last command's, with the prompt up */

	for (;;) {
		int k = pt_readkey(PT_STDIN);

		if (k == PT_KEY_EOF || k == PT_KEY_ERROR || k == PT_KEY_INTR) {
			result = LE_ERROR;
			break;
		}
		if (k == '\r' || k == '\n') {
			e.pos = e.len;
			refresh(&e);
			out("\n", 1);
			result = e.len;
			break;
		}
		if (k == PT_CTRL('c')) {
			out("^C\n", 3);
			result = LE_INTERRUPT;
			break;
		}
		if (k == PT_CTRL('d') && !e.len) {
			out("\n", 1);
			result = LE_EOF;
			break;
		}

		switch (k) {
		case PT_KEY_LEFT:
		case PT_CTRL('b'):
			e.pos = prev_cp(&e, e.pos);
			break;
		case PT_KEY_RIGHT:
		case PT_CTRL('f'):
			e.pos = next_cp(&e, e.pos);
			break;
		case PT_KEY_HOME:
		case PT_CTRL('a'):
			e.pos = 0;
			break;
		case PT_KEY_END:
		case PT_CTRL('e'):
			e.pos = e.len;
			break;
		case 0x7f:
		case '\b':
			erase_range(&e, prev_cp(&e, e.pos), e.pos);
			break;
		case PT_KEY_DELETE:
		case PT_CTRL('d'):
			erase_range(&e, e.pos, next_cp(&e, e.pos));
			break;
		case PT_CTRL('w'): {
			size_t from = e.pos;
			while (from && e.buf[from - 1] == ' ')
				from--;
			while (from && e.buf[from - 1] != ' ')
				from--;
			erase_range(&e, from, e.pos);
			break;
		}
		case PT_CTRL('u'):
			erase_range(&e, 0, e.pos);
			break;
		case PT_CTRL('k'):
			erase_range(&e, e.pos, e.len);
			break;
		case PT_KEY_ESC:
			set_line(&e, "");
			hist = h->count;
			break;
		case PT_CTRL('l'):
			out("\x1b[2J\x1b[H", 7);
			break;
		case PT_KEY_UP:
		case PT_CTRL('p'):
			if (hist > 0) {
				if (hist == h->count)
					strlcpy(draft, e.buf, sizeof(draft));
				set_line(&e, h->entry[--hist]);
			}
			break;
		case PT_KEY_DOWN:
		case PT_CTRL('n'):
			if (hist < h->count) {
				hist++;
				set_line(&e, hist == h->count ? draft : h->entry[hist]);
			}
			break;
		case '\t':
			complete(sh, &e);
			break;
		default:
			if (k >= 0x20 && k <= 0xff && k != 0x7f) {
				char ch[4] = { (char)k };
				size_t n = 1;
				/* read the rest of a UTF-8 sequence before redrawing */
				int need = (k & 0xe0) == 0xc0 ? 1 : (k & 0xf0) == 0xe0 ? 2 : (k & 0xf8) == 0xf0 ? 3 : 0;
				while (need-- > 0) {
					int more = pt_readkey(PT_STDIN);
					if (more < 0x80 || more > 0xbf)
						break;
					ch[n++] = (char)more;
				}
				insert(&e, ch, n);
			}
			break;
		}
		refresh(&e);
	}
	pt_tty_raw(PT_STDIN, false);
	return result;
}
