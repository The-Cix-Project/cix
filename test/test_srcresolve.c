/*
 * ADR-0255's source catalogue: given the policy in force, has upstream
 * published a release this platform has no recipe for?
 *
 * The property worth guarding is that the verdict is "do we HAVE it",
 * not "is upstream NEWER". Those come apart, and the case where they do
 * is live on this platform right now: recipes exist for both 6.18.40-24
 * and 7.2.3-2, so a longterm policy resolving to 6.18.46 needs a recipe
 * written even though the highest recipe on disk (7.2.3) is numerically
 * greater. A "newer available" boolean says no. The test below asserts
 * `missing` for exactly that shape, because it is the one a naive
 * comparison gets wrong.
 *
 * The other property is that a failure is a ROW WITH A REASON, never an
 * absence -- and that "never fetched" and "this channel publishes
 * nothing" stay distinguishable, since they need opposite responses
 * from an operator and both arrive as zero candidates.
 *
 * TEST DOUBLES: srcresolve.c reads recipes through four pkg.c entry
 * points. pkg.c does not link outside the full daemon, so they are
 * stubbed here. Only pkg_version_compare() is reached by the code under
 * test; the fixtures below are chosen so its ordering is unambiguous
 * (6.x before 7.x), and ADR-0107's real natural-sort ordering is
 * covered by the daemon's own package tests rather than asserted
 * through a double.
 */
#include "srcresolve.h"

#include "kernelpolicy.h"
#include "pkg.h"
#include "srcpolicy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int pkg_version_compare(const char *a, const char *b)
{
	return strcmp(a, b);
}

int pkg_recipe_list_names(char names[][PKG_IMAGE_NAME_MAX], int max)
{
	(void)names;
	(void)max;
	return 0;
}

int pkg_recipe_upstream(const char *name, char *out, size_t out_size)
{
	(void)name;
	(void)out;
	(void)out_size;
	return -1;
}

int pkg_recipe_list_versions(const char *name, char versions[][PKG_VERSION_MAX], int max)
{
	(void)name;
	(void)versions;
	(void)max;
	return 0;
}

static int g_failures;
static char g_pol_path[256];
static char g_kern_path[256];
static char g_rel_path[256];

static void fail(const char *fmt, const char *a, const char *b)
{
	fprintf(stderr, "  FAIL: ");
	fprintf(stderr, fmt, a, b);
	fprintf(stderr, "\n");
	g_failures++;
}

static void check_upstream_of(const char *in, const char *want)
{
	char got[64];

	srcresolve_upstream_of(in, got, sizeof(got));
	if (strcmp(got, want) != 0)
		fail("upstream_of(%s) = \"%s\"", in, got);
	else
		printf("  upstream_of %-14s -> %s\n", in, got);
}

/* The two recipe versions this platform really has for the kernel. */
static const char *const KERNEL_RECIPES[] = { "6.18.40-24", "7.2.3-2" };

static void expect(const char *label, const char *name, const char *kind,
                    const char *const *recipes, size_t recipe_count,
                    enum srcresolve_state want_state, const char *want_version,
                    const char *must_mention)
{
	struct srcresolve_entry e;

	srcresolve_one(name, kind, recipes, recipe_count, &e);
	if (e.state != want_state) {
		fail("%s: state is \"%s\"", label, srcresolve_state_name(e.state));
		fprintf(stderr, "         reason: %s\n", e.reason);
		return;
	}
	if (want_version != NULL && strcmp(e.resolved_version, want_version) != 0) {
		fail("%s: resolved \"%s\"", label, e.resolved_version);
		return;
	}
	if (e.reason[0] == '\0') {
		fail("%s: no reason given%s", label, "");
		return;
	}
	if (must_mention != NULL && strstr(e.reason, must_mention) == NULL) {
		fail("%s: reason did not mention \"%s\"", label, must_mention);
		fprintf(stderr, "         reason: %s\n", e.reason);
		return;
	}
	printf("  %-26s %-10s resolved=%-9s newest_recipe=%-11s\n", label,
	        srcresolve_state_name(e.state),
	        e.resolved_version[0] != '\0' ? e.resolved_version : "-",
	        e.newest_recipe_version[0] != '\0' ? e.newest_recipe_version : "-");
	return;
}

static int write_releases(void)
{
	FILE *f = fopen(g_rel_path, "w");

	if (f == NULL)
		return -1;
	/*
	 * kernel.org's real shape: the newest release of each line and
	 * nothing else. Three longterm lines, one stable, one mainline --
	 * which is precisely why a depth reaching back WITHIN a line
	 * cannot resolve against this feed.
	 */
	fprintf(f,
	        "{\"releases\":["
	        "{\"moniker\":\"mainline\",\"version\":\"7.3\",\"source\":\"https://k/7.3.tar.xz\"},"
	        "{\"moniker\":\"stable\",\"version\":\"7.2.3\",\"source\":\"https://k/7.2.3.tar.xz\"},"
	        "{\"moniker\":\"longterm\",\"version\":\"6.18.46\",\"source\":\"https://k/a.tar.xz\"},"
	        "{\"moniker\":\"longterm\",\"version\":\"6.12.60\",\"source\":\"https://k/b.tar.xz\"},"
	        "{\"moniker\":\"longterm\",\"version\":\"6.6.120\",\"source\":\"https://k/c.tar.xz\"}"
	        "]}\n");
	fclose(f);
	return 0;
}

