/*
 * kanxeoctl: a pure REST client for the Kanxeo host API
 * (docs/api/openapi.yaml). Per the project's API-First Mandate
 * (CLAUDE.md, ADR-0005), this file holds no namespace/cgroup/mount
 * logic of its own -- every subcommand is exactly one HTTP call via
 * client/src/httpclient.c, the same library test_daemon.c uses to
 * verify the daemon.
 */
#include "console.h"
#include "httpclient.h"
#include "json.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 7620

static void print_usage(FILE *out)
{
	fprintf(out,
	        "usage: kanxeoctl [--host=ADDR] [--port=N] [--json] <command> [args]\n"
	        "\n"
	        "commands:\n"
	        "  health\n"
	        "  shutdown  -- stop kanxeod; powers off the host too when it's running as\n"
	        "               real PID 1 (an installed system) -- a dev/interactive kanxeod\n"
	        "               just exits, same as it always has on SIGTERM\n"
	        "  reboot    -- stop kanxeod; restarts the host too when running as PID 1\n"
	        "  update [--image=PATH] [--kernel=PATH]  -- writes a fresh control-plane\n"
	        "               squashfs and/or a fresh kernel (already transferred onto the\n"
	        "               box, e.g. via scp) onto this daemon's own inactive A/B slot and\n"
	        "               stages a fresh loader entry for it; at least one of --image=/\n"
	        "               --kernel= required, either or both; does NOT reboot -- call\n"
	        "               reboot separately once ready to cut over\n"
	        "  backup [--output=PATH]  -- bundles platform configuration state (container\n"
	        "               defs, networks, DNS records, pkg install state + recipes) --\n"
	        "               NOT workload data, NOT image content, NEVER PKI keys. Prints the\n"
	        "               bundle (or --json) by default; --output= saves it verbatim for\n"
	        "               use with restore --input=\n"
	        "  restore --input=PATH  -- writes a previously-saved backup bundle's fields\n"
	        "               back to their real state files; does NOT reboot or hot-reload --\n"
	        "               call reboot separately for it to take effect on the next boot\n"
	        "  ps\n"
	        "  run --name=NAME --image=IMAGE [--memory-max=BYTES] [--pids-max=N] [--cpu-max=\"Q P\"]\n"
	        "      [--cpuset=0-1,3] [--disk-quota=BYTES] [--network=NAME[:IP] ...]\n"
	        "      [--ip-forward] [--dns-register] [--pki-issue] [--pki-cert-dir=PATH]\n"
	        "      [--pki-days=N] [--route=DEST/PREFIX:VIA ...] [--device=ID ...]\n"
	        "      [--interface=IFNAME ...] [--restart=always|on-failure|unless-stopped]\n"
	        "      [--restart-delay=N] [--depends-on=NAME ...]\n"
	        "      [--readiness-tcp-port=N [--readiness-timeout=N]] -- CMD [ARGS...]\n"
	        "  inspect NAME\n"
	        "  stop NAME  -- kill it now, keep its persisted definition (unlike rm)\n"
	        "  start NAME  -- bring a stopped-but-defined container back, no daemon restart needed\n"
	        "  pause NAME  -- freeze via the cgroup v2 freezer (real kernel freeze, not SIGSTOP)\n"
	        "  unpause NAME\n"
	        "  stats NAME  -- real, host-side CPU/memory/disk/network usage, gathered from\n"
	        "               cgroups + the host's own veth (no in-container agent)\n"
	        "  console NAME [--cmd=PATH]  -- interactive shell inside a running container\n"
	        "               (like `docker exec -it`), over the daemon's own WebSocket\n"
	        "               upgrade; --cmd= overrides the default /usr/bin/bash\n"
	        "  rm NAME\n"
	        "  network create --name=NAME --subnet=A.B.C.D --prefix=N [--gateway=A.B.C.D]\n"
	        "               -- no --gateway= means pure L2, no host-owned address (the\n"
	        "               default); pass it only when the host itself should route for\n"
	        "               this network\n"
	        "  network ls\n"
	        "  network rm NAME\n"
	        "  network attach-interface NAME --interface=IFNAME [--vlan=N]  -- enslaves a real\n"
	        "               host interface (see device ls's net: entries) to this network's\n"
	        "               bridge; --vlan= creates and enslaves an 802.1q sub-interface\n"
	        "               instead, leaving the parent free for other VLANs/networks\n"
	        "  network detach-interface NAME --interface=IFNAME\n"
	        "  image create --name=NAME  -- an empty image, C runtime pre-seeded, ready for\n"
	        "               pkg install --image=NAME\n"
	        "  image ls\n"
	        "  image rm NAME  -- refused for \"base\", for an image still in use, or with\n"
	        "               packages still installed into it\n"
	        "  device ls  -- lists host PCI/USB/GPU devices discoverable via sysfs, with\n"
	        "               each one's id (pass to run --device=ID) and whether it's\n"
	        "               assignable. A GPU's own bare \"gpu:N\" id (not itself listed --\n"
	        "               only its individual gpu:N:card0/gpu:N:renderD128/... member\n"
	        "               nodes are) grants everything that GPU needs in one --device=\n"
	        "               (see ADR-0028)\n"
	        "  devicemap create --name=NAME --kind=exact|vendor_model --selector=SELECTOR\n"
	        "               -- a persisted, named device binding (ADR-0048), usable in place\n"
	        "               of a raw id in run --device=; \"exact\" selector is a device.id\n"
	        "               verbatim, \"vendor_model\" selector is \"<vendor_id>:<product_id>\"\n"
	        "               and follows the device across USB ports\n"
	        "  devicemap ls  -- shows whether each mapping currently resolves to real hardware\n"
	        "  devicemap rm NAME\n"
	        "  dns record create --name=NAME --ip=A.B.C.D\n"
	        "  dns record ls\n"
	        "  dns record rm NAME\n"
	        "  dns server register --container=NAME --hosts-path=PATH\n"
	        "  dns server ls\n"
	        "  dns server unregister CONTAINER\n"
	        "  pki ca bootstrap [--common-name=NAME] [--days=N]\n"
	        "  pki ca show\n"
	        "  pki intermediate bootstrap [--common-name=NAME] [--days=N]  -- second CA tier,\n"
	        "               root must already be bootstrapped; once done, every future\n"
	        "               pki cert create is signed by it instead of the root\n"
	        "  pki intermediate show\n"
	        "  pki cert create --name=NAME [--sans=a,b,c] [--days=N]\n"
	        "  pki cert ls\n"
	        "  pki cert rm NAME\n"
	        "  pkg bootstrap [--toolchain=PATH]  -- stages a package-build toolchain into the\n"
	        "               pkgbuild image; no --toolchain= copies live from this daemon's own\n"
	        "               host (dev/test convenience, empty on a real minimal install);\n"
	        "               --toolchain=PATH imports a real, portable artifact already scp'd\n"
	        "               onto this box (build one with image/src/mktoolchainimage.c)\n"
	        "  pkg recipes\n"
	        "  pkg recipe add --name=NAME --file=PATH  -- add or update a recipe on this\n"
	        "               running system directly, no reinstall needed (ADR-0040)\n"
	        "  pkg recipe show NAME  -- print a recipe's own raw content\n"
	        "  pkg recipe rm NAME\n"
	        "  pkg install --name=NAME [--image=IMAGE] [--upgrade]\n"
	        "  pkg ls\n"
	        "  pkg rm NAME[@IMAGE]\n"
	        "  pkg update-all  -- starts an upgrade for the first installed package whose\n"
	        "               recipe version has drifted (one at a time, same v1 single-job\n"
	        "               constraint as pkg install --upgrade); call again once that job\n"
	        "               finishes to pick up the next one\n"
	        "  site show  -- this install's own instance_name/site_name/domain_suffix\n"
	        "               (ADR-0046); convenience for identification + suggesting FQDNs,\n"
	        "               never enforced\n"
	        "  site set [--instance-name=NAME] [--site-name=NAME] [--domain-suffix=NAME]\n"
	        "  daemon-config show  -- kanxeod's own listen port, HTTP/HTTPS exposure, and\n"
	        "               which network is currently its management one\n"
	        "  daemon-config set [--port=N] [--https-port=N] [--enable-http] [--disable-http]\n"
	        "               [--enable-https] [--disable-https] [--management-network=NAME]\n"
	        "               -- live, no-restart; only the fields given are changed\n"
	        "  iso build [--disk=DEV --ip=A.B.C.D --prefix=N --gateway=A.B.C.D\n"
	        "               --interface=IFNAME] [--wait]  -- assembles a fresh installer ISO\n"
	        "               server-side (ADR-0064), from the most recent \"kanxeo\"/\"kernel\"/\n"
	        "               \"isotools\" hostbuild artifacts; every flag is optional, an empty\n"
	        "               call reproduces the original edit-at-the-GRUB-menu placeholder ISO\n"
	        "  iso status  -- state/iso_path/error of the most recent ISO build\n");
}

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

/* Writes v (recursively) into w using the same escaping/structure
 * logic the daemon itself uses to build responses -- so --json mode
 * has no separate JSON-rendering implementation of its own. */
static void jw_write_value(struct json_writer *w, const struct json_value *v)
{
	size_t i;

	if (v == NULL) {
		jw_null(w);
		return;
	}
	switch (v->type) {
	case JSON_NULL:
		jw_null(w);
		break;
	case JSON_BOOL:
		jw_bool(w, v->u.boolean);
		break;
	case JSON_NUMBER:
		jw_int(w, (long long)v->u.number); /* this API never emits non-integral numbers */
		break;
	case JSON_STRING:
		jw_str(w, v->u.string);
		break;
	case JSON_ARRAY:
		jw_arr_open(w);
		for (i = 0; i < v->u.array.count; i++)
			jw_write_value(w, v->u.array.items[i]);
		jw_arr_close(w);
		break;
	case JSON_OBJECT:
		jw_obj_open(w);
		for (i = 0; i < v->u.object.count; i++) {
			jw_key(w, v->u.object.keys[i]);
			jw_write_value(w, v->u.object.values[i]);
		}
		jw_obj_close(w);
		break;
	}
}

static void print_raw_json(const struct json_value *v)
{
	struct json_writer w;

	jw_init(&w);
	jw_write_value(&w, v);
	printf("%.*s\n", (int)w.len, w.buf);
	jw_free(&w);
}

static void fmt_health(const struct json_value *v)
{
	printf("%s\n", json_str_field(v, "status"));
}

