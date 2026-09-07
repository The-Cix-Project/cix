/*
 * Recipe hygiene: the artifact-tier comment must name the artifact the
 * recipe actually fetches.
 *
 * Every recipe with a pkg_artifact_sha256 carries an explanatory block:
 *
 *   # With it set, an install fetches <artifact base_url>/zlib-1.3.2-9.tar.gz
 *
 * The filename is hardcoded, and it is copied forward verbatim when a
 * revision is bumped. Nobody updates it, so it drifts: 113 recipe
 * revisions name a file that is not theirs, bison@3.8.2-7 still saying
 * bison-3.8.2-2.tar.gz five revisions later (#226).
 *
 * Nothing breaks -- the checksum, which is the load-bearing part, was
 * always right. What breaks is the reader, who is told the wrong thing
 * by a comment that exists only to explain.
 *
 * Those 113 CANNOT be fixed. A published recipe version is immutable,
 * and pkg_recipe_add() permits exactly one edit to one: adding an
 * absent pkg_artifact_sha256 (recipe_adds_only_artifact_sha256()).
 * Correcting a comment in the same change was refused with HTTP 409,
 * which is the rule working. So this is a guard, not a cleanup.
 *
 * It scans only each package's LATEST revision, which is what makes the
 * number able to fall: a package fixes its comment when it next bumps,
 * and the count comes down with it. The going-forward convention is to
 * stop hardcoding the name at all --
 *
 *   # With it set, an install fetches <artifact base_url>/<name>-<version>.tar.gz
 *
 * -- which is accepted here as correct, cannot go stale, and costs the
 * reader nothing they could not already derive.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FETCH_PREFIX "# With it set, an install fetches <artifact base_url>/"
#define GENERIC_FORM "<name>-<version>.tar.gz"

/*
 * Packages whose latest revision names the wrong artifact, as of #226.
 *
 * GRANDFATHERED, not accepted: each is a published revision that cannot
 * be edited. Every one of these should disappear from this list the
 * next time its package bumps a revision -- fix the line to
 * GENERIC_FORM in the new revision and delete the name here.
 */
static const char *const g_stale_comment[] = {
	"bash", "bc", "binutils", "bison", "bzip2", "chrony", "coreutils", "curl", "dhcpcd",
	"diffutils", "dnsmasq", "findutils", "flex", "gawk", "gcc", "grep", "gzip",
	"libcap", "linux-pam", "make", "mtr", "nss-pam-ldapd", "openldap-client",
	"openssh", "patch", "perl", "pkgconf", "psmisc", "screen", "sed",
	"squashfs-tools", "sysklogd", "tar", "vim", "xz", "zlib",
};
#define STALE_COUNT ((int)(sizeof(g_stale_comment) / sizeof(g_stale_comment[0])))

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

static int known_stale(const char *name)
{
	int i;

	for (i = 0; i < STALE_COUNT; i++) {
		if (strcmp(g_stale_comment[i], name) == 0)
			return 1;
	}
	return 0;
}

/* Same "latest revision" rule as test_toolchain_policy and the daemon:
 * digit runs numerically, the rest lexically. */
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
 * 0 if the comment is absent or correct, 1 if it names another file.
 * out_named receives what it actually said, for the message.
 */
static int comment_is_stale(const char *path, const char *name, const char *version,
                            char *out_named, size_t out_size)
{
	FILE *f = fopen(path, "r");
	char line[4096];
	char want[512];
	int stale = 0;

	if (f == NULL)
		return 0;
	snprintf(want, sizeof(want), "%s-%s.tar.gz", name, version);
	while (fgets(line, sizeof(line), f) != NULL) {
		char *named;
		size_t len;

		if (strncmp(line, FETCH_PREFIX, strlen(FETCH_PREFIX)) != 0)
			continue;
		named = line + strlen(FETCH_PREFIX);
		len = strlen(named);
		while (len > 0 && (named[len - 1] == '\n' || named[len - 1] == '\r' ||
		                   named[len - 1] == ' '))
			named[--len] = '\0';
		/* The convention that cannot go stale. */
		if (strcmp(named, GENERIC_FORM) == 0)
			break;
		if (strcmp(named, want) != 0) {
			snprintf(out_named, out_size, "%s", named);
			stale = 1;
		}
		break;
	}
	fclose(f);
	return stale;
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
		char pkgdir[1024], latest[256] = "", path[2048], named[512] = "";
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
		if (!comment_is_stale(path, ent->d_name, latest, named, sizeof(named)))
			continue;
		found++;
		if (!known_stale(ent->d_name)) {
			fail("%s@%s's artifact comment names %s.\n"
			     "       It fetches %s-%s.tar.gz. The filename was copied forward from an\n"
			     "       earlier revision and not updated (#226). Write it as\n"
			     "         " FETCH_PREFIX GENERIC_FORM "\n"
			     "       which cannot go stale, rather than hardcoding this revision's name.",
			     ent->d_name, latest, named, ent->d_name, latest);
		}
	}
	closedir(d);

	if (found != STALE_COUNT)
		fail("found %d latest revisions with a wrong artifact comment, this test expects %d.\n"
		     "       If one was FIXED -- which is the direction this list is supposed to\n"
		     "       move -- remove it from g_stale_comment[] and lower the count with it.",
		     found, STALE_COUNT);

	if (g_failures > 0) {
		fprintf(stderr, "RECIPE HYGIENE: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("RECIPE HYGIENE: ok (%d grandfathered stale comments, none new)\n", found);
	return 0;
}
