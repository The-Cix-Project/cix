/*
 * test_controlplane_programs -- code running on the control-plane root
 * executes only programs include/controlplane_programs.h lists (#554).
 *
 * mkbootroot refuses to seal a root missing a listed program. This is
 * the other half: a program executed and not listed fails the release
 * here, before any root is assembled. It scans every source linked into
 * a binary that runs on that root -- cixd, the runtime library,
 * netplane, mkbootroot, mkinstalleriso and the fixture code they link --
 * for the two forms an executed path takes in this tree:
 *
 *   #define SOMETHING_BIN "/usr/sbin/sfdisk"
 *   execve("/bin/cixctl", ...)       (any exec* with a literal path)
 *
 * Every other exec in these sources passes a variable filled from one
 * of those, a path under a build artifact, or a command for inside a
 * container, none of which run off this root.
 *
 * Found by 0.2.57-414: test/test_image_fixture.c, linked into
 * mkbootroot, forked /usr/bin/cp, which 412 had removed from the root.
 * Runs from the repository root, as SELFTESTS do.
 */
#include "controlplane_programs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *const listed[] = {
#define CP_PATH_(id, path, source, pkg) path,
	CONTROLPLANE_PROGRAMS(CP_PATH_)
#undef CP_PATH_
};

static const char *const scan_dirs[] = {
	"daemon/src", "daemon/include", "src", "include", "netplane/src", "netplane/include",
};

static const char *const scan_files[] = {
	"image/src/mkbootroot.c", "image/src/mkinstalleriso.c", "test/test_image_fixture.c",
};

static int g_found; /* executed paths seen, the control against an empty scan */
static int g_bad;

static int is_listed(const char *path)
{
	size_t i;

	for (i = 0; i < sizeof(listed) / sizeof(listed[0]); i++) {
		if (strcmp(listed[i], path) == 0)
			return 1;
	}
	return 0;
}

/* The quoted absolute path starting at q (which points at the opening
 * quote), copied into out; 0 if q does not start one. */
static int quoted_path(const char *q, char *out, size_t out_size)
{
	const char *end;
	size_t len;

	if (q[0] != '"' || q[1] != '/')
		return 0;
	end = strchr(q + 1, '"');
	if (end == NULL)
		return 0;
	len = (size_t)(end - (q + 1));
	if (len >= out_size)
		return 0;
	memcpy(out, q + 1, len);
	out[len] = '\0';
	return 1;
}

static void check(const char *file, int line, const char *path, const char *form)
{
	g_found++;
	if (!is_listed(path)) {
		fprintf(stderr,
		        "FAIL: %s:%d executes %s (%s), which include/controlplane_programs.h does "
		        "not list -- add it there and stage it in mkbootroot, or do not execute it "
		        "on the control plane\n",
		        file, line, path, form);
		g_bad++;
	}
}

static void scan_line(const char *file, int line, const char *s)
{
	char path[256];
	const char *p;

	/* #define NAME_BIN "/path" */
	p = s;
	while (*p == ' ' || *p == '\t')
		p++;
	if (strncmp(p, "#define ", 8) == 0) {
		const char *name = p + 8;
		const char *name_end = name;

		while ((*name_end >= 'A' && *name_end <= 'Z') || (*name_end >= '0' && *name_end <= '9') ||
		       *name_end == '_')
			name_end++;
		if (name_end - name > 4 && strncmp(name_end - 4, "_BIN", 4) == 0) {
			const char *q = name_end;

			while (*q == ' ' || *q == '\t')
				q++;
			if (quoted_path(q, path, sizeof(path)))
				check(file, line, path, "a *_BIN define");
		}
		return;
	}

	/* exec*("/path" */
	for (p = strstr(s, "exec"); p != NULL; p = strstr(p + 4, "exec")) {
		const char *q = p + 4;

		while (*q >= 'a' && *q <= 'z')
			q++;
		if (*q != '(')
			continue;
		q++;
		while (*q == ' ')
			q++;
		if (quoted_path(q, path, sizeof(path)))
			check(file, line, path, "a literal exec path");
	}
}

static int scan_file(const char *file)
{
	char buf[4096];
	FILE *f = fopen(file, "r");
	int line = 0;
	int in_comment = 0;

	if (f == NULL) {
		fprintf(stderr, "FAIL: cannot read %s: %s\n", file, strerror(errno));
		return -1;
	}
	while (fgets(buf, sizeof(buf), f) != NULL) {
		const char *t = buf;

		line++;
		/* Skip comment text, where a path is prose, not an exec. Block
		 * comments here always open or continue a line with the marker. */
		while (*t == ' ' || *t == '\t')
			t++;
		if (in_comment) {
			if (strstr(t, "*/") != NULL)
				in_comment = 0;
			continue;
		}
		if (strncmp(t, "/*", 2) == 0) {
			if (strstr(t + 2, "*/") == NULL)
				in_comment = 1;
			continue;
		}
		if (strncmp(t, "//", 2) == 0 || strncmp(t, "* ", 2) == 0)
			continue;
		scan_line(file, line, buf);
	}
	fclose(f);
	return 0;
}

static int ends_with(const char *s, const char *suffix)
{
	size_t ls = strlen(s), lx = strlen(suffix);

	return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

int main(void)
{
	size_t i, j;
	int files = 0;

	for (i = 0; i < sizeof(listed) / sizeof(listed[0]); i++) {
		if (listed[i][0] != '/') {
			fprintf(stderr, "FAIL: %s is listed without an absolute path\n", listed[i]);
			g_bad++;
		}
		for (j = i + 1; j < sizeof(listed) / sizeof(listed[0]); j++) {
			if (strcmp(listed[i], listed[j]) == 0) {
				fprintf(stderr, "FAIL: %s is listed twice\n", listed[i]);
				g_bad++;
			}
		}
	}

	for (i = 0; i < sizeof(scan_dirs) / sizeof(scan_dirs[0]); i++) {
		DIR *d = opendir(scan_dirs[i]);
		struct dirent *de;

		if (d == NULL) {
			fprintf(stderr, "FAIL: cannot open %s: %s (run from the repository root)\n",
			        scan_dirs[i], strerror(errno));
			return 1;
		}
		while ((de = readdir(d)) != NULL) {
			char path[512];

			if (!ends_with(de->d_name, ".c") && !ends_with(de->d_name, ".h"))
				continue;
			snprintf(path, sizeof(path), "%s/%s", scan_dirs[i], de->d_name);
			if (scan_file(path) != 0)
				g_bad++;
			files++;
		}
		closedir(d);
	}
	for (i = 0; i < sizeof(scan_files) / sizeof(scan_files[0]); i++) {
		if (scan_file(scan_files[i]) != 0)
			g_bad++;
		files++;
	}

	/* The daemon alone declares more than ten *_BIN programs; finding
	 * fewer means the scan read the wrong tree, not a clean one. */
	if (g_found < 10) {
		fprintf(stderr, "FAIL: found only %d executed paths in %d files -- the scan did not "
		                "read the control-plane sources\n",
		        g_found, files);
		return 1;
	}
	if (g_bad > 0)
		return 1;
	printf("test_controlplane_programs: PASS (%d executed paths in %d files, all listed)\n",
	       g_found, files);
	return 0;
}
