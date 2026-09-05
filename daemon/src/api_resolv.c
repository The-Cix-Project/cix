#include "api_resolv.h"

#include "apiresp.h"
#include "http.h"
#include "json.h"
#include "resolv.h"

#include <stdio.h>
#include <string.h>

/*
 * GET/PUT /v1/system/resolv (ADR-0076): the host's own outbound DNS
 * resolver config. PUT {"nameservers": [...]} replaces the full list
 * and takes effect immediately (resolv_set() rewrites the real,
 * bind-mounted file directly -- no reboot needed); an empty array
 * clears it.
 */
void handle_resolv_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	resolv_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void respond_resolv_error(int fd, enum resolv_error err)
{
	switch (err) {
	case RESOLV_ERR_INVALID_IP:
		respond_error(fd, 400, "Bad Request", "nameservers must be valid IPv4 addresses");
		break;
	case RESOLV_ERR_TOO_MANY: {
		char msg[64];

		snprintf(msg, sizeof(msg), "too many nameservers (max %d)", RESOLV_MAX_NAMESERVERS);
		respond_error(fd, 400, "Bad Request", msg);
		break;
	}
	case RESOLV_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "could not persist resolv.conf");
		break;
	case RESOLV_OK:
		break;
	}
}

void handle_resolv_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *arr;
	const char *nameservers[RESOLV_MAX_NAMESERVERS];
	int count;
	size_t i;
	enum resolv_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	arr = json_object_get(root, "nameservers");
	if (arr == NULL || arr->type != JSON_ARRAY) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "nameservers (array) is required");
		return;
	}
	if (arr->u.array.count > RESOLV_MAX_NAMESERVERS) {
		json_free(root);
		respond_resolv_error(fd, RESOLV_ERR_TOO_MANY);
		return;
	}
	count = (int)arr->u.array.count;
	for (i = 0; i < arr->u.array.count; i++) {
		nameservers[i] = json_as_string(arr->u.array.items[i]);
		if (nameservers[i] == NULL) {
			json_free(root);
			respond_resolv_error(fd, RESOLV_ERR_INVALID_IP);
			return;
		}
	}

	rerr = resolv_set(nameservers, count);
	json_free(root);
	if (rerr != RESOLV_OK) {
		respond_resolv_error(fd, rerr);
		return;
	}

	jw_init(&w);
	resolv_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}
