/*
 * pick - choose a file from a list.
 *
 * A machine with a thumb keyboard should not make anyone type a path. The
 * emulator, the picture viewer and the player all open a file out of a
 * directory, so they all ask here instead: a list, arrows to move through
 * it, Enter to take one, Esc to change your mind. Directories can be
 * walked into, and ".." walks back out, so a card full of folders is as
 * easy to get around as a flat one.
 *
 * Only the rows that changed are drawn again, as in the editor, which
 * keeps it quick over the serial console as well as on the panel.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define GROW		32		/* entries per growth of the list */

struct entry {
	char	 name[PT_NAME_MAX];
	uint64_t size;
	bool	 is_dir;
};

struct list {
	struct entry	*e;
	int		 n, cap;
};

static bool ends_with(const char *name, const char *ext)
{
	size_t n = strlen(name), m = strlen(ext);

	if (n <= m)
		return false;
	for (size_t i = 0; i < m; i++) {
		char a = name[n - m + i], b = ext[i];

		if (a >= 'A' && a <= 'Z')
			a += 'a' - 'A';
		if (a != b)
			return false;
	}
	return true;
}

static bool wanted(const struct pt_dirent *ent, const char *const *exts)
{
	if (ent->name[0] == '.')
		return false;		/* nothing hidden, and no . or .. */
	if (ent->is_dir || !exts)
		return true;
	for (const char *const *e = exts; *e; e++)
		if (ends_with(ent->name, *e))
			return true;
	return false;
}

/* Directories first, then names, the way a file manager orders them. */
static int by_name(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	if (x->is_dir != y->is_dir)
		return x->is_dir ? -1 : 1;
	for (const char *p = x->name, *q = y->name;; p++, q++) {
		char c = *p, d = *q;

		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (d >= 'A' && d <= 'Z')
			d += 'a' - 'A';
		if (c != d)
			return (unsigned char)c - (unsigned char)d;
		if (!c)
			return 0;
	}
}

static void list_free(struct list *l)
{
	pt_free(l->e);
	l->e = NULL;
	l->n = l->cap = 0;
}

static int list_read(struct list *l, const char *dir, const char *const *exts)
{
	pt_dir_t *d;
	struct pt_dirent ent;
	int err = pt_opendir(dir, &d);

	list_free(l);
	if (err)
		return err;
	while (pt_readdir(d, &ent) == 1) {
		struct pt_stat st;
		char path[PT_PATH_MAX];

		if (!wanted(&ent, exts))
			continue;
		if (l->n == l->cap) {
			struct entry *bigger = pt_realloc(l->e, (l->cap + GROW) * sizeof(*bigger));

			if (!bigger) {
				pt_closedir(d);
				return -ENOMEM;
			}
			l->e = bigger;
			l->cap += GROW;
		}
		struct entry *e = &l->e[l->n++];
		strlcpy(e->name, ent.name, sizeof(e->name));
		e->is_dir = ent.is_dir;
		e->size = 0;
		if (!ent.is_dir && join_path(dir, ent.name, path, sizeof(path)) &&
		    !pt_stat(path, &st))
			e->size = st.size;
	}
	pt_closedir(d);
	qsort(l->e, l->n, sizeof(*l->e), by_name);
	return 0;
}

/* The heading: what is being chosen, and where the list is looking. */
static void heading(char *out, size_t size, const char *title, const char *dir, int cols)
{
	const char *home = pt_getenv("HOME");
	char shown[PT_PATH_MAX];
	int n;

	if (home && *home && !strncmp(dir, home, strlen(home)))
		snprintf(shown, sizeof(shown), "~%s", dir + strlen(home));
	else
		strlcpy(shown, dir, sizeof(shown));
	if (!title)
		title = shown;
	n = snprintf(out, size, " %s", title);
	/* The directory on the right, when it is not what the title says
	 * already and there is room for it. */
	if (strcmp(title, shown) && n + (int)strlen(shown) + 2 <= cols)
		snprintf(out + n, size - n, "%*s ", cols - n - 1, shown);
}

/* One row, already positioned. `width` is the terminal. */
static void draw_row(const struct list *l, int i, bool selected, int width)
{
	char size[16] = "";
	const struct entry *e = &l->e[i];
	int room;

	if (e->is_dir)
		snprintf(size, sizeof(size), "%s", "dir");
	else
		human_size(e->size, size, sizeof(size));
	room = width - (int)strlen(size) - 4;
	if (room < 1)
		room = 1;
	pt_printf("\x1b[2K%s %-*.*s %s\x1b[0m", selected ? "\x1b[7m" : " ",
		  room, room, e->name, size);
}

