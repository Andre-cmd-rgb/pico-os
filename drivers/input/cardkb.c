/*
 * M5Stack CardKB: an ATmega-based keyboard at I2C address 0x5f that returns
 * one byte per read, 0 when nothing was pressed. Hot-pluggable: the driver
 * probes every two seconds until it answers.
 */
#include "driver/i2c_master.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_KBD_CARDKB

#define CARDKB_ADDR	0x5f
#define PROBE_MS	2000
/*
 * One failed read means nothing: the bus is shared with the audio codec,
 * the pull-ups are the chip's own weak ones, and a Grove cable is a long
 * piece of unshielded wire. Only a run of them means the keyboard is
 * really gone.
 */
#define LOST_AFTER	10
#define DARK_POLL_MS	100	/* while the screen is dark */

/*
 * The Fn layer. The keyboard's firmware sends 0x80-0xaf for Fn plus a key,
 * one code per key in the order the matrix is scanned, so a table of the
 * keys in that order says what was pressed. There is no control key on
 * this keyboard, which would leave a shell with no Ctrl-C and no Ctrl-D,
 * so Fn+letter is it. Fn+1 to Fn+4 pick a terminal the way Alt+F1 does on
 * a Linux console, and Fn+left / Fn+right step between them; Fn+5 to Fn+0
 * are a laptop's keys: brightness, mute, volume, and doze
 * (power_shortcut()).
 */
#define FN_FIRST	0x80
#define FN_LAST		0xaf

static const char fn_key[] =
	"\x1b" "1234567890\b"	/* 0x80  esc 1...0 del */
	"\tqwertyuiop\0"	/* 0x8c  tab q...p, then Fn itself */
	"\0\0asdfghjkl\r"	/* 0x98  left up a...l enter */
	"\0\0zxcvbnm,. ";	/* 0xa4  down right z...m , . space */

_Static_assert(sizeof(fn_key) == FN_LAST - FN_FIRST + 2, "one entry per Fn code");

static void cardkb_fn(uint8_t key)
{
	char c = fn_key[key - FN_FIRST];
	int n = vt_count();

	if (!power_key())
		return;			/* it lit a dark screen, and does nothing else */

	switch (key) {
	case 0x98: tty_switch((tty_front() + n - 1) % n); return;	/* left */
	case 0xa5: tty_switch((tty_front() + 1) % n); return;		/* right */
	case 0x99: tty_input("\x1b[H", 3); return;			/* up: home */
	case 0xa4: tty_input("\x1b[F", 3); return;			/* down: end */
	case 0x8b: tty_input("\x1b[3~", 4); return;			/* del: forward */
	}
	/* Fn 1 to Fn 4 the terminals; Fn 5 to Fn 0 a laptop's keys */
	if (c >= '0' && c <= '9') {
		int which = c == '0' ? 9 : c - '1';

		if (which < n && which < 4)
			tty_switch(which);
		else
			power_shortcut(c);
		return;
	}
	if (c >= 'a' && c <= 'z') {
		char ctrl = c & 0x1f;

		tty_input(&ctrl, 1);
		return;
	}
	klog("cardkb: unmapped key 0x%02x", key);
}

static void cardkb_key(uint8_t key)
{
	char c = key;

	if (key >= FN_FIRST && key <= FN_LAST) {
		cardkb_fn(key);
		return;
	}
	switch (key) {
	case 0xb4: tty_input("\x1b[D", 3); break;
	case 0xb5: tty_input("\x1b[A", 3); break;
	case 0xb6: tty_input("\x1b[B", 3); break;
	case 0xb7: tty_input("\x1b[C", 3); break;
	case 0x08: tty_input("\x7f", 1); break;
	case 0x7f: tty_input("\x1b[3~", 4); break;
	case 0x0d: tty_input("\r", 1); break;
	case 0x09:
	case 0x1b:
		tty_input(&c, 1);
		break;
	default:
		if (key >= 0x20 && key < 0x7f)
			tty_input(&c, 1);
		else
			klog("cardkb: unmapped key 0x%02x", key);
		break;
	}
}

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev;
static TaskHandle_t task;
static unsigned errors;		/* reads that failed and then recovered */
static SemaphoreHandle_t stopped;
static volatile bool stopping;

unsigned cardkb_errors(void)
{
	return errors;
}

static void cardkb_task(void *arg)
{
	bool present = false;
	int misses = 0;

	while (!stopping) {
		uint8_t key = 0;

		if (!present) {
			if (i2c_master_probe(bus, CARDKB_ADDR, 50) != ESP_OK) {
				vTaskDelay(pdMS_TO_TICKS(PROBE_MS));
				continue;
			}
			present = true;
			misses = 0;
			klog("cardkb: keyboard found at 0x%02x", CARDKB_ADDR);
		}
		if (i2c_master_receive(dev, &key, 1, 50) != ESP_OK) {
			if (++misses >= LOST_AFTER) {
				present = false;
				klog("cardkb: keyboard disconnected (%d reads in a row failed)",
				     misses);
			}
			vTaskDelay(pdMS_TO_TICKS(CONFIG_PT_CARDKB_POLL_MS));
			continue;
		}
		errors += misses;
		misses = 0;
		if (key)
			cardkb_key(key);
		/* in the dark a key only has to wake the screen: a tenth of a
		 * second is quick enough, and the chip sleeps in between */
		vTaskDelay(pdMS_TO_TICKS(power_screen() == SCREEN_OFF ? DARK_POLL_MS :
					 CONFIG_PT_CARDKB_POLL_MS));
	}
	xSemaphoreGive(stopped);
	vTaskDelete(NULL);
}

int cardkb_init(void)
{
	const i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = CARDKB_ADDR,
		.scl_speed_hz = 100000,
	};

	/* After waking from suspend the pins still belong to the low-power core */
	if (rtc_gpio_is_valid_gpio(CONFIG_PT_CARDKB_SDA))
		rtc_gpio_deinit(CONFIG_PT_CARDKB_SDA);
	if (rtc_gpio_is_valid_gpio(CONFIG_PT_CARDKB_SCL))
		rtc_gpio_deinit(CONFIG_PT_CARDKB_SCL);

	if (i2c_bus_get(CONFIG_PT_CARDKB_SDA, CONFIG_PT_CARDKB_SCL, &bus) ||
	    i2c_master_bus_add_device(bus, &dev_cfg, &dev)) {
		klog("cardkb: I2C setup failed on SDA %d / SCL %d", CONFIG_PT_CARDKB_SDA, CONFIG_PT_CARDKB_SCL);
		return -EIO;
	}
	stopping = false;
	stopped = xSemaphoreCreateBinary();
	xTaskCreatePinnedToCore(cardkb_task, "kcardkb", 4096, NULL, 9, &task, 0);
	return 0;
}

/* Hands the pins back, for the low-power core during suspend. */
void cardkb_stop(void)
{
	if (!task)
		return;
	stopping = true;
	xSemaphoreTake(stopped, pdMS_TO_TICKS(3000));
	i2c_master_bus_rm_device(dev);	/* the bus stays: others may use it */
	task = NULL;
}

#else

int cardkb_init(void)
{
	return -ENODEV;
}

void cardkb_stop(void)
{
}

#endif
