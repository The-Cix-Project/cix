/*
 * ADR-0375: containers survive cixd execing itself in place.
 *
 * The daemon runs with --test-handover-after=N. Before then the test sets
 * up, on one network:
 *
 *   - hoa, running two services: "main" (stats_child, which runs until
 *     killed) and "tick" (ticker_child, which spends nearly all its life
 *     with a line half written), with capture on;
 *   - hox, whose one service exits at once: an exited, kept container;
 *   - hoc, restart:"always" with a long delay, killed so that its
 *     restart is still pending when the daemon hands over.
 *
 * When the handed-over daemon logs "took over", it asserts:
 *
 *   - hoa is listed with the same pid and address, still running -- the
 *     containers stayed the process's children, which is why this is not
 *     the adopt ADR-0247 removed;
 *   - its stats answer, and carry a load figure after the handover if
 *     they did before (procfuse's page was handed over as a memfd);
 *   - every line "tick" wrote is whole in the capture, and one ends
 *     after the handover: the half line the old daemon held was carried
 *     and the pipe was passed;
 *   - hox is still exited, with its status and its capture;
 *   - hoc is still down at the handover and comes back on its own: the
 *     pending restart was re-armed from its definition;
 *   - a new container on the same network gets a different address;
 *   - a stop through the API reaches cix-init over the passed socket
 *     and the exit is reaped through the reacquired pidfd.
 *
 * It ends with no drift, and then the negative case: a record naming a
 * running container whose pid is dead makes a --handover start exit 1.
 */
#include "handover.h"
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"
#include "test_drift.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7693
#define PORT_ARG "--port=7693"
#define HANDOVER_AFTER "--test-handover-after=12"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

static int wait_for_daemon(const struct cix_client *c, int max_attempts)
{
	int i;
	struct cix_response r;

	for (i = 0; i < max_attempts; i++) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			cix_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

/* build/cixd on this test's data dir, with extra (NULL or one argument),
 * its stderr to errlog when that is not NULL. */
static pid_t start_daemon(const char *extra, const char *errlog)
{
	pid_t pid;
	char *dargv[5];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = (char *)extra;
	dargv[4] = NULL;
	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		if (errlog != NULL) {
			int fd = open(errlog, O_CREAT | O_WRONLY | O_TRUNC, 0644);

			if (fd >= 0) {
				dup2(fd, STDERR_FILENO);
				close(fd);
			}
		}
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
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

struct seen {
	int found; /* 200 */
	char status[32];
	long long pid;
	long long exit_status;
	char ip[32];
	char captured[4096];
};

/* GET /v1/containers/NAME into *s. 0 for an answer (found or 404), -1. */
static int read_container(const struct cix_client *c, const char *name, struct seen *s)
{
	struct cix_response r;
	char path[128];
	const struct json_value *nets;
	int rc = -1;

	memset(s, 0, sizeof(*s));
	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", path, NULL, &r) == 0) {
		rc = 0;
		if (r.status == 200 && r.json != NULL) {
			const char *st = json_as_string(json_object_get(r.json, "status"));
			const char *cap = json_as_string(json_object_get(r.json, "captured_output"));

			s->found = 1;
			snprintf(s->status, sizeof(s->status), "%s", st != NULL ? st : "");
			s->pid = (long long)json_as_number(json_object_get(r.json, "pid"));
			s->exit_status = (long long)json_as_number(json_object_get(r.json, "exit_status"));
			snprintf(s->captured, sizeof(s->captured), "%s", cap != NULL ? cap : "");
			nets = json_object_get(r.json, "networks");
			if (nets != NULL && nets->type == JSON_ARRAY && nets->u.array.count > 0) {
				const char *a =
				    json_as_string(json_object_get(nets->u.array.items[0], "ip"));

				snprintf(s->ip, sizeof(s->ip), "%s", a != NULL ? a : "");
			}
		} else if (r.status != 404) {
			rc = -1;
		}
	}
	cix_response_free(&r);
	return rc;
}

static int running(const struct seen *s)
{
	return s->found && strcmp(s->status, "running") == 0;
}

/* Has the handed-over daemon logged its takeover? */
static int took_over(const struct cix_client *c)
{
	struct cix_response r;
	int seen;

	memset(&r, 0, sizeof(r));
	seen = cix_client_request(c, "GET", "/v1/system/logs?source=cixd&regex=took%20over&tail=5",
	                          NULL, &r) == 0 &&
	       r.body != NULL && strstr(r.body, "handover: took over") != NULL;
	cix_response_free(&r);
	return seen;
}

/* GET /v1/system/kmod-scan's body into out. 0, or -1. */
static int read_kmodscan(const struct cix_client *c, char *out, size_t out_size)
{
	struct cix_response r;
	int rc = -1;

	out[0] = '\0';
	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", "/v1/system/kmod-scan", NULL, &r) == 0 && r.status == 200 &&
	    r.body != NULL) {
		snprintf(out, out_size, "%s", r.body);
		rc = 0;
	}
	cix_response_free(&r);
	return rc;
}

