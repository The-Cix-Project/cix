/*
 * Proves Part 5 of the pkg/ redesign (task #770, ADR-0124): a
 * follow_rolling container tracks its own image's current_version --
 * when a rolling image auto-rebuilds (ADR-0107's own trigger,
 * unmodified), apply_rolling_container_restarts() (daemon/src/main.c)
 * patches the container's persisted image_version pin and, if it's
 * still live, kills and replays it onto the new rootfs via a jittered
 * one-shot timer (CONN_ROLLING_RESTART_TIMER). Proven two-sided: a
 * follow_rolling container gets a genuinely new process (pid changes,
 * not just relabeled) pinned to the new version; an otherwise-identical
 * container on the same image WITHOUT follow_rolling is left
 * completely untouched -- the opt-in gate the user asked for. Also
 * proves the per-container follow_rolling_jitter_seconds override
 * (Part 5 follow-up): round-trips through create/GET, and actually
 * takes precedence over a much larger daemon-wide default when a
 * second rebuild fires.
 *
 * Kept hermetic and gcc-free: the "package" installed is a real,
 * already-compiled test binary (build/daemon_child, dynamically linked
 * against system glibc like everything else this project produces) tar-
 * wrapped as the pkg_source payload, with a no-op pkg_build() and a
 * pkg_install() that just copies it -- pkg_seed_image_baseline()
 * already stages the runtime lib closure (ld-linux/libc/...) onto
 * every image, so this binary runs inside the container with no
 * compiler ever invoked.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"
#include "test_floor.h"

#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7657
#define PORT_ARG "--port=7657"

/*
 * Poll budget for an async operation that involves a real package build
 * or a container restart under this test's own load. 300 * 200ms = 60s.
 * Was 12s at the install wait and 20s at the rebuild waits (issue #123),
 * which a real filesystem journal stall -- the daemon's own stall
 * watchdog logs 5-13s of jbd2_log_wait_commit here on a busy box --
 * could exceed, failing the wait and cascading into every assertion
 * after it. The assertion is that the operation COMPLETES, never that it
 * completes fast; 60s is generous headroom over the worst stall observed
 * while still bounded well under the harness timeout.
 */
#define ROLL_POLL_ATTEMPTS 300

static char g_data_dir[PATH_MAX];
static int g_failures;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s\n", msg); \
			g_failures++; \
		} \
	} while (0)

static int run_cmd(const char *fmt, ...)
{
	char cmd[2048];
	va_list ap;
	int rc;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	rc = system(cmd);
	return (rc == 0) ? 0 : -1;
}

