/*
 * ADR-0159 Phase B end-to-end test: POST /v1/system/kmod-build, an
 * ordinary hostbuild against the "kernel" recipe (the exact existing
 * pkg_hostbuild_start() mechanism test_pkg.c's own step 17 already
 * proves generically) gaining an optional config_symbols parameter
 * threaded through as the build container's own real
 * THINC_KMOD_EXTRA_SYMBOLS environment variable.
 *
 * Not a real kernel build (far too slow for this suite, and the real
 * 6.18.40 kernel.recipe lives in this project's own git-tracked
 * recipes/, entirely outside this test's isolated --data-dir=
 * recipes_dir) -- a tiny fixture recipe literally named "kernel"
 * (isolated to this test's own recipes_dir, no collision with the real
 * one) whose pkg_build() writes the env var's own value straight to a
 * file, proving the daemon-side plumbing (pkg_hostbuild_start()'s new
 * parameter, pkg_fetch_completed()'s build_envp construction) actually
 * carries the REST-supplied symbols through to the real build
 * container, unmodified -- the one thing kernel.recipe's own build.sh
 * change can't be proven any other way in this sandbox.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

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

#define TEST_PORT 7684
#define PORT_ARG "--port=7684"

static char g_data_dir[PATH_MAX];
static char g_pkg_state_dir[PATH_MAX];
static char g_hbimage_dir[PATH_MAX];

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

static int run_cmd(const char *fmt, ...)
{
	char cmd[1024];
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
	if (strlen(buf) < 64)
		return -1;
	if (64 >= sha256_size)
		return -1;
	memcpy(out_sha256, buf, 64);
	out_sha256[64] = '\0';
	return 0;
}

/* Same shape as test_pkg.c's own stage_fixture_tarball() (duplicated,
 * not shared -- this project's own established convention for small,
 * self-contained test fixture helpers, see diskpart.c's comment on
 * run_sfdisk_stdin() for the identical reasoning applied to production
 * code). */
static int stage_fixture_tarball(const char *scratch_dir, char *out_tarball_path,
                                  size_t tarball_path_size, char *out_sha256, size_t sha256_size)
{
	char src_dir[512];
	char hello_c[600], makefile[600];
	FILE *f;

	snprintf(src_dir, sizeof(src_dir), "%s/kernel-1.0", scratch_dir);
	if (run_cmd("mkdir -p '%s'", src_dir) != 0)
		return -1;

	snprintf(hello_c, sizeof(hello_c), "%s/hello.c", src_dir);
	f = fopen(hello_c, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "#include <stdio.h>\nint main(void){printf(\"fixture bzImage\\n\");return 0;}\n");
	fclose(f);

	snprintf(makefile, sizeof(makefile), "%s/Makefile", src_dir);
	f = fopen(makefile, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "hello: hello.c\n\tgcc -o hello hello.c\n");
	fclose(f);

	snprintf(out_tarball_path, tarball_path_size, "%s/kernel-1.0.tarball", scratch_dir);
	if (run_cmd("tar -cf '%s' -C '%s' 'kernel-1.0'", out_tarball_path, scratch_dir) != 0)
		return -1;

	return compute_file_sha256(out_tarball_path, out_sha256, sha256_size);
}

/* pkg_build() writes $THINC_KMOD_EXTRA_SYMBOLS to symbols.txt --
 * unset/empty just produces an empty file, exactly mirroring the real
 * kernel.recipe's own "strictly additive, unset changes nothing" shape
 * (its own merge_config.sh branch is skipped the same way). */
static int write_kernel_fixture_recipe(const char *tarball_path, const char *sha256)
{
	char name_dir[256];
	char path[300];
	FILE *f;

	snprintf(name_dir, sizeof(name_dir), "%s/recipes/kernel", g_pkg_state_dir);
	mkdir(name_dir, 0755);
	snprintf(path, sizeof(path), "%s/1.0", name_dir);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/recipes/kernel/1.0/build.sh", g_pkg_state_dir);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "pkg_name=kernel\npkg_version=1.0\npkg_source=file://%s\n", tarball_path);
	fprintf(f, "pkg_sha256=%s\npkg_depends=\"\"\n\n", sha256);
	fprintf(f, "pkg_build() {\n\tgcc -o hello hello.c\n\techo -n \"$THINC_KMOD_EXTRA_SYMBOLS\" "
	           "> symbols.txt\n}\n\n");
	fprintf(f, "pkg_install() {\n\tcp hello \"$PKG_DESTDIR/hello\"\n\tcp symbols.txt "
	           "\"$PKG_DESTDIR/symbols.txt\"\n}\n");
	fclose(f);
	return 0;
}

