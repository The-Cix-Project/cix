#include "api_route.h"

#include "apiresp.h"
#include "http.h"
#include "json.h"
#include "network.h"
#include "routeconfig.h"
#include "rtnetlink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <string.h>

/*
 * A DESTINATION WITH NO PREFIX IS REFUSED, NOT IGNORED (#592).
 *
 * Prefix 0 is the default route, and the kernel has no use for a
 * destination there, so this endpoint used to drop `dest` without a
 * word. That turned a forgotten prefix into the most consequential
 * route a host has: `{"dest":"172.40.0.0","gateway":"172.30.1.1"}`
 * was handled as an add of the DEFAULT route via that gateway (read
 * from this file's own code on 2026-10-17, not measured on a host) --
 * and on DELETE, the same body removed the default route. The
 * dashboard's hand-built form guarded against it with a
 * client-side check, which protected the dashboard and nothing else;
 * the rule belongs here, where cixctl and every script get it too.
 *
 * `0.0.0.0` stays accepted with prefix 0, because it IS the default
 * route's destination and saying so explicitly is not a mistake.
 */
#define ROUTE_DEST_NEEDS_PREFIX \
	"dest names a network, so it needs a prefix of 1-32; send neither dest nor prefix " \
	"for the default route"

static int route_dest_is_ignored(const char *dest_str, int prefix_len)
{
	return prefix_len == 0 && dest_str != NULL && strcmp(dest_str, "0.0.0.0") != 0;
}

/* GET /v1/system/routes (ADR-0066): the box's own real kernel IPv4
 * routing table -- see network_write_routes_json()'s own comment for
 * why this exists (no SSH/general shell, the daemon is the only way
 * to ever see this). */
