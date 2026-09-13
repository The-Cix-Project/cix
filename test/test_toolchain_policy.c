/*
 * ADR-0224, the Toolchain Tenet: TCC by right, gcc by evidence.
 *
 * The rule is that Cix's own code is always TCC -- no bar, no
 * exception -- and third-party packages are TCC by default and gcc
 * when TCC is difficult. ADR-0226 amended ADR-0224 here: the original
 * four-part bar and its required tracking issue NO LONGER APPLY to
 * third-party packages, because the owner's instruction is that moving
 * a package this project neither owns nor wrote should be the natural
 * change rather than a battle. What is still required is
 * pkg_toolchain= and pkg_toolchain_reason=, for auditability rather
 * than permission -- a reason may be as ordinary as "the language has
 * no TCC front end".
 *
 * The rule is not the interesting part -- an informal version of it
 * already existed, as prose in CLAUDE.md, and did not stop gcc
 * reaching 21 recipes without anyone counting.
 *
 * This is the part that makes it real: a NUMBER that changes visibly.
 * Adding a gcc package means editing the list below, in a diff,
 * deliberately. Since ADR-0226 this is a VISIBILITY device rather than
 * a debt ceiling -- the set is expected to GROW, and that is fine; what
 * must not happen is it growing without anyone seeing it -- the same device test_apigen uses to keep the REST
 * surface honest. A rule nobody can count is a rule that has already
 * been broken, and this project has the evidence: 34 of 139 installed
 * packages had drifted behind their recipes because nothing counted
 * them either (#217).
 *
 * Deliberately checks only what is mechanically true: whether a
 * recipe's pkg_build() invokes gcc. Whether the reason recorded beside
 * it is a GOOD reason is review, and a gate pretending to judge that
 * would be the kind that passes while the thing it names is wrong.
 *
 * Comments are excluded from the scan on purpose. Several recipes
 * discuss gcc at length -- why they do not use it, or what it did
 * differently -- and counting prose would make the number meaningless
 * within a week.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Every recipe whose latest revision builds with gcc, as of ADR-0224.
 *
 * These are GRANDFATHERED, not blessed: the ADR requires each to carry
 * pkg_toolchain= and a reason naming the compiler gap it waits on, and
 * that backfill is tracked separately. What this list does today is
 * stop the set GROWING unnoticed, which is the failure that mattered.
 */
static const char *const g_gcc_recipes[] = {
	"btop", "binutils", "binutils-dev", "bird", "btrfs-progs", "cmake", "efivar", "elfutils",
	"fastfetch", "gcc", "gitea", "glauth", "glibc", "gnu-efi", "go", "go-bootstrap",
	"grub", "kernel", "keyutils", "kmod", "libblkid", "libxcrypt",
	"linux-headers", "node", "perl", "probe-gcc-headers", "probe-gcc-postglibc",
	/*
	 * probe-wifi-driver names /usr/bin/gcc to ASK ABOUT it, not to
	 * build with it: revision 9 runs `gcc -E -Wp,-v` to print HOSTCC's
	 * own include search path, and revision 10 tests `[ -x
	 * /usr/bin/gcc ]` to confirm it is running in kernel-builder
	 * rather than a four-tool composed environment. The scan below is
	 * textual and cannot tell a compile from a question about the
	 * compiler.
	 *
	 * Listed rather than excluded, because that is what this file
	 * already does with probe-gcc-headers and probe-gcc-postglibc, and
	 * because the point of the list is that a name appearing here is
	 * visible in a diff. Teaching the scan to skip probe-* would make
	 * every future probe invisible to it, which is a worse trade than
	 * one honest line with a reason attached.
	 */
	"probe-wifi-driver", "python",
};
#define GCC_RECIPE_COUNT ((int)(sizeof(g_gcc_recipes) / sizeof(g_gcc_recipes[0])))

static int g_failures;

static void fail(const char *fmt, ...)
{
	va_list ap;

	fputs("FAIL: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	g_failures++;
}

static int known_gcc_recipe(const char *name)
{
	int i;

	for (i = 0; i < GCC_RECIPE_COUNT; i++) {
		if (strcmp(g_gcc_recipes[i], name) == 0)
			return 1;
	}
	return 0;
}

/* Highest version directory, the same "latest revision" rule the
 * daemon's own resolution uses: digit runs numerically, the rest
 * lexically. */
static int version_newer(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
			long na = 0, nb = 0;

			while (*a >= '0' && *a <= '9')
				na = na * 10 + (*a++ - '0');
			while (*b >= '0' && *b <= '9')
				nb = nb * 10 + (*b++ - '0');
			if (na != nb)
				return na > nb;
			continue;
		}
		if (*a != *b)
			return (unsigned char)*a > (unsigned char)*b;
		a++;
		b++;
	}
	return *b == '\0' && *a != '\0';
}

