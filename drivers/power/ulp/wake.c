/*
 * Runs on the low-power RISC-V core while the system sleeps, woken by its
 * timer every 100 ms: it asks the CardKB for a key, and if one was
 * pressed -- the one it was told to wait for, if it was told one -- it
 * wakes the main cores.
 *
 * The keyboard may be on any of the RTC domain's pins, and the RTC I2C
 * controller only reaches four of them, so the I2C is done by hand: the
 * lines are open drain, let go to be high and pulled down to be low, at
 * about 40 kHz, which the CardKB's microcontroller keeps up with easily.
 */
#include <stdint.h>

#include "ulp_riscv.h"
#include "ulp_riscv_gpio.h"
#include "ulp_riscv_utils.h"

#define CARDKB_READ	(0x5f << 1 | 1)
#define HALF		(12 * ULP_RISCV_CYCLES_PER_US)	/* half a clock */
#define STRETCH		50	/* halves a slave may hold the clock low for */

/* Set by the main cores before they sleep, and read after they wake. */
uint32_t sda, scl;
uint32_t want;		/* the key that wakes it, or 0 for any */
uint32_t key;		/* the key that did */
uint32_t polls, answers;	/* how many times it asked, and was answered */

static void wait(void)
{
	ulp_riscv_delay_cycles(HALF);
}

static void set(uint32_t pin, int high)
{
	ulp_riscv_gpio_output_level((gpio_num_t)pin, high);
}

static int get(uint32_t pin)
{
	return ulp_riscv_gpio_get_level((gpio_num_t)pin);
}

/* One bit each way: SDA set while SCL is low, read while it is high. */
static int clock_bit(int out)
{
	int in;

	set(sda, out);
	wait();
	set(scl, 1);
	wait();
	for (int n = 0; !get(scl) && n < STRETCH; n++)
		wait();
	in = get(sda);
	set(scl, 0);
	return in;
}

static int write_byte(int b)
{
	for (int i = 7; i >= 0; i--)
		clock_bit(b >> i & 1);
	return !clock_bit(1);			/* the slave pulls SDA low: ACK */
}

static int read_byte(void)
{
	int b = 0;

	for (int i = 0; i < 8; i++)
		b = b << 1 | clock_bit(1);
	clock_bit(1);				/* NACK: one byte is all */
	return b;
}

int main(void)
{
	int k = 0;

	/* START: SDA falls while SCL is high */
	set(sda, 1);
	set(scl, 1);
	wait();
	set(sda, 0);
	wait();
	set(scl, 0);
	polls++;
	if (write_byte(CARDKB_READ)) {
		answers++;
		k = read_byte();
	}
	/* STOP: SDA rises while SCL is high */
	set(sda, 0);
	wait();
	set(scl, 1);
	wait();
	set(sda, 1);

	if (k && (!want || (uint32_t)k == want)) {
		key = k;
		ulp_riscv_wakeup_main_processor();
	}
	return 0;
}
