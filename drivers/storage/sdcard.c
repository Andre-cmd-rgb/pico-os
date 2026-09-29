/*
 * /mnt/sd: the microSD slot. Boards with a slot on board wire it to the
 * SDMMC controller (four data lines, tens of megabytes a second); Arduino
 * shields and breakouts use SPI. The card can be removed after
 * `umount /mnt/sd` and mounted again with `mount /mnt/sd`.
 */
#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#if CONFIG_PT_SD_MMC
#include "driver/sdmmc_host.h"
#if CONFIG_PT_SD_MMC_LDO >= 0
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#else
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#endif

#include "drivers/drivers.h"

#if CONFIG_PT_SD

#define SD_BASE		"/sdcard"	/* VFS prefix */
#define SD_PATH		"/mnt/sd"

static sdmmc_card_t *card;
#if CONFIG_PT_SD_MMC && CONFIG_PT_SD_MMC_LDO >= 0
static sd_pwr_ctrl_handle_t power;	/* the slot's own supply: on for good once on */
#endif
#if CONFIG_PT_SD_SPI
static bool	     bus_ready;
#endif

bool sd_mounted(void)
{
	return card != NULL;
}

static int sd_info(uint64_t *total, uint64_t *free)
{
	return card && esp_vfs_fat_info(SD_BASE, total, free) == ESP_OK ? 0 : -EIO;
}

static int sd_mount_format(void);

/*
 * The card is the home directory, so a card that has just been mounted
 * gets the directories a home directory has. Making them every time
 * costs eight failed mkdirs on a card that already has them, and means a
 * blank card bought this afternoon is laid out the moment it goes in.
 */
static void home_layout(void)
{
	static const char *const dirs[] = {
		"agenda", "bin", "music", "notes", "photos", "recordings", "roms", "video",
	};
	char path[64];

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(path, sizeof(path), SD_BASE "/%s", dirs[i]);
		mkdir(path, 0777);
	}
}

/*
 * A new filesystem on the card, throwing away what is there. The card has
 * to be mounted for the format call, so an unreadable one is mounted the
 * only way it can be: by formatting it on the way in.
 */
int sd_format(void)
{
	bool was_mounted = card != NULL;
	esp_err_t err;

	if (!was_mounted && sd_mount_format())
		return -ENODEV;
	err = esp_vfs_fat_sdcard_format(SD_BASE, card);
	if (err) {
		klog("sd: format failed (%s)", esp_err_to_name(err));
		return -EIO;
	}
	home_layout();
	klog("sd: card formatted");
	return 0;
}

static const struct pt_mount sd_mount_entry = {
	.path = SD_PATH,
	.vfs = SD_BASE,
	.type = "vfat",
	.source = "sdcard",
	.present = sd_mounted,
	.info = sd_info,
};

/*
 * The same card again, as the home directory. Everything a person makes
 * on this machine -- roms, music, photos, notes, scripts -- belongs on
 * the card, which a PC can read by being handed the card rather than a
 * cable; the flash keeps the system, so the machine still boots with an
 * empty slot. With no card in, this mount is not present and
 * /home/<user> is the empty directory on the flash underneath it, which
 * is what a missing disk ought to look like. No path: it is wherever the
 * user's home is, which `setup` can move.
 */
static const struct pt_mount sd_home_entry = {
	.path = NULL,
	.vfs = SD_BASE,
	.type = "vfat",
	.source = SD_PATH,
	.bind = true,
	.present = sd_mounted,
	.info = sd_info,
};

#if CONFIG_PT_SD_MMC

static int sd_mount_common(bool format);

int sd_mount(void)
{
	return sd_mount_common(false);
}

/* Mounting a card with nothing readable on it: make a filesystem first. */
static int sd_mount_format(void)
{
	return sd_mount_common(true);
}

/* The four-wire bus on boards whose slot is soldered on. */
static int sd_mount_common(bool format)
{
	sdmmc_host_t host = SDMMC_HOST_DEFAULT();
	sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
	const esp_vfs_fat_mount_config_t cfg = {
		.format_if_mount_failed = format,
		.max_files = 16,
		.allocation_unit_size = 16 * 1024,
	};

	if (card)
		return -EBUSY;
	host.slot = CONFIG_PT_SD_MMC_SLOT;
#if CONFIG_PT_SD_MMC_LDO >= 0
	if (!power) {
		const sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = CONFIG_PT_SD_MMC_LDO };

		if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &power)) {
			klog("sd: LDO %d, which powers the slot, would not start", CONFIG_PT_SD_MMC_LDO);
			return -EIO;
		}
	}
	host.pwr_ctrl_handle = power;
