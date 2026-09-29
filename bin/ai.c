/*
 * ai: language models over OpenRouter, for studying, coding and looking
 * things up on the web.
 *
 *	ai                     a chat (study, unless another mode is named)
 *	ai code [dir]          a coding assistant working in a project folder
 *	ai web                 answers from a web search, with their sources
 *	ai study perché ...    one question, answered, and back to the shell
 *
 * Each mode has its models in ~/.config/ai, the first wanted and the rest
 * tried when it is busy (the free ones are, often): OpenRouter walks the
 * list itself. The key is ~/.config/openrouter. The instructions each
 * mode gives its model are bin/ai/<mode>.txt, built in, or
 * ~/.config/ai-<mode>.txt when the user has written their own.
 *
 * Answers are streamed and laid out as they come: words wrapped to the
 * screen, **bold**, headings, lists and code picked out, links turned
 * into numbered sources. In code mode the model has tools -- list, read,
 * write and edit the project's files, run a command -- and every change
 * and command is shown and asked for first. /voice records a question
 * (cleaned as rec cleans it), has a model that hears write it down, and
 * sends it once it has been read back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

#include "codec.h"
#include "drivers/drivers.h"
#include "json.h"
#include "pt/keys.h"
#include "pt/kernel.h"
#include "util.h"

#define AI_STACK_KB	16
#define API		"https://openrouter.ai/api/v1/"
#define CONNECT_MS	15000
#define POLL_MS		1000
#define SILENCE_MS	120000		/* a model may think for a long time */
#define MODELS_MAX	3		/* OpenRouter's fallback list */
#define HISTORY_MAX	(48 * 1024)	/* of the conversation sent back each time */
#define STEPS_MAX	30		/* requests one question may lead to, tools and all */
#define CALLS_MAX	8		/* tool calls in one answer */
#define TOOL_OUT_MAX	6000		/* of a command's output or a file, for the model */
#define READ_MAX	(256 * 1024)	/* a file read or edited */
#define VOICE_MAX_S	30
#define VOICE_RATE	16000
#define LINE_MAX_IN	512

enum mode { STUDY, CODE, WEB, VOICE, MODES };

static const char *const mode_name[MODES] = { "study", "code", "web", "voice" };

/* Free first; the voice's second is paid, as no free one hears well. */
static const char *const default_models[MODES] = {
	"google/gemma-4-31b-it:free google/gemma-4-26b-a4b-it:free "
	"nvidia/nemotron-3-super-120b-a12b:free",
	"qwen/qwen3.8-27b:free poolside/laguna-s-2.1:free "
	"nvidia/nemotron-3-super-120b-a12b:free",
	"google/gemma-4-31b-it:free google/gemma-4-26b-a4b-it:free "
	"nvidia/nemotron-3-super-120b-a12b:free",
	"nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free google/gemini-2.5-flash-lite",
};

#define PROMPT(name) \
	extern const char name##_start[] asm("_binary_" #name "_txt_start"); \
	extern const char name##_end[] asm("_binary_" #name "_txt_end")
PROMPT(study);
PROMPT(code);
PROMPT(search);

/*
 * What code mode adds to its instructions when it has a folder to work in.
 */
static const char tools_prompt[] =
	"\n\nTOOLS\n"
	"You are working in the project folder {root} on the board. Use the tools to "
	"look at its files, change them and run commands; paths are relative to the "
	"folder. Read a file before changing it. Change existing files with edit_file, "
	"a small exact replacement, rather than writing them again; use write_file for "
	"new files. After a change, run the program or a quick check when that makes "
	"sense (pico programs: `pico file.pico`). The user is asked before every change "
	"and every command, and may say no: then find another way or ask them. When "
	"the task is done, stop and say in two or three lines what you changed.\n";

static const char tools_json[] =
	"[{\"type\":\"function\",\"function\":{\"name\":\"list_files\","
	"\"description\":\"List a folder of the project. Folders end in /.\","
	"\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
	"\"description\":\"relative to the project; . for its top\"}},\"required\":[\"path\"]}}},"
	"{\"type\":\"function\",\"function\":{\"name\":\"read_file\","
	"\"description\":\"Read a text file of the project. Each line comes with its number "
	"and a | for reference: they are not part of the file.\","
	"\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
	"\"from\":{\"type\":\"integer\",\"description\":\"first line, 1 if not given\"},"
	"\"lines\":{\"type\":\"integer\",\"description\":\"how many, 300 at most\"}},"
	"\"required\":[\"path\"]}}},"
	"{\"type\":\"function\",\"function\":{\"name\":\"write_file\","
	"\"description\":\"Create a file, or replace a whole file. To change part of an "
	"existing file use edit_file.\",\"parameters\":{\"type\":\"object\",\"properties\":"
	"{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
	"\"required\":[\"path\",\"content\"]}}},"
	"{\"type\":\"function\",\"function\":{\"name\":\"edit_file\","
	"\"description\":\"Replace one exact piece of a file's text with another. old_text "
	"must appear exactly once in the file: include enough of the lines around the "
	"change to make it unique.\",\"parameters\":{\"type\":\"object\",\"properties\":"
	"{\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},"
	"\"new_text\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_text\",\"new_text\"]}}},"
	"{\"type\":\"function\",\"function\":{\"name\":\"run\","
	"\"description\":\"Run a shell command in the project folder on the board and get "
	"what it printed and its exit status. A command that waits for keys gets none.\","
	"\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
	"\"required\":[\"command\"]}}}]";

struct source {
	char	url[256];
	char	title[96];
};

struct chat {
	enum mode	 mode;
	char		 models[MODES][256];	/* space-separated, first wanted */
	char		 engine[16];		/* the web search's */
	char		 key[160];
	char		 root[PT_PATH_MAX];	/* code: the project; "" for none */
	struct jbuf	 msgs;			/* the conversation: JSON objects, comma-joined */
	struct jbuf	 answer;		/* the last one, for /save */
	char		*question;		/* and what it answered */
	struct source	 src[12];		/* web: the sources cited, [1] onwards */
	int		 nsrc;
	bool		 always;		/* code: every tool allowed without asking */
	double		 spent;			/* this session, in dollars */
	int		 width;
};

/* ------------------------------------------------------------ settings */

static bool read_line_of(const char *path, char *out, size_t size)
{
	int fd = pt_open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return false;
	n = pt_read(fd, out, size - 1);
	pt_close(fd);
	if (n <= 0)
		return false;
	out[n] = '\0';
	out[strcspn(out, "\r\n")] = '\0';
	return *out != '\0';
}

/*
 * ~/.config/ai: a line a mode, its name and then its models; `engine`
 * for the web search. Written with the defaults the first time, so that
 * there is something to edit.
 */
static void settings_load(struct chat *ch)
{
	char path[PT_PATH_MAX], line[400];
	struct jbuf text = { 0 };
	int fd;

	for (int m = 0; m < MODES; m++)
		strlcpy(ch->models[m], default_models[m], sizeof(ch->models[m]));
	strlcpy(ch->engine, "parallel", sizeof(ch->engine));
	if (pt_home_file(".config", "ai", NULL, path, sizeof(path)))
		return;
	if ((fd = pt_open(path, O_RDONLY)) < 0) {
		jb_puts(&text, "# ai: the models of each mode, the one wanted first; the others\n"
			       "# are asked when it is busy. Any OpenRouter model id will do, paid\n"
			       "# ones too (openrouter.ai/models). engine is the web search's:\n"
			       "# parallel or exa.\n");
		for (int m = 0; m < MODES; m++)
			jb_printf(&text, "%s %s\n", mode_name[m], default_models[m]);
		jb_puts(&text, "engine parallel\n");
		if (!text.oom && (fd = pt_open(path, O_WRONLY | O_CREAT | O_TRUNC)) >= 0) {
			pt_write(fd, text.p, text.len);
			pt_close(fd);
		}
		jb_free(&text);
		return;
	}
	for (;;) {
		size_t n = 0;
		char c;
		ssize_t got;

		while ((got = pt_read(fd, &c, 1)) == 1 && c != '\n')
			if (n < sizeof(line) - 1)
				line[n++] = c;
		line[n] = '\0';
		if (got != 1 && !n)
			break;
		if (line[0] == '#' || !line[0])
			continue;
		for (int m = 0; m < MODES; m++) {
			size_t k = strlen(mode_name[m]);

			if (!strncmp(line, mode_name[m], k) && line[k] == ' ')
				strlcpy(ch->models[m], line + k + 1, sizeof(ch->models[m]));
		}
		if (!strncmp(line, "engine ", 7))
			strlcpy(ch->engine, line + 7, sizeof(ch->engine));
	}
	pt_close(fd);
}