/* Do hoa's stats carry a load figure? */
static int has_load(const struct cix_client *c)
{
	struct cix_response r;
	int yes;

	memset(&r, 0, sizeof(r));
	yes = cix_client_request(c, "GET", "/v1/containers/hoa/stats", NULL, &r) == 0 &&
	      r.status == 200 && r.json != NULL && json_object_get(r.json, "load") != NULL;
	cix_response_free(&r);
	return yes;
}

/*
 * The "tick" service's lines in a capture: how many are whole
 * ("tick: handover-tick N begin end"), and how many are not -- a line
 * the handover split reads " end" on its own.
 */
static void count_ticks(const char *captured, int *whole, int *broken)
{
	const char *p = captured;

	*whole = *broken = 0;
	while (*p != '\0') {
		const char *nl = strchr(p, '\n');
		size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);

		if (len >= 6 && strncmp(p, "tick: ", 6) == 0) {
			if (len > 16 && strncmp(p + 6, "handover-tick ", 14) == 0 && len >= 10 &&
			    strncmp(p + len - 10, " begin end", 10) == 0)
				(*whole)++;
			else
				(*broken)++;
		}
		if (nl == NULL)
			break;
		p = nl + 1;
	}
}

static int post(const struct cix_client *c, const char *path, const char *body, int want,
                const char *what)
{
	struct cix_response r;
	int ok;

	memset(&r, 0, sizeof(r));
	ok = cix_client_request(c, "POST", path, body, &r) == 0 && r.status == want;
	if (!ok)
		fprintf(stderr, "FAIL: %s, status=%d %.300s\n", what, r.status,
		        r.body != NULL ? r.body : "");
	cix_response_free(&r);
	return ok;
}

/*
 * The negative case: a record whose running container's pid is dead.
 * A --handover start must refuse it and exit 1 rather than serve.
 */
