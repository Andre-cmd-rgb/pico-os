#pragma once

#include <stdbool.h>
#include <stddef.h>

bool ai_path_within(const char *path, const char *root);
bool ai_path_is_note(const char *path, const char *notes, const char *home, bool sd);
bool ai_path_resolve(const char *root, const char *notes, const char *home,
		     const char *given, char *out, size_t size);
