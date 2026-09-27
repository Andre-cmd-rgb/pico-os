/*
 * The I2C buses.
 *
 * Several devices share one pair of wires: on the Freenove board the audio
 * codec, the keyboard header and the touch controller are all on the same
 * two pins. Only one driver may create that bus, so they ask for it here
 * and the first one to ask brings it up.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "drivers/drivers.h"

#define MAX_BUSES	2

static struct {
	int			sda, scl;
	i2c_master_bus_handle_t	handle;
} buses[MAX_BUSES];

static int nbuses;

/* Does anything answer at that address? For i2cdetect. */
int i2c_bus_probe(int sda, int scl, int addr)
{
	i2c_master_bus_handle_t bus;
	int ret = i2c_bus_get(sda, scl, (struct i2c_master_bus_t **)&bus);

	if (ret)
		return ret;
	return i2c_master_probe(bus, addr, 50) ? -ENODEV : 0;
}

int i2c_bus_get(int sda, int scl, i2c_master_bus_handle_t *out)
{
	for (int i = 0; i < nbuses; i++) {
		if (buses[i].sda == sda && buses[i].scl == scl) {
			*out = buses[i].handle;
			return 0;
		}
	}
	if (nbuses == MAX_BUSES)
		return -ENOSPC;

	const i2c_master_bus_config_t cfg = {
		.i2c_port = -1,			/* any free controller */
		.sda_io_num = sda,
		.scl_io_num = scl,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	i2c_master_bus_handle_t handle;

	esp_log_level_set("i2c.master", ESP_LOG_NONE);
	if (i2c_new_master_bus(&cfg, &handle)) {
		klog("i2c: bus on SDA %d / SCL %d failed", sda, scl);
		return -EIO;
	}
	/*
	 * Kept as they are through light sleep, pull-ups and all: ESP-IDF
	 * isolates every pin while the chip dozes, and the CardKB's lines
	 * have no pull-ups but the chip's own.
	 */
	gpio_sleep_sel_dis(sda);
	gpio_sleep_sel_dis(scl);
	buses[nbuses++] = (typeof(buses[0])){ sda, scl, handle };
	*out = handle;
	return 0;
}
