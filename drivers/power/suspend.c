/*
 * Suspend and power off: deep sleep, woken by a CardKB key, the side
 * button, the timer or an alarm.
 *
 * Deep sleep switches off the main cores and their RAM, so waking up is a
 * fresh boot, not a resume. The keyboard is watched by the low-power
 * RISC-V core, which asks it for a key every 100 ms (ulp/wake.c); it can
 * reach any of the RTC domain's pins, GPIO 0 to 21, and on the Freenove
 * board the CardKB is on 16 and 15. In a case, where the board's own
 * buttons cannot be reached, the keyboard is the way back.
 *
 * Suspend wakes on any key; power off only on Enter, so a key knocked in
 * a bag does not turn it on. A cell run flat powers off too, and wakes
 * every quarter of an hour to see whether a charger has come: if not, it
 * goes straight back to sleep before the screen lights.
 */
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#define KEY_WAKE (CONFIG_ULP_COPROC_TYPE_RISCV && CONFIG_PT_KBD_CARDKB && \
		  CONFIG_PT_CARDKB_SDA >= 0 && CONFIG_PT_CARDKB_SDA <= 21 && \
		  CONFIG_PT_CARDKB_SCL >= 0 && CONFIG_PT_CARDKB_SCL <= 21)

#define ENTER		0x0d
#define EMPTY_CHECK_S	(15 * 60)
/*
 * A flat cell left alone creeps back up by a tenth of a volt or so; one
 * on a charger is lifted by the charge going in and climbs out of the
 * bottom of its curve. Above this, it has been charged.
 */
#define RECOVERED_MV	3650
#define EMPTY_MARK	0x454d5054	/* "EMPT" */

static RTC_NOINIT_ATTR uint32_t empty_mark;
static const char *started_by = "power-on";	/* for the power log */

const char *power_start_reason(void)
{
	return started_by;
}

#if KEY_WAKE

#include "ulp_drivers.h"
#include "ulp_riscv.h"

#define POLL_US		100000

extern const uint8_t ulp_bin_start[] asm("_binary_ulp_drivers_bin_start");
extern const uint8_t ulp_bin_end[] asm("_binary_ulp_drivers_bin_end");

static const gpio_num_t kbd_lines[] = { CONFIG_PT_CARDKB_SDA, CONFIG_PT_CARDKB_SCL };

/*
 * Hand the keyboard's wires to the RTC domain, open drain with its
 * pull-ups (which stay powered with the RTC peripherals), and start the
 * low-power core on them. `want` is the key that wakes it; 0 is any.
 */
static int watch_keyboard(int want)
{
	cardkb_stop();
	for (int i = 0; i < 2; i++) {
		gpio_num_t pin = kbd_lines[i];

		if (!rtc_gpio_is_valid_gpio(pin) || rtc_gpio_init(pin) ||
		    rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_OUTPUT_OD) ||
		    rtc_gpio_set_level(pin, 1) || rtc_gpio_pullup_en(pin) || rtc_gpio_pulldown_dis(pin))
			return -EIO;
	}
	if (ulp_riscv_load_binary(ulp_bin_start, ulp_bin_end - ulp_bin_start) != ESP_OK)
		return -EIO;
	ulp_sda = CONFIG_PT_CARDKB_SDA;
	ulp_scl = CONFIG_PT_CARDKB_SCL;
	ulp_want = want;
	ulp_key = 0;
	ulp_polls = ulp_answers = 0;
	ulp_set_wakeup_period(0, POLL_US);
	if (esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON) ||
	    ulp_riscv_run() != ESP_OK || esp_sleep_enable_ulp_wakeup() != ESP_OK)
		return -EIO;
	return 0;
}

/* Back to sleep with the low-power core still at it: just the wake-up. */
static void keep_watching(void)
{
	esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
	esp_sleep_enable_ulp_wakeup();
}

/* Awake for good: the wires go back to the keyboard's driver. */
static void stop_watching(void)
{
	ulp_riscv_halt();
	if (ulp_polls)
		klog("power: the keyboard answered %lu of %lu polls while asleep",
		     (unsigned long)ulp_answers, (unsigned long)ulp_polls);
	for (int i = 0; i < 2; i++)
		rtc_gpio_deinit(kbd_lines[i]);
}

#else

static int watch_keyboard(int want)
{
	return -ENOTSUP;
}

static void keep_watching(void)
{
}

static void stop_watching(void)
{
}

#endif

#define BUTTON_WAKE	(CONFIG_PT_BUTTON_GPIO >= 0)

/*
 * The side button pulls its pin low when pressed. Waking on it takes the
 * RTC domain's own pull-up, which only holds if the RTC peripherals stay
 * powered through the sleep.
 */
static int watch_button(void)
{
#if BUTTON_WAKE
	const gpio_num_t pin = CONFIG_PT_BUTTON_GPIO;

	if (!esp_sleep_is_valid_wakeup_gpio(pin))
		return -ENOTSUP;
	if (esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON) ||
	    rtc_gpio_pullup_en(pin) || rtc_gpio_pulldown_dis(pin) ||
	    esp_sleep_enable_ext1_wakeup_io(1ULL << pin, ESP_EXT1_WAKEUP_ANY_LOW))
		return -EIO;
	return 0;
#else
	return -ENOTSUP;
#endif
}

/*
 * Everything that draws current and cannot switch itself off once the
 * cores stop. The backlight is the big one -- it is brighter than the
 * whole rest of the board put together -- but the amplifier's enable
 * pin matters too, because an unheld pin floats and a floating enable
 * is as likely to mean on as off.
 */
