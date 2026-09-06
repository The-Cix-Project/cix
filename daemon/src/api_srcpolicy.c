#include "api_srcpolicy.h"

#include "apiresp.h"
#include "http.h"
#include "json.h"
#include "namecheck.h"
#include "pkg.h"
#include "srcpolicy.h"
#include "srcresolve.h"
#include "srcupstream.h"

#include <stdio.h>
#include <string.h>

/*
 * ADR-0255: which upstream release a package builds.
 *
 * Every refusal here names the valid options rather than saying no.
 * That is the whole reason channels come from the discovery kind
 * instead of being free text: the answer is knowable, so it should be
 * shown. An operator who reads "does not publish a 'lts' channel; it
 * has mainline, stable, longterm" is done; one who reads "invalid
 * channel" has to go and find out.
 */

/*
 * The catalogue is recomputed here on every read rather than served
 * from a store -- see srcresolve.h for why derived state with no
 * invalidation event is worse than recomputing it.
 */
void handle_source_catalogue_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	srcresolve_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_upstream_kinds_get(int fd)
{
	struct json_writer w;
	size_t i, j;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "upstreams");
	jw_arr_open(&w);
	for (i = 0; i < srcupstream_count(); i++) {
		const struct srcupstream_kind *k = srcupstream_at(i);

		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, k->name);
		jw_key(&w, "channels");
		/*
		 * null, not [], when the project publishes a single linear
		 * sequence. "has no channels" and "has an empty list of
		 * channels" would render identically as [] and only one of them
		 * is ever true -- a caller building a picker needs to tell
		 * "nothing to choose here" from "nothing to choose yet".
		 */
		if (k->channels == NULL) {
			jw_null(&w);
		} else {
			jw_arr_open(&w);
			for (j = 0; k->channels[j] != NULL; j++)
				jw_str(&w, k->channels[j]);
			jw_arr_close(&w);
		}
		jw_key(&w, "summary");
		jw_str(&w, k->summary);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_source_policy_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	srcpolicy_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* Both PUTs answer with the whole policy set, the way setPkgPolicy
 * does: the caller sees the state its change produced rather than
 * having to ask again. */
static void respond_policy_set(int fd)
{
	struct json_writer w;

	jw_init(&w);
	srcpolicy_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_source_policy_default_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *channel;
	const char *depth;
	char err[256];

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	channel = json_as_string(json_object_get(root, "channel"));
	depth = json_as_string(json_object_get(root, "depth"));
	if (srcpolicy_default_set(channel, depth, err, sizeof(err)) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", err);
		return;
	}
	json_free(root);
	respond_policy_set(fd);
}

void handle_pkg_source_policy_put(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *channel;
	const char *depth;
	char kind[PKG_NAME_MAX];
	char err[256];

	if (name == NULL || !simple_name_is_valid(name, PKG_NAME_MAX)) {
		respond_error(fd, 400, "Bad Request", "invalid package name");
		return;
	}
	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	channel = json_as_string(json_object_get(root, "channel"));
	depth = json_as_string(json_object_get(root, "depth"));

	/*
	 * The kind comes from the RECIPE, never from the request. An
	 * operator chooses among what their package's upstream actually
	 * offers; letting the caller name the kind would put the two facts
	 * in two places and let them disagree.
	 *
	 * A package with no recipe resolves to no kind, which is the same
	 * outcome as declaring none: it cannot take a channel, and
	 * srcpolicy_set() says so.
	 */
	if (pkg_recipe_upstream(name, kind, sizeof(kind)) != 0)
		kind[0] = '\0';

	if (srcpolicy_set(name, kind, channel, depth, err, sizeof(err)) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", err);
		return;
	}
	json_free(root);
	respond_policy_set(fd);
}

void handle_pkg_source_policy_delete(int fd, const char *name)
{
	if (name == NULL || !simple_name_is_valid(name, PKG_NAME_MAX)) {
		respond_error(fd, 400, "Bad Request", "invalid package name");
		return;
	}
	/*
	 * 204 whether or not a policy was set, matching
	 * DELETE /pkg/policies/{name}: the caller's intent ("this package
	 * is on the default") is satisfied either way, and a 404 would make
	 * scripts handle a distinction that does not matter.
	 */
	(void)srcpolicy_clear(name);
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}
