/*
 * ADR-0323: the gitea-tags discovery kind, with no daemon and no network.
 *
 * The tags listing used here has the shape Gitea 1.25 returns from
 * GET /api/v1/repos/<owner>/<repo>/tags (an array of objects with
 * "name", newest first), trimmed to the fields this module reads.
 */
#include "srcgitea.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

static void test_tags_url(void)
{
	char out[512];

	check(srcgitea_tags_url("https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/"
	                        "hibr/archive/v{version}.tar.gz",
	                        out, sizeof(out)) == 0 &&
	          strcmp(out, "https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/"
	                      "hibr/tags?limit=50&page=1") == 0,
	      "the tags listing sits beside the archive route, credentials kept");
	check(srcgitea_tags_url("http://127.0.0.1:8080/api/v1/repos/o/r/archive/{version}.tar.gz", out,
	                        sizeof(out)) == 0 &&
	          strcmp(out, "http://127.0.0.1:8080/api/v1/repos/o/r/tags?limit=50&page=1") == 0,
	      "a port and a bare {version} tag are fine");
	check(srcgitea_tags_url("https://git.home.arpa/itdlabs/hibr/archive/v{version}.tar.gz", out,
	                        sizeof(out)) != 0,
	      "the web archive route is not this kind's shape");
	check(srcgitea_tags_url("https://example.org/api/v1/repos/o/archive/x.tar.gz", out,
	                        sizeof(out)) != 0,
	      "a repository path missing its repo is refused");
	check(srcgitea_tags_url("ftp://h/api/v1/repos/o/r/archive/x.tar.gz", out, sizeof(out)) != 0,
	      "only http(s)");
	check(srcgitea_tags_url("https://h/api/v1/repos/o/r/archive/x", out, 20) != 0 && out[0] == '\0',
	      "a url that does not fit is refused, not truncated");
}

static void test_version_of_tag(void)
{
	char v[SRCUPSTREAM_VERSION_MAX];

	check(srcgitea_version_of_tag("v0.99.4", "v{version}", v, sizeof(v)) == 0 &&
	          strcmp(v, "0.99.4") == 0,
	      "v{version} strips the v");
	check(srcgitea_version_of_tag("v0.1.102", "{version}", v, sizeof(v)) == 0 &&
	          strcmp(v, "v0.1.102") == 0,
	      "{version} keeps the whole tag -- cbs's own versions carry the v");
	check(srcgitea_version_of_tag("release-2.1-final", "release-{version}-final", v,
	                              sizeof(v)) == 0 &&
	          strcmp(v, "2.1") == 0,
	      "fixed text on both sides");
	check(srcgitea_version_of_tag("nightly", "v{version}", v, sizeof(v)) != 0,
	      "a tag the template does not match is skipped");
	check(srcgitea_version_of_tag("v", "v{version}", v, sizeof(v)) != 0, "an empty version is not one");
	check(srcgitea_version_of_tag("vfoo/bar", "v{version}", v, sizeof(v)) != 0,
	      "a version with a slash is refused");
	check(srcgitea_version_of_tag("v1", "v{major}", v, sizeof(v)) != 0,
	      "a template without {version} names nothing");
}

static void test_listing_and_cache(void)
{
	static const char listing[] =
	    "[{\"name\":\"v0.99.5\",\"id\":\"a\"},{\"name\":\"nightly\"},"
	    "{\"name\":\"v0.99.4\",\"commit\":{\"sha\":\"b\"}},{\"name\":\"v0.49.1\"}]";
	char out[SRCUPSTREAM_MAX_CANDIDATES][SRCUPSTREAM_VERSION_MAX];
	char tmpl[] = "/tmp/cix_srcgitea_XXXXXX";
	char cmd[PATH_MAX + 16];
	size_t n;

	check(srcgitea_versions_from_listing(listing, strlen(listing), "v{version}", out, 32) == 3 &&
	          strcmp(out[0], "0.99.5") == 0 && strcmp(out[2], "0.49.1") == 0,
	      "the listing maps to the versions its tags name, nightly skipped");
	check(srcgitea_versions_from_listing(listing, strlen(listing), "v{version}", out, 2) == 2,
	      "never more than max");
	check(srcgitea_versions_from_listing("{\"message\":\"not found\"}", 24, "v{version}", out,
	                                     32) == -1,
	      "a Gitea error document is not an empty listing");

	if (mkdtemp(tmpl) == NULL) {
		printf("  FAIL: mkdtemp: %s\n", strerror(errno));
		failures++;
		return;
	}
	check(srcgitea_init(tmpl) == 0, "the cache directory is usable");
	check(srcgitea_fetched_at("hibr") == 0 && srcgitea_candidates("hibr", out, 32) == 0,
	      "never fetched reads as 0 and no candidates");
	check(srcgitea_store_listing("hibr", listing, strlen(listing), "v{version}", 1790000000L) == 0,
	      "a listing is stored");
	n = srcgitea_candidates("hibr", out, 32);
	check(n == 3 && strcmp(out[1], "0.99.4") == 0 && srcgitea_fetched_at("hibr") == 1790000000L,
	      "and read back with its time");
	check(srcgitea_store_listing("hibr", "{}", 2, "v{version}", 1790000001L) != 0 &&
	          srcgitea_fetched_at("hibr") == 1790000000L,
	      "a bad listing does not replace a good one");
	{
		char why[128];

		check(srcgitea_store_error("hibr", "tags listing answered 404", 1790000500L) == 0 &&
		          srcgitea_error("hibr", why, sizeof(why)) == 0 &&
		          strcmp(why, "tags listing answered 404") == 0,
		      "a failed refresh records why");
		check(srcgitea_candidates("hibr", out, 32) == 3 &&
		          srcgitea_fetched_at("hibr") == 1790000000L,
		      "and keeps what the last good refresh found");
		check(srcgitea_store_listing("hibr", listing, strlen(listing), "v{version}",
		                             1790000600L) == 0 &&
		          srcgitea_error("hibr", why, sizeof(why)) == 0 && why[0] == '\0',
		      "a good refresh clears it");
		check(srcgitea_store_error("cbs", "no source template", 1L) == 0 &&
		          srcgitea_fetched_at("cbs") == 0,
		      "an error for a package never fetched leaves it never fetched");
	}
	check(srcgitea_store_listing("../x", listing, strlen(listing), "v{version}", 1L) != 0,
	      "a package name is never a path");
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpl);
	if (system(cmd) != 0)
		printf("  (could not remove %s)\n", tmpl);
}

int main(void)
{
	printf("test_srcgitea\n");
	test_tags_url();
	test_version_of_tag();
	test_listing_and_cache();
	printf("SRCGITEA RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
