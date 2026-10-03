/* Real slurp and tool_read; the input file and terminal are mocked. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "pt/sys.h"

#define READ_MAX (256 * 1024)
#define TOOL_OUT_MAX 6000

struct chat { int unused; };
enum permission { PROJECT_CHANGE };
static const char *contents;
static const char *approval_change;
static size_t content_len, position;
static int closes, spills;
static bool fail_strdup, fail_realloc;
static char written[80];

void *pt_malloc(size_t n) { return malloc(n); }
void *pt_realloc(void *p, size_t n) { return fail_realloc ? NULL : realloc(p, n); }
void pt_free(void *p) { free(p); }
char *pt_strdup(const char *s) { return fail_strdup ? NULL : strdup(s); }
const char *pt_strerror(int err) { return strerror(err < 0 ? -err : err); }

int pt_open(const char *path, int flags)
{
	assert(!strcmp(path, "/fixture") && flags == O_RDONLY);
	position = 0;
	return 7;
}

ssize_t pt_read(int fd, void *buf, size_t n)
{
	assert(fd == 7);
	if (n > content_len - position)
		n = content_len - position;
	memcpy(buf, contents + position, n);
	position += n;
	return n;
}

int pt_close(int fd) { assert(fd == 7); closes++; return 0; }
static void ai_printf(const struct chat *ch, const char *fmt, ...)
{ (void)ch; (void)fmt; }
static enum permission file_permission(const struct chat *ch, const char *path)
{ (void)ch; (void)path; return PROJECT_CHANGE; }
static bool allowed(struct chat *ch, const char *what, enum permission permission)
{
	(void)ch; (void)what; (void)permission;
	if (approval_change) {
		contents = approval_change;
		content_len = strlen(contents);
	}
	return true;
}
static int spill(const char *path, const char *text, size_t n)
{
	assert(!strcmp(path, "/fixture") && n < sizeof(written));
	memcpy(written, text, n);
	written[n] = '\0';
	spills++;
	return 0;
}

#include "ai_file_read_under_test.h"

static void fixture(const char *data, size_t n)
{
	contents = data;
	content_len = n;
	position = closes = 0;
	spills = 0;
	approval_change = NULL;
	fail_strdup = fail_realloc = false;
}

static void read_file(int from, int lines, struct jbuf *out)
{
	struct chat ch = { 0 };
	int before = closes;

	tool_read(&ch, "fixture", "/fixture", from, lines, out);
	assert(closes == before + 1);
	assert(!out->oom && out->len <= TOOL_OUT_MAX * 2);
}

int main(void)
{
	struct jbuf out = { 0 }, many = { 0 };
	char *large = malloc(READ_MAX), *text;
	size_t len;
	int err;

	assert(large);
	fixture("first\nsecond\nthird\n", 19);
	read_file(2, 1, &out);
	assert(strstr(out.p, "   2|second\n"));
	assert(!strstr(out.p, "first") && !strstr(out.p, "third"));
	assert(strstr(out.p, "more follows: read from line 3"));
	jb_free(&out);
	read_file(20, 3, &out);
	assert(strstr(out.p, "(no lines there)"));
	jb_free(&out);

	fixture("", 0);
	read_file(1, 300, &out);
	assert(strstr(out.p, "(the file is empty)"));
	jb_free(&out);
	len = 99; err = 0; closes = 0;
	fail_strdup = true;
	text = slurp("/fixture", &len, &err);
	assert(!text && err == -ENOMEM && !len && closes == 1);
	fail_strdup = false;

	fixture("data", 4);
	fail_realloc = true;
	err = 0;
	assert(!slurp("/fixture", &len, &err) && err == -ENOMEM && closes == 1);
	fail_realloc = false;

	memset(large, 'x', READ_MAX);
	fixture(large, READ_MAX);
	read_file(1, 300, &out);
	assert(strstr(out.p, "   1|"));
	assert(strstr(out.p, "(line truncated)"));
	assert(out.len < READ_MAX);
	jb_free(&out);

	for (int i = 1; i <= 200; i++)
		jb_printf(&many, "row%03d %080d\n", i, i);
	fixture(many.p, many.len);
	read_file(1, 300, &out);
	{
		const char *more = strstr(out.p, "(more follows: read from line ");
		char expected[32];
		int next = 0;

		assert(more && sscanf(more, "(more follows: read from line %d)", &next) == 1);
		assert(next > 1 && next <= 200);
		jb_free(&out);
		read_file(next, 1, &out);
		snprintf(expected, sizeof(expected), "%4d|row%03d ", next, next);
		assert(strstr(out.p, expected));
	}
	jb_free(&out);
	jb_free(&many);
	{
		struct chat ch = { 0 };

		fixture("one old three", 13);
		tool_edit(&ch, "fixture", "/fixture", "old", "new", &out);
		assert(spills == 1 && !strcmp(written, "one new three") && closes == 2);
		assert(!strcmp(out.p, "edited"));
		jb_free(&out);
		fixture("one old three", 13);
		approval_change = "one OLD three"; /* Same length, changed during approval. */
		tool_edit(&ch, "fixture", "/fixture", "old", "new", &out);
		assert(!spills && !strcmp(contents, approval_change) && closes == 2);
		assert(strstr(out.p, "file changed"));
		jb_free(&out);
	}
	free(large);
	puts("AI files: empty-file OOM, cleanup, line selection, bounded output, pagination and conflicting edits passed");
	return 0;
}
