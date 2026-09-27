/*
 * File transfer with the PC, beside the console rather than through it.
 *
 * tools/xfer.py (`make push`, `make pull`) talks to this over the serial
 * port the console is on, and nothing on the board has to be typed or
 * given up for it: the shell on the first terminal stays free, and the
 * screen shows nothing. Every line of the transfer starts with RS (0x1e),
 * which no one types and no program prints; the serial driver hands such
 * lines here and everything else to the terminal as before, and the
 * answers go back out with the same mark, so that the PC can pick them
 * out of whatever the console is printing meanwhile.
 *
 * The port is not a clean pipe -- under load a byte goes missing now and
 * then -- so the file moves in numbered chunks with a CRC-32 each, which
 * either side asks for again when it does not hear back:
 *
 *   PUT LEN CRC PATH   a file is coming: "RDY", or "ERR why"
 *   D K CRC BASE64     chunk K (at K * CHUNK): "ok K", or "re K" if damaged
 *   END                the whole file is there: its length and CRC are
 *                      checked, PATH.part becomes PATH: "OK LEN" or "ERR why"
 *   GET PATH           "SIZE LEN CRC", or "ERR why"
 *   R K                chunk K of it, as a D line
 *
 * Chunks may come in any order and more than once: the PC sends again
 * whatever is not answered. Coming here it keeps one in flight -- the USB
 * port holds about 1 KB of what arrives, and the rest is lost while a
 * chunk is written to the card -- and going back several. PATH is taken from
 * the root; ~/ is the home directory, and a path without a leading / is in
 * it too.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"
#include "pt/kernel.h"

#define CHUNK		512
#define HOME		"/home/" CONFIG_PT_USERNAME

static const char B64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static struct {
	FILE	*f;
	char	 path[PT_PATH_MAX + 16];	/* the file, as the VFS has it */
	char	 part[PT_PATH_MAX + 24];	/* and the one being written */
	uint32_t len, crc;			/* what a PUT said it would be */
	bool	 writing;
	bool	 done;				/* and it is in place: END again says so */
} x;

static void (*send)(const char *s, size_t n);

static int b64_val(int c)
{
	const char *p = c ? strchr(B64, c) : NULL;

	return p ? (int)(p - B64) : -1;
}

static int b64_decode(const char *in, size_t n, uint8_t *out, size_t max)
{
	size_t o = 0;

	for (size_t i = 0; i + 1 < n && in[i] != '='; i += 4) {
		int a = b64_val(in[i]), b = b64_val(in[i + 1]);
		int c = i + 2 < n ? b64_val(in[i + 2]) : -1;
		int d = i + 3 < n ? b64_val(in[i + 3]) : -1;

		if (a < 0 || b < 0 || o + 1 + (c >= 0) + (c >= 0 && d >= 0) > max)
			return -1;
		out[o++] = (uint8_t)(a << 2 | b >> 4);
		if (c >= 0) {
			out[o++] = (uint8_t)(b << 4 | c >> 2);
			if (d >= 0)
				out[o++] = (uint8_t)(c << 6 | d);
		}
	}
	return (int)o;
}

static size_t b64_encode(const uint8_t *in, size_t n, char *out)
{
	size_t o = 0;

	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) |
			     (i + 2 < n ? in[i + 2] : 0);

		out[o++] = B64[v >> 18 & 63];
		out[o++] = B64[v >> 12 & 63];
		out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
		out[o++] = i + 2 < n ? B64[v & 63] : '=';
	}
	return o;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = crc >> 1 ^ (0xedb88320 & -(crc & 1));
	}
	return ~crc;
}

static void reply(const char *fmt, ...)
{
	char line[96];
	va_list ap;
	int n;

	line[0] = 0x1e;
	va_start(ap, fmt);
	n = vsnprintf(line + 1, sizeof(line) - 2, fmt, ap);
	va_end(ap);
	n = n < 0 ? 0 : n > (int)sizeof(line) - 3 ? (int)sizeof(line) - 3 : n;
	line[n + 1] = '\n';
	if (send)
		send(line, n + 2);
}

static void stop(void)
{
	if (x.f)
		fclose(x.f);
	x.f = NULL;
	x.writing = false;
}

/* PATH as the VFS has it: from the root, ~/ and bare names in the home. */
static bool resolve(const char *path, char *vfs, size_t size)
{
	char abs[PT_PATH_MAX];

	if (path[0] == '~' && (path[1] == '/' || !path[1]))
		snprintf(abs, sizeof(abs), "%s%s", HOME, path + 1);
	else if (path[0] != '/')
		snprintf(abs, sizeof(abs), "%s/%s", HOME, path);
	else
		strlcpy(abs, path, sizeof(abs));
	return mount_resolve(abs, vfs, size) != NULL;
}

