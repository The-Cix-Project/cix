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

	/* ADR-0326: the catalogue supplies keys, per source. */
	{
		struct upstreamkeys_offer offers[2];
		const char *keep[1] = { "other" };
		int refused = -1;

		offers[0].package = "kernel";
		offers[0].fingerprint = TEST_FPR;
		offers[0].armored = TEST_KEY;
		offers[0].len = sizeof(TEST_KEY) - 1;
		offers[1].package = "kernel";
		offers[1].fingerprint = "0000000000000000000000000000000000000000";
		offers[1].armored = TEST_KEY;
		offers[1].len = sizeof(TEST_KEY) - 1;
		check(upstreamkeys_sync_source("cix-recipes", offers, 2, 1790000100LL, &refused, err,
		                               sizeof(err)) == 1 &&
		          refused == 1 && strstr(err, TEST_FPR) != NULL,
		      "a source's key is adopted, and one filed under another fingerprint refused");
		check(upstreamkeys_find("kernel", TEST_FPR, NULL) != NULL,
		      "the adopted key authenticates the package");
		check(upstreamkeys_remove("kernel", TEST_FPR) == 2,
		      "an operator cannot remove a key the catalogue supplies");
		check(upstreamkeys_add("kernel", TEST_FPR, TEST_KEY, sizeof(TEST_KEY) - 1, 1, err,
		                       sizeof(err)) != 0 &&
		          strstr(err, "cix-recipes") != NULL,
		      "nor replace it, and is told which source to change");
		check(upstreamkeys_init(path) == 0 && upstreamkeys_find("kernel", TEST_FPR, NULL) != NULL,
		      "the catalogue's key survives a restart");
		check(upstreamkeys_sync_source("cix-recipes", NULL, 0, 1790000200LL, &refused, err,
		                               sizeof(err)) == 0 &&
		          upstreamkeys_find("kernel", TEST_FPR, NULL) == NULL,
		      "a key removed from the source leaves at the next sync");
		check(upstreamkeys_sync_source("cix-recipes", offers, 1, 1790000300LL, &refused, err,
		                               sizeof(err)) == 1 &&
		          upstreamkeys_retain_sources(keep, 1) == 0 &&
		          upstreamkeys_find("kernel", TEST_FPR, NULL) == NULL,
		      "and a source an operator removed takes its keys with it");
	}

	unlink(path);
	printf("UPSTREAMKEYS RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL",
	       failures);
	return failures == 0 ? 0 : 1;
}
