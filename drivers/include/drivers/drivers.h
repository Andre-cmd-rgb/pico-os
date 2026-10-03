/*
 * Driver entry points, called in order by the boot code in main/init.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "pt/kernel.h"

/* video/ili9341.c */
int	lcd_init(void);
int	lcd_width(void);
int	lcd_height(void);
uint8_t	*lcd_alloc_buffer(size_t bytes);
void	lcd_draw(int x, int y, int w, int h, const uint8_t *rgb565be);
void	lcd_fill(int x, int y, int w, int h, uint16_t rgb565);
/* Screenshots: arm the capture, draw, then read the pixels back. */
int	lcd_capture_begin(void);
const uint8_t *lcd_capture_pixels(int *w, int *h);	/* RGB565, high byte first */
void	lcd_capture_end(void);
int	lcd_capture_try_end(void);	/* -EAGAIN if drawing; safe for exit cleanup */
int	lcd_capture_save(const char *path);	/* the whole dance, into a BMP */

void	lcd_backlight_set(int percent);	/* 0 turns the backlight off */
int	lcd_backlight_get(void);	/* what was set: dimming does not change it */
int	lcd_backlight_now(void);	/* what the lamp is at, dimmed or dark */
void	lcd_light(int percent);		/* the idle dimmer: -1 is the set brightness */
void	lcd_panel_power(bool on);	/* the panel's own sleep, for a dark screen */
bool	lcd_panel_on(void);
void	lcd_sleep(void);		/* lamp out and panel asleep, before deep sleep */
int	lcd_set_rotation(int r);	/* 0-3 quarter turns; only a half turn from boot's */
int	lcd_rotation(void);
int	lcd_set_clock(int hz);		/* the panel's bus, while running */
int	lcd_clock(void);
int	lcd_read_reg(uint8_t cmd, uint8_t *out, int n);	/* the panel's registers */
/*
 * Moving pictures without tearing: a whole screen in the panel's own
 * portrait order (240 wide, 320 high), sent behind its refresh.
 */
bool	lcd_native_ok(void);
/* columns c0..c0+cw-1 of rows p0..p0+ph-1, portrait, the buffer row by row */
/* -EAGAIN means a complete frame sent after its refresh window. */
int	lcd_draw_native(const uint8_t *rgb565be, int c0, int cw, int p0, int ph);
void	lcd_native_order(bool upwards);	/* which way the refresh runs */
bool	lcd_native_upwards(void);

#if CONFIG_PT_LCD_ILI9341_I80
/* Parallel-bus wiring tests (lcdprobe, lcdreg): they drive every pin by
 * hand, so they take the pins from the bus until the next boot. */
int	lcd_probe_begin(int rd_gpio, bool reset);	/* rd_gpio -1: RD tied to 3V3 */
void	lcd_probe_command(uint8_t cmd, const uint8_t *arg, int n);
int	lcd_bus_command(uint8_t cmd, const uint8_t *arg, int n);	/* through the i80 bus */
int	lcd_bus_count_edges(int gpio, int n);
int	lcd_probe_read(const uint8_t *index, int nindex, uint8_t *out, int n, int pull);
void	lcd_probe_init(void);
void	lcd_probe_fill(uint16_t rgb565);
#endif

/* tty/vt.c: the virtual terminal (screen contents and renderer) */
void	vt_init(void);
void	vt_start_display(void);
void	vt_write(const char *s, size_t n);		/* the terminal on screen */
void	vt_write_on(int which, const char *s, size_t n);
int	vt_switch(int which);
int	vt_count(void);
int	vt_active(void);
/* File transfer with the PC, beside the console (drivers/misc/xfer.c). */
void	xfer_line(char *line);
void	xfer_set_output(void (*out)(const char *s, size_t n));
int	vt_scroll(int lines);	/* look back through what scrolled off: + back, - forward */
void	vt_scroll_end(void);	/* the screen as it is again */
void	vt_note(const char *text);	/* on the status line for two seconds */
void	vt_size(int *cols, int *rows);
bool	vt_has_display(void);
void	vt_redraw(void);	/* repaint everything, after drawing behind the terminal's back */
void	vt_hold_screen(bool held);	/* stop repainting: a program owns the panel */
/*
 * The holder draws only between vt_screen_begin() and _end(), and only if
 * begin says its terminal is in front. vt_screen_gen() changes each time
 * that terminal comes back, when the whole screen must be painted again.
 */
