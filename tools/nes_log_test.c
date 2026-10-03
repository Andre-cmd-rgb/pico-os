/* The real NES logger, including a legal ROM path longer than its buffer. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char logged[256];
static int messages;

static void klog(const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(logged, sizeof(logged), format, ap);
	va_end(ap);
	assert(n >= 0 && n < (int)sizeof(logged));
	messages++;
}

#include "nes_log_under_test.c"

int main(void)
{
	char path[241], wanted[165], boundary[161];

	rg_system_log(0, NULL, "NES: System initialized!\r\n");
	assert(messages == 1 && !strcmp(logged, "nes: NES: System initialized!"));
	rg_system_log(0, NULL, "");
	rg_system_log(0, NULL, "\r\n\r\n");
	assert(messages == 1);
	memset(path, 'a', sizeof(path) - 1);
	memcpy(path, "/sdcard/", 8);
	memcpy(path + sizeof(path) - 5, ".nes", 4);
	path[sizeof(path) - 1] = '\0';
	rg_system_log(0, NULL, "ROM: Loading file '%s'\n", path);
	assert(messages == 2 && strlen(logged) == 164);
	snprintf(wanted, sizeof(wanted), "nes: ROM: Loading file '%.*s", 140, path);
	assert(!strcmp(logged, wanted));
	memset(boundary, 'b', sizeof(boundary));
	boundary[158] = '\n';
	boundary[159] = '\0';
	rg_system_log(0, NULL, "%s", boundary);
	assert(messages == 3 && strlen(logged) == 163 && logged[162] == 'b');
	boundary[159] = '\n';
	boundary[160] = '\0';
	rg_system_log(0, NULL, "%s", boundary);
	assert(messages == 4 && strlen(logged) == 163 && logged[162] == 'b');
	puts("nes log: long ROM paths, truncation and newline boundaries passed");
	return 0;
}
