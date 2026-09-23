/*
 * File transfer over the console: rx receives a file from the PC, tx sends
 * one to it. Both are driven by tools/xfer.py (`make push` / `make pull`),
 * not typed by hand.
 *
 * The console is not a clean pipe. Under load the USB port has been seen
 * to drop a few bytes -- an acknowledgement's line ending, once in a few
 * megabytes -- and a transfer that waits for what it lost waits forever.
 * So the file goes in numbered chunks, each with its own CRC-32, and a side
 * that does not hear back in time sends again:
 *
 *   base64       up to 45 bytes of the chunk (60 characters)
 *   . N CRC      chunk N ends; CRC is the CRC-32 of its bytes, in hex
 *   ok N         chunk N arrived whole. A repeat of a chunk already taken
 *                is answered the same way, so a lost "ok" costs one resend
 *   re N         chunk N arrived damaged: send it again
 *   .. LEN CRC   the end: the whole file's length and CRC-32
 *
 *   rx FILE   "RDY", then chunks from the PC. The data goes to FILE.part,
 *             which replaces FILE only once the whole file checks out;
 *             then "OK LEN" or "ERR why"
 *   tx FILE   "SIZE LEN", then chunks to the PC until it says "ok" to
 *             each, then the end line until it says "ok end"
 *
 * Base64 so that no byte is special to the terminal, which is in raw mode
 * throughout: the line editor neither echoes nor rewrites anything.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define CHUNK		512		/* tx's chunks; ~700 bytes on the wire, under the 1 KB USB buffer */
#define CHUNK_MAX	1024		/* the most rx will take in one */
#define LINE		45		/* bytes on a line: 60 characters of base64 */
#define WAIT_MS		2000		/* no answer in this long: send again */
#define TRIES		10
#define IDLE_MS		30000		/* rx: nothing at all in this long, and the PC is gone */


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
 * One line from the raw console, without its newline: its length, -1 on an
 * error or timeout, -2 for a line too long for buf (read to its end anyway,
 * so the next line starts in the right place), -3 for Ctrl-C. The console
 * is read as much at a time as has arrived, not a byte per system call.
 *
 * The terminal is raw, so Ctrl-C arrives as a byte rather than a signal;
 * it never occurs in base64, so it means "stop": typed by someone who ran
 * rx by mistake, or sent by the PC when it gives up.
 */
struct console {
	char	buf[256];
	int	len, at;
};

