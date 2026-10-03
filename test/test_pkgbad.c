/*
 * ADR-0323 answer 4: the bad-versions list, with no daemon. What
 * matters is that a mark survives a restart (or the next boot would roll
 * straight back into the failure), and that it ends when the package
 * has moved on.
 */
#include "pkgbad.h"

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

static struct pkgbad_entry mark(const char *package, const char *version)
{
	struct pkgbad_entry e;

	memset(&e, 0, sizeof(e));
	snprintf(e.package, sizeof(e.package), "%s", package);
	snprintf(e.version, sizeof(e.version), "%s", version);
	snprintf(e.image, sizeof(e.image), "%s", "jumpbox");
	snprintf(e.container, sizeof(e.container), "%s", "jump");
	snprintf(e.reason, sizeof(e.reason), "%s", "crash-looped: 3 exits within 300 s");
	e.at = 1790000000LL;
	return e;
}

/* "hibr has a newer recipe than 0.99.11-1"; nothing else does. */
static int superseded(const char *package, const char *version)
{
	return strcmp(package, "hibr") == 0 && strcmp(version, "0.99.11-1") == 0;
}

int main(void)
{
	char path[128];
	struct pkgbad_entry e;
	int i;

	printf("test_pkgbad\n");
	snprintf(path, sizeof(path), "/tmp/cix_pkgbad_%d.json", (int)getpid());
	unlink(path);

	check(pkgbad_init(path) == 0 && pkgbad_count() == 0, "a fresh host has no marks");
	e = mark("hibr", "0.99.11-1");
	check(pkgbad_add(&e) == 0 && pkgbad_is_bad("hibr", "0.99.11-1"), "a version is marked");
	check(pkgbad_find("hibr", "0.99.11-1") != NULL &&
	          strcmp(pkgbad_find("hibr", "0.99.11-1")->container, "jump") == 0 &&
	          pkgbad_find("hibr", "0.99.4-3") == NULL,
	      "and its mark can be read back");
	check(!pkgbad_is_bad("hibr", "0.99.4-3"), "another version of it is not");
	e = mark("openssl", "3.5.1-2");
	check(pkgbad_add(&e) == 0 && pkgbad_count() == 2, "a second mark");
	snprintf(e.reason, sizeof(e.reason), "%s", "never ready within 300 s");
	check(pkgbad_add(&e) == 0 && pkgbad_count() == 2 &&
	          strcmp(pkgbad_at(1)->reason, "never ready within 300 s") == 0,
	      "marking it again replaces the mark, not duplicates it");

	check(pkgbad_init(path) == 0 && pkgbad_count() == 2 && pkgbad_is_bad("openssl", "3.5.1-2") &&
	          strcmp(pkgbad_at(0)->container, "jump") == 0 && pkgbad_at(0)->at == 1790000000LL,
	      "the marks survive a restart, fields intact");

	check(pkgbad_prune(superseded) == 1 && !pkgbad_is_bad("hibr", "0.99.11-1") &&
	          pkgbad_is_bad("openssl", "3.5.1-2"),
	      "a mark ends when its package has moved on, and only that one");
	check(pkgbad_remove("openssl", "3.5.1-2") == 0 && pkgbad_count() == 0,
	      "an operator clears a mark");
	check(pkgbad_remove("openssl", "3.5.1-2") == 1, "clearing it again reports there was none");
	check(pkgbad_init(path) == 0 && pkgbad_count() == 0, "and the clearing is saved");

	for (i = 0; i < PKGBAD_MAX + 3; i++) {
		char v[16];

		snprintf(v, sizeof(v), "%d-1", i);
		e = mark("many", v);
		pkgbad_add(&e);
	}
	check(pkgbad_count() == PKGBAD_MAX && !pkgbad_is_bad("many", "0-1") &&
	          pkgbad_is_bad("many", "66-1"),
	      "a full list drops the oldest mark, never the newest failure");

	unlink(path);
	printf("PKGBAD RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
