/*
 * man: the documentation, on the device.
 *
 * The pages are built into the firmware rather than kept in a file, so
 * they are there after a reformat, on a board with no card, and whatever
 * else has gone wrong -- which is exactly when they are wanted.
 *
 * `help <command>` still explains a single command; this is for the
 * longer things: the language, the wiring, how the system is put
 * together.
 */
#include <string.h>

#include "util.h"

/* Each page is a text file embedded at build time; see bin/CMakeLists.txt. */
#define PAGE(name)							\
	extern const uint8_t name##_start[] asm("_binary_" #name "_txt_start");	\
	extern const uint8_t name##_end[]   asm("_binary_" #name "_txt_end")

PAGE(intro);
PAGE(commands);
PAGE(language);
PAGE(hardware);

static const struct {
	const char	*name;
	const char	*about;
	const uint8_t	*start, *end;
} pages[] = {
	{ "intro", "what this is and how it fits together", intro_start, intro_end },
	{ "commands", "every command, by what it is for", commands_start, commands_end },
	{ "language", "the `a` language", language_start, language_end },
	{ "hardware", "the board, its pins and what is attached", hardware_start, hardware_end },
};

#define NPAGES (sizeof(pages) / sizeof(pages[0]))

PT_PROGRAM(man, "read the manual\n"
	   "usage: man [page]\n"
	   "Without a page, lists them. `man language | more` reads a long\n"
	   "one a screen at a time, and `help <command>` explains one command.")
{
	if (argc > 2) {
		pt_dprintf(PT_STDERR, "usage: man [page]\n");
		return 2;
	}
	if (argc == 1) {
		pt_printf("pages:\n");
		for (size_t i = 0; i < NPAGES; i++)
			pt_printf("  %-10s %s\n", pages[i].name, pages[i].about);
		pt_printf("\nread one with `man <page>`, or `man <page> | more`\n");
		return 0;
	}
	for (size_t i = 0; i < NPAGES; i++) {
		if (strcmp(argv[1], pages[i].name))
			continue;
		return write_all(PT_STDOUT, (const char *)pages[i].start,
				 pages[i].end - pages[i].start) ? 1 : 0;
	}
	pt_dprintf(PT_STDERR, "man: no page called %s; `man` lists them\n", argv[1]);
	return 1;
}