static int get_line(struct console *in, char *buf, int size)
{
	int n = 0;
	bool overflow = false;

	for (;;) {
		if (in->at == in->len) {
			ssize_t got = pt_read(PT_STDIN, in->buf, sizeof(in->buf));

			if (got <= 0) {
				buf[n] = '\0';
				return -1;
			}
			in->len = (int)got;
			in->at = 0;
		}
		char c = in->buf[in->at++];

		if (c == PT_CTRL('c'))
			return -3;
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

static void set_timeout(int ms)
{
	pt_ioctl(PT_STDIN, PT_TTY_SETTIMEOUT, &ms);
}

/* Answers a chunk: "ok N" or "re N". */
static void answer(const char *what, unsigned long n)
{
	pt_printf("%s %lu\n", what, n);
}

/*
 * Reads what the PC sends after the end, for a moment, answering another
 * end line the way the first was answered: the verdict may be what got
 * lost. Anything else is swallowed, so that the shell does not take it
 * for commands.
 */
static void linger(struct console *in, const char *verdict)
{
	char line[128];

	int n;

	set_timeout(1500);
	while ((n = get_line(in, line, sizeof(line))) != -1 && n != -3)
		if (!strncmp(line, ".. ", 3))
			pt_printf("%s\n", verdict);
}

PT_PROGRAM(rx, "receive a file from the PC (see `make push`)\n"
	       "usage: rx FILE\n"
	       "The PC sends numbered chunks of base64, each with its CRC-32;\n"
	       "a damaged one is sent again. The data goes to FILE.part and\n"
	       "replaces FILE only once the whole file's CRC matches.\n"
	       "Ctrl-C stops it.")
{
	char line[128], part[PT_PATH_MAX], verdict[32];
	struct console in = { 0 };
	uint8_t *chunk;
	size_t have = 0;
	unsigned long next = 0;
	uint64_t total = 0;
	uint32_t crc = 0;
	bool damaged = false;
	const char *why = NULL;
	int fd;

	if (argc != 2) {
		pt_dprintf(PT_STDERR, "rx: usage: rx FILE\n");
		return 2;
	}
	if ((size_t)snprintf(part, sizeof(part), "%s.part", argv[1]) >= sizeof(part))
		return fail("rx", argv[1], -ENAMETOOLONG);
	if (!(chunk = pt_malloc(CHUNK_MAX)))
		return fail("rx", NULL, -ENOMEM);
	fd = pt_open(part, O_WRONLY | O_CREAT | O_TRUNC);
	if (fd < 0) {
		pt_free(chunk);
		return fail("rx", part, fd);
	}

	set_timeout(IDLE_MS);
	pt_tty_raw(PT_STDIN, true);
	pt_puts("RDY\n");
	for (;;) {
		int n = get_line(&in, line, sizeof(line));
		unsigned long k, sum;
		unsigned long long len;

		if (n == -1 || n == -3) {
			why = n == -1 ? "timeout" : "interrupted";
			break;
		}
		if (n > 2 && line[0] == '.' && line[1] == ' ') {
			/* A chunk's end: take it, or ask for it again. */
			if (sscanf(line + 2, "%lu %lx", &k, &sum) != 2) {
				answer("re", next);
			} else if (k + 1 == next) {
				answer("ok", k);	/* again: our "ok" was lost */
			} else if (k == next && !damaged && crc32_of(0, chunk, have) == sum) {
				if (write_all(fd, (const char *)chunk, have)) {
					why = "write";
					break;
				}
				crc = crc32_of(crc, chunk, have);
				total += have;
				answer("ok", next++);
			} else {
				answer("re", next);
			}
			have = 0;
			damaged = false;
			continue;
		}
		if (n > 3 && !strncmp(line, ".. ", 3)) {
			if (sscanf(line + 3, "%llu %lx", &len, &sum) != 2 || len != total || sum != crc)
				why = "crc";
			break;
		}
		/* A line of the chunk. One that is too long, or would not fit,
		 * spoils the chunk, and its end will ask for it again. */
		int m = n < 0 || have + ((size_t)n + 3) / 4 * 3 > CHUNK_MAX ? -1
		      : b64_decode(line, n, chunk + have);

		if (m < 0)
			damaged = true;
		else
			have += m;
	}

	if (pt_close(fd) && !why)
		why = "write";			/* the last block failed */
	if (!why && pt_rename(part, argv[1]))
		why = "rename";
	if (why)
		pt_unlink(part);
	pt_free(chunk);
	if (why)
		snprintf(verdict, sizeof(verdict), "ERR %s", why);
	else
		snprintf(verdict, sizeof(verdict), "OK %llu", (unsigned long long)total);
	pt_printf("%s\n", verdict);
	if (!why || !strcmp(why, "crc") || !strcmp(why, "write") || !strcmp(why, "rename"))
		linger(&in, verdict);	/* the PC is still there, and may not have heard */
	set_timeout(-1);
	pt_tty_raw(PT_STDIN, false);
	return why ? 1 : 0;
}

/* Waits for "ok N": 1 if it came, 0 for "re N" or nothing in time, -1 for Ctrl-C. */
static int acked(struct console *in, const char *want)
{
	char line[128];
	int64_t end = pt_uptime_us() + WAIT_MS * 1000LL;

	while (pt_uptime_us() < end) {
		int n = get_line(in, line, sizeof(line));

		if (n == -3)
			return -1;
		if (n == -1)
			return 0;
		if (!strcmp(line, want))
			return 1;
		if (line[0] == 'r' && line[1] == 'e' && !strcmp(line + 2, want + 2))
			return 0;		/* "re N": now, not after the wait */
	}
	return 0;
}

/* One chunk as base64 lines and its end line, in as few writes as fit. */
static void send_chunk(const uint8_t *data, size_t n, unsigned long k, char *out)
{
	size_t at = 0;

	for (size_t i = 0; i < n; i += LINE) {
		at += b64_encode(data + i, n - i < LINE ? n - i : LINE, out + at);
		out[at++] = '\n';
	}
	at += snprintf(out + at, 40, ". %lu %08lx\n", k, (unsigned long)crc32_of(0, data, n));
	write_all(PT_STDOUT, out, at);
}

PT_PROGRAM(tx, "send a file to the PC (see `make pull`)\n"
	       "usage: tx FILE\n"
	       "Numbered chunks of base64, each with its CRC-32, each sent\n"
	       "again until the PC says it has it.")
{
	struct pt_stat st;
	uint8_t *data;
	char *out, want[24];
	struct console in = { 0 };
	unsigned long k = 0;
	uint64_t total = 0;
	uint32_t crc = 0;
	int fd, err, ret = 0, tries, got = 0;

	if (argc != 2) {
		pt_dprintf(PT_STDERR, "tx: usage: tx FILE\n");
		return 2;
	}
	if ((err = pt_stat(argv[1], &st)))
		return fail("tx", argv[1], err);
	if (st.is_dir) {
		pt_dprintf(PT_STDERR, "tx: %s: is a directory\n", argv[1]);
		return 1;
	}
	if ((fd = pt_open(argv[1], O_RDONLY)) < 0)
		return fail("tx", argv[1], fd);
	data = pt_malloc(CHUNK);
	out = pt_malloc(CHUNK / LINE * 61 + 61 + 40);
	if (!data || !out) {
		ret = fail("tx", NULL, -ENOMEM);
		goto out;
	}

	set_timeout(WAIT_MS);
	pt_tty_raw(PT_STDIN, true);
	pt_printf("SIZE %llu\n", (unsigned long long)st.size);
	for (;; k++) {
		ssize_t n = 0, r = 1;

		/* A whole chunk: a read may return less. */
		while (n < CHUNK && (r = pt_read(fd, data + n, CHUNK - n)) > 0)
			n += r;
		if (r < 0) {
			pt_puts("ERR read\n");
			ret = 1;
			break;
		}
		if (n == 0)
			break;
		snprintf(want, sizeof(want), "ok %lu", k);
		for (tries = 0; tries < TRIES; tries++) {
			send_chunk(data, n, k, out);
			if ((got = acked(&in, want)))
				break;
		}
		if (tries == TRIES || got < 0) {
			ret = 1;
			break;
		}
		crc = crc32_of(crc, data, n);
		total += n;
	}
	if (!ret) {
		for (tries = 0; tries < TRIES; tries++) {
			pt_printf(".. %llu %08lx\n", (unsigned long long)total, (unsigned long)crc);
			if ((got = acked(&in, "ok end")))
				break;
		}
		ret = tries == TRIES || got < 0;
	}
	set_timeout(-1);
	pt_tty_raw(PT_STDIN, false);
out:
	pt_free(data);
	pt_free(out);
	pt_close(fd);
	return ret;
}
