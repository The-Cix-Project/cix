#include "logread.h"

#include "apiroute.h"

/* The same TCC/<regex.h> arrangement logstore.c uses: the header's own
 * branch for a compiler without VLA support, which TCC is. */
#define __STDC_NO_VLA__ 1
#include <regex.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int logread_query_parse(const char *path, struct logread_query *q)
{
	char num[32];

	memset(q, 0, sizeof(*q));
	q->has_source = url_query_param(path, "source", q->source, sizeof(q->source)) == 0;
	q->has_level = url_query_param(path, "level", q->level, sizeof(q->level)) == 0;
	q->has_container = url_query_param(path, "container", q->container, sizeof(q->container)) == 0;
	if (url_query_param(path, "tail", num, sizeof(num)) == 0)
		q->limit = atoi(num);
	if (url_query_param(path, "since", num, sizeof(num)) == 0)
		q->since = (int64_t)atoll(num);
	if (url_query_param(path, "regex", q->regex, sizeof(q->regex)) == 0) {
		/* Compiled here, not only inside logstore_tail_ex(): a
		 * malformed pattern is a real client mistake (400), not
		 * something to silently match nothing for -- and a filtered
		 * read is run in a child, which can only say it failed. */
		regex_t re;

		if (regcomp(&re, q->regex, REG_EXTENDED | REG_NOSUB | REG_ICASE) != 0)
			return -1;
		regfree(&re);
		q->has_regex = 1;
	}
	return 0;
}

int logread_query_bounded(const struct logread_query *q)
{
	/* An empty regex is no filter (logstore_tail_ex()), and since <= 0
	 * is "from the beginning". Every other filter can match little, and
	 * a read that finds fewer than `tail` matches reads every segment. */
	return !q->has_source && !q->has_level && !q->has_container &&
	       !(q->has_regex && q->regex[0] != '\0') && q->since <= 0;
}

void logread_run(const struct logread_query *q, struct json_writer *w)
{
	logstore_tail_ex(q->has_source ? q->source : NULL, q->has_level ? q->level : NULL,
	                 q->has_container ? q->container : NULL, q->has_regex ? q->regex : NULL,
	                 q->since, q->limit, w);
}

int logread_write_to(const struct logread_query *q, int fd)
{
	struct json_writer w;
	size_t off = 0;
	int rc = 0;

	jw_init(&w);
	logread_run(q, &w);
	while (off < w.len) {
		ssize_t n = write(fd, w.buf + off, w.len - off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			rc = -1;
			break;
		}
		off += (size_t)n;
	}
	jw_free(&w);
	return rc;
}

static logread_broker_fn g_broker;

void logread_set_broker(logread_broker_fn fn)
{
	g_broker = fn;
}

enum logread_answer logread_ask(const struct logread_query *q, int *result_fd)
{
	*result_fd = -1;
	if (g_broker == NULL)
		return LOGREAD_BUSY;
	return g_broker(q, result_fd);
}
