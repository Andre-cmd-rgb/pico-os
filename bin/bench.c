/*
 * bench - measure this board: CPU, FPU, memory, processes, storage, display.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "drivers/drivers.h"
#include "util.h"

#define RUN_US		1000000		/* each timed loop runs about a second */

static int64_t now(void)
{
	return pt_uptime_us();
}

static void header(void)
{
	int min, max;

	cpufreq_get(&min, &max);
	pt_printf("\x1b[1mbench\x1b[0m at %d MHz (%s)\n", cpufreq_current_mhz(), cpufreq_policy_name(min, max));
}

static volatile uint32_t crc_result;	/* read by nobody; stops the loop being optimised away */

/* table-driven CRC-32 over a buffer in internal RAM: integer work */
static int bench_cpu(void)
{
	static uint32_t table[256];
	uint8_t buf[4096];
	uint64_t bytes = 0;
	uint32_t crc = 0xffffffff;

	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xedb88320 ^ (c >> 1) : c >> 1;
		table[i] = c;
	}
	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = i * 131;

	int64_t start = now(), end = start + RUN_US;
	while (now() < end && !pt_interrupted()) {
		for (int pass = 0; pass < 64; pass++) {
			for (size_t i = 0; i < sizeof(buf); i++)
				crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
			bytes += sizeof(buf);
		}
		crc_result = crc;
	}
	double s = (now() - start) / 1e6;
	pt_printf("cpu    crc32       %7.2f MB/s\n", bytes / s / (1 << 20));
	return 0;
}

/* Mandelbrot 320x240 with single-precision floats: the S3's FPU */
static int bench_fpu(void)
{
	int64_t start = now();
	unsigned long iterations = 0;

	for (int y = 0; y < 240 && !pt_interrupted(); y++) {
		for (int x = 0; x < 320; x++) {
			float cr = -2.2f + x * (3.2f / 320), ci = -1.2f + y * (2.4f / 240);
			float zr = 0, zi = 0;
			int i = 0;
			while (i < 128 && zr * zr + zi * zi < 4.0f) {
				float t = zr * zr - zi * zi + cr;
				zi = 2.0f * zr * zi + ci;
				zr = t;
				i++;
			}
			iterations += i;
		}
	}
	double ms = (now() - start) / 1e3;
	pt_printf("fpu    mandelbrot  %7.0f ms  (%.1f M iter/s)\n", ms, iterations / (ms / 1e3) / 1e6);
	return 0;
}

/*
 * Two buffers, copied back and forth. Internal memory is scarce on a
 * machine carrying four terminals and a radio, so take what is there
 * instead of refusing to measure: a smaller pair of buffers still says
 * what the bus does, and the size is printed beside the number so it is
 * clear what was measured.
 */
static void bench_copy(const char *label, uint32_t caps, size_t size)
{
	size_t largest = heap_caps_get_largest_free_block(caps);
	uint8_t *a, *b;
	uint64_t bytes = 0;

	if (largest / 2 < size)
		size = largest / 2 & ~(size_t)(1024 - 1);
	a = size >= 1024 ? heap_caps_malloc(size, caps) : NULL;
	b = a ? heap_caps_malloc(size, caps) : NULL;
	if (!a || !b) {
		pt_printf("mem    %-11s no memory (%zu KB free, largest %zu KB)\n",
			  label, heap_caps_get_free_size(caps) / 1024, largest / 1024);
		heap_caps_free(a);
		heap_caps_free(b);
		return;
	}
	memset(a, 0x5a, size);
	int64_t start = now(), end = start + RUN_US / 2;
	while (now() < end && !pt_interrupted()) {
		memcpy(b, a, size);
		bytes += size;
	}
	double s = (now() - start) / 1e6;
	pt_printf("mem    %-11s %7.2f MB/s  (%zu KB blocks)\n", label,
		  bytes / s / (1 << 20), size / 1024);
	heap_caps_free(a);
	heap_caps_free(b);
}

