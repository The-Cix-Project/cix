/*
 * ADR-0342 (#612): the log store is read backwards, and a read with no
 * filter is told apart from one that may read the whole store.
 *
 * The reader before ADR-0342 read each segment forwards, json_parse()ing
 * every line, and kept the last N in a ring. Its replacement reads from
 * each segment's end and stops as soon as it holds N, so the one thing
 * that must not change is WHICH entries come back and in what order. This
 * test writes segments by hand -- so it controls every ts, including a
 * clock that steps backwards, a line longer than the reader's block, a
 * span of damage with no newline in it, and a last line with no newline
 * -- keeps its own list of what it wrote, and compares the store's answer
 * byte for byte with the answer computed from that list: every filter,
 * every combination this test can name, at several tails.
 *
 * logread_query_parse() and logread_query_bounded() are the other half:
 * the bound decides what is allowed to run on the event loop, so its
 * answer for each query shape is asserted here rather than left to the
 * handler.
 */
#include "json.h"
#include "logread.h"
#include "logstore.h"

#define __STDC_NO_VLA__ 1
#include <regex.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ENTRIES 8000

struct entry {
	long long ts;
	char source[LOGSTORE_SOURCE_MAX];
	char level[LOGSTORE_LEVEL_MAX];
	char container[LOGSTORE_CONTAINER_MAX];
	char msg[LOGSTORE_MSG_MAX];
};

static struct entry g_written[MAX_ENTRIES];
static int g_count;
static char g_dir[256];
static int g_failures;

static void fail(const char *what, const char *detail)
{
	fprintf(stderr, "FAIL: %s%s%s\n", what, detail[0] != '\0' ? " -- " : "", detail);
	g_failures++;
}

/* One entry, written the way write_entry() writes it, and remembered.
 * msg_full may be longer than the store keeps: what is remembered is
 * what a read returns, the first LOGSTORE_MSG_MAX - 1 bytes. */
static void put(FILE *f, long long ts, const char *src, const char *lvl, const char *cont,
                const char *msg_full)
{
	struct json_writer w;
	struct entry *e;

	if (g_count >= MAX_ENTRIES) {
		fail("too many entries for the test's own table", "");
		return;
	}
	e = &g_written[g_count++];
	e->ts = ts;
	snprintf(e->source, sizeof(e->source), "%s", src);
	snprintf(e->level, sizeof(e->level), "%s", lvl);
	snprintf(e->container, sizeof(e->container), "%s", cont);
	snprintf(e->msg, sizeof(e->msg), "%s", msg_full);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "ts");
	jw_int(&w, ts);
	jw_key(&w, "source");
	jw_str(&w, src);
	jw_key(&w, "level");
	jw_str(&w, lvl);
	jw_key(&w, "container");
	jw_str(&w, cont);
	jw_key(&w, "msg");
	jw_str(&w, msg_full);
	jw_obj_close(&w);
	fwrite(w.buf, 1, w.len, f);
	fputc('\n', f);
	jw_free(&w);
}

static FILE *open_segment(unsigned long long seq)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/seg-%020llu.jsonl", g_dir, seq);
	return fopen(path, "w");
}

/*
 * Five segments, oldest first:
 *   1: 3000 ordinary entries, mixed sources, levels and containers;
 *   2: a clock step -- ts goes back 500 s halfway through -- and one
 *      line whose message is 100 KB of quotes, so its escaped line is
 *      200 KB, three of the reader's blocks;
 *   3: empty;
 *   4: damage -- 300 KB with no newline -- between two valid runs, plus
 *      an empty line;
 *   5: the newest, 2500 entries, its last line complete JSON with no
 *      newline after it, preceded by a truncated line.
 * Garbage, the truncated line and the empty line are not remembered:
 * they are not entries, and a read must skip them.
 */
