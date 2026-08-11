/*
 * Phase 17 (ADR-0033) end-to-end test: proves GET /v1/system/backup and
 * POST /v1/system/restore (do_system_backup()/do_system_restore(),
 * daemon/src/main.c) against a real daemon, with a real daemon restart
 * to prove restored state genuinely gets replayed at boot (containerdef_
 * autostart_all()), not just written to disk. No QEMU needed -- this is
 * plain file I/O + JSON, the same class of test as test_system_update.c.
 *
 * Scope matches the ADR exactly: platform configuration state only
 * (container defs, networks, DNS records, pkg install state + recipes,
 * site config) -- never PKI, never image content, never workload data.
 * Nothing here exercises PKI at all, on purpose.
 */
#include "httpclient.h"
#include "json.h"
#include "persist.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7633
#define PORT_ARG "--port=7633"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];
static char g_container_defs_path[PATH_MAX];
static char g_networks_path[PATH_MAX];
static char g_dns_records_path[PATH_MAX];
static char g_containers_dir[PATH_MAX];

static int wait_for_daemon(const struct kx_client *c, int max_attempts)
{
	int i;
	struct kx_response r;

	for (i = 0; i < max_attempts; i++) {
		if (kx_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			kx_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/kanxeod";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/kanxeod", dargv, environ);
		perror("execve build/kanxeod");
		_exit(127);
	}
	return pid;
}

static int stop_daemon(pid_t pid)
{
	int status;

	kill(pid, SIGTERM);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static void reset_state(void)
{
	char cmd[PATH_MAX + 16];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_container_defs_path);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_networks_path);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dns_records_path);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s/keeper'", g_containers_dir);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s/restored'", g_containers_dir);
	system(cmd);
}

static int container_exists(const struct kx_client *c, const char *name)
{
	char path[128];
	struct kx_response r;
	int status;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	memset(&r, 0, sizeof(r));
	if (kx_client_request(c, "GET", path, NULL, &r) != 0)
		return -1;
	status = r.status;
	kx_response_free(&r);
	return status == 200;
}