void	vt_blank(bool dark);		/* stop drawing while the panel sleeps */
void	vt_bar_line(int y, const char *text);	/* status-line colours, for a holder */
int	vt_line_height(void);

/* The colours a theme sets: the sixteen ANSI ones, then the terminal's own. */
enum vt_color {
	VT_FG = 16,		/* text */
	VT_BG,			/* the background */
	VT_DIM,			/* faint text (SGR 2) */
	VT_BOLD,		/* bold text in the default colour */
	VT_BAR_FG,		/* the status line's text */
	VT_BAR_BG,		/* and its background */
	VT_CURSOR,
	VT_COLORS
};
enum vt_cursor { VT_CURSOR_BLOCK, VT_CURSOR_UNDERLINE, VT_CURSOR_BAR };
void	vt_set_palette(const uint32_t rgb[VT_COLORS]);	/* 0xRRGGBB each */
void	vt_set_cursor(enum vt_cursor shape, int blink_ms);	/* 0: steady */
void	vt_set_bar(bool top);

/* tty/theme.c: the screen's colours and looks, kept in /etc/theme */
enum theme_mode { THEME_DARK, THEME_LIGHT, THEME_AUTO };
struct theme_state {
	const char	*name;
	enum theme_mode	 mode;
	bool		 light;		/* what is showing, in auto mode too */
	bool		 has_dark, has_light;	/* the theme's halves */
	int		 day_from;	/* auto: light from, minutes after midnight */
	int		 day_until;	/* and until */
	int		 own;		/* colours of your own over the theme's */
	enum vt_cursor	 cursor;
	int		 blink_ms;
	bool		 bar_top;
};
void	theme_default(void);		/* the built-in one, before / is mounted */
void	theme_restore(void);		/* /etc/theme, at boot */
void	theme_tick(void);		/* once a second: auto mode's change */
int	theme_count(void);
const char *theme_name_at(int i);
const char *theme_about_at(int i);
bool	theme_has_dark_at(int i);
bool	theme_has_light_at(int i);
int	theme_use(const char *name);	/* -ENOENT for no such theme */
int	theme_set_mode(enum theme_mode mode, int day_from, int day_until);
int	theme_set_color(const char *slot, uint32_t rgb);	/* -EINVAL: no such slot */
void	theme_reset_colors(void);
void	theme_set_cursor(enum vt_cursor shape, int blink_ms);
void	theme_set_bar(bool top);
void	theme_get(struct theme_state *st);
void	theme_palette(uint32_t rgb[VT_COLORS]);	/* what is showing */
const char *theme_slot_name(int slot);
int	theme_save(void);
bool	vt_screen_begin(void);
void	vt_screen_end(void);
bool	vt_screen_front(void);		/* no lock: for deciding to pause */
unsigned vt_screen_gen(void);
/* The same for a program's helper task, which is no program itself: with
 * the terminal the program holds, from vt_screen_mine() (-1 for none). */
int	vt_screen_mine(void);
bool	vt_screen_begin_on(int vt);
bool	vt_screen_front_on(int vt);
unsigned vt_screen_gen_on(int vt);

/* tty/tty.c: the console line discipline */
void	tty_init(void);
struct pt_file *tty_console(int which);	/* made on first use */
int	tty_switch(int which);		/* put that terminal in front */
int	tty_front(void);
int	tty_of_current(void);		/* the calling process's terminal, or -1 */
void	tty_set_activate_hook(void (*fn)(int which));
void	tty_input(const char *s, size_t n);		/* from a keyboard on the board */
void	tty_input_remote(const char *s, size_t n);	/* from the PC */
void	tty_output(const char *s, size_t n);
void	tty_set_mirror(void (*write)(const char *s, size_t n));
void	tty_mirror_output(const char *s, size_t n);	/* serial only */

