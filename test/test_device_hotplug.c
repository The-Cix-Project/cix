/*
 * ADR-0161 phases A/B/D end-to-end test: composite-device interface
 * metadata (Phase A), the opt-in optional:true device grant that
 * doesn't fail creation (Phase B), and the manual live attach/detach
 * primitive (Phase D) -- POST/DELETE /v1/containers/{name}/devices.
 *
 * Phase C (the NETLINK_KOBJECT_UEVENT listener) is proven only
 * indirectly here: it calls the exact same live_attach_one_device()/
 * registry_device_live_detach() primitives this test exercises
 * directly via the REST endpoints, and start_daemon() below already
 * proves the listener's own best-effort setup (start_uevent_watch())
 * doesn't crash or hang daemon startup. Actually triggering a real
 * kernel hotplug add/remove event isn't possible in this sandbox (no
 * way to physically plug/unplug hardware) -- the same class of gap
 * CLAUDE.md's own NTP clock_settime()/BPF-probe environment notes
 * already document for other kernel-privileged mechanisms.
 *
 * device_enumerate() walks the real host's sysfs tree, so this test
 * adapts to whatever hardware is actually present (same posture
 * test_daemon_devices.c already established): the composite-device
 * assertion only runs if this host actually has one; the live
 * attach/detach scenario only runs if this host has at least one
 * assignable device not already granted by the deterministic parts.
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

#define TEST_PORT 7685
#define PORT_ARG "--port=7685"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

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

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
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
	struct thinc_client client;
	int ok = 1;
	struct thinc_response r;
	char first_assignable_id[128] = "";
	int have_assignable = 0;
	int have_composite = 0;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/hotplugtest/v1/rootfs",
	         g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/hotplugtest", g_data_dir);
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
	thinc_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. Phase A: GET /v1/devices reports real composite-device
	 * interface metadata, if this host has one. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/devices", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/devices, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *devices = json_object_get(r.json, "devices");
		size_t i;

		if (devices == NULL || devices->type != JSON_ARRAY) {
			fprintf(stderr, "FAIL: GET /v1/devices: no devices array\n");
			ok = 0;
		} else {
			for (i = 0; i < devices->u.array.count; i++) {
				const struct json_value *d = devices->u.array.items[i];
				const char *bus = json_str_field(d, "bus");
				const struct json_value *ifaces = json_object_get(d, "interfaces");

				if (ifaces == NULL || ifaces->type != JSON_ARRAY) {
					fprintf(stderr, "FAIL: device %zu has no interfaces array at all\n", i);
					ok = 0;
					continue;
				}
				if (str_eq(bus, "usb") && ifaces->u.array.count > 1) {
					const struct json_value *first = ifaces->u.array.items[0];

					have_composite = 1;
					if (json_object_get(first, "class") == NULL ||
					    json_object_get(first, "subclass") == NULL ||
					    json_object_get(first, "protocol") == NULL ||
					    json_object_get(first, "number") == NULL) {
						fprintf(stderr,
						        "FAIL: composite device %zu interface entry missing a "
						        "field\n",
						        i);
						ok = 0;
					}
				}
				if (str_eq(bus, "usb") && json_as_string(json_object_get(d, "id")) != NULL &&
				    json_object_get(d, "assignable") != NULL &&
				    json_object_get(d, "assignable")->type == JSON_BOOL &&
				    json_object_get(d, "assignable")->u.boolean != 0 &&
				    first_assignable_id[0] == '\0') {
					snprintf(first_assignable_id, sizeof(first_assignable_id), "%s",
					         json_as_string(json_object_get(d, "id")));
					have_assignable = 1;
				}
			}
		}
	}
	thinc_response_free(&r);
	if (!have_composite)
		printf("(no real composite USB device discovered on this host -- Phase A interface "
		       "assertion skipped)\n");

	/* 2. Phase B: an "optional": true device that doesn't currently
	 * resolve does NOT fail container creation -- fully deterministic
	 * regardless of real hardware (a made-up id can never resolve). */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"devoptional\",\"image\":\"hotplugtest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"],"
	                       "\"devices\":[{\"id\":\"usb:0000:0000:nonexistent\",\"optional\":true}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST devoptional with an unresolvable optional device: expected "
		                "201, got %d\n",
		        r.status);
		ok = 0;
	} else {
		const struct json_value *pending = json_object_get(r.json, "pending_devices");
		int found = 0;
		size_t i;

		if (pending == NULL || pending->type != JSON_ARRAY) {
			fprintf(stderr, "FAIL: POST devoptional: no pending_devices array in response\n");
			ok = 0;
		} else {
			for (i = 0; i < pending->u.array.count; i++) {
				if (str_eq(json_as_string(pending->u.array.items[i]),
				           "usb:0000:0000:nonexistent"))
					found = 1;
			}
			if (!found) {
				fprintf(stderr, "FAIL: POST devoptional: id not echoed in pending_devices\n");
				ok = 0;
			}
		}
	}
	thinc_response_free(&r);

	/* Still deterministic: an unresolvable NON-optional device still
	 * fails creation exactly as before (ADR-0161 Phase B is additive,
	 * not a behavior change for the pre-existing bare-string form). */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"devrequired\",\"image\":\"hotplugtest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"],"
	                       "\"devices\":[\"usb:0000:0000:nonexistent\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr,
		        "FAIL: POST devrequired with an unresolvable non-optional device: expected "
		        "400, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"devbadoptional\",\"image\":\"hotplugtest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"],"
	                       "\"devices\":[{\"id\":\"usb:0000:0000:nonexistent\",\"optional\":"
	                       "\"not-a-bool\"}]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST with non-boolean optional: expected 400, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 3. Phase D: manual live attach/detach against a real, currently-
	 * assignable device this host actually has -- skipped, not
	 * failed, if none. */
	if (have_assignable) {
		memset(&r, 0, sizeof(r));
		if (thinc_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"devlive\",\"image\":\"hotplugtest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST devlive (no devices at creation): expected 201, got %d\n",
			        r.status);
			ok = 0;
		}
		thinc_response_free(&r);

		if (ok) {
			char body[192];

			snprintf(body, sizeof(body), "{\"id\":\"%s\"}", first_assignable_id);
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "POST", "/v1/containers/devlive/devices", body, &r) !=
			        0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: POST devlive/devices (live attach): expected 200, got "
				                "%d\n",
				        r.status);
				ok = 0;
			} else {
				const struct json_value *devices = json_object_get(r.json, "devices");
				int found_live = 0;
				size_t i;

				for (i = 0; devices != NULL && i < devices->u.array.count; i++) {
					const struct json_value *d = devices->u.array.items[i];

					if (str_eq(json_str_field(d, "id"), first_assignable_id) &&
					    json_object_get(d, "live") != NULL &&
					    json_object_get(d, "live")->type == JSON_BOOL &&
					    json_object_get(d, "live")->u.boolean != 0)
						found_live = 1;
				}
				if (!found_live) {
					fprintf(stderr,
					        "FAIL: live-attached device not reported with live=true\n");
					ok = 0;
				}
			}
			thinc_response_free(&r);
		}

		/* A duplicate live-attach of the exact same id is rejected. */
		if (ok) {
			char body[192];

			snprintf(body, sizeof(body), "{\"id\":\"%s\"}", first_assignable_id);
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "POST", "/v1/containers/devlive/devices", body, &r) !=
			        0 ||
			    r.status != 500) {
				/* registry_device_live_attach() returns -1/EEXIST for a
				 * dup -- surfaced as 500 by the generic "attach failed"
				 * handler path, not a dedicated 409, since a dup here
				 * can only happen via a deliberately-adversarial second
				 * call, not a realistic operator mistake worth its own
				 * status code. */
				fprintf(stderr, "FAIL: duplicate live attach: expected 500, got %d\n", r.status);
				ok = 0;
			}
			thinc_response_free(&r);
		}

		if (ok) {
			char path[192];

			snprintf(path, sizeof(path), "/v1/containers/devlive/devices/%s", first_assignable_id);
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 200) {
				fprintf(stderr, "FAIL: DELETE devlive/devices/%s (live detach): expected 200, "
				                "got %d\n",
				        first_assignable_id, r.status);
				ok = 0;
			}
			thinc_response_free(&r);

			/* Second delete: no longer attached -> 404. */
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 404) {
				fprintf(stderr, "FAIL: second DELETE devlive/devices/%s: expected 404, got %d\n",
				        first_assignable_id, r.status);
				ok = 0;
			}
			thinc_response_free(&r);
		}

		/* A create-time grant is never live-detachable (409). */
		if (ok) {
			char body[256];

			snprintf(body, sizeof(body),
			         "{\"name\":\"devlivecreate\",\"image\":\"hotplugtest\","
			         "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],\"devices\":[\"%s\"]}",
			         first_assignable_id);
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "POST", "/v1/containers", body, &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: POST devlivecreate: expected 201, got %d\n", r.status);
				ok = 0;
			}
			thinc_response_free(&r);

			if (ok) {
				char path[192];

				snprintf(path, sizeof(path), "/v1/containers/devlivecreate/devices/%s",
				         first_assignable_id);
				memset(&r, 0, sizeof(r));
				if (thinc_client_request(&client, "DELETE", path, NULL, &r) != 0 ||
				    r.status != 409) {
					fprintf(stderr,
					        "FAIL: DELETE a create-time device grant: expected 409, got %d\n",
					        r.status);
					ok = 0;
				}
				thinc_response_free(&r);
			}

			memset(&r, 0, sizeof(r));
			thinc_client_request(&client, "DELETE", "/v1/containers/devlivecreate", NULL, &r);
			thinc_response_free(&r);
		}

		memset(&r, 0, sizeof(r));
		thinc_client_request(&client, "DELETE", "/v1/containers/devlive", NULL, &r);
		thinc_response_free(&r);
	} else {
		printf("(no assignable device discovered on this host -- Phase D live attach/detach "
		       "scenario skipped)\n");
	}

	/* 4. Live attach/detach against a container that doesn't exist,
	 * or isn't running -- fully deterministic. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers/nosuchcontainer/devices",
	                       "{\"id\":\"usb:0000:0000:x\"}", &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: POST devices on nonexistent container: expected 404, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "DELETE", "/v1/containers/nosuchcontainer/devices/usb:x", NULL,
	                       &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: DELETE devices on nonexistent container: expected 404, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	if (ok)
		printf("DEVICE HOTPLUG RESULT: PASS\n");
	else
		printf("DEVICE HOTPLUG RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
