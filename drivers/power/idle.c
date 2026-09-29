/*
 * Idle: what the machine does when nobody is using it.
 *
 * The backlight is the largest draw on the board, larger than everything
 * else together, and a pocket computer spends most of its day lying
 * there lit for nobody. So, as a phone does:
 *
 *   after `dim` seconds without a key the screen dims to a quarter of its
 *   brightness (a warning, and still readable);
 *   after `blank` seconds it goes dark: the lamp out, the panel asleep,
 *   and nothing drawn until a key brings it back. That key is swallowed,
 *   since nobody can see what it would have done;
 *   after `suspend` seconds the system goes into deep sleep, if nothing
 *   is going on: every terminal at its shell prompt, no sound playing,
 *   no PC on the USB port. A program left open -- a note being read, a
 *   file being edited -- keeps it out of deep sleep, since waking is a
 *   fresh boot and would lose it; the screen still goes dark.
 *
 * A key on the CardKB (or a USB keyboard, or the button) is someone
 * there. Typing from the PC over the serial port keeps the system out
 * of deep sleep but does not light a dark screen. A program showing
 * moving pictures holds the screen on (power_keep_screen) for as long
 * as it plays; an alarm lights it to ring.
 *
 * Ctrl-A z dozes: the screen goes dark at once, the radio rests and the
 * chip light-sleeps between the keyboard's polls, with everything kept
 * in memory -- a key brings it all back where it was, in a moment, where
 * deep sleep (`suspend`) is a fresh boot. Alarms still ring.
 *
 * Nothing polls here: the task sleeps until the next of those moments
 * is due, and a key after a dark spell wakes it early.
 *
 * The times are in /etc/power, and `power dim|blank|suspend` sets them.
 * The brightness and the volume are kept there too, a few seconds after
 * they last changed (Fn 5 to Fn 9 change them, as a laptop's keys do), so
 * that holding a key down does not wear the flash.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "pt/kernel.h"

#define CONFIG_PATH	"/etc/power"
#define DIM_DIVIDE	4	/* dimmed is a quarter of the brightness */
#define DIM_MIN		5	/* but never darker than this, in percent */
#define QUIET_CPU_US	200000	/* what a process may use in the suspend wait and be idle */
#define SAVE_AFTER_S	3	/* levels written this long after the last change */
#define LEVEL_STEP	10	/* percent, a key */
#define LIGHT_MIN	5	/* Fn 5 dims to this, not to dark */
#define MAX_PROCS	CONFIG_PT_MAX_PROCS

static struct idle_times times = {
	.dim_s = CONFIG_PT_IDLE_DIM_S,
	.blank_s = CONFIG_PT_IDLE_BLANK_S,
	.suspend_s = CONFIG_PT_IDLE_SUSPEND_MIN * 60,
};

static TaskHandle_t	 task;
static volatile int64_t	 last_key;	/* someone at the keyboard */
static volatile int64_t	 last_any;	/* anything at all, the PC included */
static volatile int	 keep;		/* programs holding the screen on */
static volatile enum screen_state state = SCREEN_ON;
static volatile bool	 asked;		/* power_suspend_soon() */
static volatile bool	 doze_asked;	/* power_doze() */
static volatile bool	 woke_up;	/* a key since the doze began */
static bool		 dozing;
static bool		 slept_before;		/* light sleep as it was before the dark */
static volatile int64_t	 save_at;	/* /etc/power to be written then; 0: not */
static int		 unmuted = CONFIG_PT_AUDIO_VOLUME;	/* what Fn 9 goes back to */

/*
 * What the processes had used when the quiet began, to tell a program
 * working away with nobody watching (a download, a script) from one
 * waiting for a key.
 */
static struct {
	int	 pid;
	uint64_t cpu_us;
} seen[MAX_PROCS];
static int nseen;

static int64_t now_us(void)
{
	return esp_timer_get_time();
}

enum screen_state power_screen(void)
{
	return state;
}

int power_poll_ms(int ms)
{
	return proc_poll_ms(ms);
}

static int dim_level(void)
{
	int b = lcd_backlight_get() / DIM_DIVIDE;

	return b < DIM_MIN ? DIM_MIN : b;
}

