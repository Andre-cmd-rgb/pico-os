/*
 * Built-in programs.
 *
 *	PT_PROGRAM(ls, "list directory contents")
 *	{
 *		...
 *		return 0;
 *	}
 *
 * defines the program's main(argc, argv) and registers it before app_main runs, the way
 * an initcall registers a driver. Nothing else needs to know the program
 * exists: the shell, `help` and tab completion all read the registry.
 */
#pragma once

#include "pt/sys.h"

typedef int (*pt_main_fn)(int argc, char **argv);

struct pt_program {
	const char		*name;
	pt_main_fn		 main;
	const char		*help;
	uint16_t		 stack_kb;	/* 0: CONFIG_PT_PROC_STACK_KB */
	struct pt_program	*next;
};

void program_register(struct pt_program *prog);
const struct pt_program *program_find(const char *name);
const struct pt_program *program_first(void);	/* sorted by name */

/* For the few commands whose name is not a C identifier, such as one with
 * a hyphen in it: the C name and the name typed at the prompt differ. */
#define PT_PROGRAM_NAMED(ident_, name_, kb_, help_)				\
	static int pt_main_##ident_(int argc, char **argv);			\
	static struct pt_program pt_prog_##ident_ = {				\
		.name = name_, .main = pt_main_##ident_, .help = help_,		\
		.stack_kb = kb_,						\
	};									\
	__attribute__((constructor)) static void pt_reg_##ident_(void)		\
	{									\
		program_register(&pt_prog_##ident_);				\
	}									\
	static int pt_main_##ident_(int argc, char **argv)

#define PT_PROGRAM_STACK(name_, kb_, help_) PT_PROGRAM_NAMED(name_, #name_, kb_, help_)
#define PT_PROGRAM(name_, help_) PT_PROGRAM_STACK(name_, 0, help_)

/*
 * Executable loaders. A loader claims files by their first bytes and runs
 * them inside the new process. v1 ships the script loader (#! and .sh);
 * a bytecode VM or native-code loader registers here the same way.
 */
struct pt_loader {
	const char		*name;
	uint16_t		 stack_kb;
	bool			(*probe)(const char *path, const uint8_t *head, size_t n);
	int			(*exec)(const char *path, int argc, char **argv);
	struct pt_loader	*next;
};

void loader_register(struct pt_loader *loader);
const struct pt_loader *loader_find(const char *path);
