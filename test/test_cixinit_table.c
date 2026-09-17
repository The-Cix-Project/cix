/*
 * test_cixinit_table -- the declaration-to-table translation (ADR-0260).
 *
 * Pure parsing and ordering: no container, no socket beyond a
 * socketpair to prove the send framing. What is pinned:
 *
 *   1. the worked example in cixinit_table.h parses, with defaults
 *   2. declaration order is kept where the graph allows, and a service
 *      declared before what it is after is moved behind it, with
 *      after_mask rewritten to TABLE indices
 *   3. a cycle, an unknown name, a self-reference and a duplicate are
 *      each refused with a message naming the service
 *   4. field validation: oneshot+restart, two ready kinds, a bad
 *      signal name, a cmd that is not strings, out-of-range numbers
 *   5. a single-service table (the build container's shape)
 *   6. send() frames the hello and each record as its own message
 */

#include "cixinit_table.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fails;

#define CHECK(cond, ...)                                                   \
	do {                                                               \
		if (!(cond)) {                                             \
			printf("  FAIL: ");                                \
			printf(__VA_ARGS__);                               \
			printf("\n");                                      \
			g_fails++;                                         \
		}                                                          \
	} while (0)

/*
 * Two addresses, so the hello's own list is exercised rather than the
 * degenerate one-address case (#477): 10.0.0.5 and 10.0.1.5, network
 * order.
 */
static const unsigned int g_test_addrs[2] = { 0x0500000au, 0x0500010au };

static int build(const char *json, struct cixinit_table *t, char *err, size_t err_size)
{
	struct json_value *root = json_parse(json, strlen(json));
	int rc;

	err[0] = '\0';
	if (root == NULL) {
		snprintf(err, err_size, "test json did not parse");
		return -1;
	}
	rc = cixinit_table_from_json(json_object_get(root, "services"), g_test_addrs, 2, t, err,
	                             err_size);
	json_free(root);
	return rc;
}

static const char *argv0(const struct cixinit_service *s)
{
	return s->argv;
}

static void test_example(void)
{
	struct cixinit_table t;
	char err[256];
	const char *json =
	    "{\"services\":["
	    "{\"name\":\"hostkeys\",\"type\":\"oneshot\",\"cmd\":[\"/usr/bin/ssh-keygen\",\"-A\"]},"
	    "{\"name\":\"nslcd\",\"cmd\":[\"/usr/sbin/nslcd\",\"-d\"],\"ready\":{\"socket\":\"/run/nslcd/socket\"}},"
	    "{\"name\":\"sshd\",\"cmd\":[\"/usr/sbin/sshd\",\"-D\",\"-e\"],\"after\":[\"hostkeys\",\"nslcd\"],"
	    " \"ready\":{\"tcp_port\":22,\"timeout_seconds\":45},\"stop_signal\":\"SIGINT\","
	    " \"restart_delay_seconds\":5,\"uid\":0}"
	    "]}";

	printf("1. the header's worked example\n");
	CHECK(build(json, &t, err, sizeof(err)) == 0, "%s", err);
	CHECK(t.count == 3 && t.hello.service_count == 3 && t.hello.magic == CIXINIT_MAGIC, "count");
	CHECK(strcmp(t.svc[0].name, "hostkeys") == 0 && t.svc[0].type == CIXINIT_TYPE_ONESHOT &&
	          t.svc[0].on_exit == CIXINIT_ON_EXIT_FAIL_CONTAINER,
	      "oneshot defaults to fail-container");
	CHECK(strcmp(argv0(&t.svc[0]), "/usr/bin/ssh-keygen") == 0 &&
	          strcmp(t.svc[0].argv + strlen("/usr/bin/ssh-keygen") + 1, "-A") == 0 &&
	          t.svc[0].argv[strlen("/usr/bin/ssh-keygen") + 1 + 3] == '\0',
	      "argv packed NUL-separated, double-NUL terminated");
	CHECK(t.svc[1].type == CIXINIT_TYPE_DAEMON && t.svc[1].on_exit == CIXINIT_ON_EXIT_RESTART &&
	          t.svc[1].ready_kind == CIXINIT_READY_SOCKET &&
	          strcmp(t.svc[1].ready_path, "/run/nslcd/socket") == 0 && t.svc[1].ready_timeout_seconds == 30 &&
	          t.svc[1].restart_delay_seconds == 2 && t.svc[1].stop_signal == SIGTERM &&
	          t.svc[1].stop_timeout_seconds == 10 && t.svc[1].uid == -1 && t.svc[1].gid == -1,
	      "daemon defaults");
	CHECK(t.svc[2].after_mask == 3u && t.svc[2].ready_kind == CIXINIT_READY_TCP && t.svc[2].ready_port == 22 &&
	          t.svc[2].ready_timeout_seconds == 45 &&
	          t.svc[2].stop_signal == SIGINT && t.svc[2].restart_delay_seconds == 5 && t.svc[2].uid == 0,
	      "sshd's explicit fields (after_mask=%u)", t.svc[2].after_mask);
	/*
	 * #477: the addresses a TCP probe tries live on the hello, once
	 * per container, not on each service. A per-service field is what
	 * let the daemon compute one address before it had parsed the
	 * networks -- always 0, so every probe went to loopback and only
	 * to loopback.
	 */
	CHECK(t.hello.addr_count == 2 && t.hello.addr_be[0] == g_test_addrs[0] &&
	          t.hello.addr_be[1] == g_test_addrs[1] && t.hello.addr_be[2] == 0,
	      "the container's addresses are on the hello, in order (addr_count=%d)", t.hello.addr_count);
	CHECK(cixinit_table_index(&t, "sshd") == 2 && cixinit_table_index(&t, "nope") == -1, "index");
	CHECK(t.order[0] == 0 && t.order[1] == 1 && t.order[2] == 2, "order kept");
}