int main(void)
{
	char err[256];

	snprintf(g_pol_path, sizeof(g_pol_path), "/tmp/cix_srcresolve_pol_%d.json", (int)getpid());
	snprintf(g_kern_path, sizeof(g_kern_path), "/tmp/cix_srcresolve_kern_%d.json", (int)getpid());
	snprintf(g_rel_path, sizeof(g_rel_path), "/tmp/cix_srcresolve_rel_%d.json", (int)getpid());
	unlink(g_pol_path);
	unlink(g_kern_path);
	printf("test_srcresolve\n");

	if (srcpolicy_init(g_pol_path) != 0 || kernelpolicy_init(g_kern_path) != 0) {
		fprintf(stderr, "  FAIL: init\n");
		return 1;
	}

	check_upstream_of("7.2.3-2", "7.2.3");
	check_upstream_of("6.18.40-24", "6.18.40");
	check_upstream_of("1.0.8", "1.0.8");
	check_upstream_of("v2.53.82", "v2.53.82");
	/* A trailing component that is not all digits belongs to upstream. */
	check_upstream_of("1.4.20-rc1", "1.4.20-rc1");

	/* A package declaring no upstream is pinned -- an answer, not a gap. */
	expect("pinned (no upstream)", "bash", "", KERNEL_RECIPES, 0, SRCRESOLVE_PINNED, NULL,
	        "pinned");

	/* A kind nothing implements says so, naming the kind. */
	expect("unknown kind", "somepkg", "sourceforge", NULL, 0, SRCRESOLVE_UNRESOLVED, NULL,
	        "sourceforge");

	if (srcpolicy_default_set("stable", "n", err, sizeof(err)) != 0) {
		fprintf(stderr, "  FAIL: default_set: %s\n", err);
		return 1;
	}

	/*
	 * BEFORE any fetch. Zero candidates because nothing has ever been
	 * downloaded -- which must not read as "stable publishes nothing".
	 */
	expect("never fetched", "kernel", "kernel.org", KERNEL_RECIPES, 2, SRCRESOLVE_UNRESOLVED,
	        NULL, "never been fetched");

	if (write_releases() != 0 || kernelpolicy_ingest_releases(g_rel_path, 1757000000L) != 0) {
		fprintf(stderr, "  FAIL: could not ingest release fixture\n");
		return 1;
	}

	/* stable/n resolves 7.2.3, and recipe 7.2.3-2 builds it. */
	expect("stable n", "kernel", "kernel.org", KERNEL_RECIPES, 2, SRCRESOLVE_CURRENT, "7.2.3",
	        "7.2.3-2");

	/*
	 * THE CASE A "NEWER AVAILABLE" BOOLEAN GETS WRONG. longterm/n
	 * resolves 6.18.46; the highest recipe on disk is 7.2.3-2, which is
	 * GREATER -- and a recipe for 6.18.46 still needs writing.
	 */
	if (srcpolicy_set("kernel", "kernel.org", "longterm", "n", err, sizeof(err)) != 0) {
		fprintf(stderr, "  FAIL: set longterm: %s\n", err);
		return 1;
	}
	expect("longterm n (recipe ahead)", "kernel", "kernel.org", KERNEL_RECIPES, 2,
	        SRCRESOLVE_MISSING, "6.18.46", "6.18.46");

	/* One release LINE back is a different question, and resolves. */
	if (srcpolicy_set("kernel", "kernel.org", "longterm", "n-1", err, sizeof(err)) != 0) {
		fprintf(stderr, "  FAIL: set longterm n-1: %s\n", err);
		return 1;
	}
	expect("longterm n-1", "kernel", "kernel.org", KERNEL_RECIPES, 2, SRCRESOLVE_MISSING,
	        "6.12.60", NULL);

	/*
	 * One RELEASE back within a line cannot resolve against this feed,
	 * and the reason has to name the feed's own limitation -- otherwise
	 * an operator reads "not found" and has nowhere to go.
	 */
	if (srcpolicy_set("kernel", "kernel.org", "stable", "n-0.1", err, sizeof(err)) != 0) {
		fprintf(stderr, "  FAIL: set stable n-0.1: %s\n", err);
		return 1;
	}
	expect("stable n-0.1 (feed limit)", "kernel", "kernel.org", KERNEL_RECIPES, 2,
	        SRCRESOLVE_UNRESOLVED, NULL, "newest release of each line");

	unlink(g_pol_path);
	unlink(g_kern_path);
	unlink(g_rel_path);
	if (g_failures > 0) {
		printf("test_srcresolve: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_srcresolve: all checks passed\n");
	return 0;
}
