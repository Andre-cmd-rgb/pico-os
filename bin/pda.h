/*
 * What todo and calendar share: the to-do list is read by the calendar
 * too, to show what is due on each day.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct todo_item {
	int	line;			/* in the file */
	bool	done;
	int	due;			/* YYYYMMDD, or 0 */
	char	text[160];		/* without the box and the date */
};

/* The items of a list (~/agenda/todo.md unless `path`), in the order they are
 * shown: open ones by due date, then the rest, then what is done. The
 * array is pt_malloc'd; the count, or <0 if the file cannot be read. */
int	todo_items(const char *path, struct todo_item **out);

/* ~/agenda/NAME, or `given` if there is one. */
bool	pda_file(const char *given, const char *name, char *out, size_t size);
