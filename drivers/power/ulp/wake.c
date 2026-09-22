/*
 * Runs on the low-power RISC-V core while the system is suspended, woken by
 * its timer every 100 ms: one I2C read from the CardKB, and if a key was
 * pressed, wake the main cores.
 */
#include <stdint.h>

#include "ulp_riscv.h"
#include "ulp_riscv_i2c_ulp_core.h"
#include "ulp_riscv_utils.h"

uint32_t key;	/* read by the main cores after they boot */

int main(void)
{
	uint8_t k = 0;

	if (ulp_riscv_i2c_master_read_from_device(&k, 1) == ESP_OK && k) {
		key = k;
		ulp_riscv_wakeup_main_processor();
	}
	return 0;
}
