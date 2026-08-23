/*
 * ADR-0142 Section 4 end-to-end test: container-storage migration --
 * GET/POST /v1/containers/{name}/migrate-storage
 * (daemon/src/containerstoragemigrate.c + main.c's own finalize_
 * container_storage_migration()).
 *
 * Same honest testability boundary test_storage_placement.c's own top
 * comment already documents: the real, successful migration path --
 * a role-assigned disk actually mounted, the container really stopped,
 * its overlay really copied and cut over, really restarted on the new
 * disk -- needs a real, disposable block device to format, which this
 * dev sandbox's own real host hardware can never safely do in an
 * automated test. What IS fully, safely testable here -- and is --
 * is everything up to that point: the REST shape, every validation/
 * rejection path (missing container, no persisted definition, missing/
 * invalid disk field, unknown disk, wrong role, present-but-unmounted,
 * already-active), none of which require ever formatting anything.
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

#define TEST_PORT 7660
#define PORT_ARG "--port=7660"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static int wait_for_daemon(const struct thinc_client *c, int max_attempts)
{
	int i;
	struct thinc_response r;

	for (i = 0; i < max_attempts; i++) {
		if (thinc_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			thinc_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
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
	struct thinc_client client;
	int ok = 1;
	struct thinc_response r;
	char non_os_disk[64] = "";

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/migtest/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/migtest", g_data_dir);
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

	thinc_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. GET migrate-storage-status for a container that has never
	 * existed at all -> 404, not a synthesized "none" (a per-container
	 * resource, unlike the daemon-wide storage kinds -- matches
	 * GET .../stats's own existence-check convention). */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/containers/nosuchcontainer/migrate-storage", NULL, &r) !=
	            0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: GET migrate-storage for nonexistent container expected 404, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 2. POST migrate-storage for a nonexistent container -> 404. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers/nosuchcontainer/migrate-storage",
	                       "{\"disk\":null}", &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: POST migrate-storage for nonexistent container expected 404, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/*
	 * 3. ADR-0181 (issue #73) changed what a restart:"no" container means
	 * here. This scenario used to assert a 400 ("nothing to replay from"),
	 * because a "no" container had no persisted definition at all. Now
	 * EVERY container is persisted, so that branch is unreachable and a
	 * "no" container is genuinely migratable -- the honest thing to assert
	 * is the behavior that remains real: asking to migrate it to the
	 * placement it is ALREADY on is a 409, exactly as it is for any other
	 * restart policy. (The daemon's own no-definition 400 branch is kept
	 * as defence in depth for a genuinely def-less internal container.)
	 */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"noreplay\",\"image\":\"migtest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST noreplay, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers/noreplay/migrate-storage", "{\"disk\":null}",
	                       &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr,
		        "FAIL: POST migrate-storage to the placement it's already on expected 409, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/containers/noreplay", NULL, &r);
	thinc_response_free(&r);

	/* 4. A real restart:"always" container for the rest of this test. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"migc\",\"image\":\"migtest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"],"
	                       "\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST migc, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 5. Fresh container, never migrated: status is state:none. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/containers/migc/migrate-storage", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET migc migrate-storage, status=%d\n", r.status);
		ok = 0;
	} else if (!str_eq(json_str_field(r.json, "state"), "none")) {
		fprintf(stderr, "FAIL: fresh container migrate status should be state:none, got %s\n",
		        json_str_field(r.json, "state"));
		ok = 0;
	}
	thinc_response_free(&r);

	/* 6. A body missing "disk" entirely -> 400. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers/migc/migrate-storage", "{}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST migc migrate-storage with no disk field expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 7. disk:null while already on the default placement -> 409
	 * (nothing to do -- the same "already active" rejection every
	 * other storage-placement kind has). */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers/migc/migrate-storage", "{\"disk\":null}",
	                       &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr,
		        "FAIL: POST migc migrate-storage disk:null while already default expected "
		        "409, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 8. An unknown disk name -> 400 (resolve_container_disk_root's own
	 * DISK_RESOLVE_NOT_FOUND, "no such disk"). */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers/migc/migrate-storage",
	                       "{\"disk\":\"nonexistentdisk99\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST migc migrate-storage unknown disk expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* Find a real, non-OS disk on this host to exercise the role/mount-
	 * state validation paths against -- every diskrole create/rm step
	 * below is non-destructive, the same precedent test_storage_
	 * placement.c already established. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/disks", NULL, &r) == 0 && r.status == 200) {
		const struct json_value *disks = json_object_get(r.json, "disks");
		size_t i;

		if (disks != NULL && disks->type == JSON_ARRAY) {
			for (i = 0; i < disks->u.array.count && non_os_disk[0] == '\0'; i++) {
				const struct json_value *d = disks->u.array.items[i];
				const struct json_value *jos = json_object_get(d, "is_os_disk");

				if (jos != NULL && jos->type == JSON_BOOL && !jos->u.boolean)
					snprintf(non_os_disk, sizeof(non_os_disk), "%s", json_str_field(d, "name"));
			}
		}
	}
	thinc_response_free(&r);

	if (non_os_disk[0] == '\0') {
		printf("(no non-OS disk discovered on this host -- scenarios 9-10 skipped)\n");
	} else {
		char body[128];

		/* 9. Right role assigned elsewhere (state-storage, not
		 * container-storage) -> 400 "disk has no container-storage role
		 * assigned". */
		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"state-storage\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/diskroles (state-storage), status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk\":\"%s\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers/migc/migrate-storage", body, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: POST migc migrate-storage to a wrong-role disk expected 400, "
			                "got %d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		{
			char path[96];

			snprintf(path, sizeof(path), "/v1/diskroles/%s", non_os_disk);
			memset(&r, 0, sizeof(r));
			thinc_client_request(&client, "DELETE", path, NULL, &r);
			thinc_response_free(&r);
		}

		/* 10. The right role (container-storage) but never actually
		 * formatted/mounted in this test -> 400 "disk is not mounted",
		 * not silently treated as success. */
		snprintf(body, sizeof(body), "{\"disk_name\":\"%s\",\"role\":\"container-storage\"}",
		         non_os_disk);
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/diskroles", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/diskroles (container-storage), status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		snprintf(body, sizeof(body), "{\"disk\":\"%s\"}", non_os_disk);
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers/migc/migrate-storage", body, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr,
			        "FAIL: POST migc migrate-storage to a role-correct but unmounted disk "
			        "expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* Status is still state:none -- no job was ever actually
		 * started by any of the rejected attempts above. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/containers/migc/migrate-storage", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET migc migrate-storage (post-rejections), status=%d\n",
			        r.status);
			ok = 0;
		} else if (!str_eq(json_str_field(r.json, "state"), "none")) {
			fprintf(stderr,
			        "FAIL: migrate status should still be state:none (no job ever actually "
			        "started), got %s\n",
			        json_str_field(r.json, "state"));
			ok = 0;
		}
		thinc_response_free(&r);

		{
			char path[96];

			snprintf(path, sizeof(path), "/v1/diskroles/%s", non_os_disk);
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 204) {
				fprintf(stderr,
				        "FAIL: DELETE diskrole for a never-active disk expected 204, got "
				        "%d\n",
				        r.status);
				ok = 0;
			}
			thinc_response_free(&r);
		}
	}

	/* Cleanup. */
	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/containers/migc", NULL, &r);
	thinc_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "CONTAINER STORAGE MIGRATE RESULT: PASS\n" : "CONTAINER STORAGE MIGRATE RESULT: FAIL\n");
	return ok ? 0 : 1;
}
