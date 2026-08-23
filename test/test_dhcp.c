/*
 * DHCP (ADR-0197) end-to-end over real HTTP: the per-network config,
 * static reservations, and -- the part that matters most -- that the
 * rendered dnsmasq files actually land inside a registered server
 * container, read back through the container file-read endpoint rather
 * than by trusting the write.
 *
 * The serving container here is an ordinary test child, not a real
 * dnsmasq: what is being proved is that thinC renders and delivers the
 * right bytes to the right path. Whether dnsmasq then hands out an
 * address is dnsmasq's own behaviour, verified on real hardware where
 * there is a wire and a client to hand one to.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7643
#define PORT_ARG "--port=7643"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];
static int ok = 1;

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

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/thincd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/thincd", dargv, environ);
		perror("execve build/thincd");
		_exit(127);
	}
	return pid;
}

/* status-only helper: most of this test is "does this request get the
 * answer it should", and spelling that out eleven times would bury the
 * few places that check content. */
static void expect(struct thinc_client *c, const char *method, const char *path, const char *body,
                    int want_status, const char *what)
{
	struct thinc_response r;

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(c, method, path, body, &r) != 0 || r.status != want_status) {
		fprintf(stderr, "FAIL: %s -- expected %d, got %d\n", what, want_status, r.status);
		ok = 0;
	}
	thinc_response_free(&r);
}

