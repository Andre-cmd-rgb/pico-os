#include <math.h>

#include "ai_stats.h"
#include "json.h"

static bool tokens(const char *p, const char *end, const char *path, uint64_t *out)
{
	const char *v, *ve;
	double n;

	v = json_get(p, end, path, &ve);
	n = v ? json_num(v, ve, -1) : -1;
	if (!isfinite(n) || n < 0 || n >= 9007199254740992.0 || n != floor(n))
		return false;
	*out = (uint64_t)n;
	return true;
}

void ai_usage_read(struct ai_usage *u, const char *p, const char *end)
{
	const char *v, *ve;
	double cost;
	uint64_t input, output;

	if (!json_get(p, end, "usage", &ve))
		return;
	if (tokens(p, end, "usage.prompt_tokens", &input) &&
	    tokens(p, end, "usage.completion_tokens", &output)) {
		u->input = input;
		u->output = output;
		tokens(p, end, "usage.completion_tokens_details.reasoning_tokens", &u->reasoning);
		tokens(p, end, "usage.prompt_tokens_details.cached_tokens", &u->cached);
		u->known = true;
	}
	v = json_get(p, end, "usage.cost", &ve);
	if (!v)
		return;
	cost = json_num(v, ve, -1);
	u->cost_known = isfinite(cost) && cost >= 0;
	u->cost = u->cost_known ? cost : 0;
}

void ai_usage_add(struct ai_usage *total, const struct ai_usage *u)
{
	total->input += u->input;
	total->output += u->output;
	total->reasoning += u->reasoning;
	total->cached += u->cached;
	total->cost += u->cost;
	total->known |= u->known;
	total->cost_known |= u->cost_known;
}

double ai_usage_tps(const struct ai_usage *u, int64_t elapsed_us)
{
	return u->known && elapsed_us > 0 ? (double)u->output * 1000000 / elapsed_us : 0;
}
