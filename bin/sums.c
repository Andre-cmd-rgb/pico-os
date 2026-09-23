/*
 * Checksums: cksum, and md5sum sha1sum sha256sum sha512sum.
 *
 * The hashes are ESP-IDF's (PSA crypto from mbedtls), which hands SHA to
 * the chip's own hardware: faster than code written here, and a hash that
 * matters is not the place to write one's own. cksum is POSIX's CRC, which
 * is a few lines and is also built on the PC for the tests.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "util.h"

#define CHUNK	4096

/* ------------------------------------------------------------ cksum */

/*
 * POSIX's cksum: CRC-32 with the polynomial 0x04c11db7 taken most
 * significant bit first, over the data and then its length, least
 * significant byte first, complemented at the end. Not zlib's CRC-32.
 * The table is made for each run: a program's statics are shared by every
 * copy of it that runs, and 256 entries cost nothing to work out.
 */
static void crc_setup(uint32_t table[256])
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i << 24;

		for (int k = 0; k < 8; k++)
			c = c & 0x80000000u ? c << 1 ^ 0x04c11db7u : c << 1;
		table[i] = c;
	}
}

static uint32_t crc_add(const uint32_t table[256], uint32_t crc, const uint8_t *p, size_t n)
{
	while (n--)
		crc = crc << 8 ^ table[(crc >> 24 ^ *p++) & 0xff];
	return crc;
}

static int cksum_fd(int fd, const char *name, void *ctx)
{
	const uint32_t *crc_table = ctx;
	uint8_t *buf = pt_malloc(CHUNK);
	uint64_t size = 0;
	uint32_t crc = 0;
	ssize_t n = 0;

	if (!buf)
		return fail("cksum", name, -ENOMEM);
	while ((n = pt_read(fd, buf, CHUNK)) > 0) {
		crc = crc_add(crc_table, crc, buf, n);
		size += n;
	}
	pt_free(buf);
	if (n < 0)
		return fail("cksum", name, n);
	for (uint64_t len = size; len; len >>= 8) {
		uint8_t b = (uint8_t)len;

		crc = crc_add(crc_table, crc, &b, 1);
	}
	pt_printf("%lu %llu%s%s\n", (unsigned long)~crc, (unsigned long long)size,
		  name && strcmp(name, "-") ? " " : "", name && strcmp(name, "-") ? name : "");
	return 0;
}

PT_PROGRAM(cksum, "print a CRC and the size of each file\n"
	   "usage: cksum [file...]\n"
	   "POSIX's CRC, the same as cksum prints on a PC.")
{
	uint32_t *table = pt_malloc(256 * sizeof(*table));
	int status;

	if (!table)
		return fail("cksum", NULL, -ENOMEM);
	crc_setup(table);
	status = for_each_input("cksum", argc, argv, 1, cksum_fd, table);
	pt_free(table);
	return status;
}

/* ------------------------------------------------------------ hashes */

#ifdef ESP_PLATFORM

#include "psa/crypto.h"

struct algo {
	const char	*prog;
	psa_algorithm_t	 alg;
	size_t		 len;		/* bytes of hash */
};

static const struct algo md5 = { "md5sum", PSA_ALG_MD5, 16 };
static const struct algo sha1 = { "sha1sum", PSA_ALG_SHA_1, 20 };
static const struct algo sha256 = { "sha256sum", PSA_ALG_SHA_256, 32 };
static const struct algo sha512 = { "sha512sum", PSA_ALG_SHA_512, 64 };

/* The hash of what fd holds, in hex: 0, or an error. */
static int hash_fd(const struct algo *a, int fd, char *hex)
{
	psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
	uint8_t *buf = pt_malloc(CHUNK), sum[64];
	size_t got = 0;
	ssize_t n = 0;
	int err = 0;

	if (!buf)
		return -ENOMEM;
	if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&op, a->alg) != PSA_SUCCESS)
		err = -EIO;
	while (!err && (n = pt_read(fd, buf, CHUNK)) > 0)
		if (psa_hash_update(&op, buf, n) != PSA_SUCCESS)
			err = -EIO;
	if (!err && n < 0)
		err = (int)n;
	if (!err && (psa_hash_finish(&op, sum, sizeof(sum), &got) != PSA_SUCCESS || got != a->len))
		err = -EIO;
	psa_hash_abort(&op);
	pt_free(buf);
	for (size_t i = 0; !err && i < got; i++)
		snprintf(hex + 2 * i, 3, "%02x", sum[i]);
	return err;
}