void handle_route_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "routes");
	if (network_write_routes_json(&w) != 0) {
		jw_free(&w);
		respond_error(fd, 500, "Internal Server Error", "failed to read kernel routing table");
		return;
	}
	/* ADR-0387: the routes this host keeps and puts back, and which of
	 * them the kernel has. */
	jw_key(&w, "saved");
	routeconfig_write_json(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* POST /v1/system/routes (ADR-0067 Part 3): adds a real IPv4 route to
 * the host's own kernel routing table via rtnl_route_add_ipv4() --
 * the write-side counterpart to handle_route_list() above. dest/
 * prefix are optional together (omitted or prefix 0 means the
 * default route, matching rtnl_route_add_ipv4()'s own convention,
 * and a dest given without a prefix is refused -- see
 * route_dest_is_ignored());
 * gateway is optional (omitted means a direct/on-link route). Scoped
 * to exactly what that primitive supports -- no RTA_OIF/interface
 * binding, no route replace semantics beyond what NLM_F_CREATE
 * already gives it. Kept since ADR-0387 (#715): routeconfig.c saves
 * every route this adds and puts it back at each start, retrying
 * until its gateway is reachable. Until then an added route lived only
 * in the kernel, and gibsson-1 lost its default route at every reboot
 * (2026-11-21). */
void handle_route_add(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *dest_str = NULL;
	const char *gateway_str = NULL;
	const struct json_value *jprefix;
	int prefix_len = 0;
	struct in_addr dest_addr;
	struct in_addr gateway_addr;
	uint32_t dest_be = 0;
	uint32_t gateway_be = 0;
	int rtfd;

	if (body != NULL && body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		dest_str = json_as_string(json_object_get(root, "dest"));
		gateway_str = json_as_string(json_object_get(root, "gateway"));
		jprefix = json_object_get(root, "prefix");
		if (jprefix != NULL)
			prefix_len = (int)json_as_number(jprefix);
	}

	if (prefix_len < 0 || prefix_len > 32) {
		if (root != NULL)
			json_free(root);
		respond_error(fd, 400, "Bad Request", "prefix must be in [0, 32]");
		return;
	}
	if (route_dest_is_ignored(dest_str, prefix_len)) {
		if (root != NULL)
			json_free(root);
		respond_error(fd, 400, "Bad Request", ROUTE_DEST_NEEDS_PREFIX);
		return;
	}
	if (prefix_len > 0) {
		if (dest_str == NULL || inet_pton(AF_INET, dest_str, &dest_addr) != 1) {
			if (root != NULL)
				json_free(root);
			respond_error(fd, 400, "Bad Request", "dest missing or not a valid IPv4 address");
			return;
		}
		dest_be = dest_addr.s_addr;
	}
	if (gateway_str != NULL) {
		if (inet_pton(AF_INET, gateway_str, &gateway_addr) != 1) {
			if (root != NULL)
				json_free(root);
			respond_error(fd, 400, "Bad Request", "gateway not a valid IPv4 address");
			return;
		}
		gateway_be = gateway_addr.s_addr;
	}
	if (root != NULL)
		json_free(root);

	rtfd = rtnl_open();
	if (rtfd < 0) {
		respond_error(fd, 500, "Internal Server Error", "could not open rtnetlink socket");
		return;
	}
	/*
	 * ADR-0387: an added route is kept, and put back at every start. A
	 * route the kernel already has is the operator asking for what is
	 * there, so it is kept too and answered 204 -- it used to be a 400
	 * that read like a failure (gibsson-1's console, 2026-11-21). Any
	 * other refusal is the kernel's verdict now, keeps nothing, and
	 * names the cause.
	 */
	if (rtnl_route_add_ipv4(rtfd, dest_be, prefix_len, gateway_be) != 0 && errno != EEXIST) {
		char msg[160];
		int err = errno;

		rtnl_close(rtfd);
		snprintf(msg, sizeof(msg), "the kernel refused the route: %s%s", strerror(err),
		         err == ENETUNREACH ? " -- no network this host is on contains the gateway" : "");
		respond_error(fd, 400, "Bad Request", msg);
		return;
	}
	rtnl_close(rtfd);
	if (routeconfig_add(dest_be, prefix_len, gateway_be, 1) != 0) {
		respond_error(fd, 500, "Internal Server Error",
		              "the route is in the kernel, but could not be saved, so it will not "
		              "survive a reboot");
		return;
	}

	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/* DELETE /v1/system/routes: the mirror-image of handle_route_add()
 * above, via the new rtnl_route_del_ipv4(). Same body shape and same
 * default-route convention (prefix 0 or omitted means the default
 * route) identifies which route to remove. */
void handle_route_del(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *dest_str = NULL;
	const char *gateway_str = NULL;
	const struct json_value *jprefix;
	int prefix_len = 0;
	struct in_addr dest_addr;
	struct in_addr gateway_addr;
	uint32_t dest_be = 0;
	uint32_t gateway_be = 0;
	int rtfd;

	if (body != NULL && body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		dest_str = json_as_string(json_object_get(root, "dest"));
		gateway_str = json_as_string(json_object_get(root, "gateway"));
		jprefix = json_object_get(root, "prefix");
		if (jprefix != NULL)
			prefix_len = (int)json_as_number(jprefix);
	}

	if (prefix_len < 0 || prefix_len > 32) {
		if (root != NULL)
			json_free(root);
		respond_error(fd, 400, "Bad Request", "prefix must be in [0, 32]");
		return;
	}
	if (route_dest_is_ignored(dest_str, prefix_len)) {
		if (root != NULL)
			json_free(root);
		respond_error(fd, 400, "Bad Request", ROUTE_DEST_NEEDS_PREFIX);
		return;
	}
	if (prefix_len > 0) {
		if (dest_str == NULL || inet_pton(AF_INET, dest_str, &dest_addr) != 1) {
			if (root != NULL)
				json_free(root);
			respond_error(fd, 400, "Bad Request", "dest missing or not a valid IPv4 address");
			return;
		}
		dest_be = dest_addr.s_addr;
	}
	if (gateway_str != NULL) {
		if (inet_pton(AF_INET, gateway_str, &gateway_addr) != 1) {
			if (root != NULL)
				json_free(root);
			respond_error(fd, 400, "Bad Request", "gateway not a valid IPv4 address");
			return;
		}
		gateway_be = gateway_addr.s_addr;
	}
	if (root != NULL)
		json_free(root);

	rtfd = rtnl_open();
	if (rtfd < 0) {
		respond_error(fd, 500, "Internal Server Error", "could not open rtnetlink socket");
		return;
	}
	{
		/* ADR-0387: forgotten as well, so it does not come back at the
		 * next start; a kept route waiting for its gateway is not in the
		 * kernel, and deleting it is still a success. */
		int in_kernel = rtnl_route_del_ipv4(rtfd, dest_be, prefix_len, gateway_be) == 0;
		int was_kept = routeconfig_del(dest_be, prefix_len, gateway_be);

		rtnl_close(rtfd);
		if (!in_kernel && !was_kept) {
			respond_error(fd, 404, "Not Found", "no matching route to delete");
			return;
		}
	}

	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}
