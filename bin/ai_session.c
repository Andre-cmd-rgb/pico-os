#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_session.h"

/* Metadata is one JSON line, followed by the message array. Listing does
 * not read every conversation, and a truncated file is never resumed. */
#define HEADER_MAX 4096

static bool valid_id(const char *id)
{
	size_t n = strlen(id);

	if (!n || n >= sizeof(((struct ai_session *)0)->id))
		return false;
	for (; *id; id++)
		if (!isalnum((unsigned char)*id) && *id != '-' && *id != '_')
			return false;
	return true;
}

static int filename(const char *dir, const char *id, char *out, size_t size)
{
	if (!valid_id(id))
		return -EINVAL;
	return snprintf(out, size, "%s/%s.chat", dir, id) >= (int)size ? -ENAMETOOLONG : 0;
}

static int write_all(int fd, const char *p, size_t len)
{
	while (len) {
		ssize_t n = pt_write(fd, p, len);

		if (n <= 0)
			return n < 0 ? (int)n : -EIO;
		p += n;
		len -= n;
	}
	return 0;
}

int ai_session_save(const char *dir, const struct ai_session *s, const struct jbuf *history)
{
	char path[PT_PATH_MAX], tmp[PT_PATH_MAX + 32];
	struct jbuf header = { 0 };
	int fd, err, closed;

	if (history->oom)
		return -ENOMEM;
	if (history->len > AI_SESSION_MAX)
		return -EFBIG;
	if ((err = filename(dir, s->id, path, sizeof(path))))
		return err;
	jb_printf(&header, "{\"version\":1,\"id\":");
	jb_str(&header, s->id);
	jb_puts(&header, ",\"title\":");
	jb_str(&header, s->title);
	jb_puts(&header, ",\"root\":");
	jb_str(&header, s->root);
	jb_puts(&header, ",\"model\":");
	jb_str(&header, s->model);
	jb_puts(&header, ",\"effort\":");
	jb_str(&header, s->effort);
	jb_printf(&header, ",\"mode\":%d,\"updated\":%lld,\"partial\":%s,\"reported\":%s,"
		  "\"usage\":{\"prompt_tokens\":%llu,\"completion_tokens\":%llu,"
		  "\"prompt_tokens_details\":{\"cached_tokens\":%llu},"
		  "\"completion_tokens_details\":{\"reasoning_tokens\":%llu}",
		  s->mode, (long long)s->updated, s->partial ? "true" : "false",
		  s->usage.known ? "true" : "false",
		  (unsigned long long)s->usage.input, (unsigned long long)s->usage.output,
		  (unsigned long long)s->usage.cached, (unsigned long long)s->usage.reasoning);
	if (s->usage.cost_known)
		jb_printf(&header, ",\"cost\":%.12g", s->usage.cost);
	jb_puts(&header, "}}\n");
	if (header.oom || header.len > HEADER_MAX) {
		err = header.oom ? -ENOMEM : -EFBIG;
		goto out;
	}
	pt_mkdir(dir);
	snprintf(tmp, sizeof(tmp), "%s.ai-%d.new", path, pt_getpid());
	if ((fd = pt_open(tmp, O_WRONLY | O_CREAT | O_TRUNC)) < 0) {
		err = fd;
		goto out;
	}
	err = write_all(fd, header.p, header.len);
	if (!err)
		err = write_all(fd, "[", 1);
	if (!err)
		err = write_all(fd, history->p ? history->p : "", history->len);
	if (!err)
		err = write_all(fd, "]\n", 2);
	closed = pt_close(fd);
	if (!err)
		err = closed;
	if (!err)
		err = pt_rename(tmp, path);
	if (err)
		pt_unlink(tmp);
out:
	jb_free(&header);
	return err;
}

