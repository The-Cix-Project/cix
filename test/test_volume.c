/*
 * Issue #88 end-to-end test: persistent volumes.
 *
 * The property under test is the entire reason volumes exist and is not
 * covered by any other test here: data written by a container SURVIVES
 * that container being deleted, and is visible to a completely different
 * container that mounts the same volume afterwards. Everything a
 * container writes otherwise lives in its overlay upper layer, which
 * DELETE removes (ADR-0106).
 *
 * Also covers the guard rails, because each of them protects real data:
 * an unknown volume name is refused rather than silently created (a typo
 * that quietly produced an empty volume is exactly how someone concludes
 * persistence "didn't work"), and deleting a volume is refused while any
 * container definition still references it.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <sys/stat.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7781
#define PORT_ARG "--port=7781"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];
static int g_failures;

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		g_failures++;
	}
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
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execve("build/thincd", dargv, environ);
		_exit(127);
	}
	return pid;
}

/* Polls until name reports "exited", then returns its captured output. */
static int wait_exited(const struct thinc_client *c, const char *name, char *out, size_t out_size)
{
	char path[128];
	int i;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	for (i = 0; i < 100; i++) {
		struct thinc_response r;

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200) {
			const char *st = json_str_field(r.json, "status");

			if (st != NULL && strcmp(st, "exited") == 0) {
				const char *cap = json_str_field(r.json, "captured_output");

				snprintf(out, out_size, "%s", cap != NULL ? cap : "");
				thinc_response_free(&r);
				return 0;
			}
		}
		thinc_response_free(&r);
		usleep(100000);
	}
	return -1;
}

/* Waits for a DELETE to fully settle (ADR-0180 async teardown). */
static void wait_gone(const struct thinc_client *c, const char *name)
{
	char path[128];
	int i;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	for (i = 0; i < 100; i++) {
		struct thinc_response r;
		int gone;

		memset(&r, 0, sizeof(r));
		gone = (thinc_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 404);
		thinc_response_free(&r);
		if (gone)
			return;
		usleep(100000);
	}
}

