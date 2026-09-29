/*
 * Tab completion: what the word under the cursor could become.
 *
 * The line is read the way the parser will read it -- quotes, backslashes,
 * the | ; & ( that start a new command -- so that a name with spaces in it
 * completes inside the quote it was begun with, and the words before it
 * say which command the word is an argument to. In command position the
 * candidates are the programs and functions; after a command they are what
 * the command registered with PT_COMPLETE (its subcommands, its kind of
 * file), or else the file names there are.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pt/program.h"
#include "sh.h"

#define MAX_WORDS	8		/* of the command before the cursor, kept */

/* The words of the command being typed, up to the one being completed. */
struct words {
	char	 text[MAX_WORDS][64];
	int	 n;			/* how many there were; the first MAX_WORDS kept */
};

static void words_add(struct words *w, const char *s, size_t len)
{
	if (w->n < MAX_WORDS) {
		len = len < sizeof(w->text[0]) - 1 ? len : sizeof(w->text[0]) - 1;
		memcpy(w->text[w->n], s, len);
		w->text[w->n][len] = '\0';
	}
	w->n++;
}

static bool is_starter(const char *s)
{
	static const char *const starters[] = {
		"then", "do", "else", "elif", "if", "while", "until", "{", "!", "time",
	};

	for (size_t k = 0; k < sizeof(starters) / sizeof(starters[0]); k++)
		if (!strcmp(s, starters[k]))
			return true;
	return false;
}

/*
 * Reads `line` up to `pos`: the words of the command there, unquoted, into
 * `w` (a new command after | ; & ( or a keyword that begins one), and the
 * word the cursor is in into `word`. Returns where that word starts in the
 * line, and in `quote` the quote it is still inside, if any.
 */
static size_t scan(const char *line, size_t pos, struct words *w, char *word, size_t size,
		   char *quote)
{
	size_t start = 0, n = 0;
	bool in_word = false;
	char q = 0;

	w->n = 0;
	for (size_t i = 0; i < pos; i++) {
		char c = line[i];

		if (q) {
			if (c == q) {
				q = 0;
			} else if (q == '"' && c == '\\' && i + 1 < pos && strchr("\"\\$`", line[i + 1])) {
				if (n + 1 < size)
					word[n++] = line[++i];
			} else if (n + 1 < size) {
				word[n++] = c;
			}
			continue;
		}
		if (c == ' ' || c == '\t' || strchr("|;&()<>", c)) {
			if (in_word) {
				word[n] = '\0';
				if (!w->n && is_starter(word))
					;		/* a keyword: the command comes next */
				else
					words_add(w, word, n);
				in_word = false;
				n = 0;
			}
			if (strchr("|;&(", c))
				w->n = 0;		/* a new command */
			continue;
		}
		if (!in_word) {
			in_word = true;
			start = i;
		}
		if (c == '\'' || c == '"') {
			q = c;
		} else if (c == '\\' && i + 1 < pos) {
			if (n + 1 < size)
				word[n++] = line[++i];
		} else if (n + 1 < size) {
			word[n++] = c;
		}
	}
	if (!in_word)
		start = pos;
	word[n] = '\0';
	*quote = q;
	return start;
}

size_t word_scan(const char *line, size_t pos, char *word, size_t size, char *quote)
{
	struct words w;

	return scan(line, pos, &w, word, size, quote);
}

bool line_has_secret(const char *line)
{
	size_t len = strlen(line);
	char word[64], quote;
	struct words w;

	/* each command on the line: its end is a ; & | outside quotes */
	for (size_t pos = 0; pos <= len; pos++) {
		if (pos < len && !strchr(";&|", line[pos]))
			continue;
		scan(line, pos, &w, word, sizeof(word), &quote);
		if (quote)
			continue;
		if (w.n + (*word != '\0') >= 4 && !strcmp(w.text[0], "wifi") &&
		    !strcmp(w.text[1], "connect"))
			return true;
	}
	return false;
}

