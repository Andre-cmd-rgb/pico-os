/* The screen driven by keys and streamed text, without a board or API key. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../bin/ai_ui.c"

static int keys[2048], nkeys, at;
static struct jbuf screen;
static bool raw, app_scroll;
static int writes;
static int64_t clock_step = 100000;

void *pt_malloc(size_t n) { return malloc(n); }
void *pt_calloc(size_t n, size_t s) { return calloc(n, s); }
void *pt_realloc(void *p, size_t n) { return realloc(p, n); }
void pt_free(void *p) { free(p); }
char *pt_strdup(const char *s) { return strdup(s); }
int64_t pt_uptime_us(void) { static int64_t t; return t += clock_step; }
int pt_tty_raw(int fd, bool on) { (void)fd; raw = on; return 0; }
int pt_ioctl(int fd, int req, void *arg)
{
	(void)fd;
	assert(req == PT_TTY_SETSCROLL);
	app_scroll = *(int *)arg;
	return 0;
}
int pt_readkey_timeout(int fd, int ms)
{
	(void)fd;
	assert(at < nkeys || ms == 0);
	return at < nkeys ? keys[at++] : PT_KEY_NONE;
}
ssize_t pt_write(int fd, const void *buf, size_t n)
{
	(void)fd;
	writes++;
	jb_add(&screen, buf, n);
	return n;
}
int pt_puts(const char *s) { return pt_write(1, s, strlen(s)); }

static void key(int k) { assert(nkeys < 2048); keys[nkeys++] = k; }
static void type(const char *s) { while (*s) key((unsigned char)*s++); }

static void expect(struct ai_ui *ui, const char *text)
{
	char out[AI_INPUT_MAX];

	key('\r');
	assert(ai_ui_readline(ui, out, sizeof(out)) == (int)strlen(text));
	assert(!strcmp(out, text));
	assert(strstr(screen.p, "\x1b[23;1H"));
	/* Drawing the last column must never scroll the terminal itself. */
	assert(!strchr(screen.p, '\n'));
	nkeys = at = 0;
	jb_free(&screen);
}

