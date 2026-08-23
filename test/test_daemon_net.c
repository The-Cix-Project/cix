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
#include <limits.h>
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
#define NET_CHILD_PORT 17700

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];
#define TEST_NETWORK_NAME "dnettest"
#define TEST_NETWORK_SUBNET "172.33.0.0"
#define TEST_NETWORK_NAME2 "dnettest2"
#define TEST_NETWORK_SUBNET2 "172.34.0.0"

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
	char *dargv[4];
	char data_dir_arg[PATH_MAX + 11];
	struct thinc_client client;
	int ok = 1;
	struct thinc_response r;
	char ip1[64] = { 0 };
	char ip2[64] = { 0 };

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/nettest/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/nettest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}

	if (test_image_fixture_build(g_image_root, "build/net_child", "net_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (test_image_fixture_build(g_image_root, "build/net_connect", "net_connect") != 0) {
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

	thinc_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. create the test networks. Explicit --address= (ADR-0037,
	 * renamed by ADR-0067): this whole file's own verification
	 * methodology connect()s from the HOST straight at a container's
	 * IP (see connect_and_echo() above) -- on the new address-less
	 * default the host has no route into either subnet at all
	 * (correct, intended behavior, not a bug). */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"" TEST_NETWORK_NAME "\",\"subnet\":\"" TEST_NETWORK_SUBNET
	                       "\",\"prefix_len\":24,\"address\":\"172.33.0.1\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/networks, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"" TEST_NETWORK_NAME2 "\",\"subnet\":\"" TEST_NETWORK_SUBNET2
	                       "\",\"prefix_len\":24,\"address\":\"172.34.0.1\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/networks (2nd), status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 2-3. create n1 with networking, confirm a real assigned ip and
	 * real connectivity through it */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
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
	thinc_response_free(&r);

	/* 4. second networked container gets a DIFFERENT ip */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
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
	thinc_response_free(&r);

	/* 5. GET /v1/containers shows both with their correct, distinct ips */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/containers", NULL, &r) != 0 || r.status != 200) {
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
	thinc_response_free(&r);

	/* 6. no "networks" field -> empty networks array, regression check
	 * on unchanged default behavior */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
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
	thinc_response_free(&r);

	/* 7. unsupported network name -> 400 */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"n4\",\"image\":\"nettest\",\"cmd\":[\"/bin/net_child\"],"
	                       "\"networks\":[\"bogus\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: unsupported network expected 400, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 8. multi-homing: one container, two networks -- Phase 7 part 2 */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
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
	thinc_response_free(&r);

	/* 9. real router topology (Phase 7 part 3): router sits on both
	 * networks with ip_forward on; h and t each get a static route via
	 * router for the other's subnet. h connects to t from inside its
	 * own netns (net_connect, not this test's own host-side
	 * connect_and_echo()) -- proving the round trip actually crosses
	 * through router's kernel routing table, driven entirely over the
	 * daemon's real HTTP API. */
	{
		char body[512];
		char r_ip_a[64] = { 0 }, r_ip_b[64] = { 0 };
		char t_ip[64] = { 0 };

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"router\",\"image\":\"nettest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"6\",\"0\"],"
		                       "\"networks\":[\"" TEST_NETWORK_NAME "\",\"" TEST_NETWORK_NAME2
		                       "\"],\"ip_forward\":true}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST router, status=%d\n", r.status);
			ok = 0;
		} else {
			const char *ip_a = network_ip_in_response(r.json, TEST_NETWORK_NAME);
			const char *ip_b = network_ip_in_response(r.json, TEST_NETWORK_NAME2);

			if (ip_a == NULL || ip_b == NULL) {
				fprintf(stderr, "FAIL: router missing an ip on one of its two networks\n");
				ok = 0;
			} else {
				snprintf(r_ip_a, sizeof(r_ip_a), "%s", ip_a);
				snprintf(r_ip_b, sizeof(r_ip_b), "%s", ip_b);
			}
		}
		thinc_response_free(&r);

		if (r_ip_b[0] != '\0') {
			/* t lives on TEST_NETWORK_NAME2, so its route to
			 * TEST_NETWORK_NAME's subnet must go via router's IP on
			 * that SAME network (r_ip_b) -- a gateway has to be
			 * directly reachable on one of the container's own
			 * connected subnets, not the far one. */
			snprintf(body, sizeof(body),
			         "{\"name\":\"t\",\"image\":\"nettest\",\"cmd\":[\"/bin/net_child\"],"
			         "\"networks\":[\"%s\"],"
			         "\"routes\":[{\"dest\":\"%s\",\"prefix_len\":24,\"via\":\"%s\"}]}",
			         TEST_NETWORK_NAME2, TEST_NETWORK_SUBNET, r_ip_b);

			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "POST", "/v1/containers", body, &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: POST t (router scenario), status=%d\n", r.status);
				ok = 0;
			} else {
				const char *ip = network_ip_in_response(r.json, TEST_NETWORK_NAME2);

				if (ip == NULL) {
					fprintf(stderr, "FAIL: t (router scenario) has no ip\n");
					ok = 0;
				} else {
					snprintf(t_ip, sizeof(t_ip), "%s", ip);
				}
			}
			thinc_response_free(&r);
		}

		if (r_ip_a[0] != '\0' && t_ip[0] != '\0') {
			int seen = 0;
			int attempt;
			long exit_status = -1;

			/* h lives on TEST_NETWORK_NAME, so its route to
			 * TEST_NETWORK_NAME2's subnet must go via router's IP on
			 * that SAME network (r_ip_a), for the same reason as t's
			 * route above. */
			snprintf(body, sizeof(body),
			         "{\"name\":\"h\",\"image\":\"nettest\",\"cmd\":[\"/bin/net_connect\",\"%s\"],"
			         "\"networks\":[\"%s\"],"
			         "\"routes\":[{\"dest\":\"%s\",\"prefix_len\":24,\"via\":\"%s\"}]}",
			         t_ip, TEST_NETWORK_NAME, TEST_NETWORK_SUBNET2, r_ip_a);

			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "POST", "/v1/containers", body, &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: POST h (router scenario), status=%d\n", r.status);
				ok = 0;
			}
			thinc_response_free(&r);

			/* h's own exit status is the real proof: net_connect only
			 * exits 0 if its round trip through router's kernel
			 * routing table actually succeeded. */
			for (attempt = 0; attempt < 100; attempt++) {
				memset(&r, 0, sizeof(r));
				if (thinc_client_request(&client, "GET", "/v1/containers/h", NULL, &r) == 0 &&
				    r.status == 200 && str_eq(json_str_field(r.json, "status"), "exited")) {
					seen = 1;
					exit_status = (long)json_as_number(json_object_get(r.json, "exit_status"));
					thinc_response_free(&r);
					break;
				}
				thinc_response_free(&r);
				usleep(100000);
			}
			if (!seen) {
				fprintf(stderr, "FAIL: h (router scenario) never reported exited\n");
				ok = 0;
			} else if (exit_status != 0) {
				fprintf(stderr,
				        "FAIL: h (router scenario) exit_status=%ld, expected 0 -- packet "
				        "forwarding through the router container did not work\n",
				        exit_status);
				ok = 0;
			}
		}

		/* cleanup: h/t/router removed before their networks -- same
		 * ordering rule the rest of this file already follows */
		thinc_client_request(&client, "DELETE", "/v1/containers/h", NULL, &r);
		thinc_response_free(&r);
		thinc_client_request(&client, "DELETE", "/v1/containers/t", NULL, &r);
		thinc_response_free(&r);
		thinc_client_request(&client, "DELETE", "/v1/containers/router", NULL, &r);
		thinc_response_free(&r);
	}

	/* 9.5 (ADR-0156/task #861): live network attach/detach on an
	 * already-running container, no recreate. n6 starts with ZERO
	 * networks -- net_child (0.0.0.0 wildcard bind, before any
	 * interface exists) is already listening by the time the live
	 * attach happens, proving the attach itself is what makes the new
	 * interface reachable, not something baked in at creation. */
	{
		char n6_ip[64] = { 0 };
		char n6_ip2[64] = { 0 };

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"n6\",\"image\":\"nettest\","
		                       "\"cmd\":[\"/bin/net_child\",\"2\"]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST n6 (no networks), status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* attach to a nonexistent network -> 404 */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers/n6/networks",
		                       "{\"name\":\"bogus\"}", &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: live attach to bogus network expected 404, got %d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* attach to a nonexistent/not-running container -> 404 */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers/nosuchcontainer/networks",
		                       "{\"name\":\"" TEST_NETWORK_NAME "\"}", &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: live attach to unknown container expected 404, got %d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* the real attach */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers/n6/networks",
		                       "{\"name\":\"" TEST_NETWORK_NAME "\"}", &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: live attach n6 to %s, status=%d\n", TEST_NETWORK_NAME, r.status);
			ok = 0;
		} else {
			const char *ip = network_ip_in_response(r.json, TEST_NETWORK_NAME);

			if (ip == NULL) {
				fprintf(stderr, "FAIL: live-attached n6 has no ip on %s\n", TEST_NETWORK_NAME);
				ok = 0;
			} else {
				snprintf(n6_ip, sizeof(n6_ip), "%s", ip);
				if (!connect_and_echo(n6_ip)) {
					fprintf(stderr,
					        "FAIL: could not connect to live-attached n6 at %s -- the whole "
					        "point of this feature\n",
					        n6_ip);
					ok = 0;
				}
			}
		}
		thinc_response_free(&r);

		/* re-attaching the same network -> 409 */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers/n6/networks",
		                       "{\"name\":\"" TEST_NETWORK_NAME "\"}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: re-attaching already-attached network expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* GET reflects it, marked live */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/containers/n6", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET n6 after live attach\n");
			ok = 0;
		} else {
			const struct json_value *networks = json_object_get(r.json, "networks");
			int found_live = 0;
			size_t i;

			if (networks != NULL && networks->type == JSON_ARRAY) {
				for (i = 0; i < networks->u.array.count; i++) {
					const struct json_value *item = networks->u.array.items[i];
					const struct json_value *live = json_object_get(item, "live");

					if (str_eq(json_str_field(item, "name"), TEST_NETWORK_NAME) && live != NULL &&
					    live->type == JSON_BOOL && live->u.boolean)
						found_live = 1;
				}
			}
			if (!found_live) {
				fprintf(stderr, "FAIL: GET n6 does not show %s as a live attachment\n",
				        TEST_NETWORK_NAME);
				ok = 0;
			}
		}
		thinc_response_free(&r);

		/* detaching a network never attached -> 404 */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "DELETE", "/v1/containers/n6/networks/" TEST_NETWORK_NAME2,
		                       NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: detach never-attached network expected 404, got %d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* the real detach */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "DELETE", "/v1/containers/n6/networks/" TEST_NETWORK_NAME,
		                       NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: live detach n6 from %s, status=%d\n", TEST_NETWORK_NAME,
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		/* GET no longer shows it at all */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "GET", "/v1/containers/n6", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET n6 after live detach\n");
			ok = 0;
		} else {
			const struct json_value *networks = json_object_get(r.json, "networks");

			if (networks == NULL || networks->type != JSON_ARRAY || networks->u.array.count != 0) {
				fprintf(stderr, "FAIL: GET n6 after detach should have an empty networks array\n");
				ok = 0;
			}
		}
		thinc_response_free(&r);

		/* re-attach after detach -- proves veth-naming collision-free
		 * across a full attach/detach/attach cycle, and that the same
		 * container can still reach the network again through a fresh
		 * pair. n6 still has one more accept() left (want_connections=2). */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers/n6/networks",
		                       "{\"name\":\"" TEST_NETWORK_NAME "\"}", &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: re-attach n6 to %s after detach, status=%d\n", TEST_NETWORK_NAME,
			        r.status);
			ok = 0;
		} else {
			const char *ip = network_ip_in_response(r.json, TEST_NETWORK_NAME);

			if (ip == NULL) {
				fprintf(stderr, "FAIL: re-attached n6 has no ip on %s\n", TEST_NETWORK_NAME);
				ok = 0;
			} else {
				snprintf(n6_ip2, sizeof(n6_ip2), "%s", ip);
				if (!connect_and_echo(n6_ip2)) {
					fprintf(stderr, "FAIL: could not connect to re-attached n6 at %s\n", n6_ip2);
					ok = 0;
				}
			}
		}
		thinc_response_free(&r);

		/* detaching a create-time (non-live) attachment -> 409, not
		 * silently torn down. A fresh, dedicated container for this --
		 * n5's own net_child already exited after its 2 real
		 * connections in scenario 8, so it's no longer `running` and
		 * would wrongly test the "not running" 404 path instead of
		 * this one. */
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"n7\",\"image\":\"nettest\",\"cmd\":[\"/bin/net_child\"],"
		                       "\"networks\":[\"" TEST_NETWORK_NAME "\"]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST n7, status=%d\n", r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "DELETE", "/v1/containers/n7/networks/" TEST_NETWORK_NAME,
		                       NULL, &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr,
			        "FAIL: detaching a create-time network attachment expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		thinc_client_request(&client, "DELETE", "/v1/containers/n7", NULL, &r);
		thinc_response_free(&r);

		thinc_client_request(&client, "DELETE", "/v1/containers/n6", NULL, &r);
		thinc_response_free(&r);
	}

	/* 10. deleting a network still in use by a container -> 409 */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "DELETE", "/v1/networks/" TEST_NETWORK_NAME, NULL, &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: DELETE in-use network expected 409, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 11. cleanup: remove containers (kills n3, still blocked in
	 * accept() since nothing can reach it -- no networking, isolated
	 * netns), then both networks via the real API */
	thinc_client_request(&client, "DELETE", "/v1/containers/n1", NULL, &r);
	thinc_response_free(&r);
	thinc_client_request(&client, "DELETE", "/v1/containers/n2", NULL, &r);
	thinc_response_free(&r);
	thinc_client_request(&client, "DELETE", "/v1/containers/n3", NULL, &r);
	thinc_response_free(&r);
	thinc_client_request(&client, "DELETE", "/v1/containers/n5", NULL, &r);
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "DELETE", "/v1/networks/" TEST_NETWORK_NAME, NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE " TEST_NETWORK_NAME " (unused) expected 204, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "DELETE", "/v1/networks/" TEST_NETWORK_NAME2, NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE " TEST_NETWORK_NAME2 " (unused) expected 204, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	kill(daemon_pid, SIGTERM);
	{
		int status;
		pid_t w = waitpid(daemon_pid, &status, 0);

		if (w != daemon_pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
			ok = 0;
		}
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "DAEMON NET RESULT: PASS\n" : "DAEMON NET RESULT: FAIL\n");
	return ok ? 0 : 1;
}
