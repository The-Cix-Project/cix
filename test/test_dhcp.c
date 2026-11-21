/*
 * DHCP (ADR-0197, ADR-0386) end-to-end over real HTTP: the per-network
 * config, static reservations, failover pairs, and -- the part that
 * matters most -- that the rendered dhcpd.conf actually lands inside a
 * registered server container, read back through the container
 * file-read endpoint rather than by trusting the write.
 *
 * The serving container here is an ordinary test child, not a real
 * dhcpd: what is being proved is that Cix renders and delivers the
 * right bytes to the right path. What dhcpd does with them is dhcpd's
 * own behaviour, measured on real hardware (ADR-0386's Context), and
 * the render itself is checked byte for byte by test_dhcpdconf.
 */
#include "httpclient.h"
#include "json.h"
#include "test_cleanup.h"
#include "test_image_fixture.h"
#include "test_drift.h"

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

/* status-only helper: most of this test is "does this request get the
 * answer it should", and spelling that out eleven times would bury the
 * few places that check content. */
static void expect(struct cix_client *c, const char *method, const char *path, const char *body,
                    int want_status, const char *what)
{
	struct cix_response r;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, method, path, body, &r) != 0 || r.status != want_status) {
		fprintf(stderr, "FAIL: %s -- expected %d, got %d\n", what, want_status, r.status);
		ok = 0;
	}
	cix_response_free(&r);
}

/* Bounded poll for a file inside a container to contain `want`.
 * Everything that changes a range restarts the serving containers on a
 * jittered timer, so a single read races the recreate -- what matters
 * is that the content arrives, not how fast. */
