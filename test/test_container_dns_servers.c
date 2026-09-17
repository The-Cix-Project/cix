/*
 * ADR-0143 end-to-end test: the optional "dns_servers" field on
 * POST /v1/containers -- staged as a real /etc/resolv.conf directly
 * into the container's own overlay upperdir before its process ever
 * execve()s, echoed back on GET (id-only, like "files"), and rejected
 * (400) if combined with an explicit files[] entry for the same path.
 *
 * Unlike storage-placement testing, this needs no real disposable
 * disk -- the staged file lands under this dev sandbox's own scratch
 * data dir, fully inspectable from the host side.
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

#define TEST_PORT 7670
#define PORT_ARG "--port=7670"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
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

/* Reads the whole content of path into buf (NUL-terminated). Returns
 * 0 on success, -1 otherwise. */
static int read_whole_file(const char *path, char *buf, size_t buf_size)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (f == NULL)
		return -1;
	n = fread(buf, 1, buf_size - 1, f);
	fclose(f);
	buf[n] = '\0';
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	struct cix_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/dnstest/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/dnstest", g_data_dir);
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
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. Invalid IPv4 entry -> 400. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"badip\",\"image\":\"dnstest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],"
	                       "\"dns_servers\":[\"not-an-ip\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST with invalid dns_servers entry expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 2. Too many entries (max 3) -> 400. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"toomany\",\"image\":\"dnstest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],"
	                       "\"dns_servers\":[\"1.1.1.1\",\"2.2.2.2\",\"3.3.3.3\",\"4.4.4.4\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST with 4 dns_servers entries expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. Combining dns_servers with an explicit files[] entry for
	 * /etc/resolv.conf -> 400, a real, unresolvable ambiguity. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"conflict\",\"image\":\"dnstest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],"
	                       "\"dns_servers\":[\"1.1.1.1\"],"
	                       "\"files\":[{\"path\":\"/etc/resolv.conf\",\"content\":\"nameserver 9.9.9.9\\n\"}]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST with dns_servers + files[/etc/resolv.conf] expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 4. Real success path: dns_servers actually staged, and echoed
	 * back correctly on GET. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"realdns\",\"image\":\"dnstest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],"
	                       "\"dns_servers\":[\"192.168.15.101\",\"192.168.15.102\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST realdns, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jarr = json_object_get(r.json, "dns_servers");

		if (jarr == NULL || jarr->type != JSON_ARRAY || jarr->u.array.count != 2 ||
		    !str_eq(json_as_string(jarr->u.array.items[0]), "192.168.15.101") ||
		    !str_eq(json_as_string(jarr->u.array.items[1]), "192.168.15.102")) {
			fprintf(stderr, "FAIL: POST realdns response did not echo dns_servers correctly\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/realdns", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET realdns, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jarr = json_object_get(r.json, "dns_servers");

		if (jarr == NULL || jarr->type != JSON_ARRAY || jarr->u.array.count != 2) {
			fprintf(stderr, "FAIL: GET realdns did not echo dns_servers correctly\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 5. The real content actually staged into the container's own
	 * overlay upperdir, readable directly from the host side. */
	{
		char resolv_path[PATH_MAX];
		char content[256];

		snprintf(resolv_path, sizeof(resolv_path), "%s/containers/realdns/upper/etc/resolv.conf",
		         g_data_dir);
		if (read_whole_file(resolv_path, content, sizeof(content)) != 0) {
			fprintf(stderr, "FAIL: could not read staged %s\n", resolv_path);
			ok = 0;
		} else if (!str_eq(content, "nameserver 192.168.15.101\nnameserver 192.168.15.102\n")) {
			fprintf(stderr, "FAIL: staged resolv.conf content mismatch, got: %s\n", content);
			ok = 0;
		}
	}

	/* 6. Omitted dns_servers -> empty array on GET, no resolv.conf
	 * staged at all. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"nodns\",\"image\":\"dnstest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST nodns, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jarr = json_object_get(r.json, "dns_servers");

		if (jarr == NULL || jarr->type != JSON_ARRAY || jarr->u.array.count != 0) {
			fprintf(stderr, "FAIL: POST nodns should echo empty dns_servers array\n");
			ok = 0;
		}
	}
	cix_response_free(&r);
	{
		char resolv_path[PATH_MAX];
		FILE *f;

		snprintf(resolv_path, sizeof(resolv_path), "%s/containers/nodns/upper/etc/resolv.conf",
		         g_data_dir);
		f = fopen(resolv_path, "r");
		if (f != NULL) {
			fclose(f);
			fprintf(stderr, "FAIL: nodns should never have a staged resolv.conf\n");
			ok = 0;
		}
	}

	/*
	 * 7-13: ADR-0295 (#451) -- an OMITTED dns_servers defaults to the
	 * registered DNS servers sharing a network with this container.
	 *
	 * Case 6 above is the guard for the other half: a container with NO
	 * networks still gets nothing, because there is no shared network to
	 * find a server on.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"dnsnet\",\"subnet\":\"172.43.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST dnsnet, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"dnsnet2\",\"subnet\":\"172.44.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST dnsnet2, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 7. A registered DNS server on dnsnet, with an EXPLICIT ip -- the
	 * only kind the default will use, since an auto-allocated resolver
	 * address is not stable across boots. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"srvdns\",\"image\":\"dnstest\","
	                       "\"networks\":[{\"name\":\"dnsnet\",\"ip\":\"172.43.0.5\"}],"
	                       "\"dns_server\":{\"hosts_path\":\"/etc/hosts-dns\"},"
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST srvdns, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 8. A SECOND DNS server on the same network gets no default, even
	 * though srvdns is registered and shares that network. A resolver's
	 * upstream is dns_forwarders_set() and its own --servers-file; a
	 * staged resolv.conf would be a second answer to the same question,
	 * and dns-1/dns-2 on the real box would point at each other. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"srvdns2\",\"image\":\"dnstest\","
	                       "\"networks\":[{\"name\":\"dnsnet\",\"ip\":\"172.43.0.6\"}],"
	                       "\"dns_server\":{\"hosts_path\":\"/etc/hosts-dns\"},"
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST srvdns2, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	{
		char resolv_path[PATH_MAX];
		FILE *f;

		snprintf(resolv_path, sizeof(resolv_path), "%s/containers/srvdns2/upper/etc/resolv.conf",
		         g_data_dir);
		f = fopen(resolv_path, "r");
		if (f != NULL) {
			fclose(f);
			fprintf(stderr, "FAIL: a DNS server must not be given a default resolver\n");
			ok = 0;
		}
	}

	/* 9. The case #451 is about: omitted, so defaulted. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"defclient\",\"image\":\"dnstest\","
	                       "\"networks\":[\"dnsnet\"],"
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST defclient, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jarr = json_object_get(r.json, "dns_servers");

		/*
		 * BOTH registered servers, in registration order (each fills
		 * the first free binding slot, so srvdns then srvdns2). This
		 * is the real box's shape -- dns-1 and dns-2 -- and it is also
		 * the dedupe check: a server attached to two of this
		 * container's networks must contribute one entry, not two.
		 */
		if (jarr == NULL || jarr->type != JSON_ARRAY || jarr->u.array.count != 2 ||
		    !str_eq(json_as_string(jarr->u.array.items[0]), "172.43.0.5") ||
		    !str_eq(json_as_string(jarr->u.array.items[1]), "172.43.0.6")) {
			fprintf(stderr, "FAIL: defclient should report both defaulted resolvers\n");
			ok = 0;
		}
	}
	cix_response_free(&r);
	{
		char resolv_path[PATH_MAX];
		char content[256];

		snprintf(resolv_path, sizeof(resolv_path), "%s/containers/defclient/upper/etc/resolv.conf",
		         g_data_dir);
		if (read_whole_file(resolv_path, content, sizeof(content)) != 0) {
			fprintf(stderr, "FAIL: defclient has no staged resolv.conf -- #451 is not fixed\n");
			ok = 0;
		} else if (!str_eq(content, "nameserver 172.43.0.5\nnameserver 172.43.0.6\n")) {
			fprintf(stderr, "FAIL: defclient resolv.conf content: %s\n", content);
			ok = 0;
		}
	}

	/* 10. An explicit [] still means none. It is the only way to ask
	 * for no resolver, so it has to keep working. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"emptyclient\",\"image\":\"dnstest\","
	                       "\"networks\":[\"dnsnet\"],\"dns_servers\":[],"
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST emptyclient, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	{
		char resolv_path[PATH_MAX];
		FILE *f;

		snprintf(resolv_path, sizeof(resolv_path),
		         "%s/containers/emptyclient/upper/etc/resolv.conf", g_data_dir);
		f = fopen(resolv_path, "r");
		if (f != NULL) {
			fclose(f);
			fprintf(stderr, "FAIL: dns_servers:[] must still mean no resolver\n");
			ok = 0;
		}
	}

	/* 11. An operator-written files[] entry wins, and is NOT the 400
	 * that combining it with an explicit dns_servers is (case 3). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"fileclient\",\"image\":\"dnstest\","
	                       "\"networks\":[\"dnsnet\"],"
	                       "\"files\":[{\"path\":\"/etc/resolv.conf\",\"content\":\"nameserver 9.9.9.9\\n\"}],"
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST fileclient (files[] + omitted dns_servers), status=%d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);
	{
		char resolv_path[PATH_MAX];
		char content[256];

		snprintf(resolv_path, sizeof(resolv_path), "%s/containers/fileclient/upper/etc/resolv.conf",
		         g_data_dir);
		if (read_whole_file(resolv_path, content, sizeof(content)) != 0 ||
		    !str_eq(content, "nameserver 9.9.9.9\n")) {
			fprintf(stderr, "FAIL: fileclient's own files[] resolv.conf must win, got: %s\n",
			        content);
			ok = 0;
		}
	}

	/* 12. A container on a network with no registered server on it gets
	 * nothing -- the default is per-network, not per-box. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"otherclient\",\"image\":\"dnstest\","
	                       "\"networks\":[\"dnsnet2\"],"
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST otherclient, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	{
		char resolv_path[PATH_MAX];
		FILE *f;

		snprintf(resolv_path, sizeof(resolv_path),
		         "%s/containers/otherclient/upper/etc/resolv.conf", g_data_dir);
		f = fopen(resolv_path, "r");
		if (f != NULL) {
			fclose(f);
			fprintf(stderr, "FAIL: a container sharing no network with a DNS server must "
			                "get no default\n");
			ok = 0;
		}
	}

	/*
	 * 13. THE DESIGN PROPERTY, and the reason this is not a
	 * registry lookup: the default reads the persisted DEFINITION, so a
	 * server that is not running still supplies it. Autostart order is
	 * containerdef_resolve_order()'s depends_on order and every
	 * definition on the real box declares depends_on: [] -- a
	 * registry-backed default would therefore find the server on the
	 * boots where it happened to start first and nothing on the others,
	 * turning #451's symptom intermittent instead of fixing it.
	 *
	 * Stopping srvdns keeps its binding (dns_server_forget() is called
	 * on DELETE, never on stop) and drops its registry entry, which is
	 * exactly the split this asserts.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers/srvdns/stop", NULL, &r) != 0 ||
	    (r.status != 200 && r.status != 204)) {
		fprintf(stderr, "FAIL: POST srvdns/stop, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"afterstop\",\"image\":\"dnstest\","
	                       "\"networks\":[\"dnsnet\"],"
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST afterstop, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	{
		char resolv_path[PATH_MAX];
		char content[256];

		snprintf(resolv_path, sizeof(resolv_path), "%s/containers/afterstop/upper/etc/resolv.conf",
		         g_data_dir);
		if (read_whole_file(resolv_path, content, sizeof(content)) != 0 ||
		    !str_eq(content, "nameserver 172.43.0.5\nnameserver 172.43.0.6\n")) {
			fprintf(stderr, "FAIL: the default must come from the DEFINITION, so a stopped "
			                "server still supplies it -- got: %s\n",
			        content);
			ok = 0;
		}
	}

	/* Cleanup. */
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/realdns", NULL, &r);
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/nodns", NULL, &r);
	cix_response_free(&r);
	{
		static const char *const made[] = { "srvdns", "srvdns2", "defclient", "emptyclient",
			                            "fileclient", "otherclient", "afterstop" };
		size_t ci;

		for (ci = 0; ci < sizeof(made) / sizeof(made[0]); ci++) {
			char path[128];

			snprintf(path, sizeof(path), "/v1/containers/%s", made[ci]);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", path, NULL, &r);
			cix_response_free(&r);
		}
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "CONTAINER DNS SERVERS RESULT: PASS\n" : "CONTAINER DNS SERVERS RESULT: FAIL\n");
	return ok ? 0 : 1;
}