static int compute_file_sha256(const char *path, char *out_sha256, size_t sha256_size)
{
	char shacmd[700];
	FILE *sp;
	char buf[128] = { 0 };

	snprintf(shacmd, sizeof(shacmd), "sha256sum '%s'", path);
	sp = popen(shacmd, "r");
	if (sp == NULL)
		return -1;
	if (fgets(buf, sizeof(buf), sp) == NULL) {
		pclose(sp);
		return -1;
	}
	pclose(sp);
	if (strlen(buf) < 64 || 64 >= sha256_size)
		return -1;
	memcpy(out_sha256, buf, 64);
	out_sha256[64] = '\0';
	return 0;
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

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

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

/* Stages <scratch_dir>/rollsvc-<version>/rollsvc (a copy of the
 * already-compiled binary_path), tars it with the usual
 * <name>-<version>/ wrapper, returns its sha256. */
static int stage_binary_fixture(const char *scratch_dir, const char *version,
                                 const char *binary_path, char *out_tarball_path,
                                 size_t tarball_path_size, char *out_sha256, size_t sha256_size)
{
	char src_dir[512];
	char dst_bin[600];

	snprintf(src_dir, sizeof(src_dir), "%s/rollsvc-%s", scratch_dir, version);
	if (run_cmd("mkdir -p '%s'", src_dir) != 0)
		return -1;

	snprintf(dst_bin, sizeof(dst_bin), "%s/rollsvc", src_dir);
	if (run_cmd("cp '%s' '%s' && chmod +x '%s'", binary_path, dst_bin, dst_bin) != 0)
		return -1;

	snprintf(out_tarball_path, tarball_path_size, "%s/rollsvc-%s.tarball", scratch_dir, version);
	if (run_cmd("tar -cf '%s' -C '%s' 'rollsvc-%s'", out_tarball_path, scratch_dir, version) != 0)
		return -1;

	return compute_file_sha256(out_tarball_path, out_sha256, sha256_size);
}

static int write_binary_recipe(const char *pkg_state_dir, const char *version,
                                const char *tarball_path, const char *sha256)
{
	char recipes_dir[256];
	char name_dir[256];
	char path[300];
	FILE *f;

	/* Unlike the real POST /v1/pkg/recipes path (pkg_recipe_add()'s own
	 * persist_mkdir_p()), this test writes recipe files directly to
	 * disk and never calls that endpoint first, so nothing else has
	 * created "recipes/" yet. */
	snprintf(recipes_dir, sizeof(recipes_dir), "%s/recipes", pkg_state_dir);
	mkdir(pkg_state_dir, 0755);
	mkdir(recipes_dir, 0755);

	snprintf(name_dir, sizeof(name_dir), "%s/recipes/rollsvc", pkg_state_dir);
	mkdir(name_dir, 0755);
	snprintf(path, sizeof(path), "%s/%s", name_dir, version);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/recipes/rollsvc/%s/build.sh", pkg_state_dir, version);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "pkg_name=rollsvc\n");
	fprintf(f, "pkg_version=%s\n", version);
	fprintf(f, "pkg_source=%s\n", test_http_src(tarball_path));
	fprintf(f, "pkg_sha256=%s\n", sha256);
	fprintf(f, "pkg_depends=\"\"\n");
	fprintf(f, "pkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n");
	/* No compiler needed -- rollsvc is already a real ELF binary. */
	fprintf(f, "pkg_build() {\n\t:\n}\n\n");
	fprintf(f, "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp rollsvc "
	           "\"$PKG_DESTDIR/usr/bin/rollsvc\"\n\tchmod +x \"$PKG_DESTDIR/usr/bin/rollsvc\"\n}\n");
	fclose(f);
	return 0;
}

static int poll_pkg_installed_version(const struct cix_client *c, const char *pkg_at_image,
                                       const char *want_version, int max_attempts)
{
	int i;
	char path[256];

	snprintf(path, sizeof(path), "/v1/pkg/%s", pkg_at_image);
	for (i = 0; i < max_attempts; i++) {
		struct cix_response r;
		int matched = 0;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200) {
			const char *st = json_str_field(r.json, "state");
			const char *ver = json_str_field(r.json, "version");

			matched = st != NULL && strcmp(st, "installed") == 0 && ver != NULL &&
			          strcmp(ver, want_version) == 0;
		}
		cix_response_free(&r);
		if (matched)
			return 0;
		usleep(200000);
	}
	return -1;
}

