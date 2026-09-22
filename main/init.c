/*
 * Boot. Brings subsystems up in dependency order, the way start_kernel()
 * does, then runs /etc/rc and keeps a login shell on the console.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "memtest.h"
#include "pt/kernel.h"
#include "pt/program.h"

#define VFS_PATH_MAX	(PT_PATH_MAX + 16)

static const char motd[] =
	" \x1b[2mtype 'help' for a list of commands, 'help <cmd>' for one\x1b[0m\n\n";

static void banner(void)
{
	const esp_app_desc_t *app = esp_app_get_description();

	klog("PocketType %s (esp-idf %s) #1 SMP %s %s", PT_VERSION, app->idf_ver, app->date, app->time);
	klog("mem: %zu KB internal, %zu KB psram available",
	     heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
	     heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

static const char *const art[] = {
	"  ____            _        _ _____",
	" |  _ \\ ___   ___| | _____| |_   _|   _ _ __   ___",
	" | |_) / _ \\ / __| |/ / _ \\ __|| || | | | '_ \\ / _ \\",
	" |  __/ (_) | (__|   <  __/ |_ | || |_| | |_) |  __/",
	" |_|   \\___/ \\___|_|\\_\\___|\\__||_| \\__, | .__/ \\___|",
	"                                    |___/|_|",
};

#define ART_LINES	(sizeof(art) / sizeof(art[0]))

/*
 * The name, printed under whatever the kernel has just said. Booting
 * takes well under a second, so the log is a handful of lines rather
 * than a wall, and leaving it on screen is more use than hiding it.
 */
static void logo(void)
{
	char line[96];

	vt_write("\n", 1);
	for (size_t i = 0; i < ART_LINES; i++) {
		int n = snprintf(line, sizeof(line), "%s\n", art[i]);

		vt_write(line, n);
	}
	vt_write("\n", 1);
}

/* The screen starts late; show it the kernel messages it missed. */
static void replay_log(void (*write)(const char *s, size_t n))
{
	char buf[256];
	size_t off = 0, n;

	while ((n = klog_read(off, buf, sizeof(buf))) > 0) {
		write(buf, n);
		off += n;
	}
}

/* Where `path` lives on its filesystem. Boot code runs before any process
 * exists, and mount points must be created even though /dev and /proc are
 * off limits through the system calls. */
static const char *vfs(const char *path, char *buf)
{
	return mount_resolve(path, buf, VFS_PATH_MAX) ? buf : NULL;
}

/* Directories every system has, mount points included, as on Linux. */
static void root_layout(void)
{
	static const char *const dirs[] = {
		"/bin", "/dev", "/etc", "/home", "/mnt", "/mnt/sd", "/proc", "/tmp",
	};
	char buf[VFS_PATH_MAX], path[PT_PATH_MAX];
	struct stat st;

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (vfs(dirs[i], buf))
			mkdir(buf, 0777);
	snprintf(path, sizeof(path), "/home/%s", CONFIG_PT_USERNAME);
	if (vfs(path, buf))
		mkdir(buf, 0777);

	if (vfs("/etc/motd", buf) && (stat(buf, &st) || st.st_size > (off_t)sizeof(motd))) {
		FILE *f = fopen(buf, "w");	/* missing, or from an older version */

		if (f) {
			fputs(motd, f);
			fclose(f);
			klog("init: wrote /etc/motd");
		}
	}
}

static int count_programs(void)
{
	int n = 0;

	for (const struct pt_program *p = program_first(); p; p = p->next)
		n++;
	return n;
}

/*
 * A shell per terminal, started when its terminal is first switched to
 * and restarted whenever it exits -- init's oldest job, once per screen.
 */
static TaskHandle_t vt_task[CONFIG_PT_VT_COUNT];

static int run_console_on(int vt, int argc, char **argv)
{
	int pid = proc_spawn_console(argc, argv, tty_console(vt));

	if (pid < 0) {
		klog("init: cannot start %s (%s)", argv[0], strerror(-pid));
		return pid;
	}
	return proc_wait_orphan(pid);
}

static int run_console(int argc, char **argv)
{
	return run_console_on(0, argc, argv);
}

static void vt_shell_task(void *arg)
{
	int vt = (int)(intptr_t)arg;

	for (;;) {
		char *login[] = { "sh", "-l", NULL };
		int status = run_console_on(vt, 2, login);

		if (status < 0)
			break;			/* no memory for another one */
		vTaskDelay(pdMS_TO_TICKS(500));
	}
	vt_task[vt] = NULL;
	vTaskDelete(NULL);
}

/* Called when a terminal is switched to for the first time. */
static void vt_activated(int vt)
{
	char name[12];

	if (vt <= 0 || vt >= CONFIG_PT_VT_COUNT || vt_task[vt])
		return;
	snprintf(name, sizeof(name), "kvt%d", vt);
	/* proc_spawn_console() copies argv and the environment on this
	 * stack, so it needs more than a waiting task would suggest. */
	xTaskCreatePinnedToCore(vt_shell_task, name, 4608, (void *)(intptr_t)vt, 5,
				&vt_task[vt], 0);
}

void app_main(void)
{
	struct pt_stat st;

	klog_init();
	file_init();
	setenv("TZ", CONFIG_PT_TZ, 1);
	tzset();
	banner();
	power_boot_reason();
	memtest_quick();
	led_init();
	serial_console_init();
	proc_init();
	cpufreq_init();
	battery_init();
	wifi_init();

#if CONFIG_PT_LCD
	lcd_init();
#endif
	vt_init();
	tty_init();
	vt_start_display();
	replay_log(vt_write);
	klog_set_console(tty_output);

	audio_init();
	tty_set_activate_hook(vt_activated);
	button_init();
	cardkb_init();
	usbkbd_init();
	usbmsc_init();
	if (!rootfs_init())
		root_layout();	/* mount points must exist before mounting on them */
	tmpfs_init();
	sd_init();
	wifi_start_supplicant();	/* now that /etc/wifi can be read */
	netconsole_init();
	modem_init();
	klog("init: %d programs, starting shell", count_programs());

	/* From here on kernel messages go to dmesg and the serial port only,
	 * so they never scribble over the shell on screen. */
	klog_set_console(tty_mirror_output);
	led_set_mode(LED_HEARTBEAT);

	logo();
	if (!pt_stat("/etc/rc", &st) && !st.is_dir) {
		char *rc[] = { "sh", "/etc/rc", NULL };
		run_console(2, rc);
	}
	for (;;) {
		char *login[] = { "sh", "-l", NULL };
		int status = run_console(2, login);
		klog("init: shell exited (%d), restarting", status);
		vTaskDelay(pdMS_TO_TICKS(500));
	}
}
