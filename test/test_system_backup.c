/*
 * Phase 17 (ADR-0033) end-to-end test: proves GET /v1/system/backup and
 * POST /v1/system/restore (do_system_backup()/do_system_restore(),
 * daemon/src/main.c) against a real daemon, with a real daemon restart
 * to prove restored state genuinely gets replayed at boot (containerdef_
 * autostart_all()), not just written to disk. No QEMU needed -- this is
 * plain file I/O + JSON, the same class of test as test_system_update.c.
 *
 * Scope matches the ADR exactly: platform configuration state only
 * (container defs, networks, DNS records, pkg install state + recipes)
 * -- never PKI, never image content, never workload data. Nothing here
 * exercises PKI at all, on purpose.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

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
#define IMAGE_ROOT "/var/lib/kanxeo/images/backuptest/rootfs"
#define CONTAINER_DEFS_PATH "/var/lib/kanxeo/container_defs.json"
#define NETWORKS_PATH "/var/lib/kanxeo/networks.json"
#define DNS_RECORDS_PATH "/var/lib/kanxeo/dns_records.json"

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
	char *dargv[3];

	dargv[0] = "build/kanxeod";
	dargv[1] = PORT_ARG;
	dargv[2] = NULL;

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
	system("rm -rf '" CONTAINER_DEFS_PATH "'");
	system("rm -rf '" NETWORKS_PATH "'");
	system("rm -rf '" DNS_RECORDS_PATH "'");
	system("rm -rf '/var/lib/kanxeo/containers/keeper'");
	system("rm -rf '/var/lib/kanxeo/containers/restored'");
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

	reset_state();
	if (test_image_fixture_build(IMAGE_ROOT, "build/daemon_child", "daemon_child") != 0)
		return 1;

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;

	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
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

	/* 2. GET /system/backup: confirm the bundle's own container_defs
	 * field, re-parsed, actually mentions "keeper". */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/system/backup", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET backup, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *defs = json_str_field(r.json, "container_defs");
		const char *nets = json_str_field(r.json, "networks");
		const char *recs = json_str_field(r.json, "dns_records");
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
		if (recipes == NULL || recipes->type != JSON_OBJECT) {
			fprintf(stderr, "FAIL: backup pkg_recipes missing or not an object\n");
			ok = 0;
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
		FILE *f = fopen(CONTAINER_DEFS_PATH, "rb");

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
		FILE *f = fopen(CONTAINER_DEFS_PATH, "rb");
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

	/* 4. Restore with neither of the five recognized fields -> 400. */
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

	printf(ok ? "SYSTEM BACKUP RESULT: PASS\n" : "SYSTEM BACKUP RESULT: FAIL\n");
	return ok ? 0 : 1;
}