/* On the idle task only, so the panel is never told two things at once. */
static void set_screen(enum screen_state want)
{
	enum screen_state was = state;

	if (want == was)
		return;
	if (want == SCREEN_OFF) {
		state = want;
		proc_set_quiet(true);
		vt_blank(true);			/* the renderer stops first */
		lcd_light(0);
		lcd_panel_power(false);
		/*
		 * Dark, the chip light-sleeps between the keyboard's polls, as
		 * a phone does with its screen off: everything is kept, and a
		 * key is answered as before. What must not sleep says so --
		 * sound playing (the I2S driver), a PC or a keyboard on USB --
		 * and the chip stays awake for as long as it does.
		 */
		slept_before = cpufreq_idle_sleep();
		if (!slept_before)
			cpufreq_set_idle_sleep(true);
		return;
	}
	if (was == SCREEN_OFF) {
		if (!slept_before)
			cpufreq_set_idle_sleep(false);
		proc_set_quiet(false);
		lcd_panel_power(true);
		vt_blank(false);		/* everything is painted again */
		wifi_retry_soon();		/* someone is back: look for the network */
	}
	state = want;
	lcd_light(want == SCREEN_DIM ? dim_level() : -1);
}

static void snapshot_procs(void)
{
	struct pt_procinfo *list = malloc(sizeof(*list) * MAX_PROCS);

	nseen = 0;
	if (!list)
		return;
	int n = proc_list(list, MAX_PROCS);

	for (int i = 0; i < n; i++) {
		seen[nseen].pid = list[i].pid;
		seen[nseen++].cpu_us = list[i].cpu_us;
	}
	free(list);
}

/*
 * Whether deep sleep would lose nothing: only the login shells are
 * there (their parent is the kernel), and none of them has been busy
 * since the quiet began -- a loop in a shell runs in the shell itself.
 */
static bool nothing_running(void)
{
	struct pt_procinfo *list = malloc(sizeof(*list) * MAX_PROCS);
	bool quiet = true;
	int n;

	if (!list)
		return false;
	n = proc_list(list, MAX_PROCS);
	for (int i = 0; i < n && quiet; i++) {
		const struct pt_procinfo *p = &list[i];
		bool known = false;

		if (p->ppid != 0 || p->state != 'R' || strcmp(p->name, "sh"))
			quiet = false;
		for (int k = 0; k < nseen && quiet; k++) {
			if (seen[k].pid == p->pid) {
				known = true;
				quiet = p->cpu_us - seen[k].cpu_us < QUIET_CPU_US;
			}
		}
		if (!known)
			quiet = false;		/* new since the quiet began */
	}
	free(list);
	return quiet;
}

/*
 * A clip holds the screen on only while it is the one on it: playing on
 * another terminal, nobody is watching it.
 */
static bool watched(void)
{
	return keep && vt_screen_front();
}

/*
 * A charger at work on the cell: the board stays up (dark, light-sleeping)
 * to follow the charge and see it finish, which asleep it cannot -- it
 * woke from a charge to a level read off the lifted voltage, and never
 * saw the end that makes it 100%.
 */
static bool charging(void)
{
	struct battery_status b;

	return !battery_status(&b) && b.charger &&
	       (b.state == BATTERY_CHARGING || b.state == BATTERY_FULL);
}

static bool may_suspend(void)
{
	if (keep || audio_busy() || alarm_ringing(NULL) || usb_serial_jtag_is_connected() ||
	    charging())
		return false;
	return nothing_running();
}

static int save_config(void);

/* Light sleep as it was asked for (`power sleep`), not as the dark has it now. */
static bool sleep_wanted(void)
{
	return state == SCREEN_OFF ? slept_before : cpufreq_idle_sleep();
}

/* `power sleep on|off`: while dark, what the screen coming back returns to. */
int power_set_sleep(bool on)
{
	if (state == SCREEN_OFF) {
		slept_before = on;
		return 0;
	}
	return cpufreq_set_idle_sleep(on);
}

static int64_t seconds(int s)
{
	return (int64_t)s * 1000000;
}

/*
 * A doze begins: dark at once (and so light-sleeping, set_screen()), the
 * radio resting.
 */
static void doze_begin(void)
{
	doze_asked = false;
	woke_up = false;
	dozing = true;
	wifi_rest();
	klog("idle: dozing; a key wakes it where it was");
}