/* misc/i2c.c: the bus several devices share (codec, keyboard, touch).
 * Spelled out rather than including driver/i2c_master.h, so that everything
 * including this header does not need the I2C component. */
struct i2c_master_bus_t;
int	i2c_bus_get(int sda, int scl, struct i2c_master_bus_t **out);
int	i2c_bus_probe(int sda, int scl, int addr);	/* 0: something answered */

/* audio/audio.c: the codec, and /dev/audio */
int	audio_init(void);
bool	audio_present(void);
bool	audio_busy(void);		/* something is playing or recording */
void	audio_stop(void);		/* the caller's sound plays out, then its stream goes */
void	audio_discard(void);		/* the caller's queued sound goes unplayed */
void	audio_sleep(void);		/* before deep sleep: all silent, codec in standby */
int	audio_set_latency(int ms);	/* how much of the caller's sound may be queued */
void	audio_claim(bool mine);		/* the alarm takes the speaker, and gives it back */
int	audio_set_rate(int hz);
int	audio_rate(void);
int	audio_buffer_us(void);		/* the caller's queue and the DMA, full, in us */
int	audio_queued_us(void);		/* until a sample written now is heard, in us */
int	audio_set_volume(int percent);
int	audio_volume(void);
int	audio_set_mic_gain(int db);	/* the analogue scale, 0-42 dB */
int	audio_set_mic_alc(bool on, int max_db);	/* let the codec ride the gain */
bool	audio_mic_alc(void);
void	audio_mic_alc_hold(bool hold);	/* counted: the codec's riding off meanwhile */
ssize_t	audio_write(const void *pcm, size_t bytes, int channels);	/* 16-bit */
ssize_t	audio_read(void *pcm, size_t bytes);				/* 16-bit mono */

/* The headphone jack, where there is one: sound goes there or to the speaker. */
enum audio_out { AUDIO_OUT_AUTO, AUDIO_OUT_SPEAKER, AUDIO_OUT_JACK };
bool	audio_has_jack(void);
bool	audio_jack_switch(void);	/* the socket says when a plug is in */
int	audio_set_output(enum audio_out out);	/* -ENODEV with no jack */
enum audio_out audio_output(void);	/* as it was asked for */
bool	audio_to_jack(void);		/* where sound would go now */

/* net/wifi.c */
struct wifi_ap {
	char	ssid[33];
	int8_t	rssi;		/* dBm; -50 is close, -80 is far */
	uint8_t	channel;
	bool	secure;
};

struct wifi_info {
	bool	up;		/* associated and holding an address */
	char	ssid[33];
	int8_t	rssi;
	uint8_t	channel;
	char	ip[16], gateway[16], netmask[16];
};

int	wifi_init(void);		/* the radio, at boot */
void	wifi_start_supplicant(void);	/* once / is mounted: try /etc/wifi */
bool	wifi_started(void);
bool	wifi_radio_on(void);		/* started and not resting between tries */
void	wifi_retry_soon(void);		/* look for the saved networks now */
void	wifi_rest(void);		/* off the network until wifi_retry_soon() */
bool	wifi_up(void);
int	wifi_connect(const char *ssid, const char *pass, int timeout_ms);
int	wifi_disconnect(void);
int	wifi_radio(bool on);
int	wifi_scan(struct wifi_ap *out, int max);
int	wifi_scan_get(int index, struct wifi_ap *out);	/* from the last scan, 1-based */
int	wifi_state(struct wifi_info *out);
int	wifi_save(const char *ssid, const char *pass);	/* into /etc/wifi */
int	wifi_forget(const char *ssid);
bool	wifi_saved(const char *ssid, char *pass, size_t size);	/* its password, if saved */
int	wifi_ntp_sync(int timeout_ms);

/* net/netconsole.c: a shell over the network, so the USB port is free */
int	netconsole_init(void);
bool	netconsole_busy(void);
int	netconsole_port(void);		/* -1 when it is not listening */

