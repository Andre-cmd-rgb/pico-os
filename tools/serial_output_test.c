#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#define portMAX_DELAY -1
#define pdMS_TO_TICKS(ms) (ms)
#define USJ_PIECE 512
#define USJ_WAIT 500
#define USJ_RECHECK 10
#define CONFIG_PT_KBD_UART 1
#define pdPASS 1
#define SERIAL_NAME "test"
typedef int esp_err_t;
typedef void *TaskHandle_t;
static pthread_mutex_t mutex;
static pthread_mutex_t *out_mutex = &mutex;
static bool unread;
static bool was_connected;
static atomic_int writers, writes, disconnected_for, delays;
static int reject_write, last_wait;
static int create_error, install_error, task_error, installed, deleted, published, uninstalled, notified;
static pthread_mutex_t *xSemaphoreCreateMutex(void) { return create_error ? NULL : &mutex; }
static void vSemaphoreDelete(pthread_mutex_t *m) { assert(m == &mutex); deleted++; }
static int serial_install(void) { assert(out_mutex == &mutex); installed++; return install_error; }
static void serial_uninstall(void) { uninstalled++; }
static void serial_rx_task(void *arg) { (void)arg; }
static int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
				  unsigned stack, void *arg, int prio, void **task, int core)
{
	assert(fn == serial_rx_task && name && stack == 4096 && !arg && prio == 10 && task && !core);
	if (!task_error)
		*task = &mutex;
	return task_error ? 0 : pdPASS;
}
static void xTaskNotifyGive(TaskHandle_t task)
{
	assert(task == &mutex && published == 2 && out_mutex == &mutex);
	notified++;
}
static const char *esp_err_to_name(int err) { (void)err; return "test error"; }
static void klog(const char *fmt, ...) { (void)fmt; }
static void serial_out(const char *s, size_t n);
static void tty_set_mirror(void (*fn)(const char *, size_t)) { assert(fn == serial_out); published++; }
static void xfer_set_output(void (*fn)(const char *, size_t)) { assert(fn == serial_out); published++; }
static bool xSemaphoreTake(pthread_mutex_t *m, int wait)
{
	if (wait == portMAX_DELAY)
		return !pthread_mutex_lock(m);
	struct timespec end;

	clock_gettime(CLOCK_REALTIME, &end);
	end.tv_sec += wait / 1000;
	return !pthread_mutex_timedlock(m, &end);
}
static void xSemaphoreGive(pthread_mutex_t *m) { assert(!pthread_mutex_unlock(m)); }
static bool usb_serial_jtag_is_connected(void)
{
	if (atomic_load(&disconnected_for) > 0) {
		atomic_fetch_sub(&disconnected_for, 1);
		return false;
	}
	return true;
}
static void vTaskDelay(int ticks)
{
	assert(ticks == USJ_RECHECK);
	atomic_fetch_add(&delays, 1);
}
static int usb_serial_jtag_write_bytes(const char *s, size_t n, int wait)
{
	(void)s;
	last_wait = wait;
	assert(atomic_fetch_add(&writers, 1) == 0);
	if (!atomic_fetch_add(&writes, 1)) {
		struct timespec delay = { 1, 300000000 };

		nanosleep(&delay, NULL);
	}
	atomic_fetch_sub(&writers, 1);
	if (reject_write) {
		reject_write = 0;
		return 0;
	}
	return n;
}
#include "serial_output_under_test.h"
static void *write_one(void *arg) { (void)arg; serial_out("x", 1); return NULL; }

int main(void)
{
	pthread_t first, second;
	struct timespec delay = { 0, 10000000 };

	create_error = 1;
	assert(serial_console_init() == -ENOMEM && !out_mutex && !installed && !published);
	create_error = 0; install_error = 1;
	assert(serial_console_init() == -EIO && !out_mutex && installed == 1 && deleted == 1 && !published);
	install_error = 0; task_error = 1;
	assert(serial_console_init() == -ENOMEM && !out_mutex && installed == 2 && deleted == 2 && uninstalled == 1 && !published && !notified);
	task_error = 0;
	assert(!serial_console_init() && out_mutex == &mutex && installed == 3 && published == 2 && notified == 1);
	assert(!pthread_mutex_init(&mutex, NULL));
	assert(!pthread_create(&first, NULL, write_one, NULL));
	while (!atomic_load(&writers)) nanosleep(&delay, NULL);
	assert(!pthread_create(&second, NULL, write_one, NULL));
	assert(!pthread_join(first, NULL) && !pthread_join(second, NULL));
	assert(atomic_load(&writes) == 2 && !atomic_load(&writers));
	atomic_store(&disconnected_for, 1);
	serial_out("transient", 9);
	assert(atomic_load(&writes) == 3 && atomic_load(&delays) == 1);
	atomic_store(&disconnected_for, 100);
	serial_out("disconnected", 12);
	serial_out("still disconnected", 18);
	assert(atomic_load(&writes) == 3 && atomic_load(&delays) == 2);
	atomic_store(&disconnected_for, 0);
	serial_out("reconnected", 11);
	assert(atomic_load(&writes) == 4 && atomic_load(&delays) == 2);
	reject_write = 1;
	serial_out("unread", 6);
	assert(unread && last_wait == USJ_WAIT);
	serial_out("reading again", 13);
	assert(!unread && !last_wait);
	assert(!pthread_mutex_destroy(&mutex));
	puts("serial output: init faults, writer ownership, transient SOF loss, disconnect and unread recovery passed");
	return 0;
}
