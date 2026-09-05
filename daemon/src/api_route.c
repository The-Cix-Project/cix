#include "api_route.h"

#include "apiresp.h"
#include "http.h"
#include "json.h"
#include "network.h"
#include "rtnetlink.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <string.h>

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
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* POST /v1/system/routes (ADR-0067 Part 3): adds a real IPv4 route to
 * the host's own kernel routing table via rtnl_route_add_ipv4() --
 * the write-side counterpart to handle_route_list() above. dest/
 * prefix are optional together (omitted or prefix 0 means the
 * default route, matching rtnl_route_add_ipv4()'s own convention);
 * gateway is optional (omitted means a direct/on-link route). Scoped
 * to exactly what that primitive supports -- no RTA_OIF/interface
 * binding, no route replace semantics beyond what NLM_F_CREATE
 * already gives it. Not a persisted Cix resource (see
 * network_write_routes_json()'s own comment) -- nothing here is
 * remembered across a reboot, same as this whole endpoint family. */
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
	if (rtnl_route_add_ipv4(rtfd, dest_be, prefix_len, gateway_be) != 0) {
		rtnl_close(rtfd);
		respond_error(fd, 400, "Bad Request",
		              "kernel rejected the route (already exists, unreachable gateway, or invalid)");
		return;
	}
	rtnl_close(rtfd);

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
	if (rtnl_route_del_ipv4(rtfd, dest_be, prefix_len, gateway_be) != 0) {
		rtnl_close(rtfd);
		respond_error(fd, 404, "Not Found", "no matching route to delete");
		return;
	}
	rtnl_close(rtfd);

	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}
