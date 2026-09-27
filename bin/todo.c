/*
 * todo - a to-do list.
 *
 * The list is ~/todo.md, a Markdown checklist, so it can be written on a
 * PC too, `notes` shows it, and whatever else is in the file (headings,
 * lines of notes) is left as it was:
 *
 *	- [ ] studiare storia @2026-09-30
 *	- [x] comprare quaderni
 *
 * A date after @ is when it is due; `todo add ... @fri` or @30/9 or
 * @tomorrow write it. The list is shown open things first, the ones
 * due soonest at the top, then the undated, then what is done; the
 * numbers `todo done N` takes are the ones shown. The calendar shows
 * what is due on each day.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dates.h"
#include "pda.h"
#include "util.h"

#define TODO_LINE	512

struct list {
	char			 path[PT_PATH_MAX];
	char			**lines;
	int			 nlines, cap;
	struct todo_item	*items;
	int			 n;
};

/* ------------------------------------------------------------ the file */

bool pda_file(const char *given, const char *name, char *out, size_t size)
{
	const char *home = pt_getenv("HOME");

	if (given)
		return strlcpy(out, given, size) < size;
	return home && join_path(home, name, out, size);
}

/* "- [ ] text @2026-09-30": an item, or false for any other line. */
static bool parse_item(const char *line, struct todo_item *it)
{
	const char *p = line;
	char *t, *at;
	int y, m, d, n;

	while (*p == ' ' || *p == '\t')
		p++;
	/* strchr would find the string's own end in each of these */
	if (!*p || !strchr("-*+", *p) || strncmp(p + 1, " [", 2) || !p[3] || !strchr(" xX", p[3]) ||
	    p[4] != ']')
		return false;
	it->done = p[3] != ' ';
	p += 5;
	while (*p == ' ')
		p++;
	strlcpy(it->text, p, sizeof(it->text));
	it->due = 0;
	/* the last @date is the due date, and not part of the text */
	at = strrchr(it->text, '@');
	if (at && sscanf(at + 1, "%d-%d-%d%n", &y, &m, &d, &n) == 3 &&
	    (!at[1 + n] || at[1 + n] == ' ') && (it->due = day_make(y, m, d)))
		memmove(at, at + 1 + n, strlen(at + 1 + n) + 1);
	for (t = it->text + strlen(it->text); t > it->text && (t[-1] == ' ' || t[-1] == '\n'); t--)
		t[-1] = '\0';
	return true;
}

static void item_line(const struct todo_item *it, char *out, size_t size)
{
	if (it->due)
		snprintf(out, size, "- [%c] %s @%04d-%02d-%02d", it->done ? 'x' : ' ', it->text,
			 it->due / 10000, it->due / 100 % 100, it->due % 100);
	else
		snprintf(out, size, "- [%c] %s", it->done ? 'x' : ' ', it->text);
}

/* Open first; of those the dated, soonest first; then the file's order. */
static int by_urgency(const void *pa, const void *pb)
{
	const struct todo_item *a = pa, *b = pb;

	if (a->done != b->done)
		return a->done ? 1 : -1;
	if (!a->done && !!a->due != !!b->due)
		return a->due ? -1 : 1;
	if (!a->done && a->due != b->due)
		return a->due < b->due ? -1 : 1;
	return a->line - b->line;
}

static bool add_line(struct list *l, const char *text)
{
	size_t cap = l->cap;
	char *copy = pt_strdup(text);

	if (!copy)
		return false;
	if (l->nlines == l->cap) {
		char **grown;

		cap = cap ? cap * 2 : 32;
		grown = pt_realloc(l->lines, cap * sizeof(*grown));
		if (!grown) {
			pt_free(copy);
			return false;
		}
		l->lines = grown;
		l->cap = cap;
	}
	l->lines[l->nlines++] = copy;
	return true;
}

/* The items again, from the lines, in the order they are shown. */
static int reindex(struct list *l)
{
	struct todo_item it;

	pt_free(l->items);
	l->items = pt_malloc((l->nlines + 1) * sizeof(*l->items));
	l->n = 0;
	if (!l->items)
		return -ENOMEM;
	for (int i = 0; i < l->nlines; i++)
		if (parse_item(l->lines[i], &it)) {
			it.line = i;
			l->items[l->n++] = it;
		}
	qsort(l->items, l->n, sizeof(*l->items), by_urgency);
	return 0;
}

