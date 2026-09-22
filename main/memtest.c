/*
 * Quick PSRAM check at boot, like a PC's POST.
 *
 * Two passes over the largest free block:
 *   - a sparse address test: every 97th word gets a value derived from its
 *     own index, then all are read back. A missing chip, a wrong size or a
 *     shorted or open address line shows up as mismatches.
 *   - a dense test of the first 64 KB with four bit patterns, which catches
 *     stuck or bridged data lines.
 * It does not look for single bad cells; that would take seconds.
 *
 * PSRAM sits behind the 64 KB data cache, so after writing, the cache is
 * written back and emptied: otherwise the reads would check the cache.
 */
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "memtest.h"
#include "pt/kernel.h"

#define STRIDE		97
#define DENSE_WORDS	16384

static inline uint32_t tag(size_t i)
{
	return (uint32_t)i * 2654435761u ^ 0x5a5aa5a5;
}

static void to_chip(void *p, size_t n)
{
	esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE |
			      ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void memtest_quick(void)
{
	static const uint32_t patterns[] = { 0x00000000, 0xffffffff, 0xaaaaaaaa, 0x55555555 };
	int64_t start = esp_timer_get_time();
	size_t bytes = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) & ~3u;
	uint32_t *mem = bytes ? heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM) : NULL;
	size_t words = bytes / 4, bad = 0;

	if (!mem) {
		klog("memtest: no PSRAM to test");
		return;
	}
	for (size_t i = 0; i < words; i += STRIDE)
		mem[i] = tag(i);
	to_chip(mem, bytes);
	for (size_t i = 0; i < words; i += STRIDE)
		bad += mem[i] != tag(i);

	size_t dense = words < DENSE_WORDS ? words : DENSE_WORDS;
	for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
		for (size_t i = 0; i < dense; i++)
			mem[i] = patterns[p];
		to_chip(mem, dense * 4);
		for (size_t i = 0; i < dense; i++)
			bad += mem[i] != patterns[p];
	}
	heap_caps_free(mem);

	int64_t ms = (esp_timer_get_time() - start) / 1000;
	if (bad)
		klog("memtest: PSRAM FAILED, %zu bad words in %zu KB (%lld ms)", bad, bytes / 1024, ms);
	else
		klog("memtest: %zu KB PSRAM ok (%lld ms)", bytes / 1024, ms);
}
