/*
 * test_direct_rootfs -- the ADR-0207 phase 2 direct-rootfs container
 * path, exercised end to end under --test-direct-rootfs.
 *
 * On btrfs a container's rootfs is a writable snapshot of its image's
 * subvolume and there is no overlay. This sandbox is ext4 with no loop
 * devices (documented in CLAUDE.md), so the snapshot ioctl itself
 * cannot run here -- what CAN run, and what this proves, is every
 * other part of the direct machinery, driven through the copy variant
 * the test flag forces:
 *
 *   - provisioning: the rootfs copy exists, the overlay dirs never do
 *   - staging: files[] land in the rootfs, not a dead upperdir
 *   - the child self-binds and pivots into the rootfs (the container
 *     RUNS, and reads the staged file from inside)
 *   - live writes land in the rootfs and persist across stop
 *   - stopped-file reads/writes take the rootfs branch of
 *     container_writable_path(), not the upper branch
 *   - restart REUSES the rootfs (state survives; a re-provision would
 *     silently discard it -- the exact failure the mode marker exists
 *     to prevent)
 *   - the image itself is never touched by container writes
 *   - deletion removes the whole container base
 *
 * The one thing left for the real btrfs host (.95) is SNAP_CREATE_V2
 * versus this test's copy -- everything downstream of provisioning is
 * identical by construction.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7639
#define PORT_ARG "--port=7639"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

static int failures;

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

#define CHECK(cond, ...)                                                                            \
	do {                                                                                       \
		if (!(cond)) {                                                                      \
			fprintf(stderr, "FAIL: ");                                                 \
			fprintf(stderr, __VA_ARGS__);                                              \
			fprintf(stderr, "\n");                                                     \
			failures++;                                                                \
		}                                                                                  \
	} while (0)

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

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[5];
	static char data_dir_arg[PATH_MAX + 12];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = "--test-direct-rootfs";
	dargv[4] = NULL;

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

static int path_exists(const char *fmt, const char *a, const char *b)
{
	char p[PATH_MAX];
	struct stat st;

	snprintf(p, sizeof(p), fmt, a, b);
	return stat(p, &st) == 0;
}

/* GET .../files and compare the body to `want`; -1 on status!=200. */
static int get_file_is(const struct cix_client *c, const char *url, const char *want)
{
	struct cix_response r;
	int ok;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", url, NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "  [get_file_is %s -> status=%d body=%.120s]\n", url, r.status,
		        r.body != NULL ? r.body : "");
		cix_response_free(&r);
		return -1;
	}
	ok = r.body != NULL && strcmp(r.body, want) == 0;
	if (!ok)
		fprintf(stderr, "  [get_file_is %s -> content=%.120s]\n", url,
		        r.body != NULL ? r.body : "(null)");
	cix_response_free(&r);
	return ok ? 0 : -2;
}

