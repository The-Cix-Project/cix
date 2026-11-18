/*
 * ADR-0379: the per-slot record that keeps a root-only roll from
 * redeploying a root that failed or that an operator left. Runs the
 * module against a temporary file and reloads it between steps, since
 * every decision it feeds is made by a later process.
 */
#include "rootslot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;
static int g_changes;
static char g_path[256];

#define CHECK(cond, ...)                                                                      \
	do {                                                                                  \
		if (!(cond)) {                                                                \
			fprintf(stderr, "FAIL: ");                                            \
			fprintf(stderr, __VA_ARGS__);                                         \
			fprintf(stderr, "\n");                                                \
			g_failures++;                                                         \
		}                                                                             \
	} while (0)

static void on_change(void)
{
	g_changes++;
}

/* A later process reading what this one wrote. */
static void reload(void)
{
	rootslot_load(g_path);
}

int main(void)
{
	char dir[] = "/tmp/test_rootslot_XXXXXX";
	char bad[80];
	int n;

	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(g_path, sizeof(g_path), "%s/root-slots.json", dir);
	rootslot_set_on_change(on_change);

	/* An absent file is an empty record. */
	reload();
	CHECK(rootslot_version("a")[0] == '\0' && rootslot_version("b")[0] == '\0',
	      "a fresh record knows no slot");
	CHECK(rootslot_bad()[0] == '\0', "a fresh record has no bad version");
	CHECK(rootslot_version("c")[0] == '\0' && rootslot_version(NULL)[0] == '\0',
	      "an unknown slot name is unknown, never NULL");

	/* set, persist, clear */
	rootslot_set("a", "v1", "c1");
	reload();
	CHECK(strcmp(rootslot_version("a"), "v1") == 0, "a set version survives a reload, got %s",
	      rootslot_version("a"));
	n = g_changes;
	rootslot_set("a", "v1", "c1");
	CHECK(g_changes == n, "setting the same version again writes and announces nothing");
	rootslot_clear("a");
	reload();
	CHECK(rootslot_version("a")[0] == '\0', "a cleared slot reloads unknown");

	/* An attempt with no version records nothing. */
	rootslot_attempt("b");
	CHECK(rootslot_resolve_start("a", bad, sizeof(bad)) == ROOTSLOT_START_SAME,
	      "an attempt on a slot with no version is not recorded");

	/* Attempt on b, the next process runs a: that version failed. */
	rootslot_set("a", "v1", "c1");
	rootslot_set("b", "v2", "c2");
	rootslot_confirmed("a");
	rootslot_attempt("b");
	reload();
	CHECK(rootslot_resolve_start("a", bad, sizeof(bad)) == ROOTSLOT_START_FAILED,
	      "an attempt on b resolved by a process running a failed");
	CHECK(strcmp(bad, "v2") == 0 && strcmp(rootslot_bad(), "v2") == 0,
	      "the failed attempt's version is bad, got %s / %s", bad, rootslot_bad());
	CHECK(strcmp(rootslot_bad_cixd(), "c2") == 0,
	      "ADR-0380: the bad mark takes the attempt's cixd, got %s", rootslot_bad_cixd());
	reload();
	CHECK(strcmp(rootslot_bad(), "v2") == 0 && strcmp(rootslot_bad_cixd(), "c2") == 0,
	      "the bad mark and its cixd survive a reload");
	CHECK(strcmp(rootslot_cixd("a"), "c1") == 0 && strcmp(rootslot_cixd("b"), "c2") == 0,
	      "each slot's cixd survives a reload, got %s / %s", rootslot_cixd("a"),
	      rootslot_cixd("b"));
	CHECK(rootslot_resolve_start("a", bad, sizeof(bad)) == ROOTSLOT_START_SAME,
	      "a resolved attempt is not resolved twice");
	rootslot_clear_bad();
	reload();
	CHECK(rootslot_bad()[0] == '\0', "a cleared bad mark reloads clear");

	/* Attempt on b, the next process runs b and confirms it. */
	rootslot_attempt("b");
	reload();
	CHECK(rootslot_resolve_start("b", bad, sizeof(bad)) == ROOTSLOT_START_SAME,
	      "an attempt on the slot now running waits for confirmation");
	rootslot_confirmed("b");
	reload();
	CHECK(rootslot_resolve_start("b", bad, sizeof(bad)) == ROOTSLOT_START_SAME &&
	          rootslot_bad()[0] == '\0',
	      "a confirmed attempt marks nothing bad");

	/* The operator's boot-next: confirmed b, the host starts on a with no
	 * attempt. What it left (b's version) must not be redeployed. */
	reload();
	CHECK(rootslot_resolve_start("a", bad, sizeof(bad)) == ROOTSLOT_START_REJECTED,
	      "leaving the confirmed slot with no attempt is a rejection");
	CHECK(strcmp(bad, "v2") == 0 && strcmp(rootslot_bad(), "v2") == 0,
	      "the version left behind is bad, got %s / %s", bad, rootslot_bad());
	CHECK(strcmp(rootslot_bad_cixd(), "c2") == 0,
	      "ADR-0380: a rejected release is its cixd, got %s", rootslot_bad_cixd());
	rootslot_confirmed("a");
	CHECK(strcmp(rootslot_bad(), "v2") == 0 && strcmp(rootslot_bad_cixd(), "c2") == 0,
	      "confirming the slot chosen does not clear the rejection");
	reload();
	CHECK(rootslot_resolve_start("a", bad, sizeof(bad)) == ROOTSLOT_START_SAME,
	      "once a is confirmed, starting on a resolves nothing");

	/*
	 * A refused switch in this process: the version is bad, and the
	 * attempt stays, because the staged slot is still the next boot.
	 * Case 1: the next start is on a (a plain restart, the old slot):
	 * the attempt failed, and nothing good is marked bad.
	 */
	rootslot_clear_bad();
	rootslot_set("b", "v3", "c3");
	rootslot_attempt("b");
	rootslot_mark_bad("v3", "c3");
	reload();
	CHECK(strcmp(rootslot_bad(), "v3") == 0, "a refused switch marks its version bad, got %s",
	      rootslot_bad());
	CHECK(rootslot_resolve_start("a", bad, sizeof(bad)) == ROOTSLOT_START_FAILED &&
	          strcmp(rootslot_bad(), "v3") == 0,
	      "the next start on the old slot resolves the attempt to the same bad version, "
	      "never the good one (got %s)",
	      rootslot_bad());

	/* Case 2: a later reboot boots the staged slot b, which confirms. It
	 * is no rejection of a, and the bad mark on b's version clears. */
	rootslot_attempt("b");
	rootslot_mark_bad("v3", "c3");
	reload();
	CHECK(rootslot_resolve_start("b", bad, sizeof(bad)) == ROOTSLOT_START_SAME,
	      "booting the staged slot with its attempt recorded is not a rejection");
	CHECK(strcmp(rootslot_bad(), "v3") == 0, "until it confirms, the mark stays");
	rootslot_confirmed("b");
	reload();
	CHECK(rootslot_bad()[0] == '\0',
	      "a confirmed boot of the bad version clears the mark, got %s", rootslot_bad());

	/*
	 * ADR-0380: both slots hold one cixd (a root-only roll moved a
	 * library). Rejecting b's root does not blame that cixd: confirming a,
	 * which runs it, forgets the cixd and keeps the version bad.
	 */
	rootslot_set("a", "v5", "c5");
	rootslot_set("b", "v6", "c5");
	rootslot_confirmed("b");
	reload();
	CHECK(rootslot_resolve_start("a", bad, sizeof(bad)) == ROOTSLOT_START_REJECTED &&
	          strcmp(rootslot_bad(), "v6") == 0 && strcmp(rootslot_bad_cixd(), "c5") == 0,
	      "leaving b marks its version and cixd, got %s / %s", rootslot_bad(),
	      rootslot_bad_cixd());
	rootslot_confirmed("a");
	reload();
	CHECK(strcmp(rootslot_bad(), "v6") == 0 && rootslot_bad_cixd()[0] == '\0',
	      "a confirmed root running the same cixd keeps the version bad and forgets the "
	      "cixd, got %s / %s",
	      rootslot_bad(), rootslot_bad_cixd());
	rootslot_clear_bad();
	CHECK(rootslot_bad()[0] == '\0' && rootslot_bad_cixd()[0] == '\0',
	      "clear_bad clears both");

	/* A record written before ADR-0380 has no cixd fields. */
	{
		FILE *f = fopen(g_path, "w");

		if (f != NULL) {
			fputs("{\"a\":\"v1\",\"bad\":\"v2\",\"confirmed_slot\":\"a\"}", f);
			fclose(f);
		}
		reload();
		CHECK(strcmp(rootslot_version("a"), "v1") == 0 && rootslot_cixd("a")[0] == '\0' &&
		          strcmp(rootslot_bad(), "v2") == 0 && rootslot_bad_cixd()[0] == '\0',
		      "an old record reads with empty cixd fields");
		rootslot_clear_bad();
	}

	/* mark_bad */
	rootslot_mark_bad("v4", "");
	reload();
	CHECK(strcmp(rootslot_bad(), "v4") == 0, "mark_bad persists, got %s", rootslot_bad());
	rootslot_mark_bad("", "c9");
	CHECK(strcmp(rootslot_bad(), "v4") == 0, "an empty version marks nothing");

	unlink(g_path);
	rmdir(dir);
	if (g_failures != 0)
		return 1;
	printf("test_rootslot: every case passes\n");
	return 0;
}