/*
 * Does this recipe DECLARE gcc?
 *
 * `pkg_toolchain="gcc"` is the authoritative answer and is checked
 * first, because the scan below can only see a compiler a recipe names
 * out loud -- and a recipe need not name one at all. `cmake` runs
 * `./bootstrap`, `fastfetch` runs `cmake`; both build with g++ or gcc,
 * both declare it with a measured reason, and neither writes a
 * compiler anywhere in pkg_build(). They were counted as TCC recipes
 * and broke the total, which failed every build's selftest while both
 * recipes were entirely correct.
 *
 * So the two signals answer different questions and both are kept. A
 * declaration is what a recipe says; the scan is what it does. Their
 * union is the set of gcc recipes, and the scan's real job is the case
 * the declaration cannot cover -- a recipe that reaches for gcc
 * WITHOUT declaring it, which is the undetected move ADR-0224 exists
 * to make impossible.
 *
 * Read before pkg_build(), where the metadata lives.
 */
static int recipe_declares_gcc(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[4096];
	int declares = 0;

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "pkg_build()", 11) == 0)
			break;
		/* 19, not 20: fgets() keeps the newline, so comparing the
		 * terminator too would never match. */
		if (strncmp(line, "pkg_toolchain=\"gcc\"", 19) == 0) {
			declares = 1;
			break;
		}
	}
	fclose(f);
	return declares;
}

/*
 * Does this recipe's pkg_build() actually invoke the gcc toolchain?
 *
 * Scans from pkg_build() onward, skipping comment lines. Anything
 * before pkg_build() is metadata and prose.
 *
 * g++ counts. It is the same toolchain wearing its C++ driver, and a
 * recipe that builds with it is exactly as much a gcc recipe as one
 * calling gcc -- which is the whole thing this count exists to keep
 * visible. Added when btop, the first C++ package here, declared
 * pkg_toolchain="gcc" and went undetected because it invokes CXX
 * rather than CC: an undetected gcc recipe is worse than a miscounted
 * one, since it means a package can move to gcc without the diff
 * anybody reviews ever showing it.
 */
static int recipe_uses_gcc(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[4096];
	int in_build = 0, uses = 0;

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p = line;

		if (!in_build) {
			if (strncmp(line, "pkg_build()", 11) == 0)
				in_build = 1;
			continue;
		}
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#')
			continue;
		if (strstr(p, "/usr/bin/gcc") != NULL || strstr(p, "CC=gcc") != NULL ||
		    strstr(p, "--cc=gcc") != NULL || strstr(p, "-Dcc=gcc") != NULL ||
		    strstr(p, "/usr/bin/g++") != NULL || strstr(p, "CXX=g++") != NULL) {
			uses = 1;
			break;
		}
	}
	fclose(f);
	return uses;
}

int main(void)
{
	DIR *d = opendir("recipes/package");
	struct dirent *ent;
	int found = 0;

	if (d == NULL) {
		fprintf(stderr, "FAIL: cannot open recipes/package -- run from the repository root\n");
		return 1;
	}

	while ((ent = readdir(d)) != NULL) {
		char pkgdir[1024], latest[256] = "", path[2048];
		DIR *vd;
		struct dirent *vent;

		if (ent->d_name[0] == '.')
			continue;
		snprintf(pkgdir, sizeof(pkgdir), "recipes/package/%s", ent->d_name);
		vd = opendir(pkgdir);
		if (vd == NULL)
			continue;
		while ((vent = readdir(vd)) != NULL) {
			struct stat st;

			if (vent->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "%s/%s/build.sh", pkgdir, vent->d_name);
			if (stat(path, &st) != 0)
				continue;
			if (latest[0] == '\0' || version_newer(vent->d_name, latest))
				snprintf(latest, sizeof(latest), "%s", vent->d_name);
		}
		closedir(vd);
		if (latest[0] == '\0')
			continue;

		snprintf(path, sizeof(path), "%s/%s/build.sh", pkgdir, latest);
		if (!recipe_declares_gcc(path) && !recipe_uses_gcc(path))
			continue;
		found++;
		if (!known_gcc_recipe(ent->d_name)) {
			fail("%s@%s builds with gcc and is not in this test's list.\n"
			     "       ADR-0226: gcc is an ordinary choice for a third-party package,\n"
			     "       so this is not a refusal -- it is a request to make the change\n"
			     "       visible. Record pkg_toolchain=\"gcc\" and a pkg_toolchain_reason\n"
			     "       naming what was measured, then add it to g_gcc_recipes[] here.\n"
			     "       That edit is the point: the set may grow, but never unnoticed.",
			     ent->d_name, latest);
		}
	}
	closedir(d);

	if (found != GCC_RECIPE_COUNT)
		fail("found %d recipes building with gcc, this test expects %d.\n"
		     "       If one was RETIRED back to TCC -- which is the direction this list\n"
		     "       is supposed to move -- remove it from g_gcc_recipes[] and lower the\n"
		     "       count with it.",
		     found, GCC_RECIPE_COUNT);

	if (g_failures > 0) {
		fprintf(stderr, "TOOLCHAIN POLICY: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("TOOLCHAIN POLICY: PASS (%d gcc exceptions, all declared)\n", found);
	return 0;
}
