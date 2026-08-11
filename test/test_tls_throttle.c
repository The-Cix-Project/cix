/*
 * Connection throttling (peer-IP-aware) for repeated failed HTTPS
 * handshakes -- closes a real gap found live on 192.168.15.95: a
 * sustained flood of "https handshake failed" warnings (an untrusting
 * client hammering the HTTPS listener) carried no peer IP and had no
 * way to stop the daemon spending a real accept+SSL_accept() attempt
 * on every single one. Proves, against a real daemon: the peer IP now
 * appears in the log line; GET/PUT /v1/system/tls-throttle round-trips
 * and validates its fields; enough failures from one source within the
 * configured window trip a real block (further connections from that
 * same source, on the plain HTTP listener too, are refused outright);
 * the block expires on its own after block_seconds; a clean handshake
 * resets an IP's failure count; the enabled=false toggle genuinely
 * disables enforcement; and -- the one real, non-obvious correctness
 * requirement this feature has -- loopback (127.0.0.1, kanxeoctl's own
 * default --host=) is never throttled, so a hostile source sharing a
 * box with the daemon's own local admin access can never lock it out.
 *
 * The "hostile source" here is 127.0.0.2 -- a second, distinct address
 * on the loopback interface (routes locally on Linux with zero extra
 * setup, no real second host needed) explicitly NOT the exact
 * "127.0.0.1" string connthrottle.c exempts, so it exercises the real
 * enforcement path the way an actual remote attacker's source IP
 * would. The daemon is bound to 0.0.0.0 so both addresses can reach it.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7663
#define HTTPS_PORT 8443
#define ATTACKER_IP "127.0.0.2"

static char g_data_dir[PATH_MAX];

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[5];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/kanxeod";
	dargv[1] = "--port=7663";
	dargv[2] = "--bind=0.0.0.0";
	dargv[3] = data_dir_arg;
	dargv[4] = NULL;

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

/* Opens a TCP connection to 127.0.0.1:dst_port, source-bound to
 * src_ip -- the only way to make the daemon see a peer address other
 * than 127.0.0.1 without a real second host, since 127.0.0.0/8 is
 * usable end to end on the loopback interface with no extra setup.
 * Returns a connected fd, or -1. */
static int connect_from(const char *src_ip, int dst_port)
{
	int fd;
	struct sockaddr_in src, dst;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&src, 0, sizeof(src));
	src.sin_family = AF_INET;
	src.sin_port = 0;
	if (inet_pton(AF_INET, src_ip, &src.sin_addr) != 1 ||
	    bind(fd, (struct sockaddr *)&src, sizeof(src)) != 0) {
		close(fd);
		return -1;
	}

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons((uint16_t)dst_port);
	inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
	if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Sends bytes that are not a valid TLS ClientHello (a plain HTTP
 * request line) -- enough for OpenSSL's SSL_accept() to fail fast with
 * a real protocol error, exactly like an untrusting/misbehaving
 * client's own malformed or rejected handshake does in practice. */
static void trigger_one_handshake_failure(const char *src_ip)
{
	int fd = connect_from(src_ip, HTTPS_PORT);
	char buf[64];

	if (fd < 0)
		return;
	write(fd, "GET / HTTP/1.1\r\n\r\n", 19);
	read(fd, buf, sizeof(buf)); /* drain whatever comes back, if anything */
	close(fd);
	usleep(150000); /* give the daemon's own event loop a real turn */
}

/* A real, minimal plain-HTTP GET /v1/health from src_ip, source-bound
 * the same way -- returns 1 if a genuine "HTTP/1.1 200" status line
 * comes back, 0 if the connection is refused/closed before any real
 * response arrives (a throttle block) or connect() itself fails. */