static void test_reorder(void)
{
	struct cixinit_table t;
	char err[256];
	const char *json =
	    "{\"services\":["
	    "{\"name\":\"web\",\"cmd\":[\"/w\"],\"after\":[\"db\",\"cache\"]},"
	    "{\"name\":\"cache\",\"cmd\":[\"/c\"]},"
	    "{\"name\":\"db\",\"cmd\":[\"/d\"],\"after\":[\"cache\"]}"
	    "]}";

	printf("2. a service declared before what it is after moves behind it\n");
	CHECK(build(json, &t, err, sizeof(err)) == 0, "%s", err);
	CHECK(strcmp(t.svc[0].name, "cache") == 0 && strcmp(t.svc[1].name, "db") == 0 &&
	          strcmp(t.svc[2].name, "web") == 0,
	      "order: %s %s %s", t.svc[0].name, t.svc[1].name, t.svc[2].name);
	CHECK(t.svc[0].after_mask == 0 && t.svc[1].after_mask == 1u && t.svc[2].after_mask == 3u,
	      "after_mask in table indices: %u %u %u", t.svc[0].after_mask, t.svc[1].after_mask,
	      t.svc[2].after_mask);
	CHECK(t.order[0] == 1 && t.order[1] == 2 && t.order[2] == 0, "body order remembered");
}

static void expect_refused(const char *what, const char *json, const char *needle)
{
	struct cixinit_table t;
	char err[256];

	CHECK(build(json, &t, err, sizeof(err)) != 0, "%s was accepted", what);
	CHECK(strstr(err, needle) != NULL, "%s: message '%s' does not mention '%s'", what, err, needle);
	printf("  %s -> %s\n", what, err);
}

static void test_graph_errors(void)
{
	printf("3. graph errors name the service\n");
	expect_refused("cycle",
	               "{\"services\":[{\"name\":\"a\",\"cmd\":[\"/a\"],\"after\":[\"b\"]},"
	               "{\"name\":\"b\",\"cmd\":[\"/b\"],\"after\":[\"a\"]}]}",
	               "cycle");
	expect_refused("unknown name",
	               "{\"services\":[{\"name\":\"a\",\"cmd\":[\"/a\"],\"after\":[\"ghost\"]}]}", "ghost");
	expect_refused("self", "{\"services\":[{\"name\":\"a\",\"cmd\":[\"/a\"],\"after\":[\"a\"]}]}",
	               "after itself");
	expect_refused("duplicate",
	               "{\"services\":[{\"name\":\"a\",\"cmd\":[\"/a\"]},{\"name\":\"a\",\"cmd\":[\"/b\"]}]}",
	               "more than once");
	expect_refused("empty list", "{\"services\":[]}", "1 to");
	expect_refused("bad name", "{\"services\":[{\"name\":\"a b\",\"cmd\":[\"/a\"]}]}", "letters");
}

