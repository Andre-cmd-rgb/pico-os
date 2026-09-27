/*
 * Entry points for the port layers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int	pico_main_compile(int argc, char **argv);
int	pico_main_run(int argc, char **argv);
bool	pico_probe(const uint8_t *head, size_t n);
int	pico_exec(const char *path, int argc, char **argv);