#endif
	/*
	 * The controller can only read into internal memory, and almost
	 * everything a program reads into is in PSRAM. Left at its default
	 * the driver then bounces each 512-byte sector through a buffer of
	 * its own, one card command apiece -- 18 commands and 4 ms for one
	 * frame of video. Sixteen sectors a command is an 8 KB buffer for
	 * the length of the read, and it shrinks if there is not that much.
	 */
	host.unaligned_multi_block_rw_max_chunk_size = 16;
	slot.clk = CONFIG_PT_SD_MMC_CLK;
	slot.cmd = CONFIG_PT_SD_MMC_CMD;
	slot.d0 = CONFIG_PT_SD_MMC_D0;
	slot.d1 = CONFIG_PT_SD_MMC_D1;
	slot.d2 = CONFIG_PT_SD_MMC_D2;
	slot.d3 = CONFIG_PT_SD_MMC_D3;
	slot.width = CONFIG_PT_SD_MMC_D1 >= 0 && CONFIG_PT_SD_MMC_D2 >= 0 &&
		     CONFIG_PT_SD_MMC_D3 >= 0 ? 4 : 1;
	slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

	esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_BASE, &host, &slot, &cfg, &card);
	if (err) {
		card = NULL;
		klog("sd: no usable card (%s)", esp_err_to_name(err));
		return err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND ? -ENODEV : -EIO;
	}
	char desc[80];
	sd_describe(desc, sizeof(desc));
	home_layout();
	klog("sd: %s on %d-bit sdmmc, mounted on %s and %s", desc, slot.width,
	     SD_PATH, user_home());
	return 0;
}

#else /* CONFIG_PT_SD_SPI */

static int sd_mount_common(bool format);

int sd_mount(void)
{
	return sd_mount_common(false);
}

static int sd_mount_format(void)
{
	return sd_mount_common(true);
}

static int sd_mount_common(bool format)
{
	if (!bus_ready)
		return -ENODEV;
	if (card)
		return -EBUSY;

	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	host.slot = SPI2_HOST;
	host.max_freq_khz = CONFIG_PT_SD_FREQ_KHZ;
	host.unaligned_multi_block_rw_max_chunk_size = 16;	/* see above */

	sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot.gpio_cs = CONFIG_PT_SD_CS;
	slot.host_id = SPI2_HOST;

	const esp_vfs_fat_mount_config_t cfg = {
		.format_if_mount_failed = format,
		.max_files = 16,
		.allocation_unit_size = 16 * 1024,
	};
	/* When no card answers, the SD driver switches its CS pin back to an
	 * input while the pin is still reserved as an output, and the GPIO
	 * driver warns "conflict found for GPIO[cs]". Nothing is wrong. */
	esp_log_level_set("gpio", ESP_LOG_ERROR);
	esp_err_t err = esp_vfs_fat_sdspi_mount(SD_BASE, &host, &slot, &cfg, &card);
	esp_log_level_set("gpio", ESP_LOG_WARN);

	if (err) {
		card = NULL;
		klog("sd: no usable card (%s)", esp_err_to_name(err));
		return err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND ? -ENODEV : -EIO;
	}
	char desc[80];
	sd_describe(desc, sizeof(desc));
	home_layout();
	klog("sd: %s, mounted on %s and %s", desc, SD_PATH, user_home());
	return 0;
}

/* Refused while files are open on the card: their descriptors would go stale
 * and could later write into whatever file is given the same number. */
#endif

int sd_unmount(void)
{
	if (!card)
		return -EINVAL;
	if (vfs_mount_busy(&sd_mount_entry))
		return -EBUSY;
	esp_log_level_set("gpio", ESP_LOG_ERROR);	/* same CS pin quirk as above */
	esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_BASE, card);
	esp_log_level_set("gpio", ESP_LOG_WARN);
	if (err)
		return -EBUSY;
	card = NULL;
	klog("sd: unmounted");
	return 0;
}

int sd_describe(char *buf, size_t size)
{
	if (!card)
		return snprintf(buf, size, "no card");
	uint64_t bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
	return snprintf(buf, size, "%s %llu MB", card->cid.name, bytes >> 20);
}

int sd_init(void)
{
	/* The IDF logs every failed probe as an error; the kernel says it once. */
	esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
	esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
	esp_log_level_set("sdspi_host", ESP_LOG_NONE);
	esp_log_level_set("sdspi_transaction", ESP_LOG_NONE);
	esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
	esp_log_level_set("SD_HOST", ESP_LOG_NONE);	/* "input line delay not supported" */

#if CONFIG_PT_SD_SPI
	const spi_bus_config_t bus = {
		.mosi_io_num = CONFIG_PT_SD_MOSI,
		.miso_io_num = CONFIG_PT_SD_MISO,
		.sclk_io_num = CONFIG_PT_SD_SCK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4096,
	};
	esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA);

	if (err) {
		klog("sd: SPI bus setup failed (%s)", esp_err_to_name(err));
		return -EIO;
	}
	bus_ready = true;
#endif
	mount_register(&sd_mount_entry);
	mount_register(&sd_home_entry);
	return sd_mount();
}

#else

bool sd_mounted(void) { return false; }
int sd_init(void) { return -ENODEV; }
int sd_mount(void) { return -ENODEV; }
int sd_unmount(void) { return -ENODEV; }
int sd_format(void) { return -ENODEV; }
int sd_describe(char *buf, size_t size) { return snprintf(buf, size, "disabled"); }

#endif