/* net/modem.c: a GSM/LTE module on a serial port */
struct modem_info {
	char	model[48];
	char	imei[20];
	char	sim[24];		/* "READY", "SIM PIN", "no card" */
	char	operator[32];
	int	rssi;			/* dBm, 0 when the module cannot tell */
	int	reg;			/* +CREG's state, -1 unknown: modem_network_text() */
	int	restarts;		/* in the last minute: more than one is its supply */
	int	supply_mv;		/* what it says it gets (AT+CBC), 0 unknown */
	bool	no_2g;			/* the SIM lacks a 2G part (a USIM only) */
	bool	registered, roaming, data;
};

struct sms {
	int	index;
	bool	unread;
	char	from[24];
	char	when[24];
	char	text[321];		/* UTF-8: 160 characters, some two bytes */
};

int	modem_init(void);
bool	modem_present(void);
int	modem_probe(void);		/* look again for one: 0 when there is one */
const char *modem_network_text(int reg);
int	modem_ussd(const char *code, char *out, size_t size);	/* *123# and the like */
int	modem_apn(char *apn, size_t asz, char *user, size_t usz, char *pass, size_t psz);
int	modem_set_apn(const char *apn, const char *user, const char *pass);	/* kept */
int	modem_radio(bool on);		/* AT+CFUN, kept in /etc/modem */
int	modem_network(const char *plmn);	/* try it first ("22201"), "" any; kept */
int	modem_diagnose(int seconds, bool radio_off);	/* restart it, every line logged */
bool	modem_radio_on(void);
void	modem_power_off(void);		/* the board is switching off: the radio too */
int	modem_load_ma(void);		/* its draw from the cell, for the battery */
int	modem_at(const char *cmd, char *reply, size_t size, int timeout_ms);
int	modem_info(struct modem_info *out);
int	modem_sms_send(const char *number, const char *text);
int	modem_sms_list(struct sms *out, int max, bool unread_only);
int	modem_sms_read(int index, struct sms *out);
int	modem_sms_delete(int index);
int	modem_data(bool on);		/* the PPP link: mobile internet */
bool	modem_data_up(void);

/* power/battery.c */
/* A lithium cell is flat at 3.3 V and its protection cuts out around 2.5 V,
 * so anything under this is not a cell at all: the board is on USB and the
 * sense pin is floating. */
#define BATTERY_NO_CELL_MV	2500
enum battery_state {
	BATTERY_NONE,		/* nothing on the sense pin */
	BATTERY_USB,		/* no cell fitted: the board is on USB */
	BATTERY_UNKNOWN,	/* a cell, but not yet enough history to judge */
	BATTERY_DISCHARGING,
	BATTERY_CHARGING,
	BATTERY_FULL,
	BATTERY_IDLE,		/* on USB, but the charger is not charging */
};

struct battery_status {
	int	mv;		/* smoothed, not the raw reading */
	int	rest;		/* with the load's sag or the charger's lift taken off */
	int	raw;		/* the last reading as it came */
	int	percent;
	int	permille;	/* the same in tenths */
	int	trend;		/* millivolts an hour, signed */
	int	ma;		/* estimated, out of the cell; into it is negative */
	int	capacity_mah;	/* on its label: `battery -c` */
	int	holds_mah;	/* what it was measured to hold lately; 0 until then */
	int	mohm;		/* the cell's resistance, learned */
	bool	mohm_guessed;	/* nothing has shown it yet */
	bool	charger;	/* one is believed to be plugged in */
	bool	usb;		/* a PC is talking over the port */
	int	cycles10;	/* tenths of a cycle used since it was fitted */
	int	used_mah;	/* counted out of it since it was fitted */
	int	health;		/* percent of what it held new; -1 until measured */
	int	measured;	/* discharges it has been measured over */
	int	minutes_left;	/* at this rate; -1 when it cannot say */
	int	minutes_full;	/* charging; -1 when it cannot say */
	int	charge_ma;	/* what the charger is believed to push; 0 if none */
	int	cal;		/* the readings' scale, ten-thousandths */
	bool	cal_meter;	/* set from a meter rather than the charger */
	enum battery_state state;
};

