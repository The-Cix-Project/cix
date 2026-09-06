/*
 * ADR-0253: a build output tree is created fresh, never inherited.
 *
 * Three real incidents came from three different build output trees
 * being reused across builds, and each looked like a different bug:
 *
 *   <artifacts>/cix   mkbootroot reads <artifacts>/cix/web, no cix
 *                     recipe has ever staged it, and every assembly
 *                     worked anyway on a web/ left by an older build.
 *                     A reinstall swept the leftovers away and
 *                     assembly began failing at once.
 *
 *   ISO stage_dir     `cp -a src dst` treats an existing directory as
 *                     "copy INTO here", so last build's payload/seed
 *                     became this build's parent -- payload/seed/.seed.
 *                     The ISO shipped the seed twice and cix-install
 *                     copied the OUTER, STALE one (#307).
 *
 *   bootroot root     a control-plane root carrying glibc objects from
 *                     two different builds panics pid 1 at boot. Two
 *                     resets of a real host.
 *
 * persist_fresh_output_dir() is the one operation all three now use.
 * This asserts the property they each depend on, including the cases
 * that are easy to get wrong: a path that does not exist yet, and a
 * path occupied by something that is not a directory.
 */
#include "persist.h"

#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

static void fail(const char *what)
{
	fprintf(stderr, "  FAIL: %s\n", what);
	g_failures++;
}

static void wr(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (f == NULL) {
		fail("could not create a fixture file");
		return;
	}
	fputs(text, f);
	fclose(f);
}

/* Entries other than . and .. */
static int count_entries(const char *dir)
{
	struct dirent *de;
	DIR *d = opendir(dir);
	int n = 0;

	if (d == NULL)
		return -1;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		n++;
	}
	closedir(d);
	return n;
}

static int is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(void)
{
	char root[] = "/tmp/cix_freshdir_XXXXXX";
	char target[512], sub[512], deep[512], file[512], link[512];

	if (mkdtemp(root) == NULL) {
		fprintf(stderr, "FRESH OUTPUT DIR: mkdtemp: %s\n", strerror(errno));
		return 1;
	}

	/* Case 1: an inherited tree, of the shape that actually caused the bugs. */
	snprintf(target, sizeof(target), "%s/stage", root);
	snprintf(sub, sizeof(sub), "%s/payload", target);
	snprintf(deep, sizeof(deep), "%s/payload/seed", target);
	if (persist_mkdir_p(deep) != 0)
		fail("could not build the fixture tree");
	snprintf(file, sizeof(file), "%s/payload/seed/stale-artifact.tar.gz", target);
	wr(file, "a 56 MiB artifact, four days stale\n");
	snprintf(file, sizeof(file), "%s/web", target);
	wr(file, "a web/ no recipe ever staged\n");
	snprintf(link, sizeof(link), "%s/dangling", target);
	if (symlink("/nonexistent", link) != 0)
		fail("could not create the fixture symlink");

	if (count_entries(target) <= 0)
		fail("fixture tree was not populated -- the test would pass vacuously");

	if (persist_fresh_output_dir(target) != 0)
		fail("persist_fresh_output_dir failed on an existing tree");
	if (!is_dir(target))
		fail("the output directory does not exist afterwards");
	if (count_entries(target) != 0)
		fail("the output directory still holds inherited content");

	/* Case 2: nothing there yet. Fresh means empty, and never-existed is empty. */
	snprintf(target, sizeof(target), "%s/never-existed", root);
	if (persist_fresh_output_dir(target) != 0)
		fail("persist_fresh_output_dir failed when the path did not exist");
	if (!is_dir(target) || count_entries(target) != 0)
		fail("a path that did not exist was not created empty");

	/* Case 3: occupied by a regular file -- must become a directory. */
	snprintf(target, sizeof(target), "%s/was-a-file", root);
	wr(target, "not a directory\n");
	if (persist_fresh_output_dir(target) != 0)
		fail("persist_fresh_output_dir failed when the path was a regular file");
	if (!is_dir(target))
		fail("a path occupied by a file did not become a directory");

	/* Case 4: idempotent -- running it twice leaves it empty, not missing. */
	if (persist_fresh_output_dir(target) != 0 || !is_dir(target) || count_entries(target) != 0)
		fail("a second call did not leave an empty directory");

	if (g_failures > 0) {
		fprintf(stderr, "FRESH OUTPUT DIR: FAIL (%d) -- tree kept at %s\n", g_failures, root);
		return 1;
	}
	printf("FRESH OUTPUT DIR: ok (inherited tree, absent path, file in the way, idempotent)\n");
	return 0;
}