static int bench_mem(void)
{
	bench_copy("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 16 * 1024);
	bench_copy("psram", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, 1024 * 1024);
	return 0;
}

static int bench_spawn(void)
{
	char *argv[] = { "true", NULL };
	const struct pt_spawn req = {
		.cmd = "true", .argc = 1, .argv = argv,
		.fd = { PT_STDIN, PT_STDOUT, PT_STDERR },
	};
	int runs = 0, status;
	int64_t start = now();

	while (runs < 100 && !pt_interrupted()) {
		int pid = pt_spawn(&req);
		if (pid < 0)
			return fail("bench", "spawn", pid);
		pt_wait(pid, &status, false);
		runs++;
	}
	pt_printf("spawn  process     %7.2f ms each\n", (now() - start) / 1e3 / runs);
	return 0;
}

static int bench_fs(const char *dir)
{
	const size_t total = 512 * 1024, chunk = 4096;
	char path[PT_PATH_MAX];
	uint8_t *buf = pt_malloc(chunk);
	uint32_t sum_w = 0, sum_r = 0;
	int err = 0;

	if (!buf)
		return fail("bench", NULL, -ENOMEM);
	if (!join_path(dir, ".bench.tmp", path, sizeof(path)))
		return fail("bench", dir, -ENAMETOOLONG);
	int fd = pt_open(path, O_WRONLY | O_CREAT | O_TRUNC);
	if (fd < 0)
		return fail("bench", path, fd);

	int64_t start = now();
	for (size_t done = 0; done < total && !err; done += chunk) {
		for (size_t i = 0; i < chunk; i++) {
			buf[i] = (done + i) * 7;
			sum_w += buf[i];
		}
		err = write_all(fd, (const char *)buf, chunk);
	}
	pt_close(fd);
	double write_s = (now() - start) / 1e6;
	if (err) {
		pt_unlink(path);
		return fail("bench", path, err);
	}

	fd = pt_open(path, O_RDONLY);
	start = now();
	ssize_t n;
	size_t read_total = 0;
	while (fd >= 0 && (n = pt_read(fd, buf, chunk)) > 0) {
		for (ssize_t i = 0; i < n; i++)
			sum_r += buf[i];
		read_total += n;
	}
	double read_s = (now() - start) / 1e6;
	if (fd >= 0)
		pt_close(fd);
	pt_unlink(path);
	pt_free(buf);

	pt_printf("fs     %-6s write %6.0f KB/s  read %6.0f KB/s%s\n", dir, total / 1024 / write_s,
		  read_total / 1024 / read_s, read_total == total && sum_r == sum_w ? "" : "  DATA MISMATCH");
	return read_total == total && sum_r == sum_w ? 0 : 1;
}

/* Line-at-a-time writing: a log, a script, the shell's history. */
static int bench_lines(const char *dir)
{
	const int lines = 2000;
	char path[PT_PATH_MAX];
	int64_t start;

	if (!join_path(dir, ".bench.lines", path, sizeof(path)))
		return fail("bench", dir, -ENAMETOOLONG);
	int fd = pt_open(path, O_WRONLY | O_CREAT | O_TRUNC);
	if (fd < 0)
		return fail("bench", path, fd);
	start = now();
	for (int i = 0; i < lines && !pt_interrupted(); i++)
		pt_dprintf(fd, "line %d of the log, written one line at a time\n", i);
	pt_close(fd);				/* the close flushes */
	double s = (now() - start) / 1e6;
	pt_unlink(path);
	pt_printf("lines  %-6s %7.0f lines/s  (%.1f us each)\n", dir, lines / s, s * 1e6 / lines);
	return 0;
}

/* Many small files: what a shell and an editor actually do to a disk. */
static int bench_files(const char *dir)
{
	const int count = 100;
	char path[PT_PATH_MAX], name[32];
	char data[1024];
	int64_t t[4];

	memset(data, 'x', sizeof(data));
	if (!join_path(dir, ".bench000", path, sizeof(path)))
		return fail("bench", dir, -ENAMETOOLONG);
	t[0] = now();
	for (int i = 0; i < count && !pt_interrupted(); i++) {
		snprintf(name, sizeof(name), ".bench%03d", i);
		int fd = pt_open(join_path(dir, name, path, sizeof(path)), O_WRONLY | O_CREAT | O_TRUNC);
		if (fd < 0)
			return fail("bench", path, fd);
		write_all(fd, data, sizeof(data));
		pt_close(fd);
	}
	t[1] = now();
	for (int i = 0; i < count && !pt_interrupted(); i++) {
		snprintf(name, sizeof(name), ".bench%03d", i);
		int fd = pt_open(join_path(dir, name, path, sizeof(path)), O_RDONLY);
		if (fd < 0)
			return fail("bench", path, fd);
		pt_read(fd, data, sizeof(data));
		pt_close(fd);
	}
	t[2] = now();
	for (int i = 0; i < count; i++) {
		snprintf(name, sizeof(name), ".bench%03d", i);
		pt_unlink(join_path(dir, name, path, sizeof(path)));
	}
	t[3] = now();
	pt_printf("files  %-6s create %4.1f ms  read %4.1f ms  delete %4.1f ms  (each, 1 KB)\n", dir,
		  (t[1] - t[0]) / 1e3 / count, (t[2] - t[1]) / 1e3 / count, (t[3] - t[2]) / 1e3 / count);
	return 0;
}

static int bench_lcd(void)
{
	if (!vt_has_display()) {
		pt_printf("lcd    disabled in menuconfig\n");
		return 0;
	}
	static const uint16_t colors[] = { 0xf800, 0x07e0, 0x001f, 0x0000 };
	int w = lcd_width(), h = lcd_height(), frames = 0;
	int64_t start = now();

	while (frames < 20 && !pt_interrupted())
		lcd_fill(0, 0, w, h, colors[frames++ % 4]);
	double s = (now() - start) / 1e6;
	vt_redraw();
	pt_printf("lcd    full frame  %7.1f fps  (%.2f MB/s on the bus)\n", frames / s,
		  frames * w * h * 2 / s / (1 << 20));
	return 0;
}

/* Busy loop with no system calls: only kill -9 can stop it. */
static int bench_hog(void)
{
	volatile unsigned long spins = 0;

	pt_printf("hog: spinning without system calls; stop it with kill -9 %d\n", pt_getpid());
	for (;;)
		spins++;
	return 0;
}

PT_PROGRAM_STACK(bench, 12, "measure speed\n"
		 "usage: bench [all | cpu | fpu | mem | spawn | lcd |\n"
		 "              fs [dir] | lines [dir] | files [dir] | hog]\n"
		 "  fs writes and reads a 512 KB file in dir (default /; try /tmp and /mnt/sd)\n"
		 "  hog spins forever without system calls, to test kill -9")
{
	const char *what = argc > 1 ? argv[1] : "all";
	int status = 0;

	if (!strcmp(what, "hog"))
		return bench_hog();
	header();
	if (!strcmp(what, "all") || !strcmp(what, "cpu"))
		status |= bench_cpu();
	if (!strcmp(what, "all") || !strcmp(what, "fpu"))
		status |= bench_fpu();
	if (!strcmp(what, "all") || !strcmp(what, "mem"))
		status |= bench_mem();
	if (!strcmp(what, "all") || !strcmp(what, "spawn"))
		status |= bench_spawn();
	if (!strcmp(what, "all") || !strcmp(what, "fs"))
		status |= bench_fs(argc > 2 ? argv[2] : "/");
	if (!strcmp(what, "all") || !strcmp(what, "lines"))
		status |= bench_lines(argc > 2 ? argv[2] : "/");
	if (!strcmp(what, "all") || !strcmp(what, "files"))
		status |= bench_files(argc > 2 ? argv[2] : "/");
	if (!strcmp(what, "all") || !strcmp(what, "lcd"))
		status |= bench_lcd();
	return status;
}
