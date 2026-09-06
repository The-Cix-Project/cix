/*
 * See kernelrecipe.h for what this is and which parts of a kernel
 * recipe must survive it untouched.
 */
#include "kernelrecipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Finds a `key="value"` assignment at the start of a line and returns
 * where the value begins and ends. Anchored to line starts on purpose:
 * these strings also appear inside comments in every kernel recipe, and
 * a search that matched those would rewrite prose.
 */
static int find_assign(const char *text, const char *key, size_t *val_start, size_t *val_end)
{
	size_t klen = strlen(key);
	const char *p = text;

	while (p != NULL && *p != '\0') {
		if ((p == text || p[-1] == '\n') && strncmp(p, key, klen) == 0 && p[klen] == '"') {
			const char *q = strchr(p + klen + 1, '"');

			if (q == NULL)
				return -1;
			*val_start = (size_t)(p + klen + 1 - text);
			*val_end = (size_t)(q - text);
			return 0;
		}
		p = strchr(p, '\n');
		if (p != NULL)
			p++;
	}
	return -1;
}

/*
 * Replaces the first space-separated field of a value, keeping the
 * rest. The kernel recipe's pkg_source and pkg_sha256 each carry two
 * fields; only the upstream tarball's may move.
 */
static int replace_first_field(const char *value, size_t value_len, const char *replacement,
                               char **out)
{
	const char *sp = memchr(value, ' ', value_len);
	size_t tail_len = (sp != NULL) ? value_len - (size_t)(sp - value) : 0;
	size_t rlen = strlen(replacement);
	char *buf = malloc(rlen + tail_len + 1);

	if (buf == NULL)
		return -1;
	memcpy(buf, replacement, rlen);
	if (tail_len > 0)
		memcpy(buf + rlen, sp, tail_len);
	buf[rlen + tail_len] = '\0';
	*out = buf;
	return 0;
}

/* Splices `repl` in place of [start,end) of text. */
static int splice(const char *text, size_t start, size_t end, const char *repl, char **out)
{
	size_t tlen = strlen(text);
	size_t rlen = strlen(repl);
	char *buf;

	if (start > end || end > tlen)
		return -1;
	buf = malloc(tlen - (end - start) + rlen + 1);
	if (buf == NULL)
		return -1;
	memcpy(buf, text, start);
	memcpy(buf + start, repl, rlen);
	memcpy(buf + start + rlen, text + end, tlen - end);
	buf[tlen - (end - start) + rlen] = '\0';
	*out = buf;
	return 0;
}

int kernel_upstream_urls(const char *version, char *out_tarball, size_t tarball_size,
                         char *out_sums, size_t sums_size)
{
	unsigned major = 0;
	const char *p = version;

	if (version == NULL || *p < '0' || *p > '9')
		return -1;
	while (*p >= '0' && *p <= '9') {
		major = major * 10 + (unsigned)(*p - '0');
		p++;
		if (major > 999)
			return -1;
	}
	if (*p != '.')
		return -1;
	/* A recipe-revision suffix is ours, never part of an upstream name. */
	if (strchr(version, '-') != NULL)
		return -1;

	if ((size_t)snprintf(out_tarball, tarball_size,
	                      "https://cdn.kernel.org/pub/linux/kernel/v%u.x/linux-%s.tar.xz", major,
	                      version) >= tarball_size)
		return -1;
	if ((size_t)snprintf(out_sums, sums_size,
	                      "https://cdn.kernel.org/pub/linux/kernel/v%u.x/sha256sums.asc",
	                      major) >= sums_size)
		return -1;
	return 0;
}

int kernel_recipe_generate(const char *current_recipe, const char *new_version,
                           const char *tarball_url, const char *tarball_sha256, char **out,
                           char *err, size_t err_size)
{
	size_t vs, ve, ss, se, hs, he;
	char *stage1 = NULL, *stage2 = NULL, *stage3 = NULL;
	char *new_source = NULL, *new_sha = NULL;
	int rc = -1;
	size_t i;

	if (out != NULL)
		*out = NULL;
	if (current_recipe == NULL || new_version == NULL || tarball_url == NULL ||
	    tarball_sha256 == NULL) {
		snprintf(err, err_size, "missing argument");
		return -1;
	}
	if (strlen(tarball_sha256) != 64) {
		snprintf(err, err_size, "checksum is %zu characters, expected 64",
		         strlen(tarball_sha256));
		return -1;
	}
	for (i = 0; i < 64; i++) {
		char c = tarball_sha256[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			snprintf(err, err_size, "checksum is not lowercase hexadecimal");
			return -1;
		}
	}

	if (find_assign(current_recipe, "pkg_version=", &vs, &ve) != 0) {
		snprintf(err, err_size, "no pkg_version= line in the current recipe");
		return -1;
	}
	if (find_assign(current_recipe, "pkg_source=", &ss, &se) != 0) {
		snprintf(err, err_size, "no pkg_source= line in the current recipe");
		return -1;
	}
	if (find_assign(current_recipe, "pkg_sha256=", &hs, &he) != 0) {
		snprintf(err, err_size, "no pkg_sha256= line in the current recipe");
		return -1;
	}

	if (replace_first_field(current_recipe + ss, se - ss, tarball_url, &new_source) != 0 ||
	    replace_first_field(current_recipe + hs, he - hs, tarball_sha256, &new_sha) != 0) {
		snprintf(err, err_size, "out of memory building the new source and checksum");
		goto out;
	}

	/*
	 * Applied from the LAST offset backwards, so an earlier splice
	 * cannot invalidate a later offset. Doing it forwards is the
	 * classic way to corrupt the second and third edits.
	 */
	if (hs > ss && ss > vs) {
		if (splice(current_recipe, hs, he, new_sha, &stage1) != 0 ||
		    splice(stage1, ss, se, new_source, &stage2) != 0 ||
		    splice(stage2, vs, ve, new_version, &stage3) != 0) {
			snprintf(err, err_size, "out of memory splicing the new recipe");
			goto out;
		}
	} else {
		snprintf(err, err_size,
		         "recipe declares pkg_version/pkg_source/pkg_sha256 in an unexpected order");
		goto out;
	}

	*out = stage3;
	stage3 = NULL;
	snprintf(err, err_size, "ok");
	rc = 0;
out:
	free(stage1);
	free(stage2);
	free(stage3);
	free(new_source);
	free(new_sha);
	return rc;
}
