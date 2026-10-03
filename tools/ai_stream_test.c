/* Real request and question code; only the transport and board UI are mocked. */
#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_ui.h"
#include "ai_jobs.h"
#include "ai_models.h"
#include "ai_session.h"

#define CALLS_MAX 8
#define STEPS_MAX 30
#define PT_CTRL(c) ((c) & 31)
enum mode { STUDY, CODE, WEB, VOICE, MODES };
static const char *const mode_name[MODES] = { "study", "code", "web", "voice" };
enum permission { PROJECT_CHANGE, NOTE_CHANGE, COMMAND_RUN };
struct chat;
struct render { struct chat *ch; int width; bool line_start; };
struct conn { int status; };

#include "ai_stream_types.h"

static const char *stream, *input;
static size_t stream_at, chunk, input_at;
static int end_error, api_errors, errors, tools, approvals, starts;
static int malloc_n, realloc_n, fail_malloc_at, fail_realloc_at;
static bool permit = true;
static char approved[1024], started[1024];

void *pt_malloc(size_t n)
{
	return ++malloc_n == fail_malloc_at ? NULL : malloc(n);
}

void *pt_calloc(size_t n, size_t size) { return calloc(n, size); }
void *pt_realloc(void *p, size_t n)
{
	return ++realloc_n == fail_realloc_at ? NULL : realloc(p, n);
}

void pt_free(void *p) { free(p); }
char *pt_strdup(const char *s) { return strdup(s); }
int64_t pt_uptime_us(void) { static int64_t now; return ++now; }
void pt_sigcatch(bool on) { (void)on; }
bool pt_interrupted(void) { return false; }
const char *pt_strerror(int err) { return strerror(err < 0 ? -err : err); }
size_t strlcpy(char *out, const char *in, size_t size)
{
	size_t n = strlen(in);

	if (size) {
		memcpy(out, in, n < size - 1 ? n : size - 1);
		out[n < size - 1 ? n : size - 1] = '\0';
	}
	return n;
}

ssize_t pt_read(int fd, void *buf, size_t size)
{
	(void)fd;
	assert(size == 1);
	if (!input[input_at])
		return 0;
	*(char *)buf = input[input_at++];
	return 1;
}

static void ai_puts(const struct chat *ch, const char *s) { (void)ch; (void)s; }
static void ai_printf(const struct chat *ch, const char *fmt, ...) { (void)ch; (void)fmt; }
void ai_ui_status(struct ai_ui *ui, const char *s) { (void)ui; (void)s; }
int ai_ui_readline(struct ai_ui *ui, char *out, size_t size)
{
	(void)ui; (void)out; (void)size;
	assert(false);
	return -1;
}

static void build(const struct chat *ch, struct jbuf *b) { (void)ch; jb_puts(b, "{}"); }
static int conn_open(struct conn *c, struct chat *ch, const char *what, const char *body, size_t len)
{
	(void)ch; (void)what; (void)body; (void)len;
	c->status = 200;
	stream_at = 0;
	return 0;
}

static int conn_read(struct conn *c, char *buf, int size)
{
	size_t n = strlen(stream) - stream_at;

	(void)c;
	if (!n)
		return end_error;
	if (n > chunk)
		n = chunk;
	if (n > (size_t)size)
		n = size;
	memcpy(buf, stream + stream_at, n);
	stream_at += n;
	return n;
}

static void conn_close(struct conn *c) { (void)c; }
static void say_error(const struct chat *ch, int err) { (void)ch; (void)err; errors++; }
static void say_api_error(const struct chat *ch, int status, const char *p, const char *end)
{
	(void)ch; (void)status; (void)p; (void)end;
	api_errors++;
}

static void put_str(struct render *r, const char *s, int n) { (void)r; (void)s; (void)n; }
static void render_end(struct render *r) { (void)r; }
static int source_add(struct chat *ch, const char *url, const char *title)
{
	(void)ch; (void)url; (void)title;
	return 0;
}

static void history_trim(struct chat *ch) { (void)ch; }
static bool cancelled(struct chat *ch) { (void)ch; return false; }
static void account_turn(struct chat *ch, const struct turn *t) { ai_usage_add(&ch->usage, &t->usage); }
static void footer(struct chat *ch, const struct turn *t, int64_t started_at)
{
	(void)t; (void)started_at;
	jb_puts(&ch->last_footer, "response");
}

static void sources(struct chat *ch) { (void)ch; }
static void save_session(struct chat *ch) { (void)ch; }
static void tool(struct chat *ch, struct call *call, struct jbuf *out)
{
	(void)ch; (void)call;
	tools++;
	jb_puts(out, "executed");
}