static int dead_pid_refused(void)
{
	struct handover_record rec;
	char path[PATH_MAX], err[256], errlog[PATH_MAX + 32], said[4096];
	pid_t dead, d;
	int status, i, ok = 0;

	dead = fork();
	if (dead == 0)
		_exit(0);
	waitpid(dead, NULL, 0);
	if (handover_record_init(&rec, 1) != 0)
		return 0;
	snprintf(rec.written_by, sizeof(rec.written_by), "test_handover");
	rec.written_at = (long long)time(NULL);
	rec.count = 1;
	snprintf(rec.containers[0].name, sizeof(rec.containers[0].name), "ghost");
	snprintf(rec.containers[0].image, sizeof(rec.containers[0].image), "hotest");
	rec.containers[0].running = 1;
	rec.containers[0].pid = dead;
	rec.containers[0].fd_init_control = rec.containers[0].fd_init_report = -1;
	rec.containers[0].fd_capture = rec.containers[0].fd_bpf_prog = -1;
	snprintf(path, sizeof(path), "%s/state/%s", g_data_dir, HANDOVER_FILE);
	if (handover_write(path, &rec, err, sizeof(err)) != 0) {
		fprintf(stderr, "FAIL: writing the dead-pid record: %s\n", err);
		handover_record_free(&rec);
		return 0;
	}
	handover_record_free(&rec);

	snprintf(errlog, sizeof(errlog), "%s/dead-pid.stderr", g_data_dir);
	d = start_daemon("--handover", errlog);
	for (i = 0; i < 300; i++) {
		pid_t w = waitpid(d, &status, WNOHANG);

		if (w == d) {
			ok = WIFEXITED(status) && WEXITSTATUS(status) == 1;
			if (!ok)
				fprintf(stderr, "FAIL: a dead-pid record: cixd --handover ended with status "
				                "0x%x, expected exit 1\n",
				        status);
			break;
		}
		usleep(100000);
	}
	if (i == 300) {
		fprintf(stderr, "FAIL: cixd --handover served on a record naming a dead pid\n");
		kill(d, SIGKILL);
		waitpid(d, NULL, 0);
	}
	/* Refused for the reason the record gives, not for any other. */
	if (ok) {
		FILE *f = fopen(errlog, "r");
		size_t n = f != NULL ? fread(said, 1, sizeof(said) - 1, f) : 0;

		said[n] = '\0';
		if (f != NULL)
			fclose(f);
		if (strstr(said, "container ghost") == NULL || strstr(said, "not alive") == NULL) {
			fprintf(stderr, "FAIL: a dead-pid record was refused, but not for the dead pid: "
			                "%.600s\n",
			        said);
			ok = 0;
		}
	}
	unlink(path);
	return ok;
}

