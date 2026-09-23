/*
 * ADR-0159 Phase B end-to-end test: POST /v1/system/kmod-build, an
 * ordinary hostbuild against the "kernel" recipe (the exact existing
 * pkg_hostbuild_start() mechanism test_pkg.c's own step 17 already
 * proves generically) gaining an optional config_symbols parameter
 * cixd writes into the build container as /build/extra/kmod-extra.config
 * (#412 -- previously threaded through as a CIX_KMOD_EXTRA_SYMBOLS
 * environment variable for the recipe to re-derive the same file
 * from; cixd owns /build/extra already, ADR-0036, and writes it
 * directly now).
 *
 * Not a real kernel build (far too slow for this suite, and the real
 * 7.2.3 kernel.recipe lives in this project's own git-tracked
 * recipes/, entirely outside this test's isolated --data-dir=
 * recipes_dir) -- a tiny fixture recipe literally named "kernel"
 * (isolated to this test's own recipes_dir, no collision with the real
 * one) whose pkg_build() copies whatever cixd staged at
 * /build/extra/kmod-extra.config straight to a file, proving the
 * daemon-side plumbing (pkg_hostbuild_start()'s new parameter,
 * write_kmod_extra_config()) actually carries the REST-supplied
 * symbols through to the real build container's own filesystem --
 * the one thing kernel.recipe's own build.sh change can't be proven
 * any other way in this sandbox.
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

#define TEST_PORT 7684
#define PORT_ARG "--port=7684"

/*
 * Polls of GET /v1/pkg/hostbuild/kernel, 300 ms apart: 600 is three
 * minutes. It was 30 (nine seconds) when the loop ended with the job
 * still "fetching" (probe-cix-testreport@5-1, 192.168.15.95,
 * 2026-09-23); @6-1 showed that was a failed status request ending the
 * wait, not the budget (see the loop). A timeout now says so.
 */
#define KMOD_POLLS 600

static char g_data_dir[PATH_MAX];
static char g_pkg_state_dir[PATH_MAX];

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

	snprintf(out_tarball_path, tarball_path_size, "%s/kernel-1.0.tar", scratch_dir);
	if (run_cmd("tar -cf '%s' -C '%s' 'kernel-1.0'", out_tarball_path, scratch_dir) != 0)
		return -1;

	return compute_file_sha256(out_tarball_path, out_sha256, sha256_size);
}

/*
 * The fixture kernel recipe, in CPDL, because the real kernel recipe is
 * CPDL and this test exists to prove the path the real one takes (#517).
 * Its shell predecessor read /build/extra/kmod-extra.config directly,
 * which a CPDL recipe cannot name: the file is outside the CBS roots.
 * cixd now hands it to CBS as `--input kmod-extra=...`, and the recipe
 * appends it with `args input "kmod-extra"` (cbs v0.1.54,
 * cix-build-system#231).
 *
 * `cat /dev/null <input>` into symbols.txt: with symbols requested the
 * file carries them, and with none the input is absent, args input adds
 * no argument, and symbols.txt is empty -- the same "strictly additive,
 * unset changes nothing" shape the real kernel recipe's merge_config.sh
 * run has.
 *
 * Declares its build tools because a hostbuild composes its build
 * container from them (ADR-0304, #482); the names are the ADR-0209 test
 * floor's own. tcc rather than gcc: gcc is not in the floor, and what
 * this test is about is whether config_symbols reaches the build, not
 * which compiler builds a five-line hello.c.
 */