/* A key: everything as it was, and a look for the network. */
static void doze_end(void)
{
	dozing = woke_up = false;
	wifi_retry_soon();
	klog("idle: awake from a doze");
}

static void idle_task(void *arg)
{
	bool snapped = false;

	for (;;) {
		int64_t now = now_us(), quiet = now - last_key, any = now - last_any;
		int64_t next = INT64_MAX;
		enum screen_state want = SCREEN_ON;

		if (doze_asked && !dozing)
			doze_begin();
		else if (dozing && woke_up)
			doze_end();
		if (watched() || alarm_ringing(NULL))
			quiet = any = 0;
		if ((dozing && !alarm_ringing(NULL)) ||
		    (times.blank_s && quiet >= seconds(times.blank_s)))
			want = SCREEN_OFF;
		else if (times.dim_s && quiet >= seconds(times.dim_s))
			want = SCREEN_DIM;
		set_screen(want);

		/* the next moment something is due */
		if (times.dim_s && quiet < seconds(times.dim_s))
			next = seconds(times.dim_s) - quiet;
		else if (times.blank_s && quiet < seconds(times.blank_s))
			next = seconds(times.blank_s) - quiet;

		/*
		 * Deep sleep, when the time has come: a minute's look at the
		 * processes first, and again every minute that one of them
		 * turns out to be busy.
		 */
		if (asked) {
			asked = false;
			klog("idle: suspending, as asked");
			power_suspend(0);
		}
		if (times.suspend_s) {
			int64_t wait = seconds(times.suspend_s) - (quiet < any ? quiet : any);

			if (wait > 0) {
				snapped = false;
			} else {
				if (snapped && may_suspend()) {
					klog("idle: nothing used for %d min; suspending",
					     times.suspend_s / 60);
					power_suspend(0);
				}
				snapshot_procs();
				snapped = true;
				wait = seconds(60);
			}
			next = wait < next ? wait : next;
		}
		if (save_at && now >= save_at) {
			save_at = 0;
			save_config();
		} else if (save_at && save_at - now < next) {
			next = save_at - now;
		}
		if (next == INT64_MAX)
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		else
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(next / 1000 + 20));
	}
}

void power_activity(void)
{
	int64_t now = now_us();

	last_key = last_any = now;
	if (task && state != SCREEN_ON)
		xTaskNotifyGive(task);
}

bool power_key(void)
{
	bool dark = state == SCREEN_OFF;

	if (dozing)
		woke_up = true;
	power_activity();
	if (dozing && task)
		xTaskNotifyGive(task);
	return !dark;
}

/* Ctrl-A z: dark now, everything kept (see the top of the file). */
void power_doze(void)
{
	doze_asked = true;
	if (task)
		xTaskNotifyGive(task);
}

/*
 * For a program about to draw on the panel itself, or take a picture of
 * it: light it, and wait until it is lit (the panel takes 120 ms).
 */
void power_screen_wake(void)
{
	power_activity();
	for (int i = 0; i < 50 && state == SCREEN_OFF; i++)
		vTaskDelay(pdMS_TO_TICKS(10));
}

void power_remote_activity(void)
{
	last_any = now_us();
}

/*
 * Deep sleep, asked for from a driver's task (a key combination): the
 * keyboard's own task cannot put the keyboard away under itself.
 */
void power_suspend_soon(void)
{
	asked = true;
	if (task)
		xTaskNotifyGive(task);
}

void power_keep_screen(bool on)
{
	keep += on ? 1 : -1;
	if (keep < 0)
		keep = 0;
	power_activity();
}

void idle_get(struct idle_times *t)
{
	*t = times;
	t->sleep = sleep_wanted();
}

