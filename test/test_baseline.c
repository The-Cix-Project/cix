/*
 * test_baseline -- the image baseline's generation really moves when
 * the baseline does (ADR-0332, #579).
 *
 * WHAT THIS GATES. An image version is a hash of its installed packages
 * AND of CIX_BASELINE_GENERATION, so a baseline change reaches existing
 * images only if that number moves. The number is hand-maintained on
 * purpose -- the same device ADR-0224 uses for the gcc recipe count and
 * test_apigen for the operation count: a value that has to change in a
 * diff is how the change stays visible. The one failure mode is
 * forgetting, and forgetting is silent: the baseline gains a node, every
 * version string stays identical, dedup repoints at the old tree, and
 * the new node reaches newly created images only. That is exactly how
 * #577 came to be closed with its own symptom still reproducing, which
 * is why this test exists rather than a comment asking people to
 * remember.
 *
 * HOW THE COUPLING WORKS, because a simpler pin does not hold. Pinning
 * the device list alone would pass for anyone who edited the list and
 * the pin together and left the generation behind -- the two edits sit
 * in the same diff and read as one change. So the pin is keyed BY
 * generation: each row below records what one generation's baseline
 * staged, and the test asserts the row for today's generation matches
 * what the header expands to right now. Then:
 *
 *   - change the list, leave the generation: the current row no longer
 *     matches, and this fails naming both strings.
 *   - bump the generation, add no row: there is no row for it, and this
 *     fails saying to add one.
 *   - do both: passes, and the table has gained a permanent record of
 *     what that generation contained.
 *
 * The table is append-only for the same reason the ADRs are: an old
 * row describes a tree that still exists on disk somewhere, and
 * rewriting it would be rewriting history rather than recording it.
 *
 * WHAT THIS DOES NOT GATE, stated because the gap is worth knowing.
 * Only the DEVICE NODES are pinned here. pkg_seed_image_baseline()
 * also stages the loader and libc, /etc/nsswitch.conf, a minimal
 * passwd/group and the ptmx symlink, and a change to any of those
 * needs the generation bumped too with nothing here to catch it. They
 * are not pinned because they are host-sourced paths and content, not
 * a table this header can expand -- so pinning them would mean
 * duplicating pkg.c's own staging list, which would drift. The device
 * list is pinned because it IS a table, and it is the half that has
 * actually moved twice (#577, #578).
 *
 * Pure preprocessor and string work: no container, no daemon, no
 * filesystem. In SELFTESTS, deliberately -- a gate in a test a build
 * container cannot run is a gate that never runs (#224), and
 * test_devices, where this would otherwise belong, creates real
 * containers and is excluded.
 */
#include "container.h"

#include <stdio.h>
#include <string.h>

/*
 * One generation's device list, canonically spelled "name:major:minor,"
 * per node in the header's own order. Order is part of the string
 * deliberately: pkg_seed_image_baseline() creates the nodes in this
 * order, so a reordering is a real change to what runs even though the
 * set is the same, and it should be visible here.
 *
 * APPEND ONLY. To change the baseline: add a row, bump
 * CIX_BASELINE_GENERATION in include/container.h to match it, and leave
 * every earlier row alone.
 */
static const struct {
	int generation;
	const char *devices;
} baseline_generations[] = {
	/*
	 * Generation 1 -- the first to be identified at all (ADR-0332).
	 * Five nodes from the original baseline plus tty 5:0, which #577
	 * added and #578 shipped; everything before this release was
	 * unversioned, so 1 starts the count rather than numbering the
	 * past.
	 */
	{ 1, "null:1:3,zero:1:5,full:1:7,random:1:8,urandom:1:9,tty:5:0," },
};

