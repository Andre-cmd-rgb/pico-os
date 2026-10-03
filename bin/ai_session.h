/* Saved chats keep their full transcript separately from the API context. */
#pragma once

#include "ai_stats.h"
#include "json.h"
#include "pt/sys.h"

#define AI_SESSION_MAX (1024 * 1024)
#define AI_SESSION_LIST_MAX 128

struct ai_session {
	char id[64], title[128], root[PT_PATH_MAX], model[256], effort[16];
	int mode;
	int64_t updated;
	struct ai_usage usage;
	bool partial;
};

int ai_session_save(const char *dir, const struct ai_session *session, const struct jbuf *history);
int ai_session_load(const char *dir, const char *id, struct ai_session *session, struct jbuf *history);
int ai_session_list(const char *dir, struct ai_session **sessions);
int ai_session_context(const struct jbuf *history, size_t limit, struct jbuf *context);