static struct ai_job_io job_io(struct chat *ch)
{
	return (struct ai_job_io){ .ctx = ch };
}

bool ai_job_done(const struct ai_job *job) { return !job->pid; }
int ai_job_start(struct ai_job *job, const char *root, const char *command)
{
	(void)job; (void)root;
	starts++;
	strlcpy(started, command, sizeof(started));
	return 1;
}

void ai_job_collect(struct ai_job *job, int wait_ms, const struct ai_job_io *io, struct jbuf *out)
{
	(void)job; (void)wait_ms; (void)io;
	jb_puts(out, "exit status 0");
}

static bool allowed(struct chat *ch, const char *what, enum permission permission)
{
	(void)ch;
	assert(permission == COMMAND_RUN);
	approvals++;
	strlcpy(approved, what, sizeof(approved));
	return permit;
}

#include "ai_stream_under_test.h"

#define CALL "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0," \
	"\"id\":\"call_1\",\"function\":{\"name\":\"write_file\",\"arguments\":\"{\\\"content\\\":\\\"part\"}}]}}]}\n\n"
#define TEXT "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n"
#define COMPLETE_CALL "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0," \
	"\"id\":\"call_1\",\"function\":{\"name\":\"write_file\",\"arguments\":\"{\\\"path\\\":" \
	"\\\"note.md\\\",\\\"content\\\":\\\"whole\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
#define DONE "data: [DONE]\n\n"
#define USAGE "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]," \
	"\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":4,\"cost\":0.01}}\n\n"
#define API_ERROR "data: {\"error\":{\"message\":\"provider disconnected\"}}\n\n"
#define MALFORMED_CALL "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0," \
	"\"id\":\"call_1\",\"function\":{\"name\":\"write_file\",\"arguments\":\"{\\\"path\\\":" \
	"\\\"note.md\\\",\\\"content\\\":\\\"whole\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]\n\n"
#define NUL_CALL "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0," \
	"\"id\":\"call_1\",\"function\":{\"name\":\"write_file\",\"arguments\":\"{\\\"path\\\":" \
	"\\\"note.md\\\",\\\"content\\\":\\\"whole\\\"}\\u0000ignored\"}}]}}]}\n\n"
#define NUL_FRAGMENT "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0," \
	"\"function\":{\"arguments\":\"\\u0000ignored\"}}]}}]}\n\n"

static void reset(const char *data, size_t size, int failure)
{
	stream = data;
	stream_at = 0;
	chunk = size;
	end_error = failure;
	malloc_n = realloc_n = fail_malloc_at = fail_realloc_at = 0;
	api_errors = errors = tools = approvals = starts = 0;
}

static void free_turn(struct turn *t)
{
	jb_free(&t->text);
	for (int i = 0; i < CALLS_MAX; i++)
		jb_free(&t->calls[i].args);
}

static void failed_question(const char *data, size_t size, int failure, int malloc_failure, int realloc_failure)
{
	struct chat *ch = calloc(1, sizeof(*ch));

	assert(ch);
	strlcpy(ch->key, "test", sizeof(ch->key));
	reset(data, size, failure);
	fail_malloc_at = malloc_failure;
	fail_realloc_at = realloc_failure;
	assert(question(ch, "test failure") == (failure == -EINTR ? 130 : 1));
	assert(!tools);
	assert(ch->msgs.p && !strstr(ch->msgs.p, "tool_calls"));
	assert(strstr(ch->archive.p, "\"role\":\"status\""));
	assert(strstr(ch->last_footer.p, failure == -EINTR ? "Response stopped" : "error") ||
	       strstr(ch->last_footer.p, "memory") || strstr(ch->last_footer.p, "Invalid argument"));
	jb_free(&ch->msgs); jb_free(&ch->archive); jb_free(&ch->answer);
	jb_free(&ch->last_footer); jb_free(&ch->last_details);
	pt_free(ch->question);
	free(ch);
}

