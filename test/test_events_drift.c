/*
 * test_events_drift -- the daemon's check of its own announcements
 * (ADR-0339 point 7), proved without a daemon.
 *
 * The end-to-end tests can show that the check reports nothing when
 * everything was announced. They cannot show that it reports something
 * when an announcement is MISSING, because nothing in a test daemon
 * changes state behind its own back cleanly. So the decision itself --
 * events_hash_body() and events_observe() in daemon/src/events.c, pure
 * functions -- is driven here directly, including the case the check
 * exists to catch.
 */
#include "events.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		g_failures++;
	}
}

static uint64_t h(const char *body, const char *volatile_fields)
{
	return events_hash_body(body, strlen(body), volatile_fields);
}

int main(void)
{
	uint64_t gen;
	struct events_check_counts c;

	printf("test_events_drift\n");
	events_init();

	/* Hashing: a volatile field is left out, at any depth, and only it. */
	check(h("{\"a\":1,\"expires_in_seconds\":5}", "expires_in_seconds") ==
	              h("{\"a\":1,\"expires_in_seconds\":9}", "expires_in_seconds"),
	      "a volatile field does not change the hash");
	check(h("{\"a\":1,\"expires_in_seconds\":5}", "") !=
	              h("{\"a\":1,\"expires_in_seconds\":9}", ""),
	      "the same field, not declared volatile, does");
	check(h("{\"sessions\":[{\"u\":\"x\",\"expires_in_seconds\":5}]}", "expires_in_seconds") ==
	              h("{\"sessions\":[{\"u\":\"x\",\"expires_in_seconds\":7}]}",
	                "expires_in_seconds"),
	      "a volatile field inside an array of objects is left out too");
	check(h("{\"a\":1,\"expires_in_seconds\":5}", "expires_in_seconds") !=
	              h("{\"a\":2,\"expires_in_seconds\":5}", "expires_in_seconds"),
	      "a real change next to a volatile field still changes the hash");
	check(h("{\"a\":\"1\"}", "") != h("{\"a\":1}", ""), "a string and a number differ");
	check(h("not json", "") != h("not json!", ""), "a body that is not JSON is hashed as bytes");

	/* A state read: the first observation is a baseline; a change with an
	 * announcement is fine; a change without one is drift. */
	check(events_observe(EV_getSystemSite, 100) == 0, "a first observation is never drift");
	check(events_observe(EV_getSystemSite, 100) == 0, "an unchanged read is not drift");
	events_announce(EV_getSystemSite);
	check(events_observe(EV_getSystemSite, 200) == 0, "an announced change is not drift");
	check(events_observe(EV_getSystemSite, 300) == 1,
	      "a change with no announcement IS drift -- the case the check exists for");
	check(events_observe(EV_getSystemSite, 300) == 0, "drift is reported once, not every pass");

	/* A sample: observing it change is what announces it. */
	gen = events_generation(EV_getHealth);
	check(events_observe(EV_getHealth, 1) == 0, "a sample's first observation is a baseline");
	check(events_generation(EV_getHealth) == gen, "a baseline announces nothing");
	check(events_observe(EV_getHealth, 2) == 0, "a sample changing is not drift");
	check(events_generation(EV_getHealth) == gen + 1, "a sample changing is announced, once");
	check(events_observe(EV_getHealth, 2) == 0 && events_generation(EV_getHealth) == gen + 1,
	      "a sample that did not change is not announced");

	c = events_check_counts();
	check(c.checked == 4 && c.drift == 1, "the counts are what was observed: 4 compared, 1 drift");

	if (g_failures != 0) {
		printf("FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
