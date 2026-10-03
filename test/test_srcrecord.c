/*
 * ADR-0323: the record of what authenticate and author found for a
 * package, whatever kind discovers it. The kernel (kernel.org) and hibr
 * (gitea-tags) keep theirs side by side, and neither touches the other.
 */
#include "srcrecord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	char tmpl[] = "/tmp/cix_srcrecord_XXXXXX", cmd[300];
	struct srcrecord_note note, got;
	struct srcrecord_candidate c, gc;

	printf("test_srcrecord\n");
	if (mkdtemp(tmpl) == NULL || srcrecord_init(tmpl) != 0) {
		printf("  FAIL: could not set up the record directory\n");
		return 1;
	}
	check(srcrecord_note("kernel", &got) != 0 && srcrecord_candidate("kernel", &gc) != 0,
	      "no note and no candidate before anything ran");

	memset(&note, 0, sizeof(note));
	snprintf(note.version, sizeof(note.version), "7.2.8");
	snprintf(note.stage, sizeof(note.stage), "authenticate");
	snprintf(note.status, sizeof(note.status), "ok");
	snprintf(note.reason, sizeof(note.reason), "kernel.org's signed sha256sums.asc");
	memset(&c, 0, sizeof(c));
	snprintf(c.version, sizeof(c.version), "7.2.8");
	snprintf(c.url, sizeof(c.url), "https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.8.tar.xz");
	snprintf(c.sha256, sizeof(c.sha256), "%064d", 7);
	snprintf(c.verification, sizeof(c.verification), "checksums: kernel.org sha256sums.asc");
	check(srcrecord_store_note("kernel", &note) == 0 && srcrecord_store_candidate("kernel", &c) == 0,
	      "a note and a candidate are stored");
	check(srcrecord_note("kernel", &got) == 0 && strcmp(got.stage, "authenticate") == 0 &&
	          strcmp(got.version, "7.2.8") == 0 &&
	          strcmp(got.reason, "kernel.org's signed sha256sums.asc") == 0,
	      "the note reads back");
	check(srcrecord_candidate("kernel", &gc) == 0 && strcmp(gc.url, c.url) == 0 &&
	          strcmp(gc.sha256, c.sha256) == 0 && strcmp(gc.verification, c.verification) == 0,
	      "the candidate reads back");
	check(srcrecord_note("hibr", &got) != 0, "another package's record is its own");

	check(srcrecord_store_candidate("kernel", NULL) == 0 &&
	          srcrecord_candidate("kernel", &gc) != 0 && srcrecord_note("kernel", &got) == 0,
	      "clearing the candidate leaves the note");
	check(srcrecord_store_note("kernel", NULL) == 0 && srcrecord_note("kernel", &got) != 0,
	      "and the note clears");
	snprintf(c.sha256, sizeof(c.sha256), "short");
	check(srcrecord_store_candidate("kernel", &c) == 0 && srcrecord_candidate("kernel", &gc) != 0,
	      "a candidate without a full sha256 is not a candidate");
	check(srcrecord_note("../etc", &got) != 0 && srcrecord_store_note("../etc", &note) != 0,
	      "a name that is not a package name is no file");

	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpl);
	if (system(cmd) != 0)
		printf("  (could not remove %s)\n", tmpl);
	printf("SRCRECORD RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
