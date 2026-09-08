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
 *
 * Also covers Phase 15 (ADR-0048): persistent device name mappings
 * (daemon/src/devicemap.c) -- exact and vendor_model resolution, a
 * container referencing a mapping name instead of a raw id, and the
 * "mapping exists but currently resolves to nothing" case correctly
 * failing rather than silently retrying as a literal raw id. Same
 * hardware-dependent guard as the rest of this file.
 *
 * Also covers ADR-0142's raw disk passthrough: a non-OS, role-less
 * whole disk is listed under GET /v1/devices as "disk:<name>", and
 * assigning/removing a disk role (daemon/src/diskrole.c) correctly
 * removes/restores it from that list -- proving device.c's own
 * enumerate_disk() stays in sync with the storage-placement system
 * rather than offering an already-daemon-owned disk for passthrough
 * too.
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

#define TEST_PORT 7626
#define PORT_ARG "--port=7626"

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

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	struct cix_response r;
	char first_assignable_id[128];
	char first_assignable_vendor[16] = "";
	char first_assignable_product[16] = "";
	int have_assignable = 0;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/devicestest/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/devicestest", g_data_dir);
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
		return 1;
	}

	/* 1. GET /v1/devices: well-formed regardless of what hardware this
	 * host actually has -- a "devices" array field must be present,
	 * and every entry (if any) must carry the documented fields. Also
	 * remembers the first assignable id found, if any, for scenario 3. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/devices", NULL, &r) != 0 || r.status != 200) {
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
					const char *vendor_id = json_str_field(d, "vendor_id");
					const char *product_id = json_str_field(d, "product_id");

					snprintf(first_assignable_id, sizeof(first_assignable_id), "%s", id);
					if (vendor_id != NULL)
						snprintf(first_assignable_vendor, sizeof(first_assignable_vendor),
						         "%s", vendor_id);
					if (product_id != NULL)
						snprintf(first_assignable_product, sizeof(first_assignable_product),
						         "%s", product_id);
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
	cix_response_free(&r);

	/* 2. an unknown device id is always rejected, regardless of what
	 * real hardware this host has -- fully deterministic. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"devbad\",\"image\":\"devicestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"]}],"
	                       "\"devices\":[\"usb:0000:0000:nonexistent\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: unknown device id expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 2b. Phase 14 part 1 (ADR-0028): a bare "gpu:0" logical id, with no
	 * GPU discovered on this host at all, is a real, deterministic
	 * exercise of device_find_group()'s own empty-expansion fallback
	 * path (no exact match, no "gpu:0:*" prefix matches either) --
	 * fully provable here regardless of hardware, unlike the positive
	 * "grouped id expands into real grants" path (see ADR-0028's own
	 * Consequences for why that part isn't testable in this sandbox). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"devgpubad\",\"image\":\"devicestest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"]}],"
	                       "\"devices\":[\"gpu:0\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: devices:[\"gpu:0\"] with no GPU discovered expected 400, got "
		                "%d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. grant a real, currently-assignable device -- only if this
	 * host has one. The create response and a follow-up GET must both
	 * echo {id, dev_path}; deleting the container must succeed. */
	if (have_assignable) {
		char body[256];

		snprintf(body, sizeof(body),
		         "{\"name\":\"devgood\",\"image\":\"devicestest\","
		         "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],\"devices\":[\"%s\"]}",
		         first_assignable_id);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers", body, &r) != 0 ||
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
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/containers/devgood", NULL, &r) != 0 ||
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
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", "/v1/containers/devgood", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE devgood, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
	} else {
		printf("(no assignable device discovered on this host -- scenario 3 skipped)\n");
	}

	/*
	 * 3b. Phase 15 (ADR-0048): persistent device name mappings -- real
	 * proof, only if this host has an assignable device (same guard as
	 * scenario 3 above, same reasoning: this is genuinely
	 * hardware-dependent, not something to fake).
	 */
	if (have_assignable) {
		char body[256];

		/* exact mapping resolves to the same real id, "present": true */
		snprintf(body, sizeof(body), "{\"name\":\"mapexact\",\"kind\":\"exact\",\"selector\":\"%s\"}",
		         first_assignable_id);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/devicemaps", body, &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/devicemaps (exact), status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *jpresent = json_object_get(r.json, "present");

			if (jpresent == NULL || jpresent->type != JSON_BOOL || !jpresent->u.boolean) {
				fprintf(stderr, "FAIL: exact mapping should resolve present=true\n");
				ok = 0;
			}
		}
		cix_response_free(&r);

		/* a container referencing the MAPPING NAME (not the raw id)
		 * resolves to the same real device -- the actual point of this
		 * whole mechanism. */
		snprintf(body, sizeof(body),
		         "{\"name\":\"devmapgood\",\"image\":\"devicestest\","
		         "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"]}],\"devices\":[\"mapexact\"]}");
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers", body, &r) != 0 || r.status != 201) {
			fprintf(stderr, "FAIL: POST devmapgood via mapping name, status=%d\n", r.status);
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
				        "FAIL: container created via mapping name did not resolve to "
				        "the real device id\n");
				ok = 0;
			}
		}
		cix_response_free(&r);
		cix_client_request(&client, "DELETE", "/v1/containers/devmapgood", NULL, &r);
		cix_response_free(&r);

		/* vendor_model mapping, if this device's own vendor/product ids
		 * were captured (usb/pci entries always have them; a net/gpu
		 * entry might not) -- resolves to at least the same real id. */
		if (first_assignable_vendor[0] != '\0' && first_assignable_product[0] != '\0') {
			snprintf(body, sizeof(body),
			         "{\"name\":\"mapmodel\",\"kind\":\"vendor_model\",\"selector\":\"%s:%s\"}",
			         first_assignable_vendor, first_assignable_product);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/devicemaps", body, &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: POST /v1/devicemaps (vendor_model), status=%d\n",
				        r.status);
				ok = 0;
			} else {
				const struct json_value *resolved = json_object_get(r.json, "resolved_ids");
				int found = 0;
				size_t i;

				if (resolved != NULL && resolved->type == JSON_ARRAY) {
					for (i = 0; i < resolved->u.array.count; i++) {
						if (str_eq(json_as_string(resolved->u.array.items[i]),
						           first_assignable_id))
							found = 1;
					}
				}
				if (!found) {
					fprintf(stderr,
					        "FAIL: vendor_model mapping did not resolve the real "
					        "device id among resolved_ids\n");
					ok = 0;
				}
			}
			cix_response_free(&r);
			cix_client_request(&client, "DELETE", "/v1/devicemaps/mapmodel", NULL, &r);
			cix_response_free(&r);
		}

		/* GET /v1/devicemaps lists what was created (mapexact still
		 * present at this point -- mapmodel already cleaned up above). */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/devicemaps", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/devicemaps, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *maps = json_object_get(r.json, "devicemaps");
			int found = 0;
			size_t i;

			if (maps != NULL && maps->type == JSON_ARRAY) {
				for (i = 0; i < maps->u.array.count; i++) {
					if (str_eq(json_str_field(maps->u.array.items[i], "name"), "mapexact"))
						found = 1;
				}
			}
			if (!found) {
				fprintf(stderr, "FAIL: mapexact missing from GET /v1/devicemaps\n");
				ok = 0;
			}
		}
		cix_response_free(&r);

		/* a mapping that exists but currently resolves to nothing is a
		 * real 400 at container-creation time -- NOT silently retried
		 * as a literal raw device id (see main.c's own
		 * devicemap_resolve()-then-device_find_group() fallback logic). */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/devicemaps",
		                       "{\"name\":\"mapabsent\",\"kind\":\"exact\","
		                       "\"selector\":\"usb:ffff:ffff:doesnotexist\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST /v1/devicemaps (deliberately absent), status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"devmapabsent\",\"image\":\"devicestest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"]}],"
		                       "\"devices\":[\"mapabsent\"]}",
		                       &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr,
			        "FAIL: container via a mapping that resolves to nothing should be "
			        "400, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* duplicate name -> 409; delete -> 204; delete again -> 404. */
		snprintf(body, sizeof(body), "{\"name\":\"mapexact\",\"kind\":\"exact\",\"selector\":\"%s\"}",
		         first_assignable_id);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/devicemaps", body, &r) != 0 || r.status != 409) {
			fprintf(stderr, "FAIL: duplicate devicemap name expected 409, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		cix_client_request(&client, "DELETE", "/v1/devicemaps/mapabsent", NULL, &r);
		cix_response_free(&r);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", "/v1/devicemaps/mapexact", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE mapexact expected 204, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", "/v1/devicemaps/mapexact", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: DELETE mapexact (already gone) expected 404, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
	} else {
		printf("(no assignable device discovered on this host -- scenario 3b skipped)\n");
	}

	/*
	 * 4/5. Phase 12 part 7's "net" bus discovery must never list a
	 * kernel-created software interface (bridges, veths, cix's own
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
		const char *veth_a = "cix-ddtest-a";
		const char *veth_b = "cix-ddtest-b";

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
				if (cix_client_request(&client, "GET", "/v1/devices", NULL, &r) != 0 ||
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
				cix_response_free(&r);

				{
					char body[300];

					snprintf(body, sizeof(body),
					         "{\"name\":\"devveth\",\"image\":\"devicestest\","
					         "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"]}],"
					         "\"interfaces\":[\"%s\"]}",
					         veth_b);

					memset(&r, 0, sizeof(r));
					if (cix_client_request(&client, "POST", "/v1/containers", body, &r) !=
					        0 ||
					    r.status != 400) {
						fprintf(stderr,
						        "FAIL: interfaces:[virtual veth] expected 400, got "
						        "%d\n",
						        r.status);
						ok = 0;
					}
					cix_response_free(&r);
				}

				rtnl_link_delete(fd, veth_a);
			}
			rtnl_close(fd);
		}
	}

	/*
	 * 6. ADR-0142: raw disk passthrough. device_enumerate() now also
	 * exposes every non-OS, role-less whole disk as "disk:<name>" (see
	 * device.c's enumerate_disk()). Genuinely host-dependent (a disk-
	 * less sandbox is theoretically possible), so this only runs if
	 * GET /v1/storage actually reports at least one non-OS disk -- true
	 * on every real machine this project targets, and confirmed true
	 * in this dev sandbox itself.
	 */
	{
		char disk_name[64] = "";

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/storage", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/storage, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *disks = json_object_get(r.json, "disks");
			size_t i;

			if (disks != NULL && disks->type == JSON_ARRAY) {
				for (i = 0; i < disks->u.array.count && disk_name[0] == '\0'; i++) {
					const struct json_value *d = disks->u.array.items[i];
					const struct json_value *jos = json_object_get(d, "is_os_disk");

					if (jos != NULL && jos->type == JSON_BOOL && !jos->u.boolean) {
						snprintf(disk_name, sizeof(disk_name), "%s",
						         json_str_field(d, "name"));
					}
				}
			}
		}
		cix_response_free(&r);

		if (disk_name[0] == '\0') {
			printf("(no non-OS disk discovered on this host -- scenario 6 skipped)\n");
		} else {
			char disk_id[80];
			char role_body[128];
			char role_path[96];
			int listed_before = 0, listed_after_role = 1, listed_after_delete = 0;

			snprintf(disk_id, sizeof(disk_id), "disk:%s", disk_name);

			/* 6a. role-less: must be listed, bus:"disk", assignable, and
			 * carry a real dev_path/major/minor (never zeroed/blank). */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/devices", NULL, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: GET /v1/devices (disk scenario, before role), "
				                "status=%d\n",
				        r.status);
				ok = 0;
			} else {
				const struct json_value *devices = json_object_get(r.json, "devices");
				size_t i;

				if (devices != NULL && devices->type == JSON_ARRAY) {
					for (i = 0; i < devices->u.array.count; i++) {
						const struct json_value *d = devices->u.array.items[i];

						if (str_eq(json_str_field(d, "id"), disk_id)) {
							const struct json_value *jassignable =
							        json_object_get(d, "assignable");
							const struct json_value *jmajor =
							        json_object_get(d, "major");
							const char *dev_path = json_str_field(d, "dev_path");

							listed_before = 1;
							if (!str_eq(json_str_field(d, "bus"), "disk") ||
							    jassignable == NULL ||
							    jassignable->type != JSON_BOOL ||
							    !jassignable->u.boolean || dev_path == NULL ||
							    dev_path[0] == '\0' || jmajor == NULL ||
							    jmajor->type != JSON_NUMBER) {
								fprintf(stderr,
								        "FAIL: %s malformed disk device "
								        "entry\n",
								        disk_id);
								ok = 0;
							}
						}
					}
				}
			}
			cix_response_free(&r);
			if (!listed_before) {
				fprintf(stderr, "FAIL: %s not listed under GET /v1/devices before a "
				                "role was assigned\n",
				        disk_id);
				ok = 0;
			}

			/* 6b. assigning it a role removes it from the passthrough
			 * list -- already daemon-owned storage, never independently
			 * grantable to a container at the same time. */
			snprintf(role_body, sizeof(role_body), "{\"disk_name\":\"%s\",\"role\":\"backup\"}",
			         disk_name);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/storage-roles", role_body, &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: POST /v1/storage-roles %s, status=%d\n", disk_name,
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/devices", NULL, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: GET /v1/devices (disk scenario, after role), "
				                "status=%d\n",
				        r.status);
				ok = 0;
			} else {
				const struct json_value *devices = json_object_get(r.json, "devices");
				size_t i;

				listed_after_role = 0;
				if (devices != NULL && devices->type == JSON_ARRAY) {
					for (i = 0; i < devices->u.array.count; i++) {
						if (str_eq(json_str_field(devices->u.array.items[i], "id"),
						           disk_id))
							listed_after_role = 1;
					}
				}
			}
			cix_response_free(&r);
			if (listed_after_role) {
				fprintf(stderr,
				        "FAIL: %s still listed under GET /v1/devices after a role "
				        "was assigned to it\n",
				        disk_id);
				ok = 0;
			}

			/* 6c. removing the role makes it passthrough-eligible again. */
			snprintf(role_path, sizeof(role_path), "/v1/storage-roles/%s", disk_name);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "DELETE", role_path, NULL, &r) != 0 ||
			    r.status != 204) {
				fprintf(stderr, "FAIL: DELETE %s, status=%d\n", role_path, r.status);
				ok = 0;
			}
			cix_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/devices", NULL, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: GET /v1/devices (disk scenario, after role "
				                "delete), status=%d\n",
				        r.status);
				ok = 0;
			} else {
				const struct json_value *devices = json_object_get(r.json, "devices");
				size_t i;

				if (devices != NULL && devices->type == JSON_ARRAY) {
					for (i = 0; i < devices->u.array.count; i++) {
						if (str_eq(json_str_field(devices->u.array.items[i], "id"),
						           disk_id))
							listed_after_delete = 1;
					}
				}
			}
			cix_response_free(&r);
			if (!listed_after_delete) {
				fprintf(stderr,
				        "FAIL: %s not listed under GET /v1/devices again after its "
				        "role was removed\n",
				        disk_id);
				ok = 0;
			}
		}
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "DAEMON DEVICES RESULT: PASS\n" : "DAEMON DEVICES RESULT: FAIL\n");
	return ok ? 0 : 1;
}
