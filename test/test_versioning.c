/*
 * test_versioning -- a Cix release is <version>-<release>, the version
 * is 0.2.x, and the release is the counter (ADR-0312).
 *
 * The scheme this replaced was never written down anywhere, which is
 * exactly how it drifted: `v2.57.358` had a major of 2 and a minor of
 * 57 that had climbed for years without either ever marking a
 * compatibility boundary, because Cix has never shipped a stable
 * interface to break. The owner's instruction, 2026-09-26: *"I want
 * this plastered on CLAUDE.md and our readme files and our build files
 * so that we don't mix things up ... I want to stop exaggerating with
 * versioning."*
 *
 * Plastering it on three documents is necessary and is not sufficient
 * -- the previous convention was in nobody's way and still rotted. So
 * it is also a gate, for the same reason ADR-0224 counts gcc recipes
 * rather than preferring TCC in words, and ADR-0309's test_naming
 * gates the CBS name rather than asking politely.
 *
 * TWO THINGS ARE CHECKED, and the second is the load-bearing one.
 *
 *   1. The build version has the right SHAPE: `0.2.57-358`, no `v`
 *      prefix, leading component 0.
 *
 *   2. No `cix@v2.57.*` recipe is left in the corpus. This is not
 *      tidiness. pkg_version_compare() is dpkg-style natural sort, so
 *      `0.2.57` loses to `v2.57.358` on the first character ('0' is a
 *      digit, 'v' is not, so bytes decide and 0x30 < 0x76). And
 *      find_recipe_path()'s own comment says the highest-ordered
 *      version is "the 'rolling implicit' default every pre-existing,
 *      non-manifest-aware caller (plain `pkg install NAME`, dependency
 *      resolution, hostbuild, update-candidate checks) relies on."
 *      Put one back and a version-less `pkg install cix` silently
 *      resolves to the retired line, forever.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "version.h" /* CIX_BUILD_VERSION */

static int g_failures;

static void fail(const char *msg)
{
	printf("  %s\n", msg);
	g_failures++;
}

/* Reads a run of digits, advancing *p. Returns how many it read. */
static int digits(const char **p)
{
	int n = 0;

	while (**p >= '0' && **p <= '9') {
		(*p)++;
		n++;
	}
	return n;
}

/*
 * `0.2.57-358`, optionally followed by what `git describe` appends in
 * a working tree that is ahead of the tag or dirty (`-2-gabc1234`,
 * `-dirty`). The suffix is tolerated deliberately: a dev tree between
 * releases is a normal state and failing there would make the gate
 * something to switch off rather than something to trust. What is
 * never tolerated is the leading shape.
 */
static void check_shape(const char *v)
{
	const char *p = v;

	if (v[0] == 'v' || v[0] == 'V') {
		fail("build version starts with a 'v' -- ADR-0312 is one string everywhere,\n"
		     "  and the tag, CIX_BUILD_VERSION, the recipe identity and the artifact\n"
		     "  name are all spelled the same way: 0.2.57-358, no prefix");
		return;
	}
	if (v[0] != '0' || v[1] != '.') {
		fail("build version does not start \"0.\" -- Cix is pre-1.0 and says so\n"
		     "  (ADR-0312). A major above 0 is a claim of a stable interface that\n"
		     "  does not exist yet; when one does, that is a deliberate change to\n"
		     "  this gate and to the three documents it backs up");
		return;
	}

	/* 0 . N . N - N */
	if (digits(&p) == 0 || *p++ != '.')
		goto malformed;
	if (digits(&p) == 0 || *p++ != '.')
		goto malformed;
	if (digits(&p) == 0 || *p++ != '-')
		goto malformed;
	if (digits(&p) == 0)
		goto malformed;
	/* Anything further must be a git-describe suffix, which always
	 * begins with '-'. A trailing '.' or digit here would mean a
	 * fourth component crept back in. */
	if (*p != '\0' && *p != '-')
		goto malformed;
	return;

malformed:
	fail("build version is not <major>.<minor>.<patch>-<release> (ADR-0312).\n"
	     "  The release is the counter: 0.2.57-358, then 0.2.57-359. It is a\n"
	     "  real CPDL `release` field, the same one all 477 recipes carry --\n"
	     "  not a fourth dotted component");
}

/*
 * The corpus lives in its own repository (ADR-0308) and a clean
 * checkout of this one does not have it, so an absent corpus is
 * skipped with a printed line rather than failed -- the same rule
 * test_naming uses, and for the same reason: a build container can
 * reach neither the network nor the host filesystem, so failing on
 * absence would fail every release for something that is not a
 * regression.
 */