static void fmt_container_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *status = json_str_field(v, "status");
	long pid = (long)json_as_number(json_object_get(v, "pid"));
	const struct json_value *exitv = json_object_get(v, "exit_status");
	const struct json_value *networks = json_object_get(v, "networks");
	const struct json_value *ip_forward = json_object_get(v, "ip_forward");
	const char *restart = json_str_field(v, "restart");
	const struct json_value *restart_delay = json_object_get(v, "restart_delay_seconds");
	const struct json_value *stopped = json_object_get(v, "stopped");
	const struct json_value *readiness = json_object_get(v, "readiness");
	const struct json_value *files = json_object_get(v, "files");
	const struct json_value *sysctls = json_object_get(v, "sysctls");
	char exit_buf[16];
	char net_buf[256];
	char readiness_buf[32];
	char delay_buf[16];
	size_t off = 0;
	size_t i;

	if (exitv != NULL && exitv->type == JSON_NUMBER)
		snprintf(exit_buf, sizeof(exit_buf), "%ld", (long)json_as_number(exitv));
	else
		snprintf(exit_buf, sizeof(exit_buf), "-");

	net_buf[0] = '\0';
	if (networks != NULL && networks->type == JSON_ARRAY) {
		for (i = 0; i < networks->u.array.count && off < sizeof(net_buf) - 1; i++) {
			const struct json_value *item = networks->u.array.items[i];
			const char *n = json_str_field(item, "name");
			const char *ip = json_str_field(item, "ip");
			int written = snprintf(net_buf + off, sizeof(net_buf) - off, "%s%s:%s",
			                        i > 0 ? "," : "", n != NULL ? n : "?",
			                        ip != NULL ? ip : "?");

			if (written > 0)
				off += (size_t)written;
		}
	}

	if (readiness != NULL && readiness->type == JSON_OBJECT) {
		snprintf(readiness_buf, sizeof(readiness_buf), "tcp/%ld",
		         (long)json_as_number(json_object_get(readiness, "tcp_port")));
	} else {
		snprintf(readiness_buf, sizeof(readiness_buf), "-");
	}

	if (restart_delay != NULL && restart_delay->type == JSON_NUMBER)
		snprintf(delay_buf, sizeof(delay_buf), "%ld", (long)json_as_number(restart_delay));
	else
		snprintf(delay_buf, sizeof(delay_buf), "-");

	printf("%-20s %-8s pid=%-8ld exit_status=%-6s networks=%-20s fwd=%-4s restart=%-15s "
	       "delay=%-4s stopped=%-5s readiness=%-10s files=%-3zu sysctls=%zu\n",
	       name, status, pid, exit_buf, net_buf[0] != '\0' ? net_buf : "-",
	       (ip_forward != NULL && ip_forward->type == JSON_BOOL && ip_forward->u.boolean) ? "yes"
	                                                                                        : "no",
	       restart != NULL ? restart : "no", delay_buf,
	       (stopped != NULL && stopped->type == JSON_BOOL && stopped->u.boolean) ? "yes" : "no",
	       readiness_buf, files != NULL && files->type == JSON_ARRAY ? files->u.array.count : 0,
	       sysctls != NULL && sysctls->type == JSON_OBJECT ? sysctls->u.object.count : 0);
}

static void fmt_list(const struct json_value *v)
{
	const struct json_value *containers = json_object_get(v, "containers");
	size_t i;

	if (containers == NULL || containers->type != JSON_ARRAY)
		return;
	for (i = 0; i < containers->u.array.count; i++)
		fmt_container_line(containers->u.array.items[i]);
}

static void fmt_removed(const struct json_value *v)
{
	(void)v;
	printf("removed\n");
}

static void fmt_network_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *subnet = json_str_field(v, "subnet");
	long prefix_len = (long)json_as_number(json_object_get(v, "prefix_len"));
	const char *gateway = json_str_field(v, "gateway"); /* NULL when this network has none */

	printf("%-20s %s/%-3ld gateway=%s\n", name, subnet, prefix_len,
	       gateway != NULL ? gateway : "none");
}

static void fmt_network_list(const struct json_value *v)
{
	const struct json_value *networks = json_object_get(v, "networks");
	size_t i;

	if (networks == NULL || networks->type != JSON_ARRAY)
		return;
	for (i = 0; i < networks->u.array.count; i++)
		fmt_network_line(networks->u.array.items[i]);
}

static void fmt_image_line(const struct json_value *v)
{
	printf("%s\n", json_str_field(v, "name"));
}

static void fmt_image_list(const struct json_value *v)
{
	const struct json_value *images = json_object_get(v, "images");
	size_t i;

	if (images == NULL || images->type != JSON_ARRAY)
		return;
	for (i = 0; i < images->u.array.count; i++)
		fmt_image_line(images->u.array.items[i]);
}

static void fmt_device_line(const struct json_value *v)
{
	const char *id = json_str_field(v, "id");
	const char *bus = json_str_field(v, "bus");
	const char *description = json_str_field(v, "description");
	const char *driver = json_str_field(v, "driver");
	const char *dev_path = json_str_field(v, "dev_path");
	const struct json_value *jassignable = json_object_get(v, "assignable");
	int assignable = jassignable != NULL && jassignable->type == JSON_BOOL &&
	                  jassignable->u.boolean;

	printf("%-48s %-4s %-40s %-12s %-20s %s\n", id, bus, description != NULL ? description : "",
	       driver != NULL && driver[0] != '\0' ? driver : "-",
	       dev_path != NULL && dev_path[0] != '\0' ? dev_path : "-",
	       assignable ? "assignable" : "unassignable");
}

static void fmt_device_list(const struct json_value *v)
{
	const struct json_value *devices = json_object_get(v, "devices");
	size_t i;

	if (devices == NULL || devices->type != JSON_ARRAY)
		return;
	for (i = 0; i < devices->u.array.count; i++)
		fmt_device_line(devices->u.array.items[i]);
}

static void fmt_devicemap_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *kind = json_str_field(v, "kind");
	const char *selector = json_str_field(v, "selector");
	const struct json_value *jpresent = json_object_get(v, "present");
	int present = jpresent != NULL && jpresent->type == JSON_BOOL && jpresent->u.boolean;
	const struct json_value *resolved = json_object_get(v, "resolved_ids");
	char resolved_buf[256] = "-";
	size_t i;

	if (resolved != NULL && resolved->type == JSON_ARRAY && resolved->u.array.count > 0) {
		size_t off = 0;

		for (i = 0; i < resolved->u.array.count && off < sizeof(resolved_buf); i++) {
			const char *id = json_as_string(resolved->u.array.items[i]);

			off += (size_t)snprintf(resolved_buf + off, sizeof(resolved_buf) - off, "%s%s",
			                         i > 0 ? "," : "", id != NULL ? id : "?");
		}
	}

	printf("%-24s %-13s %-40s %-11s %s\n", name, kind, selector,
	       present ? "present" : "absent", resolved_buf);
}

static void fmt_devicemap_list(const struct json_value *v)
{
	const struct json_value *maps = json_object_get(v, "devicemaps");
	size_t i;

	if (maps == NULL || maps->type != JSON_ARRAY)
		return;
	for (i = 0; i < maps->u.array.count; i++)
		fmt_devicemap_line(maps->u.array.items[i]);
}

static void fmt_dns_record_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *ip = json_str_field(v, "ip");
	const char *owner = json_str_field(v, "owner");

	printf("%-30s %-16s %s\n", name, ip, owner != NULL ? owner : "-");
}

static void fmt_dns_record_list(const struct json_value *v)
{
	const struct json_value *records = json_object_get(v, "records");
	size_t i;

	if (records == NULL || records->type != JSON_ARRAY)
		return;
	for (i = 0; i < records->u.array.count; i++)
		fmt_dns_record_line(records->u.array.items[i]);
}

static void fmt_dns_server_line(const struct json_value *v)
{
	const char *container = json_str_field(v, "container");
	const char *hosts_path = json_str_field(v, "hosts_path");

	printf("%-20s %s\n", container, hosts_path);
}

static void fmt_dns_server_list(const struct json_value *v)
{
	const struct json_value *servers = json_object_get(v, "servers");
	size_t i;

	if (servers == NULL || servers->type != JSON_ARRAY)
		return;
	for (i = 0; i < servers->u.array.count; i++)
		fmt_dns_server_line(servers->u.array.items[i]);
}

static void print_sans_csv(const struct json_value *v)
{
	const struct json_value *sans = json_object_get(v, "sans");
	size_t i;

	if (sans == NULL || sans->type != JSON_ARRAY)
		return;
	for (i = 0; i < sans->u.array.count; i++) {
		if (i > 0)
			printf(",");
		printf("%s", json_as_string(sans->u.array.items[i]));
	}
}

static void fmt_pki_cert_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *serial = json_str_field(v, "serial");
	const char *not_after = json_str_field(v, "not_after");
	const char *owner = json_str_field(v, "owner");

	printf("%-24s %-42s %-30s %s\n", name, serial, not_after, owner != NULL ? owner : "-");
}

static void fmt_pki_cert_list(const struct json_value *v)
{
	const struct json_value *certs = json_object_get(v, "certs");
	size_t i;

	if (certs == NULL || certs->type != JSON_ARRAY)
		return;
	for (i = 0; i < certs->u.array.count; i++)
		fmt_pki_cert_line(certs->u.array.items[i]);
}

/* Multi-line: a PEM cert (and, for a fresh issuance, a PEM key) can't
 * sensibly fit the %-Ns single-row table format every other list uses. */
static void fmt_pki_cert_issued(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *serial = json_str_field(v, "serial");
	const char *not_after = json_str_field(v, "not_after");
	const char *cert_pem = json_str_field(v, "cert_pem");
	const char *key_pem = json_str_field(v, "key_pem");

	printf("name:      %s\n", name);
	printf("serial:    %s\n", serial);
	printf("not_after: %s\n", not_after);
	printf("sans:      ");
	print_sans_csv(v);
	printf("\n\nCertificate:\n%s\n", cert_pem != NULL ? cert_pem : "");
	printf("Private Key (shown once -- save it now):\n%s\n", key_pem != NULL ? key_pem : "");
}

static void fmt_pki_ca(const struct json_value *v)
{
	const char *subject = json_str_field(v, "subject");
	const char *serial = json_str_field(v, "serial");
	const char *not_before = json_str_field(v, "not_before");
	const char *not_after = json_str_field(v, "not_after");
	const char *cert_pem = json_str_field(v, "cert_pem");

	printf("subject:    %s\n", subject);
	printf("serial:     %s\n", serial);
	printf("not_before: %s\n", not_before);
	printf("not_after:  %s\n", not_after);
	printf("\nCertificate:\n%s\n", cert_pem != NULL ? cert_pem : "");
}

/* Reissued leaves' own cert_pem/key_pem are real (shown once, same
 * as a fresh cert create()) but omitted here -- this is a summary
 * view; kanxeoctl pki cert ls / a saved --json capture is how an
 * operator gets the full material for every reissued leaf at once. */
static void fmt_pki_reset(const struct json_value *v)
{
	const struct json_value *root = json_object_get(v, "root");
	const struct json_value *intermediate = json_object_get(v, "intermediate");
	const struct json_value *reissued = json_object_get(v, "reissued");
	const char *root_subject = root != NULL ? json_str_field(root, "subject") : NULL;
	size_t i;

	printf("root:         %s\n", root_subject != NULL ? root_subject : "?");
	if (intermediate != NULL && intermediate->type == JSON_OBJECT) {
		const char *isub = json_str_field(intermediate, "subject");

		printf("intermediate: %s\n", isub != NULL ? isub : "?");
	} else {
		printf("intermediate: (none)\n");
	}
	if (reissued != NULL && reissued->type == JSON_ARRAY) {
		printf("\nreissued %zu leaf cert(s):\n", reissued->u.array.count);
		for (i = 0; i < reissued->u.array.count; i++)
			fmt_pki_cert_line(reissued->u.array.items[i]);
	}
}

/*
 * Common success/failure handling for every subcommand: 2xx prints
 * either the raw JSON (--json, or when the subcommand has no special
 * formatting) or a formatted rendering via fmt; anything else prints
 * the API's {"error": "..."} message to stderr. Always frees r.
 * Returns the process exit code.
 */
