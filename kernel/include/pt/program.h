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
	bool			 any_core;	/* may run on the kernel's core too */
	struct pt_program	*next;
};

void program_register(struct pt_program *prog);
const struct pt_program *program_find(const char *name);
const struct pt_program *program_first(void);	/* sorted by name */

/* For the few commands whose name is not a C identifier, such as one with
 * a hyphen in it: the C name and the name typed at the prompt differ. */
#define PT_PROGRAM_FULL(ident_, name_, kb_, any_, help_)			\
	static int pt_main_##ident_(int argc, char **argv);			\
	static struct pt_program pt_prog_##ident_ = {				\
		.name = name_, .main = pt_main_##ident_, .help = help_,		\
		.stack_kb = kb_, .any_core = any_,				\
	};									\
	__attribute__((constructor)) static void pt_reg_##ident_(void)		\
	{									\
		program_register(&pt_prog_##ident_);				\
	}									\
	static int pt_main_##ident_(int argc, char **argv)

#define PT_PROGRAM_NAMED(ident_, name_, kb_, help_)				\
	PT_PROGRAM_FULL(ident_, name_, kb_, false, help_)
#define PT_PROGRAM_STACK(name_, kb_, help_) PT_PROGRAM_NAMED(name_, #name_, kb_, help_)
/*
 * A program that mostly waits -- on the speaker, say -- and whose work
 * would otherwise take time from a game on the programs' core. It must
 * not hog a core for seconds on end: the kernel's core has its idle task
 * watched, and the watchdog restarts the chip if it never runs.
 */
#define PT_PROGRAM_ANYCORE(name_, kb_, help_) PT_PROGRAM_FULL(name_, #name_, kb_, true, help_)
#define PT_PROGRAM(name_, help_) PT_PROGRAM_STACK(name_, 0, help_)

/*
 * What a program's arguments can be, for Tab at the prompt. `spec` is lines
 * of "AFTER: WORDS". AFTER is the word before the one being completed --
 * nothing for the first argument, "*" for any argument after the first --
 * and WORDS what it may be. Among them <file>, <file:.mp3.wav> (those, and
 * directories), <dir> and <command> stand for the names there are at the
 * time, and <more> for what the program's `more` adds (a theme's names,
 * say). Words starting with - are offered once a - is typed. A program
 * with no spec completes file names.
 *
 *	PT_COMPLETE(led, ": on off heartbeat charge\n")
 */
typedef void (*pt_complete_add)(void *ctx, const char *word);

struct pt_completion {
	const char		*prog;
	const char		*spec;
	void			(*more)(const char *after, pt_complete_add add, void *ctx);
	struct pt_completion	*next;
};

void completion_register(struct pt_completion *c);
const struct pt_completion *completion_find(const char *prog);

#define PT_COMPLETE_NAMED(ident_, name_, spec_, more_)				\
	static struct pt_completion pt_comp_##ident_ = {			\
		.prog = name_, .spec = spec_, .more = more_,			\
	};									\
	__attribute__((constructor)) static void pt_compreg_##ident_(void)	\
	{									\
		completion_register(&pt_comp_##ident_);				\
	}
#define PT_COMPLETE_MORE(name_, spec_, more_) PT_COMPLETE_NAMED(name_, #name_, spec_, more_)
#define PT_COMPLETE(name_, spec_) PT_COMPLETE_NAMED(name_, #name_, spec_, NULL)

/*
 * Services: what an application keeps running in the background from boot
 * (a connection it holds, say). init.c starts each once the filesystems,
 * /etc and the Wi-Fi's saved networks are there; `start` returns at once,
 * leaving whatever it started to run. An application built from outside
 * this tree (make APPS=...) registers its own, as its programs do.
 */
struct pt_service {
	const char		*name;
	void			(*start)(void);
	struct pt_service	*next;
};

void service_register(struct pt_service *s);
void services_start(void);

#define PT_SERVICE(ident_, start_)						\
	static struct pt_service pt_svc_##ident_ = {				\
		.name = #ident_, .start = start_,				\
	};									\
	__attribute__((constructor)) static void pt_svcreg_##ident_(void)	\
	{									\
		service_register(&pt_svc_##ident_);				\
	}

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
