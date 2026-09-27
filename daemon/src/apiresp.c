#include "apiresp.h"

#include "http.h"

void respond_json(int fd, int status, const char *status_text, struct json_writer *w)
{
	http_set_blocking(fd);
	http_write_response(fd, status, status_text, "application/json", w->buf, w->len);
}

static void error_envelope(struct json_writer *w, const char *msg)
{
	jw_init(w);
	jw_obj_open(w);
	jw_key(w, "error");
	jw_str(w, msg);
	jw_obj_close(w);
}

void respond_error(int fd, int status, const char *status_text, const char *msg)
{
	struct json_writer w;

	error_envelope(&w, msg);
	respond_json(fd, status, status_text, &w);
	jw_free(&w);
}

/*
 * The same error, headers only, for a request that was a HEAD (#498).
 *
 * The envelope is still built -- it is what Content-Length describes,
 * and a HEAD's headers are by definition the ones a GET would carry --
 * and then not sent. Built through the same error_envelope() the real
 * responder uses rather than by a second hand-rolled copy, because the
 * length has to be the one the body would actually have been.
 *
 * Only dispatch() needs this, and only twice: HEAD can reach exactly
 * two responses that are not a static asset -- the authorization
 * gate's 401, and the 404 for a /v1/... path, which every HEAD gets
 * because the API router is keyed on the methods the spec declares and
 * none of them declares HEAD. Nothing else is reachable, so nothing
 * else calls this; a handler would have to be routed a HEAD first, and
 * none is.
 */
void respond_error_head(int fd, int status, const char *status_text, const char *msg)
{
	struct json_writer w;

	error_envelope(&w, msg);
	http_set_blocking(fd);
	http_write_response_head(fd, status, status_text, "application/json", NULL, w.len);
	jw_free(&w);
}

const char *http_status_text(int status)
{
	switch (status) {
	case 400:
		return "Bad Request";
	case 409:
		return "Conflict";
	case 500:
		return "Internal Server Error";
	default:
		return "Error";
	}
}