static void check_retired_line(void)
{
	const char *env = getenv("CIX_RECIPES_DIR");
	const char *root = (env != NULL && env[0] != '\0') ? env : "../cix-recipes/recipes";
	char cmd[1024];
	char line[1024];
	int found = 0;
	FILE *p;

	if (access(root, R_OK) != 0) {
		printf("  recipe corpus not present at %s -- the retired-line check did\n"
		       "  NOT run (set CIX_RECIPES_DIR, or clone cix-recipes beside this\n"
		       "  repository, to gate it)\n",
		       root);
		return;
	}

	/* Any cix recipe whose version is not the 0.x scheme. Matching on
	 * "not 0." rather than on "v2.57" deliberately: a future stray
	 * line would be just as harmful and would not be spelled v2.57. */
	snprintf(cmd, sizeof(cmd), "ls %s/package/ 2>/dev/null | grep '^cix@' | grep -v '^cix@0\\.'",
	         root);
	p = popen(cmd, "r");
	if (p == NULL) {
		fail("could not list the recipe corpus");
		return;
	}
	while (fgets(line, sizeof(line), p) != NULL) {
		if (!found)
			printf("  retired cix recipes still in the corpus:\n");
		printf("    %s", line);
		found++;
	}
	pclose(p);

	if (found != 0) {
		fail("a cix recipe outside the 0.x line is published (ADR-0312).\n"
		     "  pkg_version_compare() ranks it ABOVE every 0.2.x release, so the\n"
		     "  highest cix recipe would be the retired one -- and that is what\n"
		     "  plain `pkg install cix`, dependency resolution, hostbuild and every\n"
		     "  update check resolve to. Move it to trash/ and `pkg recipe rm` it");
	}
}

/*
 * A published artifact's name has ONE definition,
 * `pkg_artifact_published_name()` in daemon/src/pkg.c, and the way
 * this went wrong is the reason the check is shaped like this.
 *
 * The installer ISO used to build its own name in main.c with its own
 * format string and a hardcoded release. When #434 changed the
 * version string it was handed -- to the cix artifact version, which
 * already ends in a release -- that literal became a second release
 * number, and twenty releases published as
 * `cix-installer-0.2.57-359-1-x86_64.iso`: version `0.2.57-359`,
 * release `1`, two releases for one artifact.
 *
 * ASSERTING THE FORMAT STRING WOULD NOT HAVE CAUGHT IT, which is the
 * whole point. Both names were well-formed in isolation; they were
 * only wrong relative to each other, and neither file knew the other
 * existed. So what is gated is the COUNT: exactly one place composes
 * `<name>-<version>-<arch>`, and a second one appearing is the
 * regression, whatever it happens to spell.
 */
static void check_one_artifact_namer(void)
{
	char line[4096];
	int composers = 0;
	FILE *p;

	/*
	 * A composer is an snprintf whose literal ends in the arch-and-
	 * suffix tail this convention uses. pkg_artifact_published_name()
	 * is the one legitimate hit; anything else is a second namer.
	 */
	p = popen("grep -rn '\"%s-%s-%s%s\"' daemon/src/*.c 2>/dev/null", "r");
	if (p == NULL) {
		fail("could not scan for artifact-name composers");
		return;
	}
	while (fgets(line, sizeof(line), p) != NULL) {
		printf("  artifact-name composer: %s", line);
		composers++;
	}
	pclose(p);

	if (composers > 1) {
		fail("more than one place composes a published artifact name.\n"
		     "  There is exactly one definition -- pkg_artifact_published_name()\n"
		     "  in daemon/src/pkg.c -- and callers pass a suffix to it. A second\n"
		     "  one is how the installer ISO came to carry two release numbers\n"
		     "  for twenty releases (ADR-0312): both spellings were fine on their\n"
		     "  own and wrong relative to each other, so there was nothing a\n"
		     "  format-string assertion could have compared them against");
	} else if (composers == 0) {
		fail("no artifact-name composer found at all -- pkg_artifact_published_name()\n"
		     "  is expected to compose \"%s-%s-%s%s\". If it was deliberately\n"
		     "  rewritten, update this check to match the new one definition");
	}
}

int main(void)
{
	const char *v = CIX_BUILD_VERSION;

	printf("VERSIONING: build version is \"%s\"\n", v);

	if (strcmp(v, "unknown") == 0) {
		/* No git, no CIX_VERSION: nothing to check rather than a
		 * failure. The Makefile's own comment explains when this
		 * happens; a hostbuild always passes CIX_VERSION. */
		printf("  version is \"unknown\" -- the shape check did NOT run\n");
	} else {
		check_shape(v);
	}
	check_retired_line();
	check_one_artifact_namer();

	if (g_failures != 0) {
		printf("VERSIONING RESULT: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("VERSIONING RESULT: PASS\n");
	return 0;
}
