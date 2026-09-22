/*
 * Entry points for the port layers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int	al_main_ac(int argc, char **argv);
int	al_main_a(int argc, char **argv);
bool	al_probe(const uint8_t *head, size_t n);
int	al_exec(const char *path, int argc, char **argv);
