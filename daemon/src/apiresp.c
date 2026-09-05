#include "apiresp.h"

#include "http.h"

void respond_json(int fd, int status, const char *status_text, struct json_writer *w)
{
	http_set_blocking(fd);
	http_write_response(fd, status, status_text, "application/json", w->buf, w->len);
}

void respond_error(int fd, int status, const char *status_text, const char *msg)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "error");
	jw_str(&w, msg);
	jw_obj_close(&w);
	respond_json(fd, status, status_text, &w);
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
