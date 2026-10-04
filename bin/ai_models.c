#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_models.h"
#include "pt/sys.h"

#define MODELS_MAX 1024
#define RECORD_MAX (64 * 1024)

const char *const ai_efforts[8] = {
	"auto", "none", "minimal", "low", "medium", "high", "xhigh", "max",
};

int ai_effort_index(const char *name)
{
	for (int i = 0; i < 8; i++)
		if (!strcmp(name, ai_efforts[i]))
			return i;
	return -1;
}

static bool add_model(struct ai_catalog *c)
{
	const char *p = c->record.p, *end = p + c->record.len, *v, *ve;
	struct ai_model *m;
	char prompt[48] = "", completion[48] = "";
	double in, out;

	if (!(v = json_get(p, end, "id", &ve)))
		return true;
	if (json_get(p, end, "architecture.output_modalities", &ve) &&
	    !json_get(p, end, "architecture.output_modalities.0", &ve))
		return true;
	if ((v = json_get(p, end, "architecture.output_modalities", &ve))) {
		bool text = false;
		const char *item, *ie;
		int count = json_count(v, ve);

		for (int i = 0; i < count; i++) {
			char kind[24], index[16];

			snprintf(index, sizeof(index), "%d", i);
			item = json_get(v, ve, index, &ie);
			if (!item)
				continue;
			json_text(item, ie, kind, sizeof(kind));
			text |= !strcmp(kind, "text");
		}
		if (!text)
			return true;
	}
	if (c->count == MODELS_MAX)
		return true;
	if (c->count == c->capacity) {
		int cap = c->capacity ? c->capacity * 2 : 64;
		struct ai_model *models = pt_realloc(c->models, (size_t)cap * sizeof(*models));

		if (!models)
			return false;
		c->models = models;
		c->capacity = cap;
	}
	m = &c->models[c->count];
	memset(m, 0, sizeof(*m));
	v = json_get(p, end, "id", &ve);
	if (!v || json_text(v, ve, m->id, sizeof(m->id)) >= sizeof(m->id))
		return true;
	m->efforts = 1;		/* auto always leaves the choice to the provider */
	m->reasoning = json_get(p, end, "reasoning", &ve) != NULL;
	if ((v = json_get(p, end, "reasoning.supported_efforts", &ve))) {
		if (*v == '[') {
			const char *item, *ie;
			int count = json_count(v, ve);

			for (int i = 0; i < count; i++) {
				char effort[16], key[16];
				int index;

				snprintf(key, sizeof(key), "%d", i);
				item = json_get(v, ve, key, &ie);
				if (!item)
					continue;
				json_text(item, ie, effort, sizeof(effort));
				index = ai_effort_index(effort);
				if (index >= 0)
					m->efforts |= 1U << index;
			}
		} else if (!strncmp(v, "null", 4))
			m->efforts = 255;
	}
	if ((v = json_get(p, end, "reasoning.mandatory", &ve)) && !strncmp(v, "true", 4))
		m->efforts &= ~(1U << 1);
	if ((v = json_get(p, end, "pricing.prompt", &ve)))
		json_text(v, ve, prompt, sizeof(prompt));
	if ((v = json_get(p, end, "pricing.completion", &ve)))
		json_text(v, ve, completion, sizeof(completion));
	in = strtod(prompt, NULL) * 1000000;
	out = strtod(completion, NULL) * 1000000;
	if (in < 0 || out < 0)	/* a router: the price of whatever it picks */
		snprintf(m->label, sizeof(m->label), "varies  %s", m->id);
	else if (!in && !out)
		snprintf(m->label, sizeof(m->label), "free  %s", m->id);
	else
		snprintf(m->label, sizeof(m->label), "$%.3g/%.3g  %s", in, out, m->id);
	c->count++;
	return true;
}

bool ai_catalog_feed(struct ai_catalog *c, const char *text, size_t n)
{
	static const char key[] = "\"data\"";

	for (size_t i = 0; i < n; i++) {
		char ch = text[i];

		if (!c->array) {
			if (c->probe == (int)sizeof(key) - 1) {
				if (ch == '[')
					c->array = true;
			} else if (ch == key[c->probe])
				c->probe++;
			else
				c->probe = ch == key[0] ? 1 : 0;
			continue;
		}
		if (!c->depth) {
			if (ch != '{')
				continue;
			jb_free(&c->record);
			c->quoted = c->escaped = c->skip = false;
		}
		if (!c->skip) {
			jb_add(&c->record, &ch, 1);
			if (c->record.oom)
				return false;
			if (c->record.len > RECORD_MAX)
				c->skip = true;
		}
		if (c->quoted) {
			if (c->escaped)
				c->escaped = false;
			else if (ch == '\\')
				c->escaped = true;
			else if (ch == '"')
				c->quoted = false;
		} else if (ch == '"')
			c->quoted = true;
		else if (ch == '{')
			c->depth++;
		else if (ch == '}') {
			if (!--c->depth && !c->skip && !add_model(c))
				return false;
		}
	}
	return true;
}

void ai_catalog_free(struct ai_catalog *c)
{
	pt_free(c->models);
	jb_free(&c->record);
	memset(c, 0, sizeof(*c));
}

static void normalise(const char *s, char *out, size_t size)
{
	size_t n = 0;

	for (; *s && n + 1 < size; s++) {
		if ((*s == 'v' || *s == 'V') && isdigit((unsigned char)s[1]))
			continue;
		if (isalnum((unsigned char)*s))
			out[n++] = tolower((unsigned char)*s);
	}
	out[n] = '\0';
}

int ai_model_match(const struct ai_catalog *c, const char *name, int *matches, int size)
{
	char wanted[128], slug[128];
	int n = 0;
	bool exact = false;

	normalise(name, wanted, sizeof(wanted));
	if (!*wanted)
		return 0;
	for (int i = 0; i < c->count; i++) {
		const char *id = strchr(c->models[i].id, '/');
		bool same;

		normalise(id ? id + 1 : c->models[i].id, slug, sizeof(slug));
		same = !strcmp(slug, wanted);
		if (same && !exact) {
			n = 0;
			exact = true;
		}
		if ((same || (!exact && strstr(slug, wanted))) && n < size)
			matches[n++] = i;
	}
	return n;
}