int main(void)
{
	pid_t daemon_pid;
	struct thinc_client client;
	struct thinc_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/dhcptest/v1/rootfs",
	         g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/dhcptest", g_data_dir);
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
	if (daemon_pid < 0)
		return 1;
	thinc_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 100) != 0) {
		fprintf(stderr, "FAIL: daemon did not come up\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	expect(&client, "POST", "/v1/networks",
	        "{\"name\":\"dhcplab\",\"subnet\":\"172.30.7.0\",\"prefix_len\":24,"
	        "\"address\":\"172.30.7.1\"}",
	        201, "create the lab network");

	/* 1. A network with no DHCP config is DHCP-off, which is an answer
	 * rather than a 404 -- and it reports the same default lease a PUT
	 * would start from, so reading and enabling agree on the number. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/networks/dhcplab/dhcp", NULL, &r) != 0 ||
	    r.status != 200 || json_object_get(r.json, "enabled") == NULL ||
	    json_object_get(r.json, "enabled")->u.boolean ||
	    (long)json_as_number(json_object_get(r.json, "lease_seconds")) != 3600) {
		fprintf(stderr, "FAIL: an unconfigured network is not reported as DHCP-off\n");
		ok = 0;
	}
	thinc_response_free(&r);

	expect(&client, "GET", "/v1/networks/nosuchnet/dhcp", NULL, 404,
	        "DHCP config of an unknown network");

	/* 2. Every way of enabling DHCP that could not actually serve is
	 * refused at the point of asking, not discovered later by a client
	 * that never got an address. */
	expect(&client, "PUT", "/v1/networks/dhcplab/dhcp",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.200\"}",
	        400, "enabling with no server");
	expect(&client, "PUT", "/v1/networks/dhcplab/dhcp",
	        "{\"enabled\":true,\"range_start\":\"10.0.0.1\",\"range_end\":\"10.0.0.9\","
	        "\"server\":\"dhcpsrv\"}",
	        400, "a range outside the network's own subnet");
	expect(&client, "PUT", "/v1/networks/dhcplab/dhcp",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.200\",\"range_end\":\"172.30.7.100\","
	        "\"server\":\"dhcpsrv\"}",
	        400, "a range that runs backwards");

	/* 3. Static reservations: shape enforced, and neither a MAC nor an
	 * address may be claimed twice. */
	expect(&client, "POST", "/v1/dhcp/static",
	        "{\"mac\":\"aa:bb:cc:dd:ee:01\",\"ip\":\"172.30.7.50\",\"hostname\":\"printer\"}", 201,
	        "a valid reservation");
	expect(&client, "POST", "/v1/dhcp/static",
	        "{\"mac\":\"aa:bb:cc:dd:ee:01\",\"ip\":\"172.30.7.51\"}", 409, "the same MAC twice");
	expect(&client, "POST", "/v1/dhcp/static",
	        "{\"mac\":\"aa:bb:cc:dd:ee:02\",\"ip\":\"172.30.7.50\"}", 409,
	        "two MACs claiming one address");
	expect(&client, "POST", "/v1/dhcp/static",
	        "{\"mac\":\"AA:BB:CC:DD:EE:03\",\"ip\":\"172.30.7.52\"}", 400, "an upper-case MAC");
	expect(&client, "POST", "/v1/dhcp/static", "{\"mac\":\"nonsense\",\"ip\":\"172.30.7.53\"}", 400,
	        "a MAC that is not a MAC");
	/* A hostname becomes a name the server answers for, so it is held
	 * to a DNS label's shape rather than escaped into the file. */
	expect(&client, "POST", "/v1/dhcp/static",
	        "{\"mac\":\"aa:bb:cc:dd:ee:04\",\"ip\":\"172.30.7.54\",\"hostname\":\"bad name\"}", 400,
	        "a hostname with a space");

	/* 4. A container that is not a registered DNS server cannot be the
	 * DHCP server -- that pairing is what makes a lease resolvable. */
	expect(&client, "POST", "/v1/containers",
	        "{\"name\":\"dhcpsrv\",\"image\":\"dhcptest\","
	        "\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"],\"networks\":[\"dhcplab\"]}",
	        201, "create the serving container");
	expect(&client, "PUT", "/v1/networks/dhcplab/dhcp",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.200\","
	        "\"server\":\"dhcpsrv\"}",
	        400, "a server that is running but is not a DNS server");

	expect(&client, "POST", "/v1/dns/servers",
	        "{\"container\":\"dhcpsrv\",\"hosts_path\":\"/etc/dnsmasq-hosts\"}", 201,
	        "register it as a DNS server");
	expect(&client, "PUT", "/v1/networks/dhcplab/dhcp",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.200\","
	        "\"server\":\"dhcpsrv\",\"router\":\"172.30.7.1\"}",
	        200, "enabling DHCP with a registered server");

	/* 5. The payoff: the rendered files are really inside the serving
	 * container, read back rather than assumed from a successful write. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET",
	                       "/v1/containers/dhcpsrv/files?path=/etc/dnsmasq-dhcp.conf", NULL,
	                       &r) != 0 ||
	    r.status != 200 || r.body == NULL ||
	    strstr(r.body, "dhcp-range=172.30.7.100,172.30.7.200,3600s") == NULL ||
	    strstr(r.body, "dhcp-option=3,172.30.7.1") == NULL) {
		fprintf(stderr, "FAIL: the rendered dhcp conf did not reach the server (status=%d)\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET",
	                       "/v1/containers/dhcpsrv/files?path=/etc/dnsmasq-dhcp-hosts", NULL,
	                       &r) != 0 ||
	    r.status != 200 || r.body == NULL ||
	    strstr(r.body, "aa:bb:cc:dd:ee:01,172.30.7.50,printer") == NULL) {
		fprintf(stderr, "FAIL: the rendered reservations did not reach the server (status=%d)\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 6. Leases come from the server, so with no lease file there are
	 * simply none -- an empty list, not an error. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/dhcp/leases", NULL, &r) != 0 ||
	    r.status != 200 || json_object_get(r.json, "leases") == NULL ||
	    json_object_get(r.json, "leases")->type != JSON_ARRAY) {
		fprintf(stderr, "FAIL: GET leases, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 7. A reservation removed is a reservation gone from the file the
	 * server reads, not just from our own list. */
	expect(&client, "DELETE", "/v1/dhcp/static/aa:bb:cc:dd:ee:01", NULL, 204,
	        "remove the reservation");
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET",
	                       "/v1/containers/dhcpsrv/files?path=/etc/dnsmasq-dhcp-hosts", NULL,
	                       &r) != 0 ||
	    r.status != 200 || r.body == NULL || strstr(r.body, "aa:bb:cc:dd:ee:01") != NULL) {
		fprintf(stderr, "FAIL: a removed reservation is still in the server's own file\n");
		ok = 0;
	}
	thinc_response_free(&r);
	expect(&client, "DELETE", "/v1/dhcp/static/aa:bb:cc:dd:ee:99", NULL, 404,
	        "remove a reservation that was never there");

	/* 8. Deleting the network takes its DHCP config with it: a range
	 * for a network that no longer exists is one nothing can serve. */
	expect(&client, "DELETE", "/v1/networks/dhcplab/dhcp", NULL, 204, "disable DHCP");
	expect(&client, "DELETE", "/v1/containers/dhcpsrv", NULL, 204, "remove the container");
	/*
	 * The bridge is real kernel state that outlives this daemon, so
	 * leaving it behind would make the NEXT run of this test fail at
	 * its first request with a 404 that looks nothing like its cause.
	 * Settle-poll the container away first -- it still holds the
	 * network's attachment for a moment after DELETE returns.
	 */
	{
		int i;

		for (i = 0; i < 50; i++) {
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "GET", "/v1/containers/dhcpsrv", NULL, &r) == 0 &&
			    r.status == 404) {
				thinc_response_free(&r);
				break;
			}
			thinc_response_free(&r);
			usleep(100 * 1000);
		}
	}
	expect(&client, "DELETE", "/v1/networks/dhcplab", NULL, 204, "remove the lab network");

	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);
	printf("DHCP RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
