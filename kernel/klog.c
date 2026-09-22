/*
 * Kernel log: a ring buffer read by dmesg and /proc/kmsg, echoed to the
 * console. ESP-IDF's own ESP_LOG output is routed through here too, so a
 * driver warning from inside the IDF shows up in dmesg like any other line.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "pt/kernel.h"

static char	*ring;
static size_t	 ring_head;	/* next write */
static size_t	 ring_used;
static portMUX_TYPE ring_lock = portMUX_INITIALIZER_UNLOCKED;

static void stdout_console(const char *s, size_t n)
{
	fwrite(s, 1, n, stdout);
	fflush(stdout);
}

static void (*console)(const char *s, size_t n) = stdout_console;
static TaskHandle_t console_busy;	/* guards against a console that logs */

static void ring_store(const char *s, size_t n)
{
	const size_t cap = CONFIG_PT_KLOG_SIZE;

	if (!ring)
		return;
	if (n > cap) {
		s += n - cap;
		n = cap;
	}
	portENTER_CRITICAL(&ring_lock);
	size_t first = cap - ring_head < n ? cap - ring_head : n;
	memcpy(ring + ring_head, s, first);
	memcpy(ring, s + first, n - first);
	ring_head = (ring_head + n) % cap;
	ring_used = ring_used + n > cap ? cap : ring_used + n;
	portEXIT_CRITICAL(&ring_lock);
}

static void emit(const char *msg, size_t len)
{
	char line[256];
	int64_t us = esp_timer_get_time();
	/* Milliseconds, not microseconds, and three columns for the
	 * seconds: a boot log needs no more, and the screen is 53 wide. */
	int n = snprintf(line, sizeof(line), "[%3lu.%03lu] %.*s\n",
			 (unsigned long)(us / 1000000), (unsigned long)(us % 1000000 / 1000),
			 (int)len, msg);

	if (n < 0)
		return;
	if (n >= (int)sizeof(line))
		n = sizeof(line) - 1;
	ring_store(line, n);

	TaskHandle_t self = xTaskGetCurrentTaskHandle();
	if (console_busy == self)
		return;
	console_busy = self;
	console(line, n);
	console_busy = NULL;
}

void klog(const char *fmt, ...)
{
	char msg[200];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	emit(msg, n < (int)sizeof(msg) ? n : sizeof(msg) - 1);
}

/* ESP_LOG lines look like "W (1234) tag: text\n". Keep "tag: text". */
static int idf_vprintf(const char *fmt, va_list ap)
{
	char msg[200];
	int n = vsnprintf(msg, sizeof(msg), fmt, ap);

	if (n <= 0)
		return n;
	size_t len = n < (int)sizeof(msg) ? n : sizeof(msg) - 1;
	char *s = msg;

	if (len > 4 && s[1] == ' ' && s[2] == '(') {
		char *close = memchr(s, ')', len);
		if (close && close + 1 < s + len && close[1] == ' ') {
			len -= close + 2 - s;
			s = close + 2;
		}
	}
	while (len && (s[len - 1] == '\n' || s[len - 1] == '\r'))
		len--;
	if (len)
		emit(s, len);
	return n;
}

void klog_init(void)
{
	ring = heap_caps_malloc(CONFIG_PT_KLOG_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!ring)
		ring = heap_caps_malloc(CONFIG_PT_KLOG_SIZE, MALLOC_CAP_8BIT);
	esp_log_set_vprintf(idf_vprintf);
	esp_log_level_set("*", ESP_LOG_WARN);
}

void klog_set_console(void (*write)(const char *s, size_t n))
{
	console = write ? write : stdout_console;
}

size_t klog_size(void)
{
	return ring_used;
}

/* At most two copies: interrupts are off on this core while the lock is held. */
size_t klog_read(size_t off, char *buf, size_t n)
{
	const size_t cap = CONFIG_PT_KLOG_SIZE;
	size_t got = 0;

	if (!ring)
		return 0;
	portENTER_CRITICAL(&ring_lock);
	if (off < ring_used) {
		size_t from = (ring_head + cap - ring_used + off) % cap;
		size_t first;

		got = ring_used - off < n ? ring_used - off : n;
		first = cap - from < got ? cap - from : got;
		memcpy(buf, ring + from, first);
		memcpy(buf + first, ring, got - first);
	}
	portEXIT_CRITICAL(&ring_lock);
	return got;
}

void klog_clear(void)
{
	portENTER_CRITICAL(&ring_lock);
	ring_used = 0;
	portEXIT_CRITICAL(&ring_lock);
}
