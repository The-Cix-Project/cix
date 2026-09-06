/*
 * ADR-0141 Phase 5 end-to-end test: turns the `backup` disk role from
 * a pure inert label into a real mechanism -- GET/PUT /v1/system/
 * backup-config, GET .../status, POST .../snapshot-now
 * (daemon/src/backupconfig.c) -- plus the safety check that keeps
 * DELETE /diskroles/{name} and POST /disks/{name}/format from pulling
 * the configured backup disk out from under it.
 *
 * The real, successful snapshot-write path needs a real, mounted
 * backup-role disk -- this dev sandbox's own real host disks can't be
 * safely formatted in an automated test (same documented boundary
 * test/test_storage_placement.c already established for the other
 * three storage kinds). What IS fully, safely testable here -- and is
 * -- is everything up to that point: the REST shape, the partial-
 * update PUT semantics, every validation/rejection path a snapshot
 * attempt can hit without ever formatting anything, and the 409 safety
 * check. diskrole assignment alone is non-destructive by the API's own
 * design.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7641
#define PORT_ARG "--port=7641"

static char g_data_dir[PATH_MAX];

static int wait_for_daemon(const struct cix_client *c, int max_attempts)
{
	int i;
	struct cix_response r;

	for (i = 0; i < max_attempts; i++) {
		if (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			cix_response_free(&r);
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

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		perror("execve build/cixd");
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

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	struct cix_response r;
	char non_os_disk[64] = "";

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. Default state, fresh daemon: no disk, disabled, no schedule. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/backup-config", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/system/backup-config, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jdisk = json_object_get(r.json, "disk");
		const struct json_value *jenabled = json_object_get(r.json, "enabled");
		/* ADR-0257: this resource no longer carries a schedule. */
		const struct json_value *jinterval = json_object_get(r.json, "interval_hours");

		if (jinterval != NULL) {
			fprintf(stderr, "FAIL: backup-config still reports interval_hours (ADR-0257 "
			                "moved it to /v1/schedules)\n");
			ok = 0;
		}
		if (jdisk == NULL || jdisk->type != JSON_NULL) {
			fprintf(stderr, "FAIL: fresh daemon should report disk:null\n");
			ok = 0;
		}
		if (jenabled == NULL || jenabled->type != JSON_BOOL || jenabled->u.boolean) {
			fprintf(stderr, "FAIL: fresh daemon should report enabled:false\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/backup-config/status", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/system/backup-config/status, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *state = json_str_field(r.json, "state");

		if (!(state != NULL && strcmp(state, "never") == 0)) {
			fprintf(stderr, "FAIL: fresh daemon status should be state:never, got %s\n",
			        state != NULL ? state : "(null)");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 2. Snapshot-now with no disk configured -> real, immediate,
	 * synchronous failure -- always deterministic, no hardware needed. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/system/backup-config/snapshot-now", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: POST snapshot-now (no disk), status=%d\n", r.status);
		ok = 0;
	} else {
		const char *state = json_str_field(r.json, "state");
		const char *error = json_str_field(r.json, "error");

		if (!(state != NULL && strcmp(state, "failed") == 0)) {
			fprintf(stderr, "FAIL: snapshot-now with no disk configured should fail, got %s\n",
			        state != NULL ? state : "(null)");
			ok = 0;
		}
		if (error == NULL || strstr(error, "no backup disk configured") == NULL) {
			fprintf(stderr, "FAIL: snapshot-now error message unexpected: %s\n",
			        error != NULL ? error : "(null)");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 3. Partial-update PUT semantics (ADR-0141's own explicit design
	 * note, mirroring daemon-config): setting one field must not touch
	 * the others. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/system/backup-config", "{\"enabled\":true}", &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: PUT backup-config (enabled only), status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/backup-config", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET backup-config after partial updates, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jenabled = json_object_get(r.json, "enabled");

		if (jenabled == NULL || jenabled->type != JSON_BOOL || !jenabled->u.boolean) {
			fprintf(stderr, "FAIL: enabled should be true after the second partial PUT\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/*
	 * 4. ADR-0257: the removed field is REFUSED, not ignored. A client
	 * still sending it would otherwise believe it had set a schedule.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/system/backup-config", "{\"interval_hours\":6}", &r) !=
	            0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: PUT backup-config with the removed interval_hours expected 400, "
		                "got %d\n",
		        r.status);
		ok = 0;
	} else if (r.body == NULL || strstr(r.body, "schedules") == NULL) {
		fprintf(stderr, "FAIL: the refusal did not name /v1/schedules as the replacement\n");
		ok = 0;
	}
	cix_response_free(&r);

	/* Find a real, non-OS disk to exercise the role/mount-state
	 * validation paths against -- adapts to whatever hardware is
	 * actually present. Every step below (diskrole create/rm, PUT
	 * backup-config) is non-destructive. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/disks", NULL, &r) == 0 && r.status == 200) {
		const struct json_value *disks = json_object_get(r.json, "disks");
		size_t i;

		if (disks != NULL && disks->type == JSON_ARRAY) {
			for (i = 0; i < disks->u.array.count && non_os_disk[0] == '\0'; i++) {
				const struct json_value *d = disks->u.array.items[i];
				const struct json_value *jos = json_object_get(d, "is_os_disk");

				if (jos != NULL && jos->type == JSON_BOOL && !jos->u.boolean) {
					snprintf(non_os_disk, sizeof(non_os_disk), "%s",
					         json_str_field(d, "name"));
				}
			}
		}
	}
	cix_response_free(&r);

	if (non_os_disk[0] == '\0') {
		printf("(no non-OS disk discovered on this host -- scenarios 5-7 skipped)\n");
	} else {
		char body[128];

		/* 5. Configuring a disk with the WRONG role -> snapshot-now
		 * fails with a clear, distinct reason. */
		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"container-storage\"}",
		         non_os_disk);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/diskroles (container-storage), status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk\":\"%s\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/backup-config", body, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: PUT backup-config with a wrong-role disk, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/system/backup-config/snapshot-now", NULL, &r) !=
		            0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: POST snapshot-now (wrong role), status=%d\n", r.status);
			ok = 0;
		} else {
			const char *error = json_str_field(r.json, "error");

			if (error == NULL || strstr(error, "backup role") == NULL) {
				fprintf(stderr,
				        "FAIL: snapshot-now against a wrong-role disk should mention "
				        "the backup role, got: %s\n",
				        error != NULL ? error : "(null)");
				ok = 0;
			}
		}
		cix_response_free(&r);

		/* Clear backup-config's own disk pointer before removing the
		 * role below -- otherwise the 409 safety check (this disk is
		 * the *configured* backup-config disk, regardless of whether
		 * its role actually still matches) correctly, but inconveniently
		 * for this test's own flow, refuses the removal. Real, working-
		 * as-designed behavior -- exercised deliberately in scenario 7
		 * below, not a bug to route around silently here. */
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "PUT", "/v1/system/backup-config", "{\"disk\":null}", &r);
		cix_response_free(&r);

		{
			char path[96];

			snprintf(path, sizeof(path), "/v1/diskroles/%s", non_os_disk);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", path, NULL, &r);
			cix_response_free(&r);
		}

		/* 6. The right role (backup) but not currently mounted (never
		 * formatted in this test, the destructive step this sandbox
		 * can't safely exercise) -> a distinct, clear reason. */
		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"backup\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/diskroles (backup), status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk\":\"%s\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/backup-config", body, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: PUT backup-config with the now-correct-role disk, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/system/backup-config/snapshot-now", NULL, &r) !=
		            0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: POST snapshot-now (role-correct, unmounted), status=%d\n",
			        r.status);
			ok = 0;
		} else {
			const char *error = json_str_field(r.json, "error");

			if (error == NULL || strstr(error, "not currently mounted") == NULL) {
				fprintf(stderr,
				        "FAIL: snapshot-now against an unmounted disk should mention "
				        "that, got: %s\n",
				        error != NULL ? error : "(null)");
				ok = 0;
			}
		}
		cix_response_free(&r);

		/* 7. The 409 safety check: this disk is now the configured
		 * backup-config disk -- removing its role must be refused. */
		{
			char path[96];

			snprintf(path, sizeof(path), "/v1/diskroles/%s", non_os_disk);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 409) {
				fprintf(stderr,
				        "FAIL: DELETE diskrole for the active backup-config disk "
				        "expected 409, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);
		}

		/* Cleanup: clear the backup-config disk first (via PUT
		 * disk:null), then the role removal above is unblocked. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/backup-config", "{\"disk\":null}", &r) !=
		            0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: PUT backup-config disk:null (cleanup), status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		{
			char path[96];

			snprintf(path, sizeof(path), "/v1/diskroles/%s", non_os_disk);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 204) {
				fprintf(stderr,
				        "FAIL: DELETE diskrole after clearing backup-config expected "
				        "204, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);
		}
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "BACKUP CONFIG RESULT: PASS\n" : "BACKUP CONFIG RESULT: FAIL\n");
	return ok ? 0 : 1;
}
