/*
 * The USB host stack, shared.
 *
 * The chip has one USB port and it can host more than one thing at a
 * time -- a keyboard and a memory stick, say -- but the stack underneath
 * may only be installed once. Each driver asks for it here and the first
 * one to ask brings it up, the same arrangement as the shared I2C bus.
 *
 * Note what hosting means electrically: the board has to supply 5 V to
 * whatever is plugged in, and on a board whose USB socket is a power
 * *input* that means a powered hub or an adapter that injects power.
 */
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_USB_HOST

#include "usb/usb_host.h"

#define NOTIFY_OK	1
#define NOTIFY_FAIL	2

static bool running;

static void usb_lib_task(void *arg)
{
	const usb_host_config_t cfg = {
		.skip_phy_setup = false,
		.intr_flags = ESP_INTR_FLAG_LEVEL1,
	};

	if (usb_host_install(&cfg) != ESP_OK) {
		klog("usb: host stack failed to start");
		xTaskNotify(arg, NOTIFY_FAIL, eSetValueWithOverwrite);
		vTaskDelete(NULL);
	}
	xTaskNotify(arg, NOTIFY_OK, eSetValueWithOverwrite);
	for (;;) {
		uint32_t flags = 0;

		usb_host_lib_handle_events(portMAX_DELAY, &flags);
		if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
			usb_host_device_free_all();
	}
}

int usb_host_start(void)
{
	uint32_t answer = 0;

	if (running)
		return 0;
	if (xTaskCreatePinnedToCore(usb_lib_task, "kusb", 4096, xTaskGetCurrentTaskHandle(),
				    5, NULL, 0) != pdPASS)
		return -ENOMEM;
	xTaskNotifyWait(0, UINT32_MAX, &answer, pdMS_TO_TICKS(2000));
	if (answer != NOTIFY_OK)
		return -EIO;
	running = true;
	return 0;
}

bool usb_host_running(void)
{
	return running;
}

#else

int  usb_host_start(void) { return -ENODEV; }
bool usb_host_running(void) { return false; }

#endif
