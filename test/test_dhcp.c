/*
 * DHCP (ADR-0197) end-to-end over real HTTP: the per-network config,
 * static reservations, and -- the part that matters most -- that the
 * rendered dnsmasq files actually land inside a registered server
 * container, read back through the container file-read endpoint rather
 * than by trusting the write.
 *
 * The serving container here is an ordinary test child, not a real
 * dnsmasq: what is being proved is that Cix renders and delivers the
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
	        "{\"name\":\"dhcplab\",\"subnet\":\"172.30.7.0\",\"prefix_len\":24,"
	        "\"address\":\"172.30.7.1\"}",
	        201, "create the lab network");

	/* 1. A network with no DHCP config is DHCP-off, which is an answer
	 * rather than a 404 -- and it reports the same default lease a PUT
	 * would start from, so reading and enabling agree on the number. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/dhcp/networks/dhcplab", NULL, &r) != 0 ||
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
	expect(&client, "PUT", "/v1/dhcp/networks/dhcplab",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.200\"}",
	        400, "enabling with no server named");
	expect(&client, "PUT", "/v1/dhcp/networks/dhcplab",
	        "{\"enabled\":true,\"range_start\":\"10.0.0.1\",\"range_end\":\"10.0.0.9\"}",
	        400, "a range outside the network's own subnet");
	expect(&client, "PUT", "/v1/dhcp/networks/dhcplab",
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
	 * 4. Two registered servers, and the range is split between them.
	 * dnsmasq has no failover protocol and the two share no lease
	 * database, so disjoint pools are the only thing stopping them
	 * handing one address to two machines -- which makes the split the
	 * property worth asserting, not the fact that a file was written.
	 */
	expect(&client, "POST", "/v1/containers",
	        "{\"name\":\"dhcpsrv\",\"image\":\"dhcptest\","
	        "\"services\":[{\"name\":\"main\",\"type\":\"oneshot\",\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"]}],\"networks\":[\"dhcplab\"],"
	        "\"restart\":\"always\"}",
	        201, "create the first serving container");
	expect(&client, "POST", "/v1/containers",
	        "{\"name\":\"dhcpsrv2\",\"image\":\"dhcptest\","
	        "\"services\":[{\"name\":\"main\",\"type\":\"oneshot\",\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"]}],\"networks\":[\"dhcplab\"],"
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

	/* A range with fewer addresses than servers cannot be split, and
	 * rounding one server down to nothing would quietly make it not a
	 * server at all. */
	expect(&client, "PUT", "/v1/dhcp/networks/dhcplab",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.100\","
	        "\"servers\":[\"dhcpsrv\",\"dhcpsrv2\"]}",
	        400, "one address between two servers");

	expect(&client, "PUT", "/v1/dhcp/networks/dhcplab",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.200\","
	        "\"router\":\"172.30.7.1\",\"servers\":[\"dhcpsrv\",\"dhcpsrv2\"]}",
	        200, "enabling DHCP across both servers");

	/* The two slices are reported, adjacent, and cover the whole range
	 * with nothing shared: 101 addresses over two servers is 51 + 50. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/dhcp/networks/dhcplab", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET the split, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *slices = json_object_get(r.json, "slices");

		if (slices == NULL || slices->type != JSON_ARRAY || slices->u.array.count != 2) {
			fprintf(stderr, "FAIL: two servers did not produce two slices\n");
			ok = 0;
		} else {
			const struct json_value *a = slices->u.array.items[0];
			const struct json_value *b = slices->u.array.items[1];

			if (strcmp(json_str_field(a, "range_start"), "172.30.7.100") != 0 ||
			    strcmp(json_str_field(a, "range_end"), "172.30.7.150") != 0 ||
			    strcmp(json_str_field(b, "range_start"), "172.30.7.151") != 0 ||
			    strcmp(json_str_field(b, "range_end"), "172.30.7.200") != 0) {
				fprintf(stderr, "FAIL: the slices are not disjoint and adjacent: %s-%s / %s-%s\n",
				        json_str_field(a, "range_start"), json_str_field(a, "range_end"),
				        json_str_field(b, "range_start"), json_str_field(b, "range_end"));
				ok = 0;
			}
		}
	}
	cix_response_free(&r);

	/* And each server's own rendered conf carries only its own slice --
	 * the split is real in the file dnsmasq reads, not just in the
	 * answer we give about it. */
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv2/files?path=/etc/dnsmasq-dhcp.conf",
	                    "dhcp-range=172.30.7.151,172.30.7.200,3600s"))
		fprintf(stderr, "FAIL: the second server did not get its own slice\n");
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET",
	                       "/v1/containers/dhcpsrv2/files?path=/etc/dnsmasq-dhcp.conf", NULL,
	                       &r) == 0 && r.status == 200 && r.body != NULL &&
	    strstr(r.body, "172.30.7.100,") != NULL) {
		fprintf(stderr, "FAIL: the second server also carries the first's slice\n");
		ok = 0;
	}
	cix_response_free(&r);

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
	        "\"services\":[{\"name\":\"main\",\"type\":\"oneshot\",\"cmd\":[\"/bin/daemon_child\",\"60\",\"0\"]}],\"networks\":[\"dhcpother\"],"
	        "\"restart\":\"always\"}",
	        201, "a server on the unrelated network");
	expect(&client, "POST", "/v1/dhcp/servers", "{\"container\":\"dhcpelsewhere\"}", 201,
	        "register it as a DHCP server too");
	/* Naming a server that is not on this wire is refused: it has no
	 * interface in the subnet, so its slice would be addresses handed
	 * to something that cannot answer for them. */
	expect(&client, "PUT", "/v1/dhcp/networks/dhcplab",
	        "{\"enabled\":true,\"range_start\":\"172.30.7.100\",\"range_end\":\"172.30.7.200\","
	        "\"servers\":[\"dhcpsrv\",\"dhcpelsewhere\"]}",
	        400, "naming a server that is not on this network");
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET",
	                       "/v1/containers/dhcpelsewhere/files?path=/etc/dnsmasq-dhcp.conf", NULL,
	                       &r) == 0 && r.status == 200 && r.body != NULL &&
	    strstr(r.body, "dhcp-range=") != NULL) {
		fprintf(stderr, "FAIL: a server no range names was given one anyway\n");
		ok = 0;
	}
	cix_response_free(&r);
	/* And it did not change the split on the network it is not on. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/dhcp/networks/dhcplab", NULL, &r) != 0 ||
	    r.status != 200 || json_object_get(r.json, "slices") == NULL ||
	    json_object_get(r.json, "slices")->u.array.count != 2) {
		fprintf(stderr, "FAIL: an unrelated server changed this network's split\n");
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
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv/files?path=/etc/dnsmasq-dhcp.conf",
	                    "dhcp-range=172.30.7.100,172.30.7.150,3600s"))
		fprintf(stderr, "FAIL: the rendered dhcp conf did not reach the first server\n");
	if (!wait_for_file(&client, "/v1/containers/dhcpsrv/files?path=/etc/dnsmasq-dhcp.conf",
	                    "dhcp-option=3,172.30.7.1"))
		fprintf(stderr, "FAIL: the router option did not reach the first server\n");

	if (!wait_for_file(&client, "/v1/containers/dhcpsrv/files?path=/etc/dnsmasq-dhcp-hosts",
	                    "aa:bb:cc:dd:ee:01,172.30.7.50,printer"))
		fprintf(stderr, "FAIL: the rendered reservations did not reach the server\n");

	/*
	 * The render survives a restart, which is the property that
	 * actually makes a range take effect. dnsmasq reads its conf only
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
			                       "/v1/containers/dhcpsrv/files?path=/etc/dnsmasq-dhcp.conf",
			                       NULL, &r) == 0 && r.status == 200 && r.body != NULL &&
			    strstr(r.body, "dhcp-range=172.30.7.100,172.30.7.150,3600s") != NULL) {
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
			                       "/v1/containers/dhcpsrv/files?path=/etc/dnsmasq-dhcp-hosts",
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

	/* 8. Deleting the network takes its DHCP config with it: a range
	 * for a network that no longer exists is one nothing can serve. */
	expect(&client, "DELETE", "/v1/dhcp/networks/dhcplab", NULL, 204, "disable DHCP");
	expect(&client, "DELETE", "/v1/containers/dhcpsrv", NULL, 204, "remove the first container");
	expect(&client, "DELETE", "/v1/containers/dhcpsrv2", NULL, 204, "remove the second container");
	/*
	 * The bridge is real kernel state that outlives this daemon, so
	 * leaving it behind would make the NEXT run of this test fail at
	 * its first request with a 404 that looks nothing like its cause.
	 * Settle-poll the container away first -- it still holds the
	 * network's attachment for a moment after DELETE returns.
	 */
	{
		const char *gone[] = { "/v1/containers/dhcpsrv", "/v1/containers/dhcpsrv2" };
		size_t g;
		int i;

		for (g = 0; g < sizeof(gone) / sizeof(gone[0]); g++) {
			for (i = 0; i < 50; i++) {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", gone[g], NULL, &r) == 0 &&
				    r.status == 404) {
					cix_response_free(&r);
					break;
				}
				cix_response_free(&r);
				usleep(100 * 1000);
			}
		}
	}
	expect(&client, "DELETE", "/v1/networks/dhcplab", NULL, 204, "remove the lab network");
	expect(&client, "DELETE", "/v1/networks/dhcpother", NULL, 204, "remove the unrelated network");

	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);
	printf("DHCP RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