int	battery_init(void);
int	battery_millivolts(void);	/* <0 when there is no battery sensing */
int	battery_early_millivolts(void);	/* before battery_init: a reading, no more */
int	battery_percent(void);
int	battery_status(struct battery_status *out);
int	battery_set_capacity(int mah);	/* a new cell: its life starts again */
int	battery_calibrate(int meter_mv);	/* what a meter on the cell says now */
void	battery_save(void);		/* before the power goes */
void	battery_note(const char *what);	/* a line in the power log, /etc/power.log */
const char *battery_state_name(enum battery_state state);

/* input/blepad.c: a Bluetooth gamepad, for games */
enum pad_button {
	PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
	PAD_A, PAD_B, PAD_START, PAD_SELECT,
	PAD_BUTTON_COUNT,
};

struct pad_found {
	uint8_t	addr[6];
	uint8_t	addr_type;
	int8_t	rssi;
	bool	is_hid;		/* says it is a keyboard, mouse or pad */
	char	name[32];
};

int	pad_start(void);		/* brings the radio up: costs RAM */
bool	pad_started(void);
int	pad_scan(struct pad_found *out, int max, int seconds);
int	pad_scan_all(struct pad_found *out, int max, int seconds, bool everything);
int	pad_connect(int index);		/* from the last scan, 1-based */
bool	pad_connected(void);
uint16_t pad_buttons(void);		/* one bit per enum pad_button */
const char *pad_device_name(void);
int	pad_last_report(uint8_t *out, int max);	/* for working out a mapping */

/* input */
int	serial_console_init(void);
int	button_init(void);	/* the board's own button: switches terminals */
int	cardkb_init(void);
unsigned cardkb_errors(void);	/* I2C reads that failed and recovered */
void	cardkb_stop(void);
int	usbkbd_init(void);

/* misc/led.c: the board's RGB LED as a status light */
enum led_mode {
	LED_OFF,
	LED_ON,
	LED_HEARTBEAT,
	LED_CHARGE,		/* lit while charging, green when full, else dark */
};

int	led_init(void);
void	led_set_mode(enum led_mode mode);
enum led_mode led_get_mode(void);
void	led_set_color(uint8_t r, uint8_t g, uint8_t b);
void	led_get_color(uint8_t *r, uint8_t *g, uint8_t *b);

/* misc/alarm.c: alarms, timers and reminders, kept in /etc/alarms */
#define ALARMS_MAX	24
#define ALARM_DAILY	0x7f		/* days: bit 0 is Monday, bit 6 Sunday */
enum alarm_kind {
	ALARM_RING,			/* rings until answered */
	ALARM_CHIME,			/* a reminder: a chime, and the bar */
};

struct alarm {
	bool	on;
	uint8_t	hour, min, sec;
	uint8_t	days;			/* the weekdays it rings; none: once */
	int	date;			/* YYYYMMDD: that day only, then it goes */
	uint8_t	kind;			/* enum alarm_kind */
	uint8_t	snoozes;		/* a snooze is 1 or more: which one */
	char	label[40];
};

int	alarm_init(void);		/* once / is mounted */
int	alarm_list(struct alarm *out, int max);
int	alarm_add(const struct alarm *a);	/* its number, or <0 */
int	alarm_remove(int i);
int	alarm_enable(int i, bool on);
time_t	alarm_next(const struct alarm *a, time_t after);	/* 0: never again */
time_t	alarm_next_any(struct alarm *which);		/* 0: none set */
uint32_t alarm_seconds_until(void);	/* for suspend's timer; 0: none */
bool	alarm_ringing(struct alarm *which);
void	alarm_answer(bool stop);	/* snooze it, or stop it */
int	alarm_test(int kind);		/* ring now */
bool	alarm_key(const char *s, size_t n);	/* taken, while it rings */
bool	alarm_button(bool held);	/* a press snoozes, holding stops */