/* The header's list, built into the same canonical spelling. */
static int build_current(char *out, size_t out_size)
{
	size_t pos = 0;

#define CIX_BASELINE_TEST_ENTRY_(dname, maj, min)                                                  \
	{                                                                                          \
		int n = snprintf(out + pos, out_size - pos, "%s:%u:%u,", dname, (unsigned int)(maj),   \
		                  (unsigned int)(min));                                                \
                                                                                                   \
		if (n < 0 || (size_t)n >= out_size - pos)                                              \
			return -1;                                                                     \
		pos += (size_t)n;                                                                      \
	}
	out[0] = '\0';
	CIX_BASELINE_DEVICES(CIX_BASELINE_TEST_ENTRY_)
#undef CIX_BASELINE_TEST_ENTRY_
	return 0;
}

int main(void)
{
	char current[1024];
	const char *pinned = NULL;
	size_t i;
	size_t rows = sizeof(baseline_generations) / sizeof(baseline_generations[0]);
	int ok = 1;

	if (build_current(current, sizeof(current)) != 0) {
		fprintf(stderr, "FAIL: the baseline device list does not fit in %zu bytes -- grow the "
		                 "buffer in this test\n",
		         sizeof(current));
		return 1;
	}

	/*
	 * A generation is a count, so it starts at 1. Zero is reserved: it
	 * is what a version staged before the field existed reports, and
	 * image_current_baseline_generation() reads it as "older than
	 * generation 1" to re-produce such a tree exactly once.
	 */
	if (CIX_BASELINE_GENERATION < 1) {
		fprintf(stderr, "FAIL: CIX_BASELINE_GENERATION is %d -- 0 means \"before this field "
		                 "existed\" and is not a generation an image can be staged under\n",
		         CIX_BASELINE_GENERATION);
		ok = 0;
	}

	/* Rows ascend, with no gaps and no repeats: the table is a history,
	 * and one that skipped or repeated a number would not be readable
	 * as one. */
	for (i = 0; i < rows; i++) {
		if (baseline_generations[i].generation != (int)i + 1) {
			fprintf(stderr,
			         "FAIL: baseline_generations[%zu] is generation %d, expected %zu -- the "
			         "table must list every generation from 1 upward, in order\n",
			         i, baseline_generations[i].generation, i + 1);
			ok = 0;
		}
		if (baseline_generations[i].generation == CIX_BASELINE_GENERATION)
			pinned = baseline_generations[i].devices;
	}

	if (pinned == NULL) {
		fprintf(stderr,
		         "FAIL: CIX_BASELINE_GENERATION is %d and this table has no row for it.\n"
		         "      The generation was bumped without recording what it stages. Add:\n"
		         "          { %d, \"%s\" },\n"
		         "      to baseline_generations[] in this file.\n",
		         CIX_BASELINE_GENERATION, CIX_BASELINE_GENERATION, current);
		printf("BASELINE RESULT: FAIL\n");
		return 1;
	}

	if (strcmp(pinned, current) != 0) {
		fprintf(stderr,
		         "FAIL: the baseline device list changed but CIX_BASELINE_GENERATION did not.\n"
		         "      generation %d pins: %s\n"
		         "      container.h now has: %s\n"
		         "\n"
		         "      An image version is a hash of its packages AND this generation\n"
		         "      (ADR-0332), so a list change that leaves the number alone reaches\n"
		         "      newly created images only -- every existing image keeps the tree it\n"
		         "      has, silently. That is #577: tty 5:0 shipped and jumpbox still could\n"
		         "      not run sudo.\n"
		         "\n"
		         "      Bump CIX_BASELINE_GENERATION to %d in include/container.h and add:\n"
		         "          { %d, \"%s\" },\n"
		         "      to baseline_generations[] in this file.\n",
		         CIX_BASELINE_GENERATION, pinned, current, CIX_BASELINE_GENERATION + 1,
		         CIX_BASELINE_GENERATION + 1, current);
		ok = 0;
	}

	printf(ok ? "BASELINE RESULT: PASS\n" : "BASELINE RESULT: FAIL\n");
	return ok ? 0 : 1;
}
