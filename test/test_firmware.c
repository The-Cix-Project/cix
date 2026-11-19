/*
 * ADR-0381: the firmware image's stable link and the kernel's firmware
 * path, against a scratch directory and a plain file standing in for
 * the kernel parameter. Nothing here touches /sys.
 */
#include "firmware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, ...)                                                                      \
	do {                                                                                  \
		if (!(cond)) {                                                                \
			fprintf(stderr, "FAIL: " __VA_ARGS__);                                \
			fprintf(stderr, "\n");                                                \
			g_failures++;                                                         \
		}                                                                             \
	} while (0)

static void read_file(const char *path, char *out, size_t size)
{
	FILE *f = fopen(path, "r");
	size_t n = 0;

	out[0] = '\0';
	if (f != NULL) {
		n = fread(out, 1, size - 1, f);
		fclose(f);
	}
	out[n] = '\0';
}

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (f != NULL) {
		fputs(text, f);
		fclose(f);
	}
}

int main(void)
{
	char dir[] = "/tmp/cix_test_firmware_XXXXXX";
	char link[512], empty[512], v1[512], v2[512], param[512], got[1024], err[512];
	char longpath[FIRMWARE_PATH_MAX + 2];
	struct stat st;
	struct stat before, after;

	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(link, sizeof(link), "%s/firmware", dir);
	snprintf(empty, sizeof(empty), "%s/firmware.none", dir);
	snprintf(v1, sizeof(v1), "%s/v1/lib/firmware", dir);
	snprintf(v2, sizeof(v2), "%s/v2/lib/firmware", dir);
	snprintf(param, sizeof(param), "%s/param", dir);

	/* No version yet: the link names an empty directory it creates. */
	CHECK(firmware_link_set(link, NULL, empty, err, sizeof(err)) == 0, "link to empty: %s", err);
	firmware_link_target(link, got, sizeof(got));
	CHECK(strcmp(got, empty) == 0, "the link names the empty directory, got %s", got);
	CHECK(stat(empty, &st) == 0 && S_ISDIR(st.st_mode), "the empty directory exists");

	/* A version: the link moves to it, atomically, and a repeat writes
	 * nothing (the link's inode is unchanged). */
	CHECK(firmware_link_set(link, v1, empty, err, sizeof(err)) == 0, "link to v1: %s", err);
	firmware_link_target(link, got, sizeof(got));
	CHECK(strcmp(got, v1) == 0, "the link names v1, got %s", got);
	CHECK(lstat(link, &before) == 0, "lstat the link");
	CHECK(firmware_link_set(link, v1, empty, err, sizeof(err)) == 0, "link to v1 again: %s", err);
	CHECK(lstat(link, &after) == 0 && after.st_ino == before.st_ino,
	      "setting the same target again replaces nothing");
	CHECK(firmware_link_set(link, v2, empty, err, sizeof(err)) == 0, "link to v2: %s", err);
	firmware_link_target(link, got, sizeof(got));
	CHECK(strcmp(got, v2) == 0, "the link names v2, got %s", got);
	snprintf(got, sizeof(got), "%s.new", link);
	CHECK(access(got, F_OK) != 0, "no temporary link is left behind");

	/* A real directory where the link goes is not ours to replace. */
	unlink(link);
	CHECK(mkdir(link, 0755) == 0, "make a directory at the link path");
	CHECK(firmware_link_set(link, v1, empty, err, sizeof(err)) != 0 &&
	          strstr(err, "not a symlink") != NULL,
	      "a directory at the link path is refused, got %s", err);
	rmdir(link);

	/* The parameter: written with no newline, and not rewritten when it
	 * already reads the path (the kernel's own read adds a newline). */
	write_file(param, "");
	CHECK(firmware_path_apply(param, link, err, sizeof(err)) == 0, "apply: %s", err);
	read_file(param, got, sizeof(got));
	CHECK(strcmp(got, link) == 0, "the parameter holds the exact path, got \"%s\"", got);
	snprintf(got, sizeof(got), "%s\n", link);
	write_file(param, got);
	CHECK(stat(param, &before) == 0, "stat the parameter");
	CHECK(firmware_path_apply(param, link, err, sizeof(err)) == 0, "apply again: %s", err);
	read_file(param, got, sizeof(got));
	CHECK(got[strlen(got) - 1] == '\n',
	      "a parameter already reading the path (with the kernel's newline) is left as it is");

	/* A path the kernel would truncate is refused, never truncated. */
	memset(longpath, 'a', sizeof(longpath) - 1);
	longpath[0] = '/';
	longpath[sizeof(longpath) - 1] = '\0';
	write_file(param, "before");
	CHECK(firmware_path_apply(param, longpath, err, sizeof(err)) != 0 &&
	          strstr(err, "refused") != NULL,
	      "a %zu-byte path is refused, got %s", strlen(longpath), err);
	read_file(param, got, sizeof(got));
	CHECK(strcmp(got, "before") == 0, "a refused path writes nothing");
	CHECK(firmware_path_apply(param, "", err, sizeof(err)) != 0, "an empty path is refused");

	unlink(param);
	unlink(link);
	rmdir(empty);
	rmdir(dir);
	if (g_failures != 0)
		return 1;
	printf("test_firmware: pass\n");
	return 0;
}
