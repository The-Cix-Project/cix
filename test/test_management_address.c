/*
 * ADR-0287 end-to-end test: the single management address is the one
 * truth for where cixd answers off-box, the network is DERIVED from it,
 * and 127.0.0.1 is always bound. Verified live against a real cixd:
 *
 *  - a fresh daemon is loopback-only (configured=false);
 *  - PUT /system/management-address with a network's own address rebinds
 *    the off-box listener there, reports it bound, and the containing
 *    network derives management=true;
 *  - PUT with a second, dedicated in-subnet address adds it to the SAME
 *    bridge and rebinds again -- confirmed via getifaddrs() reading the
 *    bridge's real kernel address list, independent of the daemon;
 *  - an address in no network's subnet is refused (400);
 *  - DELETE resets to loopback-only: the off-box listener drops to
 *    127.0.0.1, management derives to false, and the dedicated address
 *    is genuinely removed from the bridge (deferred, ADR-0068) while the
 *    network's own address stays.
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
#define TEST_NETWORK_NAME "mgmttest"
#define TEST_NETWORK_SUBNET "172.96.0.0"
#define NET_ADDR "172.96.0.1"    /* the network's own address */
#define DEDICATED "172.96.0.50"  /* a second, in-subnet address cixd binds */
#define NO_NETWORK_ADDR "10.77.0.5" /* in no network's subnet */

static char g_data_dir[PATH_MAX];

