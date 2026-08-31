/*
 * test_releasekey -- proves our minisign signatures against the real
 * minisign (ADR-0220).
 *
 * The oracle is stock upstream `minisign`, never a verifier written
 * here. That is the whole point: this signature is the last thing
 * between a substituted installer ISO and a machine that boots it, so
 * checking our own format with our own reader would prove only that we
 * are self-consistent.
 *
 * Both directions are tested, and the negative half is the one that
 * matters. A signature that verifies is easy to produce by accident --
 * a truncated file, an unchecked comment, a signature over the wrong
 * bytes can all pass a careless check. What must be proven is that a
 * tampered artifact FAILS, and that the trusted comment is covered too,
 * since a signature which leaves it editable lets an attacker relabel a
 * genuine ISO.
 *
 * Skips cleanly when minisign is absent rather than passing: a test
 * that silently checks nothing is worse than one that says it cannot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "releasekey.h"

static int failures;

static int run(const char *cmd)
{
	int rc = system(cmd);

	return (rc == -1) ? -1 : rc;
}

static int have_minisign(void)
{
	return run("command -v minisign > /dev/null 2>&1") == 0;
}

int main(void)
{
	char dir[] = "/tmp/cix_relkey_XXXXXX";
	char cmd[2048], key_path[512], pem[4096], iso[512], sig[512], pub[512];
	FILE *f;
	size_t n;
	enum releasekey_error rc;

	if (!have_minisign()) {
		printf("RELEASEKEY RESULT: SKIP (minisign not installed -- this test needs the real\n"
		       "  verifier; checking our own format with our own reader would prove nothing)\n");
		return 0;
	}
	if (mkdtemp(dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
		return 1;
	}
	snprintf(key_path, sizeof(key_path), "%s/ed.pem", dir);
	snprintf(cmd, sizeof(cmd), "openssl genpkey -algorithm ed25519 -out '%s' 2>/dev/null",
	         key_path);
	if (run(cmd) != 0) {
		fprintf(stderr, "FAIL: could not generate an Ed25519 key\n");
		return 1;
	}
	releasekey_init(dir);

	/* An RSA key must be refused, and refused when it is INSTALLED --
	 * not later, on a real release, at signing time. */
	{
		char rsa_path[512];
		char rsa_pem[8192];

		snprintf(rsa_path, sizeof(rsa_path), "%s/rsa.pem", dir);
		snprintf(cmd, sizeof(cmd),
		         "openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out '%s' "
		         "2>/dev/null", rsa_path);
		run(cmd);
		f = fopen(rsa_path, "rb");
		if (f != NULL) {
			n = fread(rsa_pem, 1, sizeof(rsa_pem) - 1, f);
			fclose(f);
			rsa_pem[n] = '\0';
			if (releasekey_set(rsa_pem, n) == RELEASEKEY_OK) {
				fprintf(stderr,
				        "FAIL: an RSA key was accepted as a release key -- minisign is "
				        "Ed25519-only, so this would fail at signing time on a real "
				        "release instead of here\n");
				failures++;
			}
		}
	}

	f = fopen(key_path, "rb");
	if (f == NULL) {
		fprintf(stderr, "FAIL: cannot read the generated key\n");
		return 1;
	}
	n = fread(pem, 1, sizeof(pem) - 1, f);
	fclose(f);
	pem[n] = '\0';

	rc = releasekey_set(pem, n);
	if (rc != RELEASEKEY_OK) {
		fprintf(stderr, "FAIL: releasekey_set: %s\n", releasekey_strerror(rc));
		return 1;
	}
	if (!releasekey_is_set()) {
		fprintf(stderr, "FAIL: key reported not set after being set\n");
		failures++;
	}

	/* Public key file, in minisign's own format. */
	{
		char pubtext[1024];

		rc = releasekey_public(pubtext, sizeof(pubtext));
		if (rc != RELEASEKEY_OK) {
			fprintf(stderr, "FAIL: releasekey_public: %s\n", releasekey_strerror(rc));
			return 1;
		}
		snprintf(pub, sizeof(pub), "%s/cix.pub", dir);
		f = fopen(pub, "w");
		fputs(pubtext, f);
		fclose(f);
	}

	/* A stand-in for the ISO: the format does not care what the bytes are. */
	snprintf(iso, sizeof(iso), "%s/cix-installer.iso", dir);
	f = fopen(iso, "wb");
	fputs("not really an iso, but exactly as many bytes get signed\n", f);
	fclose(f);
	snprintf(sig, sizeof(sig), "%s.minisig", iso);

	rc = releasekey_sign_file(iso, sig, "timestamp:0\tfile:cix-installer.iso");
	if (rc != RELEASEKEY_OK) {
		fprintf(stderr, "FAIL: releasekey_sign_file: %s\n", releasekey_strerror(rc));
		return 1;
	}

	/* 1. The real verifier accepts it. */
	snprintf(cmd, sizeof(cmd), "minisign -Vm '%s' -p '%s' > /dev/null 2>&1", iso, pub);
	if (run(cmd) != 0) {
		fprintf(stderr, "FAIL: stock minisign rejected a signature we produced\n");
		failures++;
	}

	/* 2. One altered byte must break it. */
	{
		char tampered[512], tsig[512];

		snprintf(tampered, sizeof(tampered), "%s/tampered.iso", dir);
		snprintf(tsig, sizeof(tsig), "%s.minisig", tampered);
		snprintf(cmd, sizeof(cmd),
		         "cp '%s' '%s' && cp '%s' '%s' && printf 'X' | dd of='%s' bs=1 seek=3 "
		         "conv=notrunc 2>/dev/null", iso, tampered, sig, tsig, tampered);
		run(cmd);
		snprintf(cmd, sizeof(cmd), "minisign -Vm '%s' -p '%s' > /dev/null 2>&1", tampered, pub);
		if (run(cmd) == 0) {
			fprintf(stderr,
			        "FAIL: minisign ACCEPTED a tampered artifact -- a signature that does "
			        "not fail when it should is worth nothing\n");
			failures++;
		}
	}

	/* 3. The trusted comment must be covered by the global signature.
	 * Without it an attacker relabels a genuine ISO and it still
	 * verifies -- which is exactly what omitting that second signature
	 * would allow. */
	{
		char relabelled[512], rsig[512];

		snprintf(relabelled, sizeof(relabelled), "%s/relabelled.iso", dir);
		snprintf(rsig, sizeof(rsig), "%s.minisig", relabelled);
		snprintf(cmd, sizeof(cmd),
		         "cp '%s' '%s' && sed 's|file:cix-installer.iso|file:evil.iso|' '%s' > '%s'",
		         iso, relabelled, sig, rsig);
		run(cmd);
		snprintf(cmd, sizeof(cmd), "minisign -Vm '%s' -p '%s' > /dev/null 2>&1", relabelled,
		         pub);
		if (run(cmd) == 0) {
			fprintf(stderr,
			        "FAIL: minisign ACCEPTED an edited trusted comment -- the global "
			        "signature over signature||comment is missing or wrong\n");
			failures++;
		}
	}

	/* 4. A different key must not verify it. */
	{
		char other[512], otherpub[512];

		snprintf(other, sizeof(other), "%s/other.pem", dir);
		snprintf(cmd, sizeof(cmd), "openssl genpkey -algorithm ed25519 -out '%s' 2>/dev/null",
		         other);
		run(cmd);
		f = fopen(other, "rb");
		n = fread(pem, 1, sizeof(pem) - 1, f);
		fclose(f);
		pem[n] = '\0';
		{
			char other_dir[512];
			char pubtext[1024];

			snprintf(other_dir, sizeof(other_dir), "%s/other", dir);
			snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", other_dir);
			run(cmd);
			releasekey_init(other_dir);
			if (releasekey_set(pem, n) == RELEASEKEY_OK &&
			    releasekey_public(pubtext, sizeof(pubtext)) == RELEASEKEY_OK) {
				snprintf(otherpub, sizeof(otherpub), "%s/other.pub", dir);
				f = fopen(otherpub, "w");
				fputs(pubtext, f);
				fclose(f);
				snprintf(cmd, sizeof(cmd), "minisign -Vm '%s' -p '%s' > /dev/null 2>&1", iso,
				         otherpub);
				if (run(cmd) == 0) {
					fprintf(stderr, "FAIL: a different key verified our signature\n");
					failures++;
				}
			}
		}
		releasekey_init(dir);
	}

	/* 5. Clearing really removes it. */
	releasekey_clear();
	if (releasekey_is_set()) {
		fprintf(stderr, "FAIL: the key is still reported set after being cleared\n");
		failures++;
	}

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	run(cmd);

	if (failures == 0)
		printf("RELEASEKEY RESULT: PASS\n");
	else
		printf("RELEASEKEY RESULT: FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
