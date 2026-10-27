/*
 * test_userns_run -- a user-namespaced container's own root must own the
 * things this platform gives it: the /run tmpfs it mounts (#264), the
 * files it stages before the container starts (#265), and the volumes it
 * attaches (#266).
 *
 * Both are the same question -- ownership versus the user namespace -- with
 * different mechanisms and different fixes, so they are checked together
 * here rather than in two tests that would share all of their setup.
 *
 * The bug this exists to catch: mountns_pivot() mounts /run's tmpfs while
 * still running as real host root, which is UNMAPPED in the container's
 * user namespace, so the tmpfs came up owned by the overflow uid (65534)
 * and the container's root -- the uid the payload actually runs as --
 * could not write to its own scratch directory. `jump` crash-looped on
 * exactly this, and nothing caught it for the life of the feature because
 * nothing in the suite ever wrote to /run: dnsmasq is launched with
 * --pid-file= and glauth writes nothing there.
 *
 * Why it has to be a userns container: without a user namespace the
 * container's root IS host root, /run comes up owned by 0 either way, and
 * the test would pass just as happily against the broken code. The bug is
 * only reachable when the two differ.
 *
 * It runs against the phase-3 presentation: a host-0-owned snapshot behind
 * an id-mapped mount. It used to run twice, the phase-2b copy+chown tree as
 * well, because #321 had shown that the id-mapped branch, chosen by
 * cix_btrfs_is_backing(), was never executed by a test on /tmp. ADR-0360
 * removed the copy and put the gate's data dir on btrfs, so this now runs
 * the branch every host takes, with no hook to force it (cix#658).
 *
 * That means this test needs an environment that can actually run one. The
 * dev sandbox cannot (see test_image_fixture.c, which seeds
 * userns_default=false for that reason), so it SKIPS there rather than
 * failing -- but it skips only when the container never starts at all,
 * never on a container that ran and was refused the write. A skip that
 * could swallow the defect would be worse than no test.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"
#include "test_drift.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7799
#define PORT_ARG "--port=7799"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];
static char g_exit_reason[256];
static int g_failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		g_failures++;
	}
}

static int wait_for_daemon(const struct cix_client *c, int max_attempts)
{
	int i;

	for (i = 0; i < max_attempts; i++) {
		struct cix_response r;
		int ok;

		memset(&r, 0, sizeof(r));
		ok = (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0 && r.status == 200);
		cix_response_free(&r);
		if (ok)
			return 0;
		usleep(100000);
	}
	return -1;
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	int n = 0;
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[n++] = "build/cixd";
	dargv[n++] = PORT_ARG;
	dargv[n++] = data_dir_arg;
	/*
	 * #321 used to need --test-userns-idmap here: the data dir was not
	 * btrfs, so the id-mapped presentation was unreachable without it.
	 * Since ADR-0360 the data dir is under $TMPDIR, which the gate puts
	 * on btrfs, so a userns container gets the id-mapped snapshot the
	 * way every production one does, and the hook is gone (cix#658).
	 */
	dargv[n] = NULL;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		_exit(127);
	}
	return pid;
}

/*
 * Polls until the container reports "exited" and returns its exit status,
 * or -1 if it never got there. Its captured output is copied out too: on a
 * failure the RUN_OWNER line names the cause outright.
 */
static int wait_exit_status(const struct cix_client *c, const char *name, char *out,
                             size_t out_size)
{
	char path[128];
	int i;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	for (i = 0; i < 150; i++) {
		struct cix_response r;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200) {
			const char *st = json_as_string(json_object_get(r.json, "status"));
			const struct json_value *jes = json_object_get(r.json, "exit_status");

			if (st != NULL && strcmp(st, "exited") == 0 && jes != NULL &&
			    jes->type == JSON_NUMBER) {
				const char *cap =
				        json_as_string(json_object_get(r.json, "captured_output"));
				/*
				 * The child's own diag line. Four different volume
				 * steps share exit 125 and eight mkdir sites share
				 * 112, so the status alone names a group, never a
				 * cause -- reporting one without this is how a
				 * traversal failure got read as a wrong array index.
				 */
				const char *why =
				        json_as_string(json_object_get(r.json, "exit_reason"));
				int status = (int)jes->u.number;

				snprintf(g_exit_reason, sizeof(g_exit_reason), "%s",
				         why != NULL ? why : "(none)");
				snprintf(out, out_size, "%s", cap != NULL ? cap : "");
				cix_response_free(&r);
				return status;
			}
		}
		cix_response_free(&r);
		usleep(100000);
	}
	return -1;
}

/*
 * One full pass against ONE rootfs presentation. Everything below is
 * identical for both -- which is the point: the container's own root must
 * own what the platform gives it no matter how its rootfs was provisioned,
 * and the two presentations disagree about who owns the tree on disk.
 */
