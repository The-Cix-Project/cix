#include "api_kmod.h"

#include "apiresp.h"
#include "http.h"
#include "json.h"
#include "kmod.h"
#include "kmodconfig.h"

#include <stdio.h>
#include <string.h>

/*
 * ADR-0159 Phase A: POST/DELETE/GET /v1/system/kmod/{name} (real
 * modprobe/modinfo, see kmod.c) and GET /v1/system/kmod (a live
 * /proc/modules dump). A modprobe/modinfo failure -- overwhelmingly
 * "no such module" in practice, the one case a bare exit code can't
 * usefully distinguish from "bad option" or "tool not staged" -- is
 * reported as 404 uniformly across all three, rather than guessing at
 * a finer-grained status from output this project never parses for
 * that purpose.
 */
void handle_kmod_post(int fd, const char *name, const char *body, size_t body_len)
{
	char options[KMOD_OPTIONS_MAX];
	struct json_value *root = NULL;
	const struct json_value *joptions = NULL;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		joptions = json_object_get(root, "options");
	}

	if (joptions != NULL) {
		if (kmod_options_from_json(joptions, options, sizeof(options)) != 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request",
			              "options must be an object of string values");
			return;
		}
	} else {
		const char *def = kmodconfig_get_options(name);

		snprintf(options, sizeof(options), "%s", def != NULL ? def : "");
	}
	json_free(root);

	if (kmod_load(name, options) != 0) {
		respond_error(fd, 404, "Not Found", "modprobe could not load this module");
		return;
	}

	{
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, name);
		jw_key(&w, "options");
		kmod_options_write_json(options, &w);
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

void handle_kmod_delete(int fd, const char *name)
{
	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}
	if (kmod_unload(name) != 0) {
		respond_error(fd, 404, "Not Found", "modprobe -r could not unload this module");
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

void handle_kmod_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "modules");
	kmod_write_json_loaded(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_kmod_get(int fd, const char *name)
{
	struct json_writer w;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}
	jw_init(&w);
	if (kmod_write_json_info(name, &w) != 0) {
		jw_free(&w);
		respond_error(fd, 404, "Not Found", "no such module (not built/available)");
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void respond_kmodconfig_error(int fd, enum kmodconfig_error kerr)
{
	switch (kerr) {
	case KMODCONFIG_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid module name, or options too long");
		break;
	case KMODCONFIG_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "kmod config table full");
		break;
	case KMODCONFIG_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no persisted kmod config for this module");
		break;
	case KMODCONFIG_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "kmod config operation failed");
		break;
	}
}

void handle_kmodconfig_put(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *joptions;
	const struct json_value *jautoload;
	char options[KMOD_OPTIONS_MAX];
	const char *options_ptr = NULL;
	int has_autoload = 0;
	int autoload_value = 0;
	enum kmodconfig_error kerr;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	joptions = json_object_get(root, "default_options");
	if (joptions != NULL) {
		if (kmod_options_from_json(joptions, options, sizeof(options)) != 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request",
			              "default_options must be an object of string values");
			return;
		}
		options_ptr = options;
	}

	jautoload = json_object_get(root, "autoload");
	if (jautoload != NULL) {
		if (jautoload->type != JSON_BOOL) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "autoload must be a boolean");
			return;
		}
		has_autoload = 1;
		autoload_value = jautoload->u.boolean;
	}
	json_free(root);

	if (options_ptr == NULL && !has_autoload) {
		respond_error(fd, 400, "Bad Request",
		              "at least one of default_options/autoload is required");
		return;
	}

	kerr = kmodconfig_set(name, options_ptr, has_autoload, autoload_value);
	if (kerr != KMODCONFIG_OK) {
		respond_kmodconfig_error(fd, kerr);
		return;
	}

	{
		struct json_writer w;

		jw_init(&w);
		kmodconfig_write_json_one(name, &w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

void handle_kmodconfig_delete(int fd, const char *name)
{
	enum kmodconfig_error kerr;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}
	kerr = kmodconfig_delete(name);
	if (kerr != KMODCONFIG_OK) {
		respond_kmodconfig_error(fd, kerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

void handle_kmodconfig_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "kmod_config");
	kmodconfig_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