static void list_free(struct list *l)
{
	for (int i = 0; i < l->nlines; i++)
		pt_free(l->lines[i]);
	pt_free(l->lines);
	pt_free(l->items);
	memset(l, 0, sizeof(*l));
}

/* A list that does not exist yet is empty, not an error. */
static int load(struct list *l, const char *path)
{
	struct lines ls;
	char *line;
	size_t n;
	int fd;

	memset(l, 0, sizeof(*l));
	strlcpy(l->path, path, sizeof(l->path));
	if ((fd = pt_open(path, O_RDONLY)) >= 0) {
		lines_init(&ls, fd);
		while ((line = lines_next(&ls, &n))) {
			char buf[TODO_LINE];

			while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
				n--;
			if (n >= sizeof(buf))
				n = sizeof(buf) - 1;
			memcpy(buf, line, n);
			buf[n] = '\0';
			if (!add_line(l, buf))
				break;
		}
		lines_free(&ls);
		pt_close(fd);
	} else if (fd != -ENOENT) {
		return fd;
	}
	return reindex(l);
}

/* Written whole to a new file and put in place, so a pulled card or a
 * flat battery leaves the old list or the new, never half of one. */
static int save(struct list *l)
{
	char tmp[PT_PATH_MAX + 8];
	int fd, err = 0;

	snprintf(tmp, sizeof(tmp), "%s.tmp", l->path);
	if ((fd = pt_open(tmp, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
		return fd;
	for (int i = 0; i < l->nlines && !err; i++) {
		err = write_all(fd, l->lines[i], strlen(l->lines[i]));
		if (!err)
			err = write_all(fd, "\n", 1);
	}
	if ((pt_close(fd) || err) && !err)
		err = -EIO;
	if (err) {
		pt_unlink(tmp);
		return err;
	}
	return pt_rename(tmp, l->path);
}

static int set_item(struct list *l, int i)
{
	char line[TODO_LINE];
	char *copy;

	item_line(&l->items[i], line, sizeof(line));
	if (!(copy = pt_strdup(line)))
		return -ENOMEM;
	pt_free(l->lines[l->items[i].line]);
	l->lines[l->items[i].line] = copy;
	return 0;
}

static int remove_item(struct list *l, int i)
{
	int at = l->items[i].line;

	pt_free(l->lines[at]);
	memmove(&l->lines[at], &l->lines[at + 1], (l->nlines - at - 1) * sizeof(*l->lines));
	l->nlines--;
	return reindex(l);
}

/*
 * Words into an item: an @word is the due date, the rest is the text.
 * Returns 0, or 1 after saying what was wrong.
 */
static int words_to_item(char **words, int n, int today, struct todo_item *it)
{
	size_t len = 0;

	memset(it, 0, sizeof(*it));
	for (int i = 0; i < n; i++) {
		if (words[i][0] == '@' && words[i][1]) {
			if (!(it->due = day_parse(words[i] + 1, today))) {
				pt_dprintf(PT_STDERR, "todo: %s: not a day (today, fri, 30/9, "
					   "2026-09-30, +3)\n", words[i]);
				return 1;
			}
			continue;
		}
		len += snprintf(it->text + len, len < sizeof(it->text) ? sizeof(it->text) - len : 0,
				"%s%s", len ? " " : "", words[i]);
	}
	if (!it->text[0]) {
		pt_dprintf(PT_STDERR, "todo: nothing to add\n");
		return 1;
	}
	return 0;
}

static int add_item(struct list *l, const struct todo_item *it)
{
	char line[TODO_LINE];

	if (!l->nlines && (!add_line(l, "# To do") || !add_line(l, "")))
		return -ENOMEM;
	item_line(it, line, sizeof(line));
	if (!add_line(l, line))
		return -ENOMEM;
	return reindex(l);
}

int todo_items(const char *path, struct todo_item **out)
{
	char file[PT_PATH_MAX];
	struct list l;
	int err, n;

	*out = NULL;
	if (!pda_file(path, "todo.md", file, sizeof(file)))
		return -ENOENT;
	if ((err = load(&l, file))) {
		list_free(&l);
		return err;
	}
	*out = l.items;
	n = l.n;
	l.items = NULL;
	list_free(&l);
	return n;
}

/* ------------------------------------------------------------ showing */

/* "today", "late: yesterday", "Fri 2 Oct": when it is due, for a list. */
static void due_text(const struct todo_item *it, int today, char *out, size_t size)
{
	char day[24];

	out[0] = '\0';
	if (!it->due || it->done)
		return;
	day_name(it->due, today, day, sizeof(day));
	snprintf(out, size, "%s%s", it->due < today ? "late: " : "", day);
}

static void print_list(const struct list *l, int today, int width)
{
	char due[40];

	if (!l->n) {
		pt_printf("nothing to do; `todo add ...` adds something\n");
		return;
	}
	for (int i = 0; i < l->n; i++) {
		const struct todo_item *it = &l->items[i];
		int room;

		due_text(it, today, due, sizeof(due));
		room = width - 8 - (due[0] ? (int)strlen(due) + 1 : 0);
		if (room < 10)
			room = 10;
		if (due[0]) {
			size_t take = utf8_prefix(it->text, strlen(it->text), room);

			pt_printf("%2d  [%c] %.*s%*s %s\n", i + 1, it->done ? 'x' : ' ', (int)take,
				  it->text, room - utf8_width(it->text, take), "", due);
		} else {
			pt_printf("%2d  [%c] %s\n", i + 1, it->done ? 'x' : ' ', it->text);
		}
	}
}

/* ------------------------------------------------------------ the screen */

struct screen {
	struct list	*l;
	int		 sel, top, rows, cols, body, today;
	char		 note[64];
};

static void draw(struct screen *s)
{
	int open = 0, due_today = 0, late = 0;
	char due[40];

	for (int i = 0; i < s->l->n; i++) {
		const struct todo_item *it = &s->l->items[i];

		if (it->done)
			continue;
		open++;
		due_today += it->due == s->today;
		late += it->due && it->due < s->today;
	}
	pt_printf("\x1b[1;1H\x1b[0;1m To do\x1b[0;2m  %d open", open);
	if (due_today)
		pt_printf(", %d today", due_today);
	if (late)
		pt_printf(", \x1b[0;31m%d late\x1b[0;2m", late);
	pt_puts("\x1b[0m\x1b[K");

	for (int r = 0; r < s->body; r++) {
		int i = s->top + r, room, pad;
		const struct todo_item *it = i < s->l->n ? &s->l->items[i] : NULL;
		const char *look;
		size_t take;

		pt_printf("\x1b[%d;1H\x1b[0m", r + 2);
		if (!it) {
			if (!s->l->n && r == 1)
				pt_puts("  \x1b[2mnothing to do: `a` adds something\x1b[0m");
			pt_puts("\x1b[K");
			continue;
		}
		due_text(it, s->today, due, sizeof(due));
		room = s->cols - 5 - (due[0] ? (int)strlen(due) + 1 : 0);
		take = utf8_prefix(it->text, strlen(it->text), room);
		pad = room - utf8_width(it->text, take);
		look = i == s->sel ? "\x1b[0;7m" : it->done ? "\x1b[0;2m" : "\x1b[0m";
		pt_printf("%s %s %.*s%*s", look, it->done ? "[x]" : "[ ]", (int)take, it->text, pad, "");
		if (due[0])
			pt_printf(" %s%s", i == s->sel ? "" : it->due < s->today ? "\x1b[0;31m" :
				  it->due == s->today ? "\x1b[0;1;33m" : "\x1b[0;2m", due);
		pt_puts(" \x1b[0m\x1b[K");
	}
	pt_printf("\x1b[%d;1H\x1b[0;7m%s\x1b[0m\x1b[K", s->rows,
		  s->note[0] ? s->note : " a add  spc done  e edit  d del  c clear done  q");
	s->note[0] = '\0';
}

static void fix_view(struct screen *s)
{
	if (s->sel >= s->l->n)
		s->sel = s->l->n - 1;
	if (s->sel < 0)
		s->sel = 0;
	if (s->sel < s->top)
		s->top = s->sel;
	if (s->sel >= s->top + s->body)
		s->top = s->sel - s->body + 1;
}

/* Keep the cursor on the same item after the list is sorted again. */
static void follow(struct screen *s, int line)
{
	for (int i = 0; i < s->l->n; i++)
		if (s->l->items[i].line == line)
			s->sel = i;
	fix_view(s);
}

/* A line of words into an item, from the prompt. */
static int typed_item(struct screen *s, const char *text, struct todo_item *it)
{
	char buf[TODO_LINE], *words[64], *save = NULL;
	int n = 0;

	strlcpy(buf, text, sizeof(buf));
	for (char *w = strtok_r(buf, " ", &save); w && n < 64; w = strtok_r(NULL, " ", &save))
		words[n++] = w;
	if (words_to_item(words, n, s->today, it)) {
		strlcpy(s->note, " not a day: @today, @fri, @30/9, @+3", sizeof(s->note));
		return 1;
	}
	return 0;
}

static int interactive(struct list *l, int today)
{
	struct screen s = { .l = l, .today = today };
	char buf[TODO_LINE];
	int err = 0;

	pt_tty_size(PT_STDOUT, &s.cols, &s.rows);
	s.body = s.rows - 2;
	if (s.body < 1)
		return -ENOTTY;
	pt_tty_raw(PT_STDIN, true);
	pt_puts("\x1b[?25l\x1b[2J");
	for (bool done = false; !done && !err;) {
		struct todo_item it;
		int line;

		fix_view(&s);
		draw(&s);
		switch (pt_readkey(PT_STDIN)) {
		case PT_KEY_UP: case 'k':	s.sel--; break;
		case PT_KEY_DOWN: case 'j':	s.sel++; break;
		case PT_KEY_PGUP:		s.sel -= s.body; break;
		case PT_KEY_PGDN:		s.sel += s.body; break;
		case 'g': case PT_KEY_HOME:	s.sel = 0; break;
		case 'G': case PT_KEY_END:	s.sel = l->n - 1; break;
		case ' ': case '\r': case '\n': case 'x':
			if (!l->n)
				break;
			line = l->items[s.sel].line;
			l->items[s.sel].done = !l->items[s.sel].done;
			if (!(err = set_item(l, s.sel)) && !(err = save(l)) && !(err = reindex(l)))
				follow(&s, line);
			break;
		case 'a':
			buf[0] = '\0';
			if (!ask_line(s.rows, "add (@day for a due date): ", buf, sizeof(buf)) ||
			    typed_item(&s, buf, &it))
				break;
			if (!(err = add_item(l, &it)) && !(err = save(l)))
				follow(&s, l->nlines - 1);
			break;
		case 'e':
			if (!l->n)
				break;
			item_line(&l->items[s.sel], buf, sizeof(buf));
			memmove(buf, buf + 6, strlen(buf + 6) + 1);	/* no "- [ ] " */
			if (!ask_line(s.rows, "edit: ", buf, sizeof(buf)) || typed_item(&s, buf, &it))
				break;
			line = l->items[s.sel].line;
			it.done = l->items[s.sel].done;
			it.line = line;
			l->items[s.sel] = it;
			if (!(err = set_item(l, s.sel)) && !(err = save(l)) && !(err = reindex(l)))
				follow(&s, line);
			break;
		case 'd': case PT_KEY_DELETE:
			if (!l->n)
				break;
			buf[0] = '\0';
			pt_printf("\x1b[%d;1H\x1b[0;7m delete \"%.30s\"? y/n \x1b[0m\x1b[K", s.rows,
				  l->items[s.sel].text);
			if (pt_readkey(PT_STDIN) == 'y' && !(err = remove_item(l, s.sel)))
				err = save(l);
			break;
		case 'c': {
			int gone = 0;

			for (int i = l->n - 1; i >= 0 && !err; i--)
				if (l->items[i].done) {
					err = remove_item(l, i);
					gone++;
					i = l->n;	/* the list is sorted again */
				}
			if (!err && gone)
				err = save(l);
			snprintf(s.note, sizeof(s.note), " %d done thing%s cleared", gone,
				 gone == 1 ? "" : "s");
			break;
		}
		case 'q': case PT_KEY_ESC: case PT_CTRL('c'): case PT_KEY_EOF: case PT_KEY_ERROR:
			done = true;
			break;
		}
	}
	pt_puts("\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
	pt_tty_raw(PT_STDIN, false);
	return err;
}

/* ------------------------------------------------------------ the command */

/* "3 5 7": the items those numbers show, by line, so removing one does not
 * move the next. */
static int numbered(struct list *l, char **argv, int argc, int *lines)
{
	for (int i = 0; i < argc; i++) {
		char *end;
		long v = strtol(argv[i], &end, 10);

		if (*end || v < 1 || v > l->n) {
			pt_dprintf(PT_STDERR, "todo: %s: no such item (`todo ls` numbers them)\n",
				   argv[i]);
			return 1;
		}
		lines[i] = l->items[v - 1].line;
	}
	return 0;
}

static int by_line(struct list *l, int line)
{
	for (int i = 0; i < l->n; i++)
		if (l->items[i].line == line)
			return i;
	return -1;
}

PT_COMPLETE(todo, ": ls add done undo rm clear -f -d\n-f: <file:.md>\n")

PT_PROGRAM(todo, "a to-do list, in ~/todo.md\n"
	   "usage: todo                 the list, to work on\n"
	   "       todo ls              print it\n"
	   "       todo add TEXT [@DAY] @tomorrow, @fri, @30/9\n"
	   "       todo done|undo|rm N... todo clear (the done)\n"
	   "  -f FILE another list, -d DAY as if today were DAY\n"
	   "The list is a Markdown checklist, open things first,\n"
	   "the soonest due at the top; `notes` shows it too.")
{
	struct opt o = { .ind = 1 };
	struct list l;
	char path[PT_PATH_MAX];
	const char *file = NULL, *cmd;
	int c, err, today = day_today(), ret = 0, width, rows;

	while ((c = getopt_pt(&o, "todo", argc, argv, "f:d:")) != -1) {
		if (c == 'f') {
			file = o.arg;
		} else if (c == 'd') {
			if (!(today = day_parse(o.arg, day_today()))) {
				pt_dprintf(PT_STDERR, "todo: %s: not a day\n", o.arg);
				return 2;
			}
		} else {
			return 2;
		}
	}
	if (!pda_file(file, "todo.md", path, sizeof(path)))
		return fail("todo", "~/todo.md", -ENAMETOOLONG);
	if ((err = load(&l, path))) {
		list_free(&l);
		return fail("todo", path, err);
	}
	cmd = o.ind < argc ? argv[o.ind] : NULL;
	width = 80;
	if (pt_isatty(PT_STDOUT))
		pt_tty_size(PT_STDOUT, &width, &rows);

	if (!cmd && pt_isatty(PT_STDIN) && pt_isatty(PT_STDOUT)) {
		err = interactive(&l, today);
	} else if (!cmd || !strcmp(cmd, "ls")) {
		print_list(&l, today, width);
	} else if (!strcmp(cmd, "add")) {
		struct todo_item it;

		if (words_to_item(argv + o.ind + 1, argc - o.ind - 1, today, &it)) {
			ret = 2;
		} else if (!(err = add_item(&l, &it)) && !(err = save(&l))) {
			int at = by_line(&l, l.nlines - 1);

			pt_printf("%d  [ ] %s\n", at + 1, it.text);
		}
	} else if (!strcmp(cmd, "done") || !strcmp(cmd, "undo") || !strcmp(cmd, "rm")) {
		int n = argc - o.ind - 1, *lines = pt_malloc((n + 1) * sizeof(int));

		if (!n || !lines) {
			pt_dprintf(PT_STDERR, "usage: todo %s N...\n", cmd);
			ret = 2;
		} else if (numbered(&l, argv + o.ind + 1, n, lines)) {
			ret = 1;
		} else {
			for (int k = 0; k < n && !err; k++) {
				int i = by_line(&l, lines[k]);

				if (i < 0)
					continue;	/* named twice */
				if (cmd[0] == 'r') {
					err = remove_item(&l, i);
					for (int j = k + 1; j < n; j++)	/* the lines after move up */
						lines[j] -= lines[j] > lines[k];
				} else {
					l.items[i].done = cmd[0] == 'd';
					err = set_item(&l, i);
				}
			}
			if (!err && !(err = save(&l)) && !(err = reindex(&l)))
				print_list(&l, today, width);
		}
		pt_free(lines);
	} else if (!strcmp(cmd, "clear")) {
		int gone = 0;

		for (int i = l.n - 1; i >= 0 && !err; i--)
			if (l.items[i].done) {
				err = remove_item(&l, i);
				gone++;
				i = l.n;
			}
		if (!err && !(err = save(&l)))
			pt_printf("%d done thing%s cleared\n", gone, gone == 1 ? "" : "s");
	} else {
		pt_dprintf(PT_STDERR, "todo: %s: ls, add, done, undo, rm or clear\n", cmd);
		ret = 2;
	}
	list_free(&l);
	if (err)
		return fail("todo", path, err);
	return ret;
}
