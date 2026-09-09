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

/*
 * #356: does GET /v1/devices report `container` as holding `id`?
 *
 * Re-reads the endpoint on every call rather than caching, because the
 * property under test is that this is derived from live container state
 * each time rather than from a snapshot taken once.
 *
 * Returns 1 for yes, 0 for no, -1 if the endpoint could not be read or
 * held_by was missing entirely -- an absent key is itself a failure,
 * since an absent key and an empty array would be two different things
 * for a client to distinguish.
 */
static int held_by_says(struct cix_client *client, const char *id, const char *container)
{
	struct cix_response r;
	const struct json_value *devices;
	size_t i;
	int found = 0;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(client, "GET", "/v1/devices", NULL, &r) != 0 || r.status != 200) {
		cix_response_free(&r);
		return -1;
	}
	devices = json_object_get(r.json, "devices");
	if (devices == NULL || devices->type != JSON_ARRAY) {
		cix_response_free(&r);
		return -1;
	}
	for (i = 0; i < devices->u.array.count; i++) {
		const struct json_value *d = devices->u.array.items[i];
		const char *did = json_str_field(d, "id");
		const struct json_value *held;
		size_t j;

		if (did == NULL || strcmp(did, id) != 0)
			continue;
		held = json_object_get(d, "held_by");
		if (held == NULL || held->type != JSON_ARRAY) {
			cix_response_free(&r);
			return -1;
		}
		if (container == NULL) {
			found = (held->u.array.count == 0);
			break;
		}
		for (j = 0; j < held->u.array.count; j++) {
			const struct json_value *n = held->u.array.items[j];

			if (n != NULL && n->type == JSON_STRING &&
			    strcmp(n->u.string, container) == 0) {
				found = 1;
				break;
			}
		}
		break;
	}
	cix_response_free(&r);
	return found;
}