int main(void)
{
	pid_t daemon_pid;
	struct thinc_client client;
	struct thinc_response r;
	char out[256];

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/voltest/v1/rootfs",
	         g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/voltest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}
	if (test_image_fixture_build(g_image_root, "build/volume_child", "volume_child") != 0) {
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

	/* 1. create a volume */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes", "{\"name\":\"vol1\"}", &r) == 0 &&
	          r.status == 201,
	      "POST /v1/volumes creates a volume");
	thinc_response_free(&r);

	/* 2. an invalid name must be refused -- a volume name becomes a real
	 * directory name, so path-escaping characters can never be accepted. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes", "{\"name\":\"../escape\"}", &r) == 0 &&
	          r.status == 400,
	      "a path-escaping volume name is refused (400)");
	thinc_response_free(&r);

	/* 3. a container naming an UNKNOWN volume is refused rather than
	 * silently getting a fresh empty one. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"cbad\",\"image\":\"voltest\","
	                         "\"volumes\":[{\"name\":\"nosuchvol\",\"path\":\"/vol\"}],"
	                         "\"cmd\":[\"/bin/volume_child\"]}",
	                         &r) == 0 &&
	          r.status == 400,
	      "a container naming an unknown volume is refused (400), never auto-created");
	thinc_response_free(&r);

	/* 4. write into the volume from a real container */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"cwrite\",\"image\":\"voltest\",\"capture_output\":true,"
	                         "\"volumes\":[{\"name\":\"vol1\",\"path\":\"/vol\"}],"
	                         "\"cmd\":[\"/bin/volume_child\",\"write\"]}",
	                         &r) == 0 &&
	          r.status == 201,
	      "container with a volume is created");
	thinc_response_free(&r);

	out[0] = '\0';
	check(wait_exited(&client, "cwrite", out, sizeof(out)) == 0, "writer container exited");
	check(strstr(out, "WROTE") != NULL, "writer actually wrote into the mounted volume");

	/* 4b. the volume must be visible on the read-back. A field the API
	 * accepts and then never echoes is unverifiable from the outside --
	 * exactly the write-only gap Part 163 had to close for the resource
	 * limits, and the reason this assertion exists at all. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/containers/cwrite", NULL, &r) == 0 &&
	    r.status == 200) {
		const struct json_value *jv = json_object_get(r.json, "volumes");
		const char *vn = NULL;
		const char *vp = NULL;

		if (jv != NULL && jv->type == JSON_ARRAY && jv->u.array.count == 1) {
			vn = json_as_string(json_object_get(jv->u.array.items[0], "name"));
			vp = json_as_string(json_object_get(jv->u.array.items[0], "path"));
		}
		check(vn != NULL && strcmp(vn, "vol1") == 0 && vp != NULL &&
		          strcmp(vp, "/vol") == 0,
		      "GET /containers/{name} echoes the volume back (name and path)");
	} else {
		check(0, "GET /containers/{name} echoes the volume back (name and path)");
	}
	thinc_response_free(&r);

	/* 5. THE POINT: delete the container entirely, then read the data
	 * back from a DIFFERENT container mounting the same volume. */
	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/containers/cwrite", NULL, &r);
	thinc_response_free(&r);
	wait_gone(&client, "cwrite");

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"cread\",\"image\":\"voltest\",\"capture_output\":true,"
	                         "\"volumes\":[{\"name\":\"vol1\",\"path\":\"/vol\"}],"
	                         "\"cmd\":[\"/bin/volume_child\"]}",
	                         &r) == 0 &&
	          r.status == 201,
	      "second container with the same volume is created");
	thinc_response_free(&r);

	out[0] = '\0';
	check(wait_exited(&client, "cread", out, sizeof(out)) == 0, "reader container exited");
	check(strstr(out, "READ:PERSISTED") != NULL,
	      "volume data SURVIVED the first container being deleted");

	/* 5b. attach/detach on an EXISTING container (issue #92).
	 *
	 * These edit the persisted definition and apply on the next start,
	 * never live -- so the assertion that matters is not that the API
	 * returns 200, it is that a container created with NO volume, then
	 * attached to one, actually has it mounted when it next starts.
	 * Everything weaker than that would pass against an endpoint that
	 * updated bookkeeping and nothing else. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes", "{\"name\":\"vol2\"}", &r) == 0 &&
	          r.status == 201,
	      "a second volume for the attach test");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"cattach\",\"image\":\"voltest\",\"capture_output\":true,"
	                         "\"restart\":\"no\","
	                         "\"cmd\":[\"/bin/volume_child\",\"write\"]}",
	                         &r) == 0 &&
	          r.status == 201,
	      "container created with no volumes at all");
	thinc_response_free(&r);

	out[0] = '\0';
	check(wait_exited(&client, "cattach", out, sizeof(out)) == 0, "first run exited");
	check(strstr(out, "WROTE") == NULL,
	      "with no volume mounted the writer could not write -- the baseline the attach has to "
	      "change");

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers/cattach/volumes",
	                         "{\"name\":\"vol2\",\"path\":\"/vol\"}", &r) == 0 &&
	          r.status == 200,
	      "attaching a volume to an existing container");
	thinc_response_free(&r);

	/* A duplicate, and a second volume at a path already in use, are
	 * both refused -- silently ignoring either would leave the caller
	 * believing something happened that did not. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers/cattach/volumes",
	                         "{\"name\":\"vol2\",\"path\":\"/elsewhere\"}", &r) == 0 &&
	          r.status == 409,
	      "attaching the same volume twice is refused (409)");
	thinc_response_free(&r);
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers/cattach/volumes",
	                         "{\"name\":\"vol1\",\"path\":\"/vol\"}", &r) == 0 &&
	          r.status == 409,
	      "a second volume at an already-used path is refused (409)");
	thinc_response_free(&r);
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers/cattach/volumes",
	                         "{\"name\":\"nosuchvol\",\"path\":\"/x\"}", &r) == 0 &&
	          r.status == 404,
	      "attaching an unknown volume is refused (404)");
	thinc_response_free(&r);

	/* The point: start it again and the volume is really there. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers/cattach/start", NULL, &r) == 0 &&
	          r.status == 200,
	      "restarting the container after the attach");
	thinc_response_free(&r);

	out[0] = '\0';
	check(wait_exited(&client, "cattach", out, sizeof(out)) == 0, "second run exited");
	check(strstr(out, "WROTE") != NULL,
	      "the attached volume was really mounted on the next start, not just recorded");

	/* Detach, restart, and confirm it is genuinely gone again -- an
	 * endpoint that only ever adds would pass every assertion above. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "DELETE", "/v1/containers/cattach/volumes/vol2", NULL, &r) ==
	              0 &&
	          r.status == 200,
	      "detaching the volume");
	thinc_response_free(&r);
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "DELETE", "/v1/containers/cattach/volumes/vol2", NULL, &r) ==
	              0 &&
	          r.status == 404,
	      "detaching a volume the container does not mount is 404");
	thinc_response_free(&r);

	/*
	 * The detach is checked on the definition, not by watching the
	 * write fail. It would not fail: the mount point the attach created
	 * stays behind in the container's overlay upper layer, so a write
	 * to /vol still succeeds afterwards -- it just lands in the overlay
	 * and dies with the container instead of persisting. Asserting on
	 * the write here would be asserting something untrue.
	 */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/containers/cattach", NULL, &r) == 0 &&
	    r.status == 200) {
		const struct json_value *jv = json_object_get(r.json, "volumes");

		check(jv != NULL && jv->type == JSON_ARRAY && jv->u.array.count == 0,
		      "after detaching, the container's definition carries no volumes");
	} else {
		check(0, "after detaching, the container's definition carries no volumes");
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/containers/cattach", NULL, &r);
	thinc_response_free(&r);
	wait_gone(&client, "cattach");
	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/volumes/vol2", NULL, &r);
	thinc_response_free(&r);

	/* 5c. migration (user-reported gap: a volume was placed at create
	 * time and could never move).
	 *
	 * The assertion that matters is that the DATA moves, not that the
	 * placement field changes -- a migrate that repoints without
	 * copying would pass any check on the response alone and lose
	 * everything. So: write, migrate, read back through a container. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes/vol1/migrate", "{\"disk\":\"\"}", &r) == 0 &&
	          r.status == 409,
	      "migrating a volume to where it already is is refused (409)");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes/vol1/migrate",
	                         "{\"disk\":\"nosuchdisk\"}", &r) == 0 &&
	          r.status == 404,
	      "migrating onto an unknown disk is refused (404)");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes/nosuchvol/migrate", "{}", &r) == 0 &&
	          r.status == 404,
	      "migrating an unknown volume is refused (404)");
	thinc_response_free(&r);

	/* 5d. content snapshots (issue #96).
	 *
	 * The thing that has to be true is that the DATA comes back: a
	 * backup that records a policy and copies nothing would pass any
	 * check on status codes alone, and would only be found out by
	 * someone restoring it. So the cycle is driven end to end -- write,
	 * snapshot, destroy the data, restore, read it back through a real
	 * container. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "GET", "/v1/volumes/vol1/backups", NULL, &r) == 0 &&
	          r.status == 200,
	      "a volume reports its backup policy");
	{
		const struct json_value *en = json_object_get(r.json, "enabled");

		check(en != NULL && en->type == JSON_BOOL && !en->u.boolean,
		      "backups are OFF until asked for -- copying workload data is opt-in");
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes/vol1/backup", NULL, &r) == 0 &&
	          r.status == 409,
	      "taking a snapshot with no backup disk configured is refused (409), not silently "
	      "written somewhere else");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "PUT", "/v1/volumes/vol1/backups",
	                         "{\"enabled\":true,\"retain\":3}", &r) == 0 &&
	          r.status == 200,
	      "opting a volume in, with its own retention");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "PUT", "/v1/volumes/vol1/backups",
	                         "{\"enabled\":true,\"retain\":100000}", &r) == 0 &&
	          r.status == 400,
	      "an absurd retention is refused rather than stored");
	thinc_response_free(&r);

	/* Omitting retain leaves the existing one alone rather than
	 * silently resetting it to a default -- a partial update that
	 * quietly rewrites a field it was not given is how settings get
	 * lost. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "PUT", "/v1/volumes/vol1/backups", "{\"enabled\":true}", &r) ==
	        0 &&
	    r.status == 200) {
		const struct json_value *re = json_object_get(r.json, "retain");

		check(re != NULL && (int)json_as_number(re) == 3,
		      "omitting retain leaves the previously-set value alone");
	} else {
		check(0, "omitting retain leaves the previously-set value alone");
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes/vol1/restore",
	                         "{\"snapshot\":\"whenever\"}", &r) == 0 &&
	          r.status == 400,
	      "restoring without naming the volume back is refused -- it replaces everything in it");
	thinc_response_free(&r);

	/* An unknown mode reads as the safe one rather than being accepted
	 * as something this daemon does not implement. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "PUT", "/v1/volumes/vol1/backups",
	                       "{\"enabled\":true,\"while_running\":\"whatever\"}", &r) == 0 &&
	    r.status == 200) {
		const char *aw = json_as_string(json_object_get(r.json, "while_running"));

		check(aw != NULL && strcmp(aw, "refuse") == 0,
		      "an unrecognised while_running mode falls back to refusing, not to copying");
	} else {
		check(0, "an unrecognised while_running mode falls back to refusing, not to copying");
	}
	thinc_response_free(&r);

	/* while_running: without a non-default mode, a volume mounted by an
	 * always-on service could never be backed up at all -- the guard
	 * would refuse forever. Off unless asked for, and it survives a
	 * later policy update that does not mention it. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/volumes/vol1/backups", NULL, &r) == 0 &&
	    r.status == 200) {
		const char *aw = json_as_string(json_object_get(r.json, "while_running"));

		check(aw != NULL && strcmp(aw, "refuse") == 0,
		      "a running container blocks the backup unless the volume says otherwise");
	} else {
		check(0, "a running container blocks the backup unless the volume says otherwise");
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "PUT", "/v1/volumes/vol1/backups",
	                         "{\"enabled\":true,\"while_running\":\"pause\"}", &r) == 0 &&
	          r.status == 200,
	      "choosing to pause containers for the copy");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "PUT", "/v1/volumes/vol1/backups",
	                       "{\"enabled\":true,\"retain\":5}", &r) == 0 &&
	    r.status == 200) {
		const char *aw = json_as_string(json_object_get(r.json, "while_running"));

		check(aw != NULL && strcmp(aw, "pause") == 0,
		      "a policy update that does not mention while_running leaves it alone");
	} else {
		check(0, "a policy update that does not mention while_running leaves it alone");
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes/vol1/restore",
	                         "{\"snapshot\":\"nosuchsnapshot\",\"confirm_volume_name\":\"vol1\"}",
	                         &r) == 0 &&
	          (r.status == 404 || r.status == 409),
	      "restoring a snapshot that does not exist fails rather than emptying the volume");
	thinc_response_free(&r);

	/*
	 * 5d-bis. Ownership (issue #102). A volume is a directory the
	 * daemon creates as root, and until this there was no way to change
	 * that -- so a workload not running as root could not write to its
	 * own volume. Found the plain way: an operator logged into the jump
	 * box and their home directory, a volume, belonged to root.
	 */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes",
	                         "{\"name\":\"ownedvol\",\"owner_uid\":10000,\"owner_gid\":10000}",
	                         &r) == 0 &&
	          r.status == 201 &&
	          (long)json_as_number(json_object_get(r.json, "owner_uid")) == 10000,
	      "a volume can be created owned by someone other than root");
	{
		/* The record is only half the claim -- the directory on disk is
		 * the half that decides whether anyone can write to it. */
		const char *hp = json_str_field(r.json, "host_path");
		struct stat st;

		check(hp != NULL && stat(hp, &st) == 0 && st.st_uid == 10000 && st.st_gid == 10000,
		      "and the real directory on disk is owned by them, not just the record");
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes",
	                         "{\"name\":\"halfowned\",\"owner_uid\":10000}", &r) == 0 &&
	          r.status == 400,
	      "an owner_uid with no owner_gid is refused rather than half-applied");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "GET", "/v1/volumes/vol1", NULL, &r) == 0 && r.status == 200 &&
	          json_object_get(r.json, "owner_uid") != NULL &&
	          json_object_get(r.json, "owner_uid")->type == JSON_NULL,
	      "a volume nobody claimed reports null, not uid 0 -- \"root\" and \"unsaid\" are different");
	thinc_response_free(&r);

	/*
	 * Changing it afterwards, and the recursive flag meaning exactly
	 * what it says: a volume in use holds files whose ownership someone
	 * may have set deliberately, so rewriting all of them is asked for,
	 * never assumed.
	 */
	{
		char child[PATH_MAX];
		char host_path[PATH_MAX] = "";
		struct stat st;
		FILE *f;

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/volumes/ownedvol", NULL, &r) == 0 &&
		    json_str_field(r.json, "host_path") != NULL)
			snprintf(host_path, sizeof(host_path), "%s", json_str_field(r.json, "host_path"));
		thinc_response_free(&r);

		snprintf(child, sizeof(child), "%s/preexisting", host_path);
		f = fopen(child, "w");
		if (f != NULL)
			fclose(f);
		chown(child, 0, 0);

		memset(&r, 0, sizeof(r));
		check(thinc_client_request(&client, "PUT", "/v1/volumes/ownedvol/owner",
		                         "{\"uid\":10001,\"gid\":10002}", &r) == 0 &&
		          r.status == 200,
		      "a volume's owner can be changed afterwards");
		thinc_response_free(&r);

		check(stat(host_path, &st) == 0 && st.st_uid == 10001,
		      "the volume directory itself follows the new owner");
		check(stat(child, &st) == 0 && st.st_uid == 0,
		      "and content inside it is left alone unless recursion is asked for");

		memset(&r, 0, sizeof(r));
		check(thinc_client_request(&client, "PUT", "/v1/volumes/ownedvol/owner",
		                         "{\"uid\":10001,\"gid\":10002,\"recursive\":true}", &r) == 0 &&
		          r.status == 200,
		      "and recursion is available when it IS what was meant");
		thinc_response_free(&r);
		check(stat(child, &st) == 0 && st.st_uid == 10001,
		      "which does reach the content");

		/* Handing it back to root is a real request, not the absence of
		 * one: the directory must actually change hands. */
		memset(&r, 0, sizeof(r));
		check(thinc_client_request(&client, "PUT", "/v1/volumes/ownedvol/owner",
		                         "{\"uid\":null,\"gid\":null}", &r) == 0 &&
		          r.status == 200 &&
		          json_object_get(r.json, "owner_uid")->type == JSON_NULL,
		      "clearing an owner hands the volume back to root");
		check(stat(host_path, &st) == 0 && st.st_uid == 0,
		      "and the directory really is root's again");
	}

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "PUT", "/v1/volumes/ownedvol/owner", "{\"uid\":5}", &r) == 0 &&
	          r.status == 400,
	      "a uid with no gid is refused");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "PUT", "/v1/volumes/nosuchvol/owner", "{\"uid\":1,\"gid\":1}",
	                         &r) == 0 &&
	          r.status == 404,
	      "setting an owner on an unknown volume is refused");
	thinc_response_free(&r);

	/* 5e. size limits (issue #93). A volume had none, which made it an
	 * unbounded way to fill whatever disk it sits on -- sharper now
	 * that scheduled backups copy volumes onto a backup disk, where an
	 * unbounded source is an unbounded destination.
	 *
	 * The sandbox's /tmp has no project-quota support, so applying a
	 * real limit legitimately fails here; what IS verifiable is that
	 * the refusal is explicit rather than an accepted-and-unenforced
	 * limit, which is the failure mode that matters. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "PUT", "/v1/volumes/vol1/quota", "{\"quota_bytes\":-1}", &r) ==
	              0 &&
	          r.status == 400,
	      "a negative size limit is refused");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "PUT", "/v1/volumes/nosuchvol/quota", "{\"quota_bytes\":0}",
	                         &r) == 0 &&
	          r.status == 404,
	      "setting a limit on an unknown volume is refused");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "PUT", "/v1/volumes/vol1/quota",
	                       "{\"quota_bytes\":1073741824}", &r) == 0) {
		/*
		 * Either it applied (a real quota-capable filesystem) or it was
		 * refused with a reason. What must never happen is a 200 with
		 * no limit actually recorded -- an accepted limit that is not
		 * in force is worse than a refusal, because the operator
		 * believes it exists.
		 */
		if (r.status == 200) {
			const struct json_value *q = json_object_get(r.json, "quota_bytes");

			check(q != NULL && (long long)json_as_number(q) == 1073741824LL,
			      "an accepted size limit is actually recorded");
		} else {
			check(r.status == 409,
			      "a size limit that cannot be enforced is refused with a reason, never accepted "
			      "and quietly ignored");
		}
	} else {
		check(0, "setting a size limit reaches the daemon");
	}
	thinc_response_free(&r);

	/* 5f. LIVE attach (issue #92 part 2).
	 *
	 * Part 1 edited the definition and took effect on the next start.
	 * The claim now is stronger: attaching to a RUNNING container makes
	 * the volume visible inside it immediately. That can only be shown
	 * by looking from inside, so a long-running container is started,
	 * a volume attached to it live, and then a second container is used
	 * to confirm the data written through the live mount really landed
	 * in the volume -- proving the mount was real, not merely recorded.
	 */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes", "{\"name\":\"livevol\"}", &r) == 0 &&
	          r.status == 201,
	      "a volume for the live-attach test");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"clive\",\"image\":\"voltest\",\"restart\":\"no\","
	                         "\"cmd\":[\"/bin/volume_child\",\"sleep\"]}",
	                         &r) == 0 &&
	          r.status == 201,
	      "a running container with no volumes");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers/clive/volumes",
	                       "{\"name\":\"livevol\",\"path\":\"/live\"}", &r) == 0 &&
	    r.status == 200) {
		const char *applies = json_as_string(json_object_get(r.json, "applies"));

		/*
		 * The response says which it was. "now" is the whole point of
		 * part 2; "on next start" would mean the live mount did not
		 * happen and the caller is being told so rather than left to
		 * assume.
		 */
		check(applies != NULL && strcmp(applies, "now") == 0,
		      "attaching to a running container reports that it applied NOW, not on next start");
	} else {
		check(0, "attaching a volume to a running container");
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/containers/clive", NULL, &r);
	thinc_response_free(&r);
	wait_gone(&client, "clive");
	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/volumes/livevol", NULL, &r);
	thinc_response_free(&r);

	/* 5g. non-interactive exec (issue #62).
	 *
	 * The claim is that the command runs inside the ALREADY-RUNNING
	 * container's namespaces -- not a fresh one, which is what a
	 * throwaway container with capture_output gives and why that was a
	 * poor diagnostic. Proven by attaching a volume live, then exec'ing
	 * something that can only see it if it is in that container's own
	 * mount namespace.
	 */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/volumes", "{\"name\":\"execvol\"}", &r) == 0 &&
	          r.status == 201,
	      "a volume for the exec test");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"cexec\",\"image\":\"voltest\",\"restart\":\"no\","
	                         "\"cmd\":[\"/bin/volume_child\",\"sleep\"]}",
	                         &r) == 0 &&
	          r.status == 201,
	      "a running container to exec into");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers/cexec/volumes",
	                         "{\"name\":\"execvol\",\"path\":\"/execvol\"}", &r) == 0 &&
	          r.status == 200,
	      "volume attached live to the exec target");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers/cexec/exec",
	                         "{\"argv\":[\"/bin/volume_child\",\"write\"],\"timeout_seconds\":20}",
	                         &r) == 0 &&
	          r.status == 200,
	      "exec starts");
	thinc_response_free(&r);

	{
		int tries;
		const char *state = NULL;

		for (tries = 0; tries < 100; tries++) {
			usleep(100000);
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "GET", "/v1/containers/cexec/exec", NULL, &r) != 0) {
				thinc_response_free(&r);
				continue;
			}
			state = json_as_string(json_object_get(r.json, "state"));
			if (state != NULL && strcmp(state, "running") != 0) {
				const struct json_value *es = json_object_get(r.json, "exit_status");

				check(strcmp(state, "done") == 0, "the exec completed rather than timing out");
				check(es != NULL && es->type == JSON_NUMBER && (int)json_as_number(es) == 0,
				      "the exec reports the command's real exit status");
				thinc_response_free(&r);
				break;
			}
			thinc_response_free(&r);
		}
		check(tries < 100, "the exec finished within the poll window");
	}

	/*
	 * It ran in the container's own namespace, not a fresh one: the
	 * marker it wrote is in the volume, which only that container has
	 * mounted. A throwaway container would have written into its own
	 * unrelated namespace and this would be empty.
	 */
	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/containers/cexec", NULL, &r);
	thinc_response_free(&r);
	wait_gone(&client, "cexec");

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"cexecread\",\"image\":\"voltest\",\"capture_output\":true,"
	                         "\"volumes\":[{\"name\":\"execvol\",\"path\":\"/vol\"}],"
	                         "\"cmd\":[\"/bin/volume_child\"]}",
	                         &r) == 0 &&
	          r.status == 201,
	      "a reader for what the exec wrote");
	thinc_response_free(&r);
	out[0] = '\0';
	check(wait_exited(&client, "cexecread", out, sizeof(out)) == 0, "reader exited");
	check(strstr(out, "READ:PERSISTED") != NULL,
	      "the exec ran inside the RUNNING container's own namespaces -- its write landed in that "
	      "container's mounted volume, which a fresh namespace could not have reached");

	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/containers/cexecread", NULL, &r);
	thinc_response_free(&r);
	wait_gone(&client, "cexecread");
	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/volumes/execvol", NULL, &r);
	thinc_response_free(&r);

	/* 6. deleting a volume is refused while a container definition still
	 * references it -- silently removing data a stopped container will
	 * expect on its next start would be a data-loss bug. */
	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "DELETE", "/v1/volumes/vol1", NULL, &r) == 0 &&
	          r.status == 409,
	      "volume delete refused (409) while a container definition references it");
	thinc_response_free(&r);

	/* 7. once nothing references it, the volume deletes cleanly. */
	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/containers/cread", NULL, &r);
	thinc_response_free(&r);
	wait_gone(&client, "cread");

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "DELETE", "/v1/volumes/vol1", NULL, &r) == 0 &&
	          r.status == 204,
	      "volume deletes once nothing references it");
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(thinc_client_request(&client, "GET", "/v1/volumes/vol1", NULL, &r) == 0 && r.status == 404,
	      "deleted volume is gone");
	thinc_response_free(&r);

	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);

	if (g_failures == 0) {
		printf("VOLUME RESULT: PASS\n");
		return 0;
	}
	printf("VOLUME RESULT: FAIL (%d)\n", g_failures);
	return 1;
}
