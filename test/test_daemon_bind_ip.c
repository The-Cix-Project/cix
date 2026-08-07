/*
 * ADR-0068 end-to-end test: proves bind_ip is a real, dedicated second
 * address on the management network's own bridge, not just a JSON
 * field -- verified via a full live round-trip against a real kanxeod:
 * repoint management to a fresh network (a real listen-socket rebind
 * to that network's own address), then set bind_ip (another real
 * rebind, this time to a second, distinct address added to the same
 * bridge), confirmed both via reconnecting kx_client instances at each
 * new address AND via getifaddrs() reading the bridge's real kernel
 * address list directly -- then clears bind_ip and confirms the
 * daemon falls back to the network's own address with the dedicated
 * one genuinely removed from the bridge, not just unreferenced.
 */
#include "httpclient.h"
#include "json.h"
#include "rtnetlink.h"
#include "test_image_fixture.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7641
#define PORT_ARG "--port=7641"
#define TEST_NETWORK_NAME "bindtest"
#define TEST_NETWORK_SUBNET "172.96.0.0"
#define NET_ADDR "172.96.0.1"
#define BIND_IP "172.96.0.50"

static char g_data_dir[PATH_MAX];

static const char *str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/kanxeod";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/kanxeod", dargv, environ);
		perror("execve build/kanxeod");
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

/* Real, direct kernel-state check via getifaddrs() -- deliberately
 * bypasses the daemon entirely (unlike route_dump_contains() in
 * test_routes.c, which goes through the daemon's own GET endpoint)
 * since this is exactly the kind of check that must be independent of
 * whatever the daemon itself claims, to prove the bridge's real
 * address set, not just its own bookkeeping. */
