#include "api_sysctl.h"

#include "apiresp.h"
#include "http.h"
#include "json.h"
#include "sysctlconfig.h"
#include "internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * ADR-0160: GET/PUT/DELETE /v1/system/sysctl/{key}, GET /v1/system/sysctl
 * -- host-level /proc/sys REST surface, live, fully open passthrough
 * (host-auth write-gating is the only access control -- no allowlist,
 * confirmed with the user, despite the real blast radius some vm./
 * kernel. keys carry). Key translation (dots -> slashes) reuses
 * container_net_apply_sysctl() (src/container_net.c) directly for
 * writes -- the exact same scheme the existing per-container `sysctl`
 * field already uses, this daemon's own root netns instead of a
 * container's; that function only ever writes, so a small local read
 * counterpart mirrors its translation for GET.
 */
static int read_proc_sysctl(const char *key, char *out, size_t out_size)
{
	char path[16 + SYSCTL_KEY_MAX];
	char *p;
	int fd;
	ssize_t n;

	if (snprintf(path, sizeof(path), "/proc/sys/%s", key) >= (int)sizeof(path))
		return -1;
	for (p = path; *p != '\0'; p++) {
		if (*p == '.')
			*p = '/';
	}

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, out, out_size - 1);
	close(fd);
	if (n < 0)
		return -1;
	out[n] = '\0';
	/* /proc/sys values are conventionally newline-terminated -- trimmed
	 * so a single-token value's own JSON string doesn't carry a
	 * trailing "\n" nothing else in this project's JSON output does. */
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return 0;
}

/* Normalizes a PUT body's "value" field -- a plain JSON string, or a
 * JSON array of strings (joined with a single space, the real
 * kernel's own separator convention for every known tuple-shaped
 * sysctl) -- into the one canonical string form both the live write
 * and (if persisted) sysctlconfig_set() use. */
static int sysctl_value_from_json(const struct json_value *jval, char *out, size_t out_size)
{
	if (jval == NULL)
		return -1;
	if (jval->type == JSON_STRING) {
		const char *s = json_as_string(jval);

		if (s == NULL || strlen(s) >= out_size)
			return -1;
		snprintf(out, out_size, "%s", s);
		return 0;
	}
	if (jval->type == JSON_ARRAY) {
		size_t i;
		size_t len = 0;

		out[0] = '\0';
		for (i = 0; i < jval->u.array.count; i++) {
			const char *tok = json_as_string(jval->u.array.items[i]);
			size_t tok_len;

			if (tok == NULL)
				return -1;
			tok_len = strlen(tok);
			if (len + (i > 0 ? 1 : 0) + tok_len >= out_size)
				return -1;
			if (i > 0)
				out[len++] = ' ';
			memcpy(out + len, tok, tok_len);
			len += tok_len;
		}
		out[len] = '\0';
		return 0;
	}
	return -1;
}

void handle_sysctl_get(int fd, const char *key)
{
	char value[SYSCTL_VALUE_MAX];
	struct json_writer w;

	if (!sysctl_key_is_valid(key)) {
		respond_error(fd, 400, "Bad Request", "invalid sysctl key");
		return;
	}
	if (read_proc_sysctl(key, value, sizeof(value)) != 0) {
		respond_error(fd, 404, "Not Found", "no such sysctl key");
		return;
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "key");
	jw_str(&w, key);
	jw_key(&w, "value");
	sysctl_value_write_json(value, &w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_sysctl_put(int fd, const char *key, const char *body, size_t body_len)
{
	struct json_value *root;
	char value[SYSCTL_VALUE_MAX];
	int persist = 1;
	const struct json_value *jpersist;

	if (!sysctl_key_is_valid(key)) {
		respond_error(fd, 400, "Bad Request", "invalid sysctl key");
		return;
	}

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	if (sysctl_value_from_json(json_object_get(root, "value"), value, sizeof(value)) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "value (string or array of strings) is required");
		return;
	}
	jpersist = json_object_get(root, "persist");
	if (jpersist != NULL && jpersist->type == JSON_BOOL)
		persist = jpersist->u.boolean;
	json_free(root);

	if (container_net_apply_sysctl(key, value) != 0) {
		respond_error(fd, 400, "Bad Request", "the kernel rejected this key/value");
		return;
	}

	if (persist) {
		enum sysctlconfig_error serr = sysctlconfig_set(key, value);

		if (serr != SYSCTLCONFIG_OK) {
			/* The live write already succeeded -- there is no clean way
			 * to "unwrite" a sysctl, and the live value is now correct
			 * regardless of whether it ends up remembered for next
			 * boot, so this is reported, not rolled back. */
			respond_error(fd, 500, "Internal Server Error",
			              "sysctl applied live but could not be persisted");
			return;
		}
	}

	{
		char out_value[SYSCTL_VALUE_MAX];
		struct json_writer w;

		if (read_proc_sysctl(key, out_value, sizeof(out_value)) != 0)
			snprintf(out_value, sizeof(out_value), "%s", value);
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "key");
		jw_str(&w, key);
		jw_key(&w, "value");
		sysctl_value_write_json(out_value, &w);
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

void handle_sysctl_delete(int fd, const char *key)
{
	enum sysctlconfig_error serr;

	if (!sysctl_key_is_valid(key)) {
		respond_error(fd, 400, "Bad Request", "invalid sysctl key");
		return;
	}
	serr = sysctlconfig_delete(key);
	if (serr == SYSCTLCONFIG_ERR_NOT_FOUND) {
		respond_error(fd, 404, "Not Found", "no persisted sysctl entry for this key");
		return;
	}
	if (serr != SYSCTLCONFIG_OK) {
		respond_error(fd, 500, "Internal Server Error", "could not persist removal");
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

void handle_sysctl_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "sysctls");
	sysctlconfig_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