static const char *str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static int bool_field(const struct json_value *obj, const char *key)
{
	const struct json_value *v = json_object_get(obj, key);

	return v != NULL && v->type == JSON_BOOL && v->u.boolean;
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

/* Direct kernel-state check via getifaddrs() -- deliberately bypasses
 * the daemon, to prove the bridge's REAL address set rather than the
 * daemon's own bookkeeping. Returns 1 present, 0 absent, -1 on error. */
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
	struct cix_client client_lo, client_net, client_dedicated;
	int ok = 1;
	struct cix_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	cix_client_init(&client_lo, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client_lo, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. Fresh daemon: loopback-only, no management address. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client_lo, "GET", "/v1/system/management-address", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET management-address (initial), status=%d\n", r.status);
		ok = 0;
	} else if (bool_field(r.json, "configured")) {
		fprintf(stderr, "FAIL: fresh daemon reports a management address configured\n");
		ok = 0;
	}
	cix_response_free(&r);

	/* 2. An address in no network's subnet is refused. */
	memset(&r, 0, sizeof(r));
	if (ok && cix_client_request(&client_lo, "PUT", "/v1/system/management-address",
	                             "{\"address\":\"" NO_NETWORK_ADDR "\"}", &r) == 0 &&
	    r.status != 400) {
		fprintf(stderr, "FAIL: PUT address in no network should be 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. Create the network the address will live in. */
	memset(&r, 0, sizeof(r));
	if (ok && (cix_client_request(&client_lo, "POST", "/v1/networks",
	                              "{\"name\":\"" TEST_NETWORK_NAME "\",\"subnet\":\""
	                              TEST_NETWORK_SUBNET "\",\"prefix_len\":24,\"address\":\""
	                              NET_ADDR "\"}", &r) != 0 ||
	           r.status != 201)) {
		fprintf(stderr, "FAIL: POST /v1/networks, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 4. Set the management address to the network's own address. The
	 * reply arrives over this still-open loopback connection; the daemon
	 * is then reachable off-box at NET_ADDR. */
	memset(&r, 0, sizeof(r));
	if (ok && (cix_client_request(&client_lo, "PUT", "/v1/system/management-address",
	                              "{\"address\":\"" NET_ADDR "\"}", &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: PUT management-address " NET_ADDR ", status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	cix_client_init(&client_net, NET_ADDR, TEST_PORT);
	if (ok && wait_for_daemon(&client_net, 50) != 0) {
		fprintf(stderr, "FAIL: daemon not reachable at " NET_ADDR " after set\n");
		ok = 0;
	}
	/* 127.0.0.1 must still answer -- it is always bound. */
	if (ok && wait_for_daemon(&client_lo, 20) != 0) {
		fprintf(stderr, "FAIL: 127.0.0.1 stopped answering after an off-box address was set\n");
		ok = 0;
	}

	/* 5. GET reports configured/address/bound/network; the containing
	 * network derives management=true. */
	memset(&r, 0, sizeof(r));
	if (ok && (cix_client_request(&client_net, "GET", "/v1/system/management-address", NULL, &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: GET management-address after set, status=%d\n", r.status);
		ok = 0;
	}
	if (ok) {
		const char *addr = str_field(r.json, "address");
		const char *bound = str_field(r.json, "bound");
		const char *net = str_field(r.json, "network");

		if (addr == NULL || strcmp(addr, NET_ADDR) != 0 ||
		    bound == NULL || strcmp(bound, NET_ADDR) != 0 ||
		    net == NULL || strcmp(net, TEST_NETWORK_NAME) != 0 ||
		    !bool_field(r.json, "configured")) {
			fprintf(stderr, "FAIL: management-address GET wrong: addr=%s bound=%s net=%s\n",
			        addr ? addr : "(null)", bound ? bound : "(null)", net ? net : "(null)");
			ok = 0;
		}
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (cix_client_request(&client_net, "GET", "/v1/networks/" TEST_NETWORK_NAME, NULL, &r) != 0 ||
	           r.status != 200 || !bool_field(r.json, "management"))) {
		fprintf(stderr, "FAIL: network does not derive management=true, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 6. Move to a second, dedicated in-subnet address -- a real rebind,
	 * and a second address added to the same bridge. */
	memset(&r, 0, sizeof(r));
	if (ok && (cix_client_request(&client_net, "PUT", "/v1/system/management-address",
	                              "{\"address\":\"" DEDICATED "\"}", &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: PUT management-address " DEDICATED ", status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	cix_client_init(&client_dedicated, DEDICATED, TEST_PORT);
	if (ok && wait_for_daemon(&client_dedicated, 50) != 0) {
		fprintf(stderr, "FAIL: daemon not reachable at " DEDICATED " after move\n");
		ok = 0;
	}
	/* Kernel proof: the dedicated address is really on the bridge. */
	if (ok && ifname_has_ipv4(TEST_NETWORK_NAME, DEDICATED) != 1) {
		fprintf(stderr, "FAIL: bridge %s missing dedicated address %s\n", TEST_NETWORK_NAME, DEDICATED);
		ok = 0;
	}
	/* The network's own address is untouched by this. */
	if (ok && ifname_has_ipv4(TEST_NETWORK_NAME, NET_ADDR) != 1) {
		fprintf(stderr, "FAIL: bridge %s lost its own address %s\n", TEST_NETWORK_NAME, NET_ADDR);
		ok = 0;
	}

	/* 7. Reset to loopback-only. */
	memset(&r, 0, sizeof(r));
	if (ok && (cix_client_request(&client_dedicated, "DELETE", "/v1/system/management-address", NULL, &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: DELETE management-address, status=%d\n", r.status);
		ok = 0;
	}
	if (ok && bool_field(r.json, "configured")) {
		fprintf(stderr, "FAIL: still configured after reset\n");
		ok = 0;
	}
	cix_response_free(&r);

	/* 127.0.0.1 is where the box now answers. */
	if (ok && wait_for_daemon(&client_lo, 50) != 0) {
		fprintf(stderr, "FAIL: 127.0.0.1 not answering after reset\n");
		ok = 0;
	}
	memset(&r, 0, sizeof(r));
	if (ok && (cix_client_request(&client_lo, "GET", "/v1/networks/" TEST_NETWORK_NAME, NULL, &r) != 0 ||
	           r.status != 200 || bool_field(r.json, "management"))) {
		fprintf(stderr, "FAIL: network still derives management=true after reset\n");
		ok = 0;
	}
	cix_response_free(&r);

	/* 8. The dedicated address is genuinely removed from the bridge
	 * (deferred, ADR-0068 -- so poll), while the network's own address
	 * stays because it must never be deleted out from under the network. */
	if (ok) {
		int i;
		int gone = 0;

		for (i = 0; i < 50; i++) {
			if (ifname_has_ipv4(TEST_NETWORK_NAME, DEDICATED) == 0) {
				gone = 1;
				break;
			}
			usleep(100000);
		}
		if (!gone) {
			fprintf(stderr, "FAIL: bridge %s still carries dedicated %s after reset\n",
			        TEST_NETWORK_NAME, DEDICATED);
			ok = 0;
		}
	}
	if (ok && ifname_has_ipv4(TEST_NETWORK_NAME, NET_ADDR) != 1) {
		fprintf(stderr, "FAIL: bridge %s lost its own address %s after reset\n",
		        TEST_NETWORK_NAME, NET_ADDR);
		ok = 0;
	}

	if (ok)
		printf("MANAGEMENT ADDRESS RESULT: PASS\n");
	else
		printf("MANAGEMENT ADDRESS RESULT: FAIL\n");

	stop_daemon(daemon_pid);

	/* The network's bridge is real, process-independent kernel state
	 * (network_init()'s "a bridge outlives this process" design). After
	 * reset it is no longer management, so an API delete would work --
	 * but the daemon is stopped now, so remove it directly, the same
	 * real-kernel-state responsibility every test takes for what it
	 * creates. */
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