void power_quiesce(void)
{
	battery_note("going to sleep");
	clock_save();		/* the reset button, after this, starts it at 1970 */
	clock_sleeping();
	battery_save();		/* what the cell has been through */
	audio_sleep();
	vfs_sync_all();
	sd_unmount();
	led_set_mode(LED_OFF);
	lcd_sleep();
}

/* What will wake it, for the log: the watchers that could be set up. */
static const char *wake_text(int key, int button, const char *which)
{
	static char text[64];

	if (!key && !button)
		snprintf(text, sizeof(text), "%s or the side button", which);
	else
		snprintf(text, sizeof(text), "%s", !key ? which : !button ? "the side button" :
			 "the reset button");
	return text;
}

static int deep_sleep(void *unused)
{
	esp_deep_sleep_start();
	return -EIO;
}

/*
 * Sleep turns the cache off, and with it the PSRAM that `suspend`'s and
 * `poweroff`'s stacks are in: the last step is taken on the kernel's own.
 */
static void __attribute__((noreturn)) sleep_now(void)
{
	on_internal_stack(deep_sleep, NULL);
	abort();			/* esp_deep_sleep_start() never comes back */
}

int power_suspend(uint32_t wake_after_s)
{
	int key = watch_keyboard(0), button = watch_button();
	uint32_t alarm = alarm_seconds_until();

	/* An alarm wakes it a moment early: booting takes about a second. */
	if (alarm && (!wake_after_s || alarm < wake_after_s)) {
		wake_after_s = alarm > 3 ? alarm - 2 : 1;
		klog("suspend: the next alarm wakes it in %lu s", (unsigned long)alarm);
	}
	power_quiesce();
	if (wake_after_s)
		esp_sleep_enable_timer_wakeup(clock_sleep_us((uint64_t)wake_after_s * 1000000));
	klog("suspend: sleeping; wake with %s%s", wake_text(key, button, "any CardKB key"),
	     wake_after_s ? " or the timer" : "");
	vTaskDelay(pdMS_TO_TICKS(150));		/* let the LED and the log get out */
	sleep_now();
	return -EIO;
}

void power_off(void)
{
	int key = watch_keyboard(ENTER), button = watch_button();

	power_quiesce();
	klog("power: off; wake with %s", wake_text(key, button, "Enter on the CardKB"));
	vTaskDelay(pdMS_TO_TICKS(150));
	sleep_now();
}

void power_off_empty(void)
{
	watch_keyboard(ENTER);
	watch_button();
	power_quiesce();
	empty_mark = EMPTY_MARK;
	esp_sleep_enable_timer_wakeup(EMPTY_CHECK_S * 1000000ULL);
	klog("power: off, the cell is flat; it wakes when charged");
	vTaskDelay(pdMS_TO_TICKS(150));
	sleep_now();
}

/* Why the chip started, when it was not a wake from sleep. */
static void say_reset(esp_reset_reason_t why)
{
	static const char *const names[] = {
		[ESP_RST_POWERON] = "power-on", [ESP_RST_EXT] = "the reset pin",
		[ESP_RST_SW] = "a restart", [ESP_RST_PANIC] = "a crash",
		[ESP_RST_INT_WDT] = "the interrupt watchdog", [ESP_RST_TASK_WDT] = "the task watchdog",
		[ESP_RST_WDT] = "a watchdog", [ESP_RST_BROWNOUT] = "a brown-out: the supply sagged",
		[ESP_RST_USB] = "the USB port (flashing)", [ESP_RST_JTAG] = "JTAG",
	};
	const char *name = why < sizeof(names) / sizeof(names[0]) ? names[why] : NULL;

	started_by = name ? name : "something unknown";
	klog("power: started by %s", started_by);
}

void power_boot_reason(void)
{
	uint32_t causes = esp_sleep_get_wakeup_causes();
	bool slept = esp_reset_reason() == ESP_RST_DEEPSLEEP;

	if (!slept)
		say_reset(esp_reset_reason());

	/*
	 * Off because the cell was flat, and woken by the timer only to see
	 * whether it has been charged: if not, back to sleep at once, before
	 * the screen lights. The low-power core is still watching the keys.
	 */
	if (slept && empty_mark == EMPTY_MARK && causes == BIT(ESP_SLEEP_WAKEUP_TIMER)) {
		int mv = battery_early_millivolts();

		if (mv > 0 && mv < RECOVERED_MV) {
			keep_watching();
			watch_button();
			esp_sleep_enable_timer_wakeup(EMPTY_CHECK_S * 1000000ULL);
			sleep_now();
		}
		klog("power: the cell has come back to %d.%02d V", mv / 1000, mv % 1000 / 10);
	}
	empty_mark = 0;

	/* Waking from deep sleep resets the main cores, not the RTC domain: the
	 * low-power core would go on polling the keyboard every 100 ms, on pins
	 * the CardKB driver is about to take back. */
	if (slept)
		stop_watching();
#if KEY_WAKE
	if (causes & BIT(ESP_SLEEP_WAKEUP_ULP)) {
		started_by = "woke on a key";
		klog("power: woke on the keyboard, key 0x%02lx", (unsigned long)ulp_key);
		return;
	}
#endif
#if BUTTON_WAKE
	/* the pin is the RTC domain's until it is handed back */
	if (slept)
		rtc_gpio_deinit(CONFIG_PT_BUTTON_GPIO);
	if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
		started_by = "woke on the button";
		klog("power: woke on the side button");
		return;
	}
#endif
	if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
		started_by = "woke on the timer (-t or an alarm)";
		klog("power: woke on the timer");
	}
}
