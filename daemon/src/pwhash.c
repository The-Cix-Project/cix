#include "pwhash.h"

#include <string.h>

/* Declared directly rather than via a vendored header -- bcrypt.c's
 * own real, upstream OpenBSD signatures, unchanged; see that file's
 * own top comment for why only these two are kept. */
int bcrypt_newhash(const char *pass, int log_rounds, char *hash, size_t hashlen);
int bcrypt_checkpass(const char *pass, const char *goodhash);

int pwhash_bcrypt_new(const char *password, char *out, size_t out_size)
{
	if (out_size < PWHASH_BCRYPT_LEN + 1)
		return -1;
	return bcrypt_newhash(password, PWHASH_BCRYPT_COST, out, out_size) == 0 ? 0 : -1;
}

int pwhash_bcrypt_check(const char *password, const char *hash)
{
	if (password == NULL || hash == NULL || hash[0] == '\0')
		return 0;
	return bcrypt_checkpass(password, hash) == 0 ? 1 : 0;
}
