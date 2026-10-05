/*
 * gym - a training log: the day's exercises, the sets as they are done,
 * and what to aim for next.
 *
 * The program is ~/gym/schede.txt, a plain file to write anywhere:
 *
 *	[A] chest, shoulders, triceps
 *	Bench press        4x10   +2.5
 *	Lateral raise      3x12   +1
 *	Push-ups           3x15
 *	Plank              3x45s
 *
 * "[A] ..." starts a scheda, a session's list; each line under it is an
 * exercise: its name, sets x reps, and the smallest step its weight moves
 * in. With no step only the reps count; with "s" the reps are seconds
 * held; "3xmax" is as many as there are. What was done goes in
 * ~/gym/log.txt, a session a paragraph:
 *
 *	2026-10-06 A
 *	Bench press: 40x10 40x10 40x9 40x8
 *	Push-ups: x15 x15 x12
 *	Plank: 45s 40s 45s
 *
 * A set is WEIGHTxREPS, xREPS with no weight, SECONDSs, and a drop set
 * 25x7+20x5. Both files are read whole and the log written back whole,
 * so either can be put right by hand between sessions.
 *
 * What it suggests is double progression, the plain way to build muscle:
 * every set close to failure, a rep or two left, within a range that
 * tops out at the scheda's reps. Once every set reached the top, the
 * weight goes up a step and the reps start again from the bottom of the
 * range; until then the same weight and a rep more; well short of the
 * range, a step down.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dates.h"
#include "util.h"

#define NAME_LEN	48
#define SETS_LEN	256
#define RANGE		2	/* the range starts this many reps under the scheda's */
#define LOG_LINE	512

struct exercise {
	char	 name[NAME_LEN];
	int	 sets, reps;	/* the scheda's 4x10; reps 0: as many as there are */
	float	 step;		/* 0: reps alone, no weight */
	bool	 seconds;	/* held, not counted */
};

struct scheda {
	char	 name[16];
	char	 title[64];
	int	 first, n;	/* its exercises */
};

struct lift {			/* one exercise, one session */
	char	 name[NAME_LEN];
	char	 sets[SETS_LEN];	/* as the file has them: "40x10 40x9" */
};

struct session {
	int	 day;		/* YYYYMMDD */
	char	 scheda[16];
	int	 first, n;	/* its lifts */
};

struct gym {
	struct exercise	*ex;
	struct scheda	*sc;
	struct lift	*lifts;
	struct session	*ses;
	int		 nex, nsc, nlifts, nses;
	int		 capex, capsc, caplifts, capses;
	char		 plan[PT_PATH_MAX], log[PT_PATH_MAX];
};

/* One more element of `*arr`: a pointer to it, zeroed, or NULL. */
static void *grow(void **arr, int *n, int *cap, size_t size)
{
	char *p;

	if (*n == *cap) {
		int c = *cap ? *cap * 2 : 16;
		void *a = pt_realloc(*arr, (size_t)c * size);

		if (!a)
			return NULL;
		*arr = a;
		*cap = c;
	}
	p = (char *)*arr + (size_t)(*n)++ * size;
	memset(p, 0, size);
	return p;
}

#define GROW(g, what)	grow((void **)&(g)->what, &(g)->n##what, &(g)->cap##what, sizeof(*(g)->what))

static void gym_free(struct gym *g)
{
	pt_free(g->ex);
	pt_free(g->sc);
	pt_free(g->lifts);
	pt_free(g->ses);
	memset(g, 0, sizeof(*g));
}

/* lines_next()'s line, which is not a string, as one: cut at `size`. */
static char *line_copy(char *buf, size_t size, const char *line, size_t len)
{
	if (len >= size)
		len = size - 1;
	memcpy(buf, line, len);
	buf[len] = '\0';
	return buf;
}

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		*--e = '\0';
	return s;
}

/* ------------------------------------------------------------ sets */

struct set {
	float	 w;		/* the first part's weight; 0 with none */
	int	 r;		/* its reps or seconds; -1 if the log does not say */
	bool	 weight;
};

static float number(const char *s, const char **end)
{
	float v = 0, scale = 0;

	for (; (*s >= '0' && *s <= '9') || *s == '.' || *s == ','; s++) {
		if (*s == '.' || *s == ',') {
			scale = scale ? scale : 1;
			continue;
		}
		v = v * 10 + (*s - '0');
		if (scale)
			scale *= 10;
	}
	*end = s;
	return scale ? v / scale : v;
}

/* "40x10", "25x7+20x5", "x15", "45s", "40": the first part of a set. */
static bool parse_set(const char *t, struct set *s)
{
	const char *p = t;

	*s = (struct set){ .r = -1 };
	if (!strcmp(t, "done")) {
		s->r = 1;
		return true;
	}
	if (*p >= '0' && *p <= '9') {
		float v = number(p, &p);

		if (*p == 's') {
			s->r = (int)v;
			return true;
		}
		s->w = v;
		s->weight = true;
	}
	if (*p == 'x' || *p == 'X') {
		const char *q;

		s->r = (int)number(p + 1, &q);
		if (q == p + 1)
			return false;
		p = q;
	}
	return !*p || *p == '+';
}

