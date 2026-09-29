#include "base64.h"

#include <openssl/evp.h>
#include <string.h>

int base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_size)
{
	int n;

	if (out_size < ((in_len + 2) / 3) * 4 + 1)
		return -1;
	n = EVP_EncodeBlock((unsigned char *)out, in, (int)in_len);
	return n > 0 ? 0 : -1;
}

/*
 * EVP_DecodeBlock reports the PADDED length -- it decodes '=' to zero
 * bytes and counts them -- so the padding has to be subtracted here or
 * a 74-byte signature blob reads back as 75 and every length check
 * downstream is off by one. It also accepts only whole 4-character
 * groups, which is why a length that is not a multiple of four is
 * rejected before it is handed over rather than after.
 */
int base64_decode(const char *in, unsigned char *out, size_t out_size)
{
	size_t len = strlen(in);
	size_t pad = 0;
	int n;

	if (len == 0 || len % 4 != 0)
		return -1;
	if (in[len - 1] == '=')
		pad++;
	if (len >= 2 && in[len - 2] == '=')
		pad++;
	if ((len / 4) * 3 > out_size)
		return -1;
	n = EVP_DecodeBlock(out, (const unsigned char *)in, (int)len);
	if (n < 0 || (size_t)n != (len / 4) * 3)
		return -1;
	return (int)((size_t)n - pad);
}