static int wait_for_file(struct cix_client *c, const char *path, const char *want)
{
	struct cix_response r;
	int i;

	for (i = 0; i < 150; i++) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200 &&
		    r.body != NULL && strstr(r.body, want) != NULL) {
			cix_response_free(&r);
			return 1;
		}
		cix_response_free(&r);
		usleep(200 * 1000);
	}
	ok = 0;
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;

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
	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 100) != 0) {
		fprintf(stderr, "FAIL: daemon did not come up\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	expect(&client, "POST", "/v1/networks",
	        "{\"name\":\"selftest-dhcp\",\"subnet\":\"172.30.7.0\",\"prefix_len\":24,"
	        "\"address\":\"172.30.7.1\"}",
	        201, "create the lab network");
	/*
	 * A second network, created here rather than later, because the
	 * serving container has to be ATTACHED to a network before DHCP can
	 * be enabled for it there -- and a live attach would not survive
	 * what happens next. Enabling a range restarts the servers that
	 * serve it, and a live attachment is deliberately ephemeral (it is
	 * never written into the container's own definition), so the
	 * attachment would be gone by the second assertion. Declaring both
	 * at creation is what a real multi-segment DHCP server does anyway.
	 */
	expect(&client, "POST", "/v1/networks",
	        "{\"name\":\"selftest-dhcp2\",\"subnet\":\"172.30.8.0\",\"prefix_len\":24,"
	        "\"address\":\"172.30.8.1\"}",
	        201, "create the second lab network");

	/* 1. A network with no DHCP config is DHCP-off, which is an answer
	 * rather than a 404 -- and it reports the same default lease a PUT
	 * would start from, so reading and enabling agree on the number. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/dhcp/networks/selftest-dhcp", NULL, &r) != 0 ||
	    r.status != 200 || json_object_get(r.json, "enabled") == NULL ||
	    json_object_get(r.json, "enabled")->u.boolean ||
	    (long)json_as_number(json_object_get(r.json, "lease_seconds")) != 3600) {
		fprintf(stderr, "FAIL: an unconfigured network is not reported as DHCP-off\n");
		ok = 0;
	}
	cix_response_free(&r);

	expect(&client, "GET", "/v1/dhcp/networks/nosuchnet", NULL, 404,
	        "DHCP config of an unknown network");

	/* 2. Every way of enabling DHCP that could not actually serve is
	 * refused at the point of asking, not discovered later by a client
	 * that never got an address. */
	expect(&client, "PUT", "/v1/dhcp/networks/selftest-dhcp",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.200\"}",
	        400, "enabling with no server named");
	expect(&client, "PUT", "/v1/dhcp/networks/selftest-dhcp",
	        "{\"enabled\":true,\"range_start\":\"10.0.0.1\",\"range_end\":\"10.0.0.9\"}",
	        400, "a range outside the network's own subnet");
	expect(&client, "PUT", "/v1/dhcp/networks/selftest-dhcp",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.200\",\"range_end\":\"172.30.7.100\"}",
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

	/*
	 * 4. Two registered servers forming a failover pair (ADR-0386): both
	 * serve the whole range, and each one's dhcpd.conf names the other.
	 */
	expect(&client, "POST", "/v1/containers",
	        "{\"name\":\"dhcpsrv\",\"image\":\"dhcptest\","
	        "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"]}],"
	        "\"networks\":[\"selftest-dhcp\",\"selftest-dhcp2\"],"
	        "\"restart\":\"always\"}",
	        201, "create the first serving container, on both segments");
	expect(&client, "POST", "/v1/containers",
	        "{\"name\":\"dhcpsrv2\",\"image\":\"dhcptest\","
	        "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"]}],\"networks\":[\"selftest-dhcp\"],"
	        "\"restart\":\"always\"}",
	        201, "create the second serving container");
	expect(&client, "POST", "/v1/dhcp/servers", "{\"container\":\"dhcpsrv\"}", 201,
	        "register the first DHCP server");
	expect(&client, "POST", "/v1/dhcp/servers", "{\"container\":\"dhcpsrv2\"}", 201,
	        "register the second DHCP server");
	expect(&client, "POST", "/v1/dhcp/servers", "{\"container\":\"dhcpsrv\"}", 409,
	        "registering the same server twice");
	expect(&client, "POST", "/v1/dhcp/servers", "{\"container\":\"nosuchcontainer\"}", 404,
	        "registering a container that does not exist");

	/* A pair is two different servers, and a range names at most two. */
	expect(&client, "PUT", "/v1/dhcp/networks/selftest-dhcp",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.200\","
	        "\"servers\":[\"dhcpsrv\",\"dhcpsrv\"]}",
	        400, "a pair of one server twice");
	expect(&client, "PUT", "/v1/dhcp/networks/selftest-dhcp",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.200\","
	        "\"servers\":[\"dhcpsrv\",\"dhcpsrv2\",\"dhcpsrv\"]}",
	        400, "three servers on one range");
	/* A reservation is a fixed address dhcpd gives without a lease, so
	 * a range may not cover one (172.30.7.50 is reserved above). */
	expect(&client, "PUT", "/v1/dhcp/networks/selftest-dhcp",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.40\",\"range_end\":\"172.30.7.60\","
	        "\"servers\":[\"dhcpsrv\"]}",
	        400, "a range covering a reservation");

	expect(&client, "PUT", "/v1/dhcp/networks/selftest-dhcp",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.200\","
	        "\"router\":\"172.30.7.1\",\"dns_servers\":[\"172.30.7.53\"],"
	        "\"servers\":[\"dhcpsrv\",\"dhcpsrv2\"]}",
	        200, "enabling DHCP on a failover pair");
	expect(&client, "POST", "/v1/dhcp/static", "{\"mac\":\"aa:bb:cc:dd:ee:05\",\"ip\":\"172.30.7.150\"}",
	        400, "a reservation inside a pool");

	/* The pair and the DNS servers are reported. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/dhcp/networks/selftest-dhcp", NULL, &r) != 0 ||
	    r.status != 200 || r.body == NULL ||
	    strstr(r.body, "\"servers\":[\"dhcpsrv\",\"dhcpsrv2\"]") == NULL ||
	    strstr(r.body, "\"dns_servers\":[\"172.30.7.53\"]") == NULL) {
		fprintf(stderr, "FAIL: the range does not report its pair and DNS servers: %s\n",
		        r.body != NULL ? r.body : "(none)");
		ok = 0;
	}
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/dhcp/servers", NULL, &r) != 0 ||
	    r.status != 200 || r.body == NULL ||
	    strstr(r.body, "\"partner\":\"dhcpsrv2\"") ==
	        NULL) {
		fprintf(stderr, "FAIL: the servers read does not name the partner: %s\n",
		        r.body != NULL ? r.body : "(none)");
		ok = 0;
	}
	cix_response_free(&r);

	/* Each member's own dhcpd.conf: the name that sorts first is the
	 * primary and carries mclt and split; both serve the whole range
	 * through the pair's peer. */
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv/files?path=/etc/dhcpd.conf",
	                    "failover peer \"cix\" {\n  primary;\n  mclt 3600;\n  split 128;\n"))
		fprintf(stderr, "FAIL: dhcpsrv is not the pair's primary\n");
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv2/files?path=/etc/dhcpd.conf",
	                    "failover peer \"cix\" {\n  secondary;\n"))
		fprintf(stderr, "FAIL: dhcpsrv2 is not the pair's secondary\n");
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv2/files?path=/etc/dhcpd.conf",
	                    "    failover peer \"cix\";\n    range 172.30.7.100 172.30.7.200;\n"))
		fprintf(stderr, "FAIL: the secondary does not serve the whole range through the pair\n");
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv2/files?path=/etc/dhcpd.conf",
	                    "  peer address 172.30.7."))
		fprintf(stderr, "FAIL: the secondary's peer is not on the shared network\n");

	/*
	 * A registered DNS server on a DIFFERENT network gets none of this
	 * range. It has no interface in that subnet, so the pool would be
	 * one it could not serve on -- and handing it one would also
	 * restart a container that had no business being restarted.
	 */
	expect(&client, "POST", "/v1/networks",
	        "{\"name\":\"dhcpother\",\"subnet\":\"172.30.9.0\",\"prefix_len\":24,"
	        "\"address\":\"172.30.9.1\"}",
	        201, "create an unrelated network");
	expect(&client, "POST", "/v1/containers",
	        "{\"name\":\"dhcpelsewhere\",\"image\":\"dhcptest\","
	        "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"]}],\"networks\":[\"dhcpother\"],"
	        "\"restart\":\"always\"}",
	        201, "a server on the unrelated network");
	expect(&client, "POST", "/v1/dhcp/servers", "{\"container\":\"dhcpelsewhere\"}", 201,
	        "register it as a DHCP server too");
	/* Naming a server that is not on this wire is refused: it has no
	 * interface in the subnet, so its half of the pool would be handed
	 * to something that cannot answer for it. */
	expect(&client, "PUT", "/v1/dhcp/networks/selftest-dhcp",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.200\","
	        "\"servers\":[\"dhcpsrv\",\"dhcpelsewhere\"]}",
	        400, "naming a server that is not on this network");
	/* Its own file declares only its own network, with no pool. */
	if (!wait_for_file(&client, "/v1/containers/dhcpelsewhere/files?path=/etc/dhcpd.conf",
	                    "subnet 172.30.9.0 netmask 255.255.255.0 { }\n"))
		fprintf(stderr, "FAIL: a registered server did not get its own network declared\n");
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET",
	                       "/v1/containers/dhcpelsewhere/files?path=/etc/dhcpd.conf", NULL,
	                       &r) == 0 && r.status == 200 && r.body != NULL &&
	    strstr(r.body, "range ") != NULL) {
		fprintf(stderr, "FAIL: a server no range names was given one anyway\n");
		ok = 0;
	}
	cix_response_free(&r);
	/* And it did not change the pair on the network it is not on. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/dhcp/networks/selftest-dhcp", NULL, &r) != 0 ||
	    r.status != 200 || r.body == NULL ||
	    strstr(r.body, "\"servers\":[\"dhcpsrv\",\"dhcpsrv2\"]") == NULL) {
		fprintf(stderr, "FAIL: an unrelated server changed this network's pair\n");
		ok = 0;
	}
	cix_response_free(&r);
	expect(&client, "DELETE", "/v1/dhcp/servers/dhcpelsewhere", NULL, 204, "unregister it");
	expect(&client, "DELETE", "/v1/containers/dhcpelsewhere", NULL, 204, "remove it");

	/* 5. The payoff: the rendered files are really inside the serving
	 * container, read back rather than assumed from a successful write. */
	/*
	 * Bounded poll, not a single read: enabling a range restarts the
	 * serving containers on a jittered timer, so for a few seconds the
	 * container is mid-recreate. What is being asserted is that the
	 * rendered conf ends up there, not how quickly.
	 */
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv/files?path=/etc/dhcpd.conf",
	                    "subnet 172.30.7.0 netmask 255.255.255.0 {\n"
	                    "  option routers 172.30.7.1;\n"
	                    "  option domain-name-servers 172.30.7.53;\n"
	                    "  default-lease-time 3600;\n"))
		fprintf(stderr, "FAIL: the rendered range did not reach the first server\n");
	/* A reservation is a host declaration, named by its MAC. */
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv/files?path=/etc/dhcpd.conf",
	                    "host r-aa-bb-cc-dd-ee-01 {\n  hardware ethernet aa:bb:cc:dd:ee:01;\n"
	                    "  fixed-address 172.30.7.50;\n  option host-name \"printer\";\n}\n"))
		fprintf(stderr, "FAIL: the rendered reservation did not reach the server\n");

	/*
	 * One server, TWO networks: dhcpsrv is in the pair on selftest-dhcp
	 * and serves selftest-dhcp2 alone. That pool is its own, so it
	 * carries no failover peer the partner does not know about.
	 *
	 * Configured with NO router first. This PUT is a partial update, so
	 * omitting "router" later would KEEP the previous one; the only way
	 * to observe the no-router rendering is on a network that has never
	 * had one. dhcpd sends no router when none is declared.
	 */
	expect(&client, "PUT", "/v1/dhcp/networks/selftest-dhcp2",
	        "{\"enabled\":true,\"range_start\":\"172.30.8.100\",\"range_end\":\"172.30.8.150\","
	        "\"servers\":[\"dhcpsrv\"]}",
	        200, "the same server also serves the second network, with no router");
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv/files?path=/etc/dhcpd.conf",
	                    "subnet 172.30.8.0 netmask 255.255.255.0 {\n  default-lease-time 3600;\n"
	                    "  max-lease-time 3600;\n  pool {\n    range 172.30.8.100 172.30.8.150;\n"))
		fprintf(stderr, "FAIL: a server's own range has a router or a failover peer it should "
		                "not\n");

	/* Now give it one. */
	expect(&client, "PUT", "/v1/dhcp/networks/selftest-dhcp2",
	        "{\"enabled\":true,\"range_start\":\"172.30.8.100\",\"range_end\":\"172.30.8.150\","
	        "\"router\":\"172.30.8.254\",\"servers\":[\"dhcpsrv\"]}",
	        200, "give the second network a router");
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv/files?path=/etc/dhcpd.conf",
	                    "subnet 172.30.8.0 netmask 255.255.255.0 {\n"
	                    "  option routers 172.30.8.254;\n"))
		fprintf(stderr, "FAIL: the second network's router did not reach the shared server\n");
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/dhcpsrv/files?path=/etc/dhcpd.conf",
	                       NULL, &r) != 0 || r.status != 200 || r.body == NULL ||
	    strstr(r.body, "  option routers 172.30.7.1;\n") == NULL) {
		fprintf(stderr, "FAIL: the first network's router went missing when a second network "
		                "was added\n");
		ok = 0;
	}
	cix_response_free(&r);

	expect(&client, "DELETE", "/v1/dhcp/networks/selftest-dhcp2", NULL, 204,
	        "turn the second network's DHCP back off");

	/*
	 * The render survives a restart, which is the property that
	 * actually makes a range take effect. dhcpd reads its conf only
	 * at startup and a restart re-stages the container's own
	 * creation-time files[] -- so a conf written into the RUNNING
	 * container is overwritten by the placeholder on the next restart,
	 * and the range silently never applies. Found exactly that way on
	 * the real box; asserted here by restarting and re-reading.
	 */
	expect(&client, "POST", "/v1/containers/dhcpsrv/stop", NULL, 200, "stop the server");
	{
		int i;

		/* Stop is asynchronous -- wait for it to actually be gone
		 * before asking for it back. */
		for (i = 0; i < 60; i++) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/containers/dhcpsrv", NULL, &r) == 0 &&
			    r.status == 404) {
				cix_response_free(&r);
				break;
			}
			cix_response_free(&r);
			usleep(200 * 1000);
		}
	}
	expect(&client, "POST", "/v1/containers/dhcpsrv/start", NULL, 200, "start it again");
	{
		int i;

		for (i = 0; i < 60; i++) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET",
			                       "/v1/containers/dhcpsrv/files?path=/etc/dhcpd.conf",
			                       NULL, &r) == 0 && r.status == 200 && r.body != NULL &&
			    strstr(r.body, "    range 172.30.7.100 172.30.7.200;\n") != NULL) {
				cix_response_free(&r);
				break;
			}
			cix_response_free(&r);
			usleep(200 * 1000);
		}
		if (i == 60) {
			fprintf(stderr, "FAIL: the rendered range did not survive a restart -- a range that "
			                "only exists until the container restarts is a range that never "
			                "applies\n");
			ok = 0;
		}
	}

	/* 6. Leases come from the server, so with no lease file there are
	 * simply none -- an empty list, not an error. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/dhcp/leases", NULL, &r) != 0 ||
	    r.status != 200 || json_object_get(r.json, "leases") == NULL ||
	    json_object_get(r.json, "leases")->type != JSON_ARRAY) {
		fprintf(stderr, "FAIL: GET leases, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 7. A reservation removed is a reservation gone from the file the
	 * server reads, not just from our own list. */
	expect(&client, "DELETE", "/v1/dhcp/static/aa:bb:cc:dd:ee:01", NULL, 204,
	        "remove the reservation");
	{
		int i;

		/* Bounded poll rather than a single read: the server was
		 * restarted a moment ago, and a write into a container that is
		 * still coming up lands once it is up. */
		for (i = 0; i < 60; i++) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET",
			                       "/v1/containers/dhcpsrv/files?path=/etc/dhcpd.conf",
			                       NULL, &r) == 0 && r.status == 200 && r.body != NULL &&
			    strstr(r.body, "aa:bb:cc:dd:ee:01") == NULL) {
				cix_response_free(&r);
				break;
			}
			cix_response_free(&r);
			usleep(200 * 1000);
		}
		if (i == 60) {
			fprintf(stderr, "FAIL: a removed reservation is still in the server's own file\n");
			ok = 0;
		}
	}
	expect(&client, "DELETE", "/v1/dhcp/static/aa:bb:cc:dd:ee:99", NULL, 404,
	        "remove a reservation that was never there");

	/*
	 * 7b. ADR-0383: a range relayed to a server that is not on its
	 * network. st-relayed has no server on it; dhcprelay1 is on it
	 * and on selftest-dhcp, where the servers are.
	 */
	expect(&client, "POST", "/v1/networks",
	        "{\"name\":\"st-relayed\",\"subnet\":\"172.30.10.0\",\"prefix_len\":24,"
	        "\"address\":\"172.30.10.1\"}",
	        201, "create the relayed network");
	expect(&client, "POST", "/v1/containers",
	        "{\"name\":\"dhcprelay1\",\"image\":\"dhcptest\","
	        "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"]}],"
	        "\"networks\":[\"selftest-dhcp\",\"st-relayed\"],"
	        "\"restart\":\"always\"}",
	        201, "create the relay container, on both networks");
	/* A server whose only route back to the relayed network is the
	 * runtime's own default route, via selftest-dhcp's address -- so
	 * the check must count that route, not only declared ones. */
	expect(&client, "POST", "/v1/containers",
	        "{\"name\":\"dhcpsrv3\",\"image\":\"dhcptest\","
	        "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"]}],"
	        "\"networks\":[\"selftest-dhcp\"],"
	        "\"restart\":\"always\"}",
	        201, "create a server whose default route is the runtime's");
	expect(&client, "POST", "/v1/dhcp/servers", "{\"container\":\"dhcpsrv3\"}", 201,
	        "register it as a DHCP server");
	expect(&client, "POST", "/v1/dhcp/relays", "{\"container\":\"dhcpsrv\"}", 409,
	        "a DHCP server cannot also be a relay");
	expect(&client, "POST", "/v1/dhcp/relays",
	        "{\"container\":\"dhcprelay1\",\"service\":\"main\"}", 201, "register the relay");
	expect(&client, "POST", "/v1/dhcp/relays", "{\"container\":\"dhcprelay1\"}", 409,
	        "registering the same relay twice");
	expect(&client, "POST", "/v1/dhcp/servers", "{\"container\":\"dhcprelay1\"}", 409,
	        "a relay cannot also be a DHCP server");
	expect(&client, "POST", "/v1/dhcp/relays",
	        "{\"container\":\"dhcprelay1x\",\"service\":\"bad name\"}", 404,
	        "a container that does not exist");
	expect(&client, "PUT", "/v1/dhcp/networks/st-relayed",
	        "{\"enabled\":true,\"range_start\":\"172.30.10.100\",\"range_end\":\"172.30.10.150\","
	        "\"servers\":[\"dhcpsrv3\"]}",
	        400, "a server not on the network, and no relay");
	expect(&client, "PUT", "/v1/dhcp/networks/st-relayed",
	        "{\"enabled\":true,\"range_start\":\"172.30.10.100\",\"range_end\":\"172.30.10.150\","
	        "\"servers\":[\"dhcpsrv3\"],\"relays\":[\"nosuchrelay\"]}",
	        400, "a relay that is not registered");
	expect(&client, "PUT", "/v1/dhcp/networks/selftest-dhcp", "{\"relays\":[\"dhcprelay1\"]}",
	        400, "relays on a range whose servers are all attached");
	expect(&client, "PUT", "/v1/dhcp/networks/st-relayed",
	        "{\"enabled\":true,\"range_start\":\"172.30.10.100\",\"range_end\":\"172.30.10.150\","
	        "\"router\":\"172.30.10.1\",\"servers\":[\"dhcpsrv3\"],\"relays\":[\"dhcprelay1\"]}",
	        200, "a relayed range");
	/* The server declares the relayed network's subnet with the pool,
	 * and its own network with none, so it listens there and serves
	 * requests relayed from the other (ADR-0386). */
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv3/files?path=/etc/dhcpd.conf",
	                    "subnet 172.30.10.0 netmask 255.255.255.0 {\n"
	                    "  option routers 172.30.10.1;\n"))
		fprintf(stderr, "FAIL: the server does not declare the relayed range\n");
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv3/files?path=/etc/dhcpd.conf",
	                    "subnet 172.30.7.0 netmask 255.255.255.0 { }\n"))
		fprintf(stderr, "FAIL: the server does not declare its own network\n");
	/* dhcpsrv is already paired with dhcpsrv2: a server is in at most
	 * one pair. */
	expect(&client, "PUT", "/v1/dhcp/networks/st-relayed",
	        "{\"servers\":[\"dhcpsrv\",\"dhcpsrv3\"],\"relays\":[\"dhcprelay1\"]}", 400,
	        "pairing a server that already has a partner");
	/* The relay's file: DNS off, a line from its address on the relayed
	 * network to the server's on the shared one, replies accepted on
	 * eth0 (selftest-dhcp), and it listens only on eth1. */
	if (!wait_for_file(&client, "/v1/containers/dhcprelay1/files?path=/etc/dnsmasq-relay.conf",
	                    "port=0\n"))
		fprintf(stderr, "FAIL: the relay's file was not written\n");
	if (!wait_for_file(&client, "/v1/containers/dhcprelay1/files?path=/etc/dnsmasq-relay.conf",
	                    "dhcp-relay=172.30.10."))
		fprintf(stderr, "FAIL: the relay's file has no relay line from the relayed network\n");
	if (!wait_for_file(&client, "/v1/containers/dhcprelay1/files?path=/etc/dnsmasq-relay.conf",
	                    ",172.30.7."))
		fprintf(stderr, "FAIL: the relay does not send to the server on the shared network\n");
	if (!wait_for_file(&client, "/v1/containers/dhcprelay1/files?path=/etc/dnsmasq-relay.conf",
	                    "interface=eth1\n"))
		fprintf(stderr, "FAIL: the relay does not listen on the relayed network\n");
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/dhcp/networks/st-relayed", NULL, &r) != 0 ||
	    r.status != 200 || r.body == NULL || strstr(r.body, "\"relays\":[\"dhcprelay1\"]") == NULL) {
		fprintf(stderr, "FAIL: the range does not report its relay\n");
		ok = 0;
	}
	cix_response_free(&r);
	/* Unregistering the relay leaves a server that hears nothing there,
	 * so the range is turned off rather than left looking served. */
	expect(&client, "DELETE", "/v1/dhcp/relays/dhcprelay1", NULL, 204, "unregister the relay");
	expect(&client, "DELETE", "/v1/dhcp/relays/dhcprelay1", NULL, 404,
	        "unregister a relay that is not one");
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/dhcp/networks/st-relayed", NULL, &r) != 0 ||
	    r.status != 200 || r.body == NULL || strstr(r.body, "\"enabled\":false") == NULL) {
		fprintf(stderr, "FAIL: a range left with no relay to its only server stayed enabled\n");
		ok = 0;
	}
	cix_response_free(&r);
	expect(&client, "DELETE", "/v1/dhcp/networks/st-relayed", NULL, 204,
	        "remove the relayed range");
	expect(&client, "DELETE", "/v1/containers/dhcprelay1", NULL, 204, "remove the relay");
	expect(&client, "DELETE", "/v1/containers/dhcpsrv3", NULL, 204, "remove the third server");

	/* 8. Deleting the network takes its DHCP config with it: a range
	 * for a network that no longer exists is one nothing can serve. */
	expect(&client, "DELETE", "/v1/dhcp/networks/selftest-dhcp", NULL, 204, "disable DHCP");
	expect(&client, "DELETE", "/v1/containers/dhcpsrv", NULL, 204, "remove the first container");
	expect(&client, "DELETE", "/v1/containers/dhcpsrv2", NULL, 204, "remove the second container");
	/*
	 * A bridge is real kernel state that outlives this daemon, so
	 * leaving one behind makes the NEXT run fail at its first request
	 * with a 404 that looks nothing like its cause.
	 *
	 * This used to settle-poll by hand for 5 seconds (50 x 100ms), and
	 * that is shorter than the daemon's own stop grace --
	 * container_stop_grace_seconds() is max(10, longest stop_timeout)
	 * + 5, so 15 by default. It passed for as long as the containers
	 * happened to stop quickly and failed the moment they did not,
	 * with "expected 204, got 409".
	 *
	 * test_cleanup_containers_and_network() is that wait done once,
	 * with a budget that exceeds the grace and a diagnostic that names
	 * which container is still holding the network. Three files had
	 * hand-rolled versions of this with three different budgets; this
	 * is one of them stopping.
	 */
	if (test_cleanup_containers_and_network(&client, "selftest-dhcp") != 0)
		ok = 0;
	expect(&client, "DELETE", "/v1/networks/dhcpother", NULL, 204, "remove the unrelated network");
	/* selftest-dhcp2 is this test's own second segment (the one-server,
	 * two-networks case). Removing its DHCP config above does not
	 * remove the network, and a leaked bridge breaks the next run. */
	expect(&client, "DELETE", "/v1/networks/selftest-dhcp2", NULL, 204, "remove the second network");
	/* ADR-0383's relayed network: its containers were deleted above and
	 * test_cleanup_containers_and_network() waited out their teardown. */
	expect(&client, "DELETE", "/v1/networks/st-relayed", NULL, 204,
	        "remove the relayed network");

	if (test_assert_no_drift(&client, NULL) != 0)
		ok = 0;
	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);
	printf("DHCP RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