/* The sets of a lift, its first parts: how many, up to `max`. */
static int sets_of(const char *text, struct set *out, int max)
{
	char buf[SETS_LEN], *save = NULL;
	int n = 0;

	strlcpy(buf, text, sizeof(buf));
	for (char *t = strtok_r(buf, " ", &save); t && n < max; t = strtok_r(NULL, " ", &save))
		if (parse_set(t, &out[n]))
			n++;
	return n;
}

static void fmt_weight(char *out, size_t size, float w)
{
	int whole = (int)(w * 100 + 0.5f);

	if (whole % 100 == 0)
		snprintf(out, size, "%d", whole / 100);
	else if (whole % 10 == 0)
		snprintf(out, size, "%d.%d", whole / 100, whole / 10 % 10);
	else
		snprintf(out, size, "%d.%02d", whole / 100, whole % 100);
}

/* ------------------------------------------------------------ the files */

static const char template[] =
	"# Your training program, for gym. A line in brackets starts a\n"
	"# session's list (a scheda), then one exercise a line: its name,\n"
	"# SETSxREPS, and +STEP, the smallest change its weight moves in.\n"
	"# No step: the reps alone count. 3x45s: seconds held. 3xmax: as\n"
	"# many as you can. Edit it to be yours.\n"
	"\n"
	"[A] chest, shoulders, triceps\n"
	"Bench press          4x10  +2.5\n"
	"Shoulder press       4x10  +5\n"
	"Lateral raise        3x12  +1\n"
	"Triceps pushdown     4x12  +2.5\n"
	"Plank                3x45s\n"
	"\n"
	"[B] back, biceps, legs\n"
	"Lat pulldown         4x10  +5\n"
	"Seated row           4x12  +5\n"
	"Biceps curl          4x12  +1\n"
	"Leg press            4x10  +10\n"
	"Crunch               3x15\n";

/* "4x10", "3x45s", "3xmax": a scheda's sets and reps. */
static bool target(const char *t, struct exercise *e)
{
	const char *p;
	int sets = (int)number(t, &p);

	if (p == t || (*p != 'x' && *p != 'X'))
		return false;
	t = p + 1;
	if (!strcasecmp(t, "max")) {
		e->sets = sets;
		e->reps = 0;
		return true;
	}
	e->reps = (int)number(t, &p);
	if (p == t || (*p && !(*p == 's' && !p[1])))
		return false;
	e->sets = sets;
	e->seconds = *p == 's';
	return true;
}

