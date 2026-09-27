/*
 * The board's WS2812 RGB LED as a status light, driven through RMT.
 *
 *   LED_ON		steady, used while booting
 *   LED_HEARTBEAT	two short beats per second and a bit, like Linux's
 *			heartbeat trigger: the kernel is alive
 *   LED_CHARGE		the default: the board has no charge light of its
 *			own, so this is it -- the colour while the cell
 *			charges, green once it is full, dark otherwise.
 *			On the cell it costs nothing and wakes nothing.
 *   LED_OFF
 *
 * The color defaults to the theme's phosphor, dimmed by
 * CONFIG_PT_LED_BRIGHTNESS.
 */
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_LED

#include "driver/gpio.h"
#include "driver/rmt_tx.h"

#define RESOLUTION_HZ	10000000	/* 100 ns per tick */

static rmt_channel_handle_t chan;
static rmt_encoder_handle_t encoder;
static TaskHandle_t	    task;
static enum led_mode	    mode = LED_ON;
static uint8_t		    color[3];	/* r, g, b at full scale */

/* The RMT channel holds a "CPU at full speed" lock while it is enabled, so
 * it is only enabled for the few milliseconds a color takes to send. */
static void ws2812_write(uint8_t r, uint8_t g, uint8_t b)
{
	const uint8_t grb[3] = { g, r, b };
	const rmt_transmit_config_t cfg = { .loop_count = 0 };

	if (rmt_enable(chan) != ESP_OK)
		return;
	rmt_transmit(chan, encoder, grb, sizeof(grb), &cfg);
	rmt_tx_wait_all_done(chan, 100);
	rmt_disable(chan);
}

static void show(bool on)
{
	const unsigned k = CONFIG_PT_LED_BRIGHTNESS;

	if (on)
		ws2812_write(color[0] * k / 255, color[1] * k / 255, color[2] * k / 255);
	else
		ws2812_write(0, 0, 0);
}

/* What the charge light should show: 0 dark, 1 charging, 2 full. */
static int charge_light(void)
{
	struct battery_status b;

	if (battery_status(&b))
		return 0;
	return b.state == BATTERY_CHARGING ? 1 : b.state == BATTERY_FULL ? 2 : 0;
}

static void led_task(void *arg)
{
	/* beat, pause, beat, long pause: milliseconds on/off */
	static const uint16_t heartbeat[] = { 70, 130, 70, 1000 };
	const unsigned k = CONFIG_PT_LED_BRIGHTNESS;
	int step = 0, shown = -1;

	for (;;) {
		if (mode != LED_CHARGE)
			shown = -1;
		switch (mode) {
		case LED_CHARGE: {
			int want = charge_light();

			if (want != shown) {
				if (want == 2)
					ws2812_write(0, k / 2, 0);
				else
					show(want == 1);
				shown = want;
			}
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
			break;
		}
		case LED_HEARTBEAT:
			show(step % 2 == 0);
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(heartbeat[step]));
			step = (step + 1) % 4;
			break;
		case LED_ON:
		case LED_OFF:
			show(mode == LED_ON);
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			step = 0;
			break;
		}
	}
}

int led_init(void)
{
	const rmt_tx_channel_config_t chan_cfg = {
		.gpio_num = CONFIG_PT_LED_GPIO,
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = RESOLUTION_HZ,
		.mem_block_symbols = 48,
		.trans_queue_depth = 4,
	};
	/* WS2812 bit timing: 0 = 0.3 us high + 0.9 us low, 1 = the reverse (in 100 ns ticks) */
	const rmt_bytes_encoder_config_t enc_cfg = {
		.bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
		.bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
		.flags.msb_first = 1,
	};

#if CONFIG_PT_THEME_GREEN
	led_set_color(0x33, 0xff, 0x66);
#else
	led_set_color(0xff, 0xb0, 0x00);
#endif
	if (rmt_new_tx_channel(&chan_cfg, &chan) || rmt_new_bytes_encoder(&enc_cfg, &encoder)) {
		klog("led: RMT setup failed on GPIO%d", CONFIG_PT_LED_GPIO);
		return -EIO;
	}
	/* held low through light sleep: floating, the LED can latch noise */
	gpio_sleep_sel_dis(CONFIG_PT_LED_GPIO);
	show(true);
	xTaskCreatePinnedToCore(led_task, "kled", 1536, NULL, 1, &task, 0);
	return 0;
}

void led_set_mode(enum led_mode m)
{
	mode = m;
	if (task)
		xTaskNotifyGive(task);
}

enum led_mode led_get_mode(void)
{
	return mode;
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
	color[0] = r;
	color[1] = g;
	color[2] = b;
	if (task)
		xTaskNotifyGive(task);
}

void led_get_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
	*r = color[0];
	*g = color[1];
	*b = color[2];
}

#else

int led_init(void) { return -ENODEV; }
void led_set_mode(enum led_mode m) { }
enum led_mode led_get_mode(void) { return LED_OFF; }
void led_set_color(uint8_t r, uint8_t g, uint8_t b) { }
void led_get_color(uint8_t *r, uint8_t *g, uint8_t *b) { *r = *g = *b = 0; }

#endif
