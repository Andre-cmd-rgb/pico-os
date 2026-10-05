/*
 * The button on the board.
 *
 * Every board has one -- it is the one that puts the chip into its
 * bootloader at reset -- and once the system is running it is free. A
 * short press moves to the next terminal, which is the one thing worth
 * having when the keyboard in front of you has no Ctrl key. Holding it
 * goes back to the first terminal. While an alarm rings, a press snoozes
 * it and holding stops it.
 *
 * Nothing polls it while it is up: a press interrupts (and wakes the
 * chip from light sleep), and it is only watched while held.
 */
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_BUTTON_GPIO >= 0

#define POLL_MS		30
#define DEBOUNCE_MS	60
#define HOLD_MS		800

static TaskHandle_t task;

static void IRAM_ATTR pressed(void *arg)
{
	BaseType_t woken = pdFALSE;

	gpio_intr_disable(CONFIG_PT_BUTTON_GPIO);	/* the task watches it from here */
	vTaskNotifyGiveFromISR(task, &woken);
	if (woken)
		portYIELD_FROM_ISR();
}

static void button_task(void *arg)
{
	/* down already: the press that woke the board from suspend, not a
	 * request to switch terminals */
	bool was_down = gpio_get_level(CONFIG_PT_BUTTON_GPIO) == 0, woke = was_down;
	int64_t down_at = 0;

	for (;;) {
		bool down = gpio_get_level(CONFIG_PT_BUTTON_GPIO) == 0;	/* pulled up */
		int64_t now = esp_timer_get_time();

		if (down && !was_down) {
			down_at = now;
		} else if (!down && was_down) {
			int64_t held = (now - down_at) / 1000;

			if (woke)
				woke = false;
			else if (held >= DEBOUNCE_MS && !power_key())
				;		/* it lit the screen, and that is all */
			else if (held >= DEBOUNCE_MS && alarm_button(held >= HOLD_MS))
				;		/* snoozed or stopped a ringing alarm */
			else if (held >= HOLD_MS)
				tty_switch(0);
			else if (held >= DEBOUNCE_MS)
				tty_switch((tty_front() + 1) % vt_count());
		}
		was_down = down;
		if (down) {
			vTaskDelay(pdMS_TO_TICKS(POLL_MS));
		} else {
			gpio_intr_enable(CONFIG_PT_BUTTON_GPIO);
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		}
	}
}

int button_init(void)
{
	const gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << CONFIG_PT_BUTTON_GPIO,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
	};

	if (gpio_config(&cfg) != ESP_OK)
		return -EIO;
	if (ktask_create(button_task, "kbutton", 4096, NULL, 4, &task, 0) != pdPASS)
		return -ENOMEM;
	/* a low level, so that it also wakes the chip out of light sleep */
	gpio_install_isr_service(0);		/* already there is fine */
	gpio_set_intr_type(CONFIG_PT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
	gpio_isr_handler_add(CONFIG_PT_BUTTON_GPIO, pressed, NULL);
	gpio_sleep_sel_dis(CONFIG_PT_BUTTON_GPIO);	/* its pull-up, while dozing */
	gpio_wakeup_enable(CONFIG_PT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
	esp_sleep_enable_gpio_wakeup();
	klog("button: GPIO%d switches terminals (hold for the first)",
	     CONFIG_PT_BUTTON_GPIO);
	return 0;
}

#else

int button_init(void) { return -ENODEV; }

#endif