static int write_kernel_fixture_recipe(const char *tarball_path, const char *sha256)
{
	char dir[PATH_MAX];
	char path[PATH_MAX + 16];
	FILE *f;

	snprintf(dir, sizeof(dir), "%s/recipes/kernel/1.0-1", g_pkg_state_dir);
	if (run_cmd("mkdir -p '%s'", dir) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/build.cbs", dir);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fprintf(f,
	        "package \"kernel\" {\n"
	        "    version \"1.0\"\n"
	        "    release 1\n"
	        "    format \"cixpkg\"\n"
	        "\n"
	        "    sources {\n"
	        "        main \"kernel\" {\n"
	        "            url \"%s\"\n"
	        "            sha256 \"%s\"\n"
	        "        }\n"
	        "    }\n"
	        "\n"
	        "    requires {\n"
	        "        build {\n"
	        "            compiler \"tcc\"\n"
	        "            tool \"linux-headers\"\n"
	        "            tool \"bash\"\n"
	        "            tool \"coreutils\"\n"
	        "            tool \"binutils\"\n"
	        "        }\n"
	        "    }\n"
	        "\n"
	        "    build {\n"
	        "        cd \"${src}/kernel/kernel-1.0\" {\n"
	        "            run \"tcc\" {\n"
	        "                \"-o\" \"hello\" \"hello.c\"\n"
	        "            }\n"
	        "            run \"cat\" {\n"
	        "                \"/dev/null\"\n"
	        "                args input \"kmod-extra\"\n"
	        "                stdout file \"${src}/kernel/kernel-1.0/symbols.txt\"\n"
	        "            }\n"
	        "        }\n"
	        "    }\n"
	        "\n"
	        "    install {\n"
	        "        copy \"${src}/kernel/kernel-1.0/hello\" to \"${dest}/hello\"\n"
	        "        copy \"${src}/kernel/kernel-1.0/symbols.txt\" to \"${dest}/symbols.txt\"\n"
	        "    }\n"
	        "}\n",
	        test_http_src(tarball_path), sha256);
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
	struct cix_client client;
	int ok = 1;
	struct cix_response r;
	char scratch_dir[] = "/tmp/cix_test_kmod_build_XXXXXX";
	char tarball_path[512], sha256[128];
	char artifact_path[PATH_MAX] = "";
	char state[32];
	char kmod_err[400];
	int i;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_pkg_state_dir, sizeof(g_pkg_state_dir), "%s/rebuildable/pkg", g_data_dir);

	/*
	 * ADR-0209: the build floor. The kmod build composes its container
	 * from the fixture recipe's pkg_build_depends, and those resolve
	 * only against packages installed here -- without this the build
	 * failed with 'declared build tool "tcc" is not installed anywhere'
	 * (probe-cix-testreport@4-1, 192.168.15.95, 2026-09-23).
	 */
	if (test_image_fixture_seed_floor_packages(g_data_dir, "build-inputs/floor-artifacts") != 0) {
		fprintf(stderr, "FAIL: could not seed the build floor -- fetch the real package "
		                "artifacts into build-inputs/floor-artifacts first (ADR-0209)\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

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

	/*
	 * No build image is staged. This test used to copy the build
	 * environment's whole toolchain tree into an "hbimage" fixture
	 * image, for the build_image a hostbuild once composed from.
	 * ADR-0304 (#482) retired that: the build composes from the
	 * fixture recipe's pkg_build_depends, installed from the floor
	 * above, and nothing read the fixture any more. It was not free:
	 * the copy put 1,311,776 KiB on /run, which is tmpfs charged to the
	 * build's 2 GiB memory cgroup, and the kernel OOM-killed this
	 * test's daemon (probe-cix-testreport@8-1 and @9-1, 192.168.15.95,
	 * 2026-09-23).
	 */

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

	if (test_floor_install_all(&client) != 0)
		ok = 0;

	/* 1. Error paths first (no job in flight yet for any of these).
	 *
	 * An empty body used to be a 400 here, for a missing build_image;
	 * ADR-0304 (#482) retired that field, so there is nothing left for
	 * this endpoint to require and an empty body is a valid request.
	 * It is deliberately NOT exercised as a success case here -- it
	 * would start a real build and the symbol-passing case below is
	 * the one worth spending a build on. */

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/system/kmod-build",
	                       "{\"config_symbols\":[\"not_a_config_symbol\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr,
		        "FAIL: POST kmod-build with a malformed config_symbols entry: expected 400, "
		        "got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/system/kmod-build",
	                       "{\"config_symbols\":\"CONFIG_FOO\"}", &r) !=
	        0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST kmod-build with non-array config_symbols: expected 400, "
		                "got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 2. The real payoff: config_symbols actually reaches the build
	 * container as /build/extra/kmod-extra.config, one "<SYMBOL>=m"
	 * line per entry (#412). */
	memset(&r, 0, sizeof(r));
	if (ok && (cix_client_request(&client, "POST", "/v1/system/kmod-build",
	                              "{\"config_symbols\":[\"CONFIG_FOO\",\"CONFIG_BAR\"]}",
	                              &r) != 0 ||
	           r.status != 202)) {
		fprintf(stderr, "FAIL: POST kmod-build: expected 202, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	state[0] = '\0';
	kmod_err[0] = '\0';
	for (i = 0; ok && i < KMOD_POLLS; i++) {
		const char *s;

		/*
		 * A status request that fails, or answers without a state,
		 * counts as "not yet" rather than ending the wait. It used to
		 * break, and in probe-cix-testreport@6-1 (192.168.15.95,
		 * 2026-09-23) the loop ended within its budget with the job
		 * still "fetching" -- which only a break can do. Extracting an
		 * artifact runs synchronously in the single-threaded daemon,
		 * so one request can time out while the job is healthy.
		 */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/hostbuild/kernel", NULL, &r) != 0 ||
		    r.status != 200 ||
		    (s = json_as_string(json_object_get(r.json, "state"))) == NULL) {
			cix_response_free(&r);
			usleep(300000);
			continue;
		}
		/* Each transition, timed and on stderr (unbuffered), so a run
		 * killed from outside still shows how far the job got
		 * (probe-cix-testreport@7-1 timed out with no trace of it). */
		if (strcmp(state, s) != 0)
			fprintf(stderr, "    kmod-build: %s at poll %d (300 ms apart)\n", s, i);
		snprintf(state, sizeof(state), "%s", s);
		/* Kept past the free, so a failure can say why (cix-tests
		 * v2.57.251-1 reported only the final state). */
		s = json_as_string(json_object_get(r.json, "error"));
		snprintf(kmod_err, sizeof(kmod_err), "%s", s != NULL ? s : "");
		cix_response_free(&r);
		if (strcmp(state, "fetching") != 0 && strcmp(state, "building") != 0)
			break;
		usleep(300000);
	}
	if (ok && i == KMOD_POLLS) {
		fprintf(stderr, "FAIL: kmod-build still in state '%s' after %d s\n", state,
		        KMOD_POLLS * 3 / 10);
		ok = 0;
	} else if (ok && strcmp(state, "installed") != 0) {
		fprintf(stderr, "FAIL: kmod-build ended in state '%s', expected installed: %s\n", state,
		        kmod_err[0] != '\0' ? kmod_err : "(no error field)");
		ok = 0;
	}

	if (ok) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/hostbuild/kernel", NULL, &r) != 0 ||
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
		cix_response_free(&r);
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
		} else if (strcmp(symbols_content, "CONFIG_FOO=m\nCONFIG_BAR=m\n") != 0) {
			fprintf(stderr,
			        "FAIL: kmod-extra.config did not reach the build container "
			        "correctly -- got '%s', expected 'CONFIG_FOO=m\\nCONFIG_BAR=m\\n'\n",
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