static int sum_fd(int fd, const char *name, void *ctx)
{
	const struct algo *a = ctx;
	char hex[129];
	int err = hash_fd(a, fd, hex);

	if (err)
		return fail(a->prog, name, err);
	pt_printf("%s  %s\n", hex, name ? name : "-");
	return 0;
}

/*
 * -c: the lines of a list made by the same program, "HASH  NAME" each,
 * checked one by one: OK or FAILED, and how many failed at the end.
 */
static int check_fd(int fd, const char *list, void *ctx)
{
	const struct algo *a = ctx;
	struct lines l;
	size_t len;
	char *line, hex[129];
	int bad = 0, missing = 0, status = 0;

	lines_init(&l, fd);
	while ((line = lines_next(&l, &len))) {
		if (len && line[len - 1] == '\n')
			line[--len] = '\0';
		if (len < 2 * a->len + 2 || line[2 * a->len] != ' ' ||
		    (line[2 * a->len + 1] != ' ' && line[2 * a->len + 1] != '*')) {
			if (len)
				pt_dprintf(PT_STDERR, "%s: %s: not a line of %s output\n", a->prog,
					   list ? list : "-", a->prog);
			status = 1;
			continue;
		}
		const char *name = line + 2 * a->len + 2;
		int f = pt_open(name, O_RDONLY), err = f < 0 ? f : hash_fd(a, f, hex);

		if (f >= 0)
			pt_close(f);
		if (err) {
			pt_printf("%s: FAILED open or read\n", name);
			missing++;
		} else if (strncasecmp(hex, line, 2 * a->len)) {
			pt_printf("%s: FAILED\n", name);
			bad++;
		} else {
			pt_printf("%s: OK\n", name);
		}
	}
	lines_free(&l);
	if (missing)
		pt_dprintf(PT_STDERR, "%s: WARNING: %d listed file%s could not be read\n", a->prog,
			   missing, missing == 1 ? "" : "s");
	if (bad)
		pt_dprintf(PT_STDERR, "%s: WARNING: %d computed checksum%s did NOT match\n", a->prog,
			   bad, bad == 1 ? "" : "s");
	return status || bad || missing;
}

static int hash_main(const struct algo *a, int argc, char **argv)
{
	uint32_t flags;
	int i = parse_flags(a->prog, argc, argv, "c", &flags);

	if (i < 0)
		return 2;
	return for_each_input(a->prog, argc, argv, i, FLAG(flags, 'c') ? check_fd : sum_fd,
			      (void *)a) ? 1 : 0;
}

#define HASH_HELP(name)								\
	"usage: " name " [-c] [file...]\n"					\
	"  -c  check the files named in a list this printed before,\n"		\
	"      as in: " name " *.mp3 > sums; " name " -c sums"

PT_PROGRAM(md5sum, "print MD5 hashes of files\n" HASH_HELP("md5sum"))
{
	return hash_main(&md5, argc, argv);
}

PT_PROGRAM(sha1sum, "print SHA-1 hashes of files\n" HASH_HELP("sha1sum"))
{
	return hash_main(&sha1, argc, argv);
}

PT_PROGRAM(sha256sum, "print SHA-256 hashes of files\n" HASH_HELP("sha256sum"))
{
	return hash_main(&sha256, argc, argv);
}

PT_PROGRAM(sha512sum, "print SHA-512 hashes of files\n" HASH_HELP("sha512sum"))
{
	return hash_main(&sha512, argc, argv);
}

#endif /* ESP_PLATFORM */
