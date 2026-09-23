/*
 * Driver entry points, called in order by the boot code in main/init.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
int	lcd_capture_save(const char *path);	/* the whole dance, into a BMP */

void	lcd_backlight_set(int percent);	/* 0 turns the backlight off */
void	lcd_sleep(void);		/* lamp out and panel asleep, before deep sleep */
int	lcd_backlight_get(void);

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
void	vt_size(int *cols, int *rows);
bool	vt_has_display(void);
void	vt_redraw(void);	/* repaint everything, after drawing behind the terminal's back */
void	vt_hold_screen(bool held);	/* stop repainting: a program owns the panel */

/* tty/tty.c: the console line discipline */
void	tty_init(void);
struct pt_file *tty_console(int which);	/* made on first use */
int	tty_switch(int which);		/* put that terminal in front */
int	tty_front(void);
void	tty_set_activate_hook(void (*fn)(int which));
void	tty_input(const char *s, size_t n);
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
void	audio_stop(void);		/* drain, then silence the amplifier */
int	audio_set_rate(int hz);
int	audio_rate(void);
int	audio_buffer_us(void);		/* the output ring, full, in microseconds */
int	audio_set_volume(int percent);
int	audio_volume(void);
int	audio_set_mic_gain(int db);	/* the analogue scale, 0-42 dB */
int	audio_set_mic_alc(bool on, int max_db);	/* let the codec ride the gain */
bool	audio_mic_alc(void);
ssize_t	audio_write(const void *pcm, size_t bytes, int channels);	/* 16-bit */
ssize_t	audio_read(void *pcm, size_t bytes);				/* 16-bit mono */

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
bool	wifi_up(void);
int	wifi_connect(const char *ssid, const char *pass, int timeout_ms);
int	wifi_disconnect(void);
int	wifi_radio(bool on);
int	wifi_scan(struct wifi_ap *out, int max);
int	wifi_scan_get(int index, struct wifi_ap *out);	/* from the last scan, 1-based */
int	wifi_state(struct wifi_info *out);
int	wifi_save(const char *ssid, const char *pass);	/* into /etc/wifi */
int	wifi_forget(const char *ssid);
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
	bool	registered, roaming, data;
};

struct sms {
	int	index;
	bool	unread;
	char	from[24];
	char	when[24];
	char	text[161];
};

int	modem_init(void);
bool	modem_present(void);
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
};

struct battery_status {
	int	mv;		/* smoothed, not the raw reading */
	int	percent;
	int	trend;		/* millivolts per minute, signed */
	int	minutes_left;	/* at this rate; -1 when it cannot say */
	enum battery_state state;
};

int	battery_init(void);
int	battery_millivolts(void);	/* <0 when there is no battery sensing */
int	battery_percent(void);
int	battery_status(struct battery_status *out);
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
};

int	led_init(void);
void	led_set_mode(enum led_mode mode);
enum led_mode led_get_mode(void);
void	led_set_color(uint8_t r, uint8_t g, uint8_t b);
void	led_get_color(uint8_t *r, uint8_t *g, uint8_t *b);

/* power/cpufreq.c: CPU frequency policy, as in Linux cpufreq */
int	cpufreq_init(void);
int	cpufreq_set(int min_mhz, int max_mhz);		/* 80, 160 or 240 */
void	cpufreq_get(int *min_mhz, int *max_mhz);
int	cpufreq_current_mhz(void);
const char *cpufreq_policy_name(int min_mhz, int max_mhz);
int	cpufreq_set_idle_sleep(bool on);	/* light sleep whenever everything is idle */
bool	cpufreq_idle_sleep(void);
int	cpufreq_stats(char *buf, size_t size);	/* time spent per power mode */
int	cpufreq_time_summary(char *buf, size_t size);	/* "70% at 80 MHz, 28% at 240 MHz" */

/* power/suspend.c */
void	power_quiesce(void);	/* switch off what deep sleep cannot */
int	power_suspend(uint32_t wake_after_s);		/* deep sleep; returns only on error */
void	power_boot_reason(void);			/* logs how the last suspend ended */

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
