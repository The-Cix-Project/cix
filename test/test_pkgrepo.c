/*
 * ADR-0324 step B: the package repository list, with no daemon.
 *
 * The migration cases use the shape of 192.168.15.95's own
 * artifact_config.json (the LAN cache, a token, push on), because that
 * is the file this code meets on its first boot, and a cleared file,
 * because ADR-0315 makes clearing permanent. Losing `push` in migration
 * would stop every approval on the one host that authors the public
 * cache, silently -- so it is asserted, not assumed.
 */
#include "pkgrepo.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static char g_dir[PATH_MAX];

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

static void write_file(const char *name, const char *content)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", g_dir, name);
	f = fopen(path, "w");
	if (f != NULL) {
		fputs(content, f);
		fclose(f);
	}
}

static int exists(const char *name)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", g_dir, name);
	return access(path, F_OK) == 0;
}

static void reset(void)
{
	char cmd[PATH_MAX + 16];

	snprintf(cmd, sizeof(cmd), "rm -rf %s/*", g_dir);
	if (system(cmd) != 0)
		printf("  (could not empty %s)\n", g_dir);
}

static int init(void)
{
	char path[PATH_MAX], legacy[PATH_MAX];

	snprintf(path, sizeof(path), "%s/repositories.json", g_dir);
	snprintf(legacy, sizeof(legacy), "%s/artifact_config.json", g_dir);
	return pkgrepo_init(path, legacy);
}

static void test_defaults_and_migration(void)
{
	const struct pkg_repository *r;

	reset();
	check(init() == 0 && pkgrepo_count() == 1, "a fresh host has one repository");
	r = pkgrepo_at(0);
	check(r != NULL && strcmp(r->name, PKG_REPOSITORY_DEFAULT_NAME) == 0 &&
	          strcmp(r->url, PKG_REPOSITORY_DEFAULT_URL) == 0 && !r->push && r->token[0] == '\0',
	      "it is the public cache, pull only, no token (ADR-0315)");
	check(pkgrepo_push_count() == 0, "and nothing is pushed by default (#129)");

	reset();
	write_file("artifact_config.json",
	           "{\"base_url\":\"http://192.168.15.31:8080\",\"auth_token\":\"tok\","
	           "\"push_enabled\":true}\n");
	check(init() == 0 && pkgrepo_count() == 1, "the single artifact server becomes one repository");
	r = pkgrepo_at(0);
	check(r != NULL && strcmp(r->name, "192.168.15.31-8080") == 0 &&
	          strcmp(r->url, "http://192.168.15.31:8080") == 0 &&
	          strcmp(r->token, "tok") == 0 && r->push,
	      "named after its host, with its url, token and push carried");
	check(pkgrepo_push_count() == 1, "so it is still the repository this host pushes to");
	check(!exists("artifact_config.json") && exists("repositories.json"),
	      "the legacy file is gone once the list holds it");
	check(init() == 0 && pkgrepo_count() == 1 && pkgrepo_find("192.168.15.31-8080") != NULL &&
	          pkgrepo_find("192.168.15.31-8080")->push,
	      "a second boot reads the list, push intact");

	reset();
	write_file("artifact_config.json",
	           "{\"base_url\":\"\",\"auth_token\":\"\",\"push_enabled\":false}\n");
	check(init() == 0 && pkgrepo_count() == 0, "a cleared artifact config migrates to no repositories");
	check(init() == 0 && pkgrepo_count() == 0,
	      "and stays cleared: the default never comes back (ADR-0315)");
}

static void test_edits(void)
{
	struct pkg_repository r;
	char err[256];

	reset();
	write_file("repositories.json", "{\"repositories\":[]}\n");
	check(init() == 0 && pkgrepo_count() == 0, "an empty saved list loads empty");

	memset(&r, 0, sizeof(r));
	snprintf(r.name, sizeof(r.name), "%s", "site");
	snprintf(r.url, sizeof(r.url), "%s", "https://cache.example");
	check(pkgrepo_add(&r, err, sizeof(err)) == PKGSOURCE_OK, "a repository is added");
	snprintf(r.name, sizeof(r.name), "%s", "mirror");
	snprintf(r.url, sizeof(r.url), "%s", "https://mirror.example/cix");
	r.push = 1;
	snprintf(r.token, sizeof(r.token), "%s", "t");
	check(pkgrepo_add(&r, err, sizeof(err)) == PKGSOURCE_OK, "a second is added after it");
	check(strcmp(pkgrepo_at(0)->name, "site") == 0 && strcmp(pkgrepo_at(1)->name, "mirror") == 0,
	      "the list keeps the order they were added in -- the order they are tried in");
	check(pkgrepo_add(&r, err, sizeof(err)) == PKGSOURCE_ERR_EXISTS, "a duplicate is refused");
	snprintf(r.name, sizeof(r.name), "%s", "Bad Name");
	check(pkgrepo_add(&r, err, sizeof(err)) == PKGSOURCE_ERR_INVALID,
	      "a bad name is refused, by the rule recipe sources use");
	snprintf(r.name, sizeof(r.name), "%s", "nourl");
	snprintf(r.url, sizeof(r.url), "%s", "cache.example");
	check(pkgrepo_add(&r, err, sizeof(err)) == PKGSOURCE_ERR_INVALID && strstr(err, "url") != NULL,
	      "a url with no scheme is refused, naming the url");

	check(pkgrepo_push_count() == 1, "one of two pushes");
	check(pkgrepo_update("site", NULL, "s", 1, err, sizeof(err)) == PKGSOURCE_OK &&
	          pkgrepo_find("site")->push && strcmp(pkgrepo_find("site")->token, "s") == 0 &&
	          strcmp(pkgrepo_find("site")->url, "https://cache.example") == 0,
	      "a partial update changes only what it names");
	check(pkgrepo_push_count() == 2, "both push now");
	check(pkgrepo_update("site", NULL, "", -1, err, sizeof(err)) == PKGSOURCE_OK &&
	          pkgrepo_find("site")->token[0] == '\0' && pkgrepo_find("site")->push,
	      "an empty token clears it and leaves push alone");
	check(pkgrepo_update("nosuch", NULL, NULL, -1, err, sizeof(err)) == PKGSOURCE_ERR_NOT_FOUND,
	      "updating a repository that does not exist is refused");

	check(init() == 0 && pkgrepo_count() == 2 && pkgrepo_find("mirror") != NULL,
	      "edits survive a restart");
	check(pkgrepo_remove("site", err, sizeof(err)) == PKGSOURCE_OK && pkgrepo_count() == 1 &&
	          strcmp(pkgrepo_at(0)->name, "mirror") == 0,
	      "removing one keeps the rest in order");
	check(pkgrepo_remove("site", err, sizeof(err)) == PKGSOURCE_ERR_NOT_FOUND,
	      "removing it again is refused");
}

int main(void)
{
	char tmpl[] = "/tmp/cix_pkgrepo_XXXXXX";
	char cmd[PATH_MAX + 16];

	printf("test_pkgrepo\n");
	if (mkdtemp(tmpl) == NULL) {
		printf("PKGREPO RESULT: FAIL (mkdtemp: %s)\n", strerror(errno));
		return 1;
	}
	snprintf(g_dir, sizeof(g_dir), "%s", tmpl);
	test_defaults_and_migration();
	test_edits();
	snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
	if (system(cmd) != 0)
		printf("  (could not remove %s)\n", g_dir);
	printf("PKGREPO RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
