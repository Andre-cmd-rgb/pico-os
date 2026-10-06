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
#include "esp_attr.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#include "freertos/semphr.h"

/*
 * One writer at a time: the console's output comes from whatever task
 * prints, and a file transfer's answers (drivers/misc/xfer.c) must go out
 * whole, not with half a line of someone's output inside them.
 */
static SemaphoreHandle_t out_mutex;

static void out_lock(bool take)
{
	if (!out_mutex)
		return;
	if (take)
		xSemaphoreTake(out_mutex, portMAX_DELAY);
	else
		xSemaphoreGive(out_mutex);
}

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

/*
 * The PC reads this port only while a terminal has it open. While one does,
 * output waits for room, so a long `cat` or a file transfer loses nothing.
 * Once a write has waited in vain the port counts as unread and output is
 * dropped without waiting, until the PC has taken all there was: a closed
 * terminal must never stall the system. Not merely until a write slips
 * through: Linux takes a port's output in bursts while a program holds it
 * open without reading (or a modem manager probes it), and waiting again
 * at each burst stalled the screen half a second a frame -- a full-screen
 * app's list, scrolled with the board on a PC. Writes go in pieces because the
 * driver takes a write whole or not at all, and never more than its buffer.
 */
#define USJ_PIECE	512
#define USJ_WAIT	pdMS_TO_TICKS(500)
#define USJ_RECHECK	pdMS_TO_TICKS(10)

static bool unread;
static bool was_connected;

static void serial_out(const char *s, size_t n)
{
	out_lock(true);
	bool connected = usb_serial_jtag_is_connected();

	/* The SDK's SOF monitor can briefly report a disconnect after a long
	 * critical section (for example a system-task snapshot). Give a
	 * previously connected host a brief interval to be observed again. A real
	 * disconnect pays this delay once, then subsequent output drops. */
	if (!connected && was_connected) {
		vTaskDelay(USJ_RECHECK ? USJ_RECHECK : 1);
		connected = usb_serial_jtag_is_connected();
	}
	was_connected = connected;
	if (!connected) {
		out_lock(false);
		return;
	}
	while (n) {
		size_t k = n < USJ_PIECE ? n : USJ_PIECE;

		if (unread && usb_serial_jtag_wait_tx_done(0) == ESP_OK)
			unread = false;		/* emptied: someone reads it */
		if (!usb_serial_jtag_write_bytes(s, k, unread ? 0 : USJ_WAIT)) {
			unread = true;
			break;
		}
		s += k;
		n -= k;
	}
	out_lock(false);
}

static int serial_read(char *buf, size_t n)
{
	/* it returns as soon as anything arrives: the wait is only for how
	 * often the task wakes with nothing, which in light sleep is a cost,
	 * and nothing needs it to wake at all */
	return usb_serial_jtag_read_bytes(buf, n, portMAX_DELAY);
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

static void serial_uninstall(void)
{
	usb_serial_jtag_vfs_use_nonblocking();
	usb_serial_jtag_driver_uninstall();
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
	out_lock(true);
	uart_write_bytes(UART_PORT, s, n);
	out_lock(false);
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

static void serial_uninstall(void)
{
	uart_vfs_dev_use_nonblocking(UART_PORT);
	uart_driver_delete(UART_PORT);
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
/*
 * What the PC sends: typing, for the terminal, and among it lines marked
 * with RS (0x1e) for a file transfer, which go to xfer.c instead. Taking
 * a marked line can wait on the card, and the PC waits with it.
 */
static void serial_rx_task(void *arg)
{
	EXT_RAM_BSS_ATTR static char line[800];
	char buf[256];
	size_t len = 0;
	bool marked = false;

	/* Init publishes the output callbacks before any buffered transfer
	 * request can be consumed by this higher-priority task. */
	ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
	for (;;) {
		int n = serial_read(buf, sizeof(buf)), from = 0;

		for (int i = 0; i < n; i++) {
			if (marked) {
				if (buf[i] == '\n') {
					while (len && line[len - 1] == '\r')
						len--;
					line[len] = '\0';
					xfer_line(line);
					marked = false;
					from = i + 1;
				} else if (len + 1 < sizeof(line)) {
					line[len++] = buf[i];
				}
				continue;
			}
			if (buf[i] == 0x1e) {
				if (i > from)
					tty_input_remote(buf + from, i - from);
				marked = true;
				len = 0;
			}
		}
		if (!marked && n > from)
			tty_input_remote(buf + from, n - from);
	}
}
#endif

int serial_console_init(void)
{
	esp_err_t err;
#if CONFIG_PT_KBD_UART
	TaskHandle_t worker;
#endif

	out_mutex = xSemaphoreCreateMutex();
	if (!out_mutex)
		return -ENOMEM;
	err = serial_install();

	if (err) {
		vSemaphoreDelete(out_mutex);
		out_mutex = NULL;
		klog("serial: driver install failed (%s)", esp_err_to_name(err));
		return -EIO;
	}
#if CONFIG_PT_KBD_UART
	if (ktask_create(serial_rx_task, "kserial", 4096, NULL, 10, &worker, 0) != pdPASS) {
		serial_uninstall();
		vSemaphoreDelete(out_mutex);
		out_mutex = NULL;
		return -ENOMEM;
	}
#endif
	tty_set_mirror(serial_out);
	xfer_set_output(serial_out);
#if CONFIG_PT_KBD_UART
	xTaskNotifyGive(worker);
#endif
	klog("serial: console on %s", SERIAL_NAME);
	return 0;
}