static int run_presentation(const char *label)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;
	char out[512];
	int created;
	int status;
	int before = g_failures;

	printf("== presentation: %s ==\n", label);
	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/runtest/v1/rootfs",
	         g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/runtest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}
	if (test_image_fixture_build(g_image_root, "build/run_child", "run_child") != 0) {
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
	 * Real volumes to attach: #266 broke every one of these for a userns
	 * container, and a volume is the only way to reach that.
	 *
	 * TWO of them, for #322. The child's volume loop indexed its fd array
	 * with the enclosing function's `i` rather than its own `vi`, and `i`
	 * is left holding spec->cap_add_count -- zero for this container --
	 * so the FIRST volume was attached correctly by coincidence and only
	 * a second one exposes it (it re-uses the first volume's now-closed
	 * descriptor and the container dies at "volume move_mount idmap").
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/volumes", "{\"name\":\"runvol\"}", &r) != 0 ||
	    r.status != 201)
		check(0, "POST /v1/volumes creates the volume this test attaches");
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/volumes", "{\"name\":\"runvol2\"}", &r) != 0 ||
	    r.status != 201)
		check(0, "POST /v1/volumes creates the SECOND volume (#322 needs two)");
	cix_response_free(&r);

	/*
	 * "userns":true explicitly, never the platform default: the fixture
	 * deliberately seeds userns_default=false, and relying on a default
	 * would make this test silently stop testing anything the day that
	 * default changed.
	 */
	memset(&r, 0, sizeof(r));
	created = (cix_client_request(&client, "POST", "/v1/containers",
	                              "{\"name\":\"runprobe\",\"image\":\"runtest\","
	                              "\"userns\":true,\"capture_output\":true,"
	                              "\"files\":[{\"path\":\"/etc/staged_probe.conf\","
	                              "\"content\":\"secret\\n\",\"mode\":\"0600\"}],"
	                              "\"volumes\":[{\"name\":\"runvol\",\"path\":\"/vol\"},"
	                              "{\"name\":\"runvol2\",\"path\":\"/vol2\"}],"
	                              "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/run_child\"]}]}",
	                              &r) == 0 &&
	           r.status == 201);
	cix_response_free(&r);

	if (!created) {
		printf("SKIP: this environment cannot create a user-namespaced container\n");
		printf("      (the dev sandbox cannot; the real host can -- run it there)\n");
		goto done;
	}

	out[0] = '\0';
	g_exit_reason[0] = '\0';
	status = wait_exit_status(&client, "runprobe", out, sizeof(out));
	if (out[0] != '\0')
		printf("  container said: %s", out);
	if (g_exit_reason[0] != '\0')
		printf("  exit_reason: %s\n", g_exit_reason);

	if (status == 42) {
		printf("  PASS: the container's own root can write to /run, read a 0600 "
		       "file staged for it, and write to an attached volume\n");
	} else if (status == 43) {
		check(0, "a userns container's own root cannot write to /run -- "
		         "the /run tmpfs is owned by an unmapped uid (#264); "
		         "mount_container_tmpfs() in src/mountns.c is what sets this");
	} else if (status == 46) {
		check(0, "a userns container's own root cannot write to an attached VOLUME -- "
		         "the volume is not id-mapped: container_create() applies MOUNT_ATTR_IDMAP "
		         "to every userns container's volume fds (#266)");
	} else if (status == 45) {
		check(0, "a userns container's own root cannot read a file STAGED for it at "
		         "mode 0600 -- the staged file's ownership landed outside the "
		         "container's mapped range (#265); stage_container_file()'s "
		         "id_offset in daemon/src/main.c is what sets this");
	} else if (status == 125 || status == 112 || status == 126) {
		char msg[320];

		/* Say which step, from the child's own diag line: 125 covers
		 * four volume steps and 112 covers eight mkdir sites. */
		snprintf(msg, sizeof(msg),
		         "the container died before it ran (exit %d) -- %s", status,
		         g_exit_reason[0] != '\0' ? g_exit_reason : "no exit_reason recorded");
		check(0, msg);
	} else if (status < 0) {
		check(0, "the /run probe container never reached 'exited'");
	} else {
		char msg[160];

		snprintf(msg, sizeof(msg),
		         "the /run probe exited %d, which is neither writable (42) nor "
		         "denied (43) -- read its output above rather than trusting this test",
		         status);
		check(0, msg);
	}

	/*
	 * The storage model itself, not just that the container worked.
	 *
	 * ADR-0207 phase 3's whole claim is that the snapshot stays
	 * host-uid-0 on disk and the kernel does the presenting -- so the
	 * mount point the container's own root just created inside its
	 * rootfs must land as on-disk 0 under the id-map. This is a test of
	 * the model rather than of the code: a "fix" that chowned the
	 * snapshot to the subordinate base would leave every container
	 * running perfectly and fail right here, which is exactly the wrong
	 * turn taken while diagnosing #321. The copy+chown presentation,
	 * whose tree was owned by the subordinate base, was removed by
	 * ADR-0360 (cix#658).
	 */
	{
		char vp[PATH_MAX];
		struct stat vst;

		snprintf(vp, sizeof(vp), "%s/rebuildable/rootfs/runprobe/vol", g_data_dir);
		if (stat(vp, &vst) != 0) {
			check(0, "the container's rootfs still carries its volume mount point");
		} else {
			printf("  %s/vol on-disk uid=%u\n", vp, (unsigned)vst.st_uid);
			check(vst.st_uid == 0,
			      "id-mapped presentation: the rootfs stays host-uid-0 on "
			      "disk, the kernel does the presenting (ADR-0207 phase 3)");
		}
	}

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/runprobe", NULL, &r);
	cix_response_free(&r);

	check(test_assert_no_drift(&client, NULL) == 0,
	      "nothing this test changed went unannounced (ADR-0339)");
done:
	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);
	return g_failures > before ? 1 : 0;
}

int main(void)
{
	/*
	 * The one presentation every host runs. #321 shipped because this
	 * test, on /tmp, exercised a different branch from the one a btrfs
	 * host takes; on the gate's btrfs data dir it takes the same one.
	 */
	run_presentation("id-mapped snapshot (ADR-0207 phase 3)");

	if (g_failures > 0) {
		fprintf(stderr, "USERNS /run: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("USERNS /run: PASS\n");
	return 0;
}
