#include "api_syslog.h"

#include "apiresp.h"
#include "http.h"
#include "json.h"
#include "syslogfwd.h"
#include "registry.h"

#include <stdio.h>
#include <string.h>

static void respond_syslogfwd_error(int fd, enum syslogfwd_error err)
{
	switch (err) {
	case SYSLOGFWD_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "could not persist syslog_targets.json");
		break;
	case SYSLOGFWD_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "this container is already registered");
		break;
	case SYSLOGFWD_ERR_FULL:
		respond_error(fd, 400, "Bad Request", "too many registered syslog forward targets");
		break;
	case SYSLOGFWD_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such registration");
		break;
	case SYSLOGFWD_ERR_CONTAINER_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such container");
		break;
	case SYSLOGFWD_ERR_CONTAINER_NOT_RUNNING:
		respond_error(fd, 404, "Not Found", "no such running container");
		break;
	case SYSLOGFWD_OK:
		break;
	}
}

/* POST/GET/DELETE /v1/syslog/targets (logging epic Part 2, ADR-0127):
 * registering a running container (typically syslog-1/syslog-2, a real
 * syslogd such as sysklogd.recipe) as an external forward target for
 * every container-sourced log line -- mirrors handle_ntp_server_
 * create/list/delete's own REST shape exactly. */
void handle_syslog_target_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *container_name;
	enum syslogfwd_error serr;
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

	serr = syslogfwd_target_register(container_name);
	if (serr != SYSLOGFWD_OK) {
		json_free(root);
		respond_syslogfwd_error(fd, serr);
		return;
	}

	/* container_name still points into root -- build the response
	 * before freeing it, matching handle_ntp_server_create()'s own
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

void handle_syslog_target_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "targets");
	syslogfwd_target_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_syslog_target_delete(int fd, const char *name)
{
	enum syslogfwd_error serr = syslogfwd_target_unregister(name);

	if (serr != SYSLOGFWD_OK) {
		respond_syslogfwd_error(fd, serr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}
