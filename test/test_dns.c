/*
 * Phase 8 part 1 end-to-end test: proves DNS records
 * (POST/GET/DELETE /v1/dns/records) and DNS server bindings
 * (POST/GET/DELETE /v1/dns/servers) work over real HTTP -- a real
 * dnsmasq container actually answers DNS queries from records managed
 * through this API, verified with the host's own `dig` (real protocol
 * resolution, not a config-file check), including a live update via
 * the SIGHUP-reload mechanism, not just a one-time snapshot at
 * registration.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7626
#define PORT_ARG "--port=7626"
#define DNSMASQ_IMAGE_ROOT "/var/lib/kanxeo/images/dnstest/rootfs"
#define TEST_NETWORK_NAME "dnstestnet"
#define TEST_NETWORK_SUBNET "172.35.0.0"

/*
 * dnsmasq (a real, unmodified Debian package binary -- not hand-rolled,
 * see docs/ROADMAP.md Phase 8) needs far more shared libraries than
 * the minimal ld.so+libc pair every other exec target in this project
 * needs. This is the full dependency closure from `ldd
 * /usr/sbin/dnsmasq` (libc.so.6 itself is already staged by
 * test_image_fixture_build()).
 */
static const char *const DNSMASQ_LIBS[] = {
	"/lib/x86_64-linux-gnu/libcap.so.2",
	"/lib/x86_64-linux-gnu/libdbus-1.so.3",
	"/lib/x86_64-linux-gnu/libgcrypt.so.20",
	"/lib/x86_64-linux-gnu/libgmp.so.10",
	"/lib/x86_64-linux-gnu/libgpg-error.so.0",
	"/lib/x86_64-linux-gnu/libhogweed.so.6",
	"/lib/x86_64-linux-gnu/libidn2.so.0",
	"/lib/x86_64-linux-gnu/libjansson.so.4",
	"/lib/x86_64-linux-gnu/liblz4.so.1",
	"/lib/x86_64-linux-gnu/liblzma.so.5",
	"/lib/x86_64-linux-gnu/libmnl.so.0",
	"/lib/x86_64-linux-gnu/libnetfilter_conntrack.so.3",
	"/lib/x86_64-linux-gnu/libnettle.so.8",
	"/lib/x86_64-linux-gnu/libnfnetlink.so.0",
	"/lib/x86_64-linux-gnu/libnftables.so.1",
	"/lib/x86_64-linux-gnu/libnftnl.so.11",
	"/lib/x86_64-linux-gnu/libsystemd.so.0",
	"/lib/x86_64-linux-gnu/libunistring.so.2",
	"/lib/x86_64-linux-gnu/libxtables.so.12",
	"/lib/x86_64-linux-gnu/libzstd.so.1",
};
#define DNSMASQ_LIBS_COUNT (sizeof(DNSMASQ_LIBS) / sizeof(DNSMASQ_LIBS[0]))

/*
 * dnsmasq opens /dev/urandom at startup to seed its RNG (and typically
 * expects /dev/null too) -- device nodes, not regular files, so they
 * must be created with mknod() at the real major:minor the kernel
 * driver expects. This project's containers don't mount a devtmpfs
 * (no such feature exists yet), but a plain character-special file
 * still works: device access isn't gated by the mount namespace, only
 * by the node's major:minor and (absent here) any device cgroup
 * restriction, which this project's cgroup setup never applies.
 */
