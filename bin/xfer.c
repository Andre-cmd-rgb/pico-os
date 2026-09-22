/*
 * File transfer over the console: rx receives a file from the PC, tx sends
 * one to it. Both are usually driven by tools/xfer.py (`make push` / `make
 * pull`), not typed by hand.
 *
 * The wire format is base64 so that no byte value or control character is
 * special, plus a CRC-32 for integrity. `rx` puts the terminal in raw mode
 * so the line editor neither echoes nor rewrites the stream; it answers each
 * chunk with "." so the PC cannot overrun the small input buffer.
 *
 *   rx FILE     read base64 lines into FILE.part, check the CRC line, then
 *               rename FILE.part to FILE: a failed transfer leaves FILE alone
 *   tx FILE     print FILE as base64 lines, then "." and "CRC xxxxxxxx"
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"


static const char B64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

static int b64_encode(const uint8_t *in, size_t n, char *out)
{
	size_t i = 0, o = 0;

	while (i + 3 <= n) {
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
		out[o++] = B64[(v >> 18) & 63];
		out[o++] = B64[(v >> 12) & 63];
		out[o++] = B64[(v >> 6) & 63];
		out[o++] = B64[v & 63];
		i += 3;
	}
	if (i + 1 == n) {
		uint32_t v = (uint32_t)in[i] << 16;
		out[o++] = B64[(v >> 18) & 63];
		out[o++] = B64[(v >> 12) & 63];
		out[o++] = '=';
		out[o++] = '=';
	} else if (i + 2 == n) {
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
		out[o++] = B64[(v >> 18) & 63];
		out[o++] = B64[(v >> 12) & 63];
		out[o++] = B64[(v >> 6) & 63];
		out[o++] = '=';
	}
	return o;
}

static int b64_decode(const char *in, size_t n, uint8_t *out)
{
	size_t i = 0, o = 0;

	while (i < n && in[i] != '=') {
		int a = b64_val((unsigned char)in[i]);
		int b = i + 1 < n ? b64_val((unsigned char)in[i + 1]) : -1;
		int c = i + 2 < n ? b64_val((unsigned char)in[i + 2]) : -1;
		int d = i + 3 < n ? b64_val((unsigned char)in[i + 3]) : -1;

		if (a < 0 || b < 0)
			return -1;
		out[o++] = (uint8_t)((a << 2) | (b >> 4));
		if (c >= 0) {
			out[o++] = (uint8_t)((b << 4) | (c >> 2));
			if (d >= 0)
				out[o++] = (uint8_t)((c << 6) | d);
		}
		i += 4;
	}
	return (int)o;
}

/*
 * One line from the raw console, without the newline: its length, -1 on an
 * error or timeout, -2 for a line too long for buf (read to its end anyway,
 * so the next line starts in the right place).
 */
static int get_line(char *buf, int size)
{
	int n = 0;
	bool overflow = false;

	for (;;) {
		int c = pt_getc(PT_STDIN);

		if (c < 0)
			return -1;
		if (c == '\n')
			break;
		if (c == '\r')
			continue;
		if (n < size - 1)
			buf[n++] = c;
		else
			overflow = true;
	}
	buf[n] = '\0';
	return overflow ? -2 : n;
}

/* The received file replaces `to` only once it is complete and checked. */
static int install(const char *part, const char *to)
{
	struct pt_stat st;
	int err = pt_rename(part, to);

	if (err == -EEXIST && !pt_stat(to, &st) && !st.is_dir) {
		/* FAT will not rename over a file */
		err = pt_unlink(to);
		if (!err)
			err = pt_rename(part, to);
	}
	return err;
}