int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	struct cix_response r;
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
	cix_client_init(&client, "127.0.0.1", TEST_PORT);
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
	if (cix_client_request(&client, "GET", "/v1/devices", NULL, &r) != 0 || r.status != 200) {
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
	cix_response_free(&r);
	if (!have_composite)
		printf("(no real composite USB device discovered on this host -- Phase A interface "
		       "assertion skipped)\n");

	/* 2. Phase B: an "optional": true device that doesn't currently
	 * resolve does NOT fail container creation -- fully deterministic
	 * regardless of real hardware (a made-up id can never resolve). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"devoptional\",\"image\":\"hotplugtest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"]}],"
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
	cix_response_free(&r);

	/* Still deterministic: an unresolvable NON-optional device still
	 * fails creation exactly as before (ADR-0161 Phase B is additive,
	 * not a behavior change for the pre-existing bare-string form). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"devrequired\",\"image\":\"hotplugtest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"]}],"
	                       "\"devices\":[\"usb:0000:0000:nonexistent\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr,
		        "FAIL: POST devrequired with an unresolvable non-optional device: expected "
		        "400, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"devbadoptional\",\"image\":\"hotplugtest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"]}],"
	                       "\"devices\":[{\"id\":\"usb:0000:0000:nonexistent\",\"optional\":"
	                       "\"not-a-bool\"}]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: POST with non-boolean optional: expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. Phase D: manual live attach/detach against a real, currently-
	 * assignable device this host actually has -- skipped, not
	 * failed, if none. */
	if (have_assignable) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"devlive\",\"image\":\"hotplugtest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST devlive (no devices at creation): expected 201, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		if (ok) {
			char body[192];

			/* #356: nothing holds it yet. Asserted BEFORE the attach so a
			 * held_by that is always empty could not pass the after-check
			 * by accident. */
			if (held_by_says(&client, first_assignable_id, NULL) != 1) {
				fprintf(stderr, "FAIL: held_by for %s is not empty before the attach\n",
				        first_assignable_id);
				ok = 0;
			}

			snprintf(body, sizeof(body), "{\"id\":\"%s\"}", first_assignable_id);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/containers/devlive/devices", body, &r) !=
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
			cix_response_free(&r);
		}

		/* A duplicate live-attach of the exact same id is rejected. */
		if (ok) {
			char body[192];

			snprintf(body, sizeof(body), "{\"id\":\"%s\"}", first_assignable_id);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/containers/devlive/devices", body, &r) !=
			        0 ||
			    r.status != 409) {
				/*
				 * 409, and it used to be 500. #356's exclusivity check
				 * runs before anything is attached and refuses this as
				 * a conflict naming the holder, so the dup no longer
				 * reaches registry_device_live_attach()'s EEXIST and
				 * the generic "attach failed" 500. The old comment here
				 * argued a dup was not "a realistic operator mistake
				 * worth its own status code"; the same check that stops
				 * two containers sharing a disk by accident gives it
				 * one for free.
				 */
				fprintf(stderr, "FAIL: duplicate live attach: expected 409, got %d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);
		}

		/*
		 * #356: a SECOND container cannot take a device the first one
		 * holds -- and can when both say shared. This is the actual
		 * feature; the duplicate case above only proves a container
		 * cannot take a device from itself.
		 */
		if (ok) {
			char body[192];
			int made_second = 0;

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/containers",
			                       "{\"name\":\"devrival\",\"image\":\"hotplugtest\","
			                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\","
			                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}]}",
			                       &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: POST devrival: expected 201, got %d\n", r.status);
				ok = 0;
			} else {
				made_second = 1;
			}
			cix_response_free(&r);

			if (made_second) {
				/* Exclusive by default: devlive holds it and said
				 * nothing about sharing, so this is refused however
				 * politely devrival asks. */
				snprintf(body, sizeof(body), "{\"id\":\"%s\",\"shared\":true}",
				         first_assignable_id);
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "POST", "/v1/containers/devrival/devices", body,
				                       &r) != 0 ||
				    r.status != 409) {
					fprintf(stderr, "FAIL: devrival attach of a device devlive holds "
					                "exclusively: expected 409, got %d\n",
					        r.status);
					ok = 0;
				}
				cix_response_free(&r);

				/* And it stayed devlive's -- a refused grant must not
				 * half-apply. */
				if (held_by_says(&client, first_assignable_id, "devrival") != 0) {
					fprintf(stderr, "FAIL: held_by names devrival after a REFUSED attach\n");
					ok = 0;
				}

				memset(&r, 0, sizeof(r));
				cix_client_request(&client, "DELETE", "/v1/containers/devrival", NULL, &r);
				cix_response_free(&r);
			}
		}

		if (ok) {
			char path[192];

			/* #356: the live attach is visible on GET /v1/devices. That is
			 * the whole point -- before this field a device granted to a
			 * container still reported assignable:true and said nothing at
			 * all about who had it. */
			if (held_by_says(&client, first_assignable_id, "devlive") != 1) {
				fprintf(stderr, "FAIL: held_by for %s does not name devlive after the attach\n",
				        first_assignable_id);
				ok = 0;
			}

			snprintf(path, sizeof(path), "/v1/containers/devlive/devices/%s", first_assignable_id);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 200) {
				fprintf(stderr, "FAIL: DELETE devlive/devices/%s (live detach): expected 200, "
				                "got %d\n",
				        first_assignable_id, r.status);
				ok = 0;
			}
			cix_response_free(&r);

			/* #356: and the detach is visible too -- a holder list that
			 * never empties would be as wrong as one that never fills. */
			if (held_by_says(&client, first_assignable_id, NULL) != 1) {
				fprintf(stderr, "FAIL: held_by for %s is not empty again after the detach\n",
				        first_assignable_id);
				ok = 0;
			}

			/* Second delete: no longer attached -> 404. */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "DELETE", path, NULL, &r) != 0 || r.status != 404) {
				fprintf(stderr, "FAIL: second DELETE devlive/devices/%s: expected 404, got %d\n",
				        first_assignable_id, r.status);
				ok = 0;
			}
			cix_response_free(&r);
		}

		/* A create-time grant is never live-detachable (409). */
		if (ok) {
			char body[256];

			snprintf(body, sizeof(body),
			         "{\"name\":\"devlivecreate\",\"image\":\"hotplugtest\","
			         "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],\"devices\":[\"%s\"]}",
			         first_assignable_id);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/containers", body, &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: POST devlivecreate: expected 201, got %d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);

			if (ok) {
				char path[192];

				snprintf(path, sizeof(path), "/v1/containers/devlivecreate/devices/%s",
				         first_assignable_id);
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "DELETE", path, NULL, &r) != 0 ||
				    r.status != 409) {
					fprintf(stderr,
					        "FAIL: DELETE a create-time device grant: expected 409, got %d\n",
					        r.status);
					ok = 0;
				}
				cix_response_free(&r);
			}

			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", "/v1/containers/devlivecreate", NULL, &r);
			cix_response_free(&r);
		}

		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/containers/devlive", NULL, &r);
		cix_response_free(&r);
	} else {
		printf("(no assignable device discovered on this host -- Phase D live attach/detach "
		       "scenario skipped)\n");
	}

	/* 4. Live attach/detach against a container that doesn't exist,
	 * or isn't running -- fully deterministic. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers/nosuchcontainer/devices",
	                       "{\"id\":\"usb:0000:0000:x\"}", &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: POST devices on nonexistent container: expected 404, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/containers/nosuchcontainer/devices/usb:x", NULL,
	                       &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: DELETE devices on nonexistent container: expected 404, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	if (ok)
		printf("DEVICE HOTPLUG RESULT: PASS\n");
	else
		printf("DEVICE HOTPLUG RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
