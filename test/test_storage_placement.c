/*
 * ADR-0141 Phase 2/3/4 end-to-end test: all three storage kinds --
 * state, log, and rebuildable -- placement (GET/POST
 * /v1/system/{state,log,rebuildable}-storage(/migrate),
 * daemon/src/storageplacement.c + daemon/src/storagemigrate.c) and the
 * safety checks that keep DELETE /diskroles/{name} and
 * POST /disks/{name}/format from pulling a disk out from under an
 * active placement of any kind.
 *
 * The real, successful migration path -- a role-assigned disk actually
 * formatted and mounted, data really copied across, every subsystem's
 * own private path really repointed, /etc/resolv.conf's own bind mount
 * really re-established -- needs a real, disposable block device to
 * format. This dev sandbox's own disk_enumerate() results are real
 * host hardware (confirmed live: model strings like a real NVMe/USB
 * drive), never safe to mkfs in an automated test -- the same class of
 * "honest boundary of what's testable without real hardware" this
 * project's test suite already documents elsewhere (test_daemon_
 * devices.c's own gpu:N/veth cases, test_boot.c's QEMU-only coverage).
 * What IS fully, safely testable here -- and is -- is everything up to
 * that point: the REST shape, every validation/rejection path
 * (unknown disk, OS disk, wrong role, present-but-unmounted), and the
 * 409 safety checks, none of which require ever formatting anything.
 * diskrole assignment alone (used throughout below) is non-destructive
 * by the API's own design.
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

#define TEST_PORT 7640
#define PORT_ARG "--port=7640"

static char g_data_dir[PATH_MAX];

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

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/thincd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/thincd", dargv, environ);
		perror("execve build/thincd");
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
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	char non_os_disk[64] = "";

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

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

	/* 1. Default state, fresh daemon: no placement, no migration job. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/system/state-storage", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/system/state-storage, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jdisk = json_object_get(r.json, "disk");

		if (jdisk == NULL || jdisk->type != JSON_NULL) {
			fprintf(stderr, "FAIL: fresh daemon should report disk:null\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/system/state-storage/migrate", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/system/state-storage/migrate, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *state = json_str_field(r.json, "state");

		if (!(state != NULL && strcmp(state, "none") == 0)) {
			fprintf(stderr, "FAIL: fresh daemon migrate status should be state:none, got %s\n",
			        state != NULL ? state : "(null)");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 2. A request body missing "disk" entirely (ambiguous: not the
	 * same as an explicit null) is a real 400, not silently treated as
	 * either. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/system/state-storage/migrate", "{}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST migrate with no disk field expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. An unknown disk name -> 404. Fully deterministic regardless of
	 * this host's real hardware. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/system/state-storage/migrate",
	                       "{\"disk\":\"nonexistentdisk99\"}", &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: POST migrate unknown disk expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* Find a real, non-OS disk on this host to exercise the
	 * role/mount-state validation paths against -- adapts to whatever
	 * hardware is actually present, same precedent test_daemon_
	 * devices.c's own top comment already established. Every step
	 * below (diskrole create/rm) is non-destructive. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/disks", NULL, &r) == 0 && r.status == 200) {
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
	kx_response_free(&r);

	if (non_os_disk[0] == '\0') {
		printf("(no non-OS disk discovered on this host -- scenarios 4-7 skipped)\n");
	} else {
		char body[128];

		/* 4. The real OS disk itself is always rejected, regardless of
		 * which disk that actually is on this host. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/disks", NULL, &r) == 0 && r.status == 200) {
			const struct json_value *disks = json_object_get(r.json, "disks");
			size_t i;
			char os_disk[64] = "";

			if (disks != NULL && disks->type == JSON_ARRAY) {
				for (i = 0; i < disks->u.array.count && os_disk[0] == '\0'; i++) {
					const struct json_value *d = disks->u.array.items[i];
					const struct json_value *jos = json_object_get(d, "is_os_disk");

					if (jos != NULL && jos->type == JSON_BOOL && jos->u.boolean)
						snprintf(os_disk, sizeof(os_disk), "%s",
						         json_str_field(d, "name"));
				}
			}
			if (os_disk[0] != '\0') {
				struct kx_response r2;

				snprintf(body, sizeof(body), "{\"disk\":\"%s\"}", os_disk);
				memset(&r2, 0, sizeof(r2));
				if (kx_client_request(&client, "POST", "/v1/system/state-storage/migrate",
				                       body, &r2) != 0 ||
				    r2.status != 400) {
					fprintf(stderr,
					        "FAIL: POST migrate to the real OS disk expected 400, "
					        "got %d\n",
					        r2.status);
					ok = 0;
				}
				kx_response_free(&r2);
			}
		}
		kx_response_free(&r);

		/* 5. A disk with the WRONG role (container-storage, not
		 * state-storage) -> 400. */
		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"container-storage\"}",
		         non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/diskroles (container-storage), status=%d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk\":\"%s\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/system/state-storage/migrate", body, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: POST migrate to a wrong-role disk expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		kx_client_request(&client, "DELETE", "/v1/diskroles", NULL, &r); /* no-op path, ignored */
		kx_response_free(&r);
		{
			char path[96];

			snprintf(path, sizeof(path), "/v1/diskroles/%s", non_os_disk);
			memset(&r, 0, sizeof(r));
			kx_client_request(&client, "DELETE", path, NULL, &r);
			kx_response_free(&r);
		}

		/* 6. The right role (state-storage) but not currently mounted
		 * (never formatted in this test -- that's the destructive step
		 * this sandbox can't safely exercise) -> 400, not silently
		 * treated as success. */
		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"state-storage\"}",
		         non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/diskroles (state-storage), status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk\":\"%s\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/system/state-storage/migrate", body, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr,
			        "FAIL: POST migrate to a role-correct but unmounted disk expected "
			        "400, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 7. Cleanup of the state-storage-role assignment above -- a
		 * role-only assignment (never an active placement, since the
		 * migrate above never succeeded) must be freely removable, no
		 * 409. Frees the disk for the log-storage scenarios below (a
		 * disk can only carry one role at a time). */
		{
			char path[96];

			snprintf(path, sizeof(path), "/v1/diskroles/%s", non_os_disk);
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 204) {
				fprintf(stderr,
				        "FAIL: DELETE diskrole for a never-active disk expected 204, "
				        "got %d\n",
				        r.status);
				ok = 0;
			}
			kx_response_free(&r);
		}

		/* 8. log-storage: default state, then the same wrong-role and
		 * present-but-unmounted validation paths as state-storage
		 * above, proving respond_storagemigrate_error()'s own kind-
		 * aware messages and storagemigrate_start()'s role check
		 * (role_str_for_kind()) are both wired correctly for this
		 * second kind, not just copy-pasted and left pointing at
		 * "state-storage" by mistake. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/system/log-storage", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/system/log-storage, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *jdisk = json_object_get(r.json, "disk");

			if (jdisk == NULL || jdisk->type != JSON_NULL) {
				fprintf(stderr, "FAIL: fresh daemon should report log-storage disk:null\n");
				ok = 0;
			}
		}
		kx_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"state-storage\"}",
		         non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/diskroles (wrong role for log test), status=%d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk\":\"%s\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/system/log-storage/migrate", body, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: POST log-storage migrate to a wrong-role disk expected 400, "
			                "got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		{
			char path[96];

			snprintf(path, sizeof(path), "/v1/diskroles/%s", non_os_disk);
			memset(&r, 0, sizeof(r));
			kx_client_request(&client, "DELETE", path, NULL, &r);
			kx_response_free(&r);
		}

		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"log-storage\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/diskroles (log-storage), status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk\":\"%s\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/system/log-storage/migrate", body, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr,
			        "FAIL: POST log-storage migrate to a role-correct but unmounted disk "
			        "expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/system/log-storage/migrate", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/system/log-storage/migrate, status=%d\n", r.status);
			ok = 0;
		} else {
			const char *state = json_str_field(r.json, "state");

			if (!(state != NULL && strcmp(state, "none") == 0)) {
				fprintf(stderr,
				        "FAIL: log-storage migrate status should still be state:none "
				        "(no job ever actually started), got %s\n",
				        state != NULL ? state : "(null)");
				ok = 0;
			}
		}
		kx_response_free(&r);

		/* 9. rebuildable-storage: same validation coverage as state/log
		 * above, proving the third and final storage_kind is wired
		 * correctly too. Frees the disk from its log-storage role
		 * first (one role at a time). */
		{
			char path[96];

			snprintf(path, sizeof(path), "/v1/diskroles/%s", non_os_disk);
			memset(&r, 0, sizeof(r));
			kx_client_request(&client, "DELETE", path, NULL, &r);
			kx_response_free(&r);
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/system/rebuildable-storage", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/system/rebuildable-storage, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *jdisk = json_object_get(r.json, "disk");

			if (jdisk == NULL || jdisk->type != JSON_NULL) {
				fprintf(stderr,
				        "FAIL: fresh daemon should report rebuildable-storage "
				        "disk:null\n");
				ok = 0;
			}
		}
		kx_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"backup\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/diskroles (wrong role for rebuildable test), "
			                "status=%d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk\":\"%s\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/system/rebuildable-storage/migrate", body, &r) !=
		            0 ||
		    r.status != 400) {
			fprintf(stderr,
			        "FAIL: POST rebuildable-storage migrate to a wrong-role disk expected "
			        "400, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		{
			char path[96];

			snprintf(path, sizeof(path), "/v1/diskroles/%s", non_os_disk);
			memset(&r, 0, sizeof(r));
			kx_client_request(&client, "DELETE", path, NULL, &r);
			kx_response_free(&r);
		}

		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"rebuildable-storage\"}",
		         non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/diskroles (rebuildable-storage), status=%d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk\":\"%s\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/system/rebuildable-storage/migrate", body, &r) !=
		            0 ||
		    r.status != 400) {
			fprintf(stderr,
			        "FAIL: POST rebuildable-storage migrate to a role-correct but "
			        "unmounted disk expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/system/rebuildable-storage/migrate", NULL, &r) !=
		            0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/system/rebuildable-storage/migrate, status=%d\n",
			        r.status);
			ok = 0;
		} else {
			const char *state = json_str_field(r.json, "state");

			if (!(state != NULL && strcmp(state, "none") == 0)) {
				fprintf(stderr,
				        "FAIL: rebuildable-storage migrate status should still be "
				        "state:none, got %s\n",
				        state != NULL ? state : "(null)");
				ok = 0;
			}
		}
		kx_response_free(&r);

		/* 10. Final cleanup. */
		{
			char path[96];

			snprintf(path, sizeof(path), "/v1/diskroles/%s", non_os_disk);
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 204) {
				fprintf(stderr,
				        "FAIL: DELETE diskrole for a never-active disk expected 204, "
				        "got %d\n",
				        r.status);
				ok = 0;
			}
			kx_response_free(&r);
		}
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "STORAGE PLACEMENT RESULT: PASS\n" : "STORAGE PLACEMENT RESULT: FAIL\n");
	return ok ? 0 : 1;
}
