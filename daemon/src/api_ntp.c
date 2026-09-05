#include "api_ntp.h"

#include "apiresp.h"
#include "http.h"
#include "json.h"
#include "ntp.h"

#include <stdio.h>
#include <string.h>

/*
 * GET/PUT /v1/system/ntp: the host's own upstream NTP server address
 * list (task #751), the exact same shape GET/PUT /v1/system/resolv
 * already has for DNS -- PUT {"upstream": [...]} replaces the full
 * list, an empty array clears it (the host then relies solely on any
 * registered server containers, if any -- see /v1/ntp/servers below).
 * Sync status (last attempt's outcome) is a separate resource, GET
 * /v1/system/ntp/status -- config and live job state are two
 * different concerns, the same split daemon_config/swap-status and
 * diskrole/format-status already keep.
 */
void handle_ntp_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	ntp_write_json_config(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void respond_ntp_error(int fd, enum ntp_error err)
{
	switch (err) {
	case NTP_ERR_INVALID_IP:
		respond_error(fd, 400, "Bad Request", "upstream must be valid IPv4 addresses");
		break;
	case NTP_ERR_TOO_MANY: {
		char msg[64];

		snprintf(msg, sizeof(msg), "too many upstream addresses (max %d)", NTP_MAX_UPSTREAM);
		respond_error(fd, 400, "Bad Request", msg);
		break;
	}
	case NTP_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "could not persist ntp.conf");
		break;
	case NTP_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "this container is already registered");
		break;
	case NTP_ERR_FULL:
		respond_error(fd, 400, "Bad Request", "too many registered NTP servers");
		break;
	case NTP_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such registration");
		break;
	case NTP_ERR_CONTAINER_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such container");
		break;
	case NTP_ERR_CONTAINER_NOT_RUNNING:
		respond_error(fd, 404, "Not Found", "no such running container");
		break;
	case NTP_ERR_INVALID_TIME:
		respond_error(fd, 400, "Bad Request", "invalid unixtime, or clock_settime() failed");
		break;
	case NTP_OK:
		break;
	}
}

void handle_ntp_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *arr;
	const char *upstream[NTP_MAX_UPSTREAM];
	int count;
	size_t i;
	enum ntp_error nerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	arr = json_object_get(root, "upstream");
	if (arr == NULL || arr->type != JSON_ARRAY) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "upstream (array) is required");
		return;
	}
	if (arr->u.array.count > NTP_MAX_UPSTREAM) {
		json_free(root);
		respond_ntp_error(fd, NTP_ERR_TOO_MANY);
		return;
	}
	count = (int)arr->u.array.count;
	for (i = 0; i < arr->u.array.count; i++) {
		upstream[i] = json_as_string(arr->u.array.items[i]);
		if (upstream[i] == NULL) {
			json_free(root);
			respond_ntp_error(fd, NTP_ERR_INVALID_IP);
			return;
		}
	}

	nerr = ntp_set_upstream(upstream, count);
	json_free(root);
	if (nerr != NTP_OK) {
		respond_ntp_error(fd, nerr);
		return;
	}

	jw_init(&w);
	ntp_write_json_config(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_ntp_status_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	ntp_write_json_status(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}



/* POST/GET/DELETE /v1/ntp/servers (task #753): registering a running
 * container as an internal NTP time source, mirroring dns_server_
 * create/list/delete's own REST shape exactly -- see ntp.h's own
 * header comment for why no pid/pidfd is needed here (pure
 * bookkeeping, unlike DNS/LDAP server registration). */
void handle_ntp_server_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *container_name;
	enum ntp_error nerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	container_name = json_as_string(json_object_get(root, "container"));
	if (container_name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "container missing");
		return;
	}

	nerr = ntp_server_register(container_name);
	if (nerr != NTP_OK) {
		json_free(root);
		respond_ntp_error(fd, nerr);
		return;
	}

	/* container_name still points into root -- build the response
	 * before freeing it, matching handle_dns_server_create()'s own
	 * use-after-free-avoidance ordering. */
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container_name);
	jw_obj_close(&w);
	json_free(root);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

void handle_ntp_server_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	ntp_server_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_ntp_server_delete(int fd, const char *name)
{
	enum ntp_error nerr = ntp_server_unregister(name);

	if (nerr != NTP_OK) {
		respond_ntp_error(fd, nerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * GET/PUT /v1/system/time (task #752): the host's real current clock,
 * and a manual override -- an operator-facing escape hatch alongside
 * the automatic SNTP sync above, the same "live-apply, immediate
 * effect" relationship GET/PUT /v1/system/resolv already has to real
 * DNS resolution. PUT {"unixtime": N} calls clock_settime() directly.
 */
void handle_time_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	ntp_write_json_time(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_time_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *junixtime;
	int64_t unixtime;
	enum ntp_error nerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	junixtime = json_object_get(root, "unixtime");
	if (junixtime == NULL || junixtime->type != JSON_NUMBER) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "unixtime (number) is required");
		return;
	}
	unixtime = (int64_t)junixtime->u.number;
	json_free(root);

	nerr = ntp_time_set(unixtime);
	if (nerr != NTP_OK) {
		respond_ntp_error(fd, nerr);
		return;
	}

	jw_init(&w);
	ntp_write_json_time(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}