PT_PROGRAM(rx, "receive a file from the PC (see `make push`)\n"
	       "usage: rx FILE\n"
	       "Reads base64 lines; \".\" ends a chunk (answered with \".\"), \"..\"\n"
	       "ends the data, then a \"CRC xxxxxxxx\" line is checked. The data\n"
	       "goes to FILE.part and replaces FILE only once the CRC matches.")
{
	int fd, timeout = 10000;
	uint32_t crc = 0;
	size_t total = 0;
	char line[128], part[PT_PATH_MAX];
	uint8_t out[96];
	const char *why = NULL;

	if (argc != 2) {
		pt_dprintf(PT_STDERR, "rx: usage: rx FILE\n");
		return 2;
	}
	if ((size_t)snprintf(part, sizeof(part), "%s.part", argv[1]) >= sizeof(part))
		return fail("rx", argv[1], -ENAMETOOLONG);
	fd = pt_open(part, O_WRONLY | O_CREAT | O_TRUNC);
	if (fd < 0)
		return fail("rx", part, fd);

	pt_ioctl(PT_STDIN, PT_TTY_SETTIMEOUT, &timeout);
	pt_tty_raw(PT_STDIN, true);
	pt_puts("RDY\n");

	for (;;) {
		int n = get_line(line, sizeof(line));

		if (n == -1) {
			why = "timeout";
			break;
		}
		if (n == 1 && line[0] == '.') {
			pt_puts(".\n");			/* chunk ack */
			continue;
		}
		if (n == 2 && line[0] == '.' && line[1] == '.')
			break;				/* data done */

		int m = n < 0 ? -1 : b64_decode(line, n, out);
		if (m < 0) {
			why = "line";
			break;
		}
		if (write_all(fd, (const char *)out, m)) {
			why = "write";
			break;
		}
		crc = crc32_of(crc, out, m);
		total += m;
	}

	if (!why) {
		int n = get_line(line, sizeof(line));
		if (n < 4 || memcmp(line, "CRC ", 4) || strtoul(line + 4, NULL, 16) != crc)
			why = "crc";
	}
	if (pt_close(fd) && !why)
		why = "write";				/* the last block failed */
	if (!why && install(part, argv[1]))
		why = "rename";

	if (why && strcmp(why, "timeout")) {
		/* swallow the rest of what the PC already sent, so the shell
		 * does not take it for commands */
		timeout = 300;
		pt_ioctl(PT_STDIN, PT_TTY_SETTIMEOUT, &timeout);
		while (get_line(line, sizeof(line)) != -1)
			;
	}
	timeout = -1;
	pt_ioctl(PT_STDIN, PT_TTY_SETTIMEOUT, &timeout);
	pt_tty_raw(PT_STDIN, false);
	if (why) {
		pt_unlink(part);
		pt_printf("ERR %s\n", why);
		return 1;
	}
	pt_printf("OK %u\n", (unsigned)total);
	return 0;
}

PT_PROGRAM(tx, "send a file to the PC (see `make pull`)\nusage: tx FILE")
{
	int fd;
	struct pt_stat st;
	uint32_t crc = 0;
	uint8_t in[45];
	char out[61];

	if (argc != 2) {
		pt_dprintf(PT_STDERR, "tx: usage: tx FILE\n");
		return 2;
	}
	int err = pt_stat(argv[1], &st);
	if (err)
		return fail("tx", argv[1], err);
	if (st.is_dir) {
		pt_dprintf(PT_STDERR, "tx: %s: is a directory\n", argv[1]);
		return 1;
	}
	fd = pt_open(argv[1], O_RDONLY);
	if (fd < 0)
		return fail("tx", argv[1], fd);

	pt_printf("SIZE %llu\n", (unsigned long long)st.size);
	for (;;) {
		/* whole 45-byte groups: a read may return less, and padding
		 * belongs only on the last line */
		ssize_t n = 0, r = 1;

		while (n < (ssize_t)sizeof(in) && (r = pt_read(fd, in + n, sizeof(in) - n)) > 0)
			n += r;
		if (r < 0) {
			pt_close(fd);
			return fail("tx", argv[1], r);
		}
		if (n == 0)
			break;
		crc = crc32_of(crc, in, n);
		int m = b64_encode(in, n, out);
		out[m] = '\n';
		write_all(PT_STDOUT, out, m + 1);
	}
	pt_close(fd);
	pt_printf(".\nCRC %08x\n", (unsigned)crc);
	return 0;
}