static int add_candidate(struct candidates *c, const char *name, bool is_dir)
{
	for (int i = 0; i < c->count; i++)
		if (!strcmp(c->name[i], name))
			return 0;		/* a subcommand that is also a file */
	if (c->count % 32 == 0) {
		char **names = pt_realloc(c->name, (c->count + 32) * sizeof(*names));
		bool *dirs = pt_realloc(c->is_dir, (c->count + 32) * sizeof(*dirs));

		if (names)
			c->name = names;
		if (dirs)
			c->is_dir = dirs;
		if (!names || !dirs)
			return -ENOMEM;
	}
	if (!(c->name[c->count] = pt_strdup(name)))
		return -ENOMEM;
	c->is_dir[c->count++] = is_dir;
	return 0;
}

/* `name` ends in one of the extensions in ".mp3.wav", any case: FAT's names are. */
static bool has_ext(const char *name, const char *exts)
{
	size_t len = strlen(name);

	while (*exts == '.') {
		size_t n = strcspn(exts + 1, ".") + 1;

		if (len > n && !strncasecmp(name + len - n, exts, n))
			return true;
		exts += n;
	}
	return false;
}

/*
 * The names in the directory `word` points into that begin as its last
 * part does: directories always, files if `files`, and of those only the
 * ones ending in `exts` when it is given.
 */
static void add_paths(struct candidates *out, const char *word, bool files, const char *exts)
{
	char dir[PT_PATH_MAX];
	const char *slash = strrchr(word, '/'), *prefix = slash ? slash + 1 : word;
	struct pt_dirent ent;
	pt_dir_t *d;

	out->typed = strlen(prefix);
	if (!slash) {
		strcpy(dir, ".");
	} else if (word[0] == '~' && word[1] == '/') {
		const char *home = pt_getenv("HOME");

		snprintf(dir, sizeof(dir), "%s%.*s", home && *home ? home : "/",
			 (int)(slash - word - 1), word + 1);
	} else {
		snprintf(dir, sizeof(dir), "%.*s", slash == word ? 1 : (int)(slash - word), word);
	}
	if (pt_opendir(dir, &d))
		return;
	while (pt_readdir(d, &ent) == 1) {
		if (ent.name[0] == '.' && prefix[0] != '.')
			continue;
		if (strncmp(ent.name, prefix, out->typed))
			continue;
		if (ent.is_dir || (files && (!exts || has_ext(ent.name, exts))))
			add_candidate(out, ent.name, ent.is_dir);
	}
	pt_closedir(d);
}

/* The files in the directories on $PATH: scripts and compiled pico
 * programs in ~/bin, and whatever gets installed there. */
static void add_path_files(struct candidates *out, const char *word)
{
	const char *path = pt_getenv("PATH");
	size_t len = strlen(word);
	char dir[PT_PATH_MAX];
	struct pt_dirent ent;
	pt_dir_t *d;

	for (const char *p = path ? path : ""; *p;) {
		size_t n = strcspn(p, ":");

		snprintf(dir, sizeof(dir), "%.*s", (int)n, p);
		p += n + (p[n] == ':');
		if (!*dir || pt_opendir(dir, &d))
			continue;
		while (pt_readdir(d, &ent) == 1)
			if (!ent.is_dir && ent.name[0] != '.' && !strncmp(ent.name, word, len))
				add_candidate(out, ent.name, false);
		pt_closedir(d);
	}
}

static void add_commands(struct sh *sh, struct candidates *out, const char *word)
{
	size_t len = strlen(word);

	out->typed = len;
	for (const struct pt_program *p = program_first(); p; p = p->next)
		if (!strncmp(p->name, word, len))
			add_candidate(out, p->name, false);
	for (struct function *f = sh->functions; f; f = f->next)
		if (!strncmp(f->name, word, len))
			add_candidate(out, f->name, false);
	add_path_files(out, word);
}

struct more_ctx {
	struct candidates	*out;
	const char		*word;
};

