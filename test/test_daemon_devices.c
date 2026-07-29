/*
 * Phase 12 part 1 end-to-end test: proves the device-passthrough
 * resource (GET /v1/devices, the "devices" field on POST/GET
 * /v1/containers -- daemon/src/device.c + the container_dev_* wiring
 * in daemon/src/main.c) over real HTTP. The kernel-level enforcement
 * mechanism itself (the BPF_CGROUP_DEVICE program actually denying an
 * ungranted device) is already proven directly against container_
 * create() by test_devices.c; this test proves the REST/registry/CLI
 * wiring around it -- JSON parsing, id resolution, validation, and
 * response shape -- not the kernel mechanism a second time.
 *
 * device_enumerate() walks the real host's /sys/bus/usb and
 * /sys/bus/pci trees, so what's actually discoverable varies by
 * machine -- this test adapts rather than hardcoding a specific piece
 * of hardware: the always-deterministic parts (GET wiring shape,
 * unknown-id rejection) are always asserted; the "grant a real device
 * to a container" scenario only runs if this host actually has at
 * least one assignable device, and is skipped (not failed) otherwise.
 */
#include "httpclient.h"
#include "json.h"
#include "rtnetlink.h"
#include "test_image_fixture.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7626
#define PORT_ARG "--port=7626"
#define IMAGE_ROOT "/var/lib/kanxeo/images/devicestest/rootfs"

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

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[3];

	dargv[0] = "build/kanxeod";
	dargv[1] = PORT_ARG;
	dargv[2] = NULL;

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
	char first_assignable_id[128];
	int have_assignable = 0;

	if (test_image_fixture_build(IMAGE_ROOT, "build/daemon_child", "daemon_child") != 0)
		return 1;

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

	/* 1. GET /v1/devices: well-formed regardless of what hardware this
	 * host actually has -- a "devices" array field must be present,
	 * and every entry (if any) must carry the documented fields. Also
	 * remembers the first assignable id found, if any, for scenario 3. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/devices", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/devices, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *devices = json_object_get(r.json, "devices");

		if (devices == NULL || devices->type != JSON_ARRAY) {
			fprintf(stderr, "FAIL: GET /v1/devices missing a \"devices\" array\n");
			ok = 0;
		} else {
			size_t i;

			for (i = 0; i < devices->u.array.count; i++) {
				const struct json_value *d = devices->u.array.items[i];
				const struct json_value *jassignable = json_object_get(d, "assignable");
				const char *id = json_str_field(d, "id");
				const char *bus = json_str_field(d, "bus");

				if (id == NULL || bus == NULL || json_object_get(d, "dev_path") == NULL ||
				    json_object_get(d, "type") == NULL ||
				    json_object_get(d, "major") == NULL ||
				    json_object_get(d, "minor") == NULL || jassignable == NULL) {
					fprintf(stderr, "FAIL: device entry %zu missing a documented field\n",
					        i);
					ok = 0;
					continue;
				}
				if (!have_assignable && jassignable->type == JSON_BOOL &&
				    jassignable->u.boolean) {
					snprintf(first_assignable_id, sizeof(first_assignable_id), "%s", id);
					have_assignable = 1;
				}
				/* Phase 14 part 1 (ADR-0028): any "gpu" bus entry must be
				 * an individual member node -- "gpu:<idx>:<node>" -- never
				 * the bare logical "gpu:<idx>" id itself (that id only
				 * ever exists as device_find_group()'s own expansion
				 * target, not a listed discovery entry). This sandbox has
				 * no GPU driver bound to anything (confirmed directly:
				 * /sys/class/drm is empty here), so this branch is not
				 * expected to run -- checked anyway so it's correct
				 * wherever it does. */
				if (str_eq(bus, "gpu")) {
					const char *dev_path = json_str_field(d, "dev_path");
					int colons = 0;
					const char *p;

					for (p = id; *p != '\0'; p++) {
						if (*p == ':')
							colons++;
					}
					if (colons != 2 || dev_path == NULL ||
					    strncmp(dev_path, "/dev/dri/", 9) != 0) {
						fprintf(stderr,
						        "FAIL: gpu device entry %zu malformed (id=%s "
						        "dev_path=%s)\n",
						        i, id, dev_path != NULL ? dev_path : "(null)");
						ok = 0;
					}
				}
			}
		}
	}
	kx_response_free(&r);

	/* 2. an unknown device id is always rejected, regardless of what
	 * real hardware this host has -- fully deterministic. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"devbad\",\"image\":\"devicestest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"],"
	                       "\"devices\":[\"usb:0000:0000:nonexistent\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: unknown device id expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2b. Phase 14 part 1 (ADR-0028): a bare "gpu:0" logical id, with no
	 * GPU discovered on this host at all, is a real, deterministic
	 * exercise of device_find_group()'s own empty-expansion fallback
	 * path (no exact match, no "gpu:0:*" prefix matches either) --
	 * fully provable here regardless of hardware, unlike the positive
	 * "grouped id expands into real grants" path (see ADR-0028's own
	 * Consequences for why that part isn't testable in this sandbox). */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"devgpubad\",\"image\":\"devicestest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"],"
	                       "\"devices\":[\"gpu:0\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: devices:[\"gpu:0\"] with no GPU discovered expected 400, got "
		                "%d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. grant a real, currently-assignable device -- only if this
	 * host has one. The create response and a follow-up GET must both
	 * echo {id, dev_path}; deleting the container must succeed. */
	if (have_assignable) {
		char body[256];

		snprintf(body, sizeof(body),
		         "{\"name\":\"devgood\",\"image\":\"devicestest\","
		         "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],\"devices\":[\"%s\"]}",
		         first_assignable_id);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers", body, &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST devgood with a real device grant, status=%d\n",
			        r.status);
			ok = 0;
		} else {
			const struct json_value *devices = json_object_get(r.json, "devices");
			int found = 0;
			size_t i;

			if (devices != NULL && devices->type == JSON_ARRAY) {
				for (i = 0; i < devices->u.array.count; i++) {
					if (str_eq(json_str_field(devices->u.array.items[i], "id"),
					           first_assignable_id))
						found = 1;
				}
			}
			if (!found) {
				fprintf(stderr,
				        "FAIL: create response did not echo the granted device id\n");
				ok = 0;
			}
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/containers/devgood", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET devgood, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *devices = json_object_get(r.json, "devices");

			if (devices == NULL || devices->type != JSON_ARRAY || devices->u.array.count != 1) {
				fprintf(stderr, "FAIL: GET devgood devices field wrong shape\n");
				ok = 0;
			}
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/containers/devgood", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE devgood, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
	} else {
		printf("(no assignable device discovered on this host -- scenario 3 skipped)\n");
	}

	/*
	 * 4/5. Phase 12 part 7's "net" bus discovery must never list a
	 * kernel-created software interface (bridges, veths, kanxeo's own
	 * managed networks) -- only real, physically-backed hardware. A
	 * real veth pair (genuinely kernel-backed, not a mock) proves this
	 * at the REST layer, not just device.c's own unit-level
	 * /virtual/net/ exclusion: GET /v1/devices must never report it
	 * under bus:"net", and POST /v1/containers must 400 if asked to
	 * grant it as an interface anyway -- the same validation path a
	 * genuinely unknown name would hit. This dev sandbox has no real,
	 * physically-backed NIC visible in its own root netns at all
	 * (ADR-0022) -- the positive "grant a real interface, see it
	 * work" path is not provable here, unlike the device scenario
	 * above; this is the honest boundary of what's testable without
	 * real hardware, not a gap silently smoothed over.
	 */
	{
		int fd = rtnl_open();
		const char *veth_a = "kanxeo-ddtest-a";
		const char *veth_b = "kanxeo-ddtest-b";

		if (fd < 0) {
			fprintf(stderr, "FAIL: rtnl_open for veth scenario\n");
			ok = 0;
		} else {
			rtnl_link_delete(fd, veth_a); /* leftover from a prior aborted run, if any */
			if (rtnl_veth_create(fd, veth_a, veth_b) != 0) {
				fprintf(stderr, "FAIL: could not create test veth pair\n");
				ok = 0;
			} else {
				char net_id[64];

				snprintf(net_id, sizeof(net_id), "net:%s", veth_b);

				memset(&r, 0, sizeof(r));
				if (kx_client_request(&client, "GET", "/v1/devices", NULL, &r) != 0 ||
				    r.status != 200) {
					fprintf(stderr, "FAIL: GET /v1/devices (veth scenario), status=%d\n",
					        r.status);
					ok = 0;
				} else {
					const struct json_value *devices = json_object_get(r.json, "devices");
					size_t i;

					if (devices != NULL && devices->type == JSON_ARRAY) {
						for (i = 0; i < devices->u.array.count; i++) {
							if (str_eq(json_str_field(devices->u.array.items[i], "id"),
							           net_id)) {
								fprintf(stderr,
								        "FAIL: GET /v1/devices listed a virtual "
								        "veth (%s) under bus:net\n",
								        net_id);
								ok = 0;
							}
						}
					}
				}
				kx_response_free(&r);

				{
					char body[300];

					snprintf(body, sizeof(body),
					         "{\"name\":\"devveth\",\"image\":\"devicestest\","
					         "\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"],"
					         "\"interfaces\":[\"%s\"]}",
					         veth_b);

					memset(&r, 0, sizeof(r));
					if (kx_client_request(&client, "POST", "/v1/containers", body, &r) !=
					        0 ||
					    r.status != 400) {
						fprintf(stderr,
						        "FAIL: interfaces:[virtual veth] expected 400, got "
						        "%d\n",
						        r.status);
						ok = 0;
					}
					kx_response_free(&r);
				}

				rtnl_link_delete(fd, veth_a);
			}
			rtnl_close(fd);
		}
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	printf(ok ? "DAEMON DEVICES RESULT: PASS\n" : "DAEMON DEVICES RESULT: FAIL\n");
	return ok ? 0 : 1;
}
