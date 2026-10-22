/*
 * ADR-0350: the REST surface of the trust-anchor store. The store and
 * its validation are trustanchor.c; this file only speaks HTTP. A
 * change is audited by dispatch() and announced through
 * listTrustAnchors (ADR-0339), so neither happens here.
 */
#include "api_trust.h"

#include "apiresp.h"
#include "http.h"
#include "json.h"
#include "logstore.h"
#include "trustanchor.h"

#include <stdio.h>
#include <string.h>

void handle_trust_anchor_list(int fd)
{
	struct json_writer w;
	int i;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "anchors");
	jw_arr_open(&w);
	for (i = 0; i < trustanchor_count(); i++)
		trustanchor_write_json(&w, trustanchor_get(i), 0);
	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_trust_anchor_add(int fd, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const char *pem;
	char fp[65], err[512];
	enum trustanchor_result rc;
	struct json_writer w;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	pem = json_as_string(json_object_get(root, "pem"));
	if (pem == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "pem is required");
		return;
	}
	rc = trustanchor_add(pem, strlen(pem), fp, err, sizeof(err));
	json_free(root);
	switch (rc) {
	case TRUSTANCHOR_OK:
		break;
	case TRUSTANCHOR_DUPLICATE:
	case TRUSTANCHOR_FULL:
		respond_error(fd, 409, "Conflict", err);
		return;
	case TRUSTANCHOR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", err);
		return;
	default:
		respond_error(fd, 400, "Bad Request", err);
		return;
	}
	logstore_write("cixd", "info", "trust anchors: registered %s (%s)%s (ADR-0350)", fp,
	               trustanchor_find(fp)->info.subject,
	               trustanchor_bundle() == NULL
	                   ? " -- but the combined bundle is not in force; see the error above"
	                   : "");
	jw_init(&w);
	trustanchor_write_json(&w, trustanchor_find(fp), 0);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

void handle_trust_anchor_remove(int fd, const char *fingerprint)
{
	switch (trustanchor_remove(fingerprint)) {
	case TRUSTANCHOR_OK:
		logstore_write("cixd", "info", "trust anchors: removed %s (ADR-0350)", fingerprint);
		http_set_blocking(fd);
		http_write_response(fd, 204, "No Content", "application/json", "", 0);
		return;
	case TRUSTANCHOR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no trust anchor with this fingerprint");
		return;
	default:
		respond_error(fd, 500, "Internal Server Error", "could not persist the store");
		return;
	}
}