static void requests(void)
{
	struct chat ch = { .width = 53 };
	const char *bad[] = {
		CALL API_ERROR DONE, API_ERROR, CALL, CALL "data: [DONE] trailing\n",
		CALL "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n" DONE,
		CALL "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"content_filter\"}]}\n\n" DONE,
		CALL "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"error\"}]}\n\n" DONE,
		MALFORMED_CALL DONE, NUL_CALL DONE, COMPLETE_CALL NUL_FRAGMENT DONE,
	};

	for (size_t size = 1; size <= 37; size++) {
		struct turn t = { 0 };

		reset(": OPENROUTER PROCESSING\n\n" TEXT USAGE DONE, size, 0);
		assert(!ask(&ch, &t) && !errors && !api_errors);
		assert(!strcmp(t.text.p, "hello"));
		assert(t.usage.known && t.usage.cost_known && t.usage.input == 100 && t.usage.output == 4);
		free_turn(&t);
		t = (struct turn){ 0 };
		reset("data: [DONE]\r\n\r\n", size, -EINTR);
		assert(!ask(&ch, &t));
		free_turn(&t);
		t = (struct turn){ 0 };
		reset(COMPLETE_CALL USAGE DONE, size, 0);
		assert(!ask(&ch, &t) && t.ncalls == 1 && !errors && !api_errors);
		assert(!strcmp(t.calls[0].name, "write_file"));
		assert(!strcmp(t.calls[0].args.p, "{\"path\":\"note.md\",\"content\":\"whole\"}"));
		free_turn(&t);
		for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
			failed_question(bad[i], size, 0, 0, 0);
		failed_question(CALL, size, -EINTR, 0, 0);
		failed_question(CALL DONE, size, 0, 1, 0);
		/* Message, request body, line, then the tool argument buffer. */
		failed_question(CALL DONE, size, 0, 0, 4);
		failed_question(TEXT DONE, size, 0, 1, 0);
		failed_question(TEXT DONE, size, 0, 0, 4);
	}
	{
		struct turn t = { 0 };

		reset(TEXT DONE, 1024, 0);
		fail_realloc_at = 2;
		assert(ask(&ch, &t) == -ENOMEM && errors == 1);
		free_turn(&t);
	}
	{
		const char *invalid[] = { MALFORMED_CALL DONE, NUL_CALL DONE,
			COMPLETE_CALL NUL_FRAGMENT DONE };

		for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
			struct turn t = { 0 };

			reset(invalid[i], 1024, 0);
			assert(ask(&ch, &t) == -EINVAL && errors == 1);
			free_turn(&t);
		}
	}
}

static void whole_responses(void)
{
	struct conn c = { 0 };
	struct jbuf out = { 0 };
	char data[4097];

	memset(data, 'x', sizeof(data) - 1);
	data[sizeof(data) - 1] = '\0';
	reset(data, 7, 0);
	assert(!conn_all(&c, &out) && !out.oom && out.len == sizeof(data) - 1);
	assert(!strcmp(out.p, data));
	jb_free(&out);
	for (int fail = 1; fail <= 2; fail++) {
		reset(data, 128, 0);
		fail_realloc_at = fail;
		assert(conn_all(&c, &out) == -ENOMEM && out.oom);
		assert(stream_at < sizeof(data) - 1); /* Stop when assembly fails. */
		jb_free(&out);
	}
	reset(data, 128, -EINTR);
	assert(conn_all(&c, &out) == -EINTR && !out.oom);
	jb_free(&out);
}

static void commands(void)
{
	struct chat ch = { 0 };
	struct jbuf out = { 0 };
	char command[600];

	memset(command, 'x', sizeof(command));
	strcpy(command + 560, "; important suffix");
	reset("", 1, 0);
	tool_run(&ch, command, &out);
	assert(approvals == 1 && starts == 1);
	assert(!strcmp(approved + 5, command) && !strcmp(started, command));
	jb_free(&out);
	reset("", 1, 0);
	permit = false;
	tool_run(&ch, command, &out);
	assert(approvals == 1 && !starts);
	jb_free(&out);
	reset("", 1, 0);
	permit = true;
	fail_realloc_at = 1;
	tool_run(&ch, command, &out);
	assert(!approvals && !starts && strstr(out.p, "no memory"));
	jb_free(&out);
}

static void lines(void)
{
	struct chat ch = { 0 };
	char line[32];

	memset(line, 'x', sizeof(line));
	input = "unterminated"; input_at = 0;
	assert(read_question(&ch, line, sizeof(line)) == 12 && !strcmp(line, input));
	input = ""; input_at = 0;
	assert(read_question(&ch, line, sizeof(line)) == -1 && !*line);
	input = "ordinary\n"; input_at = 0;
	assert(read_question(&ch, line, sizeof(line)) == 8 && !strcmp(line, "ordinary"));
}

int main(void)
{
	requests(); whole_responses(); commands(); lines();
	puts("ai stream: completion, JSON/NUL validation, usage, provider errors, partial tools, OOM, approvals and EOF passed");
	return 0;
}