int ai_session_load(const char *dir, const char *id, struct ai_session *s, struct jbuf *history)
{
	char path[PT_PATH_MAX], chunk[512];
	struct jbuf data = { 0 };
	const char *end, *v, *ve, *nl;
	int fd, err;
	ssize_t n;
	double number;

	if ((err = filename(dir, id, path, sizeof(path))))
		return err;
	if ((fd = pt_open(path, O_RDONLY)) < 0)
		return fd;
	while ((n = pt_read(fd, chunk, sizeof(chunk))) > 0) {
		jb_add(&data, chunk, n);
		if (data.oom || data.len > AI_SESSION_MAX + HEADER_MAX + 2) {
			n = data.oom ? -ENOMEM : -EFBIG;
			break;
		}
		if (!history && memchr(data.p, '\n', data.len))
			break;
	}
	pt_close(fd);
	err = n < 0 ? (int)n : -EINVAL;
	nl = data.p ? memchr(data.p, '\n', data.len) : NULL;
	if (n < 0 || !nl || nl - data.p > HEADER_MAX || *data.p != '{' ||
	    !json_valid(data.p, nl - data.p))
		goto out;
	end = nl;
	memset(s, 0, sizeof(*s));
	v = json_get(data.p, end, "version", &ve);
	if (!v || json_num(v, ve, 0) != 1)
		goto out;
	v = json_get(data.p, end, "id", &ve);
	if (!v)
		goto out;
	json_text(v, ve, s->id, sizeof(s->id));
	if (strcmp(s->id, id))
		goto out;
	#define FIELD(name) do { \
		v = json_get(data.p, end, #name, &ve); \
		if (v) json_text(v, ve, s->name, sizeof(s->name)); \
	} while (0)
	FIELD(title); FIELD(root); FIELD(model); FIELD(effort);
	#undef FIELD
	v = json_get(data.p, end, "mode", &ve);
	number = v ? json_num(v, ve, -1) : -1;
	if (!isfinite(number) || number < 0 || number > 2 || number != floor(number))
		goto out;
	s->mode = (int)number;
	if (s->mode < 0 || s->mode > 2 || !*s->root || !*s->model || !*s->effort)
		goto out;
	v = json_get(data.p, end, "updated", &ve);
	number = v ? json_num(v, ve, 0) : 0;
	if (!isfinite(number) || number < 0 || number >= 9007199254740992.0)
		goto out;
	s->updated = (int64_t)number;
	v = json_get(data.p, end, "partial", &ve);
	s->partial = v && !strncmp(v, "true", 4);
	ai_usage_read(&s->usage, data.p, end);
	v = json_get(data.p, end, "reported", &ve);
	s->usage.known = v && !strncmp(v, "true", 4);
	if (history) {
		const char *p = nl + 1, *e = data.p + data.len;

		while (e > p && isspace((unsigned char)e[-1]))
			e--;
		if (e - p < 2 || *p != '[' || e[-1] != ']' || !json_valid(p, e - p) ||
		    json_count(p, e) < 0)
			goto out;
		jb_add(history, p + 1, e - p - 2);
		if (history->oom) {
			err = -ENOMEM;
			goto out;
		}
	}
	err = 0;
out:
	jb_free(&data);
	return err;
}

static int recent_first(const void *a, const void *b)
{
	const struct ai_session *x = a, *y = b;

	return x->updated < y->updated ? 1 : x->updated > y->updated ? -1 : strcmp(y->id, x->id);
}

/* Keep whole questions and their tool exchanges together. Display-only
 * footers never go back to the model. A single large turn remains intact. */
int ai_session_context(const struct jbuf *history, size_t limit, struct jbuf *context)
{
	const char *p = history->p, *end, *start = NULL, *last_user = NULL;

	if (!history->len)
		return 0;
	end = p + history->len;
	while (p < end) {
		const char *v, *ve, *r, *re;
		char role[16];

		v = json_get(p, end, "", &ve);
		if (!v || ve <= p || *v != '{' || !(r = json_get(v, ve, "role", &re)))
			return -EINVAL;
		json_text(r, re, role, sizeof(role));
		if (!strcmp(role, "user")) {
			last_user = v;
			if (!start && (size_t)(end - v) <= limit)
				start = v;
		} else if (strcmp(role, "assistant") && strcmp(role, "tool") && strcmp(role, "status"))
			return -EINVAL;
		p = ve;
		while (p < end && (*p == ',' || isspace((unsigned char)*p)))
			p++;
	}
	if (!start)
		start = last_user;
	for (p = start; p && p < end;) {
		const char *v, *ve, *r, *re;
		char role[16];

		v = json_get(p, end, "", &ve);
		r = json_get(v, ve, "role", &re);
		json_text(r, re, role, sizeof(role));
		if (strcmp(role, "status")) {
			if (context->len)
				jb_puts(context, ",");
			jb_add(context, v, ve - v);
		}
		p = ve;
		while (p < end && (*p == ',' || isspace((unsigned char)*p)))
			p++;
	}
	return context->oom ? -ENOMEM : 0;
}

int ai_session_list(const char *dir, struct ai_session **sessions)
{
	pt_dir_t *d;
	struct pt_dirent ent;
	struct ai_session *list, item;
	int n = 0, err;

	*sessions = NULL;
	if ((err = pt_opendir(dir, &d)))
		return err == -ENOENT ? 0 : err;
	list = pt_malloc(AI_SESSION_LIST_MAX * sizeof(*list));
	if (!list) {
		pt_closedir(d);
		return -ENOMEM;
	}
	while (pt_readdir(d, &ent) == 1) {
		size_t len = strlen(ent.name);

		if (ent.is_dir || len <= 5 || strcmp(ent.name + len - 5, ".chat"))
			continue;
		ent.name[len - 5] = '\0';
		if (ai_session_load(dir, ent.name, &item, NULL))
			continue;
		if (n < AI_SESSION_LIST_MAX)
			list[n++] = item;
		else if (recent_first(&item, &list[n - 1]) < 0)
			list[n - 1] = item;
		else
			continue;
		qsort(list, n, sizeof(*list), recent_first);
	}
	pt_closedir(d);
	*sessions = list;
	return n;
}
