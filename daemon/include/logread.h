#ifndef LOGREAD_H
#define LOGREAD_H

/*
 * logread -- how a log read is answered without holding the event loop
 * (ADR-0342, #612).
 *
 * The store is read backwards (logstore_tail_ex()), so a read with no
 * filter costs O(tail) and is answered on the loop: that is the
 * dashboard's log panel, sampled in-process every 2 s (ADR-0339). A read
 * with any filter -- source, level, container, a regex, or since > 0 --
 * may have to read every segment, which at the operator's max_bytes can
 * be gigabytes, so it is never run on the loop. It is asked of the
 * broker main.c installs: the request is parked, a helper child runs the
 * read into a memory file (logread_write_to()), and the request is served
 * again from the start with that file attached, when this same ask
 * returns it.
 *
 * Measured on 192.168.15.95, 0.2.57-540, 2026-10-18: an unfiltered
 * `tail=1` took 76 ms and a regex matching nothing took 1533 ms, all of
 * it one pass of the loop.
 *
 * logread_run() is the one caller of logstore_tail_ex(), and is itself
 * called from two places: the handler, for a bounded read, and the
 * child. test_blocking_waits counts both.
 */
#include "json.h"
#include "logstore.h"

#include <stdint.h>

#define LOGREAD_REGEX_MAX 256

struct logread_query {
	char source[LOGSTORE_SOURCE_MAX];
	char level[LOGSTORE_LEVEL_MAX];
	char container[LOGSTORE_CONTAINER_MAX];
	char regex[LOGREAD_REGEX_MAX];
	int has_source;
	int has_level;
	int has_container;
	int has_regex;
	int64_t since;
	int limit;
};

/* Reads the query string of a GET /v1/system/logs path. 0, or -1 when
 * the regex does not compile, which is the client's mistake (400). */
int logread_query_parse(const char *path, struct logread_query *q);

/* 1 when the read has no filter, so its cost is O(tail) and it may run
 * on the loop; 0 when it may have to read the whole store. */
int logread_query_bounded(const struct logread_query *q);

/* Runs the read into w as the response's JSON array. */
void logread_run(const struct logread_query *q, struct json_writer *w);

/* A helper child's work: runs the read and writes the JSON array to fd
 * from its start. 0, or -1 when the write failed. */
int logread_write_to(const struct logread_query *q, int fd);

enum logread_answer {
	LOGREAD_READY,  /* *result_fd holds the response body; the caller closes it */
	LOGREAD_PARKED, /* the request waits; write no response, it will be served again */
	LOGREAD_BUSY,   /* no read can start: answer 503 with Retry-After */
	LOGREAD_FAILED, /* the read ran and produced nothing: answer 500 */
};

typedef enum logread_answer (*logread_broker_fn)(const struct logread_query *q, int *result_fd);

/* main.c installs the broker that parks the request being served. With
 * none installed nothing can run, and the answer is BUSY. */
void logread_set_broker(logread_broker_fn fn);
enum logread_answer logread_ask(const struct logread_query *q, int *result_fd);

#endif /* LOGREAD_H */
