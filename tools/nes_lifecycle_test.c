/* Actual NES port, with allocation, clock and lifecycle faults at its boundary. */
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes/nes.h"
#include "nofrendo.h"
#include "../emu/emu.h"
#include "pt/sys.h"
#include "pt/keys.h"

typedef void *TaskHandle_t;
struct semaphore { TaskHandle_t holder; };
typedef struct semaphore StaticSemaphore_t;
typedef struct semaphore *SemaphoreHandle_t;
struct pt_file;
struct pt_file_ops { int (*ioctl)(struct pt_file *, int, void *); };
struct pt_file { const struct pt_file_ops *ops; };
struct proc { struct pt_file *fd[3]; };

#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2

static TaskHandle_t task = (void *)1;
static struct proc process;
static bool (*exit_hook)(void *);
static void *exit_arg;
static void *blocks[32];
static int allocations, fail_allocation, live, shutdowns, releases, frames;
static int rate_error, latency_error, write_error, read_calls, audio_stops, tty_busy;
static bool short_audio, rom_error, pal, screenshot, forced, startup_exit, capture_busy;
static bool captured, raw, held, ctrl_c;
static int read_timeout = -1;
static int ctrl_c_calls;
static int64_t clock_us, slept_us;
static jmp_buf process_exit;
static nes_t core;
static apu_t test_apu;
static uint8_t *rom_memory;

static void klog(const char *format, ...) { (void)format; }
static struct proc *proc_current(void) { return task == (void *)1 ? &process : NULL; }
static bool proc_alive(int pid) { return pid == 1; }
static int proc_set_cleanup(bool (*hook)(void *), void *arg)
{
	exit_hook = hook;
	exit_arg = arg;
	return 0;
}
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s)
{
	s->holder = NULL;
	return s;
}
static TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t s) { return s->holder; }
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return task; }
static int xSemaphoreTake(SemaphoreHandle_t s, unsigned timeout)
{
	if (s->holder) {
		assert(!timeout);
		return 0;
	}
	s->holder = task;
	return 1;
}
static int xSemaphoreGive(SemaphoreHandle_t s)
{
	assert(s->holder == task);
	s->holder = NULL;
	return 1;
}
static void vTaskDelay(unsigned ms) { clock_us += (int64_t)ms * 1000; }
static bool vt_has_display(void) { return true; }
static bool mount_resolve(const char *path, char *out, size_t size)
{
	return (size_t)snprintf(out, size, "%s", path) < size;
}
static void *pt_malloc_caps(size_t size, unsigned caps) { (void)caps; return pt_malloc(size); }
static int64_t esp_timer_get_time(void) { return clock_us += 100; }
static bool vt_screen_begin(void) { return true; }
static void vt_screen_end(void) { }
static void vt_hold_screen(bool on) { held = on; }
static bool vt_screen_front(void) { return true; }
static unsigned vt_screen_gen(void) { return 0; }
static int lcd_width(void) { return 320; }
static int lcd_height(void) { return 240; }
static void lcd_fill(int x, int y, int w, int h, int color)
{
	(void)x; (void)y; (void)w; (void)h; (void)color;
}
static void lcd_draw(int x, int y, int w, int h, const void *pixels)
{
	(void)x; (void)y;
	assert(w == 256 && h == 24 && pixels);
}
static int audio_set_rate(int rate) { assert(rate == 16000); return rate_error; }
static int audio_set_latency(int ms) { assert(ms == 50); return latency_error; }
static ssize_t audio_write(const void *samples, size_t bytes, int channels)
{
	assert(samples && channels == 1);
	if (write_error)
		return -write_error;
	return short_audio ? (ssize_t)bytes - 2 : (ssize_t)bytes;
}
static void audio_stop(void) { assert(task == (void *)1); audio_stops++; }
static bool sd_mounted(void) { return false; }
static const char *user_home(void) { return "/home/user"; }
static int lcd_capture_begin(void)
{
	if (captured)
		return -EBUSY;
	captured = true;
	return 0;
}
static void lcd_capture_end(void) { assert(captured); captured = false; releases++; }
static int lcd_capture_try_end(void)
{
	if (capture_busy)
		return -EAGAIN;
	lcd_capture_end();
	return 0;
}
static int lcd_capture_save(const char *path) { assert(path && captured); return 0; }

