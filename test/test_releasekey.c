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
	int have_ms;
	enum releasekey_error rc;

	/*
	 * The oracle half needs stock minisign; the verifier half needs
	 * only openssl. This used to skip the WHOLE test when minisign was
	 * absent -- and minisign is in no recipe and in no image, so it
	 * skipped on every release since ADR-0220 and nothing said so
	 * louder than one line (#406). Granular now: what can run, runs.
	 */
	have_ms = have_minisign();
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

	/*
	 * Cases 1 to 4 are the oracle half: stock minisign, never a reader
	 * written here, because checking our own format with our own code
	 * would prove only that we are self-consistent.
	 */
	if (have_ms) {
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
	} else {
		printf("  note: stock minisign is absent, so cases 1-4 (our output accepted by the\n"
		       "        real verifier) and 6-7 (its output accepted by ours) did not run.\n");
	}

	/*
	 * ---- verification (ADR-0279) ----
	 *
	 * Everything above proves stock minisign accepts what we write. This
	 * half proves the reverse: that our own verifier accepts what stock
	 * minisign writes, and -- the part that matters -- that it refuses
	 * what it must. A verifier is only worth the failures it produces.
	 */
	{
		char trust[512], foreign[512], empty[512], vc[512];
		char msec[512], mpub[512], mart[512], msig[512], edsig[512];
		char tampered[512], tsig[512], relabelled[512], rsig[512], badsig[512];
		enum releasekey_error vr;

		snprintf(trust, sizeof(trust), "%s/trusted", dir);
		/* docs/keys/ holds an X.509 certificate and a PGP block beside
		 * the minisign keys, so an entry that is not one must be
		 * skipped rather than ending the search. */
		snprintf(cmd, sizeof(cmd),
		         "mkdir -p '%s' && cp '%s' '%s/cix.pub' && printf 'not a key\n' > '%s/README.md'",
		         trust, pub, trust, trust);
		run(cmd);

		/* 5. Our own signature verifies, and hands back its comment. */
		vc[0] = '\0';
		vr = releasekey_verify_file(iso, sig, trust, vc, sizeof(vc));
		if (vr != RELEASEKEY_OK) {
			fprintf(stderr, "FAIL: our verifier rejected our own signature: %s\n",
			        releasekey_strerror(vr));
			failures++;
		} else if (strcmp(vc, "timestamp:0\tfile:cix-installer.iso") != 0) {
			fprintf(stderr, "FAIL: trusted comment came back as \"%s\"\n", vc);
			failures++;
		}

		/* 6. A signature written by STOCK minisign verifies. Checking
		 * only our own output would prove we are self-consistent, which
		 * is the trap this whole file exists to avoid. */
		if (have_ms) {
		snprintf(msec, sizeof(msec), "%s/ms.key", dir);
		snprintf(mpub, sizeof(mpub), "%s/ms.pub", dir);
		snprintf(mart, sizeof(mart), "%s/ms.bin", dir);
		snprintf(msig, sizeof(msig), "%s/ms.bin.minisig", dir);
		snprintf(cmd, sizeof(cmd),
		         "minisign -G -W -p '%s' -s '%s' > /dev/null 2>&1 && "
		         "printf 'oracle payload\n' > '%s' && "
		         "minisign -S -l -s '%s' -m '%s' -t 'cix pkg probe@1-1 sha256=00' "
		         "> /dev/null 2>&1 && cp '%s' '%s/ms.pub'",
		         mpub, msec, mart, msec, mart, mpub, trust);
		if (run(cmd) != 0) {
			fprintf(stderr, "FAIL: could not produce a stock-minisign legacy signature\n");
			failures++;
		} else {
			vc[0] = '\0';
			vr = releasekey_verify_file(mart, msig, trust, vc, sizeof(vc));
			if (vr != RELEASEKEY_OK) {
				fprintf(stderr,
				        "FAIL: our verifier rejected a signature stock minisign produced: "
				        "%s\n",
				        releasekey_strerror(vr));
				failures++;
			} else if (strcmp(vc, "cix pkg probe@1-1 sha256=00") != 0) {
				fprintf(stderr, "FAIL: minisign's trusted comment came back as \"%s\"\n", vc);
				failures++;
			}
		}

		/*
		 * 7. Prehashed "ED" must be REFUSED, not verified the wrong way.
		 *
		 * This is minisign's DEFAULT signing mode -- `-l` above is what
		 * asks for legacy -- so anyone signing a Cix artifact by hand
		 * with stock minisign produces one of these. It uses a
		 * BLAKE2b-512 prehash the daemon cannot compute, so the only
		 * safe answer is to say so. Verifying it as though it were "Ed"
		 * would check a signature over the wrong bytes.
		 */
		snprintf(edsig, sizeof(edsig), "%s/ms-prehashed.minisig", dir);
		snprintf(cmd, sizeof(cmd),
		         "minisign -S -s '%s' -m '%s' -x '%s' -t 'prehashed' > /dev/null 2>&1", msec,
		         mart, edsig);
		if (run(cmd) == 0) {
			vr = releasekey_verify_file(mart, edsig, trust, NULL, 0);
			if (vr != RELEASEKEY_ERR_BAD_SIG) {
				fprintf(stderr,
				        "FAIL: a prehashed \"ED\" signature returned %s -- it must be refused "
				        "as unparseable, never checked as if it were legacy \"Ed\"\n",
				        releasekey_strerror(vr));
				failures++;
			}
		}

		}

		/* 8. One altered byte in the artifact must fail. */
		snprintf(tampered, sizeof(tampered), "%s/v-tampered.iso", dir);
		snprintf(tsig, sizeof(tsig), "%s.minisig", tampered);
		snprintf(cmd, sizeof(cmd),
		         "cp '%s' '%s' && cp '%s' '%s' && printf 'X' | dd of='%s' bs=1 seek=3 "
		         "conv=notrunc 2>/dev/null",
		         iso, tampered, sig, tsig, tampered);
		run(cmd);
		vr = releasekey_verify_file(tampered, tsig, trust, NULL, 0);
		if (vr != RELEASEKEY_ERR_VERIFY) {
			fprintf(stderr, "FAIL: a tampered artifact returned %s, expected a verify failure\n",
			        releasekey_strerror(vr));
			failures++;
		}

		/*
		 * 9. An edited trusted comment must fail.
		 *
		 * The file is untouched, so the FIRST signature still verifies
		 * perfectly -- only the global signature over
		 * signature||trusted_comment catches this. A verifier that
		 * skipped it would accept a genuine artifact relabelled as
		 * another package, which is the whole reason ADR-0279 puts the
		 * revision binding in that comment.
		 */
		snprintf(relabelled, sizeof(relabelled), "%s/v-relabelled.iso", dir);
		snprintf(rsig, sizeof(rsig), "%s.minisig", relabelled);
		snprintf(cmd, sizeof(cmd),
		         "cp '%s' '%s' && sed 's|file:cix-installer.iso|file:evil.iso|' '%s' > '%s'", iso,
		         relabelled, sig, rsig);
		run(cmd);
		vr = releasekey_verify_file(relabelled, rsig, trust, NULL, 0);
		if (vr != RELEASEKEY_ERR_VERIFY) {
			fprintf(stderr,
			        "FAIL: an edited trusted comment returned %s -- the global signature over "
			        "signature||comment is not being checked\n",
			        releasekey_strerror(vr));
			failures++;
		}

		/* 10. A key the store does not hold is a hard failure, never a
		 * fall-through to trusting the bytes anyway. */
		snprintf(foreign, sizeof(foreign), "%s/foreign", dir);
		snprintf(cmd, sizeof(cmd),
		         "mkdir -p '%s' && openssl genpkey -algorithm ed25519 -out '%s/other.pem' "
		         "2>/dev/null",
		         foreign, foreign);
		run(cmd);
		/* A real, well-formed minisign public key that simply is not
		 * the signer -- so this tests attribution, not parsing. */
		{
			char fpub[1024];
			char fdir[512];

			snprintf(fdir, sizeof(fdir), "%s/foreignkey", dir);
			snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", fdir);
			run(cmd);
			releasekey_init(fdir);
			snprintf(cmd, sizeof(cmd), "cp '%s/other.pem' '%s/cix-release.key'", foreign, fdir);
			run(cmd);
			if (releasekey_public(fpub, sizeof(fpub)) == RELEASEKEY_OK) {
				char fpath[512];

				snprintf(fpath, sizeof(fpath), "%s/foreign.pub", foreign);
				f = fopen(fpath, "w");
				fputs(fpub, f);
				fclose(f);
			}
			releasekey_init(dir);
		}
		vr = releasekey_verify_file(iso, sig, foreign, NULL, 0);
		if (vr != RELEASEKEY_ERR_UNKNOWN_KEY) {
			fprintf(stderr, "FAIL: an untrusted signer returned %s, expected unknown key\n",
			        releasekey_strerror(vr));
			failures++;
		}

		/* 11. An empty trust store verifies NOTHING. It must not
		 * degrade into accepting anything, which is the failure mode a
		 * fresh box would hit first. */
		snprintf(empty, sizeof(empty), "%s/empty", dir);
		snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", empty);
		run(cmd);
		vr = releasekey_verify_file(iso, sig, empty, NULL, 0);
		if (vr != RELEASEKEY_ERR_UNKNOWN_KEY) {
			fprintf(stderr, "FAIL: an empty trust store returned %s, expected unknown key\n",
			        releasekey_strerror(vr));
			failures++;
		}
		vr = releasekey_verify_file(iso, sig, "/nonexistent/trust/dir", NULL, 0);
		if (vr != RELEASEKEY_ERR_UNKNOWN_KEY) {
			fprintf(stderr, "FAIL: an absent trust store returned %s, expected unknown key\n",
			        releasekey_strerror(vr));
			failures++;
		}

		/* 12. A truncated signature file is malformed, not a verify
		 * failure -- the two mean different things to a caller. */
		snprintf(badsig, sizeof(badsig), "%s/truncated.minisig", dir);
		snprintf(cmd, sizeof(cmd), "head -3 '%s' > '%s'", sig, badsig);
		run(cmd);
		vr = releasekey_verify_file(iso, badsig, trust, NULL, 0);
		if (vr != RELEASEKEY_ERR_BAD_SIG) {
			fprintf(stderr, "FAIL: a truncated signature returned %s, expected bad-sig\n",
			        releasekey_strerror(vr));
			failures++;
		}
	}

	/* 13. Clearing really removes it. */
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
