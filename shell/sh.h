#pragma once

#include "pt/match.h"
#include "pt/sys.h"

#define SH_LINE_MAX	512
#define SH_HISTORY	64
/*
 * Measured with `ps`: a login shell peaks at about 5 KB of this, and the
 * parser's own recursion is bounded by stack_limit below. 10 KB leaves
 * twice the headroom ever used, and two shells (screen and network) then
 * cost 12 KB less of the internal RAM everything else competes for.
 */
#define SH_STACK_KB	10

struct history {
	char	*entry[SH_HISTORY];
	int	 count;
};

struct candidates {
	char	**name;
	bool	 *is_dir;
	int	  count;
	size_t	  typed;	/* bytes of the word the names complete */
};

struct sh;

/* lineedit.c */
enum {
	LE_EOF = -1,		/* Ctrl-D on an empty line */
	LE_INTERRUPT = -2,	/* Ctrl-C */
	LE_ERROR = -3,
};

int	lineedit(struct sh *sh, const char *prompt, char *buf, size_t size);
void	history_add(struct history *h, const char *line);

/* sh.c */
struct history *sh_history(struct sh *sh);
int	sh_candidates(struct sh *sh, const char *line, size_t word_start, const char *word,
		      struct candidates *out);
size_t	word_scan(const char *line, size_t pos, char *word, size_t size, char *quote);
void	candidates_free(struct candidates *c);
int	run_file(struct sh *sh, const char *path);

/* ------------------------------------------------------------ memory */

struct arena_chunk;

/* Bump allocator, released in stack order back to a mark. */
struct arena {
	struct arena_chunk *top;
};

struct arena_mark {
	struct arena_chunk *chunk;
	size_t		    used;
};

void	*arena_alloc(struct arena *a, size_t n);	/* zeroed */
struct arena_mark arena_mark(struct arena *a);
void	 arena_release(struct arena *a, struct arena_mark mark);

/* A growable string, always NUL-terminated once anything was added. */
struct strbuf {
	char	*s;
	size_t	 len, cap;
	bool	 oom;
};

void	sb_add(struct strbuf *b, const char *s, size_t n);
void	sb_putc(struct strbuf *b, char c);
void	sb_puts(struct strbuf *b, const char *s);
void	sb_free(struct strbuf *b);

/* An argv under construction: owns its strings, NULL-terminated. */
struct fields {
	char	**v;
	int	  n, cap;
	bool	  oom;
};

void	fields_add(struct fields *f, const char *s);
void	fields_take(struct fields *f, char *s);
void	fields_free(struct fields *f);

/* ------------------------------------------------------------ syntax tree */

enum node_type {
	N_SIMPLE,	/* words, redirs */
	N_PIPELINE,	/* stages from body, linked by next; negate */
	N_AND,		/* cond && body */
	N_OR,		/* cond || body */
	N_LIST,		/* items from body, linked by next; background */
	N_BRACE,	/* { body } */
	N_SUBSHELL,	/* ( body ) */
	N_IF,		/* if cond then body else alt; elif is an N_IF in alt */
	N_WHILE,	/* while cond do body */
	N_UNTIL,	/* until cond do body */
	N_FOR,		/* for name [in words] do body */
	N_CASE,		/* case words in items */
	N_FUNCDEF,	/* name() body */
};

enum redir_type {
	R_IN,		/* n<file */
	R_OUT,		/* n>file */
	R_APPEND,	/* n>>file */
	R_DUP,		/* n>&m, n<&m */
	R_HEREDOC,	/* n<<WORD, n<<-WORD: the lines that follow */
};

/* Text as written, quotes and $ intact: expanded only when it runs. */
struct word {
	struct word	*next;
	char		 text[];
};

struct redir {
	struct redir	*next;
	enum redir_type	 type;
	int		 fd;
	struct word	*target;
	const char	*body;		/* R_HEREDOC: its text, delimiter line left out */
	bool		 quoted;	/* R_HEREDOC: the delimiter was quoted: no expansion */
};

struct case_item {
	struct case_item *next;
	struct word	 *patterns;
	struct node	 *body;
};

struct node {
	enum node_type	  type;
	bool		  background;	/* a list item ended by & */
	bool		  negate;	/* ! pipeline */
	bool		  in_list;	/* for ... in */
	struct node	 *next;
	struct node	 *cond, *body, *alt;
	struct word	 *words;
	struct redir	 *redirs;
	struct case_item *items;
	const char	 *name;
	const char	 *src;		/* the command's source text, for child shells */
	size_t		  src_len;
	const char	 *here;		/* here-document bodies after its line, for them too */
	size_t		  here_len;
};

/* parse.c */
enum {
	PARSE_OK,
	PARSE_INCOMPLETE,	/* ran out of text inside a construct */
	PARSE_ERROR,
};

struct parse_error {
	int		 status;
	const char	*msg;
	const char	*near;		/* the offending token, not NUL-terminated */
	int		 near_len;
};

int	parse(struct sh *sh, struct arena *a, const char *text, struct node **out,
	      struct parse_error *err);
const char *parse_skip(struct sh *sh, const char *p);
bool	valid_name(const char *s, size_t len);

/* ------------------------------------------------------------ expansion */

