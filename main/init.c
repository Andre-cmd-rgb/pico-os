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
#define VT_STACK	4608	/* a terminal's task, which starts and waits for its shell */

static const char motd[] =
	" \x1b[2mtype 'help' for the commands, 'help <cmd>' for one\x1b[0m\n\n";

static void banner(void)
{
	const esp_app_desc_t *app = esp_app_get_description();

	klog(PT_OS_NAME " %s (esp-idf %s) #1 SMP %s %s", PT_VERSION, app->idf_ver, app->date, app->time);
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
 * The name, on a clean screen: the boot log has scrolled past while the
 * system came up, and `dmesg` keeps it for whoever wants it.
 */
static void logo(void)
{
	char line[96];

	vt_write("\x1b[2J\x1b[H\n", 8);
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

/*
 * Whether /etc/motd is an old one of ours -- the hint about help, worded
 * as it used to be -- rather than the current one or somebody's own.
 */
static bool motd_is_ours(const char *path)
{
	char text[160];
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return false;
	n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = '\0';
	return n < sizeof(text) - 1 && strstr(text, "type 'help'") && strcmp(text, motd);
}

/*
 * The screen as it was left: the other way up if `rotate` said so
 * (/etc/rotate), in the colours `theme` chose (/etc/theme). Before the
 * first thing is drawn, so the boot log already looks that way.
 */
static void screen_restore(void)
{
	char buf[VFS_PATH_MAX], line[8] = "";
	FILE *f = vfs("/etc/rotate", buf) ? fopen(buf, "r") : NULL;

	if (f) {
		fgets(line, sizeof(line), f);
		fclose(f);
		if (atoi(line) == 180 && !lcd_set_rotation(lcd_rotation() ^ 2))
			klog("lcd: turned 180 degrees (/etc/rotate)");
	}
	theme_restore();
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
	strlcpy(path, user_home(), sizeof(path));
	if (vfs(path, buf))
		mkdir(buf, 0777);

	if (vfs("/etc/motd", buf) && (stat(buf, &st) || motd_is_ours(buf))) {
		FILE *f = fopen(buf, "w");	/* missing, or ours from an older version */

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
		/* the hint about help is under the logo, on the first one only */
		char *login[] = { "sh", "-l", "-q", NULL };
		int status = run_console_on(vt, 3, login);

		if (status < 0)
			break;			/* no memory for another one */
		vTaskDelay(pdMS_TO_TICKS(500));
	}
	vt_task[vt] = NULL;
	ktask_exit();
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
	ktask_create(vt_shell_task, name, VT_STACK, (void *)(intptr_t)vt, 5, &vt_task[vt], 0);
}

static bool root;		/* / is there: the user and /etc/rc can be read */

/*
 * The first terminal's: the first start's questions, /etc/rc, then a
 * login shell for as long as the system runs. A task of its own with a
 * stack in PSRAM, like the other terminals', so that app_main can return
 * and give the 8 KB of internal RAM the boot needed back to the heap.
 */
static void console_task(void *arg)
{
	struct pt_stat st;

	/* the first start: a name, the time zone, Wi-Fi, before any shell */
	if (root && !user_configured()) {
		char *setup[] = { "setup", "-f", NULL };
		run_console(2, setup);
	}
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

void app_main(void)
{
	klog_init();
	file_init();
	/* before any kernel task: their stacks are in PSRAM, and kflash is
	 * where their flash calls go */
	internal_init();
	setenv("TZ", CONFIG_PT_TZ, 1);
	tzset();
	banner();
	power_boot_reason();
	memtest_quick();
	led_init();
	serial_console_init();
	proc_init();
	cpufreq_init();
	wifi_init();

	/* / first: it says which way up the screen goes and in what colours */
	root = !rootfs_init();

	if (root) {
		user_restore();	/* the home directory's name, the time zone */
		root_layout();	/* mount points must exist before mounting on them */
		clock_restore();
	}
#if CONFIG_PT_LCD
	int lcd_error = lcd_init();

	if (lcd_error)
		klog("init: display initialization failed (%s); continuing on serial", strerror(-lcd_error));
#endif
	vt_init();
	if (root)
		screen_restore();
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
	battery_init();		/* with what /etc/battery knows of the cell */
	tmpfs_init();
	sd_init();
	/* An erased / and a card with a copy of /etc: everything above read
	 * the empty one, so start again with the settings in place. */
	if (root && etc_init() > 0)
		restart_now();
	wifi_start_supplicant();	/* now that /etc/wifi can be read */
	services_start();		/* the applications' own (PT_SERVICE) */
	alarm_init();			/* and /etc/alarms */
	idle_init();			/* and /etc/power */
	netconsole_init();
	modem_init();
	ble_init();			/* the radio itself waits to be asked for */
	klog("init: %d programs, starting shell", count_programs());

	/* From here on kernel messages go to dmesg and the serial port only,
	 * so they never scribble over the shell on screen. */
	klog_set_console(tty_mirror_output);
	led_set_mode(LED_CHARGE);

	logo();
	if (ktask_create(console_task, "kvt0", VT_STACK, NULL, 5, &vt_task[0], 0) != pdPASS)
		console_task(NULL);	/* no room for a task: on this one, then */
}