size_t strlcat(char *out, const char *src, size_t size)
{
	size_t n = strlen(out), extra = strlen(src);

	if (n < size)
		snprintf(out + n, size - n, "%s", src);
	return n + extra;
}
void *pt_malloc(size_t size)
{
	void *p;

	if (++allocations == fail_allocation)
		return NULL;
	p = malloc(size ? size : 1);
	assert(p);
	for (int i = 0; i < 32; i++)
		if (!blocks[i]) {
			blocks[i] = p;
			live++;
			return p;
		}
	abort();
}
void pt_free(void *p)
{
	if (!p)
		return;
	for (int i = 0; i < 32; i++)
		if (blocks[i] == p) {
			free(p);
			blocks[i] = NULL;
			live--;
			return;
		}
	assert(!"invalid or double free");
}
int pt_getpid(void) { return 1; }
void pt_sigcatch(bool on) { assert(on); }
bool pt_interrupted(void) { return false; }
int pt_sleep_ms(uint32_t ms) { slept_us += (int64_t)ms * 1000; clock_us += (int64_t)ms * 1000; return 0; }
int pt_tty_raw(int fd, bool on) { assert(fd == PT_STDIN); raw = on; return 0; }
int pt_mkdir(const char *path) { (void)path; return 0; }

static void forced_exit(void)
{
	assert(exit_hook && rom_memory && raw);
	read_timeout = 200;
	task = (void *)2;
	if (captured) {
		capture_busy = true;
		assert(!exit_hook(exit_arg) && rom_memory && captured);
		capture_busy = false;
	}
	tty_busy = 1;
	assert(!exit_hook(exit_arg) && !rom_memory && !captured && read_timeout == 200);
	int count = shutdowns;

	assert(exit_hook(exit_arg) && shutdowns == count && !raw && read_timeout == -1);
	exit_hook = NULL;
	task = (void *)1;
	held = false;	/* VT's existing PID reaper restores the console. */
	longjmp(process_exit, 1);
}
int pt_stat(const char *path, struct pt_stat *st)
{
	(void)path; (void)st;
	if (forced && screenshot)
		forced_exit();
	return -ENOENT;
}
int pt_readkey_timeout(int fd, int timeout)
{
	assert(fd == PT_STDIN && !timeout);
	if (screenshot && !read_calls++)
		return 'p';
	if (forced && !screenshot)
		forced_exit();
	if (frames && ctrl_c) {
		int call = ctrl_c_calls++;

		if (!call)
			return PT_CTRL('c');
		return call == 1 ? PT_KEY_NONE : 'q';
	}
	return frames ? 'q' : PT_KEY_NONE;
}
static int tty_ioctl(struct pt_file *file, int request, void *arg)
{
	(void)file;
	if (request == PT_TTY_SETTIMEOUT) {
		assert(*(int *)arg == -1);
		read_timeout = -1;
		return 0;
	}
	assert(request == PT_TTY_TRYSETRAW && !*(int *)arg);
	if (tty_busy) {
		tty_busy--;
		return -EAGAIN;
	}
	raw = false;
	return 0;
}

nes_t *nes_getptr(void) { return &core; }
nes_t *nes_init(nes_type_t system, int rate, bool stereo, const char *bios)
{
	(void)system; (void)stereo; (void)bios;
	memset(&core, 0, sizeof(core));
	core.apu = &test_apu;
	test_apu.buffer = pt_malloc(640);
	test_apu.samples_per_frame = rate / 60;
	return &core;
}
int nes_loadfile(const char *path)
{
	assert(path);
	if (rom_memory)
		pt_free(rom_memory);	/* The vendor loader retires its last ROM. */
	rom_memory = pt_malloc(256);
	if (startup_exit) {
		assert(exit_hook && exit_hook(exit_arg)); /* Owner holds its guard. */
		exit_hook = NULL;
		longjmp(process_exit, 1);
	}
	core.refresh_rate = pal ? 50 : 60;
	return rom_error || !rom_memory ? -1 : 0;
}
void nes_shutdown(void)
{
	shutdowns++;
	pt_free(test_apu.buffer);
	test_apu.buffer = NULL;
	pt_free(rom_memory);
	rom_memory = NULL;
}
uint8_t *nes_setvidbuf(uint8_t *pixels) { core.vidbuf = pixels; return NULL; }
void nes_emulate(bool draw)
{
	assert(rom_memory && test_apu.buffer);
	frames++;
	clock_us += 1000;
	if (draw)
		core.blit_func(core.vidbuf);
}
void input_connect(int port, nes_dev_t device) { (void)port; (void)device; }
void input_update(int port, int state) { (void)port; (void)state; }
void *nofrendo_buildpalette(nespal_t palette, int bitdepth)
{
	(void)palette;
	assert(bitdepth == 16);
	void *p = pt_malloc(512);

	if (p)
		memset(p, 0, 512);
	return p;
}