static void add_more(void *arg, const char *name)
{
	struct more_ctx *m = arg;

	if (!strncmp(name, m->word, strlen(m->word)))
		add_candidate(m->out, name, false);
}

/* The spec's line for the word after `after`, or for any later one; NULL if none. */
static const char *spec_line(const char *spec, const char *after, bool first)
{
	const char *any = NULL;

	for (const char *l = spec; *l; l += strcspn(l, "\n") + (l[strcspn(l, "\n")] == '\n')) {
		size_t k = strcspn(l, ":\n");

		if (l[k] != ':')
			continue;
		if (first ? !k : k == strlen(after) && !strncmp(l, after, k))
			return l + k + 1;
		if (!first && k == 1 && l[0] == '*')
			any = l + k + 1;
	}
	return any;
}

/* Candidates from a line of a spec: its words, and what its <...> stand for. */
static void add_spec(struct sh *sh, const struct pt_completion *c, const char *l,
		     const char *after, const char *word, struct candidates *out)
{
	size_t len = strlen(word);

	out->typed = len;
	while (*l && *l != '\n') {
		size_t n;

		l += strspn(l, " \t");
		n = strcspn(l, " \t\n");
		if (!n)
			break;
		if (n == 6 && !strncmp(l, "<file>", 6)) {
			add_paths(out, word, true, NULL);
		} else if (n > 7 && !strncmp(l, "<file:", 6)) {
			char exts[48];

			snprintf(exts, sizeof(exts), "%.*s", (int)(n - 7), l + 6);
			add_paths(out, word, true, exts);
		} else if (n == 5 && !strncmp(l, "<dir>", 5)) {
			add_paths(out, word, false, NULL);
		} else if (n == 9 && !strncmp(l, "<command>", 9)) {
			add_commands(sh, out, word);
		} else if (n == 6 && !strncmp(l, "<more>", 6)) {
			struct more_ctx m = { out, word };

			if (c->more)
				c->more(after, add_more, &m);
			out->typed = len;
		} else if ((l[0] != '-' || word[0] == '-') && n >= len && !strncmp(l, word, len)) {
			char w[64];

			snprintf(w, sizeof(w), "%.*s", (int)n, l);
			add_candidate(out, w, false);
			out->typed = len;
		}
		l += n;
	}
}

int sh_candidates(struct sh *sh, const char *line, size_t word_start, const char *word,
		  struct candidates *out)
{
	char scratch[PT_PATH_MAX], quote;
	const struct pt_completion *c;
	const char *l;
	struct words w;

	scan(line, word_start, &w, scratch, sizeof(scratch), &quote);
	if (!w.n) {
		if (!strchr(word, '/')) {
			add_commands(sh, out, word);
			return 0;
		}
		add_paths(out, word, true, NULL);	/* ./script */
		return 0;
	}
	c = w.n <= MAX_WORDS ? completion_find(w.text[0]) : NULL;
	if (!c) {
		add_paths(out, word, true, NULL);
		return 0;
	}
	l = spec_line(c->spec, w.text[w.n - 1], w.n == 1);
	out->typed = strlen(word);
	if (l)
		add_spec(sh, c, l, w.n > 1 ? w.text[w.n - 1] : "", word, out);
	return 0;
}

void candidates_free(struct candidates *c)
{
	for (int i = 0; i < c->count; i++)
		pt_free(c->name[i]);
	pt_free(c->name);
	pt_free(c->is_dir);
	memset(c, 0, sizeof(*c));
}

/* The shell's own commands. */
PT_COMPLETE(cd, ": <dir>\n")
PT_COMPLETE_NAMED(type_, "type", ": <command>\n", NULL)
PT_COMPLETE_NAMED(command_, "command", ": -v <command>\n-v: <command>\n", NULL)
PT_COMPLETE_NAMED(set_, "set", ": -e -u -x -o +e +u +x +o\n-o: pipefail errexit nounset xtrace\n"
		  "+o: pipefail errexit nounset xtrace\n", NULL)