static void test_field_errors(void)
{
	printf("4. field validation\n");
	expect_refused("oneshot+restart",
	               "{\"services\":[{\"name\":\"a\",\"type\":\"oneshot\",\"cmd\":[\"/a\"],\"on_exit\":\"restart\"}]}",
	               "cannot restart");
	expect_refused("two ready kinds",
	               "{\"services\":[{\"name\":\"a\",\"cmd\":[\"/a\"],\"ready\":{\"tcp_port\":1,\"socket\":\"/s\"}}]}",
	               "exactly one");
	expect_refused("bad signal",
	               "{\"services\":[{\"name\":\"a\",\"cmd\":[\"/a\"],\"stop_signal\":\"SIGFOO\"}]}", "stop_signal");
	expect_refused("cmd not strings", "{\"services\":[{\"name\":\"a\",\"cmd\":[1,2]}]}", "strings");
	expect_refused("no cmd", "{\"services\":[{\"name\":\"a\"}]}", "cmd");
	expect_refused("delay out of range",
	               "{\"services\":[{\"name\":\"a\",\"cmd\":[\"/a\"],\"restart_delay_seconds\":0}]}", "1-300");
	expect_refused("bad type", "{\"services\":[{\"name\":\"a\",\"cmd\":[\"/a\"],\"type\":\"forever\"}]}",
	               "type must be");
	expect_refused("relative socket",
	               "{\"services\":[{\"name\":\"a\",\"cmd\":[\"/a\"],\"ready\":{\"socket\":\"run/x\"}}]}",
	               "absolute");
	CHECK(cixinit_signal_from_name("TERM") == SIGTERM && cixinit_signal_from_name("SIGKILL") == SIGKILL &&
	          cixinit_signal_from_name("KILLALL") == -1,
	      "signal names");
	CHECK(strcmp(cixinit_signal_name(SIGHUP), "SIGHUP") == 0, "signal name back");
}

static void test_single(void)
{
	struct cixinit_table t;
	char *const argv[] = { (char *)"/usr/bin/bash", (char *)"-c", (char *)"make all", NULL };

	printf("5. a single-service table\n");
	CHECK(cixinit_table_single(&t, "build", argv, CIXINIT_TYPE_ONESHOT, CIXINIT_ON_EXIT_FAIL_CONTAINER) == 0,
	      "build");
	CHECK(t.count == 1 && strcmp(t.svc[0].name, "build") == 0 && t.svc[0].type == CIXINIT_TYPE_ONESHOT &&
	          strcmp(t.svc[0].argv, "/usr/bin/bash") == 0 &&
	          strcmp(t.svc[0].argv + 14, "-c") == 0 && strcmp(t.svc[0].argv + 17, "make all") == 0,
	      "single record");
	CHECK(cixinit_table_socket_bytes(&t) == sizeof(struct cixinit_hello) + sizeof(struct cixinit_service),
	      "socket bytes");
	/* A build container has no networks, so its probe candidates are
	 * loopback and nothing else -- which is what they were before
	 * #477, for every container. */
	CHECK(t.hello.addr_count == 0, "a single-service table announces no addresses");
}

static void test_send_framing(void)
{
	struct cixinit_table t;
	int sv[2];
	char err[256];
	struct cixinit_hello hello;
	struct cixinit_service svc;
	char big[2048];
	const char *json = "{\"services\":[{\"name\":\"a\",\"cmd\":[\"/a\"]},{\"name\":\"b\",\"cmd\":[\"/b\"]}]}";

	printf("6. send() frames one message per record\n");
	CHECK(build(json, &t, err, sizeof(err)) == 0, "%s", err);
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair");
	CHECK(cixinit_table_send(&t, sv[0]) == 0, "send");
	CHECK(read(sv[1], big, sizeof(big)) == (ssize_t)sizeof(hello), "first message is the hello, alone");
	memcpy(&hello, big, sizeof(hello));
	CHECK(hello.magic == CIXINIT_MAGIC && hello.service_count == 2 && hello.addr_count == 2 &&
	          hello.addr_be[0] == g_test_addrs[0] && hello.addr_be[1] == g_test_addrs[1],
	      "hello content, addresses included (#477 -- they cross the wire in the hello)");
	CHECK(read(sv[1], big, sizeof(big)) == (ssize_t)sizeof(svc), "second message is one record");
	memcpy(&svc, big, sizeof(svc));
	CHECK(strcmp(svc.name, "a") == 0, "record a");
	CHECK(read(sv[1], big, sizeof(big)) == (ssize_t)sizeof(svc), "third message is one record");
	memcpy(&svc, big, sizeof(svc));
	CHECK(strcmp(svc.name, "b") == 0, "record b");
	close(sv[0]);
	close(sv[1]);
}

int main(void)
{
	test_example();
	test_reorder();
	test_graph_errors();
	test_field_errors();
	test_single();
	test_send_framing();
	printf("CIXINIT-TABLE RESULT: %s (%d failure(s))\n", g_fails == 0 ? "PASS" : "FAIL", g_fails);
	return g_fails == 0 ? 0 : 1;
}