static int emit(struct kx_response *r, int json_mode, void (*fmt)(const struct json_value *))
{
	int rc;

	if (r->status < 200 || r->status >= 300) {
		const char *msg = json_str_field(r->json, "error");

		fprintf(stderr, "kanxeoctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r->status);
		rc = 1;
	} else if (json_mode || fmt == NULL) {
		print_raw_json(r->json);
		rc = 0;
	} else {
		fmt(r->json);
		rc = 0;
	}
	kx_response_free(r);
	return rc;
}

static int cmd_health(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/health", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_shutdown(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "POST", "/v1/system/shutdown", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_reboot(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "POST", "/v1/system/reboot", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static void fmt_update(const struct json_value *v)
{
	const struct json_value *updated = json_object_get(v, "updated");
	size_t i;

	printf("%s slot=%s updated=", json_str_field(v, "status"), json_str_field(v, "slot"));
	if (updated != NULL && updated->type == JSON_ARRAY) {
		for (i = 0; i < updated->u.array.count; i++) {
			printf("%s%s", i > 0 ? "," : "", json_as_string(updated->u.array.items[i]));
		}
	}
	printf("\n");
}

static int cmd_update(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *image = NULL;
	const char *kernel = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--kernel=", 9) == 0)
			kernel = argv[i] + 9;
		else {
			fprintf(stderr, "kanxeoctl: unknown update option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (image == NULL && kernel == NULL) {
		fprintf(stderr, "usage: kanxeoctl update [--image=PATH] [--kernel=PATH]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (image != NULL) {
		jw_key(&w, "image_path");
		jw_str(&w, image);
	}
	if (kernel != NULL) {
		jw_key(&w, "kernel_path");
		jw_str(&w, kernel);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/system/update", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_update);
}

static int cmd_ps(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/containers", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_list);
}

static int cmd_inspect(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: inspect requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s", argv[0]);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: rm requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_stop(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: stop requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/stop", argv[0]);
	if (kx_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_start(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: start requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/start", argv[0]);
	if (kx_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_pause(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: pause requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/pause", argv[0]);
	if (kx_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_unpause(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: unpause requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/unpause", argv[0]);
	if (kx_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

/*
 * kanxeoctl container stats <name> -- one-shot fetch-and-print, plain
 * key/value lines. Raw counters exactly as the daemon returns them
 * (ADR-0054: no rate/percentage computed here) -- a live-refreshing
 * view belongs to the web dashboard's own Stats tab, not this CLI.
 */
static void fmt_container_stats(const struct json_value *v)
{
	const struct json_value *cpu = json_object_get(v, "cpu");
	const struct json_value *mem = json_object_get(v, "memory");
	const struct json_value *disk = json_object_get(v, "disk");
	const struct json_value *nets = json_object_get(v, "networks");
	const struct json_value *max_v;
	size_t i;

	printf("cpu.usage_usec=%lld cpu.user_usec=%lld cpu.system_usec=%lld\n",
	       (long long)json_as_number(json_object_get(cpu, "usage_usec")),
	       (long long)json_as_number(json_object_get(cpu, "user_usec")),
	       (long long)json_as_number(json_object_get(cpu, "system_usec")));

	max_v = json_object_get(mem, "max");
	if (max_v == NULL || max_v->type == JSON_NULL) {
		printf("memory.current=%lld memory.peak=%lld memory.max=unlimited\n",
		       (long long)json_as_number(json_object_get(mem, "current")),
		       (long long)json_as_number(json_object_get(mem, "peak")));
	} else {
		printf("memory.current=%lld memory.peak=%lld memory.max=%lld\n",
		       (long long)json_as_number(json_object_get(mem, "current")),
		       (long long)json_as_number(json_object_get(mem, "peak")),
		       (long long)json_as_number(max_v));
	}

	printf("disk.upper_bytes=%lld disk.read_bytes=%lld disk.write_bytes=%lld "
	       "disk.read_ios=%lld disk.write_ios=%lld\n",
	       (long long)json_as_number(json_object_get(disk, "upper_bytes")),
	       (long long)json_as_number(json_object_get(disk, "read_bytes")),
	       (long long)json_as_number(json_object_get(disk, "write_bytes")),
	       (long long)json_as_number(json_object_get(disk, "read_ios")),
	       (long long)json_as_number(json_object_get(disk, "write_ios")));

	if (nets != NULL && nets->type == JSON_ARRAY) {
		for (i = 0; i < nets->u.array.count; i++) {
			const struct json_value *n = nets->u.array.items[i];

			printf("network[%s]: rx_bytes=%lld tx_bytes=%lld rx_packets=%lld tx_packets=%lld\n",
			       json_str_field(n, "name"),
			       (long long)json_as_number(json_object_get(n, "rx_bytes")),
			       (long long)json_as_number(json_object_get(n, "tx_bytes")),
			       (long long)json_as_number(json_object_get(n, "rx_packets")),
			       (long long)json_as_number(json_object_get(n, "tx_packets")));
		}
	}
}

static int cmd_container_stats(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: stats requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/stats", argv[0]);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_stats);
}

static int cmd_console(const struct kx_client *c, int argc, char **argv)
{
	const char *name = NULL;
	const char *cmd = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--cmd=", 6) == 0)
			cmd = argv[i] + 6;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "kanxeoctl: unknown console option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: kanxeoctl console NAME [--cmd=PATH]\n");
		return 2;
	}

	return kx_console_run(c, name, cmd) == 0 ? 0 : 1;
}

/* Matches daemon's CONTAINER_MAX_NETWORKS -- see include/container.h. */
#define CLI_MAX_NETWORKS 64
#define CLI_MAX_ROUTES 8
/* Matches daemon's CONTAINER_MAX_DEVICES -- see include/container.h. */
#define CLI_MAX_DEVICES 16
/* Matches daemon's CONTAINER_MAX_INTERFACES -- see include/container.h. */
#define CLI_MAX_INTERFACES 16
/* Matches daemon's CONTAINERDEF_MAX_DEPENDS -- see daemon/include/containerdef.h. */
#define CLI_MAX_DEPENDS 16
/* Matches daemon's CONTAINER_MAX_FILES -- see include/container.h. */
#define CLI_MAX_FILES 16
/* Matches daemon's CONTAINER_MAX_SYSCTLS -- see include/container.h. */
#define CLI_MAX_SYSCTLS 32

struct cli_route {
	char dest[64];
	int prefix_len;
	char via[64];
};

struct cli_network_attach {
	char name[64];
	char ip[64];
	int has_ip;
};

/* Parses "NAME" or "NAME:IP" (e.g. "lan1:172.30.1.50") -- an explicit,
 * operator-chosen address instead of an auto-allocated one. Purely a
 * CLI presentation-syntax split, same spirit as parse_route_flag() --
 * whether ip is actually well-formed IPv4 is the daemon's job to
 * validate, not duplicated here. */
static int parse_network_flag(const char *s, struct cli_network_attach *out)
{
	const char *colon = strchr(s, ':');
	size_t name_len;

	memset(out, 0, sizeof(*out));
	if (colon == NULL) {
		if (s[0] == '\0' || strlen(s) >= sizeof(out->name))
			return -1;
		strcpy(out->name, s);
		return 0;
	}
	name_len = (size_t)(colon - s);
	if (name_len == 0 || name_len >= sizeof(out->name))
		return -1;
	memcpy(out->name, s, name_len);
	out->name[name_len] = '\0';
	if (strlen(colon + 1) == 0 || strlen(colon + 1) >= sizeof(out->ip))
		return -1;
	strcpy(out->ip, colon + 1);
	out->has_ip = 1;
	return 0;
}

/* Parses "DEST/PREFIX:VIA" (e.g. "172.34.0.0/24:172.33.0.5") into its
 * three parts. Purely a CLI presentation-syntax split -- whether
 * dest/via are actually well-formed IPv4 is the daemon's job to
 * validate, not duplicated here. */
static int parse_route_flag(const char *s, struct cli_route *out)
{
	const char *slash = strchr(s, '/');
	const char *colon;
	size_t dest_len, via_len;

	if (slash == NULL)
		return -1;
	colon = strchr(slash + 1, ':');
	if (colon == NULL)
		return -1;

	dest_len = (size_t)(slash - s);
	if (dest_len == 0 || dest_len >= sizeof(out->dest))
		return -1;
	memcpy(out->dest, s, dest_len);
	out->dest[dest_len] = '\0';

	out->prefix_len = atoi(slash + 1);

	via_len = strlen(colon + 1);
	if (via_len == 0 || via_len >= sizeof(out->via))
		return -1;
	strcpy(out->via, colon + 1);

	return 0;
}

struct cli_file_attach {
	char container_path[256]; /* matches daemon's CONTAINER_FILE_PATH_MAX */
	char local_path[PATH_MAX];
	char mode[8]; /* e.g. "0755"; empty means "let the daemon default it" */
};

/*
 * Parses "CONTAINER_PATH=LOCAL_PATH[:MODE]" -- splits on the FIRST '='
 * (container_path never contains one; local paths could in principle,
 * though not in practice on Linux) and, if the text after the LAST
 * ':' in what remains is 1-4 octal digits, treats it as an explicit
 * mode override and strips it; otherwise the whole remainder is the
 * local path (no mode given, daemon defaults to 0644). File content
 * itself is read from local_path by the caller, not here -- this
 * function only splits the flag's own text.
 */
static int parse_file_flag(const char *s, struct cli_file_attach *out)
{
	const char *eq = strchr(s, '=');
	const char *local_start;
	const char *colon;
	size_t container_len, local_len;

	memset(out, 0, sizeof(*out));
	if (eq == NULL)
		return -1;
	container_len = (size_t)(eq - s);
	if (container_len == 0 || container_len >= sizeof(out->container_path))
		return -1;
	memcpy(out->container_path, s, container_len);
	out->container_path[container_len] = '\0';

	local_start = eq + 1;
	if (local_start[0] == '\0')
		return -1;

	colon = strrchr(local_start, ':');
	local_len = strlen(local_start);
	if (colon != NULL) {
		const char *m = colon + 1;
		size_t mode_len = strlen(m);
		int looks_octal = mode_len >= 1 && mode_len <= 4;
		size_t k;

		for (k = 0; looks_octal && k < mode_len; k++) {
			if (m[k] < '0' || m[k] > '7')
				looks_octal = 0;
		}
		if (looks_octal) {
			snprintf(out->mode, sizeof(out->mode), "%s", m);
			local_len = (size_t)(colon - local_start);
		}
	}
	if (local_len == 0 || local_len >= sizeof(out->local_path))
		return -1;
	memcpy(out->local_path, local_start, local_len);
	out->local_path[local_len] = '\0';

	return 0;
}

/* Reads path's entire content into a malloc'd buffer (NUL-terminated,
 * *out_len excludes the NUL -- matches json_as_string()'s own "plain C
 * string" shape everywhere else in this file). Returns 0, or -1 (with
 * perror on the failing path) if the file can't be opened/stat'd/read. */
static int read_local_file(const char *path, char **out_buf, size_t *out_len)
{
	FILE *f;
	long size;
	char *buf;

	f = fopen(path, "rb");
	if (f == NULL) {
		perror(path);
		return -1;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		perror(path);
		fclose(f);
		return -1;
	}
	buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		return -1;
	}
	if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
		perror(path);
		free(buf);
		fclose(f);
		return -1;
	}
	buf[size] = '\0';
	fclose(f);
	*out_buf = buf;
	*out_len = (size_t)size;
	return 0;
}

/*
 * Percent-encodes a query-string value (this CLI's first one -- see
 * daemon/src/main.c's own url_query_param(), the first query-string
 * *parser* this project has ever needed either). Only "/" needs
 * encoding for the file-read endpoint's own "path" values in practice
 * (container-relative paths are always "/"-leading), but every
 * non-alphanumeric byte is encoded for real correctness rather than
 * hand-picking just the one character known to matter today.
 */
static void url_encode_query_value(const char *in, char *out, size_t out_size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t oi = 0;

	while (*in != '\0' && oi + 1 < out_size) {
		unsigned char c = (unsigned char)*in;

		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out[oi++] = (char)c;
		} else if (oi + 3 < out_size) {
			out[oi++] = '%';
			out[oi++] = hex[c >> 4];
			out[oi++] = hex[c & 0xf];
		} else {
			break;
		}
		in++;
	}
	out[oi] = '\0';
}

static int cmd_files_get(const struct kx_client *c, int argc, char **argv)
{
	const char *name = NULL;
	const char *path_arg = NULL;
	const char *output = NULL;
	char encoded_path[256 * 3]; /* 256 matches daemon's CONTAINER_FILE_PATH_MAX */
	char path[400];
	struct kx_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--path=", 7) == 0)
			path_arg = argv[i] + 7;
		else if (strncmp(argv[i], "--output=", 9) == 0)
			output = argv[i] + 9;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "kanxeoctl: unknown files get option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || path_arg == NULL) {
		fprintf(stderr, "usage: kanxeoctl files get NAME --path=/some/path [--output=PATH]\n");
		return 2;
	}

	url_encode_query_value(path_arg, encoded_path, sizeof(encoded_path));
	snprintf(path, sizeof(path), "/v1/containers/%s/files?path=%s", name, encoded_path);

	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "kanxeoctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed", r.status);
		kx_response_free(&r);
		return 1;
	}

	/* Raw bytes, not JSON -- this is the CLI's one non-JSON response
	 * body (mirrors the daemon's own single non-JSON response
	 * primitive, http_write_response(), used only for this endpoint
	 * and the web dashboard's static assets). Written byte-for-byte,
	 * same "exact round trip" discipline cmd_backup()'s own --output=
	 * already established. */
	if (output != NULL) {
		FILE *f = fopen(output, "wb");

		if (f == NULL || (r.body != NULL && fwrite(r.body, 1, r.body_len, f) != r.body_len)) {
			perror(output);
			if (f != NULL)
				fclose(f);
			kx_response_free(&r);
			return 1;
		}
		fclose(f);
	} else if (r.body != NULL) {
		fwrite(r.body, 1, r.body_len, stdout);
	}

	kx_response_free(&r);
	return 0;
}

static int cmd_files(const struct kx_client *c, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl files get NAME --path=/some/path [--output=PATH]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "get") == 0)
		return cmd_files_get(c, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown files subcommand '%s'\n", sub);
	return 2;
}

static int cmd_backup(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *output = NULL;
	int i;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--output=", 9) == 0)
			output = argv[i] + 9;
		else {
			fprintf(stderr, "kanxeoctl: unknown backup option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (kx_client_request(c, "GET", "/v1/system/backup", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "kanxeoctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		kx_response_free(&r);
		return 1;
	}

	if (output != NULL) {
		/* The raw response body IS the bundle `restore --input=` expects
		 * verbatim -- written byte-for-byte, not re-serialized through
		 * the JSON writer, so a backup/restore round trip is exact. */
		FILE *f = fopen(output, "wb");

		if (f == NULL || (r.body != NULL &&
		                   fwrite(r.body, 1, r.body_len, f) != r.body_len)) {
			perror(output);
			if (f != NULL)
				fclose(f);
			kx_response_free(&r);
			return 1;
		}
		fclose(f);
		printf("backup written to %s (%zu bytes)\n", output, r.body_len);
		kx_response_free(&r);
		return 0;
	}

	return emit(&r, json_mode, NULL);
}

static int cmd_restore(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *input = NULL;
	int i;
	char *buf;
	size_t len;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--input=", 8) == 0)
			input = argv[i] + 8;
		else {
			fprintf(stderr, "kanxeoctl: unknown restore option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (input == NULL) {
		fprintf(stderr, "usage: kanxeoctl restore --input=PATH\n");
		return 2;
	}
	if (read_local_file(input, &buf, &len) != 0) {
		fprintf(stderr, "kanxeoctl: could not read %s\n", input);
		return 1;
	}

	if (kx_client_request(c, "POST", "/v1/system/restore", buf, &r) != 0) {
		free(buf);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	free(buf);

	return emit(&r, json_mode, fmt_health);
}

static void fmt_site_config(const struct json_value *v)
{
	const char *instance_name = json_str_field(v, "instance_name");
	const char *site_name = json_str_field(v, "site_name");
	const char *domain_suffix = json_str_field(v, "domain_suffix");

	printf("instance_name=%s site_name=%s domain_suffix=%s\n",
	       instance_name != NULL ? instance_name : "?",
	       site_name != NULL && site_name[0] != '\0' ? site_name : "(none)",
	       domain_suffix != NULL ? domain_suffix : "?");
}

static int cmd_site_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/site", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_site_config);
}

/*
 * PUT /v1/system/site requires all three fields together (server-side
 * "never silently drop a field the operator didn't mean to touch"
 * rule -- see siteconfig.h). So an operator changing just one field
 * (e.g. only --domain-suffix=) must not blow away the other two: this
 * fetches the current config first and only overrides what was
 * actually passed on the command line.
 */
static int cmd_site_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	char instance_name[128];
	char site_name[128];
	char domain_suffix[128];
	const char *new_instance_name = NULL;
	const char *new_site_name = NULL;
	const char *new_domain_suffix = NULL;
	const char *cur;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--instance-name=", 16) == 0)
			new_instance_name = argv[i] + 16;
		else if (strncmp(argv[i], "--site-name=", 12) == 0)
			new_site_name = argv[i] + 12;
		else if (strncmp(argv[i], "--domain-suffix=", 16) == 0)
			new_domain_suffix = argv[i] + 16;
		else {
			fprintf(stderr, "kanxeoctl: unknown site set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (new_instance_name == NULL && new_site_name == NULL && new_domain_suffix == NULL) {
		fprintf(stderr, "usage: kanxeoctl site set [--instance-name=NAME] [--site-name=NAME] "
		                "[--domain-suffix=NAME]\n");
		return 2;
	}

	if (kx_client_request(c, "GET", "/v1/system/site", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	if (r.status < 200 || r.status >= 300) {
		fprintf(stderr, "kanxeoctl: could not read current site config (HTTP %d)\n", r.status);
		kx_response_free(&r);
		return 1;
	}
	cur = json_str_field(r.json, "instance_name");
	snprintf(instance_name, sizeof(instance_name), "%s", cur != NULL ? cur : "kanxeo");
	cur = json_str_field(r.json, "site_name");
	snprintf(site_name, sizeof(site_name), "%s", cur != NULL ? cur : "");
	cur = json_str_field(r.json, "domain_suffix");
	snprintf(domain_suffix, sizeof(domain_suffix), "%s", cur != NULL ? cur : "internal");
	kx_response_free(&r);

	if (new_instance_name != NULL)
		snprintf(instance_name, sizeof(instance_name), "%s", new_instance_name);
	if (new_site_name != NULL)
		snprintf(site_name, sizeof(site_name), "%s", new_site_name);
	if (new_domain_suffix != NULL)
		snprintf(domain_suffix, sizeof(domain_suffix), "%s", new_domain_suffix);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "instance_name");
	jw_str(&w, instance_name);
	jw_key(&w, "site_name");
	jw_str(&w, site_name);
	jw_key(&w, "domain_suffix");
	jw_str(&w, domain_suffix);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/system/site", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_site_config);
}

static int cmd_site(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl site show\n"
		                "       kanxeoctl site set [--instance-name=NAME] [--site-name=NAME] "
		                "[--domain-suffix=NAME]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_site_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_site_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown site subcommand '%s'\n", sub);
	return 2;
}

static void fmt_daemon_config(const struct json_value *v)
{
	const char *bind = json_str_field(v, "bind");
	const char *mgmt = json_str_field(v, "management_network");
	const struct json_value *jhttp = json_object_get(v, "http_enabled");
	const struct json_value *jhttps = json_object_get(v, "https_enabled");

	printf("port=%ld bind=%s management_network=%s http_enabled=%s "
	       "https_enabled=%s https_port=%ld\n",
	       (long)json_as_number(json_object_get(v, "port")), bind != NULL ? bind : "?",
	       mgmt != NULL ? mgmt : "(none)",
	       (jhttp != NULL && jhttp->type == JSON_BOOL && jhttp->u.boolean) ? "true" : "false",
	       (jhttps != NULL && jhttps->type == JSON_BOOL && jhttps->u.boolean) ? "true" : "false",
	       (long)json_as_number(json_object_get(v, "https_port")));
}

static int cmd_daemon_config_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/daemon-config", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_daemon_config);
}

/*
 * Unlike PUT /v1/system/site, the daemon-config PUT is a genuine
 * partial update server-side (handle_daemon_config_put(), daemon/src/
 * main.c) -- fields not present in the body are left exactly as they
 * are. So this sends only what the operator actually gave on the
 * command line, no fetch-then-merge dance needed.
 */
static int cmd_daemon_config_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *port = NULL;
	const char *https_port = NULL;
	const char *management_network = NULL;
	int want_http = -1;  /* -1: untouched, 0: disable, 1: enable */
	int want_https = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--port=", 7) == 0)
			port = argv[i] + 7;
		else if (strncmp(argv[i], "--https-port=", 13) == 0)
			https_port = argv[i] + 13;
		else if (strncmp(argv[i], "--management-network=", 21) == 0)
			management_network = argv[i] + 21;
		else if (strcmp(argv[i], "--enable-http") == 0)
			want_http = 1;
		else if (strcmp(argv[i], "--disable-http") == 0)
			want_http = 0;
		else if (strcmp(argv[i], "--enable-https") == 0)
			want_https = 1;
		else if (strcmp(argv[i], "--disable-https") == 0)
			want_https = 0;
		else {
			fprintf(stderr, "kanxeoctl: unknown daemon-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (port == NULL && https_port == NULL && management_network == NULL &&
	    want_http == -1 && want_https == -1) {
		fprintf(stderr,
		        "usage: kanxeoctl daemon-config set [--port=N] [--https-port=N] "
		        "[--enable-http] [--disable-http] [--enable-https] [--disable-https] "
		        "[--management-network=NAME]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (port != NULL) {
		jw_key(&w, "port");
		jw_int(&w, atol(port));
	}
	if (https_port != NULL) {
		jw_key(&w, "https_port");
		jw_int(&w, atol(https_port));
	}
	if (management_network != NULL) {
		jw_key(&w, "management_network");
		jw_str(&w, management_network);
	}
	if (want_http != -1) {
		jw_key(&w, "http_enabled");
		jw_bool(&w, want_http);
	}
	if (want_https != -1) {
		jw_key(&w, "https_enabled");
		jw_bool(&w, want_https);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/system/daemon-config", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_daemon_config);
}

static int cmd_daemon_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl daemon-config show\n"
		                "       kanxeoctl daemon-config set [--port=N] [--https-port=N] "
		                "[--enable-http] [--disable-http] [--enable-https] [--disable-https] "
		                "[--management-network=NAME]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_daemon_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_daemon_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown daemon-config subcommand '%s'\n", sub);
	return 2;
}

struct cli_sysctl {
	char key[128]; /* matches daemon's CONTAINER_SYSCTL_KEY_MAX */
	char value[64]; /* matches daemon's CONTAINER_SYSCTL_VALUE_MAX */
};

/* Parses "KEY=VALUE" (e.g. "net.ipv4.conf.all.rp_filter=0") -- whether
 * KEY is actually a safe net.* sysctl name is the daemon's job to
 * validate, not duplicated here (same "presentation-syntax split only"
 * precedent every other parse_*_flag() in this file already has). */
static int parse_sysctl_flag(const char *s, struct cli_sysctl *out)
{
	const char *eq = strchr(s, '=');
	size_t key_len, value_len;

	memset(out, 0, sizeof(*out));
	if (eq == NULL)
		return -1;
	key_len = (size_t)(eq - s);
	if (key_len == 0 || key_len >= sizeof(out->key))
		return -1;
	memcpy(out->key, s, key_len);
	out->key[key_len] = '\0';

	value_len = strlen(eq + 1);
	if (value_len == 0 || value_len >= sizeof(out->value))
		return -1;
	strcpy(out->value, eq + 1);

	return 0;
}

static int cmd_run(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *image = NULL;
	struct cli_network_attach networks[CLI_MAX_NETWORKS];
	int network_count = 0;
	const char *devices[CLI_MAX_DEVICES];
	int device_count = 0;
	const char *interfaces[CLI_MAX_INTERFACES];
	int interface_count = 0;
	const char *restart = NULL;
	long restart_delay = -1;
	const char *depends_on[CLI_MAX_DEPENDS];
	int depends_on_count = 0;
	long readiness_tcp_port = -1;
	long readiness_timeout = -1;
	int ip_forward = 0;
	int dns_register = 0;
	int pki_issue = 0;
	const char *pki_cert_dir = NULL;
	long pki_days = -1;
	struct cli_route routes[CLI_MAX_ROUTES];
	int route_count = 0;
	struct cli_file_attach files[CLI_MAX_FILES];
	int file_count = 0;
	struct cli_sysctl sysctls[CLI_MAX_SYSCTLS];
	int sysctl_count = 0;
	long memory_max = -1;
	long pids_max = -1;
	const char *cpu_max = NULL;
	const char *cpuset_cpus = NULL;
	long long disk_quota_bytes = -1;
	int i = 0;
	int cmd_start = -1;
	struct json_writer w;
	struct kx_response r;

	while (i < argc) {
		if (strcmp(argv[i], "--") == 0) {
			cmd_start = i + 1;
			break;
		}
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--memory-max=", 13) == 0)
			memory_max = atol(argv[i] + 13);
		else if (strncmp(argv[i], "--pids-max=", 11) == 0)
			pids_max = atol(argv[i] + 11);
		else if (strncmp(argv[i], "--cpu-max=", 10) == 0)
			cpu_max = argv[i] + 10;
		else if (strncmp(argv[i], "--cpuset=", 9) == 0)
			cpuset_cpus = argv[i] + 9;
		else if (strncmp(argv[i], "--disk-quota=", 13) == 0)
			disk_quota_bytes = atoll(argv[i] + 13);
		else if (strncmp(argv[i], "--network=", 10) == 0) {
			if (network_count >= CLI_MAX_NETWORKS) {
				fprintf(stderr, "kanxeoctl: too many --network= flags (max %d)\n",
				        CLI_MAX_NETWORKS);
				return 2;
			}
			if (parse_network_flag(argv[i] + 10, &networks[network_count]) != 0) {
				fprintf(stderr, "kanxeoctl: invalid --network= value '%s'\n", argv[i] + 10);
				return 2;
			}
			network_count++;
		} else if (strncmp(argv[i], "--device=", 9) == 0) {
			if (device_count >= CLI_MAX_DEVICES) {
				fprintf(stderr, "kanxeoctl: too many --device= flags (max %d)\n",
				        CLI_MAX_DEVICES);
				return 2;
			}
			devices[device_count++] = argv[i] + 9;
		} else if (strncmp(argv[i], "--interface=", 12) == 0) {
			if (interface_count >= CLI_MAX_INTERFACES) {
				fprintf(stderr, "kanxeoctl: too many --interface= flags (max %d)\n",
				        CLI_MAX_INTERFACES);
				return 2;
			}
			interfaces[interface_count++] = argv[i] + 12;
		} else if (strncmp(argv[i], "--restart=", 10) == 0) {
			restart = argv[i] + 10;
		} else if (strncmp(argv[i], "--restart-delay=", 16) == 0) {
			restart_delay = atol(argv[i] + 16);
		} else if (strncmp(argv[i], "--depends-on=", 13) == 0) {
			if (depends_on_count >= CLI_MAX_DEPENDS) {
				fprintf(stderr, "kanxeoctl: too many --depends-on= flags (max %d)\n",
				        CLI_MAX_DEPENDS);
				return 2;
			}
			depends_on[depends_on_count++] = argv[i] + 13;
		} else if (strncmp(argv[i], "--readiness-tcp-port=", 21) == 0) {
			readiness_tcp_port = atol(argv[i] + 21);
		} else if (strncmp(argv[i], "--readiness-timeout=", 20) == 0) {
			readiness_timeout = atol(argv[i] + 20);
		} else if (strcmp(argv[i], "--ip-forward") == 0) {
			ip_forward = 1;
		} else if (strcmp(argv[i], "--dns-register") == 0) {
			dns_register = 1;
		} else if (strcmp(argv[i], "--pki-issue") == 0) {
			pki_issue = 1;
		} else if (strncmp(argv[i], "--pki-cert-dir=", 15) == 0) {
			pki_cert_dir = argv[i] + 15;
		} else if (strncmp(argv[i], "--pki-days=", 11) == 0) {
			pki_days = atol(argv[i] + 11);
		} else if (strncmp(argv[i], "--route=", 8) == 0) {
			if (route_count >= CLI_MAX_ROUTES) {
				fprintf(stderr, "kanxeoctl: too many --route= flags (max %d)\n",
				        CLI_MAX_ROUTES);
				return 2;
			}
			if (parse_route_flag(argv[i] + 8, &routes[route_count]) != 0) {
				fprintf(stderr,
				        "kanxeoctl: invalid --route= value '%s' (expected DEST/PREFIX:VIA)\n",
				        argv[i] + 8);
				return 2;
			}
			route_count++;
		} else if (strncmp(argv[i], "--file=", 7) == 0) {
			if (file_count >= CLI_MAX_FILES) {
				fprintf(stderr, "kanxeoctl: too many --file= flags (max %d)\n", CLI_MAX_FILES);
				return 2;
			}
			if (parse_file_flag(argv[i] + 7, &files[file_count]) != 0) {
				fprintf(stderr,
				        "kanxeoctl: invalid --file= value '%s' (expected "
				        "CONTAINER_PATH=LOCAL_PATH[:MODE])\n",
				        argv[i] + 7);
				return 2;
			}
			file_count++;
		} else if (strncmp(argv[i], "--sysctl=", 9) == 0) {
			if (sysctl_count >= CLI_MAX_SYSCTLS) {
				fprintf(stderr, "kanxeoctl: too many --sysctl= flags (max %d)\n",
				        CLI_MAX_SYSCTLS);
				return 2;
			}
			if (parse_sysctl_flag(argv[i] + 9, &sysctls[sysctl_count]) != 0) {
				fprintf(stderr,
				        "kanxeoctl: invalid --sysctl= value '%s' (expected KEY=VALUE)\n",
				        argv[i] + 9);
				return 2;
			}
			sysctl_count++;
		} else {
			fprintf(stderr, "kanxeoctl: unknown run option '%s'\n", argv[i]);
			return 2;
		}
		i++;
	}

	if (name == NULL || image == NULL || cmd_start < 0 || cmd_start >= argc) {
		fprintf(stderr,
		        "usage: kanxeoctl run --name=NAME --image=IMAGE [--memory-max=N] "
		        "[--pids-max=N] [--cpu-max=\"QUOTA PERIOD\"] [--cpuset=0-1,3] "
		        "[--disk-quota=BYTES] "
		        "[--network=NAME[:IP] ...] [--ip-forward] [--dns-register] "
		        "[--pki-issue] [--pki-cert-dir=PATH] [--pki-days=N] "
		        "[--route=DEST/PREFIX:VIA ...] [--device=ID ...] [--interface=IFNAME ...] "
		        "[--restart=always|on-failure|unless-stopped] [--restart-delay=N] "
		        "[--depends-on=NAME ...] "
		        "[--readiness-tcp-port=N [--readiness-timeout=N]] "
		        "[--file=CONTAINER_PATH=LOCAL_PATH[:MODE] ...] [--sysctl=KEY=VALUE ...] "
		        "-- CMD [ARGS...]\n");
		return 2;
	}

	/*
	 * Built with the json writer (jw_*), not snprintf string
	 * concatenation, because name/image/cmd come from arbitrary
	 * user-supplied argv and must be properly JSON-escaped.
	 */
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "image");
	jw_str(&w, image);
	jw_key(&w, "cmd");
	jw_arr_open(&w);
	for (i = cmd_start; i < argc; i++)
		jw_str(&w, argv[i]);
	jw_arr_close(&w);
	if (memory_max >= 0) {
		jw_key(&w, "memory_max");
		jw_int(&w, memory_max);
	}
	if (pids_max >= 0) {
		jw_key(&w, "pids_max");
		jw_int(&w, pids_max);
	}
	if (cpu_max != NULL) {
		jw_key(&w, "cpu_max");
		jw_str(&w, cpu_max);
	}
	if (cpuset_cpus != NULL) {
		jw_key(&w, "cpuset_cpus");
		jw_str(&w, cpuset_cpus);
	}
	if (disk_quota_bytes >= 0) {
		jw_key(&w, "disk_quota_bytes");
		jw_int(&w, disk_quota_bytes);
	}
	if (network_count > 0) {
		jw_key(&w, "networks");
		jw_arr_open(&w);
		for (i = 0; i < network_count; i++) {
			if (networks[i].has_ip) {
				jw_obj_open(&w);
				jw_key(&w, "name");
				jw_str(&w, networks[i].name);
				jw_key(&w, "ip");
				jw_str(&w, networks[i].ip);
				jw_obj_close(&w);
			} else {
				jw_str(&w, networks[i].name);
			}
		}
		jw_arr_close(&w);
	}
	if (ip_forward) {
		jw_key(&w, "ip_forward");
		jw_bool(&w, 1);
	}
	if (dns_register) {
		jw_key(&w, "dns_register");
		jw_bool(&w, 1);
	}
	if (pki_issue) {
		jw_key(&w, "pki_issue");
		jw_bool(&w, 1);
		if (pki_cert_dir != NULL) {
			jw_key(&w, "pki_cert_dir");
			jw_str(&w, pki_cert_dir);
		}
		if (pki_days >= 0) {
			jw_key(&w, "pki_days");
			jw_int(&w, pki_days);
		}
	}
	if (route_count > 0) {
		jw_key(&w, "routes");
		jw_arr_open(&w);
		for (i = 0; i < route_count; i++) {
			jw_obj_open(&w);
			jw_key(&w, "dest");
			jw_str(&w, routes[i].dest);
			jw_key(&w, "prefix_len");
			jw_int(&w, routes[i].prefix_len);
			jw_key(&w, "via");
			jw_str(&w, routes[i].via);
			jw_obj_close(&w);
		}
		jw_arr_close(&w);
	}
	if (device_count > 0) {
		jw_key(&w, "devices");
		jw_arr_open(&w);
		for (i = 0; i < device_count; i++)
			jw_str(&w, devices[i]);
		jw_arr_close(&w);
	}
	if (interface_count > 0) {
		jw_key(&w, "interfaces");
		jw_arr_open(&w);
		for (i = 0; i < interface_count; i++)
			jw_str(&w, interfaces[i]);
		jw_arr_close(&w);
	}
	if (restart != NULL) {
		jw_key(&w, "restart");
		jw_str(&w, restart);
	}
	if (restart_delay >= 0) {
		jw_key(&w, "restart_delay_seconds");
		jw_int(&w, restart_delay);
	}
	if (depends_on_count > 0) {
		jw_key(&w, "depends_on");
		jw_arr_open(&w);
		for (i = 0; i < depends_on_count; i++)
			jw_str(&w, depends_on[i]);
		jw_arr_close(&w);
	}
	if (readiness_tcp_port >= 0) {
		jw_key(&w, "readiness");
		jw_obj_open(&w);
		jw_key(&w, "tcp_port");
		jw_int(&w, readiness_tcp_port);
		if (readiness_timeout >= 0) {
			jw_key(&w, "timeout_seconds");
			jw_int(&w, readiness_timeout);
		}
		jw_obj_close(&w);
	}
	if (file_count > 0) {
		jw_key(&w, "files");
		jw_arr_open(&w);
		for (i = 0; i < file_count; i++) {
			char *content;
			size_t content_len;

			if (read_local_file(files[i].local_path, &content, &content_len) != 0) {
				jw_free(&w);
				return 1;
			}
			jw_obj_open(&w);
			jw_key(&w, "path");
			jw_str(&w, files[i].container_path);
			jw_key(&w, "content");
			jw_str(&w, content);
			if (files[i].mode[0] != '\0') {
				jw_key(&w, "mode");
				jw_str(&w, files[i].mode);
			}
			jw_obj_close(&w);
			free(content);
		}
		jw_arr_close(&w);
	}
	if (sysctl_count > 0) {
		jw_key(&w, "sysctls");
		jw_obj_open(&w);
		for (i = 0; i < sysctl_count; i++) {
			jw_key(&w, sysctls[i].key);
			jw_str(&w, sysctls[i].value);
		}
		jw_obj_close(&w);
	}
	jw_obj_close(&w);

	/*
	 * kx_client_request() needs a NUL-terminated C string; w.buf isn't
	 * one, but jw_ensure()'s growth policy always keeps at least one
	 * spare byte of capacity beyond w.len, so writing the NUL directly
	 * here is safe without a further allocation.
	 */
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/containers", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_network_create(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *subnet = NULL;
	const char *gateway = NULL;
	long prefix_len = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--subnet=", 9) == 0)
			subnet = argv[i] + 9;
		else if (strncmp(argv[i], "--prefix=", 9) == 0)
			prefix_len = atol(argv[i] + 9);
		else if (strncmp(argv[i], "--gateway=", 10) == 0)
			gateway = argv[i] + 10;
		else {
			fprintf(stderr, "kanxeoctl: unknown network create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL || subnet == NULL || prefix_len < 0) {
		fprintf(stderr,
		        "usage: kanxeoctl network create --name=NAME --subnet=A.B.C.D --prefix=N "
		        "[--gateway=A.B.C.D]\n"
		        "  no --gateway= means the bridge stays pure L2 (no host-owned address) --\n"
		        "  the default. Pass --gateway= only when the host itself should be this\n"
		        "  network's router.\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "subnet");
	jw_str(&w, subnet);
	jw_key(&w, "prefix_len");
	jw_int(&w, prefix_len);
	if (gateway != NULL) {
		jw_key(&w, "gateway");
		jw_str(&w, gateway);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/networks", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_network_line);
}

static int cmd_network_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/networks", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_network_list);
}

static int cmd_network_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: network rm requires a network name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/networks/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_network_attach_interface(const struct kx_client *c, int json_mode, int argc,
                                         char **argv)
{
	const char *net_name;
	const char *ifname = NULL;
	long vlan_id = 0;
	int i;
	struct json_writer w;
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: network attach-interface requires a network name\n");
		return 2;
	}
	net_name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--interface=", 12) == 0)
			ifname = argv[i] + 12;
		else if (strncmp(argv[i], "--vlan=", 7) == 0)
			vlan_id = atol(argv[i] + 7);
		else {
			fprintf(stderr, "kanxeoctl: unknown network attach-interface option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (ifname == NULL) {
		fprintf(stderr,
		        "usage: kanxeoctl network attach-interface NAME --interface=IFNAME [--vlan=N]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "ifname");
	jw_str(&w, ifname);
	if (vlan_id != 0) {
		jw_key(&w, "vlan_id");
		jw_int(&w, vlan_id);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/networks/%s/interfaces", net_name);
	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_network_line);
}

static int cmd_network_detach_interface(const struct kx_client *c, int json_mode, int argc,
                                         char **argv)
{
	const char *net_name;
	const char *ifname = NULL;
	int i;
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: network detach-interface requires a network name\n");
		return 2;
	}
	net_name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--interface=", 12) == 0)
			ifname = argv[i] + 12;
		else {
			fprintf(stderr, "kanxeoctl: unknown network detach-interface option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (ifname == NULL) {
		fprintf(stderr, "usage: kanxeoctl network detach-interface NAME --interface=IFNAME\n");
		return 2;
	}

	snprintf(path, sizeof(path), "/v1/networks/%s/interfaces/%s", net_name, ifname);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_network(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: kanxeoctl network create --name=NAME --subnet=A.B.C.D --prefix=N "
		        "[--gateway=A.B.C.D]\n"
		        "       kanxeoctl network ls\n"
		        "       kanxeoctl network rm NAME\n"
		        "       kanxeoctl network attach-interface NAME --interface=IFNAME [--vlan=N]\n"
		        "       kanxeoctl network detach-interface NAME --interface=IFNAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_network_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_network_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_network_rm(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "attach-interface") == 0)
		return cmd_network_attach_interface(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "detach-interface") == 0)
		return cmd_network_detach_interface(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown network subcommand '%s'\n", sub);
	return 2;
}

static int cmd_image_create(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else {
			fprintf(stderr, "kanxeoctl: unknown image create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL) {
		fprintf(stderr, "usage: kanxeoctl image create --name=NAME\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/images", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_image_line);
}

static int cmd_image_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/images", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_image_list);
}

static int cmd_image_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: image rm requires an image name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/images/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_image(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl image create --name=NAME\n"
		                "       kanxeoctl image ls\n"
		                "       kanxeoctl image rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_image_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_image_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_image_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown image subcommand '%s'\n", sub);
	return 2;
}

static int cmd_device_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/devices", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_device_list);
}

static int cmd_device(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl device ls\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "ls") == 0)
		return cmd_device_ls(c, json_mode);

	fprintf(stderr, "kanxeoctl: unknown device subcommand '%s'\n", sub);
	return 2;
}

static int cmd_devicemap_create(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *kind = NULL;
	const char *selector = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--kind=", 7) == 0)
			kind = argv[i] + 7;
		else if (strncmp(argv[i], "--selector=", 11) == 0)
			selector = argv[i] + 11;
		else {
			fprintf(stderr, "kanxeoctl: unknown devicemap create option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || kind == NULL || selector == NULL) {
		fprintf(stderr,
		        "usage: kanxeoctl devicemap create --name=NAME --kind=exact|vendor_model "
		        "--selector=SELECTOR\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "kind");
	jw_str(&w, kind);
	jw_key(&w, "selector");
	jw_str(&w, selector);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/devicemaps", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_devicemap_line);
}

static int cmd_devicemap_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/devicemaps", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_devicemap_list);
}

static int cmd_devicemap_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: devicemap rm requires a mapping name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/devicemaps/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_devicemap(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: kanxeoctl devicemap create --name=NAME --kind=exact|vendor_model "
		        "--selector=SELECTOR\n"
		        "       kanxeoctl devicemap ls\n"
		        "       kanxeoctl devicemap rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_devicemap_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_devicemap_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_devicemap_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown devicemap subcommand '%s'\n", sub);
	return 2;
}

static int cmd_dns_record_create(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *ip = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--ip=", 5) == 0)
			ip = argv[i] + 5;
		else {
			fprintf(stderr, "kanxeoctl: unknown dns record create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL || ip == NULL) {
		fprintf(stderr, "usage: kanxeoctl dns record create --name=NAME --ip=A.B.C.D\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "ip");
	jw_str(&w, ip);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/dns/records", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_dns_record_line);
}

static int cmd_dns_record_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/dns/records", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_dns_record_list);
}

static int cmd_dns_record_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: dns record rm requires a record name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/dns/records/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_dns_record(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: kanxeoctl dns record create --name=NAME --ip=A.B.C.D\n"
		        "       kanxeoctl dns record ls\n"
		        "       kanxeoctl dns record rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_dns_record_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_dns_record_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_dns_record_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown dns record subcommand '%s'\n", sub);
	return 2;
}

static int cmd_dns_server_register(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *container = NULL;
	const char *hosts_path = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--container=", 12) == 0)
			container = argv[i] + 12;
		else if (strncmp(argv[i], "--hosts-path=", 13) == 0)
			hosts_path = argv[i] + 13;
		else {
			fprintf(stderr, "kanxeoctl: unknown dns server register option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (container == NULL || hosts_path == NULL) {
		fprintf(stderr,
		        "usage: kanxeoctl dns server register --container=NAME --hosts-path=PATH\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container);
	jw_key(&w, "hosts_path");
	jw_str(&w, hosts_path);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/dns/servers", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_dns_server_line);
}

static int cmd_dns_server_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/dns/servers", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_dns_server_list);
}

static int cmd_dns_server_unregister(const struct kx_client *c, int json_mode, int argc,
                                      char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: dns server unregister requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/dns/servers/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_dns_server(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: kanxeoctl dns server register --container=NAME --hosts-path=PATH\n"
		        "       kanxeoctl dns server ls\n"
		        "       kanxeoctl dns server unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "register") == 0)
		return cmd_dns_server_register(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_dns_server_ls(c, json_mode);
	if (strcmp(sub, "unregister") == 0)
		return cmd_dns_server_unregister(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown dns server subcommand '%s'\n", sub);
	return 2;
}

static int cmd_dns(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl dns record ...\n"
		                "       kanxeoctl dns server ...\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "record") == 0)
		return cmd_dns_record(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "server") == 0)
		return cmd_dns_server(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown dns subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pki_ca_bootstrap(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *common_name = NULL;
	long days = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--common-name=", 14) == 0)
			common_name = argv[i] + 14;
		else if (strncmp(argv[i], "--days=", 7) == 0)
			days = atol(argv[i] + 7);
		else {
			fprintf(stderr, "kanxeoctl: unknown pki ca bootstrap option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (common_name != NULL) {
		jw_key(&w, "common_name");
		jw_str(&w, common_name);
	}
	if (days >= 0) {
		jw_key(&w, "days");
		jw_int(&w, days);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pki/ca", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_ca_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pki/ca", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_ca(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl pki ca bootstrap [--common-name=NAME] [--days=N]\n"
		                "       kanxeoctl pki ca show\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "bootstrap") == 0)
		return cmd_pki_ca_bootstrap(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_pki_ca_show(c, json_mode);

	fprintf(stderr, "kanxeoctl: unknown pki ca subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pki_intermediate_bootstrap(const struct kx_client *c, int json_mode, int argc,
                                           char **argv)
{
	const char *common_name = NULL;
	long days = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--common-name=", 14) == 0)
			common_name = argv[i] + 14;
		else if (strncmp(argv[i], "--days=", 7) == 0)
			days = atol(argv[i] + 7);
		else {
			fprintf(stderr, "kanxeoctl: unknown pki intermediate bootstrap option '%s'\n",
			        argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (common_name != NULL) {
		jw_key(&w, "common_name");
		jw_str(&w, common_name);
	}
	if (days >= 0) {
		jw_key(&w, "days");
		jw_int(&w, days);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pki/intermediate", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_intermediate_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pki/intermediate", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_intermediate(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl pki intermediate bootstrap [--common-name=NAME] [--days=N]\n"
		                "       kanxeoctl pki intermediate show\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "bootstrap") == 0)
		return cmd_pki_intermediate_bootstrap(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_pki_intermediate_show(c, json_mode);

	fprintf(stderr, "kanxeoctl: unknown pki intermediate subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pki_cert_create(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *sans = NULL;
	long days = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--sans=", 7) == 0)
			sans = argv[i] + 7;
		else if (strncmp(argv[i], "--days=", 7) == 0)
			days = atol(argv[i] + 7);
		else {
			fprintf(stderr, "kanxeoctl: unknown pki cert create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL) {
		fprintf(stderr, "usage: kanxeoctl pki cert create --name=NAME [--sans=a,b,c] [--days=N]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	if (sans != NULL) {
		char buf[1024];
		char *tok, *save = NULL;

		snprintf(buf, sizeof(buf), "%s", sans);
		jw_key(&w, "sans");
		jw_arr_open(&w);
		tok = strtok_r(buf, ",", &save);
		while (tok != NULL) {
			jw_str(&w, tok);
			tok = strtok_r(NULL, ",", &save);
		}
		jw_arr_close(&w);
	}
	if (days >= 0) {
		jw_key(&w, "days");
		jw_int(&w, days);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pki/certs", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_cert_issued);
}

static int cmd_pki_cert_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pki/certs", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pki_cert_list);
}

static int cmd_pki_cert_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: pki cert rm requires a cert name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/pki/certs/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_pki_cert(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl pki cert create --name=NAME [--sans=a,b,c] [--days=N]\n"
		                "       kanxeoctl pki cert ls\n"
		                "       kanxeoctl pki cert rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_pki_cert_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_pki_cert_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_pki_cert_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown pki cert subcommand '%s'\n", sub);
	return 2;
}

/*
 * Destructive: wipes and regenerates the whole CA chain (root, plus
 * the intermediate if one exists), reissuing every currently-tracked
 * leaf under the new chain. No separate --yes confirmation flag --
 * matches every other destructive kanxeoctl subcommand's own direct-
 * execution convention (pki cert rm, network rm, ...); the web
 * dashboard's own confirm dialog is where the "are you sure" prompt
 * lives for this project.
 */
static int cmd_pki_reset(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *root_cn = NULL;
	const char *intermediate_cn = NULL;
	long root_days = -1;
	long intermediate_days = -1;
	long leaf_days = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--root-common-name=", 19) == 0)
			root_cn = argv[i] + 19;
		else if (strncmp(argv[i], "--intermediate-common-name=", 27) == 0)
			intermediate_cn = argv[i] + 27;
		else if (strncmp(argv[i], "--root-days=", 12) == 0)
			root_days = atol(argv[i] + 12);
		else if (strncmp(argv[i], "--intermediate-days=", 20) == 0)
			intermediate_days = atol(argv[i] + 20);
		else if (strncmp(argv[i], "--leaf-days=", 12) == 0)
			leaf_days = atol(argv[i] + 12);
		else {
			fprintf(stderr, "kanxeoctl: unknown pki reset option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (root_cn != NULL) {
		jw_key(&w, "root_common_name");
		jw_str(&w, root_cn);
	}
	if (intermediate_cn != NULL) {
		jw_key(&w, "intermediate_common_name");
		jw_str(&w, intermediate_cn);
	}
	if (root_days >= 0) {
		jw_key(&w, "root_days");
		jw_int(&w, root_days);
	}
	if (intermediate_days >= 0) {
		jw_key(&w, "intermediate_days");
		jw_int(&w, intermediate_days);
	}
	if (leaf_days >= 0) {
		jw_key(&w, "leaf_days");
		jw_int(&w, leaf_days);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pki/reset", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_reset);
}

static int cmd_pki(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl pki ca ...\n"
		                "       kanxeoctl pki intermediate ...\n"
		                "       kanxeoctl pki cert ...\n"
		                "       kanxeoctl pki reset [--root-common-name=NAME]\n"
		                "               [--intermediate-common-name=NAME] [--root-days=N]\n"
		                "               [--intermediate-days=N] [--leaf-days=N]  -- wipes and\n"
		                "               regenerates the whole CA chain, reissuing every leaf\n"
		                "               currently tracked; defaults name each tier after this\n"
		                "               install's own domain_suffix (kanxeoctl site show)\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "ca") == 0)
		return cmd_pki_ca(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "intermediate") == 0)
		return cmd_pki_intermediate(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "cert") == 0)
		return cmd_pki_cert(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "reset") == 0)
		return cmd_pki_reset(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown pki subcommand '%s'\n", sub);
	return 2;
}

static void fmt_bootstrapped(const struct json_value *v)
{
	(void)v;
	printf("bootstrapped\n");
}

static void fmt_pkg_recipe_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *version = json_str_field(v, "version");
	const char *depends = json_str_field(v, "depends");

	printf("%-24s %-16s %s\n", name, version, depends != NULL && depends[0] != '\0' ? depends : "-");
}

static void fmt_pkg_recipe_list(const struct json_value *v)
{
	const struct json_value *recipes = json_object_get(v, "recipes");
	size_t i;

	if (recipes == NULL || recipes->type != JSON_ARRAY)
		return;
	for (i = 0; i < recipes->u.array.count; i++)
		fmt_pkg_recipe_line(recipes->u.array.items[i]);
}

static void fmt_pkg_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *image = json_str_field(v, "image");
	const char *version = json_str_field(v, "version");
	const char *state = json_str_field(v, "state");
	const char *error = json_str_field(v, "error");
	const char *available = json_str_field(v, "available_version");

	printf("%-24s %-16s %-12s %-10s %-14s %s\n", name,
	       image != NULL && image[0] != '\0' ? image : "-", version, state,
	       available != NULL && available[0] != '\0' ? available : "-",
	       error != NULL && error[0] != '\0' ? error : "-");
}

static void fmt_pkg_list(const struct json_value *v)
{
	const struct json_value *packages = json_object_get(v, "packages");
	size_t i;

	if (packages == NULL || packages->type != JSON_ARRAY)
		return;
	for (i = 0; i < packages->u.array.count; i++)
		fmt_pkg_line(packages->u.array.items[i]);
}

static int cmd_pkg_bootstrap(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *toolchain = NULL;
	int i;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--toolchain=", 12) == 0)
			toolchain = argv[i] + 12;
		else {
			fprintf(stderr, "kanxeoctl: unknown pkg bootstrap option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (toolchain == NULL) {
		if (kx_client_request(c, "POST", "/v1/pkg/bootstrap", NULL, &r) != 0) {
			fprintf(stderr, "kanxeoctl: could not reach daemon\n");
			return 1;
		}
	} else {
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "toolchain_path");
		jw_str(&w, toolchain);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';

		if (kx_client_request(c, "POST", "/v1/pkg/bootstrap", w.buf, &r) != 0) {
			jw_free(&w);
			fprintf(stderr, "kanxeoctl: could not reach daemon\n");
			return 1;
		}
		jw_free(&w);
	}
	return emit(&r, json_mode, fmt_bootstrapped);
}

static int cmd_pkg_recipes(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pkg/recipes", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_recipe_list);
}

/* ADR-0040: add or update a recipe on an already-running system, no
 * ISO rebuild/reinstall needed -- the real, ongoing way recipes get
 * onto a system. --name= is the lookup key (must match the .recipe
 * content's own pkg_name= field, validated server-side); --file= is a
 * local path to the .recipe file's content, read and embedded the
 * same way `run --file=` already stages container config files. */
static int cmd_pkg_recipe_add(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *file = NULL;
	char *content;
	size_t content_len;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--file=", 7) == 0)
			file = argv[i] + 7;
		else {
			fprintf(stderr, "kanxeoctl: unknown pkg recipe add option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || file == NULL) {
		fprintf(stderr, "usage: kanxeoctl pkg recipe add --name=NAME --file=PATH\n");
		return 2;
	}
	if (read_local_file(file, &content, &content_len) != 0) {
		fprintf(stderr, "kanxeoctl: could not read %s\n", file);
		return 1;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "content");
	jw_str(&w, content);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	free(content);

	if (kx_client_request(c, "POST", "/v1/pkg/recipes", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "kanxeoctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		kx_response_free(&r);
		return 1;
	}
	printf("recipe '%s' added\n", name);
	kx_response_free(&r);
	return 0;
}

static void fmt_pkg_recipe_show(const struct json_value *v)
{
	const char *content = json_str_field(v, "content");

	printf("%s", content != NULL ? content : "");
}

static int cmd_pkg_recipe_show(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl pkg recipe show NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/pkg/recipes/%s", argv[0]);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_recipe_show);
}

static int cmd_pkg_recipe_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl pkg recipe rm NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/pkg/recipes/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_pkg_recipe(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl pkg recipe add --name=NAME --file=PATH\n"
		                "       kanxeoctl pkg recipe show NAME\n"
		                "       kanxeoctl pkg recipe rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "add") == 0)
		return cmd_pkg_recipe_add(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_pkg_recipe_show(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_pkg_recipe_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown pkg recipe subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pkg_install(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *image = NULL;
	int upgrade = 0;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strcmp(argv[i], "--upgrade") == 0)
			upgrade = 1;
		else {
			fprintf(stderr, "kanxeoctl: unknown pkg install option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: kanxeoctl pkg install --name=NAME [--image=IMAGE] [--upgrade]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	if (image != NULL) {
		jw_key(&w, "image");
		jw_str(&w, image);
	}
	if (upgrade) {
		jw_key(&w, "upgrade");
		jw_bool(&w, 1);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pkg/install", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pkg_line);
}

/* Polls GET /v1/pkg/hostbuild/name until state leaves "fetching"/
 * "building" (--wait's own loop, and --deploy's own prerequisite --
 * it needs the finished artifact_path, not the 202's own in-flight
 * snapshot). Prints nothing itself; *out is the final response,
 * caller-owned (kx_response_free()'d by the caller). Returns 0 on a
 * real terminal state (installed/failed), -1 if the daemon became
 * unreachable mid-poll. */
static int poll_hostbuild(const struct kx_client *c, const char *name, struct kx_response *out)
{
	char path[300];

	snprintf(path, sizeof(path), "/v1/pkg/hostbuild/%s", name);
	for (;;) {
		const char *state;

		if (kx_client_request(c, "GET", path, NULL, out) != 0) {
			fprintf(stderr, "kanxeoctl: could not reach daemon\n");
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || (strcmp(state, "fetching") != 0 && strcmp(state, "building") != 0))
			return 0;
		kx_response_free(out);
		usleep(500000);
	}
}

static int cmd_pkg_hostbuild(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *build_image = NULL;
	int wait = 0, deploy = 0;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--build-image=", 14) == 0)
			build_image = argv[i] + 14;
		else if (strcmp(argv[i], "--wait") == 0)
			wait = 1;
		else if (strcmp(argv[i], "--deploy") == 0)
			deploy = 1; /* implies --wait -- a not-yet-finished artifact has no path to deploy */
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "kanxeoctl: unknown pkg hostbuild option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || build_image == NULL) {
		fprintf(stderr,
		        "usage: kanxeoctl pkg hostbuild NAME --build-image=IMAGE [--wait] [--deploy]\n");
		return 2;
	}
	if (deploy)
		wait = 1;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "build_image");
	jw_str(&w, build_image);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pkg/hostbuild", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status < 200 || r.status >= 300)
		return emit(&r, json_mode, fmt_pkg_line);

	if (!wait)
		return emit(&r, json_mode, fmt_pkg_line);
	kx_response_free(&r);

	if (poll_hostbuild(c, name, &r) != 0)
		return 1;

	/* emit() frees r internally -- can't use it after, so only call it
	 * on the terminal "we're done, nothing more to read out of r" path
	 * (an error response, or no --deploy requested). --deploy still
	 * needs artifact_path out of r afterward, so that path prints
	 * manually instead and keeps r alive a little longer. */
	if (r.status < 200 || r.status >= 300 || !deploy)
		return emit(&r, json_mode, fmt_pkg_line);

	if (json_mode)
		print_raw_json(r.json);
	else
		fmt_pkg_line(r.json);

	{
		const char *state = json_str_field(r.json, "state");
		const char *artifact_path = json_str_field(r.json, "artifact_path");
		char deploy_arg[PATH_MAX + 16];
		char *deploy_argv[1];
		int rc;

		if (state == NULL || strcmp(state, "installed") != 0 || artifact_path == NULL) {
			kx_response_free(&r);
			fprintf(stderr, "kanxeoctl: hostbuild did not produce an artifact to deploy\n");
			return 1;
		}
		/* Honest, explicit per-name handling -- only what this
		 * mechanism has real recipes for today gets a real deploy
		 * path, not a fake-generic dispatcher pretending to support
		 * every possible hostbuild recipe name. */
		if (strcmp(name, "kernel") == 0)
			snprintf(deploy_arg, sizeof(deploy_arg), "--kernel=%s/bzImage", artifact_path);
		else if (strcmp(name, "kanxeo") == 0)
			/* kanxeod-root.squashfs is assembled server-side by the
			 * daemon itself (ADR-0057), asynchronously, once this
			 * hostbuild's own artifacts finish harvesting -- may not
			 * exist yet the instant --wait's own poll sees
			 * state=="installed" (that only means pkg_build_completed()
			 * ran, not that the follow-on mkbootroot child has
			 * finished). This CLI never invokes mkbootroot itself
			 * (API-First Mandate) -- if the squashfs isn't there yet,
			 * /system/update's own real, existing 400 for a missing/
			 * unreadable image_path is the honest answer, not a second
			 * polling loop bolted on here for one recipe name.
			 */
			snprintf(deploy_arg, sizeof(deploy_arg), "--image=%s/kanxeod-root.squashfs",
			         artifact_path);
		else {
			kx_response_free(&r);
			fprintf(stderr, "kanxeoctl: --deploy has no rule for hostbuild '%s' yet\n", name);
			return 1;
		}
		kx_response_free(&r);
		deploy_argv[0] = deploy_arg;
		rc = cmd_update(c, json_mode, 1, deploy_argv);
		return rc;
	}
}

static int cmd_pkg_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pkg", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_list);
}

static int cmd_pkg_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "kanxeoctl: pkg rm requires a package name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/pkg/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

/* "status" is only present in the "nothing to update" response --
 * pkg_get_one()'s own JSON shape (name/image/version/state/...) has no
 * such field, so its presence alone unambiguously picks which shape
 * this response is. */
static void fmt_pkg_update_all(const struct json_value *v)
{
	const char *status = json_str_field(v, "status");

	if (status != NULL) {
		printf("%s\n", status);
		return;
	}
	fmt_pkg_line(v);
}

static int cmd_pkg_update_all(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "POST", "/v1/pkg/update-all", "{}", &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_update_all);
}

static int cmd_pkg(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: kanxeoctl pkg bootstrap [--toolchain=PATH]\n"
		                "       kanxeoctl pkg recipes\n"
		                "       kanxeoctl pkg recipe add --name=NAME --file=PATH\n"
		                "       kanxeoctl pkg recipe show NAME\n"
		                "       kanxeoctl pkg recipe rm NAME\n"
		                "       kanxeoctl pkg install --name=NAME [--image=IMAGE] [--upgrade]\n"
		                "       kanxeoctl pkg hostbuild NAME --build-image=IMAGE [--wait] [--deploy]\n"
		                "       kanxeoctl pkg ls\n"
		                "       kanxeoctl pkg rm NAME[@IMAGE]\n"
		                "       kanxeoctl pkg update-all\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "bootstrap") == 0)
		return cmd_pkg_bootstrap(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "recipes") == 0)
		return cmd_pkg_recipes(c, json_mode);
	if (strcmp(sub, "recipe") == 0)
		return cmd_pkg_recipe(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "install") == 0)
		return cmd_pkg_install(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "hostbuild") == 0)
		return cmd_pkg_hostbuild(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_pkg_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_pkg_rm(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "update-all") == 0)
		return cmd_pkg_update_all(c, json_mode);

	fprintf(stderr, "kanxeoctl: unknown pkg subcommand '%s'\n", sub);
	return 2;
}

static void fmt_iso_status(const struct json_value *v)
{
	const char *state = json_str_field(v, "state");
	const char *iso_path = json_str_field(v, "iso_path");
	const char *error = json_str_field(v, "error");

	printf("state=%s", state != NULL ? state : "?");
	if (iso_path != NULL)
		printf(" iso_path=%s", iso_path);
	if (error != NULL)
		printf(" error=%s", error);
	printf("\n");
}

/* Polls GET /v1/system/iso until state leaves "building" -- --wait's own
 * loop, the same shape poll_hostbuild() already established. */
static int poll_iso(const struct kx_client *c, struct kx_response *out)
{
	for (;;) {
		const char *state;

		if (kx_client_request(c, "GET", "/v1/system/iso", NULL, out) != 0) {
			fprintf(stderr, "kanxeoctl: could not reach daemon\n");
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || strcmp(state, "building") != 0)
			return 0;
		kx_response_free(out);
		usleep(500000);
	}
}

static int cmd_iso_status(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/iso", NULL, &r) != 0) {
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_iso_status);
}

static int cmd_iso_build(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *disk = NULL;
	const char *ip = NULL;
	const char *prefix = NULL;
	const char *gateway = NULL;
	const char *interface = NULL;
	int wait = 0;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--disk=", 7) == 0)
			disk = argv[i] + 7;
		else if (strncmp(argv[i], "--ip=", 5) == 0)
			ip = argv[i] + 5;
		else if (strncmp(argv[i], "--prefix=", 9) == 0)
			prefix = argv[i] + 9;
		else if (strncmp(argv[i], "--gateway=", 10) == 0)
			gateway = argv[i] + 10;
		else if (strncmp(argv[i], "--interface=", 12) == 0)
			interface = argv[i] + 12;
		else if (strcmp(argv[i], "--wait") == 0)
			wait = 1;
		else {
			fprintf(stderr, "kanxeoctl: unknown iso build option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (disk != NULL) {
		jw_key(&w, "disk");
		jw_str(&w, disk);
	}
	if (ip != NULL) {
		jw_key(&w, "ip");
		jw_str(&w, ip);
	}
	if (prefix != NULL) {
		jw_key(&w, "prefix");
		jw_str(&w, prefix);
	}
	if (gateway != NULL) {
		jw_key(&w, "gateway");
		jw_str(&w, gateway);
	}
	if (interface != NULL) {
		jw_key(&w, "interface");
		jw_str(&w, interface);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/system/iso", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "kanxeoctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status < 200 || r.status >= 300 || !wait)
		return emit(&r, json_mode, fmt_iso_status);
	kx_response_free(&r);

	if (poll_iso(c, &r) != 0)
		return 1;
	return emit(&r, json_mode, fmt_iso_status);
}

static int cmd_iso(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: kanxeoctl iso build [--disk=DEV --ip=A.B.C.D --prefix=N "
		        "--gateway=A.B.C.D --interface=IFNAME] [--wait]\n"
		        "       kanxeoctl iso status\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "build") == 0)
		return cmd_iso_build(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "status") == 0)
		return cmd_iso_status(c, json_mode);

	fprintf(stderr, "kanxeoctl: unknown iso subcommand '%s'\n", sub);
	return 2;
}

/*
 * The one dispatch table, shared by main()'s own one-shot invocation
 * and run_shell()'s interactive loop below -- extracted so both call
 * exactly the same code per command instead of two copies of this
 * chain drifting apart over time. argc/argv here are already just the
 * command's own remaining arguments (the leading "--host="-style
 * global flags and the command name itself are stripped by the
 * caller before this is reached).
 */
static int dispatch_command(const struct kx_client *client, int json_mode, const char *cmd,
                             int argc, char **argv)
{
	if (strcmp(cmd, "health") == 0)
		return cmd_health(client, json_mode);
	if (strcmp(cmd, "shutdown") == 0)
		return cmd_shutdown(client, json_mode);
	if (strcmp(cmd, "reboot") == 0)
		return cmd_reboot(client, json_mode);
	if (strcmp(cmd, "update") == 0)
		return cmd_update(client, json_mode, argc, argv);
	if (strcmp(cmd, "backup") == 0)
		return cmd_backup(client, json_mode, argc, argv);
	if (strcmp(cmd, "restore") == 0)
		return cmd_restore(client, json_mode, argc, argv);
	if (strcmp(cmd, "site") == 0)
		return cmd_site(client, json_mode, argc, argv);
	if (strcmp(cmd, "daemon-config") == 0)
		return cmd_daemon_config(client, json_mode, argc, argv);
	if (strcmp(cmd, "iso") == 0)
		return cmd_iso(client, json_mode, argc, argv);
	if (strcmp(cmd, "ps") == 0)
		return cmd_ps(client, json_mode);
	if (strcmp(cmd, "run") == 0)
		return cmd_run(client, json_mode, argc, argv);
	if (strcmp(cmd, "inspect") == 0)
		return cmd_inspect(client, json_mode, argc, argv);
	if (strcmp(cmd, "stop") == 0)
		return cmd_stop(client, json_mode, argc, argv);
	if (strcmp(cmd, "start") == 0)
		return cmd_start(client, json_mode, argc, argv);
	if (strcmp(cmd, "pause") == 0)
		return cmd_pause(client, json_mode, argc, argv);
	if (strcmp(cmd, "unpause") == 0)
		return cmd_unpause(client, json_mode, argc, argv);
	if (strcmp(cmd, "stats") == 0)
		return cmd_container_stats(client, json_mode, argc, argv);
	if (strcmp(cmd, "console") == 0)
		return cmd_console(client, argc, argv);
	if (strcmp(cmd, "files") == 0)
		return cmd_files(client, argc, argv);
	if (strcmp(cmd, "rm") == 0)
		return cmd_rm(client, json_mode, argc, argv);
	if (strcmp(cmd, "network") == 0)
		return cmd_network(client, json_mode, argc, argv);
	if (strcmp(cmd, "image") == 0)
		return cmd_image(client, json_mode, argc, argv);
	if (strcmp(cmd, "device") == 0)
		return cmd_device(client, json_mode, argc, argv);
	if (strcmp(cmd, "devicemap") == 0)
		return cmd_devicemap(client, json_mode, argc, argv);
	if (strcmp(cmd, "dns") == 0)
		return cmd_dns(client, json_mode, argc, argv);
	if (strcmp(cmd, "pki") == 0)
		return cmd_pki(client, json_mode, argc, argv);
	if (strcmp(cmd, "pkg") == 0)
		return cmd_pkg(client, json_mode, argc, argv);

	fprintf(stderr, "kanxeoctl: unknown command '%s'\n", cmd);
	print_usage(stderr);
	return 2;
}

#define SHELL_MAX_TOKENS 64

/*
 * Splits line (modified in place) into up to max_tokens whitespace-
 * separated tokens, treating a "..."/'...' span as one token with the
 * quotes stripped -- not a full shell grammar (no backslash-escapes
 * inside quotes), just enough for the one real case that needs it:
 * "run --name=X --image=Y -- /bin/sh -c \"sleep 1\"", where a CMD arg
 * needs an embedded space.
 */
static int tokenize_line(char *line, char **tokens, int max_tokens)
{
	int n = 0;
	char *p = line;

	while (*p != '\0' && n < max_tokens) {
		char quote = '\0';

		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (*p == '\0')
			break;
		if (*p == '"' || *p == '\'') {
			quote = *p;
			p++;
		}
		tokens[n++] = p;
		if (quote != '\0') {
			while (*p != '\0' && *p != quote)
				p++;
		} else {
			while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
				p++;
		}
		if (*p != '\0') {
			*p = '\0';
			p++;
		}
	}
	return n;
}

/*
 * Interactive shell: entered when kanxeoctl is invoked with no command
 * and stdin is a real terminal (see main()) -- one persistent client,
 * one dispatch_command() call per typed line, no reconnect-per-command
 * ceremony. Plain fgets(), deliberately no GNU readline (no history/
 * arrow-key editing) -- this project's own CLI links against nothing
 * but its own code today, and readline would be its first external
 * runtime dependency; not warranted for what was asked ("keep it
 * simple"). A failed command prints its existing error and continues
 * the loop -- a broken command shouldn't end the session, the same
 * posture any real shell already has. "exit"/"quit" or EOF (Ctrl-D)
 * end it; "help" reuses print_usage(), not a second copy of it.
 */
static int run_shell(const struct kx_client *client, int json_mode)
{
	char line[4096];
	char *tokens[SHELL_MAX_TOKENS];

	printf("kanxeoctl interactive shell -- type a command (e.g. \"ps\"), \"help\", or \"exit\"\n");
	for (;;) {
		int n;

		printf("kanxeo> ");
		fflush(stdout);
		if (fgets(line, sizeof(line), stdin) == NULL) {
			printf("\n");
			break;
		}
		n = tokenize_line(line, tokens, SHELL_MAX_TOKENS);
		if (n == 0)
			continue;
		if (strcmp(tokens[0], "exit") == 0 || strcmp(tokens[0], "quit") == 0)
			break;
		if (strcmp(tokens[0], "help") == 0) {
			print_usage(stdout);
			continue;
		}
		dispatch_command(client, json_mode, tokens[0], n - 1, tokens + 1);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *host = DEFAULT_HOST;
	int port = DEFAULT_PORT;
	int json_mode = 0;
	int i = 1;
	const char *cmd;
	struct kx_client client;

	while (i < argc && strncmp(argv[i], "--", 2) == 0) {
		if (strncmp(argv[i], "--host=", 7) == 0)
			host = argv[i] + 7;
		else if (strncmp(argv[i], "--port=", 7) == 0)
			port = atoi(argv[i] + 7);
		else if (strcmp(argv[i], "--json") == 0)
			json_mode = 1;
		else {
			fprintf(stderr, "kanxeoctl: unknown option '%s'\n", argv[i]);
			print_usage(stderr);
			return 2;
		}
		i++;
	}

	kx_client_init(&client, host, port);

	if (i >= argc) {
		if (isatty(STDIN_FILENO))
			return run_shell(&client, json_mode);
		print_usage(stderr);
		return 2;
	}
	cmd = argv[i++];

	return dispatch_command(&client, json_mode, cmd, argc - i, argv + i);
}