static int read_file_string(const char *path, char *out, size_t out_size)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (f == NULL)
		return -1;
	n = fread(out, 1, out_size - 1, f);
	fclose(f);
	out[n] = '\0';
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	char scratch_dir[] = "/tmp/thinc_test_kmod_build_XXXXXX";
	char tarball_path[512], sha256[128];
	char artifact_path[PATH_MAX] = "";
	char state[32];
	int i;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_pkg_state_dir, sizeof(g_pkg_state_dir), "%s/rebuildable/pkg", g_data_dir);
	snprintf(g_hbimage_dir, sizeof(g_hbimage_dir), "%s/rebuildable/images/hbimage", g_data_dir);

	run_cmd("mkdir -p '%s/recipes'", g_pkg_state_dir);

	if (mkdtemp(scratch_dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (stage_fixture_tarball(scratch_dir, tarball_path, sizeof(tarball_path), sha256,
	                           sizeof(sha256)) != 0) {
		fprintf(stderr, "FAIL: could not stage fixture tarball\n");
		return 1;
	}
	if (write_kernel_fixture_recipe(tarball_path, sha256) != 0) {
		fprintf(stderr, "FAIL: could not write kernel fixture recipe\n");
		return 1;
	}

	/* hbimage: a real gcc-capable rootfs (ADR-0107/0108 manifest.json
	 * shape, same fixture test_pkg.c's own hostbuild step already
	 * establishes). */
	{
		char hb_rootfs[PATH_MAX];
		char hb_manifest[PATH_MAX];
		FILE *f;

		snprintf(hb_rootfs, sizeof(hb_rootfs), "%s/hbfixture/rootfs", g_hbimage_dir);
		if (test_image_fixture_stage_toolchain(hb_rootfs) != 0) {
			fprintf(stderr, "FAIL: could not stage hbimage toolchain\n");
			return 1;
		}
		snprintf(hb_manifest, sizeof(hb_manifest), "%s/manifest.json", g_hbimage_dir);
		f = fopen(hb_manifest, "w");
		if (f == NULL) {
			fprintf(stderr, "FAIL: could not write hbimage manifest.json\n");
			return 1;
		}
		fprintf(f,
		        "{\"packages\":[],\"current_version\":\"hbfixture\","
		        "\"versions\":[{\"version\":\"hbfixture\",\"created_at\":0}]}");
		fclose(f);
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

	/* 1. Error paths first (no job in flight yet for any of these). */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/system/kmod-build", "{}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST kmod-build with no build_image: expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/system/kmod-build",
	                       "{\"build_image\":\"hbimage\",\"config_symbols\":[\"not_a_config_symbol\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr,
		        "FAIL: POST kmod-build with a malformed config_symbols entry: expected 400, "
		        "got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/system/kmod-build",
	                       "{\"build_image\":\"hbimage\",\"config_symbols\":\"CONFIG_FOO\"}", &r) !=
	        0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST kmod-build with non-array config_symbols: expected 400, "
		                "got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/system/kmod-build",
	                       "{\"build_image\":\"no-such-image\"}", &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: POST kmod-build with unknown build_image: expected 404, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. The real payoff: config_symbols actually reaches the build
	 * container as THINC_KMOD_EXTRA_SYMBOLS, space-joined. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "POST", "/v1/system/kmod-build",
	                              "{\"build_image\":\"hbimage\","
	                              "\"config_symbols\":[\"CONFIG_FOO\",\"CONFIG_BAR\"]}",
	                              &r) != 0 ||
	           r.status != 202)) {
		fprintf(stderr, "FAIL: POST kmod-build hbimage: expected 202, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	state[0] = '\0';
	for (i = 0; ok && i < 30; i++) {
		const char *s;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/hostbuild/kernel", NULL, &r) != 0 ||
		    r.status != 200) {
			kx_response_free(&r);
			break;
		}
		s = json_as_string(json_object_get(r.json, "state"));
		if (s == NULL) {
			kx_response_free(&r);
			break;
		}
		snprintf(state, sizeof(state), "%s", s);
		kx_response_free(&r);
		if (strcmp(state, "fetching") != 0 && strcmp(state, "building") != 0)
			break;
		usleep(300000);
	}
	if (ok && strcmp(state, "installed") != 0) {
		fprintf(stderr, "FAIL: kmod-build ended in state '%s', expected installed\n", state);
		ok = 0;
	}

	if (ok) {
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/hostbuild/kernel", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET hostbuild/kernel after completion, status=%d\n", r.status);
			ok = 0;
		} else {
			const char *ap = json_as_string(json_object_get(r.json, "artifact_path"));

			if (ap == NULL) {
				fprintf(stderr, "FAIL: artifact_path is null after completion\n");
				ok = 0;
			} else {
				snprintf(artifact_path, sizeof(artifact_path), "%s", ap);
			}
		}
		kx_response_free(&r);
	}

	if (ok && artifact_path[0] != '\0') {
		char symbols_path[PATH_MAX];
		char symbols_content[256];
		struct stat st;
		char hello_path[PATH_MAX];

		snprintf(hello_path, sizeof(hello_path), "%s/hello", artifact_path);
		if (stat(hello_path, &st) != 0 || !S_ISREG(st.st_mode)) {
			fprintf(stderr, "FAIL: kmod-build artifact 'hello' missing on disk at '%s'\n",
			        hello_path);
			ok = 0;
		}

		snprintf(symbols_path, sizeof(symbols_path), "%s/symbols.txt", artifact_path);
		if (read_file_string(symbols_path, symbols_content, sizeof(symbols_content)) != 0) {
			fprintf(stderr, "FAIL: could not read symbols.txt at '%s'\n", symbols_path);
			ok = 0;
		} else if (strcmp(symbols_content, "CONFIG_FOO CONFIG_BAR") != 0) {
			fprintf(stderr,
			        "FAIL: THINC_KMOD_EXTRA_SYMBOLS did not reach the build container "
			        "correctly -- got '%s', expected 'CONFIG_FOO CONFIG_BAR'\n",
			        symbols_content);
			ok = 0;
		}
	}

	if (ok)
		printf("KMOD BUILD RESULT: PASS\n");
	else
		printf("KMOD BUILD RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	run_cmd("rm -rf '%s'", scratch_dir);
	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