static bool key_load(struct chat *ch)
{
	char path[PT_PATH_MAX];

	return !pt_home_file(".config", "openrouter", NULL, path, sizeof(path)) &&
	       read_line_of(path, ch->key, sizeof(ch->key));
}

/* The mode's instructions, the user's own if there are any, with {date}, {city} and {root} filled in. */
static void system_prompt(const struct chat *ch, struct jbuf *out)
{
	static const struct { const char *start, *end; } built_in[] = {
		[STUDY] = { study_start, study_end },
		[CODE] = { code_start, code_end },
		[WEB] = { search_start, search_end },
	};
	char path[PT_PATH_MAX], name[32], date[64], city[48];
	const char *tz = user_tz_name(), *p, *end;
	struct jbuf own = { 0 };
	time_t now = time(NULL);
	struct tm tm;
	int fd;

	localtime_r(&now, &tm);
	strftime(date, sizeof(date), "%A %e %B %Y, %H:%M", &tm);
	p = strrchr(tz, '/');
	strlcpy(city, p ? p + 1 : *tz ? tz : "an unknown place", sizeof(city));
	for (char *u = city; *u; u++)
		if (*u == '_')
			*u = ' ';
	snprintf(name, sizeof(name), "ai-%s.txt", mode_name[ch->mode]);
	if (!pt_home_file(".config", name, NULL, path, sizeof(path)) &&
	    (fd = pt_open(path, O_RDONLY)) >= 0) {
		char buf[512];
		ssize_t n;

		while ((n = pt_read(fd, buf, sizeof(buf))) > 0)
			jb_add(&own, buf, n);
		pt_close(fd);
	}
	if (own.len) {
		p = own.p;
		end = own.p + own.len;
	} else {
		p = built_in[ch->mode].start;
		end = built_in[ch->mode].end;
		if (end > p && !end[-1])
			end--;			/* EMBED_TXTFILES ends it with a NUL */
	}
	for (int part = 0; part < 2; part++) {
		while (p < end) {
			const char *brace = memchr(p, '{', end - p);

			if (!brace) {
				jb_add(out, p, end - p);
				break;
			}
			jb_add(out, p, brace - p);
			p = brace + 6;
			if (!strncmp(brace, "{date}", 6)) {
				jb_puts(out, date);
			} else if (!strncmp(brace, "{city}", 6)) {
				jb_puts(out, city);
			} else if (!strncmp(brace, "{root}", 6)) {
				jb_puts(out, ch->root);
			} else {
				jb_add(out, brace, 1);
				p = brace + 1;
			}
		}
		if (ch->mode != CODE || !ch->root[0])
			break;
		p = tools_prompt;
		end = tools_prompt + sizeof(tools_prompt) - 1;
	}
	jb_free(&own);
}

/* ------------------------------------------------------------ the connection */

struct conn {
	esp_http_client_handle_t client;
	int		status;
	int64_t		quiet_since;
};

static void conn_close(struct conn *c)
{
	if (c->client) {
		esp_http_client_close(c->client);
		esp_http_client_cleanup(c->client);
		c->client = NULL;
	}
}

/* A request to the API sent, and its answer's headers read: 0, or -errno. */
static int conn_open(struct conn *c, const struct chat *ch, const char *what, const char *body,
		     size_t len)
{
	char url[96], auth[200];
	esp_http_client_config_t cfg = {
		.url = url,
		.method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
		.timeout_ms = CONNECT_MS,
		.buffer_size = 4096,
		.buffer_size_tx = 2048,
		.crt_bundle_attach = esp_crt_bundle_attach,
	};
	size_t sent = 0;

	esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
	esp_log_level_set("esp-tls", ESP_LOG_NONE);
	esp_log_level_set("esp-tls-mbedtls", ESP_LOG_NONE);
	esp_log_level_set("transport_base", ESP_LOG_NONE);
	if (!wifi_up())
		return -ENETDOWN;
	snprintf(url, sizeof(url), API "%s", what);
	snprintf(auth, sizeof(auth), "Bearer %s", ch->key);
	if (!(c->client = esp_http_client_init(&cfg)))
		return -ENOMEM;
	esp_http_client_set_header(c->client, "Authorization", auth);
	esp_http_client_set_header(c->client, "X-Title", "PocketType");
	if (body)
		esp_http_client_set_header(c->client, "Content-Type", "application/json");
	if (esp_http_client_open(c->client, body ? len : 0)) {
		conn_close(c);
		return -EHOSTUNREACH;
	}
	while (body && sent < len) {
		int n = esp_http_client_write(c->client, body + sent, len - sent);

		if (n <= 0) {
			conn_close(c);
			return -EIO;
		}
		sent += n;
	}
	if (esp_http_client_fetch_headers(c->client) < 0) {
		conn_close(c);
		return -EIO;
	}
	c->status = esp_http_client_get_status_code(c->client);
	esp_http_client_set_timeout_ms(c->client, POLL_MS);
	c->quiet_since = esp_timer_get_time();
	return 0;
}

/* The next piece of the answer: its length, 0 at its end, -EINTR, -ETIMEDOUT. */
static int conn_read(struct conn *c, char *buf, int size)
{
	for (;;) {
		int n;

		if (pt_interrupted())
			return -EINTR;
		n = esp_http_client_read(c->client, buf, size);
		if (n > 0) {
			c->quiet_since = esp_timer_get_time();
			return n;
		}
		if (n == -ESP_ERR_HTTP_EAGAIN) {
			if (esp_timer_get_time() - c->quiet_since > SILENCE_MS * 1000LL)
				return -ETIMEDOUT;
			continue;
		}
		return n == 0 || esp_http_client_is_complete_data_received(c->client) ? 0 : -EIO;
	}
}

/* A whole answer that is not streamed, into out. */
static int conn_all(struct conn *c, struct jbuf *out)
{
	char buf[1024];
	int n;

	while ((n = conn_read(c, buf, sizeof(buf))) > 0)
		jb_add(out, buf, n);
	return n;
}

static void say_error(int err)
{
	pt_printf("\x1b[31mai: %s\x1b[0m\n",
		  err == -ENETDOWN ? "not on a network (wifi connect)" :
		  err == -EHOSTUNREACH ? "cannot reach openrouter.ai" :
		  err == -ETIMEDOUT ? "no answer for two minutes" :
		  err == -EINTR ? "stopped" : pt_strerror(err));
}

/* What an error answer says, in words: OpenRouter's message, and why the model's side failed. */
static void say_api_error(int status, const char *p, const char *e)
{
	const char *v, *ve;
	char msg[240] = "", raw[160] = "";

	if ((v = json_get(p, e, "error.message", &ve)))
		json_text(v, ve, msg, sizeof(msg));
	if ((v = json_get(p, e, "error.metadata.raw", &ve)))
		json_text(v, ve, raw, sizeof(raw));
	if (status == 402 && strstr(msg, "audio"))
		strlcpy(msg, "OpenRouter wants $0.50 in the account before it takes audio",
			sizeof(msg));
	else if (status == 429)
		strlcpy(msg, "every model on the list is busy: try again in a minute, or "
			     "/model a paid one", sizeof(msg));
	pt_printf("\x1b[31mai: %s%s%s\x1b[0m\n", *msg ? msg : "the request failed",
		  *raw && status != 429 ? ": " : "", status != 429 ? raw : "");
}

/* ------------------------------------------------------------ laying out */

enum link_state { L_NONE, L_TEXT, L_CLOSED, L_URL };

