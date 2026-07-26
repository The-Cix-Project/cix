/*
 * Phase 7 parts 1+2 end-to-end test: proves the dynamic network
 * resource (POST/GET/DELETE /v1/networks -- daemon/src/network.c) and
 * container IP allocation against it work over real HTTP -- containers
 * created via POST /v1/containers with a "networks" array naming
 * networks created through this same API get real, distinct,
 * connectable IPs (one interface per entry, part 2's multi-homing),
 * and containers created without it are completely unaffected (the
 * explicit regression check for this endpoint's unchanged default
 * behavior).
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7624
#define PORT_ARG "--port=7624"
#define IMAGE_ROOT "/var/lib/kanxeo/images/nettest/rootfs"
#define NET_CHILD_PORT 17700
#define TEST_NETWORK_NAME "dnettest"
#define TEST_NETWORK_SUBNET "172.33.0.0"
#define TEST_NETWORK_NAME2 "dnettest2"
#define TEST_NETWORK_SUBNET2 "172.34.0.0"

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

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/* Finds {"name": network_name, "ip": ...} inside a Container response's
 * "networks" array and returns the ip, or NULL if not attached to it. */
static const char *network_ip_in_response(const struct json_value *container,
                                           const char *network_name)
{
	const struct json_value *networks = json_object_get(container, "networks");
	size_t i;

	if (networks == NULL || networks->type != JSON_ARRAY)
		return NULL;
	for (i = 0; i < networks->u.array.count; i++) {
		const struct json_value *item = networks->u.array.items[i];

		if (str_eq(json_str_field(item, "name"), network_name))
			return json_str_field(item, "ip");
	}
	return NULL;
}

/*
 * The container's network interface is configured before execve()
 * (container_net_child_configure() runs first), but net_child itself
 * still needs a brief, variable amount of wall-clock time after
 * execve() to reach its own bind()/listen() call -- so the very first
 * connect() attempt can legitimately see ECONNREFUSED. Retry, same
 * pattern as every other "wait for something async" check in this
 * project, rather than a single blind sleep before connecting once.
 */
