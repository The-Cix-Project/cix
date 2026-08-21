/*
 * ADR-0179 (issue #29): unit test for the subordinate-ID allocator.
 * Links subid.c + its ldap.c dependency directly (no daemon
 * subprocess -- this is pure in-process allocator logic) against a
 * throwaway state file, and asserts the four properties the ADR names:
 * per-key idempotence, distinct non-overlapping ranges, the 100000
 * floor, and persistence across a reload.
 */
#include "subid.h"
#include "ldap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* subid.c calls exactly one ldap.c symbol (the collision check). Stub
 * it so this unit test links against subid.c alone, without dragging
 * in ldap.c's own hostauth/registry dependency chain -- the collision
 * logic itself is exercised via subid_overlaps_uidnumber() below, and
 * the ADR's floor makes the ldap-direction check a belt-and-suspenders
 * guard, verified in test_ldap's own user-create path instead. */
int ldap_user_uidnumber_in_range(long long lo, long long hi)
{
	(void)lo;
	(void)hi;
	return 0;
}

static int ok = 1;

#define CHECK(cond, msg)                                                                          \
	do {                                                                                       \
		if (!(cond)) {                                                                      \
			fprintf(stderr, "FAIL: %s\n", msg);                                        \
			ok = 0;                                                                     \
		}                                                                                  \
	} while (0)

int main(void)
{
	char tmpl[] = "/tmp/thinc_subid_test_XXXXXX";
	int fd = mkstemp(tmpl);
	long long a, a2, b, c;

	if (fd < 0) {
		perror("mkstemp");
		return 1;
	}
	close(fd);
	unlink(tmpl); /* subid_init tolerates a missing file (fresh table) */

	CHECK(subid_init(tmpl) == 0, "subid_init on a fresh path");

	/* First allocation lands exactly on the floor. */
	CHECK(subid_lookup_or_assign("op-1000", &a) == 0, "assign op-1000");
	CHECK(a == SUBID_FLOOR, "first range starts at the 100000 floor");

	/* Idempotent per key. */
	CHECK(subid_lookup_or_assign("op-1000", &a2) == 0, "re-lookup op-1000");
	CHECK(a2 == a, "same key returns the same base forever");

	/* A distinct key gets a distinct, non-overlapping range. */
	CHECK(subid_lookup_or_assign("op-2000", &b) == 0, "assign op-2000");
	CHECK(b == a + SUBID_RANGE_LEN, "second key one full width past the first");
	CHECK(b >= a + SUBID_RANGE_LEN && a + SUBID_RANGE_LEN <= b,
	      "ranges do not overlap");

	/* A third, keyed on a container name (host-auth-off path). */
	CHECK(subid_lookup_or_assign("container:jump", &c) == 0, "assign by container name");
	CHECK(c == b + SUBID_RANGE_LEN, "third distinct range");

	/* overlaps check reports membership correctly. */
	CHECK(subid_overlaps_uidnumber(a) == 1, "floor base is inside a range");
	CHECK(subid_overlaps_uidnumber(a + SUBID_RANGE_LEN - 1) == 1, "range top is inside");
	CHECK(subid_overlaps_uidnumber(SUBID_FLOOR - 1) == 0, "just below the floor is outside");
	CHECK(subid_overlaps_uidnumber(5001) == 0, "a conventional LDAP uid is outside");

	/* Persistence across a reload: a fresh init of the same file must
	 * return the identical bases, and a new key must continue past
	 * them (not restart at the floor). */
	CHECK(subid_init(tmpl) == 0, "re-init reloads the persisted table");
	{
		long long a_reload, d;

		CHECK(subid_lookup_or_assign("op-1000", &a_reload) == 0, "re-lookup after reload");
		CHECK(a_reload == a, "persisted base survives a reload");
		CHECK(subid_lookup_or_assign("op-3000", &d) == 0, "assign a new key after reload");
		CHECK(d == c + SUBID_RANGE_LEN, "new range continues past the reloaded ones");
	}

	unlink(tmpl);
	printf("SUBID RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
