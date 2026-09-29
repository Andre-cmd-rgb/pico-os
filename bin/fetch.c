/*
 * picofetch: the machine at a glance, the way fastfetch shows a PC -- a
 * picture of it on the left, what it is and how it is doing on the right,
 * and the theme's colours underneath. Everything is read from where the
 * other commands read it (uname, free, df, battery, wifi, theme), and it
 * fits the 53 columns.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "pt/kernel.h"
#include "pt/program.h"
#include "util.h"

#define LOGO_W		13

/* The pocket computer itself: its screen with a prompt, and its keys. */
static const char *const logo[] = {
	" ___________ ",
	"|  _______  |",
	"| |>_     | |",
	"| |_______| |",
	"|  o o o o  |",
	"|  o o o o  |",
	"|  o o o o  |",
	"|___________|",
};

#define LOGO_H	(int)(sizeof(logo) / sizeof(logo[0]))

struct fetch {
	int	row;
	int	width;
};

/* A line of the right-hand column, beside the logo's next line. */
static void line(struct fetch *f, const char *key, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void line(struct fetch *f, const char *key, const char *fmt, ...)
{
	char value[128];
	va_list ap;
	int room;

	va_start(ap, fmt);
	vsnprintf(value, sizeof(value), fmt, ap);
	va_end(ap);
	pt_printf("\x1b[1;33m%s\x1b[0m  ", f->row < LOGO_H ? logo[f->row] : "             ");
	room = f->width - LOGO_W - 2;
	if (key) {
		room -= (int)strlen(key) + 2;
		pt_printf("\x1b[1;33m%s\x1b[0m: %.*s\n", key, room > 0 ? (int)utf8_prefix(value,
			  strlen(value), room) : 0, value);
	} else {
		pt_printf("%.*s\n", (int)utf8_prefix(value, strlen(value), room), value);
	}
	f->row++;
}

static void size_of(uint64_t bytes, char *out, size_t n)
{
	if (bytes >= 10ULL << 30)
		snprintf(out, n, "%llu GB", (unsigned long long)(bytes >> 30));
	else if (bytes >= 10ULL << 20)
		snprintf(out, n, "%llu MB", (unsigned long long)(bytes >> 20));
	else
		snprintf(out, n, "%llu KB", (unsigned long long)(bytes >> 10));
}

static void disk(struct fetch *f, const char *key, const char *path)
{
	for (int i = 0; i < mount_count(); i++) {
		const struct pt_mount *m = mount_get(i);
		uint64_t total, free;
		char used_s[16], total_s[16];

		if (m->bind || strcmp(mount_path(m), path) || (m->present && !m->present()) ||
		    !m->info || m->info(&total, &free))
			continue;
		size_of(total - free, used_s, sizeof(used_s));
		size_of(total, total_s, sizeof(total_s));
		line(f, key, "%s / %s (%d%%)", used_s, total_s,
		     total ? (int)((total - free) * 100 / total) : 0);
		return;
	}
}

static int built_in(void)
{
	int n = 0;

	for (const struct pt_program *p = program_first(); p; p = p->next)
		n++;
	return n;
}

static int programs_in(const char *dir)
{
	struct pt_dirent ent;
	pt_dir_t *d;
	int n = 0;

	if (pt_opendir(dir, &d))
		return 0;
	while (pt_readdir(d, &ent) == 1)
		n += !ent.is_dir && ent.name[0] != '.';
	pt_closedir(d);
	return n;
}

PT_PROGRAM(picofetch, "the machine at a glance, as fastfetch shows a PC\n"
	   "usage: picofetch")
{
	const esp_app_desc_t *app = esp_app_get_description();
	const char *host = pt_getenv("HOSTNAME") ? pt_getenv("HOSTNAME") : "pockettype";
	const char *user = pt_getenv("USER") ? pt_getenv("USER") : user_name();
	struct pt_winsize ws = { 53, 23 };
	struct fetch f = { 0 };
	struct theme_state th;
	struct battery_status bat;
	struct wifi_info net;
	multi_heap_info_t in, ps;
	esp_chip_info_t chip;
	char bin[PT_PATH_MAX], rule[40];
	unsigned long m = pt_uptime_us() / 60000000;
	int lo, hi, n;

	if (argc > 1) {
		pt_dprintf(PT_STDERR, "usage: picofetch\n");
		return 2;
	}
	pt_ioctl(PT_STDOUT, PT_TTY_GETSIZE, &ws);
	f.width = ws.cols ? ws.cols : 53;

	n = snprintf(rule, sizeof(rule), "%s@%s", user, host);
	/* printed whole: its colours would count as width in line() */
	pt_printf("\x1b[1;33m%s\x1b[0m  \x1b[1;33m%s\x1b[0m@\x1b[1;33m%s\x1b[0m\n", logo[f.row++],
		  user, host);
	memset(rule, '-', n < (int)sizeof(rule) ? n : (int)sizeof(rule) - 1);
	rule[n < (int)sizeof(rule) ? n : (int)sizeof(rule) - 1] = '\0';
	line(&f, NULL, "%s", rule);
	line(&f, "OS", "PocketType %s", PT_VERSION);
	line(&f, "Kernel", "ESP-IDF %s", app->idf_ver);
	if (m < 60)
		line(&f, "Uptime", "%lu min", m);
	else
		line(&f, "Uptime", "%lu h %lu min", m / 60, m % 60);
	line(&f, "Programs", "%d built in, %d in ~/bin", built_in(),
	     programs_in(home_dir(bin, sizeof(bin), "bin")));
	line(&f, "Shell", "sh");
	line(&f, "Display", "%dx%d, ILI9341", lcd_width(), lcd_height());
	line(&f, "Terminal", "vt%d, %dx%d", vt_active() + 1, ws.cols, ws.rows);
	theme_get(&th);
	line(&f, "Theme", "%s (%s)", th.name, th.light ? "light" : "dark");
	esp_chip_info(&chip);
	cpufreq_get(&lo, &hi);
	line(&f, "CPU", "%s, %d cores, %d-%d MHz",
#if CONFIG_IDF_TARGET_ESP32P4
	     "ESP32-P4",
#else
	     "ESP32-S3",
#endif
	     chip.cores, lo, hi);
	heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
	heap_caps_get_info(&ps, MALLOC_CAP_SPIRAM);
	line(&f, "Memory", "%zu/%zu KB, PSRAM %.1f/%.1f MB", in.total_allocated_bytes / 1024,
	     (in.total_allocated_bytes + in.total_free_bytes) / 1024,
	     ps.total_allocated_bytes / 1048576.0,
	     (ps.total_allocated_bytes + ps.total_free_bytes) / 1048576.0);
	disk(&f, "Disk (/)", "/");
	disk(&f, "Disk (~)", "/mnt/sd");
	if (!battery_status(&bat) && bat.state != BATTERY_NONE && bat.state != BATTERY_USB)
		line(&f, "Battery", "%d%%, %s", bat.percent, battery_state_name(bat.state));
	if (!wifi_state(&net) && net.up) {
		line(&f, "Wi-Fi", "%s", net.ssid);
		line(&f, "Local IP", "%s", net.ip);
	}
	line(&f, NULL, "%s", "");
	/* the theme's sixteen colours, as fastfetch shows a terminal's */
	for (int row = 0; row < 2; row++) {
		char blocks[160];
		int k = 0;

		for (int c = 0; c < 8; c++)
			k += snprintf(blocks + k, sizeof(blocks) - k, "\x1b[%dm   ",
				      (row ? 100 : 40) + c);
		snprintf(blocks + k, sizeof(blocks) - k, "\x1b[0m");
		pt_printf("\x1b[1;33m%s\x1b[0m  %s\n", f.row < LOGO_H ? logo[f.row] : "             ",
			  blocks);
		f.row++;
	}
	while (f.row < LOGO_H)
		line(&f, NULL, "%s", "");
	return 0;
}
