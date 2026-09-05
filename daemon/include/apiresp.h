#ifndef APIRESP_H
#define APIRESP_H

/*
 * apiresp -- writing a JSON response onto a client fd.
 *
 * The layer between http.c and the operation handlers. http.c is
 * deliberately JSON-agnostic: it moves bytes and parses requests, and
 * giving it a json_writer dependency would make the transport know
 * about the payload format. The handlers, equally, should not be
 * repeating status-line and content-type detail 900 times over.
 *
 * These three functions were static in main.c, which is what kept every
 * operation handler there with them: respond_error() alone has 722 call
 * sites, so no handler could be moved out of that file while its most
 * common call was file-local. Extracting them is what makes main.c
 * divisible at all -- the handlers are the bulk of it, and this is the
 * only thing they all share.
 *
 * No call site changed. The functions are the same bytes in a different
 * translation unit.
 */
#include "json.h"

/*
 * Writes w's buffer as the response body with an application/json
 * content type.
 *
 * Sets the fd blocking first, which is still correct rather than
 * vestigial: for the in-flight client request the sink installed in
 * main() takes these bytes and no write happens here at all
 * (client_conn_finish() puts the fd back to non-blocking before
 * draining it), while any other fd reaching this path is written
 * straight out and does need blocking mode.
 */
void respond_json(int fd, int status, const char *status_text, struct json_writer *w);

/* The one-line error body this API answers every refusal with:
 * {"error": "<msg>"}. */
void respond_error(int fd, int status, const char *status_text, const char *msg);

/*
 * The reason phrase for a status this daemon actually returns.
 *
 * Deliberately not a complete table: it exists for the handlers that
 * compute a status code and then need its text, and a code not listed
 * here is one nothing returns yet. "Error" rather than a wrong phrase
 * for anything else.
 */
const char *http_status_text(int status);

#endif /* APIRESP_H */
