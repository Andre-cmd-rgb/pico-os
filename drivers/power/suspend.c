/*
 * Suspend: deep sleep, with the low-power RISC-V core watching the CardKB.
 *
 * Deep sleep switches off the main cores and their RAM, so waking up is a
 * fresh boot, not a resume. The low-power core can only use the RTC I2C
 * controller, which exists on GPIO1/3 (SDA) and GPIO0/2 (SCL); with the CardKB
 * anywhere else only the timer or the reset button wakes the board.
 */
#include "esp_sleep.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#define KEY_WAKE (CONFIG_ULP_COPROC_TYPE_RISCV && CONFIG_PT_KBD_CARDKB && \
		  (CONFIG_PT_CARDKB_SDA == 1 || CONFIG_PT_CARDKB_SDA == 3) && \
		  (CONFIG_PT_CARDKB_SCL == 0 || CONFIG_PT_CARDKB_SCL == 2))

#if KEY_WAKE

#include "ulp_drivers.h"
#include "ulp_riscv.h"
#include "ulp_riscv_i2c.h"

#define CARDKB_ADDR	0x5f
#define POLL_US		100000

extern const uint8_t ulp_bin_start[] asm("_binary_ulp_drivers_bin_start");
extern const uint8_t ulp_bin_end[] asm("_binary_ulp_drivers_bin_end");

static int watch_keyboard(void)
{
	const ulp_riscv_i2c_cfg_t cfg = {
		.i2c_pin_cfg = {
			.sda_io_num = CONFIG_PT_CARDKB_SDA,
			.scl_io_num = CONFIG_PT_CARDKB_SCL,
			.sda_pullup_en = true,
			.scl_pullup_en = true,
		},
		ULP_RISCV_I2C_STANDARD_MODE_CONFIG()
	};

	cardkb_stop();
	if (ulp_riscv_i2c_master_init(&cfg) != ESP_OK)
		return -EIO;
	ulp_riscv_i2c_master_set_slave_addr(CARDKB_ADDR);
	ulp_riscv_i2c_master_set_slave_reg_addr(0);
	if (ulp_riscv_load_binary(ulp_bin_start, ulp_bin_end - ulp_bin_start) != ESP_OK)
		return -EIO;
	ulp_key = 0;
	ulp_set_wakeup_period(0, POLL_US);
	if (ulp_riscv_run() != ESP_OK || esp_sleep_enable_ulp_wakeup() != ESP_OK)
		return -EIO;
	return 0;
}

#else

static int watch_keyboard(void)
{
	return -ENOTSUP;
}

#endif

/*
 * Everything that draws current and cannot switch itself off once the
 * cores stop. The backlight is the big one -- it is brighter than the
 * whole rest of the board put together -- but the amplifier's enable
 * pin matters too, because an unheld pin floats and a floating enable
 * is as likely to mean on as off.
 */
void power_quiesce(void)
{
	audio_stop();
	vfs_sync_all();
	sd_unmount();
	led_set_mode(LED_OFF);
	lcd_sleep();
}

int power_suspend(uint32_t wake_after_s)
{
	int key = watch_keyboard();

	power_quiesce();
	if (wake_after_s)
		esp_sleep_enable_timer_wakeup((uint64_t)wake_after_s * 1000000);
	klog("suspend: sleeping; wake with %s%s", key ? "the reset button" : "any CardKB key",
	     wake_after_s ? " or the timer" : "");
	vTaskDelay(pdMS_TO_TICKS(150));		/* let the LED and the log get out */
	esp_deep_sleep_start();
	return -EIO;
}

void power_boot_reason(void)
{
	uint32_t causes = esp_sleep_get_wakeup_causes();

#if KEY_WAKE
	/* Waking from deep sleep resets the main cores, not the RTC domain: the
	 * low-power core would go on polling the keyboard every 100 ms, on pins
	 * the CardKB driver is about to take back. */
	if (esp_reset_reason() == ESP_RST_DEEPSLEEP)
		ulp_riscv_halt();
	if (causes & BIT(ESP_SLEEP_WAKEUP_ULP)) {
		klog("power: woke from suspend, key 0x%02lx", (unsigned long)ulp_key);
		return;
	}
#endif
	if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER))
		klog("power: woke from suspend on the timer");
}
