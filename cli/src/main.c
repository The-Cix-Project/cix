/*
 * kanxeoctl: a pure REST client for the Kanxeo host API
 * (docs/api/openapi.yaml). Per the project's API-First Mandate
 * (CLAUDE.md, ADR-0005), this file holds no namespace/cgroup/mount
 * logic of its own -- every subcommand is exactly one HTTP call via
 * client/src/httpclient.c, the same library test_daemon.c uses to
 * verify the daemon.
 */
#include "httpclient.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 7620

static void print_usage(FILE *out)
{
	fprintf(out,
	        "usage: kanxeoctl [--host=ADDR] [--port=N] [--json] <command> [args]\n"
	        "\n"
	        "commands:\n"
	        "  health\n"
	        "  ps\n"
	        "  run --name=NAME --image=IMAGE [--memory-max=BYTES] [--pids-max=N] [--network=NAME ...]\n"
	        "      [--ip-forward] [--route=DEST/PREFIX:VIA ...] -- CMD [ARGS...]\n"
	        "  inspect NAME\n"
	        "  rm NAME\n"
	        "  network create --name=NAME --subnet=A.B.C.D --prefix=N\n"
	        "  network ls\n"
	        "  network rm NAME\n");
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
	char exit_buf[16];
	char net_buf[256];
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

	printf("%-20s %-8s pid=%-8ld exit_status=%-6s networks=%-20s fwd=%s\n", name, status, pid,
	       exit_buf, net_buf[0] != '\0' ? net_buf : "-",
	       (ip_forward != NULL && ip_forward->type == JSON_BOOL && ip_forward->u.boolean) ? "yes"
	                                                                                        : "no");
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
	const char *gateway = json_str_field(v, "gateway");

	printf("%-20s %s/%-3ld gateway=%s\n", name, subnet, prefix_len, gateway);
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

#define CLI_MAX_NETWORKS 4
#define CLI_MAX_ROUTES 8

struct cli_route {
	char dest[64];
	int prefix_len;
	char via[64];
};

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

static int cmd_run(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *image = NULL;
	const char *networks[CLI_MAX_NETWORKS];
	int network_count = 0;
	int ip_forward = 0;
	struct cli_route routes[CLI_MAX_ROUTES];
	int route_count = 0;
	long memory_max = -1;
	long pids_max = -1;
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
		else if (strncmp(argv[i], "--network=", 10) == 0) {
			if (network_count >= CLI_MAX_NETWORKS) {
				fprintf(stderr, "kanxeoctl: too many --network= flags (max %d)\n",
				        CLI_MAX_NETWORKS);
				return 2;
			}
			networks[network_count++] = argv[i] + 10;
		} else if (strcmp(argv[i], "--ip-forward") == 0) {
			ip_forward = 1;
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
		} else {
			fprintf(stderr, "kanxeoctl: unknown run option '%s'\n", argv[i]);
			return 2;
		}
		i++;
	}

	if (name == NULL || image == NULL || cmd_start < 0 || cmd_start >= argc) {
		fprintf(stderr,
		        "usage: kanxeoctl run --name=NAME --image=IMAGE [--memory-max=N] "
		        "[--pids-max=N] [--network=NAME ...] [--ip-forward] "
		        "[--route=DEST/PREFIX:VIA ...] -- CMD [ARGS...]\n");
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
	if (network_count > 0) {
		jw_key(&w, "networks");
		jw_arr_open(&w);
		for (i = 0; i < network_count; i++)
			jw_str(&w, networks[i]);
		jw_arr_close(&w);
	}
	if (ip_forward) {
		jw_key(&w, "ip_forward");
		jw_bool(&w, 1);
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
		else {
			fprintf(stderr, "kanxeoctl: unknown network create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL || subnet == NULL || prefix_len < 0) {
		fprintf(stderr,
		        "usage: kanxeoctl network create --name=NAME --subnet=A.B.C.D --prefix=N\n");
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

static int cmd_network(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: kanxeoctl network create --name=NAME --subnet=A.B.C.D --prefix=N\n"
		        "       kanxeoctl network ls\n"
		        "       kanxeoctl network rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_network_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_network_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_network_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "kanxeoctl: unknown network subcommand '%s'\n", sub);
	return 2;
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

	if (i >= argc) {
		print_usage(stderr);
		return 2;
	}
	cmd = argv[i++];

	kx_client_init(&client, host, port);

	if (strcmp(cmd, "health") == 0)
		return cmd_health(&client, json_mode);
	if (strcmp(cmd, "ps") == 0)
		return cmd_ps(&client, json_mode);
	if (strcmp(cmd, "run") == 0)
		return cmd_run(&client, json_mode, argc - i, argv + i);
	if (strcmp(cmd, "inspect") == 0)
		return cmd_inspect(&client, json_mode, argc - i, argv + i);
	if (strcmp(cmd, "rm") == 0)
		return cmd_rm(&client, json_mode, argc - i, argv + i);
	if (strcmp(cmd, "network") == 0)
		return cmd_network(&client, json_mode, argc - i, argv + i);

	fprintf(stderr, "kanxeoctl: unknown command '%s'\n", cmd);
	print_usage(stderr);
	return 2;
}