static int can_reach_health(const char *src_ip)
{
	int fd = connect_from(src_ip, TEST_PORT);
	char buf[64];
	ssize_t n;
	int ok;

	if (fd < 0)
		return 0;
	if (write(fd, "GET /v1/health HTTP/1.1\r\nHost: x\r\n\r\n", 38) < 0) {
		close(fd);
		return 0;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	ok = (strncmp(buf, "HTTP/1.1 200", 12) == 0);
	return ok;
}

static int get_int_field(const struct json_value *obj, const char *key)
{
	return (int)json_as_number(json_object_get(obj, key));
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. Default config, sane out of the box. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/system/tls-throttle", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET tls-throttle (defaults), status=%d\n", r.status);
		ok = 0;
	}
	if (ok) {
		const struct json_value *jenabled = json_object_get(r.json, "enabled");

		if (jenabled == NULL || jenabled->type != JSON_BOOL || !jenabled->u.boolean) {
			fprintf(stderr, "FAIL: expected enabled=true by default\n");
			ok = 0;
		}
		if (get_int_field(r.json, "threshold") != 20 || get_int_field(r.json, "window_seconds") != 60 ||
		    get_int_field(r.json, "block_seconds") != 300) {
			fprintf(stderr, "FAIL: unexpected default threshold/window/block values\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 2. Validation: an out-of-range field is rejected and doesn't
	 * silently apply the other, in-range fields either. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "PUT", "/v1/system/tls-throttle", "{\"threshold\":0}", &r) != 0 ||
	           r.status != 400)) {
		fprintf(stderr, "FAIL: expected 400 for threshold=0, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. A real partial update: only threshold/window/block change,
	 * matching this project's own established config-PUT convention. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "PUT", "/v1/system/tls-throttle",
	                              "{\"threshold\":3,\"window_seconds\":60,\"block_seconds\":2}", &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: PUT tls-throttle, status=%d\n", r.status);
		ok = 0;
	}
	if (ok && (get_int_field(r.json, "threshold") != 3 || get_int_field(r.json, "window_seconds") != 60 ||
	           get_int_field(r.json, "block_seconds") != 2)) {
		fprintf(stderr, "FAIL: PUT did not apply threshold/window/block correctly\n");
		ok = 0;
	}
	kx_response_free(&r);

	/* 4. Bootstrap PKI and turn HTTPS on so real handshakes can be
	 * attempted against it. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "POST", "/v1/pki/ca", "{}", &r) != 0 || r.status != 201)) {
		fprintf(stderr, "FAIL: POST /v1/pki/ca, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "PUT", "/v1/system/daemon-config", "{\"https_enabled\":true}", &r) !=
	               0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: PUT https_enabled=true, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. Three malformed handshakes (threshold=3) from the attacker
	 * source -- the third one must trip the block. */
	if (ok) {
		int i;

		for (i = 0; i < 3; i++)
			trigger_one_handshake_failure(ATTACKER_IP);
	}

	/* 6. The peer IP now appears in the log line -- the actual gap
	 * this whole feature closes. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "GET",
	                              "/v1/system/logs?source=kanxeod&regex=" ATTACKER_IP "&tail=10", NULL, &r) !=
	               0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: GET logs regex=" ATTACKER_IP ", status=%d\n", r.status);
		ok = 0;
	}
	if (ok && (r.json == NULL || r.json->type != JSON_ARRAY || r.json->u.array.count == 0)) {
		fprintf(stderr, "FAIL: expected at least one log entry mentioning the attacker's peer IP\n");
		ok = 0;
	}
	kx_response_free(&r);

	/* 7. GET tls-throttle/status shows this IP tracked, at/above
	 * threshold, and blocked. */
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "GET", "/v1/system/tls-throttle/status", NULL, &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: GET tls-throttle/status, status=%d\n", r.status);
		ok = 0;
	}
	if (ok) {
		const struct json_value *entries = json_object_get(r.json, "entries");
		int found = 0;
		size_t i;

		if (entries == NULL || entries->type != JSON_ARRAY) {
			fprintf(stderr, "FAIL: tls-throttle/status has no entries array\n");
			ok = 0;
		} else {
			for (i = 0; i < entries->u.array.count; i++) {
				const struct json_value *e = entries->u.array.items[i];
				const char *ip = json_as_string(json_object_get(e, "ip"));
				const struct json_value *jblocked = json_object_get(e, "blocked");

				if (ip != NULL && strcmp(ip, ATTACKER_IP) == 0) {
					found = 1;
					if (get_int_field(e, "fail_count") < 3) {
						fprintf(stderr, "FAIL: expected fail_count >= 3, got %d\n",
						        get_int_field(e, "fail_count"));
						ok = 0;
					}
					if (jblocked == NULL || jblocked->type != JSON_BOOL || !jblocked->u.boolean) {
						fprintf(stderr, "FAIL: expected " ATTACKER_IP " to be blocked\n");
						ok = 0;
					}
				}
			}
			if (!found) {
				fprintf(stderr, "FAIL: " ATTACKER_IP " not present in tls-throttle/status\n");
				ok = 0;
			}
		}
	}
	kx_response_free(&r);

	/* 8. The blocked attacker is refused outright on the PLAIN HTTP
	 * listener too (one throttle table, checked uniformly for both) --
	 * while loopback (127.0.0.1) stays completely unaffected, proving
	 * the daemon's own local admin access can never be locked out by a
	 * hostile source sharing the box. */
	if (ok && can_reach_health(ATTACKER_IP)) {
		fprintf(stderr, "FAIL: expected the blocked attacker's request to be refused\n");
		ok = 0;
	}
	memset(&r, 0, sizeof(r));
	if (ok && (kx_client_request(&client, "GET", "/v1/health", NULL, &r) != 0 || r.status != 200)) {
		fprintf(stderr, "FAIL: loopback must stay reachable even while another source is blocked, status=%d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 9. The block expires on its own (block_seconds=2) -- poll until
	 * the attacker source can reach it again rather than a fixed sleep. */
	if (ok) {
		int i;
		int recovered = 0;

		for (i = 0; i < 50; i++) {
			if (can_reach_health(ATTACKER_IP)) {
				recovered = 1;
				break;
			}
			usleep(100000);
		}
		if (!recovered) {
			fprintf(stderr, "FAIL: block never expired\n");
			ok = 0;
		}
	}

	/* 10. A clean request resets the failure count (connthrottle_
	 * record_success) -- one more malformed handshake (below the
	 * threshold of 3) followed by a real success must leave fail_count
	 * at 0, not 1. */
	if (ok) {
		trigger_one_handshake_failure(ATTACKER_IP);

		if (!can_reach_health(ATTACKER_IP)) {
			fprintf(stderr, "FAIL: attacker source should still be reachable (below threshold)\n");
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (ok && (kx_client_request(&client, "GET", "/v1/system/tls-throttle/status", NULL, &r) != 0 ||
		           r.status != 200)) {
			fprintf(stderr, "FAIL: GET tls-throttle/status (post-recovery), status=%d\n", r.status);
			ok = 0;
		}
		if (ok) {
			const struct json_value *entries = json_object_get(r.json, "entries");
			size_t i;

			for (i = 0; entries != NULL && i < entries->u.array.count; i++) {
				const struct json_value *e = entries->u.array.items[i];
				const char *ip = json_as_string(json_object_get(e, "ip"));

				if (ip != NULL && strcmp(ip, ATTACKER_IP) == 0 && get_int_field(e, "fail_count") != 0) {
					fprintf(stderr, "FAIL: expected fail_count reset to 0 after a clean request, got %d\n",
					        get_int_field(e, "fail_count"));
					ok = 0;
				}
			}
		}
		kx_response_free(&r);
	}

	/* 11. enabled=false genuinely disables enforcement -- flood well
	 * past the configured threshold and confirm the attacker source is
	 * still able to connect. */
	memset(&r, 0, sizeof(r));
	if (ok &&
	    (kx_client_request(&client, "PUT", "/v1/system/tls-throttle", "{\"enabled\":false}", &r) != 0 ||
	     r.status != 200)) {
		fprintf(stderr, "FAIL: PUT enabled=false, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (ok) {
		int i;

		for (i = 0; i < 6; i++)
			trigger_one_handshake_failure(ATTACKER_IP);

		if (!can_reach_health(ATTACKER_IP)) {
			fprintf(stderr, "FAIL: expected the attacker source to still be reachable with throttling disabled\n");
			ok = 0;
		}
	}

	if (ok)
		printf("TLS THROTTLE RESULT: PASS\n");
	else
		printf("TLS THROTTLE RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
