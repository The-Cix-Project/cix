#include "api_logs.h"

#include "apiresp.h"
#include "apiroute.h"
#include "daemonpaths.h"
#include "http.h"
#include "json.h"
#include "logread.h"
#include "logstore.h"

#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* GET /v1/system/kmsg?tail=N -- tail of the kernel ring buffer (/dev/kmsg).
 * The only kernel-log window a shell-less installed host has: dmesg-class
 * diagnostics (mount failures, driver probes, OOM) over the REST API, the same
 * spirit as /v1/system/logs but for the KERNEL's own messages, not cixd's.
 * Single-threaded event loop, so a static ring buffer is safe. ADR-0179 phase
 * 2c needed this to read overlayfs's own "mounting read-only" pr_warn on the
 * shell-less .95 box. */
void handle_kmsg(int fd, const struct http_request *req)
{
	static struct {
		long long ts;
		int prio;
		char text[256];
	} ring[512];
	struct json_writer w;
	char rbuf[8192], tail_str[16];
	int kfd, count = 0, head = 0, tail = 200, emit, start, i;
	ssize_t n;

	if (url_query_param(req->path, "tail", tail_str, sizeof(tail_str)) == 0) {
		tail = atoi(tail_str);
		if (tail < 1)
			tail = 1;
		if (tail > 512)
			tail = 512;
	}

	kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (kfd < 0) {
		respond_error(fd, 500, "Internal Server Error", "cannot open /dev/kmsg");
		return;
	}
	for (;;) {
		char *semi, *msgtext, *nl;
		int prio = 0;
		long long seq = 0, ts = 0;

		n = read(kfd, rbuf, sizeof(rbuf) - 1);
		if (n < 0) {
			if (errno == EPIPE) /* ring overwritten mid-read; skip ahead */
				continue;
			break; /* EAGAIN = drained, or a real error */
		}
		if (n == 0)
			break;
		rbuf[n] = '\0';
		/* Record header is "prio,seq,ts_usec,flags;message[\n continuation]". */
		sscanf(rbuf, "%d,%lld,%lld", &prio, &seq, &ts);
		semi = strchr(rbuf, ';');
		msgtext = (semi != NULL) ? semi + 1 : rbuf;
		nl = strchr(msgtext, '\n');
		if (nl != NULL)
			*nl = '\0';
		ring[head].ts = ts;
		ring[head].prio = prio;
		snprintf(ring[head].text, sizeof(ring[head].text), "%s", msgtext);
		head = (head + 1) % 512;
		if (count < 512)
			count++;
	}
	close(kfd);

	emit = (count < tail) ? count : tail;
	start = (head - emit + 512) % 512;
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "entries");
	jw_arr_open(&w);
	for (i = 0; i < emit; i++) {
		int idx = (start + i) % 512;

		jw_obj_open(&w);
		jw_key(&w, "ts_usec");
		jw_int(&w, ring[idx].ts);
		jw_key(&w, "priority");
		jw_int(&w, ring[idx].prio);
		jw_key(&w, "message");
		jw_str(&w, ring[idx].text);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * ADR-0342 (#612): a read with no filter is answered here, at O(tail); a
 * filtered one is asked of the broker, which parks this request while a
 * helper child reads the store, and serves it again with the result.
 */
void handle_logs_get(int fd, const struct http_request *req)
{
	struct logread_query q;
	struct json_writer w;
	int rfd;

	if (logread_query_parse(req->path, &q) != 0) {
		respond_error(fd, 400, "Bad Request", "invalid regex pattern");
		return;
	}
	if (logread_query_bounded(&q)) {
		jw_init(&w);
		logread_run(&q, &w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
		return;
	}

	switch (logread_ask(&q, &rfd)) {
	case LOGREAD_PARKED:
		return;
	case LOGREAD_BUSY:
		respond_error_hdrs(fd, 503, "Service Unavailable", "Retry-After: 1\r\n",
		                   "log reads are busy -- try again shortly", 0);
		return;
	case LOGREAD_FAILED:
		respond_error(fd, 500, "Internal Server Error", "the log read ended without a result");
		return;
	case LOGREAD_READY:
		break;
	}
	respond_json_from_fd(fd, rfd, "the log read");
}

void handle_logs_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "max_bytes");
	jw_int(&w, logstore_max_bytes());
	jw_key(&w, "min_level");
	jw_str(&w, logstore_min_level());
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Both fields are optional and independent -- a caller changing only
 * max_bytes doesn't need to resupply min_level and vice versa (each
 * setter validates/persists on its own, matching every other partial-
 * update PUT in this daemon, e.g. daemon-config's "only the fields
 * given are touched" convention). At least one of the two is
 * required, or this is a no-op PUT that would silently succeed
 * without changing anything.
 */
void handle_logs_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jmax, *jlevel;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jmax = json_object_get(root, "max_bytes");
	jlevel = json_object_get(root, "min_level");
	if (jmax == NULL && jlevel == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "max_bytes and/or min_level required");
		return;
	}

	if (jmax != NULL) {
		enum logstore_error lerr = logstore_set_max_bytes((int64_t)json_as_number(jmax));

		if (lerr != LOGSTORE_OK) {
			json_free(root);
			respond_error(fd, lerr == LOGSTORE_ERR_INVALID_MAX_BYTES ? 400 : 500,
			              lerr == LOGSTORE_ERR_INVALID_MAX_BYTES ? "Bad Request"
			                                                     : "Internal Server Error",
			              lerr == LOGSTORE_ERR_INVALID_MAX_BYTES
			                  ? "max_bytes out of range"
			                  : "log config could not be persisted");
			return;
		}
	}
	if (jlevel != NULL) {
		enum logstore_error lerr = logstore_set_min_level(json_as_string(jlevel));

		if (lerr != LOGSTORE_OK) {
			json_free(root);
			respond_error(fd, lerr == LOGSTORE_ERR_INVALID_MIN_LEVEL ? 400 : 500,
			              lerr == LOGSTORE_ERR_INVALID_MIN_LEVEL ? "Bad Request"
			                                                     : "Internal Server Error",
			              lerr == LOGSTORE_ERR_INVALID_MIN_LEVEL
			                  ? "min_level must be one of emerg/alert/crit/err(or)/warning(warn)/"
			                    "notice/info/debug"
			                  : "log config could not be persisted");
			return;
		}
	}
	json_free(root);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "max_bytes");
	jw_int(&w, logstore_max_bytes());
	jw_key(&w, "min_level");
	jw_str(&w, logstore_min_level());
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}
