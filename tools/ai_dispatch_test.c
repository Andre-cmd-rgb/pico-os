#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "pt/sys.h"

struct chat { int unused; };
struct call { char name[64]; struct jbuf args; };
enum permission { PROJECT_CHANGE };
static int operations, allocations;
static bool fail_alloc;
void *pt_malloc(size_t n) { allocations++; return fail_alloc ? NULL : malloc(n); }
void *pt_realloc(void *p, size_t n) { return realloc(p, n); }
void pt_free(void *p) { free(p); }
const char *pt_strerror(int err) { return strerror(-err); }
#define strlcpy test_strlcpy
static size_t strlcpy(char *d, const char *s, size_t n)
{ snprintf(d, n, "%s", s); return strlen(s); }
static int ai_printf(struct chat *ch, const char *fmt, ...)
{ (void)ch; (void)fmt; return 0; }
static bool inside(struct chat *ch, const char *rel, char *out, size_t n)
{ (void)ch; strlcpy(out, rel, n); return strcmp(rel, "../outside") != 0; }
static enum permission file_permission(struct chat *ch, const char *path)
{ (void)ch; (void)path; return PROJECT_CHANGE; }
static bool allowed(struct chat *ch, const char *what, enum permission permission)
{ (void)ch; (void)what; (void)permission; return true; }
int pt_mkdir(const char *path) { (void)path; operations++; return 0; }
static void tool_list(struct chat *ch, const char *path, struct jbuf *out)
{ (void)ch; (void)path; operations++; jb_puts(out, "listed"); }
static void tool_read(struct chat *ch, const char *rel, const char *path, int from, int lines,
		      struct jbuf *out)
{ (void)ch; (void)rel; (void)path; assert(from >= 1 && lines > 0); operations++; jb_puts(out, "read"); }
static void tool_write(struct chat *ch, const char *rel, const char *path, const char *s,
		       struct jbuf *out)
{ (void)ch; (void)rel; (void)path; assert(!strcmp(s, "whole")); operations++; jb_puts(out, "written"); }
static void tool_edit(struct chat *ch, const char *rel, const char *path, const char *s,
		      const char *t, struct jbuf *out)
{ (void)ch; (void)rel; (void)path; assert(!strcmp(s, "old") && !strcmp(t, "new")); operations++; jb_puts(out, "edited"); }
static void tool_run(struct chat *ch, const char *s, struct jbuf *out)
{ (void)ch; assert(!strcmp(s, "echo checked")); operations++; jb_puts(out, "exit 0"); }
static void tool_job(struct chat *ch, int pid, bool stop, int wait, struct jbuf *out)
{ (void)ch; (void)stop; assert(pid == 7 && wait >= 0); operations++; jb_puts(out, "exit 0"); }

#include "ai_dispatch_under_test.h"

static void dispatch(const char *name, const char *args, bool success)
{
	struct chat ch = { 0 };
	struct call call = { 0 };
	struct jbuf out = { 0 };
	int before = operations;

	strlcpy(call.name, name, sizeof(call.name));
	jb_puts(&call.args, args);
	tool(&ch, &call, &out);
	assert(operations == before + success);
	assert(success || (out.p && strstr(out.p, "error:")));
	jb_free(&out); jb_free(&call.args);
}

int main(void)
{
	const char *bad[] = { "", "{", "{\"path\":\"x\",\"content\":\"partial",
		"{\"path\":\"x\",\"content\":\"whole\"} trailing", "[]",
		"{\"path\":true,\"content\":\"whole\"}", "{\"content\":\"whole\"}",
		"{\"path\":\"x\",\"content\":7}",
		"{\"path\":\"x\\u0000other\",\"content\":\"whole\"}",
		"{\"path\":\"x\",\"content\":\"whole\\u0000other\"}" };

	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
		dispatch("write_file", bad[i], false);
	dispatch("write_file", "{\"path\":\"../outside\",\"content\":\"whole\"}", false);
	dispatch("read_file", "{\"path\":\"x\",\"from\":\"2\"}", false);
	dispatch("command_status", "{\"job\":1.5}", false);
	dispatch("run", "{\"command\":null}", false);
	dispatch("list_files", "{\"path\":\".\"}", true);
	dispatch("read_file", "{\"path\":\"x\",\"from\":2,\"lines\":3}", true);
	dispatch("write_file", "{\"path\":\"x\",\"content\":\"whole\"}", true);
	dispatch("edit_file", "{\"path\":\"x\",\"old_text\":\"old\",\"new_text\":\"new\"}", true);
	dispatch("run", "{\"command\":\"echo checked\"}", true);
	dispatch("command_status", "{\"job\":7,\"wait_ms\":0}", true);
	char long_path[PT_PATH_MAX + 24];
	struct jbuf args = { 0 };

	memset(long_path, 'x', sizeof(long_path) - 1); long_path[sizeof(long_path) - 1] = 0;
	jb_puts(&args, "{\"path\":"); jb_str(&args, long_path); jb_puts(&args, "}");
	dispatch("list_files", args.p, false);
	jb_free(&args);
	struct call call = { .name = "list_files" };
	struct chat ch = { 0 };
	struct jbuf out = { 0 };

	jb_puts(&call.args, "{\"path\":\"x\"}");
	fail_alloc = true;
	tool(&ch, &call, &out);
	fail_alloc = false;
	assert(out.p && strstr(out.p, "Cannot allocate memory"));
	jb_free(&out); jb_free(&call.args);
	/* Exact-sized inputs exercise the permissive reader's boundary too. */
	for (size_t n = 0; n < 16; n++) {
		const char text[] = "{\"x\":\"broken\\";
		char *p = malloc(n ? n : 1);
		const char *end;

		memcpy(p, text, n < sizeof(text) ? n : sizeof(text));
		if (n > sizeof(text)) memset(p + sizeof(text), 'x', n - sizeof(text));
		const char *v = json_get(p, p + n, "x", &end);

		assert(!v || end <= p + n);
		pt_free(p);
	}
	puts("AI dispatch: complete typed arguments, path limits, NUL, allocation failure and JSON bounds passed");
	return 0;
}