static time_t wait_for_present(const struct kx_client *c, const char *name, int max_attempts)
{
	int attempts;

	for (attempts = 0; attempts < max_attempts; attempts++) {
		if (container_exists(c, name) == 1)
			return time(NULL);
		usleep(100000);
	}
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	char *saved_bundle = NULL;
	size_t saved_bundle_len = 0;
	char *pre_restore_defs = NULL;
	size_t pre_restore_defs_len = 0;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/images/backuptest/v1/rootfs", g_data_dir);
	snprintf(g_container_defs_path, sizeof(g_container_defs_path), "%s/container_defs.json",
	         g_data_dir);
	snprintf(g_networks_path, sizeof(g_networks_path), "%s/networks.json", g_data_dir);
	snprintf(g_dns_records_path, sizeof(g_dns_records_path), "%s/dns_records.json", g_data_dir);
	snprintf(g_containers_dir, sizeof(g_containers_dir), "%s/containers", g_data_dir);

	reset_state();
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/images/backuptest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}
	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. Real state: a network, a DNS record, and a persisted
	 * ("restart":"always") container named "keeper". */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"backupnet\",\"subnet\":\"172.62.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST network, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/dns/records",
	                       "{\"name\":\"backup.internal\",\"ip\":\"172.62.0.5\"}", &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST dns record, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"keeper\",\"image\":\"backuptest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"],"
	                       "\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST keeper, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "PUT", "/v1/system/site",
	                       "{\"instance_name\":\"backuptest-instance\",\"site_name\":\"\","
	                       "\"domain_suffix\":\"internal\"}",
	                       &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: PUT site config, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* ADR-0120 regression coverage: do_system_backup()'s own recipe walk
	 * used to assume the pre-ADR-0107 flat <name>.recipe layout
	 * (opendir(PKG_RECIPES_DIR) filtering a ".recipe" suffix directly),
	 * which silently matched nothing at all once recipes moved to the
	 * nested <name>/<version>/build.sh layout -- every entry in
	 * PKG_RECIPES_DIR became a per-name subdirectory instead. A real
	 * recipe added here, with its exact "<name>/<version>" backup key
	 * and content asserted below, is what would have caught that. */
	{
		char recipe_body[1024];
		struct json_writer rw;

		snprintf(recipe_body, sizeof(recipe_body),
		         "pkg_name=backuptestpkg\npkg_version=1.0\n"
		         "pkg_source=http://127.0.0.1:1/unreachable.tar.gz\n"
		         "pkg_sha256=%s\npkg_depends=\"\"\n\n"
		         "pkg_build() {\n\ttrue\n}\n\npkg_install() {\n\ttrue\n}\n",
		         "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		jw_init(&rw);
		jw_obj_open(&rw);
		jw_key(&rw, "name");
		jw_str(&rw, "backuptestpkg");
		jw_key(&rw, "content");
		jw_str(&rw, recipe_body);
		jw_obj_close(&rw);
		rw.buf[rw.len] = '\0';

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/recipes", rw.buf, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: POST recipe backuptestpkg, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
		free(rw.buf);
	}

	/* 2. GET /system/backup: confirm the bundle's own container_defs
	 * field, re-parsed, actually mentions "keeper", and that
	 * site_config actually carries the instance_name just set. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/system/backup", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET backup, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *defs = json_str_field(r.json, "container_defs");
		const char *nets = json_str_field(r.json, "networks");
		const char *recs = json_str_field(r.json, "dns_records");
		const char *site = json_str_field(r.json, "site_config");
		const struct json_value *recipes = json_object_get(r.json, "pkg_recipes");

		if (defs == NULL || strstr(defs, "keeper") == NULL) {
			fprintf(stderr, "FAIL: backup container_defs missing \"keeper\"\n");
			ok = 0;
		}
		if (nets == NULL || strstr(nets, "backupnet") == NULL) {
			fprintf(stderr, "FAIL: backup networks missing \"backupnet\"\n");
			ok = 0;
		}
		if (recs == NULL || strstr(recs, "backup.internal") == NULL) {
			fprintf(stderr, "FAIL: backup dns_records missing \"backup.internal\"\n");
			ok = 0;
		}
		if (site == NULL || strstr(site, "backuptest-instance") == NULL) {
			fprintf(stderr, "FAIL: backup site_config missing \"backuptest-instance\"\n");
			ok = 0;
		}
		if (recipes == NULL || recipes->type != JSON_OBJECT) {
			fprintf(stderr, "FAIL: backup pkg_recipes missing or not an object\n");
			ok = 0;
		} else {
			const struct json_value *entry = json_object_get(recipes, "backuptestpkg/1.0");
			const char *content = entry != NULL ? json_as_string(entry) : NULL;

			if (content == NULL || strstr(content, "pkg_name=backuptestpkg") == NULL) {
				fprintf(stderr,
				        "FAIL: backup pkg_recipes missing \"backuptestpkg/1.0\" key with real "
				        "content (ADR-0120 regression: version-keyed layout not walked)\n");
				ok = 0;
			}
		}
		/* Only container_defs is saved for the later restore step below
		 * -- restoring the full bundle (including "networks") would
		 * reintroduce backupnet's own real bridge on the post-restore
		 * restart, right after it's deliberately deleted a few lines
		 * down; restore's own fields are independent by design (ADR-
		 * 0033), so restoring just this one field is both a cleaner
		 * test setup and a more precise proof of exactly what this
		 * test is about: container definitions round-tripping through
		 * backup/restore/replay. */
		if (defs != NULL) {
			saved_bundle_len = strlen(defs);
			saved_bundle = malloc(saved_bundle_len + 1);
			if (saved_bundle != NULL)
				memcpy(saved_bundle, defs, saved_bundle_len + 1);
		}
	}
	kx_response_free(&r);

	if (saved_bundle == NULL) {
		fprintf(stderr, "FAIL: could not capture the backup bundle for later restore\n");
		ok = 0;
	}

	/* backupnet's own real bridge interface must be torn down via the
	 * real API (not just its JSON state left to rot) -- rm -rf'ing
	 * networks.json alone leaves a real, still-existing kernel bridge
	 * behind that would collide with the next run's own POST of the
	 * same name (confirmed directly: this is the correct convention
	 * test_networks.c's own suite already follows -- it has no
	 * reset_state() at all, relying entirely on DELETE for cleanup). */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/backupnet", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE backupnet expected 204, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/dns/records/backup.internal", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE backup.internal expected 204, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* Remember the real on-disk container_defs.json content before
	 * restore ever runs, to prove a bad restore leaves it untouched. */
	{
		FILE *f = fopen(g_container_defs_path, "rb");

		if (f != NULL) {
			fseek(f, 0, SEEK_END);
			pre_restore_defs_len = (size_t)ftell(f);
			fseek(f, 0, SEEK_SET);
			pre_restore_defs = malloc(pre_restore_defs_len + 1);
			if (pre_restore_defs != NULL) {
				size_t n = fread(pre_restore_defs, 1, pre_restore_defs_len, f);

				pre_restore_defs[n] = '\0';
			}
			fclose(f);
		}
	}

	/* 3. Bad restore: a malformed container_defs field is rejected 400,
	 * and the real file on disk is confirmed byte-for-byte untouched. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/system/restore",
	                       "{\"container_defs\":\"not valid json\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: restore with bad container_defs expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);
	{
		FILE *f = fopen(g_container_defs_path, "rb");
		char after[65536] = { 0 };
		size_t n = 0;

		if (f != NULL) {
			n = fread(after, 1, sizeof(after) - 1, f);
			fclose(f);
		}
		if (pre_restore_defs == NULL || n != pre_restore_defs_len ||
		    memcmp(after, pre_restore_defs, n) != 0) {
			fprintf(stderr, "FAIL: container_defs.json was modified by a rejected restore\n");
			ok = 0;
		}
	}

	/* 4. Restore with none of the six recognized fields -> 400. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/system/restore", "{}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: restore with no fields expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. Delete "keeper" entirely (live + persisted definition gone),
	 * then restore the earlier-saved bundle and confirm a real daemon
	 * restart brings "keeper" back -- the actual end-to-end proof, not
	 * just that a write succeeded. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/containers/keeper", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE keeper expected 204, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (container_exists(&client, "keeper") != 0) {
		fprintf(stderr, "FAIL: keeper should be gone after DELETE\n");
		ok = 0;
	}

	if (saved_bundle != NULL) {
		struct json_writer restore_body;

		jw_init(&restore_body);
		jw_obj_open(&restore_body);
		jw_key(&restore_body, "container_defs");
		jw_str(&restore_body, saved_bundle);
		jw_obj_close(&restore_body);
		restore_body.buf[restore_body.len] = '\0';

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/system/restore", restore_body.buf, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "status"), "restored")) {
			fprintf(stderr, "FAIL: restore of saved bundle expected 200 \"restored\", got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
		jw_free(&restore_body);
	}

	/* ADR-0120 regression coverage, restore side: a distinct (name,
	 * version) key than the one added via POST /v1/pkg/recipes above,
	 * to prove do_system_restore() itself (not just pkg_recipe_add())
	 * writes to the real nested <name>/<version>/build.sh path -- the
	 * old restore code wrote a flat "<key>.recipe" file that nothing
	 * in the version-keyed lookup path (find_recipe_path()) would ever
	 * find again. */
	{
		struct json_writer recipe_restore;
		char expected_path[PATH_MAX];
		char *on_disk = NULL;
		size_t on_disk_len = 0;

		jw_init(&recipe_restore);
		jw_obj_open(&recipe_restore);
		jw_key(&recipe_restore, "pkg_recipes");
		jw_obj_open(&recipe_restore);
		jw_key(&recipe_restore, "restoredpkg/2.0");
		jw_str(&recipe_restore, "pkg_name=restoredpkg\npkg_version=2.0\n");
		jw_obj_close(&recipe_restore);
		jw_obj_close(&recipe_restore);
		recipe_restore.buf[recipe_restore.len] = '\0';

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/system/restore", recipe_restore.buf, &r) !=
		        0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: restore of pkg_recipes expected 200, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
		jw_free(&recipe_restore);

		snprintf(expected_path, sizeof(expected_path), "%s/pkg/recipes/restoredpkg/2.0/build.sh",
		         g_data_dir);
		if (persist_read_file(expected_path, &on_disk, &on_disk_len) != 0 || on_disk == NULL ||
		    strstr(on_disk, "pkg_name=restoredpkg") == NULL) {
			fprintf(stderr,
			        "FAIL: restored recipe not found at real nested path %s (ADR-0120 "
			        "regression: restore wrote the old flat layout)\n",
			        expected_path);
			ok = 0;
		}
		free(on_disk);
	}

	/* Restore does NOT hot-reload -- keeper must still be absent from
	 * the live registry right now, only the on-disk definition changed. */
	if (container_exists(&client, "keeper") != 0) {
		fprintf(stderr, "FAIL: restore must not live-reload -- keeper should still be absent "
		                "until a real restart\n");
		ok = 0;
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon (post-restore restart) never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	if (wait_for_present(&client, "keeper", 50) == 0) {
		fprintf(stderr,
		        "FAIL: keeper (restored via /system/restore) did not come back on restart\n");
		ok = 0;
	} else {
		printf("keeper (deleted, then restored via /system/restore) came back on a real "
		       "daemon restart\n");
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon (post-restore) did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	free(saved_bundle);
	free(pre_restore_defs);
	(void)saved_bundle_len;

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "SYSTEM BACKUP RESULT: PASS\n" : "SYSTEM BACKUP RESULT: FAIL\n");
	return ok ? 0 : 1;
}
