#include "routeconfig.h"

#include "logstore.h"
#include "persist.h"
#include "rtnetlink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ADR-0387. The file is [{"dest":"A.B.C.D","prefix":N,"gateway":"A.B.C.D"}],
 * gateway absent for an on-link route. Whether a route is in the kernel
 * is not saved: the kernel is the truth for that, and a start always
 * tries again.
 */

static char g_state_path[512];
static struct route_saved g_routes[ROUTECONFIG_MAX];
static int g_count;

static void ip_str(uint32_t be, char *out, size_t size)
{
	struct in_addr a;

	a.s_addr = be;
	if (inet_ntop(AF_INET, &a, out, (socklen_t)size) == NULL)
		snprintf(out, size, "?");
}

static int save_state(void)
{
	struct json_writer w;
	char ip[INET_ADDRSTRLEN];
	int i, rc;

	if (g_state_path[0] == '\0')
		return 0;
	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < g_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "dest");
		ip_str(g_routes[i].dest_be, ip, sizeof(ip));
		jw_str(&w, ip);
		jw_key(&w, "prefix");
		jw_int(&w, g_routes[i].prefix);
		if (g_routes[i].gateway_be != 0) {
			jw_key(&w, "gateway");
			ip_str(g_routes[i].gateway_be, ip, sizeof(ip));
			jw_str(&w, ip);
		}
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int routeconfig_init(const char *state_path)
{
	char *buf = NULL;
	size_t len = 0, i;
	struct json_value *root;

	g_count = 0;
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* nothing kept yet */
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY || root->u.array.count > ROUTECONFIG_MAX) {
		json_free(root);
		fprintf(stderr, "%s: malformed saved routes\n", g_state_path);
		return -1;
	}
	for (i = 0; i < root->u.array.count; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *dest = json_as_string(json_object_get(item, "dest"));
		const char *gw = json_as_string(json_object_get(item, "gateway"));
		const struct json_value *jp = json_object_get(item, "prefix");
		struct in_addr a;
		struct route_saved r;

		memset(&r, 0, sizeof(r));
		if (dest == NULL || inet_pton(AF_INET, dest, &a) != 1 || jp == NULL ||
		    jp->type != JSON_NUMBER) {
			json_free(root);
			fprintf(stderr, "%s: malformed saved route %zu\n", g_state_path, i);
			return -1;
		}
		r.dest_be = a.s_addr;
		r.prefix = (int)json_as_number(jp);
		if (gw != NULL) {
			if (inet_pton(AF_INET, gw, &a) != 1) {
				json_free(root);
				return -1;
			}
			r.gateway_be = a.s_addr;
		}
		if (r.prefix < 0 || r.prefix > 32) {
			json_free(root);
			return -1;
		}
		g_routes[g_count++] = r;
	}
	json_free(root);
	return 0;
}

static int route_find(uint32_t dest_be, int prefix, uint32_t gateway_be)
{
	int i;

	for (i = 0; i < g_count; i++)
		if (g_routes[i].dest_be == dest_be && g_routes[i].prefix == prefix &&
		    g_routes[i].gateway_be == gateway_be)
			return i;
	return -1;
}

int routeconfig_add(uint32_t dest_be, int prefix, uint32_t gateway_be, int applied)
{
	int i = route_find(dest_be, prefix, gateway_be);

	if (i >= 0) {
		g_routes[i].applied = applied;
		return 0;
	}
	if (g_count >= ROUTECONFIG_MAX)
		return -1;
	g_routes[g_count].dest_be = dest_be;
	g_routes[g_count].prefix = prefix;
	g_routes[g_count].gateway_be = gateway_be;
	g_routes[g_count].applied = applied;
	g_count++;
	if (save_state() != 0) {
		g_count--;
		return -1;
	}
	return 0;
}

int routeconfig_del(uint32_t dest_be, int prefix, uint32_t gateway_be)
{
	int i, removed = 0;

	/* No gateway in the request matches any gateway, as the kernel's
	 * own delete does: otherwise DELETE {dest,prefix} would remove the
	 * route from the kernel and leave it kept, and the next pass would
	 * put it back. */
	for (i = 0; i < g_count;) {
		if (g_routes[i].dest_be == dest_be && g_routes[i].prefix == prefix &&
		    (gateway_be == 0 || g_routes[i].gateway_be == gateway_be)) {
			memmove(&g_routes[i], &g_routes[i + 1],
			        (size_t)(g_count - i - 1) * sizeof(g_routes[0]));
			g_count--;
			removed = 1;
			continue;
		}
		i++;
	}
	if (removed)
		(void)save_state();
	return removed;
}

int routeconfig_apply(int *changed)
{
	int i, fd = -1, waiting = 0;

	*changed = 0;

	for (i = 0; i < g_count; i++) {
		struct route_saved *r = &g_routes[i];
		char dest[INET_ADDRSTRLEN], gw[INET_ADDRSTRLEN];

		/* Every kept route, every pass: an add the kernel already has
		 * answers EEXIST, and one it dropped (its interface went down)
		 * goes back in. */
		int added;

		if (fd < 0 && (fd = rtnl_open()) < 0)
			return g_count;
		added = rtnl_route_add_ipv4(fd, r->dest_be, r->prefix, r->gateway_be) == 0;
		if (!added && errno != EEXIST) {
			if (r->applied) {
				logstore_write("cixd", "warn",
				               "a kept route is no longer accepted by the kernel (%s) -- "
				               "retrying until its gateway is reachable (ADR-0387)",
				               strerror(errno));
				*changed = 1;
			}
			r->applied = 0;
			waiting++;
			continue; /* the gateway is not reachable yet: try again next pass */
		}
		if (added)
			*changed = 1; /* the kernel's table moved: it lacked the route */
		if (r->applied)
			continue;
		r->applied = 1;
		*changed = 1;
		ip_str(r->dest_be, dest, sizeof(dest));
		ip_str(r->gateway_be, gw, sizeof(gw));
		logstore_write("cixd", "info", "route %s/%d via %s is in the kernel (ADR-0387)", dest,
		               r->prefix, r->gateway_be != 0 ? gw : "on-link");
	}
	if (fd >= 0)
		rtnl_close(fd);
	return waiting;
}

void routeconfig_write_json(struct json_writer *w)
{
	char ip[INET_ADDRSTRLEN];
	int i;

	jw_arr_open(w);
	for (i = 0; i < g_count; i++) {
		jw_obj_open(w);
		jw_key(w, "dest");
		ip_str(g_routes[i].dest_be, ip, sizeof(ip));
		jw_str(w, ip);
		jw_key(w, "prefix");
		jw_int(w, g_routes[i].prefix);
		jw_key(w, "gateway");
		if (g_routes[i].gateway_be != 0) {
			ip_str(g_routes[i].gateway_be, ip, sizeof(ip));
			jw_str(w, ip);
		} else {
			jw_null(w);
		}
		jw_key(w, "state");
		jw_str(w, g_routes[i].applied ? "applied" : "waiting");
		jw_obj_close(w);
	}
	jw_arr_close(w);
}