static int ifname_has_ipv4(const char *ifname, const char *ip_str)
{
	struct ifaddrs *list, *p;
	struct in_addr want;
	int found = 0;

	if (inet_pton(AF_INET, ip_str, &want) != 1)
		return -1;
	if (getifaddrs(&list) != 0)
		return -1;

	for (p = list; p != NULL; p = p->ifa_next) {
		struct sockaddr_in *sin;

		if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET)
			continue;
		if (strcmp(p->ifa_name, ifname) != 0)
			continue;
		sin = (struct sockaddr_in *)(void *)p->ifa_addr;
		if (sin->sin_addr.s_addr == want.s_addr) {
			found = 1;
			break;
		}
	}
	freeifaddrs(list);
	return found;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client_initial, client_net_addr, client_bind_ip, client_fallback;
	int ok = 1;
	struct kx_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	kx_client_init(&client_initial, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client_initial, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. Create the network bind_ip will eventually live alongside. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client_initial, "POST", "/v1/networks",
	                       "{\"name\":\"" TEST_NETWORK_NAME "\",\"subnet\":\"" TEST_NETWORK_SUBNET
	                       "\",\"prefix_len\":24,\"address\":\"" NET_ADDR "\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/networks, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. Repoint management to it -- a real listen-socket rebind to
	 * NET_ADDR. The response to this exact request still arrives over
	 * the still-open connection client_initial made to 127.0.0.1
	 * (handle_daemon_config_put()'s own documented guarantee), but any
	 * further request needs a client actually connected to the new
	 * address. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client_initial, "PUT", "/v1/system/daemon-config",
	                              "{\"management_network\":\"" TEST_NETWORK_NAME "\"}", &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: PUT management_network, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	kx_client_init(&client_net_addr, NET_ADDR, TEST_PORT);
	if (ok && wait_for_daemon(&client_net_addr, 50) != 0) {
		fprintf(stderr, "FAIL: daemon did not become reachable at " NET_ADDR " after repoint\n");
		ok = 0;
	}

	/* 3. Set a dedicated bind_ip -- a second, real address added to
	 * the SAME bridge, then another rebind, this time to it. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client_net_addr, "PUT", "/v1/system/daemon-config",
	                              "{\"bind_ip\":\"" BIND_IP "\"}", &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: PUT bind_ip, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	kx_client_init(&client_bind_ip, BIND_IP, TEST_PORT);
	if (ok && wait_for_daemon(&client_bind_ip, 50) != 0) {
		fprintf(stderr, "FAIL: daemon did not become reachable at " BIND_IP " after bind_ip set\n");
		ok = 0;
	}

	/* 4. GET daemon-config reports both bind (== bind_ip now) and
	 * bind_ip itself, distinctly. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client_bind_ip, "GET", "/v1/system/daemon-config", NULL, &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: GET daemon-config after bind_ip set, status=%d\n", r.status);
		ok = 0;
	}
	if (ok) {
		const char *bind = str_field(r.json, "bind");
		const char *bind_ip = str_field(r.json, "bind_ip");

		if (bind == NULL || strcmp(bind, BIND_IP) != 0) {
			fprintf(stderr, "FAIL: expected bind=" BIND_IP ", got %s\n", bind != NULL ? bind : "(null)");
			ok = 0;
		}
		if (bind_ip == NULL || strcmp(bind_ip, BIND_IP) != 0) {
			fprintf(stderr, "FAIL: expected bind_ip=" BIND_IP ", got %s\n",
			        bind_ip != NULL ? bind_ip : "(null)");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 5. The network's own address must be completely untouched by
	 * bind_ip's existence. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client_bind_ip, "GET",
	                              "/v1/networks/" TEST_NETWORK_NAME, NULL, &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: GET network after bind_ip set, status=%d\n", r.status);
		ok = 0;
	}
	if (ok) {
		const char *addr = str_field(r.json, "address");

		if (addr == NULL || strcmp(addr, NET_ADDR) != 0) {
			fprintf(stderr, "FAIL: network's own address changed, expected " NET_ADDR ", got %s\n",
			        addr != NULL ? addr : "(null)");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 6. Real kernel-level proof: BOTH addresses now sit on the bridge
	 * simultaneously (via getifaddrs(), independent of the daemon's
	 * own claims). */
	if (ok && ifname_has_ipv4(TEST_NETWORK_NAME, NET_ADDR) != 1) {
		fprintf(stderr, "FAIL: bridge %s missing its own address %s\n", TEST_NETWORK_NAME, NET_ADDR);
		ok = 0;
	}
	if (ok && ifname_has_ipv4(TEST_NETWORK_NAME, BIND_IP) != 1) {
		fprintf(stderr, "FAIL: bridge %s missing dedicated bind_ip %s\n", TEST_NETWORK_NAME, BIND_IP);
		ok = 0;
	}

	/* 7. Clear bind_ip -- falls back to the network's own address, and
	 * the dedicated one is genuinely removed from the bridge, not just
	 * unreferenced. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client_bind_ip, "PUT", "/v1/system/daemon-config",
	                              "{\"bind_ip\":null}", &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: PUT bind_ip clear, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	kx_client_init(&client_fallback, NET_ADDR, TEST_PORT);
	if (ok && wait_for_daemon(&client_fallback, 50) != 0) {
		fprintf(stderr, "FAIL: daemon did not fall back to " NET_ADDR " after bind_ip clear\n");
		ok = 0;
	}

	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client_fallback, "GET", "/v1/system/daemon-config", NULL, &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: GET daemon-config after bind_ip clear, status=%d\n", r.status);
		ok = 0;
	}
	if (ok) {
		const struct json_value *jbind_ip = json_object_get(r.json, "bind_ip");
		const char *bind = str_field(r.json, "bind");

		if (jbind_ip != NULL && jbind_ip->type != JSON_NULL) {
			fprintf(stderr, "FAIL: expected bind_ip null after clear\n");
			ok = 0;
		}
		if (bind == NULL || strcmp(bind, NET_ADDR) != 0) {
			fprintf(stderr, "FAIL: expected bind=" NET_ADDR " after clear, got %s\n",
			        bind != NULL ? bind : "(null)");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 8. Real kernel-level proof the dedicated address is genuinely
	 * gone, not merely unreferenced -- the network's own address must
	 * still be there. The daemon defers this specific removal by
	 * BIND_IP_CLEANUP_DELAY_SECONDS (main.c, ADR-0068) precisely to
	 * avoid racing the response this same request is still delivering
	 * over a connection whose own local address might be the one being
	 * removed -- so this check polls rather than asserting instantly. */
	if (ok) {
		int i;
		int gone = 0;

		for (i = 0; i < 50; i++) {
			int present = ifname_has_ipv4(TEST_NETWORK_NAME, BIND_IP);

			if (present == 0) {
				gone = 1;
				break;
			}
			usleep(100000);
		}
		if (!gone) {
			fprintf(stderr, "FAIL: bridge %s still carries cleared bind_ip %s after waiting\n",
			        TEST_NETWORK_NAME, BIND_IP);
			ok = 0;
		}
	}
	if (ok && ifname_has_ipv4(TEST_NETWORK_NAME, NET_ADDR) != 1) {
		fprintf(stderr, "FAIL: bridge %s lost its own address %s after bind_ip clear\n",
		        TEST_NETWORK_NAME, NET_ADDR);
		ok = 0;
	}

	if (ok)
		printf("DAEMON BIND IP RESULT: PASS\n");
	else
		printf("DAEMON BIND IP RESULT: FAIL\n");

	stop_daemon(daemon_pid);

	/*
	 * TEST_NETWORK_NAME held is_management for this whole run and was
	 * therefore never deletable via DELETE /v1/networks (network_
	 * delete() refuses while is_management is set, and this test never
	 * had a second network to repoint onto before finishing) -- its
	 * bridge is real, process-independent kernel state (network_init()'s
	 * own "a bridge outlives this process" design) that would otherwise
	 * leak into every subsequent run. Deleted directly, bypassing the
	 * now-dead daemon entirely, the same real-kernel-state responsibility
	 * every other test in this suite already takes for what it creates.
	 */
	{
		int rtfd = rtnl_open();

		if (rtfd >= 0) {
			rtnl_link_delete(rtfd, TEST_NETWORK_NAME);
			rtnl_close(rtfd);
		}
	}

	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
