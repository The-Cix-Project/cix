/*
 * ADR-0323 rung 4: the trusted-origins list, with no daemon. The
 * template in the first case is the shape an own-forge recipe declares,
 * credentials and placeholder included, because that is what the list
 * is asked about.
 */
#include "srctrust.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

int main(void)
{
	char path[128], o[SRCTRUST_ORIGIN_MAX], err[256];
	const char *one[] = { "https://git.home.arpa" };
	const char *two[] = { "https://git.home.arpa", "http://127.0.0.1:8080" };
	const char *dup[] = { "https://git.home.arpa", "https://git.home.arpa" };
	const char *bad[] = { "https://git.home.arpa/itdlabs" };
	const char *upper[] = { "https://Git.Home.Arpa" };

	printf("test_srctrust\n");
	snprintf(path, sizeof(path), "/tmp/cix_srctrust_%d.json", (int)getpid());
	unlink(path);

	check(srctrust_origin_of("https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/"
	                         "hibr/archive/v{version}.tar.gz",
	                         o, sizeof(o)) == 0 &&
	          strcmp(o, "https://git.home.arpa") == 0,
	      "the origin of a template drops credentials and path");
	check(srctrust_origin_of("http://127.0.0.1:8080/x?y", o, sizeof(o)) == 0 &&
	          strcmp(o, "http://127.0.0.1:8080") == 0,
	      "a port belongs to the origin");
	check(srctrust_origin_of("HTTPS://x", o, sizeof(o)) != 0, "the scheme is matched as written");
	check(srctrust_origin_of("https://Git.Home.Arpa", o, sizeof(o)) == 0 &&
	          strcmp(o, "https://git.home.arpa") == 0,
	      "the host is lowercased");
	check(srctrust_origin_of("ftp://x/y", o, sizeof(o)) != 0, "only http(s)");
	check(srctrust_origin_of("https:///path", o, sizeof(o)) != 0, "an empty host is not an origin");

	check(srctrust_init(path) == 0 && srctrust_count() == 0,
	      "a fresh host trusts nothing (ADR-0323: operator's API call only)");
	check(!srctrust_trusts("https://git.home.arpa/x"), "so nothing is trusted");
	check(srctrust_set(one, 1, err, sizeof(err)) == 0 && srctrust_count() == 1,
	      "an origin is added");
	check(srctrust_trusts("https://u:t@git.home.arpa/api/v1/repos/o/r/archive/v1.tar.gz"),
	      "and a url from it is trusted, credentials or not");
	check(!srctrust_trusts("https://git.home.arpa.evil.example/x"),
	      "a host that merely begins with it is not");
	check(!srctrust_trusts("http://git.home.arpa/x"), "nor the same host over another scheme");
	check(srctrust_set(dup, 2, err, sizeof(err)) != 0 && strstr(err, "twice") != NULL &&
	          srctrust_count() == 1,
	      "a duplicate is refused and nothing changes");
	check(srctrust_set(bad, 1, err, sizeof(err)) != 0 && strstr(err, "not an origin") != NULL,
	      "a url with a path is not an origin");
	check(srctrust_set(upper, 1, err, sizeof(err)) != 0,
	      "an origin must already be in its canonical form");
	check(srctrust_set(two, 2, err, sizeof(err)) == 0 && srctrust_init(path) == 0 &&
	          srctrust_count() == 2 && strcmp(srctrust_at(1), "http://127.0.0.1:8080") == 0,
	      "the list survives a restart, in order");
	check(srctrust_set(NULL, 0, err, sizeof(err)) == 0 && srctrust_init(path) == 0 &&
	          srctrust_count() == 0,
	      "and can be emptied");
	unlink(path);
	printf("SRCTRUST RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