static int read_plan(struct gym *g)
{
	struct lines in;
	struct scheda *sc = NULL;
	char *line;
	size_t len;
	int fd = pt_open(g->plan, O_RDONLY);

	if (fd == -ENOENT) {
		/* a first start: something to begin from, to be made one's own */
		if ((fd = pt_open(g->plan, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
			return fd;
		write_all(fd, template, sizeof(template) - 1);
		pt_close(fd);
		fd = pt_open(g->plan, O_RDONLY);
	}
	if (fd < 0)
		return fd;
	lines_init(&in, fd);
	while ((line = lines_next(&in, &len))) {
		char buf[LOG_LINE], *s = trim(line_copy(buf, sizeof(buf), line, len)), *words[16];
		char *save = NULL, *close;
		struct exercise *e;
		int n = 0, name_end;

		if (!*s || *s == '#')
			continue;
		if (*s == '[' && (close = strchr(s, ']'))) {
			*close = '\0';
			if (!(sc = GROW(g, sc)))
				break;
			strlcpy(sc->name, trim(s + 1), sizeof(sc->name));
			strlcpy(sc->title, trim(close + 1), sizeof(sc->title));
			sc->first = g->nex;
			continue;
		}
		if (!sc || !(e = GROW(g, ex)))
			continue;
		for (char *w = strtok_r(s, " \t", &save); w && n < 16; w = strtok_r(NULL, " \t", &save))
			words[n++] = w;
		/* the name is what comes before sets x reps or a step */
		name_end = n;
		for (int i = n - 1; i > 0; i--) {
			if (words[i][0] == '+' && words[i][1] >= '0' && words[i][1] <= '9') {
				const char *end;

				e->step = number(words[i] + 1, &end);
				name_end = i;
			} else if (target(words[i], e)) {
				name_end = i;
			}
		}
		for (int i = 0; i < name_end; i++) {
			if (i)
				strlcat(e->name, " ", sizeof(e->name));
			strlcat(e->name, words[i], sizeof(e->name));
		}
		if (!e->sets)
			e->sets = 1;		/* a thing to do once: mobility, a warm-up */
		sc->n++;
	}
	lines_free(&in);
	pt_close(fd);
	return 0;
}

static int read_log(struct gym *g)
{
	struct lines in;
	struct session *ses = NULL;
	char *line;
	size_t len;
	int fd = pt_open(g->log, O_RDONLY);

	if (fd == -ENOENT)
		return 0;
	if (fd < 0)
		return fd;
	lines_init(&in, fd);
	while ((line = lines_next(&in, &len))) {
		char buf[LOG_LINE], *s = trim(line_copy(buf, sizeof(buf), line, len)), *colon;
		int y, m, d, used;
		struct lift *l;

		if (!*s || *s == '#')
			continue;
		if (sscanf(s, "%4d-%2d-%2d%n", &y, &m, &d, &used) == 3 && day_make(y, m, d)) {
			if (!(ses = GROW(g, ses)))
				break;
			ses->day = day_make(y, m, d);
			strlcpy(ses->scheda, trim(s + used), sizeof(ses->scheda));
			ses->first = g->nlifts;
			continue;
		}
		if (!ses || !(colon = strchr(s, ':')) || !(l = GROW(g, lifts)))
			continue;
		*colon = '\0';
		strlcpy(l->name, trim(s), sizeof(l->name));
		strlcpy(l->sets, trim(colon + 1), sizeof(l->sets));
		ses->n++;
	}
	lines_free(&in);
	pt_close(fd);
	return 0;
}

/* The whole log again, in order, beside the old and then over it. */
static int write_log(const struct gym *g)
{
	char tmp[PT_PATH_MAX + 8], line[LOG_LINE];
	int fd, err = 0;

	snprintf(tmp, sizeof(tmp), "%s.new", g->log);
	if ((fd = pt_open(tmp, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
		return fd;
	for (int i = 0; i < g->nses && !err; i++) {
		const struct session *s = &g->ses[i];
		int lifts = 0;

		for (int k = 0; k < s->n; k++)
			lifts += g->lifts[s->first + k].sets[0] != '\0';
		if (!lifts)
			continue;		/* started, nothing done: not kept */
		snprintf(line, sizeof(line), "%s%04d-%02d-%02d %s\n", i ? "\n" : "", s->day / 10000,
			 s->day / 100 % 100, s->day % 100, s->scheda);
		err = write_all(fd, line, strlen(line));
		for (int k = 0; k < s->n && !err; k++) {
			const struct lift *l = &g->lifts[s->first + k];

			if (!l->sets[0])
				continue;
			snprintf(line, sizeof(line), "%s: %s\n", l->name, l->sets);
			err = write_all(fd, line, strlen(line));
		}
	}
	if (pt_close(fd) && !err)
		err = -EIO;
	if (!err)
		err = pt_rename(tmp, g->log);
	if (err)
		pt_unlink(tmp);
	return err;
}

/* ------------------------------------------------------------ what to aim for */

static const struct lift *find_lift(const struct gym *g, const struct session *s, const char *name)
{
	for (int k = 0; k < s->n; k++)
		if (!strcasecmp(g->lifts[s->first + k].name, name))
			return &g->lifts[s->first + k];
	return NULL;
}

/* The last session before `day` that did this exercise, and its lift. */
static const struct lift *last_time(const struct gym *g, const char *name, int day, int *when)
{
	for (int i = g->nses - 1; i >= 0; i--) {
		const struct lift *l;

		if (g->ses[i].day >= day)
			continue;
		if ((l = find_lift(g, &g->ses[i], name)) && l->sets[0]) {
			*when = g->ses[i].day;
			return l;
		}
	}
	return NULL;
}

struct aim {
	char	 text[80];
	float	 w;		/* what to put on: the set entry starts here */
	int	 r;
};

/*
 * Double progression from last time's sets. The working weight is the one
 * most of them used (the heavier, if two were used as often); "reached" is
 * every set at it making the scheda's reps, as many sets as it asks for.
 */
static void aim(const struct exercise *e, const struct lift *last, struct aim *a)
{
	struct set sets[32];
	int n = last ? sets_of(last->sets, sets, 32) : 0, top = e->reps, low = e->reps - RANGE;
	char w[16], w2[16];

	if (low < 1)
		low = 1;
	a->w = 0;
	a->r = top ? top : 10;
	if (!n) {
		if (e->step)
			snprintf(a->text, sizeof(a->text), "first time: a weight you lift %d times, 1-2 left",
				 top ? top : 10);
		else
			snprintf(a->text, sizeof(a->text), "first time: as many as clean form allows");
		return;
	}
	if (!e->step || e->seconds || !top) {
		int best = 0;

		for (int i = 0; i < n; i++)
			best = sets[i].r > best ? sets[i].r : best;
		a->r = e->seconds ? best + 5 : best + 1;
		if (e->seconds)
			snprintf(a->text, sizeof(a->text), "aim %d s, 5 more than last time", a->r);
		else
			snprintf(a->text, sizeof(a->text), "aim %d: one more than last time's best", a->r);
		return;
	}
	{
		float wt = 0;
		int most = 0, at = 0, reached = 0, short_of = 0, best = 0;

		for (int i = 0; i < n; i++) {
			int c = 0;

			for (int k = 0; k < n; k++)
				c += sets[k].w == sets[i].w;
			if (c > most || (c == most && sets[i].w > wt)) {
				most = c;
				wt = sets[i].w;
			}
		}
		for (int i = 0; i < n; i++) {
			int r = sets[i].r < 0 ? top : sets[i].r;	/* not written down: made */

			if (sets[i].w != wt)
				continue;
			at++;
			reached += r >= top;
			short_of += r < low;
			best = r > best ? r : best;
		}
		fmt_weight(w, sizeof(w), wt);
		if (at >= e->sets && reached == at) {
			a->w = wt + e->step;
			a->r = low;
			fmt_weight(w2, sizeof(w2), a->w);
			snprintf(a->text, sizeof(a->text), "up: %s kg, %d-%d reps (all %d made at %s)", w2, low,
				 top, top, w);
		} else if (short_of * 2 > at) {
			a->w = wt > e->step ? wt - e->step : wt;
			a->r = top;
			fmt_weight(w2, sizeof(w2), a->w);
			snprintf(a->text, sizeof(a->text), "down: %s kg, build back to %d reps", w2, top);
		} else {
			a->w = wt;
			a->r = best < top ? best + 1 : top;
			snprintf(a->text, sizeof(a->text), "%s kg again: a rep more, up to %d each set", w, top);
		}
	}
}

/* ------------------------------------------------------------ the screen */

enum view { PICK, SESSION, HISTORY, DAY };

struct ui {
	struct gym	*g;
	enum view	 view;
	int		 cols, rows, body;
	int		 sel, top;
	int		 today;
	int		 cur;		/* today's session, or -1 */
	int		 sc;		/* its scheda */
	int		 day;		/* DAY: the session shown */
	char		 note[80];
};

static struct scheda *scheda_named(struct gym *g, const char *name)
{
	for (int i = 0; i < g->nsc; i++)
		if (!strcasecmp(g->sc[i].name, name))
			return &g->sc[i];
	return NULL;
}

/* The rows of a session: the scheda's exercises, then any others done. */
static int session_rows(const struct ui *u)
{
	const struct gym *g = u->g;
	int n = u->sc >= 0 ? g->sc[u->sc].n : 0;

	if (u->cur >= 0)
		for (int k = 0; k < g->ses[u->cur].n; k++) {
			const struct lift *l = &g->lifts[g->ses[u->cur].first + k];
			bool listed = false;

			for (int i = 0; u->sc >= 0 && i < g->sc[u->sc].n; i++)
				listed |= !strcasecmp(g->ex[g->sc[u->sc].first + i].name, l->name);
			n += !listed;
		}
	return n;
}

/* Row i's exercise: the scheda's, or one done that is not on it (made up from its name). */
static struct exercise row_exercise(const struct ui *u, int i, const struct lift **done)
{
	const struct gym *g = u->g;
	int listed = u->sc >= 0 ? g->sc[u->sc].n : 0;
	struct exercise e = { .sets = 3, .reps = 10, .step = 2.5f };

	*done = NULL;
	if (i < listed) {
		e = g->ex[g->sc[u->sc].first + i];
		if (u->cur >= 0)
			*done = find_lift(g, &g->ses[u->cur], e.name);
		return e;
	}
	for (int k = 0, extra = listed; u->cur >= 0 && k < g->ses[u->cur].n; k++) {
		const struct lift *l = &g->lifts[g->ses[u->cur].first + k];
		bool on = false;

		for (int j = 0; j < listed; j++)
			on |= !strcasecmp(g->ex[g->sc[u->sc].first + j].name, l->name);
		if (!on && extra++ == i) {
			strlcpy(e.name, l->name, sizeof(e.name));
			*done = l;
		}
	}
	return e;
}

static int count_sets(const char *text)
{
	struct set s[32];

	return sets_of(text, s, 32);
}

/* `s` cut or padded with spaces to exactly `cols` columns of screen. */
static void fit(char *out, size_t size, const char *s, int cols)
{
	size_t n = utf8_prefix(s, strlen(s), cols);
	int pad = cols - utf8_width(s, n);

	snprintf(out, size, "%.*s%*s", (int)n, s, pad > 0 ? pad : 0, "");
}

static void put_row(int row, const char *look, const char *text, int cols)
{
	size_t n = utf8_prefix(text, strlen(text), cols);
	int pad = cols - utf8_width(text, n);

	pt_printf("\x1b[%d;1H%s%.*s%*s\x1b[0m", row, look, (int)n, text, pad > 0 ? pad : 0, "");
}

static void key_line(struct ui *u, const char *keys)
{
	put_row(u->rows, "\x1b[0;7m", u->note[0] ? u->note : keys, u->cols);
	u->note[0] = '\0';
}

static int list_len(const struct ui *u)
{
	switch (u->view) {
	case PICK:	return u->g->nsc;
	case SESSION:	return session_rows(u);
	case HISTORY:	return u->g->nses;
	default:	return u->day >= 0 ? u->g->ses[u->day].n : 0;
	}
}

static void draw_pick(struct ui *u)
{
	struct gym *g = u->g;
	char line[160], when[32];

	put_row(1, "\x1b[0;1m", " Gym: today's scheda", u->cols);
	for (int r = 0; r < u->body; r++) {
		int i = u->top + r, last = 0;
		const struct scheda *s = i < g->nsc ? &g->sc[i] : NULL;

		if (!s) {
			put_row(r + 2, "\x1b[0m", g->nsc || r != 1 ? "" :
				"  no scheda: write them in ~/gym/schede.txt", u->cols);
			continue;
		}
		for (int k = g->nses - 1; k >= 0 && !last; k--)
			if (!strcasecmp(g->ses[k].scheda, s->name))
				last = g->ses[k].day;
		if (last)
			day_name(last, u->today, when, sizeof(when));
		snprintf(line, sizeof(line), " [%s] %s%s%s", s->name, s->title, last ? "  last: " : "",
			 last ? when : "");
		put_row(r + 2, i == u->sel ? "\x1b[0;7m" : "\x1b[0m", line, u->cols);
	}
	key_line(u, " enter start  h history  q quit");
}

static void draw_session(struct ui *u)
{
	struct gym *g = u->g;
	const struct scheda *s = u->sc >= 0 ? &g->sc[u->sc] : NULL;
	char line[SETS_LEN + NAME_LEN * 2 + 32], when[32], tgt[16], name[NAME_LEN * 2], title[160];
	int n = session_rows(u), panel = 2;
	const struct lift *done, *last;
	struct exercise e;
	struct aim a;
	int lday = 0;

	day_name(u->today, u->today, when, sizeof(when));
	snprintf(line, sizeof(line), " [%s] %s", s ? s->name : "?", s ? s->title : "");
	fit(title, sizeof(title), line, u->cols - (int)strlen(when) - 2);
	pt_printf("\x1b[1;1H\x1b[0;1m%s \x1b[0;2m%s\x1b[0m\x1b[K", title, when);
	for (int r = 0; r < u->body - panel; r++) {
		int i = u->top + r;
		const char *look = i == u->sel ? "\x1b[0;7m" : "\x1b[0m";

		if (i >= n) {
			put_row(r + 2, "\x1b[0m", "", u->cols);
			continue;
		}
		e = row_exercise(u, i, &done);
		if (e.reps)
			snprintf(tgt, sizeof(tgt), "%dx%d%s", e.sets, e.reps, e.seconds ? "s" : "");
		else
			snprintf(tgt, sizeof(tgt), "%dxmax", e.sets);
		fit(name, sizeof(name), e.name, 20);
		snprintf(line, sizeof(line), " %s %-6s %s", name, tgt, done ? done->sets : "");
		put_row(r + 2, done && count_sets(done->sets) >= e.sets && i != u->sel ? "\x1b[0;2m" : look,
			line, u->cols);
	}
	/* the one picked: last time, and what to aim for */
	if (n) {
		e = row_exercise(u, u->sel, &done);
		last = last_time(g, e.name, u->today, &lday);
		aim(&e, last, &a);
		if (last) {
			day_name(lday, u->today, when, sizeof(when));
			snprintf(line, sizeof(line), " last, %s: %s", when, last->sets);
		} else {
			snprintf(line, sizeof(line), " %s: not done before", e.name);
		}
		put_row(u->rows - 2, "\x1b[0;2m", line, u->cols);
		snprintf(line, sizeof(line), " %s", a.text);
		put_row(u->rows - 1, "\x1b[0;1m", line, u->cols);
	}
	key_line(u, " enter a set  d undo  a add  h history  c scheda  q");
}

static void draw_history(struct ui *u)
{
	struct gym *g = u->g;
	char line[160], when[32];

	put_row(1, "\x1b[0;1m", " Gym: every session, the last first", u->cols);
	for (int r = 0; r < u->body; r++) {
		int i = u->top + r, k = g->nses - 1 - i, sets = 0;
		const struct session *s = k >= 0 && i < g->nses ? &g->ses[k] : NULL;

		if (!s) {
			put_row(r + 2, "\x1b[0m", "", u->cols);
			continue;
		}
		for (int j = 0; j < s->n; j++)
			sets += count_sets(g->lifts[s->first + j].sets);
		day_name(s->day, u->today, when, sizeof(when));
		snprintf(line, sizeof(line), " %-16s [%s]  %d exercises, %d sets", when, s->scheda, s->n, sets);
		put_row(r + 2, i == u->sel ? "\x1b[0;7m" : "\x1b[0m", line, u->cols);
	}
	key_line(u, " enter open  esc back  q quit");
}

static void draw_day(struct ui *u)
{
	struct gym *g = u->g;
	const struct session *s = &g->ses[u->day];
	char line[SETS_LEN + NAME_LEN * 2 + 32], when[32];

	day_name(s->day, u->today, when, sizeof(when));
	snprintf(line, sizeof(line), " %s, [%s]", when, s->scheda);
	put_row(1, "\x1b[0;1m", line, u->cols);
	for (int r = 0; r < u->body; r++) {
		int i = u->top + r;

		if (i >= s->n) {
			put_row(r + 2, "\x1b[0m", "", u->cols);
			continue;
		}
		char name[NAME_LEN * 2];

		fit(name, sizeof(name), g->lifts[s->first + i].name, 20);
		snprintf(line, sizeof(line), " %s %s", name, g->lifts[s->first + i].sets);
		put_row(r + 2, "\x1b[0m", line, u->cols);
	}
	key_line(u, " esc back  q quit");
}

static void draw(struct ui *u)
{
	switch (u->view) {
	case PICK:	draw_pick(u); break;
	case SESSION:	draw_session(u); break;
	case HISTORY:	draw_history(u); break;
	case DAY:	draw_day(u); break;
	}
}

static void fix_view(struct ui *u)
{
	int n = list_len(u), body = u->view == SESSION ? u->body - 2 : u->body;

	if (u->sel >= n)
		u->sel = n - 1;
	if (u->sel < 0)
		u->sel = 0;
	if (u->sel < u->top)
		u->top = u->sel;
	if (u->sel >= u->top + body)
		u->top = u->sel - body + 1;
}

/* ------------------------------------------------------------ doing things */

/* Today's session for scheda `sc`, made if there is none yet. */
static int start_today(struct ui *u, int sc)
{
	struct gym *g = u->g;
	struct session *s;

	if (u->cur < 0) {
		if (!(s = GROW(g, ses)))
			return -ENOMEM;
		s->day = u->today;
		s->first = g->nlifts;
		u->cur = g->nses - 1;
	}
	strlcpy(g->ses[u->cur].scheda, g->sc[sc].name, sizeof(g->ses[u->cur].scheda));
	u->sc = sc;
	u->view = SESSION;
	u->sel = u->top = 0;
	return 0;
}

/* The lift for `name` in today's session, made at its end if need be. */
static struct lift *today_lift(struct ui *u, const char *name)
{
	struct gym *g = u->g;
	struct session *s = &g->ses[u->cur];
	struct lift *l;

	for (int k = 0; k < s->n; k++)
		if (!strcasecmp(g->lifts[s->first + k].name, name))
			return &g->lifts[s->first + k];
	/* today's session is the last, so its lifts are the last too */
	if (s->first + s->n != g->nlifts || !(l = GROW(g, lifts)))
		return NULL;
	strlcpy(l->name, name, sizeof(l->name));
	g->ses[u->cur].n++;
	return l;
}

/*
 * A set, entered on the panel: kg and reps, filled in from the aim. Digits
 * replace the number being edited, the arrows move the weight a step and
 * the reps by one, Tab or space goes from one to the other.
 */
static bool enter_set(struct ui *u, const struct exercise *e, struct aim *a, char *out, size_t size)
{
	bool weight = e->step > 0 && !e->seconds, on_w = weight, fresh = true;
	float w = a->w;
	int r = a->r;
	char typed[12] = "", ws[16];

	for (;;) {
		int k;

		fmt_weight(ws, sizeof(ws), w);
		pt_printf("\x1b[%d;1H\x1b[0m set:  ", u->rows - 1);
		if (weight)
			pt_printf("kg %s %s \x1b[0m  ", on_w ? "\x1b[7m" : "\x1b[1m", typed[0] && on_w ? typed : ws);
		pt_printf("%s %s %d \x1b[0m  \x1b[2menter saves\x1b[0m\x1b[K", e->seconds ? "s" : "reps",
			  !on_w ? "\x1b[7m" : "\x1b[1m", r);
		k = pt_readkey(PT_STDIN);
		if (k == '\r' || k == '\n')
			break;
		if (k == PT_KEY_ESC || k == PT_CTRL('c') || k < 0)
			return false;
		if (k == '\t' || k == ' ') {
			on_w = weight && !on_w;
			fresh = true;
			typed[0] = '\0';
		} else if (k == PT_KEY_LEFT && weight) {
			w = w > e->step ? w - e->step : 0;
		} else if (k == PT_KEY_RIGHT && weight) {
			w += e->step;
		} else if (k == PT_KEY_UP) {
			r++;
		} else if (k == PT_KEY_DOWN && r > 0) {
			r--;
		} else if ((k >= '0' && k <= '9') || ((k == '.' || k == ',') && on_w)) {
			size_t n = fresh ? 0 : strlen(typed);
			const char *end;

			if (n + 1 < sizeof(typed)) {
				typed[n] = (char)k;
				typed[n + 1] = '\0';
			}
			fresh = false;
			if (on_w)
				w = number(typed, &end);
			else
				r = (int)number(typed, &end);
		} else if (k == 0x7f || k == PT_CTRL('h')) {
			size_t n = strlen(typed);
			const char *end;

			if (n)
				typed[--n] = '\0';
			if (on_w)
				w = n ? number(typed, &end) : 0;
			else
				r = n ? (int)number(typed, &end) : 0;
			fresh = false;
		}
	}
	fmt_weight(ws, sizeof(ws), w);
	if (e->seconds)
		snprintf(out, size, "%ds", r);
	else if (weight)
		snprintf(out, size, "%sx%d", ws, r);
	else
		snprintf(out, size, "x%d", r);
	return r > 0;
}

static int log_set(struct ui *u)
{
	struct gym *g = u->g;
	const struct lift *done, *last;
	struct exercise e;
	struct lift *l;
	struct aim a;
	char set[32];
	int lday;

	if (!session_rows(u))
		return 0;
	e = row_exercise(u, u->sel, &done);
	last = last_time(g, e.name, u->today, &lday);
	aim(&e, last, &a);
	/* a set already done today goes on from it, not from the aim */
	if (done && done->sets[0]) {
		struct set s[32];
		int n = sets_of(done->sets, s, 32);

		if (n && s[n - 1].weight)
			a.w = s[n - 1].w;
		if (n && s[n - 1].r > 0)
			a.r = s[n - 1].r;
	}
	if (!enter_set(u, &e, &a, set, sizeof(set)))
		return 0;
	if (!(l = today_lift(u, e.name)))
		return -ENOMEM;
	if (strlen(l->sets) + strlen(set) + 2 >= sizeof(l->sets)) {
		snprintf(u->note, sizeof(u->note), " no room for more sets of %.30s", e.name);
		return 0;
	}
	if (l->sets[0])
		strlcat(l->sets, " ", sizeof(l->sets));
	strlcat(l->sets, set, sizeof(l->sets));
	snprintf(u->note, sizeof(u->note), " %.30s: set %d, %.24s", e.name, count_sets(l->sets), set);
	return write_log(g);
}

static int undo_set(struct ui *u)
{
	const struct lift *done;
	struct exercise e;
	char *sp;

	if (!session_rows(u))
		return 0;
	e = row_exercise(u, u->sel, &done);
	if (!done || !done->sets[0])
		return 0;
	sp = strrchr(((struct lift *)done)->sets, ' ');
	snprintf(u->note, sizeof(u->note), " %.30s: %.30s taken off", e.name, sp ? sp + 1 : done->sets);
	if (sp)
		*sp = '\0';
	else
		((struct lift *)done)->sets[0] = '\0';
	return write_log(u->g);
}

/* The scheda after the one done last: where a rotation goes on. */
static int next_scheda(const struct gym *g)
{
	if (!g->nses || !g->nsc)
		return 0;
	for (int i = 0; i < g->nsc; i++)
		if (!strcasecmp(g->sc[i].name, g->ses[g->nses - 1].scheda))
			return (i + 1) % g->nsc;
	return 0;
}

static bool key(struct ui *u, int k, int *err)
{
	struct gym *g = u->g;

	if (k == 'q' || k == PT_CTRL('c') || k == PT_KEY_EOF || k == PT_KEY_ERROR)
		return false;
	switch (k) {
	case PT_KEY_UP: case 'k':	u->sel--; return true;
	case PT_KEY_DOWN: case 'j':	u->sel++; return true;
	case PT_KEY_PGUP:		u->sel -= u->body; return true;
	case PT_KEY_PGDN:		u->sel += u->body; return true;
	case PT_KEY_HOME: case 'g':	u->sel = 0; return true;
	case PT_KEY_END: case 'G':	u->sel = list_len(u) - 1; return true;
	case 'h':
		if (u->view != HISTORY) {
			u->view = HISTORY;
			u->sel = u->top = 0;
		}
		return true;
	}
	switch (u->view) {
	case PICK:
		if ((k == '\r' || k == '\n') && g->nsc)
			*err = start_today(u, u->sel);
		break;
	case SESSION:
		if (k == '\r' || k == '\n' || k == ' ') {
			*err = log_set(u);
		} else if (k == 'd' || k == PT_KEY_DELETE) {
			*err = undo_set(u);
		} else if (k == 'a') {
			char name[NAME_LEN] = "";

			if (ask_line(u->rows, " exercise not on the scheda: ", name, sizeof(name)) &&
			    !today_lift(u, trim(name)))
				*err = -ENOMEM;
			u->sel = session_rows(u) - 1;
		} else if (k == 'c' || k == PT_KEY_ESC) {
			const struct session *s = &g->ses[u->cur];
			bool any = false;

			for (int j = 0; j < s->n; j++)
				any |= g->lifts[s->first + j].sets[0] != '\0';
			if (any && k == 'c') {
				snprintf(u->note, sizeof(u->note), " today's sets are under [%s] already", s->scheda);
			} else if (k == 'c') {
				u->view = PICK;
				u->sel = u->sc;
			} else {
				return false;
			}
		}
		break;
	case HISTORY:
		if ((k == '\r' || k == '\n') && g->nses) {
			u->day = g->nses - 1 - u->sel;
			u->view = DAY;
			u->sel = u->top = 0;
		} else if (k == PT_KEY_ESC) {
			u->view = u->cur >= 0 ? SESSION : PICK;
			u->sel = u->top = 0;
		}
		break;
	case DAY:
		if (k == PT_KEY_ESC) {
			u->view = HISTORY;
			u->sel = g->nses - 1 - u->day;
		}
		break;
	}
	return true;
}

static int run(struct ui *u)
{
	int err = 0;

	pt_tty_raw(PT_STDIN, true);
	pt_puts("\x1b[?25l\x1b[2J");
	while (!err) {
		fix_view(u);
		draw(u);
		if (!key(u, pt_readkey(PT_STDIN), &err))
			break;
	}
	pt_puts("\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
	pt_tty_raw(PT_STDIN, false);
	return err;
}

/* `gym last`: the sessions printed, the newest last, as the log has them. */
static void print_log(const struct gym *g, int count)
{
	for (int i = count < g->nses ? g->nses - count : 0; i < g->nses; i++) {
		const struct session *s = &g->ses[i];

		pt_printf("%04d-%02d-%02d %s\n", s->day / 10000, s->day / 100 % 100, s->day % 100, s->scheda);
		for (int k = 0; k < s->n; k++)
			pt_printf("  %s: %s\n", g->lifts[s->first + k].name, g->lifts[s->first + k].sets);
	}
}

/* `gym plan`: a scheda's exercises, each with last time and the aim. */
static int print_plan(struct gym *g, const char *name, int today)
{
	struct scheda *sc = name ? scheda_named(g, name) : g->nsc ? &g->sc[next_scheda(g)] : NULL;
	char when[32], tgt[16];

	if (!sc) {
		pt_dprintf(PT_STDERR, "gym: %s: no such scheda in ~/gym/schede.txt\n", name ? name : "any");
		return 1;
	}
	pt_printf("[%s] %s\n", sc->name, sc->title);
	for (int i = 0; i < sc->n; i++) {
		const struct exercise *e = &g->ex[sc->first + i];
		const struct lift *last;
		struct aim a;
		int day = 0;

		last = last_time(g, e->name, today + 1, &day);
		aim(e, last, &a);
		if (e->reps)
			snprintf(tgt, sizeof(tgt), "%dx%d%s", e->sets, e->reps, e->seconds ? "s" : "");
		else
			snprintf(tgt, sizeof(tgt), "%dxmax", e->sets);
		pt_printf("%s  %s\n", e->name, tgt);
		if (last) {
			day_name(day, today, when, sizeof(when));
			pt_printf("  last, %s: %s\n", when, last->sets);
		}
		pt_printf("  %s\n", a.text);
	}
	return 0;
}

PT_COMPLETE(gym, ": last plan\n")

PT_PROGRAM(gym, "a training log: the sets, and what to aim for\n"
	   "usage: gym          today's session, to log it\n"
	   "       gym last [N] the last N sessions (3)\n"
	   "       gym plan [S] scheda S (the next one), each\n"
	   "                    exercise's last sets and aim\n"
	   "The program is ~/gym/schede.txt, a scheda a block:\n"
	   "  [A] chest, shoulders\n"
	   "  Bench press   4x10  +2.5\n"
	   "and what was done goes in ~/gym/log.txt. For each\n"
	   "exercise it shows last time's sets and the aim:\n"
	   "every set at the top of the reps, a step up; short\n"
	   "of the range, a step down; else a rep more.")
{
	struct gym g = { 0 };
	struct ui u = { .g = &g, .cur = -1, .sc = -1, .day = -1, .today = day_today() };
	int err;

	if ((err = pt_home_file("gym", "schede.txt", NULL, g.plan, sizeof(g.plan))) ||
	    (err = pt_home_file("gym", "log.txt", NULL, g.log, sizeof(g.log))))
		return fail("gym", "~/gym", err);
	if ((err = read_plan(&g)) || (err = read_log(&g))) {
		gym_free(&g);
		return fail("gym", "~/gym", err);
	}
	if (argc > 1 && !strcmp(argv[1], "last")) {
		print_log(&g, argc > 2 ? atoi(argv[2]) : 3);
		gym_free(&g);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "plan")) {
		err = print_plan(&g, argc > 2 ? argv[2] : NULL, u.today);
		gym_free(&g);
		return err;
	}
	if (argc > 1) {
		pt_dprintf(PT_STDERR, "usage: gym [last [N] | plan [SCHEDA]]\n");
		gym_free(&g);
		return 2;
	}
	if (!pt_isatty(PT_STDIN) || !pt_isatty(PT_STDOUT)) {
		print_log(&g, 3);
		gym_free(&g);
		return 0;
	}
	pt_tty_size(PT_STDOUT, &u.cols, &u.rows);
	u.body = u.rows - 2;
	/* a session already begun today goes on; otherwise the next scheda is offered */
	if (g.nses && g.ses[g.nses - 1].day == u.today) {
		struct scheda *s = scheda_named(&g, g.ses[g.nses - 1].scheda);

		u.cur = g.nses - 1;
		u.sc = s ? (int)(s - g.sc) : -1;
		u.view = SESSION;
	} else {
		u.view = PICK;
		u.sel = next_scheda(&g);
	}
	err = run(&u);
	gym_free(&g);
	return err ? fail("gym", "~/gym/log.txt", err) : 0;
}
