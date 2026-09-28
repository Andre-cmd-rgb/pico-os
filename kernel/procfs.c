/*
 * /proc: small text files generated when opened, plus whatever a driver
 * adds at boot (proc_register), the way a network driver adds /proc/net.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_clk.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "pt/kernel.h"

#define GEN_SIZE 1024

typedef int (*gen_fn)(char *buf, size_t size);

static int gen_version(char *b, size_t n)
{
	const esp_app_desc_t *app = esp_app_get_description();

	return snprintf(b, n, "PocketType version %s (esp-idf %s) #1 SMP %s %s\n",
			PT_VERSION, app->idf_ver, app->date, app->time);
}

static int gen_uptime(char *b, size_t n)
{
	int64_t us = esp_timer_get_time();

	return snprintf(b, n, "%lld.%02lld\n", us / 1000000, (us % 1000000) / 10000);
}

static int gen_meminfo(char *b, size_t n)
{
	multi_heap_info_t in, ps;

	heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
	heap_caps_get_info(&ps, MALLOC_CAP_SPIRAM);
	size_t in_total = in.total_free_bytes + in.total_allocated_bytes;
	size_t ps_total = ps.total_free_bytes + ps.total_allocated_bytes;

	return snprintf(b, n,
			"MemTotal:        %8zu kB\n"
			"MemFree:         %8zu kB\n"
			"InternalTotal:   %8zu kB\n"
			"InternalFree:    %8zu kB\n"
			"InternalLargest: %8zu kB\n"
			"InternalMinFree: %8zu kB\n"
			"PsramTotal:      %8zu kB\n"
			"PsramFree:       %8zu kB\n"
			"PsramLargest:    %8zu kB\n",
			(in_total + ps_total) / 1024, (in.total_free_bytes + ps.total_free_bytes) / 1024,
			in_total / 1024, in.total_free_bytes / 1024, in.largest_free_block / 1024,
			in.minimum_free_bytes / 1024,
			ps_total / 1024, ps.total_free_bytes / 1024, ps.largest_free_block / 1024);
}

#if CONFIG_IDF_TARGET_ESP32P4
#define CPU_MODEL	"ESP32-P4 (RISC-V)"
#else
#define CPU_MODEL	"ESP32-S3 (Xtensa LX7)"
#endif

static int gen_cpuinfo(char *b, size_t n)
{
	esp_chip_info_t chip;
	uint32_t flash = 0;

	esp_chip_info(&chip);
	esp_flash_get_size(NULL, &flash);
	return snprintf(b, n,
			"model name : " CPU_MODEL "\n"
			"cores      : %d\n"
			"revision   : v%d.%d\n"
			"cpu MHz    : %d\n"
			"flash      : %lu MB\n"
			"psram      : %zu MB\n"
			"features   :%s%s\n",
			chip.cores, chip.revision / 100, chip.revision % 100,
			esp_clk_cpu_freq() / 1000000, (unsigned long)(flash >> 20),
			esp_psram_get_size() >> 20,
			chip.features & CHIP_FEATURE_WIFI_BGN ? " wifi" : "",
			chip.features & CHIP_FEATURE_BLE ? " ble" : "");
}

static int gen_mounts(char *b, size_t n)
{
	int len = 0;

	for (int i = 0; i < mount_count() && len < (int)n; i++) {
		const struct pt_mount *m = mount_get(i);
		if (!m->present || m->present())
			len += snprintf(b + len, n - len, "%s %s %s rw%s 0 0\n",
					m->source, mount_path(m), m->type, m->bind ? ",bind" : "");
	}
	if (len < (int)n)
		len += snprintf(b + len, n - len, "devfs /dev devfs rw 0 0\nproc /proc proc ro 0 0\n");
	return len;
}

static const struct {
	const char *name;
	gen_fn	    gen;
} entries[] = {
	{ "cpuinfo", gen_cpuinfo },
	{ "kmsg", NULL },		/* the whole kernel log */
	{ "meminfo", gen_meminfo },
	{ "mounts", gen_mounts },
	{ "uptime", gen_uptime },
	{ "version", gen_version },
};

#define N_ENTRIES (sizeof(entries) / sizeof(entries[0]))
#define N_EXTRA	  4

static struct {
	const char *name;
	gen_fn	    gen;
} extra[N_EXTRA];

static int nextra;

int proc_register(const char *name, int (*gen)(char *buf, size_t size))
{
	if (nextra == N_EXTRA)
		return -ENOSPC;
	extra[nextra].name = name;
	extra[nextra].gen = gen;
	nextra++;
	return 0;
}

static const char *entry_name(int i)
{
	return i < (int)N_ENTRIES ? entries[i].name : extra[i - N_ENTRIES].name;
}

static gen_fn entry_gen(int i)
{
	return i < (int)N_ENTRIES ? entries[i].gen : extra[i - N_ENTRIES].gen;
}

static int entry_index(const char *entry)
{
	for (int i = 0; i < (int)N_ENTRIES + nextra; i++)
		if (!strcmp(entry, entry_name(i)))
			return i;
	return -1;
}

static bool proc_lookup(const char *entry)
{
	return entry_index(entry) >= 0;
}

static int proc_readdir(int index, struct pt_dirent *ent)
{
	if (index < 0 || index >= (int)N_ENTRIES + nextra)
		return 0;
	strlcpy(ent->name, entry_name(index), sizeof(ent->name));
	ent->is_dir = false;
	return 1;
}

static struct pt_file *proc_open(const char *entry, int *err)
{
	int i = entry_index(entry);
	gen_fn gen = entry_gen(i);
	size_t size = gen ? GEN_SIZE : klog_size();
	char *buf = malloc(size ? size : 1);

	if (!buf) {
		*err = -ENOMEM;
		return NULL;
	}
	int len = gen ? gen(buf, size) : (int)klog_read(0, buf, size);
	if (len < 0)
		len = 0;
	if (len > (int)size)
		len = size;
	struct pt_file *f = mem_file_open(buf, len);
	if (!f)
		*err = -ENOMEM;
	return f;
}

const struct pt_kernfs procfs = {
	.name = "proc",
	.lookup = proc_lookup,
	.open = proc_open,
	.readdir = proc_readdir,
};
