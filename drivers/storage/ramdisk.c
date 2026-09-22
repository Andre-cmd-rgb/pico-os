/*
 * RAM disks: FAT on memory that is handed out as it is used.
 *
 * /tmp is one of these, and it is 2 MB in the same sense that a Linux
 * tmpfs is: that is the most it can hold, not what it takes. A sector is
 * allocated the first time something writes to it, reads of sectors that
 * were never written give zeros, and when FatFs frees clusters it tells
 * the driver (CTRL_TRIM) and the memory goes straight back. So an empty
 * /tmp costs a few kilobytes of FAT metadata, and deleting a file really
 * does return the RAM.
 */
#include <stdio.h>
#include <string.h>

#include "diskio_impl.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#define SECTOR		4096
#define TMP_VFS		"/tmpfs"

static uint8_t	**disk[FF_VOLUMES];	/* one pointer per sector, NULL when unwritten */
static size_t	  sectors[FF_VOLUMES];
static size_t	  live[FF_VOLUMES];	/* sectors that actually exist */
static uint32_t	  disk_caps[FF_VOLUMES];

static DSTATUS ram_init(BYTE pdrv)
{
	return 0;
}

static DRESULT ram_read(BYTE pdrv, BYTE *buf, DWORD sector, UINT count)
{
	if (sector + count > sectors[pdrv])
		return RES_PARERR;
	for (UINT i = 0; i < count; i++, buf += SECTOR) {
		const uint8_t *page = disk[pdrv][sector + i];

		if (page)
			memcpy(buf, page, SECTOR);
		else
			memset(buf, 0, SECTOR);	/* never written: it is zeros */
	}
	return RES_OK;
}

static DRESULT ram_write(BYTE pdrv, const BYTE *buf, DWORD sector, UINT count)
{
	if (sector + count > sectors[pdrv])
		return RES_PARERR;
	for (UINT i = 0; i < count; i++, buf += SECTOR) {
		uint8_t **page = &disk[pdrv][sector + i];

		if (!*page) {
			*page = heap_caps_malloc(SECTOR, disk_caps[pdrv]);
			if (!*page)
				return RES_ERROR;	/* the RAM disk is full */
			live[pdrv]++;
		}
		memcpy(*page, buf, SECTOR);
	}
	return RES_OK;
}

/* FatFs says which sectors a deleted file no longer needs. */
static void ram_trim(BYTE pdrv, LBA_t first, LBA_t last)
{
	for (LBA_t i = first; i <= last && i < sectors[pdrv]; i++)
		if (disk[pdrv][i]) {
			heap_caps_free(disk[pdrv][i]);
			disk[pdrv][i] = NULL;
			live[pdrv]--;
		}
}

static DRESULT ram_ioctl(BYTE pdrv, BYTE cmd, void *buf)
{
	switch (cmd) {
	case CTRL_SYNC:
		return RES_OK;
	case GET_SECTOR_COUNT:
		*(LBA_t *)buf = sectors[pdrv];
		return RES_OK;
	case GET_SECTOR_SIZE:
		*(WORD *)buf = SECTOR;
		return RES_OK;
	case GET_BLOCK_SIZE:
		*(DWORD *)buf = 1;
		return RES_OK;
	case CTRL_TRIM:
		ram_trim(pdrv, ((LBA_t *)buf)[0], ((LBA_t *)buf)[1]);
		return RES_OK;
	}
	return RES_PARERR;
}

static const ff_diskio_impl_t ram_ops = {
	.init = ram_init,
	.status = ram_init,
	.read = ram_read,
	.write = ram_write,
	.ioctl = ram_ioctl,
};

int ramdisk_mount(const char *vfs, size_t kb, bool format)
{
	uint8_t work[SECTOR];
	char drive[8];
	BYTE pdrv;
	FATFS *fs;

	if (ff_diskio_get_drive(&pdrv) != ESP_OK)
		return -ENOSPC;
	sectors[pdrv] = kb * 1024 / SECTOR;
	disk_caps[pdrv] = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
	disk[pdrv] = heap_caps_calloc(sectors[pdrv], sizeof(*disk[pdrv]), disk_caps[pdrv]);
	if (!disk[pdrv]) {			/* no PSRAM on this board */
		disk_caps[pdrv] = MALLOC_CAP_8BIT;
		disk[pdrv] = heap_caps_calloc(sectors[pdrv], sizeof(*disk[pdrv]), disk_caps[pdrv]);
	}
	if (!disk[pdrv])
		return -ENOMEM;
	live[pdrv] = 0;
	ff_diskio_register(pdrv, &ram_ops);
	snprintf(drive, sizeof(drive), "%u:", pdrv);

	const esp_vfs_fat_conf_t conf = { .base_path = vfs, .fat_drive = drive, .max_files = 16 };
	const MKFS_PARM opt = { .fmt = FM_ANY | FM_SFD, .n_fat = 1 };
	if (esp_vfs_fat_register(&conf, &fs) != ESP_OK ||
	    (format && f_mkfs(drive, &opt, work, sizeof(work)) != FR_OK) ||
	    f_mount(fs, drive, 1) != FR_OK)
		return -EIO;
	return 0;
}

static bool tmp_ready;

static bool tmp_present(void)
{
	return tmp_ready;
}

static int tmp_info(uint64_t *total, uint64_t *free)
{
	return esp_vfs_fat_info(TMP_VFS, total, free) == ESP_OK ? 0 : -EIO;
}

static const struct pt_mount tmp_mount = {
	.path = "/tmp",
	.vfs = TMP_VFS,
	.type = "tmpfs",
	.source = "ram",
	.present = tmp_present,
	.info = tmp_info,
};

int tmpfs_init(void)
{
	int err = ramdisk_mount(TMP_VFS, CONFIG_PT_TMP_KB, true);

	if (err) {
		klog("tmpfs: %d KB RAM disk failed (%s)", CONFIG_PT_TMP_KB, strerror(-err));
		return err;
	}
	tmp_ready = true;
	mount_register(&tmp_mount);
	klog("tmpfs: /tmp holds up to %d KB, taken from RAM as it is used", CONFIG_PT_TMP_KB);
	return 0;
}