static int connect_and_echo(const char *ip)
{
	int attempt;
	struct sockaddr_in addr;
	char sbyte = 9, rbyte = 0;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(NET_CHILD_PORT);
	inet_pton(AF_INET, ip, &addr.sin_addr);

	for (attempt = 0; attempt < 30; attempt++) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);

		if (fd < 0) {
			perror("socket");
			return 0;
		}
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			int ok = write(fd, &sbyte, 1) == 1 && read(fd, &rbyte, 1) == 1 && rbyte == sbyte;

			close(fd);
			return ok;
		}
		close(fd);
		usleep(100000);
	}
	fprintf(stderr, "connect to %s: timed out retrying\n", ip);
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	char *dargv[3];
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	char ip1[64] = { 0 };
	char ip2[64] = { 0 };

	if (test_image_fixture_build(IMAGE_ROOT, "build/net_child", "net_child") != 0)
		return 1;

	dargv[0] = "build/kanxeod";
	dargv[1] = PORT_ARG;
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

	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	/* 1. create the test networks */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"" TEST_NETWORK_NAME "\",\"subnet\":\"" TEST_NETWORK_SUBNET
	                       "\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/networks, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"" TEST_NETWORK_NAME2 "\",\"subnet\":\"" TEST_NETWORK_SUBNET2
	                       "\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/networks (2nd), status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2-3. create n1 with networking, confirm a real assigned ip and
	 * real connectivity through it */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"n1\",\"image\":\"nettest\",\"cmd\":[\"/bin/net_child\"],"
	                       "\"networks\":[\"" TEST_NETWORK_NAME "\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST n1, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *ip = network_ip_in_response(r.json, TEST_NETWORK_NAME);

		if (ip == NULL) {
			fprintf(stderr, "FAIL: n1 has no ip on %s\n", TEST_NETWORK_NAME);
			ok = 0;
		} else {
			snprintf(ip1, sizeof(ip1), "%s", ip);
			if (!connect_and_echo(ip1)) {
				fprintf(stderr, "FAIL: could not connect to n1 at %s\n", ip1);
				ok = 0;
			}
		}
	}
	kx_response_free(&r);

	/* 4. second networked container gets a DIFFERENT ip */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"n2\",\"image\":\"nettest\",\"cmd\":[\"/bin/net_child\"],"
	                       "\"networks\":[\"" TEST_NETWORK_NAME "\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST n2, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *ip = network_ip_in_response(r.json, TEST_NETWORK_NAME);

		if (ip == NULL) {
			fprintf(stderr, "FAIL: n2 has no ip on %s\n", TEST_NETWORK_NAME);
			ok = 0;
		} else {
			snprintf(ip2, sizeof(ip2), "%s", ip);
			if (str_eq(ip1, ip2)) {
				fprintf(stderr, "FAIL: n2 got the same ip as n1 (%s)\n", ip2);
				ok = 0;
			}
			if (!connect_and_echo(ip2)) {
				fprintf(stderr, "FAIL: could not connect to n2 at %s\n", ip2);
				ok = 0;
			}
		}
	}
	kx_response_free(&r);

	/* 5. GET /v1/containers shows both with their correct, distinct ips */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/containers", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/containers\n");
		ok = 0;
	} else {
		const struct json_value *containers = json_object_get(r.json, "containers");
		int found1 = 0, found2 = 0;
		size_t i;

		if (containers != NULL && containers->type == JSON_ARRAY) {
			for (i = 0; i < containers->u.array.count; i++) {
				const struct json_value *item = containers->u.array.items[i];
				const char *n = json_str_field(item, "name");

				if (str_eq(n, "n1") && str_eq(network_ip_in_response(item, TEST_NETWORK_NAME), ip1))
					found1 = 1;
				if (str_eq(n, "n2") && str_eq(network_ip_in_response(item, TEST_NETWORK_NAME), ip2))
					found2 = 1;
			}
		}
		if (!found1 || !found2) {
			fprintf(stderr,
			        "FAIL: GET /v1/containers missing correct ips (found1=%d found2=%d)\n",
			        found1, found2);
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 6. no "networks" field -> empty networks array, regression check
	 * on unchanged default behavior */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"n3\",\"image\":\"nettest\",\"cmd\":[\"/bin/net_child\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST n3 (no networks), status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *networks = json_object_get(r.json, "networks");

		if (networks == NULL || networks->type != JSON_ARRAY || networks->u.array.count != 0) {
			fprintf(stderr, "FAIL: n3 (no networks) should have an empty networks array\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 7. unsupported network name -> 400 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"n4\",\"image\":\"nettest\",\"cmd\":[\"/bin/net_child\"],"
	                       "\"networks\":[\"bogus\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: unsupported network expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 8. multi-homing: one container, two networks -- Phase 7 part 2 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"n5\",\"image\":\"nettest\",\"cmd\":[\"/bin/net_child\",\"2\"],"
	                       "\"networks\":[\"" TEST_NETWORK_NAME "\",\"" TEST_NETWORK_NAME2 "\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST n5 (multi-homed), status=%d\n", r.status);
		ok = 0;
	} else {
		const char *ip_a = network_ip_in_response(r.json, TEST_NETWORK_NAME);
		const char *ip_b = network_ip_in_response(r.json, TEST_NETWORK_NAME2);
		char ip_a_buf[64] = { 0 };
		char ip_b_buf[64] = { 0 };

		if (ip_a == NULL || ip_b == NULL) {
			fprintf(stderr, "FAIL: n5 missing an ip on one of its two networks\n");
			ok = 0;
		} else {
			snprintf(ip_a_buf, sizeof(ip_a_buf), "%s", ip_a);
			snprintf(ip_b_buf, sizeof(ip_b_buf), "%s", ip_b);
			if (!connect_and_echo(ip_a_buf)) {
				fprintf(stderr, "FAIL: could not connect to n5 at %s (%s)\n", ip_a_buf,
				        TEST_NETWORK_NAME);
				ok = 0;
			}
			if (!connect_and_echo(ip_b_buf)) {
				fprintf(stderr, "FAIL: could not connect to n5 at %s (%s)\n", ip_b_buf,
				        TEST_NETWORK_NAME2);
				ok = 0;
			}
		}
	}
	kx_response_free(&r);

	/* 9. deleting a network still in use by a container -> 409 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/" TEST_NETWORK_NAME, NULL, &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: DELETE in-use network expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 10. cleanup: remove containers (kills n3, still blocked in
	 * accept() since nothing can reach it -- no networking, isolated
	 * netns), then both networks via the real API */
	kx_client_request(&client, "DELETE", "/v1/containers/n1", NULL, &r);
	kx_response_free(&r);
	kx_client_request(&client, "DELETE", "/v1/containers/n2", NULL, &r);
	kx_response_free(&r);
	kx_client_request(&client, "DELETE", "/v1/containers/n3", NULL, &r);
	kx_response_free(&r);
	kx_client_request(&client, "DELETE", "/v1/containers/n5", NULL, &r);
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/" TEST_NETWORK_NAME, NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE " TEST_NETWORK_NAME " (unused) expected 204, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/" TEST_NETWORK_NAME2, NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE " TEST_NETWORK_NAME2 " (unused) expected 204, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	kill(daemon_pid, SIGTERM);
	{
		int status;
		pid_t w = waitpid(daemon_pid, &status, 0);

		if (w != daemon_pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
			ok = 0;
		}
	}

	printf(ok ? "DAEMON NET RESULT: PASS\n" : "DAEMON NET RESULT: FAIL\n");
	return ok ? 0 : 1;
}
