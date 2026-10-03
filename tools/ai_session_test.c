/* Saved chats use real files, including short writes and failed replacements. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../bin/ai_session.h"

static int write_left = -1;
static bool close_fail, rename_fail;
static size_t read_bytes;

void *pt_malloc(size_t n) { return malloc(n); }
void *pt_realloc(void *p, size_t n) { return realloc(p, n); }
void pt_free(void *p) { free(p); }
int pt_getpid(void) { return getpid(); }
int pt_open(const char *p, int flags) { int fd = open(p, flags, 0600); return fd < 0 ? -errno : fd; }
ssize_t pt_read(int fd, void *buf, size_t n)
{
	ssize_t got = read(fd, buf, n);

	if (got > 0)
		read_bytes += got;
	return got < 0 ? -errno : got;
}
ssize_t pt_write(int fd, const void *p, size_t n)
{
	ssize_t got;

	if (!write_left)
		return -ENOSPC;
	if (n > 7)
		n = 7;
	if (write_left > 0 && n > (size_t)write_left)
		n = write_left;
	got = write(fd, p, n);
	if (got > 0 && write_left > 0)
		write_left -= got;
	return got < 0 ? -errno : got;
}
int pt_close(int fd)
{
	int err = close(fd) ? -errno : 0;

	if (close_fail) {
		close_fail = false;
		return -EIO;
	}
	return err;
}
int pt_mkdir(const char *p) { return mkdir(p, 0700) ? -errno : 0; }
int pt_unlink(const char *p) { return unlink(p) ? -errno : 0; }
int pt_rename(const char *a, const char *b) { return rename_fail ? -EIO : rename(a, b) ? -errno : 0; }
struct pt_dir { DIR *dir; };
int pt_opendir(const char *p, pt_dir_t **out)
{
	DIR *dir = opendir(p);

	if (!dir)
		return -errno;
	*out = malloc(sizeof(**out));
	assert(*out);
	(*out)->dir = dir;
	return 0;
}
int pt_readdir(pt_dir_t *d, struct pt_dirent *out)
{
	struct dirent *ent = readdir(d->dir);

	if (!ent)
		return 0;
	snprintf(out->name, sizeof(out->name), "%s", ent->d_name);
	out->is_dir = ent->d_type == DT_DIR;
	return 1;
}
void pt_closedir(pt_dir_t *d) { closedir(d->dir); free(d); }

static void usage(void)
{
	const char *data = "{\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":40,"
		"\"cost\":0.0123,\"prompt_tokens_details\":{\"cached_tokens\":20},"
		"\"completion_tokens_details\":{\"reasoning_tokens\":10}}}";
	struct ai_usage u = { 0 }, total = { 0 };
	const char *bad[] = { "null", "-1", "1.5", "1e999", "9007199254740992" };

	ai_usage_read(&u, data, data + strlen(data));
	assert(u.known && u.cost_known && u.input == 100 && u.output == 40);
	assert(u.cached == 20 && u.reasoning == 10 && fabs(u.cost - .0123) < 1e-10);
	assert(ai_usage_tps(&u, 2000000) == 20 && ai_usage_tps(&u, 0) == 0);
	ai_usage_add(&total, &u);
	ai_usage_add(&total, &u);
	assert(total.input == 200 && total.output == 80 && total.reasoning == 20);
	assert(fabs(total.cost - .0246) < 1e-10);
	ai_usage_read(&u, "{}", "{}" + 2);
	assert(u.input == 100 && u.cost_known);
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		char json[160];

		u = (struct ai_usage){ 0 };
		snprintf(json, sizeof(json), "{\"usage\":{\"prompt_tokens\":%s,\"completion_tokens\":1,\"cost\":%s}}",
			 bad[i], bad[i]);
		ai_usage_read(&u, json, json + strlen(json));
		assert(!u.known);
		if (i != 2 && i != 4)
			assert(!u.cost_known);
	}
	u = (struct ai_usage){ 0 };
	data = "{\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0,\"cost\":0}}";
	ai_usage_read(&u, data, data + strlen(data));
	assert(u.known && u.cost_known && !u.cost);
}

static void intact(const char *dir, const struct jbuf *expected)
{
	struct ai_session loaded;
	struct jbuf data = { 0 };

	assert(!ai_session_load(dir, "one", &loaded, &data));
	assert(data.len == expected->len && !memcmp(data.p, expected->p, data.len));
	jb_free(&data);
}

static void overwrite(const char *path, const struct jbuf *header, const char *body)
{
	FILE *f = fopen(path, "w");

	assert(f && fwrite(header->p, 1, header->len, f) == header->len);
	assert(fputs(body, f) >= 0 && !fclose(f));
}

int main(int argc, char **argv)
{
	struct ai_session s = { .id = "one", .title = "café / studio", .root = "/home/andre/notes",
		.model = "test/model:free", .effort = "auto", .mode = 0, .updated = 10,
		.usage = { .input = 100, .output = 40, .cost = .0123, .known = true, .cost_known = true } };
	struct ai_session loaded, *list;
	struct jbuf history = { 0 }, data = { 0 }, header = { 0 }, context = { 0 };
	char path[PT_PATH_MAX], block[4096];
	int fd, n;

	assert(argc == 2);
	usage();
	jb_puts(&history, "{\"role\":\"user\",\"content\":\"old question\"},{\"role\":\"assistant\",\"content\":\"");
	memset(block, 'x', sizeof(block));
	for (int i = 0; i < 5; i++)
		jb_add(&history, block, sizeof(block));
	jb_puts(&history, "\"},{\"role\":\"status\",\"content\":\"old usage\"},"
		"{\"role\":\"user\",\"content\":\"café \\nnew question\"},"
		"{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
		"\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"note.md\\\"}\"}}]},"
		"{\"role\":\"tool\",\"tool_call_id\":\"call_1\",\"content\":\"file text\"},"
		"{\"role\":\"assistant\",\"content\":\"answer **bold**\"},"
		"{\"role\":\"status\",\"content\":\"stats\"}");
	assert(!ai_session_save(argv[1], &s, &history));
	assert(!ai_session_load(argv[1], s.id, &loaded, &data));
	assert(!strcmp(loaded.title, s.title) && !strcmp(loaded.root, s.root));
	assert(loaded.usage.known && loaded.usage.input == 100 && !loaded.partial);
	assert(fabs(loaded.usage.cost - .0123) < 1e-10);
	assert(data.len == history.len && !memcmp(data.p, history.p, data.len));
	jb_free(&data);
	read_bytes = 0;
	assert(!ai_session_load(argv[1], s.id, &loaded, NULL) && read_bytes <= 1024);
	assert(!ai_session_context(&history, 1800, &context));
	assert(strstr(context.p, "new question") && strstr(context.p, "tool_calls") && strstr(context.p, "call_1"));
	assert(!strstr(context.p, "old question") && !strstr(context.p, "stats") && !strstr(context.p, "status"));
	jb_free(&context);
	assert(!ai_session_context(&history, 10, &context));
	assert(strstr(context.p, "new question") && strstr(context.p, "file text"));
	jb_free(&context);

	/* Failed writes, failed closes, failed renames and oversize chats all
	 * leave the earlier complete snapshot available. */
	write_left = 20;
	assert(ai_session_save(argv[1], &s, &history) == -ENOSPC);
	write_left = -1;
	intact(argv[1], &history);
	close_fail = true;
	assert(ai_session_save(argv[1], &s, &history) == -EIO);
	intact(argv[1], &history);
	rename_fail = true;
	assert(ai_session_save(argv[1], &s, &history) == -EIO);
	rename_fail = false;
	intact(argv[1], &history);
	for (int i = 0; i <= AI_SESSION_MAX / (int)sizeof(block); i++)
		jb_add(&data, block, sizeof(block));
	assert(ai_session_save(argv[1], &s, &data) == -EFBIG);
	jb_free(&data);
	intact(argv[1], &history);
	assert(ai_session_load(argv[1], "../one", &loaded, NULL) == -EINVAL);
	assert(ai_session_load(argv[1], "one/other", &loaded, NULL) == -EINVAL);

	snprintf(path, sizeof(path), "%s/one.chat", argv[1]);
	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	while (read(fd, block, 1) == 1) {
		jb_add(&header, block, 1);
		if (*block == '\n')
			break;
	}
	close(fd);
	const char *corrupt[] = { "[{", "[{\"role\":\"user\",}]", "[{},]", "[] trailing", "[\"bad\\x\"]", "[01]" };
	for (size_t i = 0; i < sizeof(corrupt) / sizeof(*corrupt); i++) {
		overwrite(path, &header, corrupt[i]);
		assert(ai_session_load(argv[1], "one", &loaded, &data) == -EINVAL);
		assert(!data.len);
	}
	assert(!ai_session_save(argv[1], &s, &history));
	s.usage.known = false;
	s.usage.cost_known = false;
	s.usage.cost = 0;
	s.partial = true;
	assert(!ai_session_save(argv[1], &s, &history));
	assert(!ai_session_load(argv[1], s.id, &loaded, NULL));
	assert(loaded.partial && !loaded.usage.known && !loaded.usage.cost_known);

	for (int i = 0; i < 140; i++) {
		snprintf(s.id, sizeof(s.id), "chat-%03d", i);
		s.updated = 100 + i;
		assert(!ai_session_save(argv[1], &s, &history));
	}
	n = ai_session_list(argv[1], &list);
	assert(n == AI_SESSION_LIST_MAX && list[0].updated == 239 && list[n - 1].updated == 112);
	for (int i = 1; i < n; i++)
		assert(list[i - 1].updated >= list[i].updated);
	pt_free(list);
	jb_free(&history);
	jb_free(&header);
	puts("ai sessions: usage, full history, tool context, atomic saves, corruption and newest-chat listing passed");
	return 0;
}
