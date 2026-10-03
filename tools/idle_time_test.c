#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(m) assert(!pthread_mutex_lock(m))
#define portEXIT_CRITICAL(m) assert(!pthread_mutex_unlock(m))

static portMUX_TYPE time_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t last_key, last_any, save_at;
static atomic_int writers_done;
static pthread_barrier_t race_start;

#include "idle_time_under_test.h"

#define BASE ((int64_t)UINT32_MAX - 64)
#define WRITES 10000

static void *writer(void *arg)
{
	bool local = (intptr_t)arg;

	for (int i = 1; i <= WRITES; i++) {
		activity_record(BASE + i, local);
		if (i > 10)
			activity_record(BASE + i - 10, local);
	}
	atomic_fetch_add(&writers_done, 1);
	return NULL;
}

static void *later_save(void *arg)
{
	(void)arg;
	pthread_barrier_wait(&race_start);
	save_schedule(200);
	return NULL;
}

int main(void)
{
	pthread_t local, remote, setter;
	int64_t key, any, deadline, previous_key = BASE, previous_any = BASE;

	activity_record(BASE, true);
	assert(!pthread_create(&local, NULL, writer, (void *)(intptr_t)true));
	assert(!pthread_create(&remote, NULL, writer, (void *)(intptr_t)false));
	do {
		activity_times(&key, &any);
		assert(key >= previous_key && any >= previous_any && any >= key);
		assert(key <= BASE + WRITES && any <= BASE + WRITES);
		previous_key = key;
		previous_any = any;
	} while (atomic_load(&writers_done) < 2);
	assert(!pthread_join(local, NULL));
	assert(!pthread_join(remote, NULL));
	activity_times(&key, &any);
	assert(key == BASE + WRITES && any == key);
	/* Remote input keeps deep sleep away without becoming a local key. */
	activity_record(any + 1, false);
	activity_times(&key, &any);
	assert(key == BASE + WRITES && any == key + 1);

	save_schedule(100);
	save_schedule(200);
	assert(!save_take_due(100, &deadline) && deadline == 200);
	assert(save_take_due(200, &deadline) && !deadline);
	/* A change during the filesystem save survives its earlier claim. */
	save_schedule(300);
	save_schedule(250);
	assert(!save_take_due(200, &deadline) && deadline == 300);
	assert(save_take_due(300, &deadline) && !deadline);
	assert(!pthread_barrier_init(&race_start, NULL, 2));
	for (int i = 0; i < 100; i++) {
		save_schedule(100);
		assert(!pthread_create(&setter, NULL, later_save, NULL));
		pthread_barrier_wait(&race_start);
		save_take_due(100, &deadline);
		assert(!pthread_join(setter, NULL));
		/* Either ordering must retain the newer, not-yet-due request. */
		assert(!save_take_due(100, &deadline) && deadline == 200);
		assert(save_take_due(200, &deadline) && !deadline);
	}
	assert(!pthread_barrier_destroy(&race_start));
	assert(!pthread_mutex_destroy(&time_lock));
	puts("idle times: cross-core 32-bit boundary snapshots, monotonic activity and concurrent save deadlines passed");
	return 0;
}
