/*
 * A USB memory stick, mounted on /mnt/usb.
 *
 * Plug one in and it appears; pull it out and it goes away. The
 * filesystem is FAT, as it is on the card, so the same files work on
 * both without thinking about it.
 */
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_USB_MSC

#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#define USB_BASE	"/usbfs"
#define USB_PATH	"/mnt/usb"

static msc_host_device_handle_t	 device;
static msc_host_vfs_handle_t	 vfs;
static QueueHandle_t		 events;
static uint8_t			 device_addr;

static bool usb_mounted(void)
{
	return vfs != NULL;
}

static int usb_info(uint64_t *total, uint64_t *free)
{
	return usb_mounted() && esp_vfs_fat_info(USB_BASE, total, free) == ESP_OK ? 0 : -EIO;
}

static const struct pt_mount usb_mount_entry = {
	.path = USB_PATH,
	.vfs = USB_BASE,
	.type = "vfat",
	.source = "usb",
	.present = usb_mounted,
	.info = usb_info,
};

static void msc_event(const msc_host_event_t *event, void *arg)
{
	msc_host_event_t copy = *event;

	xQueueSend(events, &copy, 0);
}

static void mount_stick(uint8_t addr)
{
	const esp_vfs_fat_mount_config_t cfg = {
		.format_if_mount_failed = false,
		.max_files = 8,
		.allocation_unit_size = 8 * 1024,
	};
	msc_host_device_info_t info;

	if (device)
		return;				/* one at a time */
	if (msc_host_install_device(addr, &device) != ESP_OK) {
		klog("usb: cannot talk to the stick");
		return;
	}
	if (msc_host_vfs_register(device, USB_BASE, &cfg, &vfs) != ESP_OK) {
		klog("usb: no filesystem on the stick (FAT only)");
		msc_host_uninstall_device(device);
		device = NULL;
		return;
	}
	device_addr = addr;
	if (msc_host_get_device_info(device, &info) == ESP_OK)
		klog("usb: %.*s, %llu MB, mounted on %s", (int)sizeof(info.iProduct),
		     (const char *)info.iProduct,
		     (uint64_t)info.sector_count * info.sector_size >> 20, USB_PATH);
	else
		klog("usb: stick mounted on %s", USB_PATH);
}

static void unmount_stick(void)
{
	if (vfs) {
		msc_host_vfs_unregister(vfs);
		vfs = NULL;
	}
	if (device) {
		msc_host_uninstall_device(device);
		device = NULL;
		klog("usb: stick removed");
	}
}

int usb_unmount(void)
{
	if (!vfs)
		return -EINVAL;
	if (vfs_mount_busy(&usb_mount_entry))
		return -EBUSY;
	unmount_stick();
	return 0;
}

static void msc_task(void *arg)
{
	msc_host_event_t event;

	for (;;) {
		if (xQueueReceive(events, &event, portMAX_DELAY) != pdTRUE)
			continue;
		if (event.event == MSC_DEVICE_CONNECTED)
			mount_stick(event.device.address);
		else if (event.event == MSC_DEVICE_DISCONNECTED)
			unmount_stick();
	}
}

int usbmsc_init(void)
{
	const msc_host_driver_config_t cfg = {
		.create_backround_task = true,
		.task_priority = 5,
		.stack_size = 4096,
		.callback = msc_event,
	};
	int ret;

	if ((ret = usb_host_start()))
		return ret;
	events = xQueueCreate(4, sizeof(msc_host_event_t));
	if (!events)
		return -ENOMEM;
	if (msc_host_install(&cfg) != ESP_OK) {
		klog("usb: mass storage driver failed to start");
		return -EIO;
	}
	if (xTaskCreatePinnedToCore(msc_task, "kusbmsc", 3072, NULL, 4, NULL, 0) != pdPASS)
		return -ENOMEM;
	mount_register(&usb_mount_entry);
	klog("usb: waiting for a memory stick");
	return 0;
}

#else

int usbmsc_init(void) { return -ENODEV; }
int usb_unmount(void) { return -ENODEV; }

#endif
