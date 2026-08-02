/*
 * ADR-0038 end-to-end test: proves POST/DELETE /v1/networks/{name}/
 * interfaces (daemon/src/network.c's network_attach_interface()/
 * network_detach_interface()) over real HTTP.
 *
 * What this file can and can't prove, honestly: this dev sandbox has
 * no real, physically-backed NIC visible in its own root netns at all
 * (confirmed directly, same as ADR-0022's own documented gap) -- every
 * interface this sandbox could create for itself (veth, dummy, or
 * anything else) lives under /sys/devices/virtual/net/, which
 * enumerate_net_one() (daemon/src/device.c) deliberately excludes from
 * "net:" device discovery. That means the *positive* path -- attach a
 * real assignable interface, confirm it becomes unassignable
 * afterward, confirm a VLAN sub-interface becomes the actual bridge
 * port -- needs real hardware this environment doesn't have, and isn't
 * attempted here (the mechanism itself, rtnl_vlan_create()/
 * rtnl_link_clear_master(), is proven directly against a real veth
 * pair in test/test_rtnetlink.c, which needs no device_find() gate at
 * all). What *is* fully provable here, and is exactly what this test
 * covers: every validation path network_attach_interface() enforces
 * before it would ever touch the kernel, including that a veth -- a
 * real, kernel-backed interface, just not a real physical one -- is
 * correctly rejected the same way an unknown name is, proving
 * enumerate_net_one()'s existing /virtual/net/ exclusion is reused
 * unchanged for this new endpoint, not reimplemented.
 */
#include "httpclient.h"
#include "json.h"
#include "rtnetlink.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7633
#define PORT_ARG "--port=7633"
#define VETH_A "kanxeo-nif-a"
#define VETH_B "kanxeo-nif-b"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

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

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	int rtfd;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/images/netifacetest/rootfs", g_data_dir);

	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* A real, kernel-backed veth pair -- visible in /sys/class/net,
	 * genuinely not a made-up name, but still a software netdev
	 * (/virtual/net/), exactly the case this file's own header comment
	 * explains. */
	rtfd = rtnl_open();
	if (rtfd < 0) {
		perror("rtnl_open");
		return 1;
	}
	rtnl_link_delete(rtfd, VETH_A);
	if (rtnl_veth_create(rtfd, VETH_A, VETH_B) != 0) {
		perror("rtnl_veth_create");
		rtnl_close(rtfd);
		return 1;
	}
	rtnl_close(rtfd);

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;

	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	/* 1. a gateway-less network (the new default) to attach to */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"nifnet\",\"subnet\":\"172.48.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST nifnet, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. unknown ifname rejected */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks/nifnet/interfaces",
	                       "{\"ifname\":\"kanxeo-nif-nonexistent\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: unknown ifname expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. a real, kernel-backed veth end is still rejected -- it's
	 * software (/virtual/net/), the same exclusion GET /v1/devices'
	 * own "net:" discovery already applies, reused unchanged here. */
	memset(&r, 0, sizeof(r));
	{
		char body[128];

		snprintf(body, sizeof(body), "{\"ifname\":\"%s\"}", VETH_A);
		if (kx_client_request(&client, "POST", "/v1/networks/nifnet/interfaces", body, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: veth ifname expected 400, got %d\n", r.status);
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 4. attach to an unknown network 404s */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks/nosuchnet/interfaces",
	                       "{\"ifname\":\"kanxeo-nif-a\"}", &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: attach to unknown network expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. an out-of-range vlan_id is rejected */
	memset(&r, 0, sizeof(r));
	{
		char body[128];

		snprintf(body, sizeof(body), "{\"ifname\":\"%s\",\"vlan_id\":4095}", VETH_A);
		if (kx_client_request(&client, "POST", "/v1/networks/nifnet/interfaces", body, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: vlan_id 4095 expected 400, got %d\n", r.status);
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 6. none of the above ever actually attached anything -- the
	 * network's own interfaces[] stays empty throughout */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/networks/nifnet", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET nifnet, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *ifaces = json_object_get(r.json, "interfaces");

		if (ifaces == NULL || ifaces->type != JSON_ARRAY || ifaces->u.array.count != 0) {
			fprintf(stderr, "FAIL: nifnet.interfaces expected empty, got %zu entries\n",
			        ifaces != NULL && ifaces->type == JSON_ARRAY ? ifaces->u.array.count : (size_t)-1);
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 7. detaching something never attached 404s */
	memset(&r, 0, sizeof(r));
	{
		char path[128];

		snprintf(path, sizeof(path), "/v1/networks/nifnet/interfaces/%s", VETH_A);
		if (kx_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr, "FAIL: detach never-attached expected 404, got %d\n", r.status);
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* cleanup */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/nifnet", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "warning: could not clean up 'nifnet' network, status=%d\n", r.status);
	}
	kx_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	rtfd = rtnl_open();
	if (rtfd >= 0) {
		rtnl_link_delete(rtfd, VETH_A);
		rtnl_close(rtfd);
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "NETWORK INTERFACES RESULT: PASS\n" : "NETWORK INTERFACES RESULT: FAIL\n");
	return ok ? 0 : 1;
}
