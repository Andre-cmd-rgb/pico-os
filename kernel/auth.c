/*
 * The password the network shell asks for.
 *
 * Only a hash of it is kept, in /etc/shadow on the internal flash:
 *
 *	pbkdf2-sha256$ITERATIONS$SALT$HASH
 *
 * PBKDF2 with HMAC-SHA-256, a random 16-byte salt and enough iterations
 * that one guess costs the board a noticeable fraction of a second -- and
 * costs anyone with a copy of the file the same, per guess, per password.
 *
 * The hashing is ESP-IDF's (PSA crypto from mbedtls), not written here: a
 * home-made hash is the one place where "our own" is the wrong answer.
 *
 * This is kernel code and runs on tasks that are not processes (the
 * network console's, for one), so it reaches the file through the mount
 * table and stdio, as the battery driver does, rather than pt_open().
 */
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "psa/crypto.h"

#include "pt/kernel.h"

#define SHADOW		"/etc/shadow"
#define SCHEME		"pbkdf2-sha256"
#define ITERATIONS	20000
#define SALT_LEN	16
#define HASH_LEN	32

static int derive(const char *password, const uint8_t *salt, unsigned iterations,
		  uint8_t out[HASH_LEN])
{
	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
	psa_status_t st = psa_crypto_init();

	if (st == PSA_SUCCESS)
		st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
	if (st == PSA_SUCCESS)
		st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iterations);
	if (st == PSA_SUCCESS)
		st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, SALT_LEN);
	if (st == PSA_SUCCESS)
		st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
						    (const uint8_t *)password, strlen(password));
	if (st == PSA_SUCCESS)
		st = psa_key_derivation_output_bytes(&op, out, HASH_LEN);
	psa_key_derivation_abort(&op);
	return st == PSA_SUCCESS ? 0 : -EIO;
}

static void to_hex(const uint8_t *in, size_t n, char *out)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < n; i++) {
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 15];
	}
	out[2 * n] = '\0';
}

static int from_hex(const char *in, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned v;

		if (sscanf(in + 2 * i, "%2x", &v) != 1)
			return -EINVAL;
		out[i] = (uint8_t)v;
	}
	return in[2 * n] && in[2 * n] != '\n' ? -EINVAL : 0;
}

/* The stored line, split up: 0, -ENOENT when there is none, -EINVAL if damaged. */
static int load(unsigned *iterations, uint8_t salt[SALT_LEN], uint8_t hash[HASH_LEN])
{
	char path[64], line[160], salt_hex[2 * SALT_LEN + 1], hash_hex[2 * HASH_LEN + 1];
	FILE *f;
	int ret = -EINVAL;

	if (!mount_resolve(SHADOW, path, sizeof(path)) || !(f = fopen(path, "r")))
		return -ENOENT;
	if (fgets(line, sizeof(line), f) &&
	    sscanf(line, SCHEME "$%u$%32[0-9a-f]$%64[0-9a-f]", iterations, salt_hex, hash_hex) == 3 &&
	    *iterations >= 1000 && !from_hex(salt_hex, salt, SALT_LEN) &&
	    !from_hex(hash_hex, hash, HASH_LEN))
		ret = 0;
	fclose(f);
	return ret;
}

bool auth_is_set(void)
{
	unsigned iterations;
	uint8_t salt[SALT_LEN], hash[HASH_LEN];

	return load(&iterations, salt, hash) == 0;
}

int auth_check(const char *password)
{
	unsigned iterations;
	uint8_t salt[SALT_LEN], want[HASH_LEN], got[HASH_LEN], diff = 0;
	int err = load(&iterations, salt, want);

	if (err)
		return err;
	if ((err = derive(password, salt, iterations, got)))
		return err;
	for (int i = 0; i < HASH_LEN; i++)	/* the same time, however much matches */
		diff |= got[i] ^ want[i];
	return diff ? -EACCES : 0;
}

/* Sets the password, or with NULL removes it. Written aside, then renamed. */
int auth_set(const char *password)
{
	char path[64], tmp[72], salt_hex[2 * SALT_LEN + 1], hash_hex[2 * HASH_LEN + 1];
	uint8_t salt[SALT_LEN], hash[HASH_LEN];
	FILE *f;
	int err;

	if (!mount_resolve(SHADOW, path, sizeof(path)))
		return -ENOENT;
	if (!password)
		return remove(path) && errno != ENOENT ? -errno : 0;
	esp_fill_random(salt, sizeof(salt));
	if ((err = derive(password, salt, ITERATIONS, hash)))
		return err;
	to_hex(salt, sizeof(salt), salt_hex);
	to_hex(hash, sizeof(hash), hash_hex);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (!(f = fopen(tmp, "w")))
		return -errno;
	fprintf(f, SCHEME "$%u$%s$%s\n", ITERATIONS, salt_hex, hash_hex);
	if (fclose(f)) {
		err = -errno;
		remove(tmp);
		return err;
	}
	return rename(tmp, path) ? -errno : 0;
}
