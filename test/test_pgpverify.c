/*
 * ADR-0254: OpenPGP clearsign verification.
 *
 * The fixtures are embedded rather than fetched: this must run in a
 * build container with no network and no gpg, and a test that needs
 * either is a test that silently stops running.
 *
 * The signed document is deliberately SIXTY-FIVE lines. A short one
 * would pass against a broken implementation: canonicalisation turns
 * LF into CRLF, so a k-line document grows by up to k bytes, and the
 * original malloc(len + 2) was an underestimate for anything past two
 * lines. Against kernel.org's real ~200-line sha256sums.asc it overran
 * the heap outright. A four-line fixture fits in the slack and proves
 * nothing.
 */
#include "pgpverify.h"
#include "test_pgp_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void fail(const char *what, const char *detail)
{
	fprintf(stderr, "  FAIL: %s (%s)\n", what, detail);
	g_failures++;
}

static enum pgp_verify_result verify(const char *doc, const char *fpr, char **text)
{
	char err[256];

	err[0] = '\0';
	return pgp_clearsign_verify(doc, strlen(doc), TEST_KEY, sizeof(TEST_KEY) - 1, fpr, text, NULL,
	                            err, sizeof(err));
}

int main(void)
{
	char *text = NULL;
	char sha[65];
	char *tampered;
	char *truncated;
	enum pgp_verify_result r;
	size_t i;

	r = verify(SIGNED_DOC, TEST_FPR, &text);
	if (r != PGP_VERIFY_OK)
		fail("a valid clearsigned document did not verify", pgp_verify_result_name(r));
	else if (text == NULL)
		fail("verified but returned no text", "text is NULL");

	if (text != NULL) {
		if (strstr(text, "this line is dash-escaped and must survive unescaping") == NULL)
			fail("dash-escaped line was not unescaped", "marker absent");
		if (strstr(text, "not signed   ") != NULL)
			fail("trailing whitespace was not stripped", "spaces survived");
		if (strstr(text, "\r\n") == NULL)
			fail("canonical text is not CRLF", "no CRLF found");

		if (pgp_checksum_lookup(text, "linux-7.2.3.tar.xz", sha, sizeof(sha)) != 0)
			fail("checksum lookup failed on verified text", "not found");
		else if (strlen(sha) != 64)
			fail("checksum is not 64 hex characters", sha);
		if (pgp_checksum_lookup(text, "linux-does-not-exist.tar.xz", sha, sizeof(sha)) == 0)
			fail("lookup invented a checksum for an absent file", sha);
	}
	free(text);
	text = NULL;

	/* one altered hex digit -- the exact attack this file exists to stop */
	tampered = strdup(SIGNED_DOC);
	if (tampered == NULL)
		return 1;
	for (i = 0; tampered[i] != '\0'; i++) {
		if (strncmp(tampered + i, "  linux-7.2.3.tar.xz", 20) == 0) {
			tampered[i - 1] = (char)(tampered[i - 1] == 'a' ? 'b' : 'a');
			break;
		}
	}
	r = verify(tampered, TEST_FPR, &text);
	if (r != PGP_VERIFY_BAD_SIGNATURE)
		fail("a tampered checksum was not rejected", pgp_verify_result_name(r));
	if (text != NULL)
		fail("failed verification still returned text", "text is not NULL");
	free(tampered);

	text = NULL;
	r = verify(SIGNED_DOC, "0000000000000000000000000000000000000000", &text);
	if (r != PGP_VERIFY_WRONG_KEY)
		fail("a wrong pinned fingerprint was accepted", pgp_verify_result_name(r));

	text = NULL;
	r = verify(SIGNED_DOC, "", &text);
	if (r != PGP_VERIFY_WRONG_KEY)
		fail("an empty fingerprint was not refused", pgp_verify_result_name(r));

	truncated = strdup(SIGNED_DOC);
	if (truncated == NULL)
		return 1;
	truncated[strlen(truncated) - 200] = '\0';
	text = NULL;
	r = verify(truncated, TEST_FPR, &text);
	if (r == PGP_VERIFY_OK)
		fail("a truncated document verified", "should be malformed or bad");
	free(truncated);

	{
		char fpr[PGP_FINGERPRINT_HEX_MAX], err[256];

		if (pgp_key_fingerprint(TEST_KEY, sizeof(TEST_KEY) - 1, fpr, err, sizeof(err)) !=
		        PGP_VERIFY_OK ||
		    strcmp(fpr, TEST_FPR) != 0)
			fail("the key's own fingerprint is not the one it verifies under", fpr);
		if (pgp_key_fingerprint("not a key", 9, fpr, err, sizeof(err)) == PGP_VERIFY_OK)
			fail("text that is not a key produced a fingerprint", fpr);
	}

	if (g_failures > 0) {
		fprintf(stderr, "PGP VERIFY: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("PGP VERIFY: ok (valid, canonicalisation, lookup, tampered, wrong key, no pin, "
	       "truncated, key fingerprint)\n");
	return 0;
}
