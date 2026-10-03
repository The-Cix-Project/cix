/*
 * ADR-0318's upstream key store, with no daemon: a key enters only
 * under its own fingerprint, answers only for its own package, survives
 * a restart, and is the key a release is then verified with.
 */
#include "pgpverify.h"
#include "test_pgp_fixture.h"
#include "upstreamkeys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EMPTY_ARMOR "-----BEGIN PGP PUBLIC KEY BLOCK-----\n\n-----END PGP PUBLIC KEY BLOCK-----\n"

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

int main(void)
{
	char path[128], err[512], spaced[64];
	const char *key;
	char *text = NULL;
	size_t len = 0;
	int i, n;

	printf("test_upstreamkeys\n");
	snprintf(path, sizeof(path), "/tmp/cix_upstreamkeys_%d.json", (int)getpid());
	unlink(path);

	check(upstreamkeys_init(path) == 0 && upstreamkeys_find("kernel", TEST_FPR, NULL) == NULL,
	      "a fresh host trusts no upstream key");
	check(upstreamkeys_add("kernel", "0000000000000000000000000000000000000000", TEST_KEY,
	                       sizeof(TEST_KEY) - 1, 1, err, sizeof(err)) != 0 &&
	          strstr(err, TEST_FPR) != NULL,
	      "a key pinned under someone else's fingerprint is refused, naming its own");
	check(upstreamkeys_add("kernel", "B886", TEST_KEY, sizeof(TEST_KEY) - 1, 1, err,
	                       sizeof(err)) != 0,
	      "a fingerprint that is not 40 hex digits is refused");
	check(upstreamkeys_add("kernel", TEST_FPR, EMPTY_ARMOR, strlen(EMPTY_ARMOR), 1,
	                       err, sizeof(err)) != 0,
	      "text that is not a usable key is refused");

	/* Pinned the way a person copies it: spaced, lower case. */
	for (i = 0, n = 0; TEST_FPR[i] != '\0'; i++) {
		if (i > 0 && i % 4 == 0)
			spaced[n++] = ' ';
		spaced[n++] = (char)(TEST_FPR[i] >= 'A' && TEST_FPR[i] <= 'F' ? TEST_FPR[i] + 32
		                                                                 : TEST_FPR[i]);
	}
	spaced[n] = '\0';
	check(upstreamkeys_add("kernel", spaced, TEST_KEY, sizeof(TEST_KEY) - 1, 1790000000LL, err,
	                       sizeof(err)) == 0,
	      "the key is added under its own fingerprint, however the pin is spelled");
	check(upstreamkeys_find("hibr", TEST_FPR, NULL) == NULL,
	      "and answers for no other package");

	check(upstreamkeys_init(path) == 0, "reload");
	key = upstreamkeys_find("kernel", TEST_FPR, &len);
	check(key != NULL && len == sizeof(TEST_KEY) - 1, "it survives a restart, bytes intact");
	check(key != NULL &&
	          pgp_clearsign_verify(SIGNED_DOC, strlen(SIGNED_DOC), key, len, TEST_FPR, &text,
	                               NULL, err, sizeof(err)) == PGP_VERIFY_OK,
	      "and a release list verifies with the stored key");
	free(text);

	check(upstreamkeys_remove("kernel", spaced) == 0 &&
	          upstreamkeys_find("kernel", TEST_FPR, NULL) == NULL,
	      "an operator removes it");
	check(upstreamkeys_remove("kernel", TEST_FPR) == 1, "removing it again says there was none");
	check(upstreamkeys_init(path) == 0 && upstreamkeys_find("kernel", TEST_FPR, NULL) == NULL,
	      "and the removal is saved");

	unlink(path);
	printf("UPSTREAMKEYS RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL",
	       failures);
	return failures == 0 ? 0 : 1;
}