static void idle_load(void)
{
	char path[64], line[64];
	FILE *f;

	if (!mount_resolve(CONFIG_PATH, path, sizeof(path)) || !(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof(line), f)) {
		int v;

		if (sscanf(line, "dim %d", &v) == 1 && v >= 0)
			times.dim_s = v;
		else if (sscanf(line, "blank %d", &v) == 1 && v >= 0)
			times.blank_s = v;
		else if (sscanf(line, "suspend %d", &v) == 1 && v >= 0)
			times.suspend_s = v;
		else if (!strncmp(line, "sleep ", 6))
			cpufreq_set_idle_sleep(!strncmp(line + 6, "on", 2));
		else if (sscanf(line, "backlight %d", &v) == 1 && v >= LIGHT_MIN && v <= 100)
			lcd_backlight_set(v);
		else if (sscanf(line, "volume %d", &v) == 1 && v >= 0 && v <= 100) {
			audio_set_volume(v);
			if (v)
				unmuted = v;
		} else if (!strncmp(line, "output ", 7))
			audio_set_output(!strncmp(line + 7, "jack", 4) ? AUDIO_OUT_JACK :
					 AUDIO_OUT_SPEAKER);
	}
	fclose(f);
}

/* All of /etc/power: the times, light sleep, the brightness and the volume. */
static int save_config(void)
{
	char path[64];
	int light = lcd_backlight_get();
	FILE *f;

	if (!mount_resolve(CONFIG_PATH, path, sizeof(path)) || !(f = fopen(path, "w")))
		return -EIO;
	fprintf(f, "# seconds without a key before the screen dims, goes dark,\n"
		"# and the system suspends; 0 is never. And light sleep between\n"
		"# events. `power` sets these. The brightness and the volume as\n"
		"# they were last set (backlight, volume, Fn 5 to Fn 9).\n"
		"dim %d\nblank %d\nsuspend %d\nsleep %s\n", times.dim_s, times.blank_s,
		times.suspend_s, sleep_wanted() ? "on" : "off");
	if (light >= LIGHT_MIN)
		fprintf(f, "backlight %d\n", light);	/* never a dark screen at boot */
	if (audio_present())
		fprintf(f, "volume %d\n", audio_volume());
	if (audio_has_jack() && audio_output() != AUDIO_OUT_AUTO)
		fprintf(f, "output %s\n", audio_output() == AUDIO_OUT_JACK ? "jack" : "speaker");
	return fclose(f) ? -EIO : 0;
}

int idle_set(const struct idle_times *t)
{
	if (t->dim_s < 0 || t->blank_s < 0 || t->suspend_s < 0)
		return -EINVAL;
	times = *t;
	if (task)
		xTaskNotifyGive(task);
	return save_config();
}

void power_levels_changed(void)
{
	save_at = now_us() + seconds(SAVE_AFTER_S);
	if (task)
		xTaskNotifyGive(task);
}

static void show_level(const char *what, int percent)
{
	char text[24];

	if (percent)
		snprintf(text, sizeof(text), "%s %d%%", what, percent);
	else
		snprintf(text, sizeof(text), "%s off", what);
	vt_note(text);
}

static int clamp_level(int v, int lo)
{
	return v < lo ? lo : v > 100 ? 100 : v;
}

/*
 * Fn 5 and Fn 6 the brightness down and up, Fn 7 and Fn 8 the volume down
 * and up, a tenth at a time, Fn 9 the sound off and back; Fn 0 dozes. The
 * status line says where it is now.
 */
void power_shortcut(char key)
{
	int light = lcd_backlight_get(), vol = audio_volume();

	switch (key) {
	case '5':
	case '6':
		light = clamp_level(light + (key == '6' ? LEVEL_STEP : -LEVEL_STEP), LIGHT_MIN);
		lcd_backlight_set(light);
		show_level("\u2600", light);
		break;
	case '9':
		if (!audio_present())
			return;
		if (vol) {
			unmuted = vol;
			vol = 0;
		} else {
			vol = unmuted ? unmuted : 50;
		}
		audio_set_volume(vol);
		show_level("\u266a", vol);
		break;
	case '7':
	case '8':
		if (!audio_present())
			return;
		vol = clamp_level(vol + (key == '8' ? LEVEL_STEP : -LEVEL_STEP), 0);
		if (vol)
			unmuted = vol;
		audio_set_volume(vol);
		show_level("\u266a", vol);
		break;
	case '0':
		power_doze();
		return;
	default:
		return;
	}
	power_levels_changed();
}

int idle_init(void)
{
	idle_load();
	last_key = last_any = now_us();
	if (!vt_has_display())
		times.dim_s = times.blank_s = 0;	/* nothing to dim */
	xTaskCreatePinnedToCore(idle_task, "kidle", 3072, NULL, 3, &task, 0);
	return 0;
}