static void draw_all(const struct list *l, const char *title, const char *dir,
		     int sel, int top, int cols, int rows)
{
	int body = rows - 2;

	char head[PT_PATH_MAX + 32];

	heading(head, sizeof(head), title, dir, cols);
	pt_printf("\x1b[H\x1b[2K\x1b[7m%-*.*s\x1b[0m\n", cols, cols, head);
	for (int r = 0; r < body; r++) {
		int i = top + r;

		pt_printf("\x1b[%d;1H", r + 2);
		if (i < l->n)
			draw_row(l, i, i == sel, cols);
		else
			pt_puts("\x1b[2K");
	}
	pt_printf("\x1b[%d;1H\x1b[2K\x1b[2m", rows);
	if (l->n)
		pt_printf(" %d/%d  enter opens, esc quits", sel + 1, l->n);
	else
		pt_puts(" nothing here  -  esc quits");
	pt_puts("\x1b[0m");
}

/* The first entry at or after `from` whose name starts with `c`. */
static int jump_to(const struct list *l, int from, int c)
{
	if (c >= 'A' && c <= 'Z')
		c += 'a' - 'A';
	for (int k = 1; k <= l->n; k++) {
		int i = (from + k) % l->n;
		int first = l->e[i].name[0];

		if (first >= 'A' && first <= 'Z')
			first += 'a' - 'A';
		if (first == c)
			return i;
	}
	return from;
}

int pick_file(const char *dir, const char *const *exts, const char *title,
	      char *out, size_t size)
{
	struct list l = { 0 };
	char here[PT_PATH_MAX];
	int sel = 0, top = 0, cols, rows, body, ret;
	bool reread = true, redraw = true;

	if (pt_abspath(dir, here, sizeof(here)))
		strlcpy(here, dir, sizeof(here));
	pt_tty_size(PT_STDOUT, &cols, &rows);
	body = rows - 2;
	if (body < 1)
		return -ENOTTY;
	pt_tty_raw(PT_STDIN, true);
	pt_puts("\x1b[?25l\x1b[2J");

	for (;;) {
		if (reread) {
			if ((ret = list_read(&l, here, exts)))
				goto done;
			sel = top = 0;
			reread = false;
			redraw = true;
		}
		if (redraw) {
			draw_all(&l, title, here, sel, top, cols, rows);
			redraw = false;
		}

		int key = pt_readkey(PT_STDIN);
		int was = sel;

		switch (key) {
		case PT_KEY_UP:		sel--; break;
		case PT_KEY_DOWN:	sel++; break;
		case PT_KEY_PGUP:	sel -= body; break;
		case PT_KEY_PGDN:	sel += body; break;
		case PT_KEY_HOME:	sel = 0; break;
		case PT_KEY_END:	sel = l.n - 1; break;
		case PT_KEY_ESC:
		case PT_CTRL('c'):
		case 'q':
			ret = -ECANCELED;
			goto done;
		case PT_KEY_LEFT:
		case PT_CTRL('h'): {
			char *slash = strrchr(here, '/');

			if (slash && slash != here) {
				*slash = '\0';
				reread = true;
			} else if (slash && here[1]) {
				here[1] = '\0';
				reread = true;
			}
			continue;
		}
		case '\r':
		case '\n':
		case PT_KEY_RIGHT:
			if (!l.n)
				continue;
			if (l.e[sel].is_dir) {
				char next[PT_PATH_MAX];

				if (join_path(here, l.e[sel].name, next, sizeof(next))) {
					strlcpy(here, next, sizeof(here));
					reread = true;
				}
				continue;
			}
			ret = join_path(here, l.e[sel].name, out, size) ? 0 : -ENAMETOOLONG;
			goto done;
		case PT_KEY_EOF:
		case PT_KEY_ERROR:
			ret = -EIO;
			goto done;
		default:
			if (key >= ' ' && key < 0x7f && l.n)
				sel = jump_to(&l, sel, key);
			break;
		}

		if (sel < 0)
			sel = 0;
		if (sel >= l.n)
			sel = l.n ? l.n - 1 : 0;
		if (sel == was)
			continue;
		if (sel < top || sel >= top + body) {
			top = sel - body / 2;
			if (top > l.n - body)
				top = l.n - body;
			if (top < 0)
				top = 0;
			redraw = true;
			continue;
		}
		/* Same page: only the row left and the row arrived at. */
		pt_printf("\x1b[%d;1H", was - top + 2);
		draw_row(&l, was, false, cols);
		pt_printf("\x1b[%d;1H", sel - top + 2);
		draw_row(&l, sel, true, cols);
		pt_printf("\x1b[%d;1H\x1b[2K\x1b[2m %d/%d  enter opens, esc quits\x1b[0m",
			  rows, sel + 1, l.n);
	}
done:
	list_free(&l);
	pt_puts("\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
	pt_tty_raw(PT_STDIN, false);
	return ret;
}