struct render {
	struct chat	*ch;
	int		 width, col, indent;
	bool		 line_start, heading, bold, italic, code, block, skip_line, star;
	int		 ticks;			/* backticks at the start of a line */
	char		 word[192];
	int		 wlen, wcols;
	enum link_state	 link;
	char		 ltext[160], lurl[256];
	int		 ltlen, lulen;
};

static void flush_word(struct render *r)
{
	if (!r->wlen)
		return;
	if (r->col > r->indent && r->col + r->wcols > r->width) {
		pt_printf("\n%*s", r->indent, "");
		r->col = r->indent;
	}
	pt_write(PT_STDOUT, r->word, r->wlen);
	r->col += r->wcols;
	r->wlen = r->wcols = 0;
}

static void word_add(struct render *r, const char *s, int n, int cols)
{
	if (r->wlen + n >= (int)sizeof(r->word))
		flush_word(r);
	memcpy(r->word + r->wlen, s, n);
	r->wlen += n;
	r->wcols += cols;
	if (r->wcols >= r->width - r->indent)
		flush_word(r);			/* a word longer than a line */
}

static void newline(struct render *r)
{
	flush_word(r);
	if (r->heading || r->bold) {
		pt_puts("\x1b[22m");
		r->heading = r->bold = false;
	}
	if (r->italic) {			/* never left open past its line */
		pt_puts("\x1b[39m");
		r->italic = false;
	}
	pt_puts("\n");
	r->col = r->indent = 0;
	r->line_start = true;
	r->ticks = 0;
}

static int source_add(struct chat *ch, const char *url, const char *title)
{
	for (int i = 0; i < ch->nsrc; i++)
		if (!strcmp(ch->src[i].url, url))
			return i + 1;
	if (ch->nsrc == (int)(sizeof(ch->src) / sizeof(ch->src[0])))
		return 0;
	strlcpy(ch->src[ch->nsrc].url, url, sizeof(ch->src[0].url));
	strlcpy(ch->src[ch->nsrc].title, title ? title : "", sizeof(ch->src[0].title));
	return ++ch->nsrc;
}

static void put(struct render *r, char c);

static void put_str(struct render *r, const char *s, int n)
{
	for (int i = 0; i < n; i++)
		put(r, s[i]);
}

/* A Markdown link finished: its words, and the source's number. */
static void link_done(struct render *r)
{
	char mark[16];
	int n = source_add(r->ch, r->lurl, r->ltext);
	bool bare = !memchr(r->ltext, ' ', r->ltlen) && memchr(r->ltext, '.', r->ltlen);

	r->link = L_NONE;
	if (!bare)				/* a site's name alone says nothing the number does not */
		put_str(r, r->ltext, r->ltlen);
	snprintf(mark, sizeof(mark), "[%d]", n);
	if (n)
		put_str(r, mark, strlen(mark));
}

/*
 * One byte of the answer onto the screen. Words are held until they end,
 * so that a line breaks between them; the Markdown the models write is
 * turned into what the terminal can do.
 */
static void put(struct render *r, char c)
{
	unsigned char u = c;

	if (r->link == L_TEXT) {
		if (c == ']')
			r->link = L_CLOSED;
		else if (c == '\n' || r->ltlen == (int)sizeof(r->ltext) - 1) {
			r->link = L_NONE;		/* not a link after all */
			put(r, '[');
			put_str(r, r->ltext, r->ltlen);
			put(r, c);
		} else
			r->ltext[r->ltlen++] = c;
		return;
	}
	if (r->link == L_CLOSED) {
		if (c == '(') {
			r->link = L_URL;
			r->lulen = 0;
			return;
		}
		r->link = L_NONE;			/* [1], or brackets in the text */
		put(r, '[');
		put_str(r, r->ltext, r->ltlen);
		put(r, ']');
		put(r, c);
		return;
	}
	if (r->link == L_URL) {
		if (c == ')') {
			r->lurl[r->lulen] = '\0';
			r->ltext[r->ltlen] = '\0';
			link_done(r);
		} else if (r->lulen < (int)sizeof(r->lurl) - 1)
			r->lurl[r->lulen++] = c;
		return;
	}
	if (r->skip_line) {				/* a fence's language name */
		if (c == '\n') {
			r->skip_line = false;
			pt_puts("\n");
			r->col = 0;
			r->line_start = true;
		}
		return;
	}
	if (r->line_start && c == '`') {
		r->ticks++;
		if (r->ticks == 3) {
			r->block = !r->block;
			pt_puts(r->block ? "\x1b[36m" : "\x1b[39m");
			r->skip_line = true;
			r->ticks = 0;
		}
		return;
	}
	if (r->line_start && r->ticks) {		/* one or two: inline code, then this */
		int t = r->ticks;

		r->ticks = 0;
		r->line_start = false;
		while (t--)
			put(r, '`');
		put(r, c);
		return;
	}
	if (r->block) {
		pt_write(PT_STDOUT, &c, 1);
		r->line_start = c == '\n';
		return;
	}
	if (r->star) {					/* the star before this one */
		r->star = false;
		if (c == '*') {
			r->bold = !r->bold;
			word_add(r, r->bold ? "\x1b[1m" : "\x1b[22m", r->bold ? 4 : 5, 0);
			return;
		}
		if (r->line_start && c == ' ') {
			r->line_start = false;
			pt_puts("\xe2\x80\xa2 ");	/* a bullet */
			r->col += 2;
			r->indent = r->col;
			return;
		}
		/*
		 * *italics*, in the paler colour notes uses: a star that
		 * opens against a word, or closes one. "2 * 3" stays.
		 */
		if (!r->italic && c != ' ' && c != '\n') {
			r->italic = true;
			word_add(r, "\x1b[33m", 5, 0);
		} else if (r->italic) {
			r->italic = false;
			word_add(r, "\x1b[39m", 5, 0);
		} else {
			word_add(r, "*", 1, 1);
		}
		r->line_start = false;
	}
	if (c == '\n') {
		newline(r);
		return;
	}
	if (r->line_start) {
		if (c == ' ') {				/* a nested list's indent */
			pt_puts(" ");
			r->col++;
			return;
		}
		if (c == '#') {
			if (!r->heading)
				pt_puts("\x1b[1m");
			r->heading = true;
			return;
		}
		if (r->heading && c == ' ')
			return;
		if (c == '-') {
			r->star = false;
			r->line_start = false;
			r->ticks = -1;			/* "- " is a bullet; see below */
			return;
		}
	}
	if (r->ticks == -1) {				/* after a "-" at the start */
		r->ticks = 0;
		if (c == ' ') {
			pt_puts("\xe2\x80\xa2 ");
			r->col += 2;
			r->indent = r->col;
			return;
		}
		word_add(r, "-", 1, 1);
	}
	if (c == '*' && !r->code) {
		r->star = true;
		return;
	}
	r->line_start = false;
	if (c == '`') {
		r->code = !r->code;
		word_add(r, r->code ? "\x1b[36m" : "\x1b[39m", 5, 0);
		return;
	}
	if (c == '[' && !r->code && r->ch->mode == WEB) {
		r->link = L_TEXT;
		r->ltlen = 0;
		return;
	}
	if (c == ' ' || c == '\t') {
		flush_word(r);
		if (r->col < r->width) {
			pt_puts(" ");
			r->col++;
		}
		return;
	}
	word_add(r, &c, 1, (u & 0xc0) != 0x80);	/* a continuation byte takes no room */
}

static void render_end(struct render *r)
{
	if (r->star)
		word_add(r, "*", 1, 1);
	if (r->link != L_NONE) {
		put_str(r, r->ltext, r->ltlen);
		r->link = L_NONE;
	}
	flush_word(r);
	pt_puts("\x1b[0m");
	if (r->col)
		pt_puts("\n");
	r->col = 0;
}

/* ------------------------------------------------------------ the conversation */

struct call {
	char		id[80];
	char		name[24];
	struct jbuf	args;
};

struct turn {
	struct render	 r;
	struct jbuf	 text;
	struct call	 calls[CALLS_MAX];
	int		 ncalls;
	char		 model[96];
	double		 cost;
	bool		 thinking;
};

