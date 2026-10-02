/*
 * ADR-0324: the source list and who owns a name, with no daemon.
 *
 * The migration cases use the shape of 192.168.15.95's own
 * repo_config.json (a gitea forge with a token and the 0.2.57-432 commit
 * switch), because that is the file this code will meet on its first
 * boot, and a cleared file, because ADR-0315 makes clearing permanent.
 */
#include "pkgsource.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
	char path[PATH_MAX], legacy[PATH_MAX], offers[PATH_MAX];

	snprintf(path, sizeof(path), "%s/sources.json", g_dir);
	snprintf(legacy, sizeof(legacy), "%s/repo_config.json", g_dir);
	snprintf(offers, sizeof(offers), "%s/sources", g_dir);
	return pkgsource_init(path, legacy, offers);
}

static void test_defaults_and_migration(void)
{
	const struct pkg_source *s;

	reset();
	check(init() == 0 && pkgsource_count() == 1, "a fresh host has one source");
	s = pkgsource_at(0);
	check(s != NULL && strcmp(s->name, PKG_SOURCE_DEFAULT_NAME) == 0 &&
	          strcmp(s->url, PKG_SOURCE_DEFAULT_URL) == 0 && s->trust_keys && !s->write,
	      "it is the public catalogue, read, trusted for keys (ADR-0315)");

	reset();
	write_file("repo_config.json",
	           "{\"repo_url\":\"https://git.home.arpa/itdlabs/cix-recipes\",\"repo_kind\":"
	           "\"gitea\",\"ref\":\"main\",\"auth_token\":\"tok\",\"commit\":true}\n");
	check(init() == 0 && pkgsource_count() == 1, "the single repo config becomes one source");
	s = pkgsource_at(0);
	check(s != NULL && strcmp(s->name, "cix-recipes") == 0 && strcmp(s->kind, "gitea") == 0 &&
	          strcmp(s->token, "tok") == 0 && s->write && s->trust_keys,
	      "named after its repository, its token, write and trust_keys carried");
	check(!exists("repo_config.json") && exists("sources.json"),
	      "the legacy file is gone once the list holds it");
	check(init() == 0 && pkgsource_count() == 1 && pkgsource_find("cix-recipes") != NULL,
	      "a second boot reads the list, not a default");

	reset();
	write_file("repo_config.json",
	           "{\"repo_url\":\"\",\"repo_kind\":\"gitea\",\"ref\":\"master\",\"auth_token\":\"\"}\n");
	check(init() == 0 && pkgsource_count() == 0, "a cleared repo config migrates to no sources");
	check(init() == 0 && pkgsource_count() == 0,
	      "and stays cleared: the default never comes back (ADR-0315)");
}

static void test_validation(void)
{
	struct pkg_source s;
	char err[256];

	reset();
	write_file("sources.json", "{\"sources\":[]}\n");
	check(init() == 0 && pkgsource_count() == 0, "an empty saved list loads empty");

	memset(&s, 0, sizeof(s));
	snprintf(s.name, sizeof(s.name), "%s", "site");
	snprintf(s.url, sizeof(s.url), "%s", "https://forge.example/acme/recipes");
	snprintf(s.kind, sizeof(s.kind), "%s", "github");
	s.write = 1;
	check(pkgsource_add(&s, err, sizeof(err)) == PKGSOURCE_ERR_INVALID && strstr(err, "gitea"),
	      "write on a forge with no commit client is refused, naming gitea");
	s.write = 0;
	check(pkgsource_add(&s, err, sizeof(err)) == PKGSOURCE_OK, "a read github source is added");
	check(strcmp(pkgsource_find("site")->ref, "main") == 0, "ref defaults to main");
	check(pkgsource_add(&s, err, sizeof(err)) == PKGSOURCE_ERR_EXISTS, "a duplicate is refused");
	snprintf(s.name, sizeof(s.name), "%s", "Bad Name");
	check(pkgsource_add(&s, err, sizeof(err)) == PKGSOURCE_ERR_INVALID, "a bad name is refused");
	snprintf(s.name, sizeof(s.name), "%s", "nourl");
	s.url[0] = '\0';
	check(pkgsource_add(&s, err, sizeof(err)) == PKGSOURCE_ERR_INVALID, "a missing url is refused");

	check(pkgsource_update("site", NULL, "gitea", NULL, "secret", 1, -1, err, sizeof(err)) ==
	              PKGSOURCE_OK &&
	          pkgsource_find("site")->write && strcmp(pkgsource_find("site")->token, "secret") == 0,
	      "a partial update changes only what it names");
	check(pkgsource_update("site", NULL, "github", NULL, NULL, -1, -1, err, sizeof(err)) ==
	          PKGSOURCE_ERR_INVALID,
	      "moving a writable source to a forge with no commit client is refused");
	check(pkgsource_update("nosuch", NULL, NULL, NULL, NULL, -1, -1, err, sizeof(err)) ==
	          PKGSOURCE_ERR_NOT_FOUND,
	      "updating a source that does not exist is refused");
}

