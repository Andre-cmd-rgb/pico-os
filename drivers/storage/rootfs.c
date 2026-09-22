/*
 * The root filesystem on the internal flash "storage" partition: LittleFS
 * by default, FAT behind wear levelling as an option, or a RAM disk for
 * QEMU. It is formatted the first time it fails to mount.
 */
#include <string.h>

#include "sdkconfig.h"

#include "drivers/drivers.h"

#define ROOT_VFS	"/rootfs"
#define ROOT_LABEL	"storage"

static bool mounted;

static bool root_present(void)
{
	return mounted;
}

#if CONFIG_PT_ROOTFS_LITTLEFS

#include "esp_littlefs.h"

#define ROOT_TYPE	"littlefs"
#define ROOT_SOURCE	"flash:" ROOT_LABEL

static int root_info(uint64_t *total, uint64_t *free)
{
	size_t t = 0, used = 0;

	if (esp_littlefs_info(ROOT_LABEL, &t, &used) != ESP_OK)
		return -EIO;
	*total = t;
	*free = t - used;
	return 0;
}

static int root_mount(void)
{
	const esp_vfs_littlefs_conf_t conf = {
		.base_path = ROOT_VFS,
		.partition_label = ROOT_LABEL,
		.format_if_mount_failed = true,
	};
	esp_err_t err = esp_vfs_littlefs_register(&conf);

	if (err)
		klog("rootfs: littlefs mount failed (%s)", esp_err_to_name(err));
	return err ? -EIO : 0;
}

#elif CONFIG_PT_ROOTFS_FAT

#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#define ROOT_TYPE	"vfat"
#define ROOT_SOURCE	"flash:" ROOT_LABEL

static int root_info(uint64_t *total, uint64_t *free)
{
	return esp_vfs_fat_info(ROOT_VFS, total, free) == ESP_OK ? 0 : -EIO;
}

static int root_mount(void)
{
	const esp_vfs_fat_mount_config_t cfg = {
		.format_if_mount_failed = true,
		.max_files = 16,
		.allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
	};
	wl_handle_t wl;
	esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(ROOT_VFS, ROOT_LABEL, &cfg, &wl);

	if (err)
		klog("rootfs: fat mount failed (%s)", esp_err_to_name(err));
	return err ? -EIO : 0;
}

#else /* CONFIG_PT_ROOTFS_RAMDISK */

#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"

#define ROOT_TYPE	"ramfs"
#define ROOT_SOURCE	"ram"

static int root_info(uint64_t *total, uint64_t *free)
{
	return esp_vfs_fat_info(ROOT_VFS, total, free) == ESP_OK ? 0 : -EIO;
}

static int root_mount(void)
{
	bool psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 4 * 1024 * 1024;
	int err = ramdisk_mount(ROOT_VFS, psram ? 2048 : 192, true);

	if (err)
		klog("rootfs: RAM disk failed (%s)", strerror(-err));
	return err;
}

#endif

static const struct pt_mount root_mount_entry = {
	.path = "/",
	.vfs = ROOT_VFS,
	.type = ROOT_TYPE,
	.source = ROOT_SOURCE,
	.present = root_present,
	.info = root_info,
};

/*
 * Back to an empty filesystem. The commands live in the application
 * partition, not in here, so the machine keeps working with the root
 * filesystem unmounted -- but anything with a file open on it will get
 * an error, which is why this is the last thing a factory reset does
 * before rebooting.
 */
int rootfs_format(void)
{
	int err;

	if (!mounted)
		return -ENODEV;
	vfs_sync_all();
	mounted = false;
#if CONFIG_PT_ROOTFS_LITTLEFS
	esp_vfs_littlefs_unregister(ROOT_LABEL);
	if (esp_littlefs_format(ROOT_LABEL)) {
		klog("rootfs: format failed");
		return -EIO;
	}
#else
	klog("rootfs: this filesystem cannot be formatted in place");
	return -ENOTSUP;
#endif
	if ((err = root_mount()))
		return err;
	mounted = true;
	klog("rootfs: formatted");
	return 0;
}

int rootfs_init(void)
{
	int err = root_mount();

	if (err)
		return err;
	mounted = true;
	mount_register(&root_mount_entry);

	uint64_t total = 0, free = 0;
	root_info(&total, &free);
	klog("rootfs: / is %s on %s, %llu KB free of %llu KB", ROOT_TYPE, ROOT_SOURCE,
	     free / 1024, total / 1024);
	return 0;
}