/* Fetches pid and image_version for name; returns 0 on success. */
static int fetch_container_pid_version(const struct cix_client *c, const char *name, long *out_pid,
                                        char *out_version, size_t version_size)
{
	struct cix_response r;
	char path[300];
	int rc = -1;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200) {
		const char *ver = json_str_field(r.json, "image_version");

		*out_pid = (long)json_as_number(json_object_get(r.json, "pid"));
		if (ver != NULL)
			snprintf(out_version, version_size, "%s", ver);
		else
			out_version[0] = '\0';
		rc = 0;
	}
	cix_response_free(&r);
	return rc;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;
	char scratch_dir[] = "/tmp/cix_test_rollrestart_XXXXXX";
	char pkg_state_dir[PATH_MAX];
	char image_dir[PATH_MAX];
	char tarball_v1[512], sha_v1[128];
	char tarball_v2[512], sha_v2[128];
	char old_version[128], new_version[128];
	long follow_pid_before, follow_pid_after;
	long plain_pid_before, plain_pid_after;
	char follow_ver_before[128], follow_ver_after[128];
	char plain_ver_before[128], plain_ver_after[128];
	int i;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	/*
	 * ADR-0209: the build floor. Real, recipe-built package artifacts
	 * seeded into this daemon's own cache, so installing them is a
	 * cache hit that needs no build environment -- the same way a fresh
	 * host gets its first packages. There is no shared sandbox to
	 * inherit one from any more, and nothing here is fabricated.
	 */
	if (test_image_fixture_seed_floor_packages(g_data_dir, "build-inputs/floor-artifacts") != 0) {
		fprintf(stderr, "could not seed the build floor -- fetch the real package artifacts "
		                "into build-inputs/floor-artifacts first (ADR-0209)\n");
		return 1;
	}
	if (mkdtemp(scratch_dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	snprintf(pkg_state_dir, sizeof(pkg_state_dir), "%s/rebuildable/pkg", g_data_dir);
	snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/rollctrimg", g_data_dir);

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never became healthy\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* Regular (non-hostbuild) `pkg install` builds every package inside
	 * a shared toolchain sandbox (g_pkgbuild_rootfs, ADR-0056), never
	 * the target image's own rootfs -- an empty POST here bootstraps it
	 * from this sandbox's own real host environment (same call
	 * test_pkg.c's own scenario 1 makes), needed before any install
	 * below can create its build container. */
	CHECK(test_floor_install_all(&client) == 0, "install the build floor");

	/* --- rolling-config GET/PUT round trip, and range validation --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/system/rolling-config", NULL, &r) == 0 &&
	          r.status == 200,
	      "GET /v1/system/rolling-config (default)");
	if (r.json != NULL) {
		CHECK((long)json_as_number(json_object_get(r.json, "jitter_window_seconds")) == 60,
		      "default jitter_window_seconds is 60");
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "PUT", "/v1/system/rolling-config",
	                         "{\"jitter_window_seconds\":99999}", &r) == 0 &&
	          r.status == 400,
	      "PUT rolling-config out of range (99999) is 400");
	cix_response_free(&r);

	/* Deterministic restart timing for the rest of this test: no jitter. */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "PUT", "/v1/system/rolling-config",
	                         "{\"jitter_window_seconds\":0}", &r) == 0 &&
	          r.status == 200,
	      "PUT rolling-config jitter_window_seconds=0");
	if (r.json != NULL) {
		CHECK((long)json_as_number(json_object_get(r.json, "jitter_window_seconds")) == 0,
		      "rolling-config round-trips to 0");
	}
	cix_response_free(&r);

	/* --- stage rollsvc 1.0 (a real prebuilt binary, no compiler) --- */
	CHECK(stage_binary_fixture(scratch_dir, "1.0", "build/daemon_child", tarball_v1,
	                            sizeof(tarball_v1), sha_v1, sizeof(sha_v1)) == 0,
	      "stage rollsvc 1.0 fixture");
	CHECK(write_binary_recipe(pkg_state_dir, "1.0", tarball_v1, sha_v1) == 0,
	      "write rollsvc 1.0 recipe");

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/images", "{\"name\":\"rollctrimg\"}", &r) == 0 &&
	          r.status == 201,
	      "POST rollctrimg image");
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	/* Containers run from this image below, so it needs a C library to
	 * execve anything (#186) -- a package now, not four files the
	 * baseline used to copy off the build host. */
	CHECK(cix_client_request(&client, "POST", "/v1/pkg/install",
	                         "{\"name\":\"glibc\",\"image\":\"rollctrimg\"}", &r) == 0 &&
	              r.status == 202,
	      "POST install glibc@rollctrimg");
	cix_response_free(&r);
	CHECK(poll_pkg_installed_version(&client, "glibc@rollctrimg", TEST_FLOOR_GLIBC_VERSION, ROLL_POLL_ATTEMPTS) == 0,
	      "glibc@rollctrimg reaches installed");

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/pkg/install",
	                         "{\"name\":\"rollsvc\",\"image\":\"rollctrimg\"}", &r) == 0 &&
	          r.status == 202,
	      "POST install rollsvc@rollctrimg");
	cix_response_free(&r);
	CHECK(poll_pkg_installed_version(&client, "rollsvc@rollctrimg", "1.0", ROLL_POLL_ATTEMPTS) == 0,
	      "rollsvc@rollctrimg (1.0) reaches installed");

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/images/rollctrimg/manifest",
	                         "{\"package\":\"rollsvc\",\"mode\":\"rolling\",\"version\":\"1.0\"}",
	                         &r) == 0 &&
	          r.status == 204,
	      "POST rollctrimg manifest (rollsvc rolling@1.0)");
	cix_response_free(&r);

	CHECK(test_image_fixture_read_current_version(image_dir, old_version, sizeof(old_version)) == 0,
	      "read rollctrimg's pre-rebuild current_version");

	/* --- two containers on the same rolling image: one opts in, one doesn't --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"rollctr-follow\",\"image\":\"rollctrimg\","
	                         "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/usr/bin/rollsvc\",\"300\",\"0\"]}],"
	                         "\"restart\":\"always\",\"follow_rolling\":true}",
	                         &r) == 0 &&
	          r.status == 201,
	      "POST rollctr-follow (follow_rolling=true)");
	if (r.json != NULL) {
		const struct json_value *fr = json_object_get(r.json, "follow_rolling");

		CHECK(fr != NULL && fr->type == JSON_BOOL && fr->u.boolean,
		      "create response reports follow_rolling=true");
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"rollctr-plain\",\"image\":\"rollctrimg\","
	                         "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/usr/bin/rollsvc\",\"300\",\"0\"]}],"
	                         "\"restart\":\"always\"}",
	                         &r) == 0 &&
	          r.status == 201,
	      "POST rollctr-plain (no follow_rolling)");
	if (r.json != NULL) {
		const struct json_value *fr = json_object_get(r.json, "follow_rolling");

		CHECK(fr != NULL && fr->type == JSON_BOOL && !fr->u.boolean,
		      "create response reports follow_rolling=false by default");
	}
	cix_response_free(&r);

	CHECK(fetch_container_pid_version(&client, "rollctr-follow", &follow_pid_before,
	                                   follow_ver_before, sizeof(follow_ver_before)) == 0 &&
	          follow_pid_before > 0,
	      "rollctr-follow is running before the rebuild");
	CHECK(fetch_container_pid_version(&client, "rollctr-plain", &plain_pid_before,
	                                   plain_ver_before, sizeof(plain_ver_before)) == 0 &&
	          plain_pid_before > 0,
	      "rollctr-plain is running before the rebuild");
	CHECK(strcmp(follow_ver_before, old_version) == 0, "rollctr-follow pinned to pre-rebuild version");
	CHECK(strcmp(plain_ver_before, old_version) == 0, "rollctr-plain pinned to pre-rebuild version");

	/* --- publish rollsvc 2.0: the ONLY action taken. No install/upgrade
	 * request, no restart request -- the daemon's own rolling-rebuild
	 * trigger (ADR-0107, unmodified) and this test's new reconciliation
	 * hook (ADR-0124) must do everything from here. --- */
	CHECK(stage_binary_fixture(scratch_dir, "2.0", "build/daemon_child", tarball_v2,
	                            sizeof(tarball_v2), sha_v2, sizeof(sha_v2)) == 0,
	      "stage rollsvc 2.0 fixture");
	{
		struct json_writer w;
		char body[2048];

		/* Built purely in memory and published ONLY through POST
		 * /v1/pkg/recipes -- unlike rollsvc 1.0 (write_binary_recipe(),
		 * a direct on-disk write used once as setup before anything
		 * else touches that version), this version must reach the
		 * daemon exclusively through the real API call for
		 * pkg_recipe_add()'s own trigger (queue_rolling_rebuilds_for())
		 * to ever fire -- pre-writing the file first (as an earlier
		 * draft of this test did) makes the daemon see an
		 * already-published version and reject the POST as a 409
		 * (recipe versions are immutable). */
		{
			char content[1200];

			snprintf(content, sizeof(content),
			         "pkg_name=rollsvc\npkg_version=2.0\npkg_source=%s\n"
			         "pkg_sha256=%s\npkg_depends=\"\"\n"
		         "pkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
			         "pkg_build() {\n\t:\n}\n\n"
			         "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp rollsvc "
			         "\"$PKG_DESTDIR/usr/bin/rollsvc\"\n\tchmod +x "
			         "\"$PKG_DESTDIR/usr/bin/rollsvc\"\n}\n",
			         test_http_src(tarball_v2), sha_v2);

			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, "rollsvc");
			jw_key(&w, "content");
			jw_str(&w, content);
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			snprintf(body, sizeof(body), "%s", w.buf);
		}

		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "POST", "/v1/pkg/recipes", body, &r) == 0 &&
		          r.status == 204,
		      "publish rollsvc 2.0 recipe (triggers rolling auto-rebuild)");
		jw_free(&w);
		cix_response_free(&r);
	}

	/* Wait for the auto-rebuild to move rollctrimg's own current_version. */
	new_version[0] = '\0';
	for (i = 0; i < ROLL_POLL_ATTEMPTS; i++) {
		if (test_image_fixture_read_current_version(image_dir, new_version, sizeof(new_version)) ==
		        0 &&
		    strcmp(new_version, old_version) != 0)
			break;
		usleep(200000);
	}
	CHECK(strcmp(new_version, old_version) != 0, "rollctrimg's current_version advanced");

	/* Wait for rollctr-follow to actually restart onto it: real proof is
	 * a DIFFERENT pid (killed and replayed, not just relabeled) AND the
	 * new image_version. */
	follow_pid_after = follow_pid_before;
	follow_ver_after[0] = '\0';
	for (i = 0; i < ROLL_POLL_ATTEMPTS; i++) {
		if (fetch_container_pid_version(&client, "rollctr-follow", &follow_pid_after,
		                                 follow_ver_after, sizeof(follow_ver_after)) == 0 &&
		    follow_pid_after != follow_pid_before && strcmp(follow_ver_after, new_version) == 0)
			break;
		usleep(200000);
	}
	CHECK(follow_pid_after > 0 && follow_pid_after != follow_pid_before,
	      "rollctr-follow got a genuinely new process (pid changed)");
	CHECK(strcmp(follow_ver_after, new_version) == 0,
	      "rollctr-follow's pinned image_version advanced to the new current_version");

	/* rollctr-plain must be completely untouched. */
	CHECK(fetch_container_pid_version(&client, "rollctr-plain", &plain_pid_after, plain_ver_after,
	                                   sizeof(plain_ver_after)) == 0,
	      "rollctr-plain still reachable");
	CHECK(plain_pid_after == plain_pid_before,
	      "rollctr-plain's pid is unchanged (opt-in gate honored)");
	CHECK(strcmp(plain_ver_after, old_version) == 0,
	      "rollctr-plain is still pinned to the pre-rebuild version");

	/* --- per-container jitter override (Part 5 follow-up) ---
	 *
	 * First, a pure round-trip proof: an explicit
	 * follow_rolling_jitter_seconds on create is echoed back by GET,
	 * not silently dropped.
	 *
	 * Second, a real behavioral proof that the override actually wins
	 * over the daemon-wide default, not just that it's stored: crank
	 * the daemon-wide default up to its own maximum (3600s -- up to an
	 * hour), give a new container an explicit override of 0 (restart
	 * immediately, no jitter), trigger one more rebuild, and confirm
	 * this container restarts within a short poll window. If the
	 * per-container override were being ignored in favor of the
	 * (now huge) daemon default, a short poll window would essentially
	 * never observe a restart -- rolling_jitter_seconds() draws
	 * uniformly from [0, 3600], so P(<=~10s by chance alone) is under
	 * 0.3%.
	 */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"rollctr-jitter\",\"image\":\"rollctrimg\","
	                         "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/usr/bin/rollsvc\",\"300\",\"0\"]}],"
	                         "\"restart\":\"always\",\"follow_rolling\":true,"
	                         "\"follow_rolling_jitter_seconds\":0}",
	                         &r) == 0 &&
	          r.status == 201,
	      "POST rollctr-jitter (follow_rolling=true, jitter override=0)");
	if (r.json != NULL) {
		const struct json_value *jj = json_object_get(r.json, "follow_rolling_jitter_seconds");

		CHECK(jj != NULL && jj->type == JSON_NUMBER && (long)json_as_number(jj) == 0,
		      "create response echoes follow_rolling_jitter_seconds=0, not null");
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/containers/rollctr-jitter", NULL, &r) == 0 &&
	          r.status == 200,
	      "GET rollctr-jitter");
	if (r.json != NULL) {
		const struct json_value *jj = json_object_get(r.json, "follow_rolling_jitter_seconds");

		CHECK(jj != NULL && jj->type == JSON_NUMBER && (long)json_as_number(jj) == 0,
		      "GET rollctr-jitter reports follow_rolling_jitter_seconds=0");
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "PUT", "/v1/system/rolling-config",
	                         "{\"jitter_window_seconds\":3600}", &r) == 0 &&
	          r.status == 200,
	      "PUT rolling-config jitter_window_seconds=3600 (the daemon-wide default a container-level "
	      "override must beat)");
	cix_response_free(&r);

	{
		long jitter_pid_before, jitter_pid_after;
		char jitter_ver_before[128], jitter_ver_after[128];
		char tarball_v3[512], sha_v3[128];
		char newer_version[128];
		struct json_writer w;
		char body[2048];
		char content[1200];

		CHECK(fetch_container_pid_version(&client, "rollctr-jitter", &jitter_pid_before,
		                                   jitter_ver_before, sizeof(jitter_ver_before)) == 0 &&
		          jitter_pid_before > 0,
		      "rollctr-jitter is running before the second rebuild");
		CHECK(strcmp(jitter_ver_before, new_version) == 0,
		      "rollctr-jitter pinned to the first rebuild's version");

		CHECK(stage_binary_fixture(scratch_dir, "3.0", "build/daemon_child", tarball_v3,
		                            sizeof(tarball_v3), sha_v3, sizeof(sha_v3)) == 0,
		      "stage rollsvc 3.0 fixture");

		snprintf(content, sizeof(content),
		         "pkg_name=rollsvc\npkg_version=3.0\npkg_source=%s\n"
		         "pkg_sha256=%s\npkg_depends=\"\"\n"
		         "pkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
		         "pkg_build() {\n\t:\n}\n\n"
		         "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp rollsvc "
		         "\"$PKG_DESTDIR/usr/bin/rollsvc\"\n\tchmod +x "
		         "\"$PKG_DESTDIR/usr/bin/rollsvc\"\n}\n",
		         test_http_src(tarball_v3), sha_v3);

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, "rollsvc");
		jw_key(&w, "content");
		jw_str(&w, content);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		snprintf(body, sizeof(body), "%s", w.buf);

		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "POST", "/v1/pkg/recipes", body, &r) == 0 &&
		          r.status == 204,
		      "publish rollsvc 3.0 recipe (triggers second rolling auto-rebuild)");
		jw_free(&w);
		cix_response_free(&r);

		newer_version[0] = '\0';
		for (i = 0; i < ROLL_POLL_ATTEMPTS; i++) {
			if (test_image_fixture_read_current_version(image_dir, newer_version,
			                                             sizeof(newer_version)) == 0 &&
			    strcmp(newer_version, new_version) != 0)
				break;
			usleep(200000);
		}
		CHECK(strcmp(newer_version, new_version) != 0,
		      "rollctrimg's current_version advanced a second time");

		jitter_pid_after = jitter_pid_before;
		jitter_ver_after[0] = '\0';
		for (i = 0; i < 50; i++) { /* ~10s -- see this block's own comment on why */
			if (fetch_container_pid_version(&client, "rollctr-jitter", &jitter_pid_after,
			                                 jitter_ver_after, sizeof(jitter_ver_after)) == 0 &&
			    jitter_pid_after != jitter_pid_before &&
			    strcmp(jitter_ver_after, newer_version) == 0)
				break;
			usleep(200000);
		}
		CHECK(jitter_pid_after > 0 && jitter_pid_after != jitter_pid_before,
		      "rollctr-jitter restarted promptly despite a 3600s daemon-wide default -- its own "
		      "jitter_seconds=0 override took effect");
		CHECK(strcmp(jitter_ver_after, newer_version) == 0,
		      "rollctr-jitter's pinned image_version advanced to the second rebuild's version");
	}

	CHECK(stop_daemon(daemon_pid) == 0, "daemon shut down cleanly");
	run_cmd("rm -rf '%s'", scratch_dir);
	test_data_dir_cleanup(g_data_dir);

	if (g_failures > 0) {
		fprintf(stderr, "test_rolling_restart: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_rolling_restart: OK\n");
	return 0;
}