int main(void)
{
	struct cix_client client;
	struct cix_response r;
	pid_t daemon_pid;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/dtest/v1/rootfs",
	         g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/dtest", g_data_dir);
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
	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/*
	 * 0. ADR-0207 phase 3 plumbing (the kernel-side userns itself
	 * cannot run in this sandbox -- its LSM forbids the uid_map write
	 * -- so what is assertable here is that the DEFAULT machinery
	 * plumbs end to end; the secure default engaging for real is .95's
	 * verification):
	 *   - the fixture-seeded config reads back userns_default=false
	 *   - PUT flips it and it persists through the config channel
	 *   - a container created under default-off reports userns:false
	 */
	{
		struct cix_response cr;

		memset(&cr, 0, sizeof(cr));
		if (cix_client_request(&client, "GET", "/v1/system/daemon-config", NULL, &cr) == 0 &&
		    cr.json != NULL) {
			const struct json_value *jud = json_object_get(cr.json, "userns_default");

			CHECK(jud != NULL && jud->type == JSON_BOOL && !jud->u.boolean,
			      "the fixture-seeded userns_default=false should read back");
		} else {
			CHECK(0, "GET daemon-config failed");
		}
		cix_response_free(&cr);

		memset(&cr, 0, sizeof(cr));
		if (cix_client_request(&client, "PUT", "/v1/system/daemon-config",
		                       "{\"userns_default\": true}", &cr) != 0 || cr.status != 200) {
			CHECK(0, "PUT userns_default=true failed (status=%d)", cr.status);
		}
		cix_response_free(&cr);
		memset(&cr, 0, sizeof(cr));
		if (cix_client_request(&client, "GET", "/v1/system/daemon-config", NULL, &cr) == 0 &&
		    cr.json != NULL) {
			const struct json_value *jud = json_object_get(cr.json, "userns_default");

			CHECK(jud != NULL && jud->type == JSON_BOOL && jud->u.boolean,
			      "userns_default should flip to true via the API");
		}
		cix_response_free(&cr);
		/* back to off -- this sandbox cannot run a userns container */
		memset(&cr, 0, sizeof(cr));
		cix_client_request(&client, "PUT", "/v1/system/daemon-config",
		                   "{\"userns_default\": false}", &cr);
		cix_response_free(&cr);
	}

	/* 1. Create with a files[] entry. Under --test-direct-rootfs this
	 * must provision <base>/rootfs and stage the file INTO it. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"dt\",\"image\":\"dtest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"],"
	                       "\"restart\":\"no\","
	                       "\"files\":[{\"path\":\"/etc/seed.conf\","
	                       "\"content\":\"seeded\\n\"}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create dt, status=%d body=%s\n", r.status,
		        r.body != NULL ? r.body : "");
		failures++;
	}
	cix_response_free(&r);

	{
		struct cix_response cr;

		memset(&cr, 0, sizeof(cr));
		if (cix_client_request(&client, "GET", "/v1/containers/dt", NULL, &cr) == 0 &&
		    cr.json != NULL) {
			const struct json_value *ju = json_object_get(cr.json, "userns");

			CHECK(ju != NULL && ju->type == JSON_BOOL && !ju->u.boolean,
			      "a container created under default-off must report userns:false");
		}
		cix_response_free(&cr);
	}
	{
		char base[PATH_MAX];

		snprintf(base, sizeof(base), "%s/containers/dt", g_data_dir);
		CHECK(path_exists("%s%s", base, "/rootfs/bin/daemon_child"),
		      "the rootfs copy should carry the image's own binary");
		CHECK(path_exists("%s%s", base, "/rootfs/etc/seed.conf"),
		      "files[] must be staged into the ROOTFS -- a file staged into a dead "
		      "upperdir is the exact integration bug this asserts against");
		CHECK(!path_exists("%s%s", base, "/upper"),
		      "no overlay upperdir may exist for a direct-mode container");
		CHECK(!path_exists("%s%s", base, "/merged"),
		      "no overlay merged dir may exist for a direct-mode container");
	}

	/* 2. The container actually runs and sees the staged file --
	 * proving the self-bind + pivot into the rootfs worked. */
	CHECK(get_file_is(&client, "/v1/containers/dt/files?path=%2Fetc%2Fseed.conf",
	                  "seeded\n") == 0,
	      "the running container should read the staged file through /proc/<pid>/root");

	/* 3. A live write lands in the container's rootfs. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/containers/dt/files?path=%2Fetc%2Fstate.conf",
	                       "{\"content\":\"written-live\\n\"}", &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: PUT live state.conf, status=%d\n", r.status);
		failures++;
	}
	cix_response_free(&r);

	/* ...and never leaks into the IMAGE: the copy is independent. */
	CHECK(!path_exists("%s%s", g_image_root, "/etc/state.conf"),
	      "a container write must never appear in the image rootfs");

	/*
	 * 4. Restart reuse: stop, then start, then read the live-written
	 * state back from the RUNNING container. If the restart had
	 * re-provisioned the rootfs instead of reusing it, state.conf
	 * would be gone -- the exact silent data loss the on-disk mode
	 * marker exists to prevent. (The files API on a CLEANLY stopped
	 * container is deliberately not asserted here: a clean stop
	 * removes the registry entry asynchronously, so those calls are
	 * a race against teardown -- a separate, pre-existing gap, filed
	 * as its own issue, not a phase-2 concern.)
	 */
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "POST", "/v1/containers/dt/stop", NULL, &r);
	cix_response_free(&r);

	/*
	 * The mode-flap timebomb, directly: flip the platform default to
	 * userns=ON while dt is stopped. Its revival replays the persisted
	 * body, into which creation PINNED "userns":false (image_version-
	 * style) -- so the restart below must come back non-userns and
	 * healthy. If the pin regressed, the replay would consult the new
	 * default, attempt CLONE_NEWUSER, and die on this sandbox's own
	 * LSM -- every assertion after this line would fail, loudly.
	 */
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "PUT", "/v1/system/daemon-config",
	                   "{\"userns_default\": true}", &r);
	cix_response_free(&r);
	{
		int i;

		/* wait until the stop has fully settled (start would 409 while
		 * teardown is in flight) */
		for (i = 0; i < 50; i++) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/containers/dt/start", NULL, &r) == 0 &&
			    r.status == 200) {
				cix_response_free(&r);
				break;
			}
			cix_response_free(&r);
			usleep(200000);
		}
		CHECK(i < 50, "start after stop never succeeded");
	}
	CHECK(get_file_is(&client, "/v1/containers/dt/files?path=%2Fetc%2Fstate.conf",
	                  "written-live\n") == 0,
	      "state written before the restart must survive it (rootfs reused, not rebuilt)");
	CHECK(get_file_is(&client, "/v1/containers/dt/files?path=%2Fetc%2Fseed.conf",
	                  "seeded\n") == 0,
	      "the staged file must also survive the restart");
	{
		struct cix_response cr;

		memset(&cr, 0, sizeof(cr));
		if (cix_client_request(&client, "GET", "/v1/containers/dt", NULL, &cr) == 0 &&
		    cr.json != NULL) {
			const struct json_value *ju = json_object_get(cr.json, "userns");

			CHECK(ju != NULL && ju->type == JSON_BOOL && !ju->u.boolean,
			      "dt's isolation mode must be PINNED at creation -- a default flip while "
			      "it was stopped must not flap it to userns on revival");
		}
		cix_response_free(&cr);
	}
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "PUT", "/v1/system/daemon-config",
	                   "{\"userns_default\": false}", &r);
	cix_response_free(&r);

	/*
	 * 5. The exited-but-registered case, deterministically: a container
	 * whose process exits on its own keeps its registry entry
	 * (registry_mark_exited), and its files are then served from disk
	 * -- which for a direct-mode container must be the ROOTFS branch of
	 * container_writable_path(), for both reads and writes. This is the
	 * same pattern the existing suite uses for the overlay upper case.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"dt2\",\"image\":\"dtest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"],"
	                       "\"restart\":\"no\","
	                       "\"files\":[{\"path\":\"/etc/seed2.conf\","
	                       "\"content\":\"seeded2\\n\"}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create dt2, status=%d\n", r.status);
		failures++;
	}
	cix_response_free(&r);
	{
		int i;

		for (i = 0; i < 50; i++) {
			const char *st_field = NULL;

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/containers/dt2", NULL, &r) == 0 &&
			    r.json != NULL)
				st_field = json_str_field(r.json, "status");
			if (st_field != NULL && strcmp(st_field, "exited") == 0) {
				cix_response_free(&r);
				break;
			}
			cix_response_free(&r);
			usleep(200000);
		}
		CHECK(i < 50, "dt2 never reached exited");
	}
	CHECK(get_file_is(&client, "/v1/containers/dt2/files?path=%2Fetc%2Fseed2.conf",
	                  "seeded2\n") == 0,
	      "an exited direct-mode container's files are read from its rootfs");
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/containers/dt2/files?path=%2Fetc%2Fcold.conf",
	                       "{\"content\":\"written-cold\\n\"}", &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: PUT on exited dt2, status=%d\n", r.status);
		failures++;
	}
	cix_response_free(&r);
	{
		char p2[PATH_MAX];
		struct stat pst;

		snprintf(p2, sizeof(p2), "%s/containers/dt2/rootfs/etc/cold.conf", g_data_dir);
		CHECK(stat(p2, &pst) == 0,
		      "the exited-container write must land in the ROOTFS, not a dead upperdir");
	}
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/dt2", NULL, &r);
	cix_response_free(&r);

	/* 6. Delete removes the whole base, rootfs included. */
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/dt", NULL, &r);
	cix_response_free(&r);
	{
		int i;
		char base[PATH_MAX];
		struct stat st;

		snprintf(base, sizeof(base), "%s/containers/dt", g_data_dir);
		/* deletion completes asynchronously once the child is reaped */
		for (i = 0; i < 50 && stat(base, &st) == 0; i++)
			usleep(100000);
		CHECK(stat(base, &st) != 0, "the container base should be gone after delete");
	}

	/*
	 * 7. The UPGRADE migration (containerdef.c's
	 * migrate_pin_userns_absent): a definition persisted before the
	 * phase-3 default flip has no "userns" key at all -- replaying it
	 * against a userns_default:true platform would silently flap the
	 * container to userns on its first post-upgrade revival. The
	 * migration pins "userns":false into such a definition at load.
	 * Simulated the only honest way: create a def, stop the daemon,
	 * strip the pinned key from the persisted file (exactly what a
	 * pre-upgrade file looks like), set the default to true, restart.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"dt3\",\"image\":\"dtest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"],"
	                       "\"restart\":\"no\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create dt3, status=%d\n", r.status);
		failures++;
	}
	cix_response_free(&r);
	kill(daemon_pid, SIGTERM);
	{
		int status;

		if (waitpid(daemon_pid, &status, 0) != daemon_pid) {
			fprintf(stderr, "FAIL: daemon did not exit for the migration restart\n");
			failures++;
		}
	}
	{
		char defs_path[PATH_MAX], cfg_path[PATH_MAX];
		FILE *f;
		static char defs[65536];
		size_t n = 0;
		char *hit;
		/* the creation-time pin, as it appears escaped inside the
		 * stored body string */
		static const char pin[] = ",\\\"userns\\\":false";

		snprintf(defs_path, sizeof(defs_path), "%s/state/container_defs.json", g_data_dir);
		f = fopen(defs_path, "r");
		if (f != NULL) {
			n = fread(defs, 1, sizeof(defs) - 1, f);
			fclose(f);
		}
		defs[n] = '\0';
		hit = strstr(defs, pin);
		CHECK(hit != NULL, "dt3's persisted body must carry the creation-time pin");
		if (hit != NULL) {
			/* excise the pin -- the file is now byte-for-byte what a
			 * pre-phase-3 daemon would have written */
			memmove(hit, hit + sizeof(pin) - 1, n - (size_t)(hit - defs) - (sizeof(pin) - 1) + 1);
			f = fopen(defs_path, "w");
			if (f != NULL) {
				fputs(defs, f);
				fclose(f);
			}
		}
		snprintf(cfg_path, sizeof(cfg_path), "%s/state/daemon_config.json", g_data_dir);
		f = fopen(cfg_path, "w");
		if (f != NULL) {
			fputs("{\"userns_default\": true}", f);
			fclose(f);
		}
	}
	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (wait_for_daemon(&client, 100) != 0) {
		fprintf(stderr, "FAIL: daemon did not come back for the migration check\n");
		failures++;
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers/dt3/start", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: start dt3 after migration, status=%d body=%.120s\n", r.status,
		        r.body != NULL ? r.body : "");
		failures++;
	}
	cix_response_free(&r);
	{
		int i;
		const struct json_value *ju = NULL;

		for (i = 0; i < 50; i++) {
			const char *st_field = NULL;

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/containers/dt3", NULL, &r) == 0 &&
			    r.json != NULL)
				st_field = json_str_field(r.json, "status");
			if (st_field != NULL &&
			    (strcmp(st_field, "exited") == 0 || strcmp(st_field, "running") == 0)) {
				ju = json_object_get(r.json, "userns");
				CHECK(ju != NULL && ju->type == JSON_BOOL && !ju->u.boolean,
				      "a pre-flip definition must be MIGRATED to userns:false at load -- "
				      "not resolved against the new default on revival");
				cix_response_free(&r);
				break;
			}
			cix_response_free(&r);
			usleep(200000);
		}
		CHECK(i < 50, "dt3 never came up after the migration restart");
	}
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/dt3", NULL, &r);
	cix_response_free(&r);

	kill(daemon_pid, SIGTERM);
	{
		int status;
		pid_t w = waitpid(daemon_pid, &status, 0);

		if (w != daemon_pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
			failures++;
		}
	}
	test_data_dir_cleanup(g_data_dir);

	printf(failures == 0 ? "DIRECT ROOTFS RESULT: PASS\n"
	                     : "DIRECT ROOTFS RESULT: FAIL (%d)\n",
	       failures);
	return failures == 0 ? 0 : 1;
}
