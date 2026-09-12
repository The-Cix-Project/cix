/*
 * Phase 15 (ADR-0030) end-to-end test: proves per-container config
 * files ("files" on POST /v1/containers -- daemon/src/main.c's own
 * pre-clone3() upperdir staging) and generalized sysctls ("sysctls",
 * daemon/src/main.c + src/container_net.c's container_net_apply_sysctl())
 * over real HTTP against a live daemon, with a real daemon restart to
 * prove files/sysctls survive a restart:"always" replay too.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7631
#define PORT_ARG "--port=7631"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];
static char g_container_defs_path[PATH_MAX];
static char g_containers_dir[PATH_MAX];

static long json_num_field(const struct json_value *obj, const char *key)
{
	return (long)json_as_number(json_object_get(obj, key));
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

static void reset_state(void)
{
	char cmd[PATH_MAX * 2];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_container_defs_path);
	system(cmd);
	/*
	 * registry_remove() never deletes a container's own upper/work/
	 * merged directories on disk (a real, pre-existing, separate gap --
	 * not this test's job to fix) -- so a stale directory from an
	 * earlier run (manual or automated) under one of these exact names
	 * could otherwise make open(O_CREAT|O_TRUNC, mode)'s own mode
	 * argument silently be a no-op (mode is only applied when a file is
	 * newly created, never on an existing one), producing a false
	 * failure that has nothing to do with today's actual behavior.
	 */
	snprintf(cmd, sizeof(cmd),
	         "rm -rf '%s/filetest' '%s/sysctltest' '%s/badpath1' '%s/badpath2' "
	         "'%s/badsysctl1' '%s/badsysctl2' '%s/persisttest' '%s/readtest'",
	         g_containers_dir, g_containers_dir, g_containers_dir, g_containers_dir,
	         g_containers_dir, g_containers_dir, g_containers_dir, g_containers_dir);
	system(cmd);
}

static long fetch_pid(const struct cix_client *c, const char *name)
{
	char path[128];
	struct cix_response r;
	long pid = -1;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200)
		pid = json_num_field(r.json, "pid");
	cix_response_free(&r);
	return pid;
}

/* Forks a helper that setns()'s into pid's own /proc/<pid>/ns/net and
 * reads sysctl_path back -- proving a sysctl actually landed inside
 * THAT container's netns specifically (not just that the write
 * syscall succeeded somewhere), without ever moving this test process
 * itself into another netns (the helper's own netns membership dies
 * with it on exit). Returns the read value in out_value, or -1 on any
 * failure. */