static int ensure_dev_node(const char *image_root, const char *rel_path, dev_t dev)
{
	char path[512];
	char dir[512];
	char *slash;

	if (snprintf(path, sizeof(path), "%s%s", image_root, rel_path) >= (int)sizeof(path))
		return -1;
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash != NULL) {
		*slash = '\0';
		mkdir(dir, 0755);
	}

	if (mknod(path, S_IFCHR | 0666, dev) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

/*
 * dnsmasq's -u/-g resolve the given name via NSS (getpwnam/getgrnam)
 * even when it's already running as that user -- so "-u root -g
 * root" still needs a minimal /etc/passwd + /etc/group with a root
 * entry, which this image otherwise has no reason to carry.
 */
static int write_minimal_passwd_group(const char *image_root)
{
	char path[512];
	FILE *f;

	if (snprintf(path, sizeof(path), "%s/etc", image_root) >= (int)sizeof(path))
		return -1;
	mkdir(path, 0755);

	snprintf(path, sizeof(path), "%s/etc/passwd", image_root);
	f = fopen(path, "w");
	if (f == NULL) {
		perror(path);
		return -1;
	}
	fprintf(f, "root:x:0:0:root:/root:/bin/sh\n");
	fclose(f);

	snprintf(path, sizeof(path), "%s/etc/group", image_root);
	f = fopen(path, "w");
	if (f == NULL) {
		perror(path);
		return -1;
	}
	fprintf(f, "root:x:0:\n");
	fclose(f);

	return 0;
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

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/* Finds {"name": network_name, "ip": ...} inside a Container response's
 * "networks" array -- same helper as test_daemon_net.c. */
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
 * Runs `dig @server_ip qname +short` and captures its stdout. Real
 * protocol-level DNS resolution, not a check of dnsmasq's config or
 * hosts file -- the actual payoff of this whole test. dig's own
 * +time/+tries handle the "container just execve'd, dnsmasq hasn't
 * reached listen() yet" startup race, the same category of async-wait
 * every other connectivity check in this project already retries for.
 */
static int run_dig(const char *server_ip, const char *qname, char *out, size_t out_size)
{
	int pipefd[2];
	pid_t pid;
	ssize_t n;
	size_t total = 0;
	int status;
	char at_server[80];

	snprintf(at_server, sizeof(at_server), "@%s", server_ip);

	if (pipe(pipefd) != 0) {
		perror("pipe");
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		perror("fork");
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		char *argv[] = { "dig",   at_server, (char *)qname, "+short",
			          "+time=2", "+tries=5", NULL };

		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execve("/usr/bin/dig", argv, environ);
		perror("execve dig");
		_exit(127);
	}

	close(pipefd[1]);
	while (total + 1 < out_size) {
		n = read(pipefd[0], out + total, out_size - total - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}
	out[total] = '\0';
	close(pipefd[0]);

	waitpid(pid, &status, 0);
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	char *dargv[3];
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	char server_ip[64] = { 0 };
	char dig_out[256];
	size_t i;

	if (test_image_fixture_build(DNSMASQ_IMAGE_ROOT, "/usr/sbin/dnsmasq", "dnsmasq") != 0) {
		fprintf(stderr, "FAIL: could not stage dnsmasq -- is it installed? (apt-get install "
		                "dnsmasq)\n");
		return 1;
	}
	for (i = 0; i < DNSMASQ_LIBS_COUNT; i++) {
		if (test_image_fixture_add_lib(DNSMASQ_IMAGE_ROOT, DNSMASQ_LIBS[i]) != 0) {
			fprintf(stderr, "FAIL: could not stage %s\n", DNSMASQ_LIBS[i]);
			return 1;
		}
	}
	if (ensure_dev_node(DNSMASQ_IMAGE_ROOT, "/dev/urandom", makedev(1, 9)) != 0 ||
	    ensure_dev_node(DNSMASQ_IMAGE_ROOT, "/dev/null", makedev(1, 3)) != 0) {
		fprintf(stderr, "FAIL: could not create /dev nodes in dnsmasq image\n");
		return 1;
	}
	if (write_minimal_passwd_group(DNSMASQ_IMAGE_ROOT) != 0) {
		fprintf(stderr, "FAIL: could not write /etc/passwd,group in dnsmasq image\n");
		return 1;
	}

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

	/* 1. records CRUD + hostname/ip validation */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/dns/records",
	                       "{\"name\":\"svc.test\",\"ip\":\"10.9.9.9\"}", &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST svc.test, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/dns/records/svc.test", NULL, &r) != 0 ||
	    r.status != 200 || !str_eq(json_str_field(r.json, "ip"), "10.9.9.9")) {
		fprintf(stderr, "FAIL: GET svc.test, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/dns/records",
	                       "{\"name\":\"svc.test\",\"ip\":\"10.9.9.9\"}", &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate record expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/dns/records",
	                       "{\"name\":\"bad..name\",\"ip\":\"10.9.9.9\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: invalid hostname expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/dns/records",
	                       "{\"name\":\"bad.test\",\"ip\":\"not-an-ip\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: invalid ip expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. create the network the dnsmasq container lives on. Explicit
	 * --gateway= (ADR-0037): this test's own verification methodology
	 * runs `dig` from the HOST straight at a container's IP, which
	 * needs the host to have a real route into this subnet at all --
	 * on the new gateway-less default, the bridge carries no host-
	 * owned address, so the host has no such route (correct, intended
	 * behavior for the new default, not a bug: this project's own
	 * routing model no longer assumes the host is ever a participant
	 * on a network unless explicitly asked to be, ADR-0037). */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"" TEST_NETWORK_NAME "\",\"subnet\":\"" TEST_NETWORK_SUBNET
	                       "\",\"prefix_len\":24,\"gateway\":\"172.35.0.1\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/networks, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2b. dns_register requires networks -- 400 without it (Phase 8 part 2) */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"badreg\",\"image\":\"dnstest\",\"cmd\":[\"/bin/dnsmasq\"],"
	                       "\"dns_register\":true}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: dns_register without networks expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. create the real dnsmasq container */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"dnsserver\",\"image\":\"dnstest\","
	                       "\"cmd\":[\"/bin/dnsmasq\",\"-k\",\"-u\",\"root\",\"-g\",\"root\","
	                       "\"-p\",\"53\",\"-H\",\"/etc/dnsmasq-hosts\",\"-R\",\"-h\","
	                       "\"-x\",\"/etc/dnsmasq.pid\"],"
	                       "\"memory_max\":67108864,\"pids_max\":32,"
	                       "\"networks\":[\"" TEST_NETWORK_NAME "\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST dnsserver, status=%d\n", r.status);
		ok = 0;
	} else {
		const char *ip = network_ip_in_response(r.json, TEST_NETWORK_NAME);

		if (ip == NULL) {
			fprintf(stderr, "FAIL: dnsserver has no ip\n");
			ok = 0;
		} else {
			snprintf(server_ip, sizeof(server_ip), "%s", ip);
		}
	}
	kx_response_free(&r);

	/* 4. register it as a DNS server; 404/400/409 validation first */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/dns/servers",
	                       "{\"container\":\"no-such-container\",\"hosts_path\":\"/etc/x\"}",
	                       &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: register nonexistent container expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/dns/servers",
	                       "{\"container\":\"dnsserver\",\"hosts_path\":\"etc/x\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: non-absolute hosts_path expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/dns/servers",
	                       "{\"container\":\"dnsserver\",\"hosts_path\":\"/etc/dnsmasq-hosts\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: register dnsserver, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/dns/servers",
	                       "{\"container\":\"dnsserver\",\"hosts_path\":\"/etc/dnsmasq-hosts\"}",
	                       &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate registration expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. the actual payoff: real DNS resolution via the host's own dig */
	if (server_ip[0] != '\0') {
		if (run_dig(server_ip, "svc.test", dig_out, sizeof(dig_out)) != 0 ||
		    strstr(dig_out, "10.9.9.9") == NULL) {
			fprintf(stderr, "FAIL: dig svc.test @%s did not resolve to 10.9.9.9, got: %s\n",
			        server_ip, dig_out);
			ok = 0;
		}

		/* 6. live update: a new record while dnsmasq keeps running,
		 * proving the SIGHUP-reload path, not just the initial
		 * bake-in at registration time */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/dns/records",
		                       "{\"name\":\"svc2.test\",\"ip\":\"10.9.9.10\"}", &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST svc2.test, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (run_dig(server_ip, "svc2.test", dig_out, sizeof(dig_out)) != 0 ||
		    strstr(dig_out, "10.9.9.10") == NULL) {
			fprintf(stderr,
			        "FAIL: dig svc2.test @%s did not resolve to 10.9.9.10 after live "
			        "update, got: %s\n",
			        server_ip, dig_out);
			ok = 0;
		}

		/*
		 * 6b. Phase 8 part 2: automatic container DNS registration.
		 * A container created with dns_register:true gets its own
		 * record (owner == its own name) with no separate POST
		 * /v1/dns/records call, picked up by the already-running,
		 * already-registered dnsmasq container the same way a
		 * manual record is (dns_record_create() already calls
		 * dns_server_sync_all() internally).
		 */
		{
			char webapp_ip[64] = { 0 };

			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/containers",
			                       "{\"name\":\"webapp\",\"image\":\"dnstest\","
			                       "\"cmd\":[\"/bin/dnsmasq\",\"-k\",\"-u\",\"root\",\"-g\",\"root\","
			                       "\"-p\",\"53\",\"-H\",\"/etc/dnsmasq-hosts\",\"-R\",\"-h\","
			                       "\"-x\",\"/etc/dnsmasq.pid\"],"
			                       "\"networks\":[\"" TEST_NETWORK_NAME "\"],"
			                       "\"dns_register\":true}",
			                       &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: POST webapp (dns_register), status=%d\n", r.status);
				ok = 0;
			} else {
				const char *ip = network_ip_in_response(r.json, TEST_NETWORK_NAME);

				if (ip == NULL) {
					fprintf(stderr, "FAIL: webapp has no ip\n");
					ok = 0;
				} else {
					snprintf(webapp_ip, sizeof(webapp_ip), "%s", ip);
				}
			}
			kx_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "GET", "/v1/dns/records/webapp", NULL, &r) != 0 ||
			    r.status != 200 || !str_eq(json_str_field(r.json, "ip"), webapp_ip) ||
			    !str_eq(json_str_field(r.json, "owner"), "webapp")) {
				fprintf(stderr,
				        "FAIL: GET webapp record, status=%d, ip=%s (want %s), owner=%s "
				        "(want webapp)\n",
				        r.status, json_str_field(r.json, "ip") ? json_str_field(r.json, "ip") : "?",
				        webapp_ip,
				        json_str_field(r.json, "owner") ? json_str_field(r.json, "owner") : "(null)");
				ok = 0;
			}
			kx_response_free(&r);

			if (webapp_ip[0] != '\0') {
				if (run_dig(server_ip, "webapp", dig_out, sizeof(dig_out)) != 0 ||
				    strstr(dig_out, webapp_ip) == NULL) {
					fprintf(stderr,
					        "FAIL: dig webapp @%s did not resolve to %s, got: %s\n",
					        server_ip, webapp_ip, dig_out);
					ok = 0;
				}
			}

			/*
			 * 6c. ownership-scoped cleanup: a manually-created
			 * record sharing a container's exact name must
			 * survive that container's deletion -- proves
			 * dns_record_forget_owner() checks owner_container,
			 * not just the name. Also exercises the
			 * "registration best-effort skipped on collision"
			 * path: creating container "shadow" with
			 * dns_register:true must still succeed (201) even
			 * though a record named "shadow" already exists.
			 */
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/dns/records",
			                       "{\"name\":\"shadow\",\"ip\":\"10.9.9.20\"}", &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: POST shadow (manual), status=%d\n", r.status);
				ok = 0;
			}
			kx_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/containers",
			                       "{\"name\":\"shadow\",\"image\":\"dnstest\","
			                       "\"cmd\":[\"/bin/dnsmasq\",\"-k\",\"-u\",\"root\",\"-g\",\"root\","
			                       "\"-p\",\"53\",\"-H\",\"/etc/dnsmasq-hosts\",\"-R\",\"-h\","
			                       "\"-x\",\"/etc/dnsmasq.pid\"],"
			                       "\"networks\":[\"" TEST_NETWORK_NAME "\"],"
			                       "\"dns_register\":true}",
			                       &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr,
				        "FAIL: POST shadow container (colliding dns_register), status=%d\n",
				        r.status);
				ok = 0;
			}
			kx_response_free(&r);

			kx_client_request(&client, "DELETE", "/v1/containers/shadow", NULL, &r);
			kx_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "GET", "/v1/dns/records/shadow", NULL, &r) != 0 ||
			    r.status != 200 || !str_eq(json_str_field(r.json, "ip"), "10.9.9.20")) {
				fprintf(stderr,
				        "FAIL: manually-created 'shadow' record did not survive "
				        "same-named container's deletion, status=%d\n",
				        r.status);
				ok = 0;
			}
			kx_response_free(&r);

			kx_client_request(&client, "DELETE", "/v1/containers/webapp", NULL, &r);
			kx_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "GET", "/v1/dns/records/webapp", NULL, &r) != 0 ||
			    r.status != 404) {
				fprintf(stderr,
				        "FAIL: webapp's auto-registered record survived container "
				        "deletion, status=%d\n",
				        r.status);
				ok = 0;
			}
			kx_response_free(&r);
		}
	} else {
		fprintf(stderr, "FAIL: skipping dig checks, dnsserver never got an ip\n");
		ok = 0;
	}

	/* 7. deleting the container cleans up its dns_server binding */
	kx_client_request(&client, "DELETE", "/v1/containers/dnsserver", NULL, &r);
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/dns/servers", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/dns/servers\n");
		ok = 0;
	} else {
		const struct json_value *servers = json_object_get(r.json, "servers");
		size_t j;
		int still_there = 0;

		if (servers != NULL && servers->type == JSON_ARRAY) {
			for (j = 0; j < servers->u.array.count; j++) {
				if (str_eq(json_str_field(servers->u.array.items[j], "container"), "dnsserver"))
					still_there = 1;
			}
		}
		if (still_there) {
			fprintf(stderr, "FAIL: dns server binding survived container deletion\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* cleanup */
	kx_client_request(&client, "DELETE", "/v1/dns/records/svc.test", NULL, &r);
	kx_response_free(&r);
	kx_client_request(&client, "DELETE", "/v1/dns/records/svc2.test", NULL, &r);
	kx_response_free(&r);
	kx_client_request(&client, "DELETE", "/v1/dns/records/shadow", NULL, &r);
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/" TEST_NETWORK_NAME, NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE " TEST_NETWORK_NAME " expected 204, got %d\n", r.status);
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

	printf(ok ? "DNS RESULT: PASS\n" : "DNS RESULT: FAIL\n");
	return ok ? 0 : 1;
}