static void msg_add(struct chat *ch, const char *role, const char *content)
{
	if (ch->msgs.len)
		jb_puts(&ch->msgs, ",");
	jb_printf(&ch->msgs, "{\"role\":\"%s\",\"content\":", role);
	jb_str(&ch->msgs, content);
	jb_puts(&ch->msgs, "}");
}

/*
 * Too long a conversation costs more with every question, and fills the
 * memory: the oldest questions go, a whole one at a time, so that what is
 * left starts with a question and a tool's answer never loses its call.
 */
static void history_trim(struct chat *ch)
{
	struct jbuf arr = { 0 };
	const char *v, *ve, *rv, *rve;
	char path[16], role[16];
	int n;

	if (ch->msgs.len <= HISTORY_MAX)
		return;
	jb_puts(&arr, "[");
	jb_add(&arr, ch->msgs.p, ch->msgs.len);
	jb_puts(&arr, "]");
	if (arr.oom || (n = json_count(arr.p, arr.p + arr.len)) < 2) {
		jb_free(&arr);
		return;
	}
	for (int i = 1; i < n; i++) {
		snprintf(path, sizeof(path), "%d", i);
		if (!(v = json_get(arr.p, arr.p + arr.len, path, &ve)))
			break;
		snprintf(path, sizeof(path), "%d.role", i);
		if ((rv = json_get(arr.p, arr.p + arr.len, path, &rve)))
			json_text(rv, rve, role, sizeof(role));
		else
			role[0] = '\0';
		if (!strcmp(role, "user") && arr.p + arr.len - v - 1 <= HISTORY_MAX) {
			struct jbuf kept = { 0 };

			jb_add(&kept, v, arr.p + arr.len - 1 - v);
			if (!kept.oom) {
				jb_free(&ch->msgs);
				ch->msgs = kept;
			}
			break;
		}
	}
	jb_free(&arr);
}

static void build(const struct chat *ch, struct jbuf *b)
{
	struct jbuf sys = { 0 };
	const char *m = ch->models[ch->mode];
	int n = 0;

	jb_puts(b, "{\"models\":[");
	while (*m && n < MODELS_MAX) {
		size_t len = strcspn(m, " ");

		if (len) {
			jb_puts(b, n++ ? "," : "");
			jb_strn(b, m, len);
		}
		m += len + (m[len] == ' ');
	}
	jb_puts(b, "],\"stream\":true,\"usage\":{\"include\":true},"
		   "\"reasoning\":{\"effort\":\"low\"}");
	if (ch->mode == WEB)
		jb_printf(b, ",\"plugins\":[{\"id\":\"web\",\"engine\":\"%s\",\"max_results\":5}]",
			  ch->engine);
	if (ch->mode == CODE && ch->root[0])
		jb_printf(b, ",\"tools\":%s", tools_json);
	system_prompt(ch, &sys);
	jb_puts(b, ",\"messages\":[{\"role\":\"system\",\"content\":");
	jb_strn(b, sys.p ? sys.p : "", sys.len);
	jb_puts(b, "}");
	if (ch->msgs.len) {
		jb_puts(b, ",");
		jb_add(b, ch->msgs.p, ch->msgs.len);
	}
	jb_puts(b, "]}");
	jb_free(&sys);
}

/* A piece of a tool call, as it streams: its id, its name, more of its arguments. */
static void take_calls(struct turn *t, const char *v, const char *ve)
{
	int n = json_count(v, ve);

	for (int i = 0; i < n; i++) {
		const char *item, *ie, *f, *fe;
		char path[16];
		int at;

		snprintf(path, sizeof(path), "%d", i);
		if (!(item = json_get(v, ve, path, &ie)))
			continue;
		at = (f = json_get(item, ie, "index", &fe)) ? (int)json_num(f, fe, i) : i;
		if (at < 0 || at >= CALLS_MAX)
			continue;
		if (at >= t->ncalls)
			t->ncalls = at + 1;
		if ((f = json_get(item, ie, "id", &fe)) && *f == '"')
			json_text(f, fe, t->calls[at].id, sizeof(t->calls[at].id));
		if ((f = json_get(item, ie, "function.name", &fe)) && *f == '"')
			json_text(f, fe, t->calls[at].name, sizeof(t->calls[at].name));
		if ((f = json_get(item, ie, "function.arguments", &fe)) && *f == '"') {
			char *args = json_dup(f, fe);

			if (args) {
				jb_puts(&t->calls[at].args, args);
				pt_free(args);
			}
		}
	}
}

/* One event of the stream: a piece of the answer, a tool call, the cost. */
static void event(struct turn *t, const char *p, const char *e)
{
	const char *d, *de, *v, *ve;

	if (!t->model[0] && (v = json_get(p, e, "model", &ve)))
		json_text(v, ve, t->model, sizeof(t->model));
	if ((v = json_get(p, e, "usage.cost", &ve)))
		t->cost = json_num(v, ve, 0);
	if (!(d = json_get(p, e, "choices.0.delta", &de)))
		return;
	if ((v = json_get(d, de, "reasoning", &ve)) && *v == '"' && !t->thinking && !t->text.len) {
		pt_puts("\x1b[2mthinking...\x1b[0m\r");
		t->thinking = true;
	}
	if ((v = json_get(d, de, "content", &ve)) && *v == '"' && ve - v > 2) {
		char *s = json_dup(v, ve);

		if (s) {
			if (t->thinking && !t->text.len) {
				pt_puts("\x1b[K");	/* the "thinking" goes */
				t->thinking = false;
			}
			jb_puts(&t->text, s);
			put_str(&t->r, s, strlen(s));
			pt_free(s);
		}
	}
	if ((v = json_get(d, de, "tool_calls", &ve)) && *v == '[')
		take_calls(t, v, ve);
	if ((v = json_get(d, de, "annotations", &ve)) && *v == '[') {
		int n = json_count(v, ve);

		for (int i = 0; i < n; i++) {
			char path[32], url[256] = "", title[96] = "";
			const char *u, *ue;

			snprintf(path, sizeof(path), "%d.url_citation.url", i);
			if ((u = json_get(v, ve, path, &ue)))
				json_text(u, ue, url, sizeof(url));
			snprintf(path, sizeof(path), "%d.url_citation.title", i);
			if ((u = json_get(v, ve, path, &ue)))
				json_text(u, ue, title, sizeof(title));
			if (*url) {
				int k = source_add(t->r.ch, url, title);

				if (k && !t->r.ch->src[k - 1].title[0])
					strlcpy(t->r.ch->src[k - 1].title, title, sizeof(title));
			}
		}
	}
}

/*
 * One request: sent, and its answer streamed onto the screen as it comes.
 * Returns 0 with the answer in *t, or -errno (already said).
 */
static int ask(struct chat *ch, struct turn *t)
{
	struct jbuf body = { 0 }, line = { 0 };
	struct conn c = { 0 };
	char buf[1024];
	int err, n;

	build(ch, &body);
	if (body.oom) {
		jb_free(&body);
		say_error(-ENOMEM);
		return -ENOMEM;
	}
	pt_puts("\x1b[2m...\x1b[0m\r");
	err = conn_open(&c, ch, "chat/completions", body.p, body.len);
	jb_free(&body);
	pt_puts("\x1b[K");
	if (err) {
		say_error(err);
		return err;
	}
	if (c.status != 200) {
		struct jbuf all = { 0 };

		conn_all(&c, &all);
		conn_close(&c);
		say_api_error(c.status, all.p ? all.p : "", all.p ? all.p + all.len : "");
		jb_free(&all);
		return -EIO;
	}
	t->r = (struct render){ .ch = ch, .width = ch->width, .line_start = true };
	/* server-sent events: "data: {...}" lines, ": ..." to keep it open */
	while ((n = conn_read(&c, buf, sizeof(buf))) > 0) {
		for (int i = 0; i < n; i++) {
			if (buf[i] != '\n') {
				jb_add(&line, buf + i, 1);
				continue;
			}
			if (line.len > 6 && !strncmp(line.p, "data: ", 6) &&
			    strncmp(line.p + 6, "[DONE]", 6)) {
				const char *v, *ve;

				if ((v = json_get(line.p + 6, line.p + line.len, "error.message", &ve)))
					say_api_error(0, line.p + 6, line.p + line.len);
				else
					event(t, line.p + 6, line.p + line.len);
			}
			line.len = 0;
		}
	}
	jb_free(&line);
	conn_close(&c);
	render_end(&t->r);
	if (n < 0) {
		say_error(n);
		return n;
	}
	return 0;
}