#include "nes_port_under_test.c"

static void reset(void)
{
	assert(!live && !raw && !exit_hook && !rom_memory && read_timeout == -1);
	allocations = fail_allocation = shutdowns = releases = frames = 0;
	rate_error = latency_error = write_error = read_calls = audio_stops = tty_busy = 0;
	short_audio = rom_error = pal = screenshot = forced = startup_exit = capture_busy = false;
	captured = held = ctrl_c = false;
	ctrl_c_calls = 0;
	clock_us = slept_us = 0;
	assert(!atomic_load(&owner));
}

int main(void)
{
	const struct pt_file_ops ops = { .ioctl = tty_ioctl };
	struct pt_file tty = { .ops = &ops };
	struct nes_options opt = { .sound = false, .frameskip = 1 };
	int64_t ntsc_sleep;

	process.fd[PT_STDIN] = &tty;
	reset();
	assert(!nes_run("normal.nes", &opt) && frames == 1 && !held);
	ntsc_sleep = slept_us;
	reset(); ctrl_c = true;
	assert(!nes_run("ctrl-c.nes", &opt) && frames == 1 && ctrl_c_calls == 1);
	assert(!raw && !held && !live && !exit_hook && read_timeout == -1);
	reset(); pal = true;
	assert(!nes_run("pal.nes", &opt) && slept_us > ntsc_sleep + 2000);
	for (int fail = 1; fail <= 5; fail++) {
		reset(); fail_allocation = fail;
		int ret = nes_run("oom.nes", &opt);

		assert(ret == (fail == 5 ? -ENOEXEC : -ENOMEM));
		assert(!live && !raw && !exit_hook && !atomic_load(&owner));
	}
	reset(); rom_error = true;
	assert(nes_run("bad.nes", &opt) == -ENOEXEC);
	opt.sound = true;
	reset(); rate_error = -ENODEV;
	assert(nes_run("codec.nes", &opt) == -ENODEV && !frames);
	reset(); latency_error = -ENOMEM;
	assert(nes_run("stream.nes", &opt) == -ENOMEM && !frames);
	reset(); write_error = EIO;
	assert(nes_run("write.nes", &opt) == -EIO && frames == 1);
	reset(); short_audio = true;
	assert(nes_run("short.nes", &opt) == -EIO && frames == 1);
	opt.sound = false;
	reset(); screenshot = true;
	assert(!nes_run("capture.nes", &opt) && !captured && releases == 1);
	reset(); forced = true;
	if (!setjmp(process_exit))
		nes_run("killed.nes", &opt);
	assert(!live && !raw && !rom_memory && !atomic_load(&owner) && !audio_stops);
	reset(); screenshot = forced = true;
	if (!setjmp(process_exit))
		nes_run("capture-kill.nes", &opt);
	assert(!live && !captured && releases == 1 && !atomic_load(&owner));
	reset(); startup_exit = true;
	if (!setjmp(process_exit))
		nes_run("startup-kill.nes", &opt);
	assert(!live && !raw && !rom_memory && !atomic_load(&owner));
	reset();
	assert(!nes_run("after-kill.nes", &opt));
	reset();
	/* A completed hook can remain installed until its owner unregisters
	 * it, while another game has already acquired the shared buffers. */
	struct nes_cleanup finished = { .pid = 1 };
	finished.guard = xSemaphoreCreateMutexStatic(&finished.guard_storage);
	atomic_store(&owner, 1);
	assert(nes_cleanup_step(&finished));
	uint8_t *next_rows = pt_malloc(64), *next_video = pt_malloc(64);

	rowbuf = rowmem = next_rows;
	vidbuf = next_video;
	apu = &test_apu;
	want_shot = true;
	atomic_store(&owner, 2);
	assert(nes_cleanup_step(&finished));
	assert(rowbuf == next_rows && rowmem == next_rows && vidbuf == next_video);
	assert(apu == &test_apu && want_shot && live == 2 && atomic_load(&owner) == 2);
	pt_free(next_rows);
	pt_free(next_video);
	rowbuf = rowmem = vidbuf = NULL;
	apu = NULL;
	want_shot = false;
	atomic_store(&owner, 0);
	reset();
	puts("nes: lifecycle/timeout restoration, raw Ctrl-C, reaper retries, startup exit, allocation/audio faults and PAL pacing passed");
	return 0;
}