#define CTLESC		'\x01'	/* marks the next byte as quoted */

enum {
	X_SPLIT = 1 << 0,	/* field splitting */
	X_GLOB = 1 << 1,	/* pathname expansion */
	X_PATTERN = 1 << 2,	/* keep quote marks: the result is a pattern */
	X_ASSIGN = 1 << 3,	/* ~ also after : */
};

/* expand.c: both print their own errors */
int	expand_words(struct sh *sh, struct word *w, struct fields *out);
char	*expand_word(struct sh *sh, const char *text, int flags);
/* A pattern as the shell holds it, quoted characters marked with CTLESC. */
static inline bool pattern_match(const char *pattern, const char *s)
{
	return pt_glob_match(pattern, s, CTLESC, 0);
}

/* arith.c */
int	arith_eval(struct sh *sh, const char *expr, long long *result);

/* ------------------------------------------------------------ the shell */

struct function {
	struct function	*next;
	const char	*name;
	struct node	*body;
	struct arena	 arena;
	int		 refs;
	char		 src[];		/* the whole definition */
};

struct local_var {
	struct local_var *next;
	char		 *old;		/* NULL: was unset */
	char		  name[];
};

/* A job: what one command line started, in the background or stopped. */
#define MAX_JOBS	16
#define MAX_STAGES	8

struct job {
	int		 id;		/* %1, %2 ...; 0 is a free slot */
	int		 pgid;
	int		 pid[MAX_STAGES];
	bool		 done[MAX_STAGES];
	int		 n, status;	/* the last process's status */
	bool		 stopped;
	char		*cmd;		/* as it was typed */
};

struct alias {
	struct alias	*next;
	char		*value;
	char		 name[];
};

enum { TRAP_EXIT, TRAP_INT, TRAP_TERM, NTRAPS };

struct sh {
	int		  status;
	int		  subst_status;	/* of the last $(...) */
	int		  last_bg;	/* $! */
	int		  argc;		/* positional parameters, $0 first */
	char		**argv;
	void		 *params;	/* the block holding argv, if the shell owns it */
	int		  pgid;		/* the process group the shell runs in */
	bool		  interactive;	/* owns the terminal: job control */
	bool		  exit_requested;
	bool		  interrupted;
	bool		  returning;
	int		  breaking;	/* loop levels still to leave */
	int		  continuing;
	int		  loops;	/* loop depth in the current function */
	int		  funcs;	/* function call depth */
	int		  sourcing;
	/* set -e -u -x -o pipefail */
	bool		  errexit, nounset, xtrace, pipefail;
	int		  no_errexit;	/* inside a condition: a failure is an answer */
	struct job	  jobs[MAX_JOBS];
	int		  current_job;	/* the id %% and fg with nothing mean */
	bool		  warned_stopped;	/* exit once with stopped jobs: a warning */
	char		 *traps[NTRAPS];	/* NULL: as usual; "": ignored */
	bool		  in_trap;
	struct alias	 *aliases;
	const char	 *expanding[8];	/* aliases being expanded, not again */
	int		  nexpanding;
	uintptr_t	  stack_limit;	/* lowest safe stack address */
	struct local_var *locals;
	struct function	 *functions;
	struct arena	  ast;
	struct history	  history;
};

/* jobs.c */
struct job *job_add(struct sh *sh, int pgid, const int *pids, int n, const char *cmd, size_t len);
struct job *job_find(struct sh *sh, const char *spec, const char *who);
void	job_free(struct job *j);
void	job_ended(struct sh *sh, int pid, int st, bool report);
void	jobs_reap(struct sh *sh);
void	job_report(struct job *j, const char *what);
bool	jobs_stopped(struct sh *sh);
int	job_wait(struct sh *sh, struct job *j, bool cont);
int	jobs_expand(struct sh *sh, struct fields *args, const char *who);
int	builtin_jobs(struct sh *sh, int argc, char **argv);
int	builtin_fg(struct sh *sh, int argc, char **argv);
int	builtin_bg(struct sh *sh, int argc, char **argv);

/* exec.c */
int	exec_node(struct sh *sh, struct node *n);
void	sh_caught(struct sh *sh);		/* a signal arrived: its trap, or stop */
int	sh_take_signal(void);			/* which, and it is no longer pending */
void	sh_signal(struct sh *sh, int sig);
int	run_text(struct sh *sh, const char *text, const char *where);
int	capture(struct sh *sh, const char *text, struct strbuf *out);
int	run_command_argv(struct sh *sh, int argc, char **argv);
bool	sh_stack_low(struct sh *sh);
bool	sh_unwinding(struct sh *sh);
void	sh_parse_error(const char *where, int line, const char *text,
		       const struct parse_error *err);
int	process_group(void);
struct function *function_find(struct sh *sh, const char *name);
void	function_put(struct function *f);
void	function_remove(struct sh *sh, const char *name);

/* builtins.c */
struct builtin {
	const char *name;
	int	  (*fn)(struct sh *sh, int argc, char **argv);
	bool	    special;	/* found before functions */
};

const struct builtin *builtin_find(const char *name);
struct alias *alias_find(struct sh *sh, const char *name);

/* test.c */
int	sh_test(int argc, char **argv);