/* ------------------------------------------------------------ the tools */

/*
 * A path the model gave, inside the project: made absolute, its "." and
 * ".." worked out, and refused if it leads out of the folder.
 */
static bool inside(const struct chat *ch, const char *given, char *out, size_t size)
{
	size_t root = strlen(ch->root), len;
	const char *p = given;

	if (*given == '/') {
		if (strncmp(given, ch->root, root) || (given[root] && given[root] != '/'))
			return false;
		p = given + root;
	}
	if (strlcpy(out, ch->root, size) >= size)
		return false;
	len = root;
	while (*p) {
		size_t n;

		while (*p == '/')
			p++;
		n = strcspn(p, "/");
		if (!n)
			break;
		if (n == 2 && !strncmp(p, "..", 2)) {
			if (len <= root)
				return false;
			while (len > root && out[len - 1] != '/')
				len--;
			len -= len > root;
			out[len] = '\0';
		} else if (!(n == 1 && *p == '.')) {
			if (len + 1 + n >= size)
				return false;
			out[len++] = '/';
			memcpy(out + len, p, n);
			len += n;
			out[len] = '\0';
		}
		p += n;
	}
	return true;
}

/* y, n or a: whether the user lets it. */
static bool allowed(struct chat *ch, const char *what)
{
	int k;

	pt_printf("\x1b[33m\xe2\x86\x92 %s\x1b[0m\n", what);
	if (ch->always)
		return true;
	pt_puts("  allow? [y]es [n]o [a]lways: ");
	pt_tty_raw(PT_STDIN, true);
	do
		k = pt_readkey(PT_STDIN);
	while (k >= 0 && !strchr("yYnNaA\r", k) && k != PT_KEY_ESC);
	pt_tty_raw(PT_STDIN, false);
	if (k == 'a' || k == 'A')
		ch->always = true;
	pt_puts(k == 'y' || k == 'Y' || k == '\r' ? "yes\n" : ch->always ? "always\n" : "no\n");
	return k == 'y' || k == 'Y' || k == '\r' || ch->always;
}

static char *slurp(const char *path, size_t *len, int *err)
{
	struct jbuf b = { 0 };
	char buf[1024];
	ssize_t n;
	int fd = pt_open(path, O_RDONLY);

	if (fd < 0) {
		*err = fd;
		return NULL;
	}
	while ((n = pt_read(fd, buf, sizeof(buf))) > 0 && b.len < READ_MAX)
		jb_add(&b, buf, n);
	pt_close(fd);
	if (b.oom || !b.p) {
		jb_free(&b);
		*err = b.oom ? -ENOMEM : 0;
		if (!b.oom) {
			*len = 0;
			return pt_strdup("");
		}
		return NULL;
	}
	*len = b.len;
	return b.p;
}