static int build_store(void)
{
	static const char *const sources[] = { "cixd", "kernel", "audit", "container", "events" };
	static const char *const levels[] = { "info", "warn", "error", "debug" };
	static const char *const containers[] = { "", "jump", "dns-1", "ldap-1" };
	FILE *f;
	long long ts = 1792000000;
	char msg[512];
	char *big;
	int i;

	f = open_segment(1);
	if (f == NULL)
		return -1;
	for (i = 0; i < 3000; i++) {
		const char *src = sources[i % 5];

		snprintf(msg, sizeof(msg), "segment one entry %d %s", i, i % 7 == 0 ? "needle" : "hay");
		put(f, ts++, src, levels[i % 4], strcmp(src, "container") == 0 ? containers[1 + i % 3] : "",
		    msg);
	}
	fclose(f);

	f = open_segment(2);
	if (f == NULL)
		return -1;
	for (i = 0; i < 1000; i++) {
		if (i == 500)
			ts -= 500; /* the clock stepped back */
		snprintf(msg, sizeof(msg), "segment two entry %d after%s", i, i >= 500 ? "step" : "none");
		put(f, ts++, sources[i % 5], levels[i % 4], "", msg);
	}
	big = malloc(100 * 1024 + 1);
	if (big == NULL)
		return -1;
	memset(big, '"', 100 * 1024);
	big[100 * 1024] = '\0';
	put(f, ts++, "cixd", "warn", "", big);
	free(big);
	put(f, ts++, "cixd", "info", "", "after the long line");
	fclose(f);

	f = open_segment(3);
	if (f == NULL)
		return -1;
	fclose(f);

	f = open_segment(4);
	if (f == NULL)
		return -1;
	for (i = 0; i < 200; i++) {
		snprintf(msg, sizeof(msg), "segment four before damage %d", i);
		put(f, ts++, "container", "info", containers[1 + i % 3], msg);
	}
	for (i = 0; i < 300 * 1024; i++)
		fputc('x', f);
	fputc('\n', f);
	fputc('\n', f);
	for (i = 0; i < 200; i++) {
		snprintf(msg, sizeof(msg), "segment four after damage %d needle", i);
		put(f, ts++, "kernel", "warn", "", msg);
	}
	fclose(f);

	f = open_segment(5);
	if (f == NULL)
		return -1;
	for (i = 0; i < 2500; i++) {
		const char *src = sources[(i * 3) % 5];

		snprintf(msg, sizeof(msg), "segment five entry %d %s", i, i % 11 == 0 ? "needle" : "hay");
		put(f, ts++, src, levels[i % 4], strcmp(src, "container") == 0 ? containers[1 + i % 3] : "",
		    msg);
	}
	fputs("{\"ts\":1,\"source\":\"cixd\",\"level\":\"info\",\"contai\n", f); /* truncated */
	{
		/* The last entry, with no newline after it: written, then the
		 * newline put() added is taken back off. */
		long end;

		put(f, ts++, "audit", "info", "", "the last line, unterminated");
		fflush(f);
		end = ftell(f);
		if (ftruncate(fileno(f), end - 1) != 0) {
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	return 0;
}

/* What the store must answer: the last `limit` matches, oldest first,
 * rendered as logstore_tail_ex() renders them. */
static void expected(const struct logread_query *q, struct json_writer *w)
{
	int eff = (q->limit > 0 && q->limit <= 5000) ? q->limit : 1000;
	int *keep = malloc(sizeof(int) * (size_t)eff);
	int n = 0, start = 0, i;
	regex_t re;
	int have_re = q->has_regex && q->regex[0] != '\0';

	if (keep == NULL)
		return;
	if (have_re && regcomp(&re, q->regex, REG_EXTENDED | REG_NOSUB | REG_ICASE) != 0)
		have_re = 0;
	for (i = 0; i < g_count; i++) {
		const struct entry *e = &g_written[i];

		if ((q->has_source && strcmp(e->source, q->source) != 0) ||
		    (q->has_level && strcmp(e->level, q->level) != 0) ||
		    (q->has_container && strcmp(e->container, q->container) != 0) ||
		    (have_re && regexec(&re, e->msg, 0, NULL, 0) != 0) || (q->since > 0 && e->ts < q->since))
			continue;
		/* a ring of the last eff matches */
		if (n < eff) {
			keep[n++] = i;
		} else {
			keep[start] = i;
			start = (start + 1) % eff;
		}
	}
	jw_arr_open(w);
	for (i = 0; i < n; i++) {
		const struct entry *e = &g_written[keep[(start + i) % (n < eff ? n : eff)]];

		jw_obj_open(w);
		jw_key(w, "ts");
		jw_int(w, e->ts);
		jw_key(w, "source");
		jw_str(w, e->source);
		jw_key(w, "level");
		jw_str(w, e->level);
		jw_key(w, "container");
		jw_str(w, e->container);
		jw_key(w, "msg");
		jw_str(w, e->msg);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	if (have_re)
		regfree(&re);
	free(keep);
}

static void check_query(const char *path)
{
	struct logread_query q;
	struct json_writer got, want;

	if (logread_query_parse(path, &q) != 0) {
		fail("query did not parse", path);
		return;
	}
	jw_init(&got);
	jw_init(&want);
	logread_run(&q, &got);
	expected(&q, &want);
	if (got.len != want.len || memcmp(got.buf, want.buf, got.len) != 0) {
		char detail[600];

		snprintf(detail, sizeof(detail), "%s: store answered %zu bytes, expected %zu", path,
		         got.len, want.len);
		fail("a read returned different entries from the ones written", detail);
	}
	jw_free(&got);
	jw_free(&want);
}

static void check_bound(const char *path, int want_bounded)
{
	struct logread_query q;

	if (logread_query_parse(path, &q) != 0) {
		fail("query did not parse", path);
		return;
	}
	if (logread_query_bounded(&q) != want_bounded)
		fail(want_bounded ? "a read with no filter was not bounded"
		                  : "a filtered read was treated as bounded, so it would run on the loop",
		     path);
}

/* logread_write_to() writes exactly what logread_run() produces. */
static void check_write_to(const char *path)
{
	char file[300];
	struct logread_query q;
	struct json_writer w;
	struct stat st;
	char *buf;
	int fd;

	snprintf(file, sizeof(file), "%s/result", g_dir);
	fd = open(file, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || logread_query_parse(path, &q) != 0) {
		fail("could not set up logread_write_to()", path);
		if (fd >= 0)
			close(fd);
		return;
	}
	if (logread_write_to(&q, fd) != 0 || fstat(fd, &st) != 0) {
		fail("logread_write_to() failed", path);
		close(fd);
		return;
	}
	jw_init(&w);
	logread_run(&q, &w);
	buf = malloc((size_t)st.st_size + 1);
	if (buf == NULL || pread(fd, buf, (size_t)st.st_size, 0) != st.st_size ||
	    (size_t)st.st_size != w.len || memcmp(buf, w.buf, w.len) != 0)
		fail("logread_write_to() wrote something other than logread_run()'s answer", path);
	free(buf);
	jw_free(&w);
	close(fd);
	unlink(file);
}

int main(void)
{
	static const char *const reads[] = {
		"/v1/system/logs",
		"/v1/system/logs?tail=1",
		"/v1/system/logs?tail=7",
		"/v1/system/logs?tail=300",
		"/v1/system/logs?tail=5000",
		"/v1/system/logs?tail=9000",
		"/v1/system/logs?since=0&tail=300",
		"/v1/system/logs?source=kernel&tail=40",
		"/v1/system/logs?source=events&tail=5000",
		"/v1/system/logs?source=nosuchsource&tail=5",
		"/v1/system/logs?level=error&tail=100",
		"/v1/system/logs?source=container&container=jump&tail=40",
		"/v1/system/logs?container=dns-1&tail=5000",
		"/v1/system/logs?regex=needle&tail=200",
		"/v1/system/logs?regex=NEEDLE&source=kernel&tail=3000",
		"/v1/system/logs?regex=zzznomatchzzz&tail=1",
		"/v1/system/logs?regex=&tail=10",
		"/v1/system/logs?regex=afterstep&tail=1000",
		"/v1/system/logs?since=1792003200&tail=5000",
		"/v1/system/logs?since=1792003200&regex=segment%20two&tail=5000",
		"/v1/system/logs?since=1792007000&tail=40",
		"/v1/system/logs?since=99999999999&tail=40",
		"/v1/system/logs?regex=long%20line&tail=5",
		"/v1/system/logs?source=cixd&level=warn&tail=5",
		"/v1/system/logs?regex=unterminated&tail=5",
	};
	char state[300];
	size_t i;

	snprintf(g_dir, sizeof(g_dir), "/tmp/cix_test_logread_XXXXXX");
	if (mkdtemp(g_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	if (build_store() != 0) {
		fprintf(stderr, "FAIL: could not write the segments\n");
		return 1;
	}
	snprintf(state, sizeof(state), "%s/state.json", g_dir);
	if (logstore_init(g_dir, state) != 0) {
		fprintf(stderr, "FAIL: logstore_init\n");
		return 1;
	}

	for (i = 0; i < sizeof(reads) / sizeof(reads[0]); i++)
		check_query(reads[i]);

	/* The bound: only a read with no filter may run on the loop. */
	check_bound("/v1/system/logs", 1);
	check_bound("/v1/system/logs?tail=5000", 1);
	check_bound("/v1/system/logs?since=0&tail=300", 1);
	check_bound("/v1/system/logs?regex=&tail=10", 1);
	check_bound("/v1/system/logs?source=cixd", 0);
	check_bound("/v1/system/logs?source=", 0);
	check_bound("/v1/system/logs?level=info", 0);
	check_bound("/v1/system/logs?container=jump", 0);
	check_bound("/v1/system/logs?regex=x", 0);
	check_bound("/v1/system/logs?since=1", 0);
	{
		struct logread_query q;

		if (logread_query_parse("/v1/system/logs?regex=%5B", &q) == 0)
			fail("a regex that does not compile was accepted", "[");
	}

	check_write_to("/v1/system/logs?regex=needle&tail=5000");
	check_write_to("/v1/system/logs?tail=3");

	if (g_failures != 0) {
		fprintf(stderr, "test_logread: %d failure(s); store left at %s\n", g_failures, g_dir);
		return 1;
	}
	{
		char cmd[400];

		snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
		if (system(cmd) != 0)
			fprintf(stderr, "note: could not remove %s\n", g_dir);
	}
	printf("test_logread: %zu reads matched what was written, every bound held\n",
	       sizeof(reads) / sizeof(reads[0]));
	return 0;
}
