/* Parsing is independent of PSA: malformed files must not reach password hashing. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SHADOW "/etc/shadow"
#define SCHEME "pbkdf2-sha256"
#define SALT_LEN 16
#define HASH_LEN 32
static const char *test_shadow;
static void *mount_resolve(const char *path, char *out, size_t size)
{
	assert(!strcmp(path, SHADOW));
	assert((size_t)snprintf(out, size, "%s", test_shadow) < size);
	return out;
}
#include "auth_parse_under_test.h"

static void shadow(const char *salt_text, const char *hash_text, const char *tail, bool valid)
{
	unsigned iterations = 0;
	uint8_t salt[SALT_LEN], hash[HASH_LEN];
	FILE *f = fopen(test_shadow, "w");

	assert(f && fprintf(f, SCHEME "$20000$%s$%s%s", salt_text, hash_text, tail) > 0);
	assert(!fclose(f));
	assert(load(&iterations, salt, hash) == (valid ? 0 : -EINVAL));
	if (valid)
		assert(iterations == 20000 && salt[0] == 0 && salt[1] == 0x11 && hash[31] == 0xff);
}

int main(int argc, char **argv)
{
	const char *salt = "00112233445566778899aabbccddeeff";
	const char *hash = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
	uint8_t out[HASH_LEN];

	assert(argc == 2);
	test_shadow = argv[1];
	assert(from_hex("1", out, SALT_LEN) == -EINVAL);
	assert(from_hex("00112233445566778899aabbccddeeffg", out, SALT_LEN) == -EINVAL);
	assert(from_hex("00112233445566778899aabbccddeeff00", out, SALT_LEN) == -EINVAL);
	shadow(salt, hash, "\n", true);
	shadow(salt, hash, "", true);
	shadow("1", hash, "\n", false);
	shadow(salt, "1", "\n", false);
	shadow("00112233445566778899aabbccddeef", hash, "\n", false);
	shadow(salt, "00112233445566778899aabbccddeeff", "\n", false);
	shadow(salt, hash, "0\n", false);
	shadow(salt, hash, "garbage\n", false);
	puts("auth: exact salt/hash length, hexadecimal digits and trailing data passed");
	return 0;
}
