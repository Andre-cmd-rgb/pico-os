/* Exercise the actual renderer, including the /web citation stack overflow. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "json.h"
#include "util.h"

enum mode { STUDY, CODE, WEB };
struct source { char url[256], title[96]; };
struct chat { enum mode mode; int nsrc; struct source src[12]; };
static struct jbuf output;

void *pt_realloc(void *p, size_t n) { return realloc(p, n); }
void pt_free(void *p) { free(p); }

static void ai_write(const struct chat *ch, const char *s, size_t n)
{
	(void)ch;
	jb_add(&output, s, n);
}

static void ai_puts(const struct chat *ch, const char *s)
{
	ai_write(ch, s, strlen(s));
}

static void ai_printf(const struct chat *ch, const char *fmt, ...)
{
	char s[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	assert(n >= 0 && n < (int)sizeof(s));
	ai_write(ch, s, n);
}

#include "ai_renderer_under_test.h"

static void check(enum mode mode, const char *text, const char *expected, int sources)
{
	/* Streaming chunks may end inside a label, URL, or UTF-8 character. */
	for (int chunk = 1; chunk <= 37; chunk++) {
		struct chat ch = { .mode = mode };
		struct render r = { .ch = &ch, .width = 53, .line_start = true };
		size_t len = strlen(text);

		for (size_t at = 0; at < len; at += chunk) {
			size_t n = len - at < (size_t)chunk ? len - at : (size_t)chunk;

			put_str(&r, text + at, n);
		}
		render_end(&r);
		assert(!output.oom && r.link == L_NONE);
		assert(strstr(output.p, expected));
		assert(ch.nsrc == sources);
		if (sources)
			assert(!strcmp(ch.src[0].url, "https://example.com"));
		jb_free(&output);
	}
}

int main(void)
{
	char long_label[400], links[2048];
	struct chat ch = { .mode = WEB };
	struct render r = { .ch = &ch, .width = 53, .line_start = true };

	check(WEB, "Hi! Here is a citation [1].\n", "citation [1].", 0);
	check(WEB, "[1][2] and [brackets] plus []!", "[1][2] and [brackets] plus []!", 0);
	check(WEB, "[two words\nNext line", "[two words\nNext line", 0);
	check(WEB, "ends [open", "ends [open", 0);
	check(WEB, "ends [closed]", "ends [closed]", 0);
	check(WEB, "ends [label](https://exam", "[label](https://exam", 0);
	check(WEB, "[A useful link](https://example.com) [1].", "A useful link[1] [1].", 1);
	check(WEB, "[example.com](https://example.com)!", "[1]!", 1);
	check(WEB, "[Café](https://example.com)", "Café[1]", 1);
	check(WEB, "[[nested]](https://example.com)", "[[nested]](https://example.com)", 0);
	check(WEB, "[**literal**](https://example.com)", "**literal**[1]", 1);
	check(WEB, "`[1]`", "[1]", 0);
	check(WEB, "```c\narray[1];\n```\n", "array[1];", 0);
	check(STUDY, "[link](https://example.com)", "[link](https://example.com)", 0);
	memset(long_label, 'x', sizeof(long_label));
	long_label[0] = '[';
	strcpy(long_label + sizeof(long_label) - 8, "] end\n");
	check(WEB, long_label, "] end", 0);
	links[0] = 0;
	for (int i = 0; i < 20; i++) {
		char s[80];

		snprintf(s, sizeof(s), "[Link %d](https://example.com/%d) ", i, i);
		strcat(links, s);
	}
	put_str(&r, links, strlen(links));
	render_end(&r);
	assert(ch.nsrc == 12 && strstr(output.p, "Link 19"));
	jb_free(&output);
	puts("ai renderer: citations, links, partial links, UTF-8, fences and source limits passed");
	return 0;
}
