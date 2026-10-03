/* The model list is read a record at a time, without keeping its descriptions. */
#pragma once

#include "json.h"

struct ai_model {
	char id[128], label[192];
	unsigned efforts;
	bool reasoning;
};

struct ai_catalog {
	struct ai_model *models;
	int count, capacity, depth, probe;
	bool array, quoted, escaped, skip;
	struct jbuf record;
};

bool ai_catalog_feed(struct ai_catalog *catalog, const char *text, size_t n);
void ai_catalog_free(struct ai_catalog *catalog);
int ai_model_match(const struct ai_catalog *catalog, const char *name, int *matches, int size);
extern const char *const ai_efforts[8];
int ai_effort_index(const char *name);
