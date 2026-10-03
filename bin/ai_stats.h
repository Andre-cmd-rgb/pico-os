/* Provider-reported usage; streamed characters are never counted as tokens. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct ai_usage {
	uint64_t input, output, reasoning, cached;
	double cost;
	bool known, cost_known;
};

void ai_usage_read(struct ai_usage *usage, const char *json, const char *end);
void ai_usage_add(struct ai_usage *total, const struct ai_usage *usage);
double ai_usage_tps(const struct ai_usage *usage, int64_t elapsed_us);
