/*
 * Phase 4 end-to-end test: proves the real, shipped thincctl binary
 * works against a real daemon over real HTTP -- not just that
 * client/src/httpclient.c works (that's already exercised by
 * test_daemon.c). Stages the same shared test image, forks the
 * daemon, then forks+execve's build/thincctl itself for each check,
 * capturing its stdout/stderr and exit code.
 */
#include "httpclient.h"
#include "test_image_fixture.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7622
#define PORT_ARG "--port=7622"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

static int wait_for_daemon(int max_attempts)
{
	struct thinc_client c;
	struct thinc_response r;
	int i;

	thinc_client_init(&c, "127.0.0.1", TEST_PORT);
	for (i = 0; i < max_attempts; i++) {
		if (thinc_client_request(&c, "GET", "/v1/health", NULL, &r) == 0) {
			thinc_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

static int run_cli(char *const argv[], char *out, size_t out_size, int *exit_code)
{
	int pipefd[2];
	pid_t pid;
	ssize_t n;
	size_t total = 0;
	int status;

	if (pipe(pipefd) != 0) {
		perror("pipe");
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execve("build/thincctl", argv, environ);
		perror("execve build/thincctl");
		_exit(127);
	}

	close(pipefd[1]);
	while (total + 1 < out_size) {
		n = read(pipefd[0], out + total, out_size - total - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}
	out[total] = '\0';
	close(pipefd[0]);

	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		return -1;
	}
	*exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return 0;
}

static long parse_pid(const char *out)
{
	const char *p = strstr(out, "pid=");

	if (p == NULL)
		return -1;
	return atol(p + 4);
}

/* ADR-0180: rm of a running container is asynchronous -- settle-poll
 * (via inspect exiting nonzero once the name 404s) before deleting a
 * network the container was attached to, or the network rm can
 * transiently 409 off the not-yet-reaped attachment. */
static void wait_rm_settled(const char *name)
{
	char *inspect_argv[] = { "thincctl", PORT_ARG, "container", "inspect", (char *)name, NULL };
	char out[4096];
	int rc, i;

	for (i = 0; i < 50; i++) {
		if (run_cli(inspect_argv, out, sizeof(out), &rc) == 0 && rc != 0)
			return;
		usleep(100 * 1000);
	}
}

int main(void)
{
	pid_t daemon_pid;
	char *dargv[4];
	char data_dir_arg[PATH_MAX + 11];
	char out[4096];
	int rc;
	int ok = 1;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/test/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/test", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}

	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (test_image_fixture_build(g_image_root, "build/net_child", "net_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/thincd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	daemon_pid = fork();
	if (daemon_pid < 0) {
		perror("fork");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (daemon_pid == 0) {
		execve("build/thincd", dargv, environ);
		perror("execve build/thincd");
		_exit(127);
	}

	if (wait_for_daemon(50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. health */
	{
		char *argv[] = { "thincctl", PORT_ARG, "health", NULL };

		if (run_cli(argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl health, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
	}

	/* 2. run c1, exits quickly with code 5 */
	{
		char *argv[] = { "thincctl",  PORT_ARG,
			          "container", "run", "--name=c1",
			          "--image=test", "--",
			          "/bin/daemon_child", "0",
			          "5",         NULL };

		if (run_cli(argv, out, sizeof(out), &rc) != 0 || rc != 0 ||
		    strstr(out, "running") == NULL) {
			fprintf(stderr, "FAIL: thincctl run c1, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
	}

	/* 3. container ls lists it */
	{
		char *argv[] = { "thincctl", PORT_ARG, "container", "ls", NULL };

		if (run_cli(argv, out, sizeof(out), &rc) != 0 || rc != 0 ||
		    strstr(out, "c1") == NULL) {
			fprintf(stderr, "FAIL: thincctl container ls, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
	}

	/* 4. poll inspect c1 until exited, check exit_status */
	{
		char *argv[] = { "thincctl", PORT_ARG, "container", "inspect", "c1", NULL };
		int i;
		int seen = 0;

		for (i = 0; i < 50; i++) {
			if (run_cli(argv, out, sizeof(out), &rc) == 0 && rc == 0 &&
			    strstr(out, "exited") != NULL) {
				seen = 1;
				break;
			}
			usleep(100000);
		}
		if (!seen || strstr(out, "exit_status=5") == NULL) {
			fprintf(stderr, "FAIL: thincctl inspect c1 never showed exited/exit_status=5, out=%s\n",
			        out);
			ok = 0;
		}
	}

	/* 5. run c2 (long-running), rm it while running, confirm PID gone */
	{
		char *run_argv[] = { "thincctl",  PORT_ARG,
			              "container", "run", "--name=c2",
			              "--image=test", "--",
			              "/bin/daemon_child", "30",
			              "0",         NULL };
		char *rm_argv[] = { "thincctl", PORT_ARG, "container", "rm", "c2", NULL };
		char *inspect_argv[] = { "thincctl", PORT_ARG, "container", "inspect", "c2", NULL };
		long pid;
		char proc_path[64];
		struct stat st;

		if (run_cli(run_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl run c2, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
		pid = parse_pid(out);

		if (run_cli(rm_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl rm c2, rc=%d out=%s\n", rc, out);
			ok = 0;
		}

		/* ADR-0180: rm of a running container is asynchronous --
		 * settle-poll until the process is reaped and the name 404s
		 * (bounded tight; >5s for a SIGKILLed child is a regression). */
		{
			int i, torn_down = 0;

			snprintf(proc_path, sizeof(proc_path), "/proc/%ld", pid);
			for (i = 0; i < 50; i++) {
				if ((pid <= 0 || stat(proc_path, &st) != 0) &&
				    (run_cli(inspect_argv, out, sizeof(out), &rc) == 0 && rc != 0)) {
					torn_down = 1;
					break;
				}
				usleep(100 * 1000);
			}
			if (!torn_down) {
				fprintf(stderr, "FAIL: c2 (pid %ld) never fully torn down after rm\n", pid);
				ok = 0;
			}
		}
	}

	/* 6. duplicate name -> nonzero exit, error on output */
	{
		char *argv[] = { "thincctl",  PORT_ARG,
			          "container", "run", "--name=c1",
			          "--image=test", "--",
			          "/bin/daemon_child", "0",
			          "1",         NULL };

		if (run_cli(argv, out, sizeof(out), &rc) != 0 || rc == 0 ||
		    strstr(out, "thincctl:") == NULL) {
			fprintf(stderr, "FAIL: duplicate name should fail with an error, rc=%d out=%s\n",
			        rc, out);
			ok = 0;
		}
	}

	/* 7. unreachable daemon -> nonzero exit, no crash */
	{
		char *argv[] = { "thincctl", "--port=1", "health", NULL };

		if (run_cli(argv, out, sizeof(out), &rc) != 0 || rc != 1) {
			fprintf(stderr, "FAIL: unreachable daemon should exit 1, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
	}

	/* 8. network create, then --network= shows a real assigned ip */
	{
		char *net_create_argv[] = { "thincctl",      PORT_ARG,          "network",
			                     "create",         "--name=clitest",  "--subnet=172.32.0.0",
			                     "--prefix=24",    NULL };
		char *run_argv[] = { "thincctl", PORT_ARG, "container", "run",
			              "--name=c3", "--image=test", "--network=clitest",
			              "--", "/bin/net_child", NULL };
		char *rm_argv[] = { "thincctl", PORT_ARG, "container", "rm", "c3", NULL };
		char *net_rm_argv[] = { "thincctl", PORT_ARG, "network", "rm", "clitest", NULL };

		if (run_cli(net_create_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl network create clitest, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
		if (run_cli(run_argv, out, sizeof(out), &rc) != 0 || rc != 0 ||
		    strstr(out, "networks=-") != NULL || strstr(out, "networks=clitest:172.32.0.") == NULL) {
			fprintf(stderr, "FAIL: thincctl run c3 --network=clitest, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
		if (run_cli(rm_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl rm c3, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
		wait_rm_settled("c3");
		if (run_cli(net_rm_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl network rm clitest, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
	}

	/* 9. repeated --network= flags -> a container attached to two
	 * networks at once, Phase 7 part 2's multi-homing, driven through
	 * the real thincctl binary */
	{
		char *net_create_a_argv[] = { "thincctl",       PORT_ARG,           "network",
			                       "create",          "--name=climulti1", "--subnet=172.36.0.0",
			                       "--prefix=24",     NULL };
		char *net_create_b_argv[] = { "thincctl",       PORT_ARG,           "network",
			                       "create",          "--name=climulti2", "--subnet=172.37.0.0",
			                       "--prefix=24",     NULL };
		char *run_argv[] = { "thincctl",         PORT_ARG,
			              "container", "run", "--name=c4",
			              "--image=test",      "--network=climulti1",
			              "--network=climulti2", "--",
			              "/bin/net_child",    "2",
			              NULL };
		char *rm_argv[] = { "thincctl", PORT_ARG, "container", "rm", "c4", NULL };
		char *net_rm_a_argv[] = { "thincctl", PORT_ARG, "network", "rm", "climulti1", NULL };
		char *net_rm_b_argv[] = { "thincctl", PORT_ARG, "network", "rm", "climulti2", NULL };

		if (run_cli(net_create_a_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl network create climulti1, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
		if (run_cli(net_create_b_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl network create climulti2, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
		if (run_cli(run_argv, out, sizeof(out), &rc) != 0 || rc != 0 ||
		    strstr(out, "networks=climulti1:172.36.0.") == NULL ||
		    strstr(out, "climulti2:172.37.0.") == NULL) {
			fprintf(stderr, "FAIL: thincctl run c4 with two --network= flags, rc=%d out=%s\n", rc,
			        out);
			ok = 0;
		}
		if (run_cli(rm_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl rm c4, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
		wait_rm_settled("c4");
		if (run_cli(net_rm_a_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl network rm climulti1, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
		if (run_cli(net_rm_b_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl network rm climulti2, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
	}

	/* 10. --ip-forward and --route= are accepted and plumbed through
	 * to the daemon -- the full 3-container proof that packets are
	 * actually forwarded already lives in test_daemon_net.c; this just
	 * confirms the real thincctl binary sends these fields correctly */
	{
		char *net_create_argv[] = { "thincctl",      PORT_ARG,          "network",
			                     "create",         "--name=clifwd",   "--subnet=172.38.0.0",
			                     "--prefix=24",    NULL };
		char *run_argv[] = { "thincctl",     PORT_ARG,
			              "container", "run", "--name=c5",
			              "--image=test",  "--network=clifwd",
			              "--ip-forward",  "--route=10.0.0.0/24:172.38.0.1",
			              "--",            "/bin/net_child",
			              NULL };
		char *rm_argv[] = { "thincctl", PORT_ARG, "container", "rm", "c5", NULL };
		char *net_rm_argv[] = { "thincctl", PORT_ARG, "network", "rm", "clifwd", NULL };

		if (run_cli(net_create_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl network create clifwd, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
		if (run_cli(run_argv, out, sizeof(out), &rc) != 0 || rc != 0 ||
		    strstr(out, "fwd=yes") == NULL) {
			fprintf(stderr, "FAIL: thincctl run c5 --ip-forward --route=, rc=%d out=%s\n", rc,
			        out);
			ok = 0;
		}
		if (run_cli(rm_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl rm c5, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
		wait_rm_settled("c5");
		if (run_cli(net_rm_argv, out, sizeof(out), &rc) != 0 || rc != 0) {
			fprintf(stderr, "FAIL: thincctl network rm clifwd, rc=%d out=%s\n", rc, out);
			ok = 0;
		}
	}

	/* 11. rolling-config set --jitter-window-seconds=N (ADR-0124) -- a
	 * real CLI-level round trip through the actual argv parser, not
	 * just the REST layer test_rolling_restart.c already covers; this
	 * caught a real off-by-one in the "--jitter-window-seconds="
	 * prefix length once (24 chars, not 25) that a REST-only test
	 * could never have seen. */
	{
		char *set0_argv[] = { "thincctl", PORT_ARG, "rolling-config", "set",
			               "--jitter-window-seconds=0", NULL };
		char *show_argv[] = { "thincctl", PORT_ARG, "rolling-config", "show", NULL };
		char *set_back_argv[] = { "thincctl", PORT_ARG, "rolling-config", "set",
			                   "--jitter-window-seconds=60", NULL };

		if (run_cli(set0_argv, out, sizeof(out), &rc) != 0 || rc != 0 ||
		    strstr(out, "jitter_window_seconds=0") == NULL) {
			fprintf(stderr, "FAIL: thincctl rolling-config set --jitter-window-seconds=0, rc=%d out=%s\n",
			        rc, out);
			ok = 0;
		}
		if (run_cli(show_argv, out, sizeof(out), &rc) != 0 || rc != 0 ||
		    strstr(out, "jitter_window_seconds=0") == NULL) {
			fprintf(stderr, "FAIL: thincctl rolling-config show after set 0, rc=%d out=%s\n",
			        rc, out);
			ok = 0;
		}
		if (run_cli(set_back_argv, out, sizeof(out), &rc) != 0 || rc != 0 ||
		    strstr(out, "jitter_window_seconds=60") == NULL) {
			fprintf(stderr, "FAIL: thincctl rolling-config set --jitter-window-seconds=60, rc=%d out=%s\n",
			        rc, out);
			ok = 0;
		}
	}

	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);

	printf(ok ? "CLI RESULT: PASS\n" : "CLI RESULT: FAIL\n");
	return ok ? 0 : 1;
}