static int spill(const char *path, const char *text, size_t len)
{
	char tmp[PT_PATH_MAX + 8];
	int fd, err = 0;

	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if ((fd = pt_open(tmp, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
		return fd;
	if (len && pt_write(fd, text, len) != (ssize_t)len)
		err = -EIO;
	pt_close(fd);
	if (!err)
		err = pt_rename(tmp, path);	/* whole or not at all */
	if (err)
		pt_unlink(tmp);
	return err;
}

static void tool_list(struct chat *ch, const char *path, struct jbuf *out)
{
	struct pt_dirent ent;
	pt_dir_t *d;
	int n = 0;

	pt_printf("\x1b[2m\xe2\x86\x92 list %s\x1b[0m\n", *path ? path : ".");
	if (pt_opendir(path, &d)) {
		jb_puts(out, "error: no such folder");
		return;
	}
	while (pt_readdir(d, &ent) == 1 && n++ < 300)
		jb_printf(out, "%s%s\n", ent.name, ent.is_dir ? "/" : "");
	pt_closedir(d);
	if (!n)
		jb_puts(out, "(empty)");
}

static void tool_read(const char *shown, const char *path, int from, int lines, struct jbuf *out)
{
	size_t len;
	int err = 0, at = 1, shown_n = 0;
	char *text;
	const char *p, *end;

	pt_printf("\x1b[2m\xe2\x86\x92 read %s\x1b[0m\n", shown);
	if (!(text = slurp(path, &len, &err))) {
		jb_printf(out, "error: %s", pt_strerror(err));
		return;
	}
	if (from < 1)
		from = 1;
	if (lines < 1 || lines > 300)
		lines = 300;
	for (p = text, end = text + len; p < end && out->len < TOOL_OUT_MAX * 2; at++) {
		const char *nl = memchr(p, '\n', end - p);
		size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);

		if (at >= from && at < from + lines) {
			jb_printf(out, "%4d|", at);
			jb_add(out, p, n);
			jb_puts(out, "\n");
			shown_n++;
		}
		p += n + (nl != NULL);
	}
	if (!shown_n)
		jb_puts(out, len ? "(no lines there)" : "(the file is empty)");
	else if (p < end || at - 1 >= from + lines)
		jb_printf(out, "(more follows: read from line %d)", from + shown_n);
	pt_free(text);
}

static void tool_write(struct chat *ch, const char *shown, const char *path, const char *content,
		       struct jbuf *out)
{
	char what[PT_PATH_MAX + 48];
	int lines = 0, err;

	for (const char *p = content; *p; p++)
		lines += *p == '\n';
	snprintf(what, sizeof(what), "write %s (%d lines)", shown, lines + (*content != 0));
	if (!allowed(ch, what)) {
		jb_puts(out, "The user did not allow this.");
		return;
	}
	err = spill(path, content, strlen(content));
	jb_puts(out, err ? pt_strerror(err) : "written");
}

static void tool_edit(struct chat *ch, const char *shown, const char *path, const char *old,
		      const char *new, struct jbuf *out)
{
	char what[PT_PATH_MAX + 200], first_old[48], first_new[48];
	size_t len, olen = strlen(old);
	char *text, *hit, *again;
	int err = 0;

	if (!*old || !(text = slurp(path, &len, &err))) {
		jb_printf(out, "error: %s", *old ? pt_strerror(err) : "old_text is empty");
		return;
	}
	hit = strstr(text, old);
	again = hit ? strstr(hit + 1, old) : NULL;
	if (!hit || again) {
		jb_puts(out, !hit ? "error: old_text is not in the file (read it again)" :
			      "error: old_text appears more than once: include more lines around it");
		pt_printf("\x1b[2m\xe2\x86\x92 edit %s: %s\x1b[0m\n", shown,
			  !hit ? "the text is not there" : "the text is there twice");
		pt_free(text);
		return;
	}
	snprintf(first_old, sizeof(first_old), "%.*s", (int)strcspn(old, "\n"), old);
	snprintf(first_new, sizeof(first_new), "%.*s", (int)strcspn(new, "\n"), new);
	snprintf(what, sizeof(what), "edit %s\n  - %s\n  + %s", shown, first_old, first_new);
	if (!allowed(ch, what)) {
		jb_puts(out, "The user did not allow this.");
		pt_free(text);
		return;
	}
	{
		struct jbuf b = { 0 };

		jb_add(&b, text, hit - text);
		jb_puts(&b, new);
		jb_add(&b, hit + olen, len - (hit - text) - olen);
		err = b.oom ? -ENOMEM : spill(path, b.p ? b.p : "", b.len);
		jb_free(&b);
	}
	pt_free(text);
	jb_puts(out, err ? pt_strerror(err) : "edited");
}

/* The command run by the shell in the project, what it printed kept. */
static void tool_run(struct chat *ch, const char *command, struct jbuf *out)
{
	char what[300], cwd[PT_PATH_MAX], scrap[256];
	char *argv[] = { "sh", "-c", (char *)command, NULL };
	struct pt_spawn req = { .cmd = "sh", .argc = 3, .argv = argv, .fd = { -1, -1, -1 } };
	int fds[2], pid, status = 0, null_fd;
	bool stopped = false;
	ssize_t n;

	snprintf(what, sizeof(what), "run: %s", command);
	if (!allowed(ch, what)) {
		jb_puts(out, "The user did not allow this.");
		return;
	}
	if (pt_pipe(fds)) {
		jb_puts(out, "error: no pipe");
		return;
	}
	null_fd = pt_open("/dev/null", O_RDONLY);
	req.fd[0] = null_fd;
	req.fd[1] = req.fd[2] = fds[1];
	strlcpy(cwd, pt_getcwd(), sizeof(cwd));
	pt_chdir(ch->root);
	pt_sigcatch(true);
	pid = pt_spawn(&req);
	pt_chdir(cwd);
	pt_close(fds[1]);
	if (null_fd >= 0)
		pt_close(null_fd);
	if (pid < 0) {
		pt_close(fds[0]);
		pt_sigcatch(false);
		jb_printf(out, "error: %s", pt_strerror(pid));
		return;
	}
	/* shown as it comes, and kept for the model up to a limit */
	while ((n = pt_read(fds[0], scrap, sizeof(scrap))) != 0) {
		if (n == -EINTR) {
			if (!stopped)
				pt_kill(pid, PT_SIGINT);
			stopped = true;
			pt_sigcatch(true);
			continue;
		}
		if (n < 0)
			break;
		pt_printf("\x1b[2m");
		pt_write(PT_STDOUT, scrap, n);
		pt_printf("\x1b[0m");
		if (out->len < TOOL_OUT_MAX)
			jb_add(out, scrap, n);
	}
	pt_close(fds[0]);
	while (pt_wait(pid, &status, false) == -EINTR)
		;
	pt_sigcatch(true);
	if (out->len >= TOOL_OUT_MAX)
		jb_puts(out, "\n(the rest of the output was cut)");
	jb_printf(out, "\n%s %d", stopped ? "stopped by the user; status" : "exit status", status);
}

static void tool(struct chat *ch, struct call *c, struct jbuf *out)
{
	const char *a = c->args.p ? c->args.p : "{}", *e = a + c->args.len, *v, *ve;
	char rel[PT_PATH_MAX] = ".", path[PT_PATH_MAX];
	char *s1 = NULL, *s2 = NULL, *s3 = NULL;

	if (!c->args.len)
		e = a + 2;
	if ((v = json_get(a, e, "path", &ve)))
		json_text(v, ve, rel, sizeof(rel));
	if (strcmp(c->name, "run") && !inside(ch, rel, path, sizeof(path))) {
		pt_printf("\x1b[2m\xe2\x86\x92 %s %s: outside the project, refused\x1b[0m\n",
			  c->name, rel);
		jb_puts(out, "error: that path is outside the project folder");
		return;
	}
	if (!strcmp(c->name, "list_files")) {
		tool_list(ch, path, out);
	} else if (!strcmp(c->name, "read_file")) {
		int from = (v = json_get(a, e, "from", &ve)) ? (int)json_num(v, ve, 1) : 1;
		int lines = (v = json_get(a, e, "lines", &ve)) ? (int)json_num(v, ve, 300) : 300;

		tool_read(rel, path, from, lines, out);
	} else if (!strcmp(c->name, "write_file") && (v = json_get(a, e, "content", &ve)) &&
		   (s1 = json_dup(v, ve))) {
		tool_write(ch, rel, path, s1, out);
	} else if (!strcmp(c->name, "edit_file") && (v = json_get(a, e, "old_text", &ve)) &&
		   (s1 = json_dup(v, ve)) && (v = json_get(a, e, "new_text", &ve)) &&
		   (s2 = json_dup(v, ve))) {
		tool_edit(ch, rel, path, s1, s2, out);
	} else if (!strcmp(c->name, "run") && (v = json_get(a, e, "command", &ve)) &&
		   (s3 = json_dup(v, ve))) {
		tool_run(ch, s3, out);
	} else {
		jb_printf(out, "error: %s is not a tool here, or its arguments are wrong",
			  c->name);
	}
	pt_free(s1);
	pt_free(s2);
	pt_free(s3);
}

/* ------------------------------------------------------------ a question */

static void footer(struct chat *ch, const struct turn *t, int64_t started)
{
	const char *slash = strchr(t->model, '/');
	int64_t ms = (pt_uptime_us() - started) / 1000;

	ch->spent += t->cost;
	pt_printf("\x1b[2m-- %s, %d.%d s, ", slash ? slash + 1 : t->model[0] ? t->model : "?",
		  (int)(ms / 1000), (int)(ms % 1000 / 100));
	if (t->cost > 0)
		pt_printf("$%.4f\x1b[0m\n", t->cost);
	else
		pt_printf("free\x1b[0m\n");
}

static void sources(struct chat *ch)
{
	for (int i = 0; i < ch->nsrc; i++) {
		const char *host = strstr(ch->src[i].url, "://");
		char h[48];

		host = host ? host + 3 : ch->src[i].url;
		snprintf(h, sizeof(h), "%.*s", (int)strcspn(host, "/"), host);
		pt_printf("\x1b[2m[%d] %.*s%s%s\x1b[0m\n", i + 1, ch->width - 12,
			  ch->src[i].title[0] ? ch->src[i].title : h,
			  ch->src[i].title[0] ? " - " : "", ch->src[i].title[0] ? h : "");
	}
}

/*
 * A question, and its answer: in code mode, as many rounds of tool calls
 * as the model needs, each one asked for, until it answers in words.
 */
static void question(struct chat *ch, const char *q)
{
	int64_t started = pt_uptime_us();
	size_t before;
	int steps = 0;

	ch->nsrc = 0;
	history_trim(ch);
	before = ch->msgs.len;
	msg_add(ch, "user", q);
	pt_free(ch->question);
	ch->question = pt_strdup(q);
	jb_free(&ch->answer);
	pt_sigcatch(true);
	for (;;) {
		struct turn *t = pt_calloc(1, sizeof(*t));
		int err;

		if (!t) {
			say_error(-ENOMEM);
			break;
		}
		err = ask(ch, t);
		if (err) {
			/* nothing came back: the question goes too, or it would be asked twice */
			if (!steps && ch->msgs.p) {
				ch->msgs.len = before;
				ch->msgs.p[before] = '\0';
			}
			for (int i = 0; i < CALLS_MAX; i++)
				jb_free(&t->calls[i].args);
			jb_free(&t->text);
			pt_free(t);
			break;
		}
		/* the answer into the conversation, tool calls and all */
		jb_puts(&ch->msgs, ",{\"role\":\"assistant\",\"content\":");
		jb_strn(&ch->msgs, t->text.p ? t->text.p : "", t->text.len);
		if (t->ncalls) {
			jb_puts(&ch->msgs, ",\"tool_calls\":[");
			for (int i = 0; i < t->ncalls; i++) {
				struct call *c = &t->calls[i];

				if (!c->id[0])
					snprintf(c->id, sizeof(c->id), "call_%d_%d", steps, i);
				jb_printf(&ch->msgs, "%s{\"id\":", i ? "," : "");
				jb_str(&ch->msgs, c->id);
				jb_puts(&ch->msgs, ",\"type\":\"function\",\"function\":{\"name\":");
				jb_str(&ch->msgs, c->name);
				jb_puts(&ch->msgs, ",\"arguments\":");
				jb_strn(&ch->msgs, c->args.p ? c->args.p : "{}", c->args.p ? c->args.len : 2);
				jb_puts(&ch->msgs, "}}");
			}
			jb_puts(&ch->msgs, "]");
		}
		jb_puts(&ch->msgs, "}");
		if (t->text.len) {
			jb_free(&ch->answer);
			ch->answer = t->text;
			t->text = (struct jbuf){ 0 };
		}
		/* the tools it asked for, and their results, for the next round */
		for (int i = 0; i < t->ncalls; i++) {
			struct jbuf out = { 0 };

			tool(ch, &t->calls[i], &out);
			jb_puts(&ch->msgs, ",{\"role\":\"tool\",\"tool_call_id\":");
			jb_str(&ch->msgs, t->calls[i].id);
			jb_puts(&ch->msgs, ",\"content\":");
			jb_strn(&ch->msgs, out.p ? out.p : "", out.len);
			jb_puts(&ch->msgs, "}");
			jb_free(&out);
			jb_free(&t->calls[i].args);
		}
		if (!t->ncalls || ++steps >= STEPS_MAX || pt_interrupted() || ch->msgs.oom) {
			if (steps >= STEPS_MAX)
				pt_printf("\x1b[33mai: %d rounds of tools: stopping here\x1b[0m\n",
					  STEPS_MAX);
			footer(ch, t, started);
			jb_free(&t->text);
			pt_free(t);
			break;
		}
		ch->spent += t->cost;
		jb_free(&t->text);
		pt_free(t);
	}
	if (ch->mode == WEB && ch->nsrc)
		sources(ch);
	pt_sigcatch(false);
}

/* ------------------------------------------------------------ the voice */

/*
 * A question said out loud: recorded until Enter (Esc drops it), cleaned
 * as rec cleans a voice, and written down by a model that hears. The text
 * is shown before it is sent. Returns it pt_malloc'd, or NULL.
 */
static char *voice(struct chat *ch)
{
	size_t cap = (size_t)VOICE_RATE * 2 * VOICE_MAX_S, got = WAV_HEADER_BYTES, b64len = 0;
	uint8_t *wav = pt_malloc(WAV_HEADER_BYTES + cap);
	struct voice *clean = voice_new(VOICE_RATE, 15, -20);
	struct jbuf body = { 0 }, all = { 0 };
	struct conn c = { 0 };
	unsigned char *b64 = NULL;
	char *text = NULL;
	const char *m = ch->models[VOICE];
	int key = PT_KEY_NONE, err, n = 0;
	int64_t start;

	if (!wav || !clean || !audio_present()) {
		pt_puts(audio_present() ? "ai: no memory to record\n" : "ai: there is no microphone\n");
		goto out;
	}
	audio_set_rate(VOICE_RATE);
	audio_mic_alc_hold(true);
	pt_tty_raw(PT_STDIN, true);
	pt_puts("\x1b[31m\xe2\x97\x8f\x1b[0m speak; Enter when done, Esc to drop it\n");
	start = pt_uptime_us();
	while (got < WAV_HEADER_BYTES + cap) {
		size_t want = VOICE_RATE / 4 * 2;	/* a quarter of a second */
		ssize_t r;

		if (want > WAV_HEADER_BYTES + cap - got)
			want = WAV_HEADER_BYTES + cap - got;
		if ((r = audio_read(wav + got, want)) <= 0)
			break;
		voice_run(clean, (int16_t *)(wav + got), r / 2);
		got += r;
		pt_printf("\r  %d s ", (int)((pt_uptime_us() - start) / 1000000));
		key = pt_readkey_timeout(PT_STDIN, 0);
		if (key == '\r' || key == '\n' || key == PT_KEY_ESC || key == PT_CTRL('c'))
			break;
	}
	audio_stop();
	audio_mic_alc_hold(false);
	pt_tty_raw(PT_STDIN, false);
	pt_puts("\n");
	if (key == PT_KEY_ESC || key == PT_CTRL('c') || got < WAV_HEADER_BYTES + VOICE_RATE / 2) {
		pt_puts("ai: dropped\n");
		goto out;
	}
	wav_header(wav, VOICE_RATE, 1, got - WAV_HEADER_BYTES);
	mbedtls_base64_encode(NULL, 0, &b64len, wav, got);
	if (!(b64 = pt_malloc(b64len + 1)) ||
	    mbedtls_base64_encode(b64, b64len + 1, &b64len, wav, got)) {
		pt_puts("ai: no memory for the recording\n");
		goto out;
	}
	pt_free(wav);
	wav = NULL;
	jb_puts(&body, "{\"models\":[");
	while (*m && n < MODELS_MAX) {
		size_t len = strcspn(m, " ");

		if (len) {
			jb_puts(&body, n++ ? "," : "");
			jb_strn(&body, m, len);
		}
		m += len + (m[len] == ' ');
	}
	jb_puts(&body, "],\"usage\":{\"include\":true},\"reasoning\":{\"effort\":\"low\"},"
		       "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
		       "\"Write down exactly what is said in this recording, in the language it "
		       "is said in, with correct spelling and accents. Write only what is said: "
		       "no comments, no quotation marks. If nothing can be understood, write "
		       "nothing.\"},{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"");
	jb_add(&body, (char *)b64, b64len);
	jb_puts(&body, "\",\"format\":\"wav\"}}]}]}");
	pt_free(b64);
	b64 = NULL;
	if (body.oom) {
		pt_puts("ai: no memory for the recording\n");
		goto out;
	}
	pt_puts("\x1b[2mlistening...\x1b[0m\r");
	pt_sigcatch(true);
	err = conn_open(&c, ch, "chat/completions", body.p, body.len);
	pt_puts("\x1b[K");
	if (!err)
		err = conn_all(&c, &all);
	conn_close(&c);
	pt_sigcatch(false);
	if (err) {
		say_error(err);
	} else if (c.status != 200) {
		say_api_error(c.status, all.p ? all.p : "", all.p ? all.p + all.len : "");
	} else {
		const char *v, *ve;

		if ((v = json_get(all.p, all.p + all.len, "usage.cost", &ve)))
			ch->spent += json_num(v, ve, 0);
		if ((v = json_get(all.p, all.p + all.len, "choices.0.message.content", &ve)) &&
		    (text = json_dup(v, ve))) {
			char *s = text, *e = text + strlen(text);

			while (*s == ' ' || *s == '\n')
				s++;
			while (e > s && (e[-1] == ' ' || e[-1] == '\n'))
				*--e = '\0';
			memmove(text, s, e - s + 1);
			if (!*text) {
				pt_puts("ai: nothing was understood\n");
				pt_free(text);
				text = NULL;
			}
		}
	}
out:
	jb_free(&body);
	jb_free(&all);
	pt_free(b64);
	pt_free(wav);
	voice_free(clean);
	return text;
}

/* ------------------------------------------------------------ the chat */

static void show_models(const struct chat *ch)
{
	pt_printf("%s: %s\n", mode_name[ch->mode], ch->models[ch->mode]);
}

static void set_mode(struct chat *ch, enum mode m, const char *root)
{
	if (m != ch->mode) {
		jb_free(&ch->msgs);		/* another mode, another conversation */
		ch->always = false;
	}
	ch->mode = m;
	if (m == CODE) {
		const char *dir = root && *root ? root : pt_getcwd();
		struct pt_stat st;

		if (*dir != '/')
			snprintf(ch->root, sizeof(ch->root), "%s/%s", pt_getcwd(), dir);
		else
			strlcpy(ch->root, dir, sizeof(ch->root));
		if (pt_stat(ch->root, &st) || !st.is_dir) {
			pt_printf("ai: %s: not a folder; no tools\n", ch->root);
			ch->root[0] = '\0';
		}
		{
			size_t len = strlen(ch->root);

			while (len > 1 && ch->root[len - 1] == '/')
				ch->root[--len] = '\0';
		}
	}
}

static void credits(struct chat *ch)
{
	struct conn c = { 0 };
	struct jbuf all = { 0 };
	int err;

	pt_sigcatch(true);
	err = conn_open(&c, ch, "credits", NULL, 0);
	if (!err)
		err = conn_all(&c, &all);
	conn_close(&c);
	pt_sigcatch(false);
	if (err) {
		say_error(err);
	} else if (c.status != 200) {
		say_api_error(c.status, all.p ? all.p : "", all.p ? all.p + all.len : "");
	} else {
		const char *e = all.p + all.len, *v, *ve;
		double bought = (v = json_get(all.p, e, "data.total_credits", &ve)) ? json_num(v, ve, 0) : 0;
		double used = (v = json_get(all.p, e, "data.total_usage", &ve)) ? json_num(v, ve, 0) : 0;

		pt_printf("$%.2f left of $%.2f bought; $%.4f this session\n", bought - used, bought,
			  ch->spent);
		if (bought < 10)
			pt_printf("(with $10 bought in all, the free models allow\n"
				  "1000 requests a day instead of 50)\n");
	}
	jb_free(&all);
}

static void save(struct chat *ch)
{
	char path[PT_PATH_MAX], name[24];
	time_t now = time(NULL);
	struct tm tm;
	int fd;

	if (!ch->answer.len || !ch->question) {
		pt_puts("ai: nothing to save yet\n");
		return;
	}
	localtime_r(&now, &tm);
	strftime(name, sizeof(name), "%Y-%m-%d.md", &tm);
	if (pt_home_file("notes/ai", name, NULL, path, sizeof(path)) ||
	    (fd = pt_open(path, O_WRONLY | O_CREAT | O_APPEND)) < 0) {
		pt_puts("ai: cannot write to ~/notes/ai\n");
		return;
	}
	pt_dprintf(fd, "## %s\n\n", ch->question);
	pt_write(fd, ch->answer.p, ch->answer.len);
	pt_dprintf(fd, "\n\n");
	pt_close(fd);
	pt_printf("saved in ~/notes/ai/%s\n", name);
}

static void help(void)
{
	pt_puts("/study /code [dir] /web  change mode (and start\n"
		"                         a new conversation)\n"
		"/voice or /v   say the question\n"
		"/model [ids]   this mode's models (paid ones too)\n"
		"/new           forget the conversation\n"
		"/save          the last answer into ~/notes/ai\n"
		"/cost          what is left in the account\n"
		"/quit, Ctrl-D  back to the shell\n"
		"Ctrl-C stops an answer. In code mode the model\n"
		"asks before it changes a file or runs anything.\n");
}

/* A command typed at the prompt: false to leave. */
static bool command(struct chat *ch, char *line)
{
	char *arg = line + strcspn(line, " ");

	while (*arg == ' ')
		*arg++ = '\0';
	if (!strcmp(line, "/quit") || !strcmp(line, "/q") || !strcmp(line, "/exit"))
		return false;
	if (!strcmp(line, "/study"))
		set_mode(ch, STUDY, NULL);
	else if (!strcmp(line, "/code"))
		set_mode(ch, CODE, arg);
	else if (!strcmp(line, "/web") || !strcmp(line, "/search"))
		set_mode(ch, WEB, NULL);
	else if (!strcmp(line, "/new")) {
		jb_free(&ch->msgs);
		ch->always = false;
		pt_puts("a new conversation\n");
		return true;
	} else if (!strcmp(line, "/model") || !strcmp(line, "/models")) {
		if (*arg)
			strlcpy(ch->models[ch->mode], arg, sizeof(ch->models[0]));
		show_models(ch);
		return true;
	} else if (!strcmp(line, "/save"))
		save(ch);
	else if (!strcmp(line, "/cost"))
		credits(ch);
	else if (!strcmp(line, "/voice") || !strcmp(line, "/v")) {
		char *said = voice(ch);

		if (said) {
			int k;

			pt_printf("\x1b[1m%s\x1b[0m\nEnter sends it, Esc drops it\n", said);
			pt_tty_raw(PT_STDIN, true);
			do
				k = pt_readkey(PT_STDIN);
			while (k >= 0 && k != '\r' && k != '\n' && k != PT_KEY_ESC && k != PT_CTRL('c'));
			pt_tty_raw(PT_STDIN, false);
			if (k == '\r' || k == '\n')
				question(ch, said);
			pt_free(said);
		}
		return true;
	} else if (!strcmp(line, "/help") || !strcmp(line, "/?"))
		help();
	else
		pt_printf("ai: %s: not a command (/help)\n", line);
	if (!strncmp(line, "/study", 6) || !strncmp(line, "/code", 5) || !strncmp(line, "/web", 4) ||
	    !strncmp(line, "/search", 7)) {
		show_models(ch);
		if (ch->mode == CODE && ch->root[0])
			pt_printf("working in %s\n", ch->root);
	}
	return true;
}

/* A line typed at the prompt, in the terminal's own editing: its length, -1 at the end. */
static int read_question(const struct chat *ch, char *buf, size_t size)
{
	size_t n = 0;

	pt_printf("\x1b[1m%s>\x1b[0m ", mode_name[ch->mode]);
	for (;;) {
		char c;
		ssize_t got = pt_read(PT_STDIN, &c, 1);

		if (got <= 0 || pt_interrupted())
			return n ? (int)n : -1;
		if (c == '\n')
			break;
		if (n < size - 1)
			buf[n++] = c;
	}
	buf[n] = '\0';
	return (int)n;
}

PT_COMPLETE(ai, ": study code web -m\ncode: <dir>\n")

PT_PROGRAM_STACK(ai, AI_STACK_KB, "talk to a language model, over OpenRouter\n"
	   "usage: ai [-m model] [study | code [dir] | web] [question]\n"
	   "study answers like a tutor, in Italian; code works on\n"
	   "the files of a project folder, asking before every\n"
	   "change; web searches and gives its sources. With a\n"
	   "question it answers it and ends; without, a chat\n"
	   "(/help there). The key is ~/.config/openrouter, the\n"
	   "models of each mode ~/.config/ai.")
{
	struct chat *ch = pt_calloc(1, sizeof(*ch));
	struct pt_winsize ws = { 53, 23 };
	char line[LINE_MAX_IN];
	struct jbuf q = { 0 };
	int i = 1;

	if (!ch)
		return fail("ai", NULL, -ENOMEM);
	settings_load(ch);
	if (!key_load(ch)) {
		pt_dprintf(PT_STDERR, "ai: no key: put your OpenRouter key on one line in\n"
				      "    ~/.config/openrouter (openrouter.ai/keys)\n");
		pt_free(ch);
		return 1;
	}
	pt_ioctl(PT_STDOUT, PT_TTY_GETSIZE, &ws);
	ch->width = ws.cols ? ws.cols : 53;
	if (i + 1 < argc && !strcmp(argv[i], "-m")) {
		strlcpy(line, argv[i + 1], sizeof(line));
		i += 2;
	} else {
		line[0] = '\0';
	}
	if (i < argc && !strcmp(argv[i], "code")) {
		bool dir = i + 1 < argc && (strchr(argv[i + 1], '/') || !strcmp(argv[i + 1], ".") ||
					    !strcmp(argv[i + 1], ".."));

		set_mode(ch, CODE, dir ? argv[i + 1] : NULL);
		i += 1 + dir;
	} else if (i < argc && (!strcmp(argv[i], "web") || !strcmp(argv[i], "search"))) {
		set_mode(ch, WEB, NULL);
		i++;
	} else if (i < argc && !strcmp(argv[i], "study")) {
		i++;
	}
	if (line[0])
		strlcpy(ch->models[ch->mode], line, sizeof(ch->models[0]));
	for (; i < argc; i++) {
		if (q.len)
			jb_puts(&q, " ");
		jb_puts(&q, argv[i]);
	}
	if (q.len) {
		question(ch, q.p);
		jb_free(&q);
	} else {
		pt_printf("\x1b[1mai\x1b[0m, %s mode: %.*s\n", mode_name[ch->mode],
			  (int)strcspn(ch->models[ch->mode], " "), ch->models[ch->mode]);
		if (ch->mode == CODE && ch->root[0])
			pt_printf("working in %s\n", ch->root);
		pt_puts("/help for the commands, /quit to leave\n");
		for (;;) {
			int n = read_question(ch, line, sizeof(line));

			if (n < 0) {
				pt_puts("\n");
				break;
			}
			if (!n)
				continue;
			if (line[0] == '/') {
				if (!command(ch, line))
					break;
				continue;
			}
			question(ch, line);
		}
	}
	jb_free(&ch->msgs);
	jb_free(&ch->answer);
	pt_free(ch->question);
	pt_free(ch);
	return 0;
}