static void test_ownership(void)
{
	static const char *const a_items[] = { "package:glibc", "package:hibr", "image:jumpbox" };
	static const char *const b_items[] = { "package:glibc", "package:acme-tool" };
	struct pkg_source s;
	char out[256], err[256];

	reset();
	write_file("sources.json", "{\"sources\":[]}\n");
	init();
	memset(&s, 0, sizeof(s));
	snprintf(s.kind, sizeof(s.kind), "%s", "gitea");
	snprintf(s.url, sizeof(s.url), "%s", "https://a.example/o/r");
	snprintf(s.name, sizeof(s.name), "%s", "public");
	pkgsource_add(&s, err, sizeof(err));
	snprintf(s.name, sizeof(s.name), "%s", "site");
	pkgsource_add(&s, err, sizeof(err));

	check(pkgsource_offers_write("public", a_items, 3) == 0 &&
	          pkgsource_offers_write("site", b_items, 2) == 0 && pkgsource_offers_load() == 0,
	      "offers are written and loaded");
	check(pkgsource_owner_of("package:hibr", out, sizeof(out)) == PKGSOURCE_OWNER_ONE &&
	          strcmp(out, "public") == 0,
	      "a name one source offers belongs to it");
	check(pkgsource_owner_of("package:acme-tool", out, sizeof(out)) == PKGSOURCE_OWNER_ONE &&
	          strcmp(out, "site") == 0,
	      "a site's own package belongs to the site");
	check(pkgsource_owner_of("package:nothing", out, sizeof(out)) == PKGSOURCE_OWNER_NONE,
	      "a name nobody offers belongs to nobody");
	check(pkgsource_owner_of("package:glibc", out, sizeof(out)) == PKGSOURCE_OWNER_CONFLICT &&
	          strstr(out, "public") != NULL && strstr(out, "site") != NULL,
	      "a name two sources offer is a conflict naming both -- list order decides nothing");
	check(pkgsource_owner_of("image:jumpbox", out, sizeof(out)) == PKGSOURCE_OWNER_ONE,
	      "images are owned the same way");

	check(pkgsource_choose("package:glibc", "site", err, sizeof(err)) == PKGSOURCE_OK &&
	          pkgsource_owner_of("package:glibc", out, sizeof(out)) == PKGSOURCE_OWNER_ONE &&
	          strcmp(out, "site") == 0,
	      "the operator's choice resolves the conflict");
	check(pkgsource_choose("glibc", "site", err, sizeof(err)) == PKGSOURCE_ERR_INVALID,
	      "an item without a kind is refused");
	check(pkgsource_choose("package:glibc", "nosuch", err, sizeof(err)) ==
	          PKGSOURCE_ERR_NOT_FOUND,
	      "choosing a source that does not exist is refused");

	/* Survives a reload. */
	init();
	check(pkgsource_owner_of("package:glibc", out, sizeof(out)) == PKGSOURCE_OWNER_ONE &&
	          strcmp(out, "site") == 0,
	      "choices and offers survive a restart");

	check(pkgsource_remove("site", err, sizeof(err)) == PKGSOURCE_OK &&
	          pkgsource_owner_of("package:glibc", out, sizeof(out)) == PKGSOURCE_OWNER_ONE &&
	          strcmp(out, "public") == 0 &&
	          pkgsource_owner_of("package:acme-tool", out, sizeof(out)) == PKGSOURCE_OWNER_NONE,
	      "removing a source drops its offers and choices, and ownership follows");
}

int main(void)
{
	char tmpl[] = "/tmp/cix_pkgsource_XXXXXX";
	char cmd[PATH_MAX + 16];

	printf("test_pkgsource\n");
	if (mkdtemp(tmpl) == NULL) {
		printf("PKGSOURCE RESULT: FAIL (mkdtemp: %s)\n", strerror(errno));
		return 1;
	}
	snprintf(g_dir, sizeof(g_dir), "%s", tmpl);
	test_defaults_and_migration();
	test_validation();
	test_ownership();
	snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
	if (system(cmd) != 0)
		printf("  (could not remove %s)\n", g_dir);
	printf("PKGSOURCE RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