/* power/cpufreq.c: CPU frequency policy, as in Linux cpufreq */
int	cpufreq_init(void);
int	cpufreq_set(int min_mhz, int max_mhz);		/* one of cpufreq_speeds */
void	cpufreq_get(int *min_mhz, int *max_mhz);
void	cpufreq_boost(bool on);	/* counted: the policy's top as its floor too */
bool	cpufreq_try_boost(bool on);	/* reaper retries instead of blocking */
int	cpufreq_boosted(void);
void	cpufreq_speeds(int *slowest, int *middle, int *fastest);	/* 80, 160, 240 on the S3 */
int	cpufreq_current_mhz(void);
const char *cpufreq_policy_name(int min_mhz, int max_mhz);
int	cpufreq_set_idle_sleep(bool on);	/* light sleep whenever everything is idle */
bool	cpufreq_idle_sleep(void);
int	cpufreq_stats(char *buf, size_t size);	/* time spent per power mode */
int	cpufreq_time_summary(char *buf, size_t size);	/* "70% at 80 MHz, 28% at 240 MHz" */

/* power/suspend.c */
/* power/idle.c: the screen dims, goes dark, and the system suspends */
enum screen_state { SCREEN_ON, SCREEN_DIM, SCREEN_OFF };
struct idle_times {
	int	dim_s;		/* without a key before the screen dims; 0 never */
	int	blank_s;	/* before it goes dark */
	int	suspend_s;	/* before deep sleep, if nothing is going on */
	bool	sleep;		/* light sleep between events */
};
int	idle_init(void);		/* once / is mounted: /etc/power */
void	idle_get(struct idle_times *t);
int	idle_set(const struct idle_times *t);	/* and into /etc/power */
void	power_activity(void);		/* someone is here: lights the screen */
bool	power_key(void);		/* a key: false if it only woke the screen */
void	power_remote_activity(void);	/* the PC typing: no deep sleep, no light */
void	power_screen_wake(void);	/* light it now and wait: before drawing on it */
/* Fn 5 to Fn 0 on the CardKB: brightness, mute, volume, doze (idle.c). */
void	power_shortcut(char key);
/* A brightness or volume just set, to be remembered in /etc/power. */
void	power_levels_changed(void);
void	power_keep_screen(bool on);	/* moving pictures: never dim (counted) */
void	power_suspend_soon(void);	/* from a driver's task: the idle task does it */
void	power_doze(void);		/* dark now, everything kept, until a key */
int	power_set_sleep(bool on);	/* `power sleep`: light sleep when idle, dark or not */
enum screen_state power_screen(void);
int	power_poll_ms(int ms);		/* a polling period, longer while it is dark */

void	power_quiesce(void);	/* switch off what deep sleep cannot */
int	power_suspend(uint32_t wake_after_s);		/* deep sleep; returns only on error */
void	power_off(void) __attribute__((noreturn));	/* deep sleep until Enter */
void	power_off_empty(void) __attribute__((noreturn));	/* a flat cell: until charged */
void	power_boot_reason(void);			/* logs how the last suspend ended */
const char *power_start_reason(void);		/* what that was, in words */

/* input/serial.c */
void	serial_idle_sleep(bool on);	/* let console input wake the chip */

/* storage */
int	rootfs_init(void);	/* storage/rootfs.c: mounts / */
int	rootfs_format(void);	/* a new root filesystem: everything on the flash goes */
int	tmpfs_init(void);	/* storage/ramdisk.c: mounts /tmp */
int	ramdisk_mount(const char *vfs, size_t kb, bool format);
int	sd_init(void);		/* storage/sdcard.c: mounts /mnt/sd */
int	sd_mount(void);
int	sd_unmount(void);
int	sd_format(void);	/* a new filesystem: everything on the card goes */
bool	sd_mounted(void);
int	sd_describe(char *buf, size_t size);

/* misc/usbhost.c: the one USB host stack, shared by its drivers */
int	usb_host_start(void);
bool	usb_host_running(void);

/* storage/usbmsc.c: a memory stick on /mnt/usb */
int	usbmsc_init(void);
int	usb_unmount(void);