int main(void)
{
	struct ai_ui *ui = ai_ui_open(53, 23);
	char before[4], longline[AI_INPUT_MAX];
	int old_back;

	assert(ui && raw && app_scroll);
	ai_ui_mode(ui, "study");
	ai_ui_context(ui, "~/notes/history", "gemma-4-31b-it", "auto");
	ai_ui_usage(ui, .0123, 1234, false);
	ai_ui_draw(ui, true);
	assert(strstr(screen.p, " AI "));
	assert(strstr(screen.p, "AI study"));
	assert(strstr(screen.p, "1.2k tok  $0.0123"));
	assert(!strstr(screen.p, "~/notes/history"));
	assert(!strstr(screen.p, "gemma-4-31b"));
	assert(ui->body == 20);
	assert(!strstr(screen.p, "\x1b[44;37"));
	{
		const char *header = strstr(screen.p, "\x1b[7m") + 4;
		const char *end = strstr(header, "\x1b[0m");

		assert(end - header == 53);
		assert(!memchr(header, '\x1b', end - header));
		assert(strstr(screen.p, "\x1b[22;1H\x1b[0;2m-----------------------------------------------------\x1b[0m"));
		assert(!strstr(screen.p, "Esc exit"));
	}
	ai_ui_draw(NULL, true);
	jb_free(&screen);
	{
		char out[AI_INPUT_MAX];

		/* A clean blocking key read needs no repaint. Each changed draft
		 * is painted once before the next key, even without a delay. */
		writes = 0;
		clock_step = 0;
		key(PT_KEY_NONE);
		assert(ai_ui_key(ui, -1) == PT_KEY_NONE && !writes);
		nkeys = at = 0;
		type("abc"); key('\r');
		assert(ai_ui_readline(ui, out, sizeof(out)) == 3 && !strcmp(out, "abc"));
		assert(writes == 4); /* initial prompt and three draft changes */
		clock_step = 100000;
		nkeys = at = 0;
		jb_free(&screen);
	}
	ai_ui_context(ui, "~/notes/a-very-long-subject-folder", "nemotron-3-super-120b-a12b:free", "minimal");
	ai_ui_draw(ui, true);
	assert(!strstr(screen.p, "minimal"));
	ai_ui_usage(ui, .0123, 1234, true);
	ai_ui_draw(ui, true);
	assert(strstr(screen.p, "1.2k+ tok  $0.0123+"));
	jb_free(&screen);
	ai_ui_context(ui, "~/notes/history", "gemma-4-31b-it", "auto");
	{
		const char *items[] = { "free google/gemma-4", "$0.3/1 deepseek/deepseek-v4.1-flash" };

		type("deepseek4.1-flash"); key('\r');
		assert(ai_ui_choose(ui, "Model", items, 2, 0) == 1);
		nkeys = at = 0;
		type("no-match"); key(PT_KEY_PGDN); key(PT_KEY_PGUP); key(PT_KEY_ESC);
		assert(ai_ui_choose(ui, "Model", items, 2, 0) == -1);
		nkeys = at = 0;
		jb_free(&screen);
	}
	type("/mo"); key('\t'); expect(ui, "/model ");
	type("/"); expect(ui, "/");
	type("/s"); key('\t'); key('\t'); expect(ui, "/save ");
	type("café!"); key(PT_KEY_LEFT); key(127); type("è"); expect(ui, "cafè!");
	type("abc"); key(PT_KEY_HOME); key(PT_KEY_DELETE); type("A");
	key(PT_KEY_END); type("d"); expect(ui, "Abcd");
	type("draft"); key(PT_KEY_UP); key(PT_KEY_DOWN); expect(ui, "draft");
	key(PT_KEY_UP); key(PT_KEY_HOME); type("new "); expect(ui, "new draft");
	key(PT_KEY_UP); key(PT_CTRL('u')); type("edited recall");
	key(PT_KEY_UP); key(PT_KEY_DOWN); expect(ui, "edited recall");
	type("one two"); key(PT_CTRL('w')); type("three"); expect(ui, "one three");
	type("abcd"); key(PT_KEY_LEFT); key(PT_CTRL('u')); expect(ui, "d");
	type("abcd"); key(PT_KEY_HOME); key(PT_CTRL('k')); type("z"); expect(ui, "z");
	type("discard"); key(PT_CTRL('c')); type("keep"); expect(ui, "keep");
	memset(longline, 'a', sizeof(longline) - 1); longline[sizeof(longline) - 1] = 0;
	type(longline); type("overflow"); expect(ui, longline);
	for (int i = 0; i < 50; i++) {
		char s[32];

		snprintf(s, sizeof(s), "prompt %d", i);
		type(s); expect(ui, s);
	}
	assert(ui->nhistory == INPUT_HISTORY);
	for (int i = 0; i < 1000; i++) {
		char s[32];

		snprintf(s, sizeof(s), "%04d a streamed line\n", i);
		ai_ui_write(ui, s, strlen(s));
	}
	assert(ui->count == TRANSCRIPT_ROWS);
	key(PT_KEY_PGUP); assert(ai_ui_key(ui, 0) == PT_KEY_NONE);
	old_back = ui->back;
	memcpy(before, row(ui, max_back(ui) - ui->back)[0].text, sizeof(before));
	ai_ui_write(ui, "new line\n", 9);
	assert(ui->back == old_back + 1);
	assert(!memcmp(before, row(ui, max_back(ui) - ui->back)[0].text, sizeof(before)));
	key(PT_CTRL('n')); assert(ai_ui_key(ui, 0) == PT_KEY_NONE); assert(ui->back <= old_back);
	key(PT_KEY_CTRL_HOME); assert(ai_ui_key(ui, 0) == PT_KEY_NONE);
	assert(ui->back == max_back(ui));
	key(PT_KEY_CTRL_END); assert(ai_ui_key(ui, 0) == PT_KEY_NONE); assert(!ui->back);
	ui->back = 0;
	ai_ui_write(ui, "\x1b[31m", 5);
	ai_ui_write(ui, "\xc3", 1); ai_ui_write(ui, "\xa8", 1);
	assert(row(ui, ui->count - 1)[0].len == 2);
	assert(row(ui, ui->count - 1)[0].style == 31);
	ai_ui_write(ui, "\r\x1b[Kok", 6);
	assert(row(ui, ui->count - 1)[0].text[0] == 'o');
	ai_ui_status(ui, "Answering...");
	key(PT_CTRL('c')); assert(ai_ui_poll(ui));
	ai_ui_status(ui, ""); assert(!ai_ui_poll(ui));
	key(PT_KEY_PGUP); assert(!ai_ui_poll(ui));
	key(PT_KEY_ESC); assert(ai_ui_poll(ui));
	ai_ui_status(ui, ""); assert(!ai_ui_poll(ui));
	ai_ui_draft(ui, "transcribed question");
	nkeys = at = 0;
	expect(ui, "transcribed question");
	key(PT_CTRL('d')); assert(ai_ui_readline(ui, longline, sizeof(longline)) == -1);
	nkeys = at = 0;
	type("unsent draft"); key(PT_KEY_ESC);
	assert(ai_ui_readline(ui, longline, sizeof(longline)) == -2);
	ai_ui_reset(ui);
	assert(ui->count == 1 && !ui->back && !ui->len && !ui->cancelled);
	ai_ui_close(ui); assert(!raw && !app_scroll);
	jb_free(&screen);
	assert(!ai_ui_open(4, 2));
	puts("ai UI: full-width header, separator, pickers, editing, UTF-8, history, completion, scroll, cancellation passed");
	return 0;
}
