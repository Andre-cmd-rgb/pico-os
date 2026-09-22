/*
 * The serial console: mirrors all terminal output and feeds what you type on
 * the PC into the terminal. It follows ESP-IDF's console setting
 * (menuconfig: Component config -> ESP System Settings -> Channel for console
 * output):
 *
 *   UART0 (default)	the board's COM port; the native USB port stays free
 *			for a USB keyboard
 *   USB Serial/JTAG	the native USB port; handy with a single cable, but
 *			then there is no USB keyboard
 */
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

/*
 * The PC reads this port only while a terminal has it open. While one does,
 * output waits for room, so a long `cat` or a file transfer loses nothing.
 * Once a write has waited in vain the port counts as unread and output is
 * dropped without waiting, until the PC takes something again: a closed
 * terminal must never stall the system. Writes go in pieces because the
 * driver takes a write whole or not at all, and never more than its buffer.
 */
#define USJ_PIECE	512
#define USJ_WAIT	pdMS_TO_TICKS(500)

static bool unread;

static void serial_out(const char *s, size_t n)
{
	if (!usb_serial_jtag_is_connected())
		return;
	while (n) {
		size_t k = n < USJ_PIECE ? n : USJ_PIECE;

		if (!usb_serial_jtag_write_bytes(s, k, unread ? 0 : USJ_WAIT)) {
			unread = true;
			return;
		}
		unread = false;
		s += k;
		n -= k;
	}
}

static int serial_read(char *buf, size_t n)
{
	return usb_serial_jtag_read_bytes(buf, n, pdMS_TO_TICKS(15));
}

static int serial_install(void)
{
	usb_serial_jtag_driver_config_t cfg = {
		.rx_buffer_size = 1024,
		.tx_buffer_size = 4096,
	};
	esp_err_t err = usb_serial_jtag_driver_install(&cfg);

	if (err)
		return err;
	usb_serial_jtag_vfs_use_driver();
	return 0;
}

/* While a PC is connected the driver keeps the chip awake
 * (CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION), so there is nothing to set up. */
void serial_idle_sleep(bool on)
{
}

#define SERIAL_NAME	"native USB"

#else

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "driver/uart_wakeup.h"
#include "esp_sleep.h"

#define UART_PORT	CONFIG_ESP_CONSOLE_UART_NUM

static void serial_out(const char *s, size_t n)
{
	uart_write_bytes(UART_PORT, s, n);
}

static int serial_read(char *buf, size_t n)
{
	return uart_read_bytes(UART_PORT, buf, n, pdMS_TO_TICKS(15));
}

static int serial_install(void)
{
	esp_err_t err = uart_driver_install(UART_PORT, 1024, 2048, 0, NULL, 0);

	if (err)
		return err;
	uart_vfs_dev_use_driver(UART_PORT);
	return 0;
}

/* In light sleep a UART byte wakes the chip, but that byte and a couple after
 * it are lost: the first keypress after a quiet spell may not arrive. */
void serial_idle_sleep(bool on)
{
	if (on) {
		const uart_wakeup_cfg_t wake = {
			.wakeup_mode = UART_WK_MODE_ACTIVE_THRESH,
			.rx_edge_threshold = 3,
		};
		uart_wakeup_setup(UART_PORT, &wake);
		esp_sleep_enable_uart_wakeup(UART_PORT);
	} else {
		esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_UART);
	}
}

#define SERIAL_NAME	"UART0 (COM port)"

#endif

#if CONFIG_PT_KBD_UART
static void serial_rx_task(void *arg)
{
	char buf[64];

	for (;;) {
		int n = serial_read(buf, sizeof(buf));
		if (n > 0)
			tty_input(buf, n);
	}
}
#endif

int serial_console_init(void)
{
	esp_err_t err = serial_install();

	if (err) {
		klog("serial: driver install failed (%s)", esp_err_to_name(err));
		return -EIO;
	}
	tty_set_mirror(serial_out);
#if CONFIG_PT_KBD_UART
	xTaskCreatePinnedToCore(serial_rx_task, "kserial", 3072, NULL, 10, NULL, 0);
#endif
	klog("serial: console on %s", SERIAL_NAME);
	return 0;
}