int main(void)
{
	struct cix_client client;
	struct cix_response r;
	struct seen a, x, cc, b;
	pid_t daemon_pid;
	int ok = 1, i, had_load = 0, whole_before = 0, whole = 0, broken = 0;
	long long pid_a = 0, x_status = 0, pid_c = 0;
	char ip_a[32] = "";
	static char kmod_before[8192], kmod_after[8192];
	time_t handed;

	memset(&a, 0, sizeof(a));
	memset(&x, 0, sizeof(x));
	memset(&cc, 0, sizeof(cc));
	memset(&b, 0, sizeof(b));
	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/hotest/v1/rootfs",
	         g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/hotest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0 ||
		    test_image_fixture_build(g_image_root, "build/stats_child", "stats_child") != 0 ||
		    test_image_fixture_build(g_image_root, "build/ticker_child", "ticker_child") != 0 ||
		    test_image_fixture_build(g_image_root, "build/output_child", "output_child") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}
	daemon_pid = start_daemon(HANDOVER_AFTER, NULL);
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

	ok = post(&client, "/v1/networks",
	          "{\"name\":\"honet\",\"subnet\":\"172.41.0.0\",\"prefix_len\":24}", 201,
	          "POST /v1/networks") &&
	     post(&client, "/v1/containers",
	          "{\"name\":\"hoa\",\"image\":\"hotest\",\"capture_output\":true,\"services\":["
	          "{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/stats_child\"]},"
	          "{\"name\":\"tick\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/ticker_child\"]}"
	          "],\"memory_max\":67108864,\"networks\":[\"honet\"]}",
	          201, "POST hoa") &&
	     post(&client, "/v1/containers",
	          "{\"name\":\"hox\",\"image\":\"hotest\",\"capture_output\":true,\"services\":["
	          "{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/output_child\"]}"
	          "]}",
	          201, "POST hox") &&
	     post(&client, "/v1/containers",
	          "{\"name\":\"hoc\",\"image\":\"hotest\",\"restart\":\"always\","
	          "\"restart_delay_seconds\":15,\"services\":["
	          "{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/stats_child\"]}"
	          "],\"memory_max\":67108864}",
	          201, "POST hoc");

	if (ok && (read_container(&client, "hoa", &a) != 0 || !running(&a) || a.pid <= 0 ||
	           a.ip[0] == '\0')) {
		fprintf(stderr, "FAIL: hoa before the handover: status=%s pid=%lld ip=%s\n", a.status,
		        a.pid, a.ip);
		ok = 0;
	}
	pid_a = a.pid;
	snprintf(ip_a, sizeof(ip_a), "%s", a.ip);

	/* hox exits at once and is kept. */
	for (i = 0; ok && i < 100; i++) {
		if (read_container(&client, "hox", &x) == 0 && x.found && !running(&x) &&
		    strstr(x.captured, "capture-test-stdout-line") != NULL)
			break;
		usleep(100000);
	}
	if (ok && i == 100) {
		fprintf(stderr, "FAIL: hox never read as exited with its capture: status=%s\n",
		        x.status);
		ok = 0;
	}
	x_status = x.exit_status;

	/* hoc is killed; its restart is 15 s doubled away, past the handover. */
	if (ok && (read_container(&client, "hoc", &cc) != 0 || !running(&cc) || cc.pid <= 0)) {
		fprintf(stderr, "FAIL: hoc before the kill: status=%s pid=%lld\n", cc.status, cc.pid);
		ok = 0;
	}
	if (ok) {
		pid_c = cc.pid;
		kill((pid_t)pid_c, SIGKILL);
		for (i = 0; i < 100; i++) {
			if (read_container(&client, "hoc", &cc) == 0 && !running(&cc))
				break;
			usleep(100000);
		}
		if (i == 100) {
			fprintf(stderr, "FAIL: hoc still running after SIGKILL to pid %lld\n", pid_c);
			ok = 0;
		}
	}

	/* The kernel module scan's report as this cixd holds it, which the
	 * successor must answer with unchanged: it travels in the record. */
	if (ok && read_kmodscan(&client, kmod_before, sizeof(kmod_before)) != 0) {
		fprintf(stderr, "FAIL: GET /v1/system/kmod-scan before the handover\n");
		ok = 0;
	}

	/* The handover: at 12 s the daemon execs itself and logs its takeover.
	 * Meanwhile, note whether hoa's stats carry a load figure. */
	for (i = 0; ok && i < 300 && !took_over(&client); i++) {
		if (!had_load)
			had_load = has_load(&client);
		usleep(200000);
	}
	if (ok && !took_over(&client)) {
		fprintf(stderr, "FAIL: the daemon never logged \"handover: took over\"\n");
		ok = 0;
	}
	handed = time(NULL);

	if (ok) {
		if (read_kmodscan(&client, kmod_after, sizeof(kmod_after)) != 0 ||
		    strcmp(kmod_before, kmod_after) != 0) {
			fprintf(stderr,
			        "FAIL: the kernel module scan's report changed across the handover:\n"
			        "before: %.600s\nafter:  %.600s\n",
			        kmod_before, kmod_after);
			ok = 0;
		}

		/* hoc's restart was pending: it is still down right after. */
		if (read_container(&client, "hoc", &cc) != 0 || running(&cc)) {
			fprintf(stderr, "FAIL: hoc must still be down at the handover: status=%s\n",
			        cc.status);
			ok = 0;
		}

		if (read_container(&client, "hoa", &a) != 0 || !running(&a) || a.pid != pid_a ||
		    strcmp(a.ip, ip_a) != 0) {
			fprintf(stderr,
			        "FAIL: hoa after the handover must be the same running container: "
			        "status=%s pid %lld -> %lld ip %s -> %s\n",
			        a.status, pid_a, a.pid, ip_a, a.ip);
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/containers/hoa/stats", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: hoa's stats after the handover, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		if (had_load) {
			for (i = 0; i < 100 && !has_load(&client); i++)
				usleep(100000);
			if (i == 100) {
				fprintf(stderr, "FAIL: hoa's stats carried a load figure before the "
				                "handover and none after it\n");
				ok = 0;
			}
		}
		printf("handover: hoa's load figure %s\n",
		       had_load ? "checked across the handover"
		                : "was never sampled here (no procfuse), so not checked");

		/* A tick line ends after the handover, and none is split. The
		 * count is taken again now, once the takeover is known, so the
		 * line waited for is one the new cixd read from the passed pipe. */
		if (read_container(&client, "hoa", &a) == 0 && a.found)
			count_ticks(a.captured, &whole_before, &broken);
		for (i = 0; i < 100; i++) {
			if (read_container(&client, "hoa", &a) == 0 && a.found) {
				count_ticks(a.captured, &whole, &broken);
				if (whole > whole_before || broken > 0)
					break;
			}
			usleep(100000);
		}
		if (broken > 0 || whole <= whole_before) {
			fprintf(stderr,
			        "FAIL: hoa's tick lines across the handover: %d whole (%d before), %d "
			        "split; capture:\n%s\n",
			        whole, whole_before, broken, a.captured);
			ok = 0;
		}

		/* hox is the same exited container, capture and all. */
		if (read_container(&client, "hox", &x) != 0 || !x.found || running(&x) ||
		    x.exit_status != x_status ||
		    strstr(x.captured, "capture-test-stdout-line") == NULL) {
			fprintf(stderr,
			        "FAIL: hox after the handover: found=%d status=%s exit_status %lld -> %lld\n",
			        x.found, x.status, x_status, x.exit_status);
			ok = 0;
		}

		/* A new container cannot be given hoa's address. */
		if (!post(&client, "/v1/containers",
		          "{\"name\":\"hob\",\"image\":\"hotest\",\"services\":[{\"name\":\"main\","
		          "\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/stats_child\"]}],"
		          "\"memory_max\":67108864,\"networks\":[\"honet\"]}",
		          201, "POST hob after the handover"))
			ok = 0;
		if (read_container(&client, "hob", &b) != 0 || b.ip[0] == '\0' ||
		    strcmp(b.ip, ip_a) == 0) {
			fprintf(stderr, "FAIL: hob must get its own address, not hoa's (%s): got %s\n",
			        ip_a, b.ip);
			ok = 0;
		}

		/* hoc comes back by itself: one re-armed backoff, 15 s doubled. */
		for (i = 0; i < 600; i++) {
			if (read_container(&client, "hoc", &cc) == 0 && running(&cc))
				break;
			usleep(100000);
		}
		if (!running(&cc) || cc.pid == pid_c) {
			fprintf(stderr,
			        "FAIL: hoc's pending restart was not re-armed: status=%s pid=%lld, %ld s "
			        "after the handover\n",
			        cc.status, cc.pid, (long)(time(NULL) - handed));
			ok = 0;
		}

		/* A stop through the API: cix-init's socket and the pidfd survived. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers/hoa/stop", NULL, &r) != 0 ||
		    (r.status != 200 && r.status != 202 && r.status != 204)) {
			fprintf(stderr, "FAIL: POST hoa/stop after the handover, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		for (i = 0; i < 200; i++) {
			if (read_container(&client, "hoa", &a) != 0 || !running(&a))
				break;
			usleep(100000);
		}
		if (i == 200) {
			fprintf(stderr, "FAIL: hoa was still running 20 s after its stop\n");
			ok = 0;
		}
	}

	{
		static const char *const names[] = { "hoa", "hob", "hoc", "hox" };
		char path[64];

		for (i = 0; i < 4; i++) {
			snprintf(path, sizeof(path), "/v1/containers/%s", names[i]);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", path, NULL, &r);
			cix_response_free(&r);
		}
	}
	for (i = 0; i < 100; i++) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", "/v1/networks/honet", NULL, &r) == 0 &&
		    r.status == 204) {
			cix_response_free(&r);
			break;
		}
		cix_response_free(&r);
		usleep(100000);
	}
	if (test_assert_no_drift(&client, NULL) != 0)
		ok = 0;
	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}
	if (!dead_pid_refused())
		ok = 0;
	test_data_dir_cleanup(g_data_dir);
	printf("HANDOVER RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
