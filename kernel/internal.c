/*
 * Work that needs a stack in internal RAM. Process stacks live in PSRAM,
 * where there are megabytes to spare, but PSRAM is reached through the
 * cache, and writing, erasing or even reading the flash turns the cache
 * off for the moment it takes: a task whose stack is in PSRAM would lose
 * its own stack in the middle of the call, and ESP-IDF asserts rather than
 * let it. The same goes for going to sleep.
 *
 * So a process that gets as far as the flash hands the call to kflash, a
 * kernel task with a small internal stack, and waits for it. Every flash
 * call from anywhere -- LittleFS on /, NVS for the Wi-Fi's calibration --
 * comes through the wrappers below, which the linker puts in place of the
 * real functions (--wrap, in CMakeLists.txt); a caller whose stack is
 * already internal goes straight through.
 *
 * When the program itself runs from PSRAM (SPIRAM_XIP_FROM_PSRAM, as on
 * the S3) a read leaves the cache on and goes straight through too; a
 * write or an erase still turns it off while any of the flash is mapped.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_flash.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "pt/kernel.h"

/* 3 KB: a flash call, or esp_restart() and its shutdown handlers */
#define KFLASH_STACK	3072
#define KFLASH_CORE	1		/* proc.c's PROC_CORE */
#define KFLASH_PRIORITY	3		/* over PROC_PRIORITY */

#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA
#define READS_NEED_HELP	0
#else
#define READS_NEED_HELP	1
#endif

static struct {
	SemaphoreHandle_t	lock;		/* one caller at a time */
	SemaphoreHandle_t	start, done;
	TaskHandle_t		task;
	int		      (*fn)(void *arg);
	void		       *arg;
	int			ret;
} job;

static void kflash(void *unused)
{
	for (;;) {
		xSemaphoreTake(job.start, portMAX_DELAY);
		job.ret = job.fn(job.arg);
		xSemaphoreGive(job.done);
	}
}

static inline bool IRAM_ATTR stack_is_internal(void)
{
	return !esp_ptr_external_ram((const void *)esp_cpu_get_sp());
}

/*
 * The lock is a FreeRTOS mutex on purpose: while a process holds it, the
 * reaper's forced kill (proc.c) sees a lock held and waits, rather than
 * deleting a task whose call kflash is still working on.
 */
int on_internal_stack(int (*fn)(void *arg), void *arg)
{
	int ret;

	if (stack_is_internal() || !job.task)
		return fn(arg);
	xSemaphoreTake(job.lock, portMAX_DELAY);
	job.fn = fn;
	job.arg = arg;
	xSemaphoreGive(job.start);
	xSemaphoreTake(job.done, portMAX_DELAY);
	ret = job.ret;
	xSemaphoreGive(job.lock);
	return ret;
}

void internal_init(void)
{
	job.lock = xSemaphoreCreateMutex();
	job.start = xSemaphoreCreateBinary();
	job.done = xSemaphoreCreateBinary();
	/*
	 * On the programs' core, one step above them: the caller is waiting,
	 * so the handover is a switch on the spot, not a wake-up sent across
	 * to the other core.
	 */
	xTaskCreatePinnedToCore(kflash, "kflash", KFLASH_STACK, NULL, KFLASH_PRIORITY, &job.task,
				KFLASH_CORE);
}

static int restart(void *unused)
{
	esp_restart();
	return 0;
}

/* Restarting turns the cache off on the way down, like the flash. */
void restart_now(void)
{
	on_internal_stack(restart, NULL);
}

/* ------------------------------------------------------------ the flash */

struct flash_call {
	esp_flash_t	*chip;
	void		*buf;
	uint32_t	 addr, len;
};

esp_err_t __real_esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address,
				uint32_t length);
esp_err_t __real_esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address,
				 uint32_t length);
esp_err_t __real_esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len);
esp_err_t __real_esp_flash_erase_chip(esp_flash_t *chip);
esp_err_t __real_esp_flash_read_encrypted(esp_flash_t *chip, uint32_t address,
					  void *out_buffer, uint32_t length);
esp_err_t __real_esp_flash_write_encrypted(esp_flash_t *chip, uint32_t address,
					   const void *buffer, uint32_t length);

static int do_read(void *arg)
{
	struct flash_call *c = arg;

	return __real_esp_flash_read(c->chip, c->buf, c->addr, c->len);
}

static int do_write(void *arg)
{
	struct flash_call *c = arg;

	return __real_esp_flash_write(c->chip, c->buf, c->addr, c->len);
}

static int do_erase(void *arg)
{
	struct flash_call *c = arg;

	return __real_esp_flash_erase_region(c->chip, c->addr, c->len);
}

static int do_erase_chip(void *arg)
{
	struct flash_call *c = arg;

	return __real_esp_flash_erase_chip(c->chip);
}

static int do_read_encrypted(void *arg)
{
	struct flash_call *c = arg;

	return __real_esp_flash_read_encrypted(c->chip, c->addr, c->buf, c->len);
}

static int do_write_encrypted(void *arg)
{
	struct flash_call *c = arg;

	return __real_esp_flash_write_encrypted(c->chip, c->addr, c->buf, c->len);
}

/*
 * In IRAM, and straight through for an internal stack, so that a caller
 * with the cache already off -- a panic writing its dump -- still works.
 */
esp_err_t IRAM_ATTR __wrap_esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address,
					  uint32_t length)
{
	struct flash_call c = { chip, buffer, address, length };

	if (!READS_NEED_HELP || stack_is_internal())
		return __real_esp_flash_read(chip, buffer, address, length);
	return on_internal_stack(do_read, &c);
}

esp_err_t IRAM_ATTR __wrap_esp_flash_write(esp_flash_t *chip, const void *buffer,
					   uint32_t address, uint32_t length)
{
	struct flash_call c = { chip, (void *)buffer, address, length };

	if (stack_is_internal())
		return __real_esp_flash_write(chip, buffer, address, length);
	return on_internal_stack(do_write, &c);
}

esp_err_t IRAM_ATTR __wrap_esp_flash_erase_region(esp_flash_t *chip, uint32_t start,
						  uint32_t len)
{
	struct flash_call c = { chip, NULL, start, len };

	if (stack_is_internal())
		return __real_esp_flash_erase_region(chip, start, len);
	return on_internal_stack(do_erase, &c);
}

esp_err_t IRAM_ATTR __wrap_esp_flash_erase_chip(esp_flash_t *chip)
{
	struct flash_call c = { chip, NULL, 0, 0 };

	if (stack_is_internal())
		return __real_esp_flash_erase_chip(chip);
	return on_internal_stack(do_erase_chip, &c);
}

esp_err_t IRAM_ATTR __wrap_esp_flash_read_encrypted(esp_flash_t *chip, uint32_t address,
						    void *out_buffer, uint32_t length)
{
	struct flash_call c = { chip, out_buffer, address, length };

	/* an encrypted read goes through a mapping, which stops the cache */
	if (stack_is_internal())
		return __real_esp_flash_read_encrypted(chip, address, out_buffer, length);
	return on_internal_stack(do_read_encrypted, &c);
}

esp_err_t IRAM_ATTR __wrap_esp_flash_write_encrypted(esp_flash_t *chip, uint32_t address,
						     const void *buffer, uint32_t length)
{
	struct flash_call c = { chip, (void *)buffer, address, length };

	if (stack_is_internal())
		return __real_esp_flash_write_encrypted(chip, address, buffer, length);
	return on_internal_stack(do_write_encrypted, &c);
}
