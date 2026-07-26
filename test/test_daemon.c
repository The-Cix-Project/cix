/*
 * Phase 3 end-to-end test: proves the REST daemon actually implements
 * docs/api/openapi.yaml over real HTTP, not just that its internal
 * functions work. Stages one minimal test image, forks and execve's
 * the built daemon on a test port, then drives it purely as an HTTP
 * client (hand-written requests -- test-only code, same spirit as
 * test_overlay.c's host-side assertions) while using our own json.c
 * to parse responses rather than ad hoc string matching.
 */
#include "json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7621
#define IMAGE_ROOT "/var/lib/kanxeo/images/test/rootfs"

static int mkdir_p1(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

static int copy_file(const char *src_path, const char *dst_path)
{
	int src, dst;
	char buf[4096];
	ssize_t n;

	src = open(src_path, O_RDONLY);
	if (src < 0) {
		perror(src_path);
		return -1;
	}
	dst = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (dst < 0) {
		perror(dst_path);
		close(src);
		return -1;
	}
	while ((n = read(src, buf, sizeof(buf))) > 0) {
		if (write(dst, buf, (size_t)n) != n) {
			perror("write");
			close(src);
			close(dst);
			return -1;
		}
	}
	close(src);
	close(dst);
	return n < 0 ? -1 : 0;
}

static int build_test_image(void)
{
	char path[256];

	if (mkdir_p1("/var/lib/kanxeo") != 0 || mkdir_p1("/var/lib/kanxeo/images") != 0 ||
	    mkdir_p1("/var/lib/kanxeo/images/test") != 0 || mkdir_p1(IMAGE_ROOT) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/bin", IMAGE_ROOT);
	if (mkdir_p1(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/bin/daemon_child", IMAGE_ROOT);
	if (copy_file("build/daemon_child", path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib64", IMAGE_ROOT);
	if (mkdir_p1(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib64/ld-linux-x86-64.so.2", IMAGE_ROOT);
	if (copy_file("/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib", IMAGE_ROOT);
	if (mkdir_p1(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu", IMAGE_ROOT);
	if (mkdir_p1(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu/libc.so.6", IMAGE_ROOT);
	if (copy_file("/usr/lib/x86_64-linux-gnu/libc.so.6", path) != 0)
		return -1;

	return 0;
}

static int connect_to_daemon(void)
{
	int fd;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(TEST_PORT);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int wait_for_daemon(int max_attempts)
{
	int i, fd;

	for (i = 0; i < max_attempts; i++) {
		fd = connect_to_daemon();
		if (fd >= 0) {
			close(fd);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

static int write_all(int fd, const char *buf, size_t n)
{
	size_t written = 0;
	ssize_t w;

	while (written < n) {
		w = write(fd, buf + written, n - written);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		written += (size_t)w;
	}
	return 0;
}

struct http_result {
	int status;
	struct json_value *json; /* NULL if the body wasn't valid JSON (e.g. a 204) */
};

static void http_result_free(struct http_result *r)
{
	json_free(r->json);
	r->json = NULL;
}

static int do_request(const char *method, const char *path, const char *body,
                       struct http_result *out)
{
	int fd;
	char req[8192];
	int req_len;
	char resp[16384];
	size_t total = 0;
	ssize_t n;
	char *body_start;
	int status;

	fd = connect_to_daemon();
	if (fd < 0)
		return -1;

	if (body != NULL) {
		req_len = snprintf(req, sizeof(req),
		                    "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
		                    "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
		                    method, path, strlen(body), body);
	} else {
		req_len = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
		                    method, path);
	}
	if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
		close(fd);
		return -1;
	}
	if (write_all(fd, req, (size_t)req_len) != 0) {
		close(fd);
		return -1;
	}

	while (total + 1 < sizeof(resp)) {
		n = read(fd, resp + total, sizeof(resp) - total - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}
	resp[total] = '\0';
	close(fd);

	if (sscanf(resp, "HTTP/1.1 %d", &status) != 1)
		return -1;
	out->status = status;

	body_start = strstr(resp, "\r\n\r\n");
	out->json = NULL;
	if (body_start != NULL) {
		body_start += 4;
		size_t blen = total - (size_t)(body_start - resp);

		if (blen > 0)
			out->json = json_parse(body_start, blen);
	}
	return 0;
}

static long json_int_field(const struct json_value *obj, const char *key)
{
	return (long)json_as_number(json_object_get(obj, key));
}

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static int poll_until_exited(const char *name, int max_attempts, struct http_result *out)
{
	int i;
	char path[128];

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	for (i = 0; i < max_attempts; i++) {
		http_result_free(out);
		if (do_request("GET", path, NULL, out) != 0)
			return -1;
		if (out->status == 200 && str_eq(json_str_field(out->json, "status"), "exited"))
			return 0;
		usleep(100000);
	}
	return -1;
}

int main(void)
{
	pid_t daemon_pid;
	int ok = 1;
	struct http_result r;
	char *dargv[3];

	memset(&r, 0, sizeof(r));

	if (build_test_image() != 0)
		return 1;

	dargv[0] = "build/kanxeod";
	dargv[1] = "--port=7621";
	dargv[2] = NULL;

	daemon_pid = fork();
	if (daemon_pid < 0) {
		perror("fork");
		return 1;
	}
	if (daemon_pid == 0) {
		execve("build/kanxeod", dargv, environ);
		perror("execve build/kanxeod");
		_exit(127);
	}

	if (wait_for_daemon(50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	/* 1. health */
	if (do_request("GET", "/v1/health", NULL, &r) != 0 || r.status != 200 ||
	    !str_eq(json_str_field(r.json, "status"), "ok")) {
		fprintf(stderr, "FAIL: GET /v1/health\n");
		ok = 0;
	}
	http_result_free(&r);

	/* 2. create c1, exits quickly with code 5 */
	if (do_request("POST", "/v1/containers",
	               "{\"name\":\"c1\",\"image\":\"test\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"5\"]}",
	               &r) != 0 ||
	    r.status != 201 || !str_eq(json_str_field(r.json, "status"), "running")) {
		fprintf(stderr, "FAIL: POST /v1/containers (c1), status=%d\n", r.status);
		ok = 0;
	}
	http_result_free(&r);

	/* 3. list shows it */
	if (do_request("GET", "/v1/containers", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/containers\n");
		ok = 0;
	} else {
		const struct json_value *containers = json_object_get(r.json, "containers");
		int found = 0;
		size_t i;

		if (containers != NULL && containers->type == JSON_ARRAY) {
			for (i = 0; i < containers->u.array.count; i++) {
				if (str_eq(json_str_field(containers->u.array.items[i], "name"), "c1"))
					found = 1;
			}
		}
		if (!found) {
			fprintf(stderr, "FAIL: c1 missing from GET /v1/containers\n");
			ok = 0;
		}
	}
	http_result_free(&r);

	/* 4. poll until c1 exits, check exit_status */
	if (poll_until_exited("c1", 50, &r) != 0) {
		fprintf(stderr, "FAIL: c1 never reported exited\n");
		ok = 0;
	} else if (json_int_field(r.json, "exit_status") != 5) {
		fprintf(stderr, "FAIL: c1 exit_status expected 5, got %ld\n",
		        json_int_field(r.json, "exit_status"));
		ok = 0;
	}
	http_result_free(&r);

	/* 5. create c2, sleeps 30s -- then delete it while running */
	if (do_request("POST", "/v1/containers",
	               "{\"name\":\"c2\",\"image\":\"test\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}",
	               &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/containers (c2), status=%d\n", r.status);
		ok = 0;
	}
	{
		long c2_pid = json_int_field(r.json, "pid");
		char proc_path[64];
		struct stat st;

		http_result_free(&r);

		if (do_request("DELETE", "/v1/containers/c2", NULL, &r) != 0 || r.status != 204) {
			fprintf(stderr, "FAIL: DELETE /v1/containers/c2, status=%d\n", r.status);
			ok = 0;
		}
		http_result_free(&r);

		snprintf(proc_path, sizeof(proc_path), "/proc/%ld", c2_pid);
		if (stat(proc_path, &st) == 0) {
			fprintf(stderr, "FAIL: c2's process %ld still exists after DELETE\n", c2_pid);
			ok = 0;
		}
	}

	/* 6. deleted container is gone */
	if (do_request("GET", "/v1/containers/c2", NULL, &r) != 0 || r.status != 404) {
		fprintf(stderr, "FAIL: GET /v1/containers/c2 after delete, status=%d\n", r.status);
		ok = 0;
	}
	http_result_free(&r);

	/* 7. duplicate name -> 409 */
	if (do_request("POST", "/v1/containers",
	               "{\"name\":\"c1\",\"image\":\"test\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"1\"]}",
	               &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate name expected 409, got %d\n", r.status);
		ok = 0;
	}
	http_result_free(&r);

	/* 8. missing image -> 400 */
	if (do_request("POST", "/v1/containers", "{\"name\":\"c3\",\"cmd\":[\"/bin/daemon_child\"]}",
	               &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: missing image expected 400, got %d\n", r.status);
		ok = 0;
	}
	http_result_free(&r);

	/* 9. clean shutdown */
	kill(daemon_pid, SIGTERM);
	{
		int status;
		pid_t w = waitpid(daemon_pid, &status, 0);

		if (w != daemon_pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
			ok = 0;
		}
	}

	printf(ok ? "DAEMON RESULT: PASS\n" : "DAEMON RESULT: FAIL\n");
	return ok ? 0 : 1;
}