/* The CRC of what is in `f`, reading it again from the start. */
static bool file_crc(FILE *f, uint32_t *len, uint32_t *crc)
{
	static uint8_t buf[CHUNK];
	size_t n;

	*len = 0;
	*crc = 0;
	if (fseek(f, 0, SEEK_SET))
		return false;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		*crc = crc32_update(*crc, buf, n);
		*len += n;
	}
	return !ferror(f);
}

static void put(const char *args)
{
	unsigned long len, crc;
	int at = 0;

	stop();
	if (sscanf(args, "%lu %lx %n", &len, &crc, &at) < 2 || !at || !args[at]) {
		reply("ERR bad PUT");
		return;
	}
	if (!resolve(args + at, x.path, sizeof(x.path))) {
		reply("ERR %s: no such place", args + at);
		return;
	}
	snprintf(x.part, sizeof(x.part), "%s.part", x.path);
	if (!(x.f = fopen(x.part, "w+b"))) {
		reply("ERR %s: cannot write there", args + at);
		return;
	}
	x.len = len;
	x.crc = crc;
	x.writing = true;
	x.done = false;
	reply("RDY");
}

static void chunk(const char *args)
{
	static uint8_t data[CHUNK];
	unsigned long k, crc;
	int at = 0, n;

	if (!x.writing) {
		reply("ERR no PUT");
		return;
	}
	if (sscanf(args, "%lu %lx %n", &k, &crc, &at) < 2 || !at) {
		reply("ERR bad chunk");
		return;
	}
	n = b64_decode(args + at, strlen(args + at), data, sizeof(data));
	if (n < 0 || crc32_update(0, data, n) != crc || (uint64_t)k * CHUNK + n > x.len) {
		reply("re %lu", k);
		return;
	}
	if (fseek(x.f, (long)(k * CHUNK), SEEK_SET) || fwrite(data, 1, n, x.f) != (size_t)n) {
		reply("ERR writing: the disk may be full");
		fclose(x.f);
		x.f = NULL;
		x.writing = false;
		remove(x.part);
		return;
	}
	reply("ok %lu", k);
}

static void end(void)
{
	uint32_t len, crc;

	if (!x.writing) {
		if (x.done)
			reply("OK %lu", (unsigned long)x.len);	/* the first OK was lost */
		else
			reply("ERR no PUT");
		return;
	}
	if (fflush(x.f) || !file_crc(x.f, &len, &crc) || len != x.len || crc != x.crc) {
		stop();
		remove(x.part);
		reply("ERR the pieces do not make the file; try again");
		return;
	}
	stop();
	remove(x.path);			/* FAT will not rename over a file */
	if (rename(x.part, x.path)) {
		remove(x.part);
		reply("ERR cannot put it in place");
		return;
	}
	x.done = true;
	reply("OK %lu", (unsigned long)len);
}

static void get(const char *path)
{
	uint32_t len, crc;

	stop();
	if (!resolve(path, x.path, sizeof(x.path)) || !(x.f = fopen(x.path, "rb"))) {
		reply("ERR %s: no such file", path);
		return;
	}
	if (!file_crc(x.f, &len, &crc)) {
		stop();
		reply("ERR %s: cannot read it", path);
		return;
	}
	reply("SIZE %lu %08lx", (unsigned long)len, (unsigned long)crc);
}

static void read_chunk(const char *args)
{
	static uint8_t data[CHUNK];
	static char line[CHUNK * 4 / 3 + 40];
	unsigned long k;
	size_t n;
	int head;

	if (!x.f || x.writing || sscanf(args, "%lu", &k) != 1) {
		reply("ERR no GET");
		return;
	}
	if (fseek(x.f, (long)(k * CHUNK), SEEK_SET)) {
		reply("ERR reading");
		return;
	}
	n = fread(data, 1, sizeof(data), x.f);
	head = snprintf(line, sizeof(line), "\x1e" "D %lu %08lx ", k,
			(unsigned long)crc32_update(0, data, n));
	head += (int)b64_encode(data, n, line + head);
	line[head++] = '\n';
	if (send)
		send(line, head);
}

void xfer_set_output(void (*out)(const char *s, size_t n))
{
	send = out;
}

/* One line from the PC, without its mark or its newline. */
void xfer_line(char *line)
{
	power_remote_activity();	/* someone is there: not idle */
	if (!strncmp(line, "D ", 2))
		chunk(line + 2);
	else if (!strncmp(line, "R ", 2))
		read_chunk(line + 2);
	else if (!strncmp(line, "PUT ", 4))
		put(line + 4);
	else if (!strcmp(line, "END"))
		end();
	else if (!strncmp(line, "GET ", 4))
		get(line + 4);
	else if (!strcmp(line, "BYE"))
		stop();
	else
		reply("ERR what is %.20s", line);
}