static int read_sysctl_in_netns(long pid, const char *sysctl_path, char *out_value,
                                  size_t out_size)
{
	int pipefd[2];
	pid_t child;

	if (pipe(pipefd) != 0) {
		perror("pipe");
		return -1;
	}

	child = fork();
	if (child < 0) {
		perror("fork");
		return -1;
	}
	if (child == 0) {
		char ns_path[128];
		int nsfd;
		int sfd;
		char buf[64];
		ssize_t n;

		close(pipefd[0]);
		snprintf(ns_path, sizeof(ns_path), "/proc/%ld/ns/net", pid);
		nsfd = open(ns_path, O_RDONLY);
		if (nsfd < 0) {
			perror("open netns");
			_exit(1);
		}
		if (setns(nsfd, CLONE_NEWNET) != 0) {
			perror("setns");
			_exit(1);
		}
		sfd = open(sysctl_path, O_RDONLY);
		if (sfd < 0) {
			perror("open sysctl");
			_exit(1);
		}
		n = read(sfd, buf, sizeof(buf));
		if (n <= 0) {
			perror("read sysctl");
			_exit(1);
		}
		/* Raw bytes only -- the parent's own read() length is what
		 * determines out_value's real extent, not a NUL sent over the
		 * pipe (which would just be one more ordinary byte, not a
		 * sentinel, and would throw off the parent's own trailing-
		 * newline check by one position). */
		if (write(pipefd[1], buf, (size_t)n) < 0)
			_exit(1);
		_exit(0);
	}

	close(pipefd[1]);
	{
		ssize_t n = read(pipefd[0], out_value, out_size - 1);
		int status;

		close(pipefd[0]);
		waitpid(child, &status, 0);
		if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
			return -1;
		out_value[n] = '\0';
		/* Trim a trailing newline, if any, for a clean string compare. */
		if (out_value[n - 1] == '\n')
			out_value[n - 1] = '\0';
	}
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	struct cix_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/filestest/v1/rootfs", g_data_dir);
	snprintf(g_container_defs_path, sizeof(g_container_defs_path), "%s/state/container_defs.json",
	         g_data_dir);
	snprintf(g_containers_dir, sizeof(g_containers_dir), "%s/containers", g_data_dir);

	reset_state();
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/filestest", g_data_dir);
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

	/* 1. a real file lands at the right host path, with the right
	 * content and mode. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"filetest\",\"image\":\"filestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],"
	                       "\"files\":[{\"path\":\"/etc/bird.conf\","
	                       "\"content\":\"router id 1.1.1.1;\\n\",\"mode\":\"0640\"}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST filetest, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *files = json_object_get(r.json, "files");

		if (files == NULL || files->type != JSON_ARRAY || files->u.array.count != 1 ||
		    strcmp(json_as_string(files->u.array.items[0]), "/etc/bird.conf") != 0) {
			fprintf(stderr, "FAIL: create response did not echo the staged file path\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	{
		char bird_conf_path[PATH_MAX];
		FILE *f;
		char buf[128];
		struct stat st;

		snprintf(bird_conf_path, sizeof(bird_conf_path), "%s/filetest/upper/etc/bird.conf",
		         g_containers_dir);
		f = fopen(bird_conf_path, "r");

		if (f == NULL) {
			fprintf(stderr, "FAIL: staged file not found on disk\n");
			ok = 0;
		} else {
			if (fgets(buf, sizeof(buf), f) == NULL ||
			    strcmp(buf, "router id 1.1.1.1;\n") != 0) {
				fprintf(stderr, "FAIL: staged file content wrong: '%s'\n", buf);
				ok = 0;
			}
			fclose(f);
		}
		if (stat(bird_conf_path, &st) != 0 || (st.st_mode & 0777) != 0640) {
			fprintf(stderr, "FAIL: staged file mode wrong\n");
			ok = 0;
		}
	}

	/* 1b. GET .../files?path=... (ADR-0055) while the container is
	 * still running returns the exact staged bytes, via the daemon's
	 * own /proc/<pid>/root branch -- proven with a real byte-for-byte
	 * comparison, not just a 200. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/filetest/files?path=%2Fetc%2Fbird.conf",
	                       NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET files (running) status=%d\n", r.status);
		ok = 0;
	} else if (r.body == NULL || r.body_len != strlen("router id 1.1.1.1;\n") ||
	           memcmp(r.body, "router id 1.1.1.1;\n", r.body_len) != 0) {
		fprintf(stderr, "FAIL: GET files (running) wrong content\n");
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * 1c. #343: every container has a /tmp, and it is a directory.
	 *
	 * These images never had one and it cost three separate
	 * investigations, the last being hostapd_cli -- which creates its
	 * own client socket before connecting and hardcodes that path to
	 * /tmp, so it reported "Could not connect to hostapd" while hostapd
	 * was healthy throughout. The failure always names what could not
	 * be reached, never the directory that was missing.
	 *
	 * Asserted exactly as the issue measured it: 400 "path is a
	 * directory, not a file" is what /run already answers and what a
	 * present /tmp must answer too. A 404 here is the bug.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/filetest/files?path=%2Ftmp", NULL,
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: /tmp in a running container: expected 400 (a directory), got %d"
		                " -- 404 means the container has no /tmp (#343)\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* A file really can be written there, which is the thing software
	 * actually wants -- a directory that exists but is not writable
	 * would satisfy the check above and none of the callers. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT",
	                       "/v1/containers/filetest/files?path=%2Ftmp%2Fprobe.txt",
	                       "{\"content\":\"tmp is writable\\n\"}", &r) != 0 ||
	    (r.status != 204 && r.status != 200)) {
		fprintf(stderr, "FAIL: writing /tmp/probe.txt in a running container, status=%d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/filetest", NULL, &r);
	cix_response_free(&r);

	/* 2. path traversal rejected -- both a ".." component and a
	 * missing leading '/'. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"badpath1\",\"image\":\"filestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],"
	                       "\"files\":[{\"path\":\"/../../etc/passwd\",\"content\":\"x\"}]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: files path traversal expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"badpath2\",\"image\":\"filestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],"
	                       "\"files\":[{\"path\":\"etc/passwd\",\"content\":\"x\"}]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: files no-leading-slash expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. a sysctl actually lands inside THIS container's own netns,
	 * not the host's. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"sysctltest\",\"image\":\"filestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],"
	                       "\"sysctls\":{\"net.ipv4.conf.all.rp_filter\":\"0\"}}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST sysctltest, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	{
		long pid = fetch_pid(&client, "sysctltest");
		char value[64];

		if (pid < 0) {
			fprintf(stderr, "FAIL: sysctltest has no pid\n");
			ok = 0;
		} else if (read_sysctl_in_netns(pid, "/proc/sys/net/ipv4/conf/all/rp_filter", value,
		                                 sizeof(value)) != 0) {
			fprintf(stderr, "FAIL: could not read rp_filter inside sysctltest's netns\n");
			ok = 0;
		} else if (strcmp(value, "0") != 0) {
			fprintf(stderr, "FAIL: rp_filter inside sysctltest's netns is '%s', expected '0'\n",
			        value);
			ok = 0;
		}
	}

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/sysctltest", NULL, &r);
	cix_response_free(&r);

	/* 4. sysctl key validation: non-net.* and a malformed net.* key. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"badsysctl1\",\"image\":\"filestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],"
	                       "\"sysctls\":{\"vm.swappiness\":\"10\"}}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: non-net.* sysctl expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"badsysctl2\",\"image\":\"filestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],"
	                       "\"sysctls\":{\"net..foo\":\"1\"}}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: malformed sysctl key expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 5. files survive a real daemon restart, correctly re-staged from
	 * the persisted body (ADR-0025's own replay mechanism, no extra
	 * work needed for files -- confirmed here, not just assumed). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"persisttest\",\"image\":\"filestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],\"restart\":\"always\","
	                       "\"files\":[{\"path\":\"/etc/pbr.conf\",\"content\":\"mode=pbr\\n\"}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST persisttest, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (first instance)\n");
		ok = 0;
	}
	{
		char cmd[PATH_MAX + 16];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s/persisttest'", g_containers_dir);
		system(cmd);
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: restarted daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	usleep(500000);

	{
		char pbr_conf_path[PATH_MAX];
		FILE *f;
		char buf[128];

		snprintf(pbr_conf_path, sizeof(pbr_conf_path), "%s/persisttest/upper/etc/pbr.conf",
		         g_containers_dir);
		f = fopen(pbr_conf_path, "r");

		if (f == NULL) {
			fprintf(stderr, "FAIL: persisttest's file was not re-staged after a daemon "
			                "restart\n");
			ok = 0;
		} else {
			if (fgets(buf, sizeof(buf), f) == NULL || strcmp(buf, "mode=pbr\n") != 0) {
				fprintf(stderr, "FAIL: re-staged file content wrong: '%s'\n", buf);
				ok = 0;
			}
			fclose(f);
		}
	}

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/persisttest", NULL, &r);
	cix_response_free(&r);

	/* 6. GET .../files?path=... (ADR-0055) once a container has exited
	 * on its own (registry_mark_exited() -- a natural process exit,
	 * never DELETE, which really does remove the entry) still resolves
	 * a file, falling back first to the real, host-visible upper/ dir,
	 * then to the image's own read-only rootfs/ -- both fallback
	 * branches proven for real, not just one. Also proves 400 on an
	 * unsafe path and 404 for both a missing container and a missing
	 * file. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"readtest\",\"image\":\"filestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"]}],"
	                       "\"files\":[{\"path\":\"/etc/only-in-upper.conf\","
	                       "\"content\":\"from-upper\\n\"}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST readtest, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* A real wait for the child to actually exit and for
	 * handle_container_event() to call registry_mark_exited() -- not
	 * assumed instantaneous. */
	usleep(500000);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET",
	                       "/v1/containers/readtest/files?path=%2Fetc%2Fonly-in-upper.conf", NULL,
	                       &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET files (exited, upper/) status=%d\n", r.status);
		ok = 0;
	} else if (r.body == NULL || r.body_len != strlen("from-upper\n") ||
	           memcmp(r.body, "from-upper\n", r.body_len) != 0) {
		fprintf(stderr, "FAIL: GET files (exited, upper/) wrong content\n");
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/readtest/files?path=%2Fbin%2Fdaemon_child",
	                       NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET files (exited, rootfs/ fallback) status=%d\n", r.status);
		ok = 0;
	} else if (r.body == NULL || r.body_len == 0) {
		fprintf(stderr, "FAIL: GET files (exited, rootfs/ fallback) empty body\n");
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET",
	                       "/v1/containers/readtest/files?path=%2F..%2F..%2Fetc%2Fpasswd", NULL,
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: GET files traversal expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/readtest/files?path=etc%2Fhosts", NULL,
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: GET files no-leading-slash expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET",
	                       "/v1/containers/nosuchcontainer/files?path=%2Fetc%2Fhosts", NULL,
	                       &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: GET files unknown container expected 404, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET",
	                       "/v1/containers/readtest/files?path=%2Fno%2Fsuch%2Ffile", NULL,
	                       &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: GET files missing file expected 404, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/readtest", NULL, &r);
	cix_response_free(&r);

	/* owner/group (ADR-0144): a staged file's real on-disk uid/gid,
	 * checked directly via stat() against the container's own
	 * upperdir -- the GET .../files endpoint reads content, not
	 * metadata, so this is the only way to actually confirm fchown()
	 * took effect rather than just trusting it compiled. */
	{
		struct stat st;
		char owned_path[PATH_MAX];

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"ownertest\",\"image\":\"filestest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],"
		                       "\"files\":[{\"path\":\"/etc/owned\",\"content\":\"x\\n\","
		                       "\"mode\":\"0640\",\"owner\":75,\"group\":76}]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST ownertest, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		snprintf(owned_path, sizeof(owned_path), "%s/ownertest/upper/etc/owned", g_containers_dir);
		if (stat(owned_path, &st) != 0) {
			fprintf(stderr, "FAIL: stat(%s): %s\n", owned_path, strerror(errno));
			ok = 0;
		} else if (st.st_uid != 75 || st.st_gid != 76 || (st.st_mode & 0777) != 0640) {
			fprintf(stderr,
			        "FAIL: owned file got uid=%d gid=%d mode=%o, want uid=75 gid=76 mode=0640\n",
			        (int)st.st_uid, (int)st.st_gid, st.st_mode & 0777);
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/containers/ownertest", NULL, &r);
		cix_response_free(&r);
	}

	/* 8. PUT .../files?path=... (ADR-0153) writes a new file into a
	 * still-running container via /proc/<pid>/root -- the overlay
	 * copy-up lands it in the real, host-visible upper/ dir, checked
	 * directly with stat()+fopen(), and a follow-up GET proves the
	 * daemon's own read path sees the exact same bytes. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"writetest\",\"image\":\"filestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST writetest, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT",
	                       "/v1/containers/writetest/files?path=%2Fetc%2Flive.conf",
	                       "{\"content\":\"hello live world\\n\",\"mode\":\"0640\","
	                       "\"owner\":75,\"group\":76}",
	                       &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: PUT files (running) status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	{
		char live_conf_path[PATH_MAX];
		FILE *f;
		char buf[128];
		struct stat st;

		snprintf(live_conf_path, sizeof(live_conf_path), "%s/writetest/upper/etc/live.conf",
		         g_containers_dir);
		f = fopen(live_conf_path, "r");
		if (f == NULL) {
			fprintf(stderr, "FAIL: PUT'd file not found on disk (running)\n");
			ok = 0;
		} else {
			if (fgets(buf, sizeof(buf), f) == NULL || strcmp(buf, "hello live world\n") != 0) {
				fprintf(stderr, "FAIL: PUT'd file content wrong (running): '%s'\n", buf);
				ok = 0;
			}
			fclose(f);
		}
		if (stat(live_conf_path, &st) != 0) {
			fprintf(stderr, "FAIL: stat(%s): %s\n", live_conf_path, strerror(errno));
			ok = 0;
		} else if (st.st_uid != 75 || st.st_gid != 76 || (st.st_mode & 0777) != 0640) {
			fprintf(stderr,
			        "FAIL: PUT'd file got uid=%d gid=%d mode=%o, want uid=75 gid=76 mode=0640\n",
			        (int)st.st_uid, (int)st.st_gid, st.st_mode & 0777);
			ok = 0;
		}
	}

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/writetest/files?path=%2Fetc%2Flive.conf",
	                       NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET files after PUT (running) status=%d\n", r.status);
		ok = 0;
	} else if (r.body == NULL || r.body_len != strlen("hello live world\n") ||
	           memcmp(r.body, "hello live world\n", r.body_len) != 0) {
		fprintf(stderr, "FAIL: GET files after PUT (running) wrong content\n");
		ok = 0;
	}
	cix_response_free(&r);

	/* overwriting an existing file replaces its content, not appends. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT",
	                       "/v1/containers/writetest/files?path=%2Fetc%2Flive.conf",
	                       "{\"content\":\"replaced\\n\"}", &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: PUT files (overwrite) status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/writetest/files?path=%2Fetc%2Flive.conf",
	                       NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET files after overwrite status=%d\n", r.status);
		ok = 0;
	} else if (r.body == NULL || r.body_len != strlen("replaced\n") ||
	           memcmp(r.body, "replaced\n", r.body_len) != 0) {
		fprintf(stderr, "FAIL: GET files after overwrite wrong content ('%.*s')\n",
		        (int)r.body_len, r.body ? r.body : "");
		ok = 0;
	}
	cix_response_free(&r);

	/* path traversal / missing leading slash / missing content / unknown
	 * container all rejected the same way the read side already is. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT",
	                       "/v1/containers/writetest/files?path=%2F..%2F..%2Fetc%2Fpasswd",
	                       "{\"content\":\"x\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: PUT files traversal expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/containers/writetest/files?path=etc%2Fhosts",
	                       "{\"content\":\"x\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: PUT files no-leading-slash expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/containers/writetest/files?path=%2Fetc%2Fx", "{}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: PUT files missing content expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT",
	                       "/v1/containers/nosuchcontainer/files?path=%2Fetc%2Fx",
	                       "{\"content\":\"x\"}", &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: PUT files unknown container expected 404, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/writetest", NULL, &r);
	cix_response_free(&r);

	/* 9. PUT against a container that has exited on its own (the !running
	 * branch, writing straight into upper/ rather than through
	 * /proc/<pid>/root) -- and confirms the write is live/ephemeral: it
	 * never touches the container's own persisted files[] body, so it is
	 * NOT what a recreate would replay (that durability boundary belongs
	 * to a container recipe instead, by design -- see ADR-0153). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"writetest2\",\"image\":\"filestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST writetest2, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	usleep(500000);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT",
	                       "/v1/containers/writetest2/files?path=%2Fetc%2Fstopped.conf",
	                       "{\"content\":\"written while stopped\\n\"}", &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: PUT files (!running) status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET",
	                       "/v1/containers/writetest2/files?path=%2Fetc%2Fstopped.conf", NULL,
	                       &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET files after PUT (!running) status=%d\n", r.status);
		ok = 0;
	} else if (r.body == NULL || r.body_len != strlen("written while stopped\n") ||
	           memcmp(r.body, "written while stopped\n", r.body_len) != 0) {
		fprintf(stderr, "FAIL: GET files after PUT (!running) wrong content\n");
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/writetest2", NULL, &r);
	cix_response_free(&r);

	/*
	 * 10 (#418). A container whose own definition declares the
	 * ldap_server role must come up with the record set already
	 * rendered into its config file.
	 *
	 * It did not. create_container_persisted() ends with
	 * resync_managed_services(), and register_declared_server_roles()
	 * runs AFTER that -- so the sync iterated bindings that did not yet
	 * include this container, and the write that would have seeded it
	 * lived only in handle_ldap_server_create() (POST /ldap/servers).
	 * Measured on 192.168.15.95, 2026-09-12: a recreated ldap-2 had 0
	 * [[users]] stanzas against an untouched ldap-1's 3, and an
	 * ldapsearch bind with the CORRECT password returned 49 there and 0
	 * here. Both carry follow_rolling, so an ordinary image update
	 * silently emptied the directory it rebuilt -- while the container
	 * reported "running", listened, and presented a valid certificate.
	 *
	 * Asserted against the staged file on the host rather than the
	 * files API, which does not necessarily read the container's own
	 * tree (#394). The check is that the user's own name appears: an
	 * empty [[users]] section and a populated one are the whole
	 * difference between every bind failing and every bind working, and
	 * "the file exists" cannot tell them apart.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/ldap/groups",
	                       "{\"name\":\"seedgroup\",\"gidnumber\":8100}", &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create group seedgroup, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/ldap/users",
	                       "{\"name\":\"seeduser\",\"uidnumber\":8100,"
	                       "\"primarygroup\":8100,\"password\":\"seed-pw\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create user seeduser, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(
	        &client, "POST", "/v1/containers",
	        "{\"name\":\"ldapseed\",\"image\":\"filestest\","
	        "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	        "\"ldap_server\":{\"config_path\":\"/etc/glauth.cfg\"},"
	        "\"files\":[{\"path\":\"/etc/glauth.cfg\","
	        "\"content\":\"watchconfig = true\\n\\n[backend]\\n  datastore = \\\"config\\\"\\n\"}]}",
	        &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST ldapseed, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	{
		char cfg_path[PATH_MAX];
		FILE *f;

		snprintf(cfg_path, sizeof(cfg_path), "%s/ldapseed/upper/etc/glauth.cfg",
		         g_containers_dir);
		f = fopen(cfg_path, "r");
		if (f == NULL) {
			fprintf(stderr, "FAIL: ldapseed's glauth config not staged at all\n");
			ok = 0;
		} else {
			char buf[8192];
			size_t n = fread(buf, 1, sizeof(buf) - 1, f);
			int has_stanza, has_user;

			buf[n] = '\0';
			fclose(f);
			has_stanza = strstr(buf, "[[users]]") != NULL;
			has_user = strstr(buf, "seeduser") != NULL;
			if (!has_stanza || !has_user) {
				fprintf(stderr,
				        "FAIL: a declared ldap_server role came up unseeded "
				        "([[users]]=%d seeduser=%d) -- every bind against it would be "
				        "refused with invalidCredentials (#418)\n",
				        has_stanza, has_user);
				ok = 0;
			}
		}
	}

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/ldapseed", NULL, &r);
	cix_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (second instance)\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "CONTAINER FILES RESULT: PASS\n" : "CONTAINER FILES RESULT: FAIL\n");
	return ok ? 0 : 1;
}
