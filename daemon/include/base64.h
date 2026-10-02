#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>

/*
 * Standard base64 (RFC 4648 section 4, with padding, no line wrapping),
 * on OpenSSL's EVP_EncodeBlock()/EVP_DecodeBlock() -- cixd links
 * -lcrypto already. One implementation for every caller: minisign
 * signatures and keys (releasekey.c), HTTP Basic credentials
 * (hostauth.c, #543) and a forge commit's file content (forgecommit.c,
 * ADR-0323).
 */

/* Encodes in_len bytes into out as a NUL-terminated string. out needs
 * ((in_len + 2) / 3) * 4 + 1 bytes. 0 on success, -1 otherwise. */
int base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_size);

/*
 * Decodes the NUL-terminated string in; returns the number of bytes
 * written, or -1 if in is not whole, padded base64 or out is too small.
 *
 * `out` must hold (strlen(in) / 4) * 3 bytes -- the PADDED length, not
 * the number of bytes you expect back. Sizing it to the expected result
 * is the mistake this paragraph exists for: a 64-byte signature needs
 * 66 bytes here, and a destination of exactly 64 makes the guard refuse
 * the decode. It fails closed, which is the right direction and is also
 * indistinguishable from malformed input -- so a programming error
 * presents as "nothing verifies", which is how it reached a real build
 * (#403). The result is NOT NUL-terminated.
 */
int base64_decode(const char *in, unsigned char *out, size_t out_size);

#endif
