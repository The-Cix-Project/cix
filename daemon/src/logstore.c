#include "logstore.h"
#include "persist.h"

/*
 * TCC can't parse glibc's own <regex.h> regexec() prototype as
 * written -- it declares its regmatch_t array parameter with a C99
 * VLA-in-prototype size expression referencing __nmatch
 * (regmatch_t __pmatch[_Restrict_arr_ _REGEX_NELTS(__nmatch)]), a
 * syntax TCC's simpler parser rejects outright ("__nmatch
 * undeclared", confirmed directly). The header's own _REGEX_NELTS
 * macro already exists precisely for compilers without VLA support
 * (see its own #ifndef __STDC_NO_VLA__ guard) -- defining that
 * standard C11 feature-test macro ourselves, accurately (TCC really
 * doesn't support this construct), makes the header emit a plain
 * `regmatch_t __pmatch[]` parameter instead, which TCC parses fine.
 * Not a hand-replaced struct/prototype (the struct epoll_event/
 * clone_args precedent, include/linux_compat.h) -- glibc's own real
 * declaration is used as-is, just steered onto the branch it already
 * carries for exactly this situation.
 */
#define __STDC_NO_VLA__ 1
#include <regex.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char g_dir[512];
static char g_state_path[512];
static int64_t g_max_bytes = LOGSTORE_DEFAULT_MAX_BYTES;
static char g_min_level[LOGSTORE_LEVEL_MAX] = LOGSTORE_DEFAULT_MIN_LEVEL;
static uint64_t g_next_seq = 1;
static int g_initialized;

/*
 * Real syslog severity order (RFC 5424, the same scale kmsg_level_name()
 * below already names) -- lower number is more severe. "error"/"warn"
 * are accepted as synonyms for "err"/"warning" since cixd/audit's
 * own logstore_write() callers always say "error", never "err".
 * Anything unrecognized ranks as INFO (6) -- permissive by
 * construction, so a typo'd or future level name is never silently
 * dropped entirely.
 */
static int level_rank(const char *level)
{
	static const struct {
		const char *name;
		int rank;
	} levels[] = {
	    {"emerg", 0}, {"alert", 1},   {"crit", 2},  {"err", 3},    {"error", 3},
	    {"warning", 4}, {"warn", 4},  {"notice", 5}, {"info", 6},  {"debug", 7},
	};
	size_t i;

	if (level == NULL)
		return 6;
	for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		if (strcmp(level, levels[i].name) == 0)
			return levels[i].rank;
	}
	return 6;
}

int logstore_level_severity(const char *level)
{
	return level_rank(level);
}

static int g_kmsg_fd = -1;

static FILE *g_current_fp;
static uint64_t g_current_seq;
static long g_current_size;

static int save_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "max_bytes");
	jw_int(&w, g_max_bytes);
	jw_key(&w, "min_level");
	jw_str(&w, g_min_level);
	jw_key(&w, "next_seq");
	jw_int(&w, (long long)g_next_seq);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jmax, *jseq, *jlevel;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted state yet -- defaults already set */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed persisted logstore state\n", g_state_path);
		return -1;
	}

	jmax = json_object_get(root, "max_bytes");
	if (jmax != NULL)
		g_max_bytes = (int64_t)json_as_number(jmax);
	jlevel = json_object_get(root, "min_level");
	if (jlevel != NULL && json_as_string(jlevel) != NULL)
		snprintf(g_min_level, sizeof(g_min_level), "%s", json_as_string(jlevel));
	jseq = json_object_get(root, "next_seq");
	if (jseq != NULL)
		g_next_seq = (uint64_t)json_as_number(jseq);

	json_free(root);
	return 0;
}

static void segment_path(uint64_t seq, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/seg-%020llu.jsonl", g_dir, (unsigned long long)seq);
}

/* Every real segment file currently on disk, ascending by sequence
 * number -- a plain insertion sort, since n is always small
 * (LOGSTORE_SEGMENT_COUNT plus at most a couple of stragglers between
 * a rotation and its own eviction pass). */
static int segment_seq_list(uint64_t *out, int cap)
{
	DIR *d;
	struct dirent *de;
	int n = 0;
	int i, j;

	d = opendir(g_dir);
	if (d == NULL)
		return 0;
	while ((de = readdir(d)) != NULL && n < cap) {
		unsigned long long seq;

		if (sscanf(de->d_name, "seg-%020llu.jsonl", &seq) == 1)
			out[n++] = seq;
	}
	closedir(d);

	for (i = 1; i < n; i++) {
		uint64_t key = out[i];

		j = i - 1;
		while (j >= 0 && out[j] > key) {
			out[j + 1] = out[j];
			j--;
		}
		out[j + 1] = key;
	}
	return n;
}

static void evict_old_segments(void)
{
	uint64_t seqs[LOGSTORE_SEGMENT_COUNT + 4];
	int n = segment_seq_list(seqs, LOGSTORE_SEGMENT_COUNT + 4);
	int i;

	for (i = 0; i < n - LOGSTORE_SEGMENT_COUNT; i++) {
		char path[600];

		segment_path(seqs[i], path, sizeof(path));
		unlink(path);
	}
}

static long per_segment_max_bytes(void)
{
	int64_t v = g_max_bytes / LOGSTORE_SEGMENT_COUNT;

	return v < 1 ? 1 : (long)v;
}

/* Opens (creating if needed) the segment g_current_fp should append
 * to next -- reopens the already-current one on a fresh process (a
 * daemon restart resumes the same segment rather than always
 * starting a new one), rotates to a brand new one once the current
 * segment has grown past its own share of the total cap. */
static int ensure_current_segment_open(void)
{
	char path[600];
	struct stat st;
	int need_new = 0;

	if (g_current_fp == NULL) {
		need_new = (g_next_seq == 1);
	} else if (g_current_size >= per_segment_max_bytes()) {
		fclose(g_current_fp);
		g_current_fp = NULL;
		need_new = 1;
	}

	if (g_current_fp == NULL && !need_new) {
		g_current_seq = g_next_seq - 1;
		segment_path(g_current_seq, path, sizeof(path));
		g_current_fp = fopen(path, "a");
		if (g_current_fp == NULL)
			return -1;
		g_current_size = (stat(path, &st) == 0) ? (long)st.st_size : 0;
	}

	if (need_new) {
		g_current_seq = g_next_seq;
		g_next_seq++;
		if (save_state() != 0)
			return -1;
		segment_path(g_current_seq, path, sizeof(path));
		g_current_fp = fopen(path, "w");
		if (g_current_fp == NULL)
			return -1;
		g_current_size = 0;
		evict_old_segments();
	}
	return 0;
}

void logstore_repoint(const char *new_dir, const char *new_state_path)
{
	if (g_current_fp != NULL) {
		fclose(g_current_fp);
		g_current_fp = NULL;
	}
	snprintf(g_dir, sizeof(g_dir), "%s", new_dir);
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

int logstore_init(const char *dir, const char *state_path)
{
	if (snprintf(g_dir, sizeof(g_dir), "%s", dir) >= (int)sizeof(g_dir))
		return -1;
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	if (persist_mkdir_p(g_dir) != 0)
		return -1;

	g_max_bytes = LOGSTORE_DEFAULT_MAX_BYTES;
	snprintf(g_min_level, sizeof(g_min_level), "%s", LOGSTORE_DEFAULT_MIN_LEVEL);
	g_next_seq = 1;
	if (load_state() != 0)
		return -1;

	g_initialized = 1;
	return 0;
}

static const char *kmsg_level_name(int prio)
{
	static const char *const names[8] = { "emerg", "alert",  "crit",  "err",
		                               "warning", "notice", "info", "debug" };

	if (prio < 0 || prio > 7)
		return "info";
	return names[prio];
}

int logstore_kmsg_fd(void)
{
	if (g_kmsg_fd < 0)
		g_kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	return g_kmsg_fd;
}

void logstore_kmsg_readable(void)
{
	char buf[2048];
	ssize_t n;
	int overran = 0;

	if (g_kmsg_fd < 0)
		return;

	for (;;) {
		n = read(g_kmsg_fd, buf, sizeof(buf) - 1);
		if (n < 0) {
			/*
			 * The three errno cases here mean genuinely different
			 * things, and collapsing them into one `return` lost the
			 * only one that matters (#300).
			 *
			 * EAGAIN is the ordinary end of a drain: nothing new
			 * right now, come back on the next EPOLLIN.
			 *
			 * EINTR is a signal landing mid-read. Nothing was
			 * consumed and nothing was lost, so retry rather than
			 * abandoning the rest of the queue until the next
			 * readiness notification.
			 *
			 * EPIPE is DATA LOSS, and it is the reason this branch
			 * exists. /dev/kmsg hands it back when the record this
			 * reader was positioned at has already been overwritten
			 * in the kernel's ring, and the kernel then advances the
			 * position to the oldest record still present (kernel
			 * Documentation/ABI/testing/dev-kmsg). Reading it as
			 * "nothing new" meant kernel messages vanished from
			 * GET /v1/system/logs with nothing anywhere saying so --
			 * and this reader is drained by the daemon's own event
			 * loop, so it falls behind precisely when the daemon is
			 * busy, which is exactly when the messages worth keeping
			 * are being written. A gap that is recorded is a
			 * diagnosis; a gap that is silent turns log-store
			 * silence into false evidence that nothing was written.
			 *
			 * The position has already moved, so draining continues
			 * rather than returning: the records after the gap are
			 * still there to be read. One entry per drain, however
			 * many records were skipped, because a reader this far
			 * behind would otherwise write a flood of its own into
			 * the store it is trying to preserve.
			 */
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR)
				continue;
			if (errno == EPIPE) {
				if (!overran) {
					overran = 1;
					logstore_write("kernel", "warning",
					               "log store fell behind /dev/kmsg: kernel records were "
					               "overwritten before they could be read, so some entries "
					               "are missing here");
				}
				continue;
			}
			logstore_write("kernel", "err",
			               "log store cannot read /dev/kmsg: %s", strerror(errno));
			return;
		}
		if (n == 0)
			return;
		buf[n] = '\0';

		{
			/* /dev/kmsg record format (kernel's own
			 * Documentation/ABI/testing/dev-kmsg): each read() returns
			 * exactly one record, "<prio>,<seq>,<us>,<flags>;<text>",
			 * optionally followed by "\n KEY=VALUE" continuation
			 * lines this daemon has no use for and simply truncates
			 * away. */
			int prio = 6;
			char *semi = strchr(buf, ';');
			char *msg = semi != NULL ? semi + 1 : buf;
			char *nl = strchr(msg, '\n');

			sscanf(buf, "%d,", &prio);
			if (nl != NULL)
				*nl = '\0';
			logstore_write("kernel", kmsg_level_name(prio & 7), "%s", msg);
		}
	}
}

/* Shared by logstore_write()/logstore_write_container() -- container
 * is "" for every non-container entry (this store's own established
 * convention: empty string means N/A, no jw_null() used anywhere else
 * in this file). */
static void write_entry(const char *source, const char *level, const char *container,
                         const char *msg)
{
	char src[LOGSTORE_SOURCE_MAX];
	char lvl[LOGSTORE_LEVEL_MAX];
	char cont[LOGSTORE_CONTAINER_MAX];
	struct json_writer w;

	if (!g_initialized)
		return;
	snprintf(lvl, sizeof(lvl), "%s", level != NULL ? level : "info");
	if (level_rank(lvl) > level_rank(g_min_level))
		return; /* less severe than the configured floor -- dropped before any write */

	snprintf(src, sizeof(src), "%s", source != NULL ? source : "cixd");
	snprintf(cont, sizeof(cont), "%s", container != NULL ? container : "");

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "ts");
	jw_int(&w, (long long)time(NULL));
	jw_key(&w, "source");
	jw_str(&w, src);
	jw_key(&w, "level");
	jw_str(&w, lvl);
	jw_key(&w, "container");
	jw_str(&w, cont);
	jw_key(&w, "msg");
	jw_str(&w, msg);
	jw_obj_close(&w);

	if (ensure_current_segment_open() == 0 && g_current_fp != NULL) {
		fwrite(w.buf, 1, w.len, g_current_fp);
		fputc('\n', g_current_fp);
		fflush(g_current_fp);
		g_current_size += (long)w.len + 1;
	}
	jw_free(&w);
}

void logstore_write(const char *source, const char *level, const char *fmt, ...)
{
	char msg[LOGSTORE_MSG_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	write_entry(source, level, NULL, msg);
}

void logstore_write_container(const char *container, const char *level, const char *fmt, ...)
{
	char msg[LOGSTORE_MSG_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	write_entry("container", level, container, msg);
}

enum logstore_error logstore_set_max_bytes(int64_t max_bytes)
{
	int64_t prev = g_max_bytes;

	if (max_bytes < LOGSTORE_MIN_MAX_BYTES || max_bytes > LOGSTORE_MAX_MAX_BYTES)
		return LOGSTORE_ERR_INVALID_MAX_BYTES;
	g_max_bytes = max_bytes;
	if (save_state() != 0) {
		g_max_bytes = prev;
		return LOGSTORE_ERR_PERSIST_FAILED;
	}
	return LOGSTORE_OK;
}

int64_t logstore_max_bytes(void)
{
	return g_max_bytes;
}

enum logstore_error logstore_set_min_level(const char *min_level)
{
	char prev[LOGSTORE_LEVEL_MAX];
	static const char *const valid[] = {"emerg",   "alert", "crit",  "err",  "error",
	                                     "warning", "warn",  "notice", "info", "debug"};
	size_t i;
	int ok = 0;

	if (min_level == NULL)
		return LOGSTORE_ERR_INVALID_MIN_LEVEL;
	for (i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
		if (strcmp(min_level, valid[i]) == 0) {
			ok = 1;
			break;
		}
	}
	if (!ok)
		return LOGSTORE_ERR_INVALID_MIN_LEVEL;

	snprintf(prev, sizeof(prev), "%s", g_min_level);
	snprintf(g_min_level, sizeof(g_min_level), "%s", min_level);
	if (save_state() != 0) {
		snprintf(g_min_level, sizeof(g_min_level), "%s", prev);
		return LOGSTORE_ERR_PERSIST_FAILED;
	}
	return LOGSTORE_OK;
}

const char *logstore_min_level(void)
{
	return g_min_level;
}

struct tail_entry {
	int64_t ts;
	char source[LOGSTORE_SOURCE_MAX];
	char level[LOGSTORE_LEVEL_MAX];
	char container[LOGSTORE_CONTAINER_MAX];
	char msg[LOGSTORE_MSG_MAX];
};

/*
 * Rotate the first n entries left by k, in place (#450). Three
 * reversals: reverse [0,k), reverse [k,n), reverse [0,n). Used to
 * straighten a wrapped segment ring without a second allocation.
 */
static void reverse_range(struct tail_entry *a, int lo, int hi)
{
	while (lo < hi) {
		struct tail_entry t = a[lo];

		a[lo++] = a[hi];
		a[hi--] = t;
	}
}

static void rotate_left(struct tail_entry *a, int n, int k)
{
	if (n <= 0 || k <= 0 || k >= n)
		return;
	reverse_range(a, 0, k - 1);
	reverse_range(a, k, n - 1);
	reverse_range(a, 0, n - 1);
}

void logstore_tail(const char *source_filter, const char *level_filter, int64_t since,
                    int limit, struct json_writer *w)
{
	logstore_tail_ex(source_filter, level_filter, NULL, NULL, since, limit, w);
}

void logstore_tail_ex(const char *source_filter, const char *level_filter,
                       const char *container_filter, const char *msg_regex, int64_t since,
                       int limit, struct json_writer *w)
{
	uint64_t seqs[LOGSTORE_SEGMENT_COUNT + 4];
	int seg_count = segment_seq_list(seqs, LOGSTORE_SEGMENT_COUNT + 4);
	int eff_limit = (limit > 0 && limit <= 5000) ? limit : 1000;
	struct tail_entry *ring;
	int ring_head = 0, ring_count = 0;
	/* Filled from the END backwards, because segments are read newest
	 * first (#450). out_start is where the held run begins. */
	int out_start;
	int need;
	int i;
	regex_t re;
	int have_re = 0;

	if (msg_regex != NULL && msg_regex[0] != '\0') {
		if (regcomp(&re, msg_regex, REG_EXTENDED | REG_NOSUB | REG_ICASE) != 0) {
			/* An invalid pattern matches nothing, rather than crashing
			 * or silently ignoring the filter -- the caller (main.c)
			 * already 400s a malformed regex before ever reaching
			 * here; this is just defense in depth. */
			jw_arr_open(w);
			jw_arr_close(w);
			return;
		}
		have_re = 1;
	}

	ring = calloc((size_t)eff_limit, sizeof(*ring));
	if (ring == NULL) {
		if (have_re)
			regfree(&re);
		jw_arr_open(w);
		jw_arr_close(w);
		return;
	}
	out_start = eff_limit;
	need = eff_limit;

	/*
	 * NEWEST SEGMENT FIRST, and stop as soon as the tail is full (#450).
	 *
	 * This used to walk every segment from the oldest, getline() and
	 * json_parse() every line in the whole store, and keep the last N
	 * in a ring. So a `tail=300` paid for the entire log store, every
	 * time. The dashboard's log panel polls
	 * GET /v1/system/logs?since=0&tail=300 every two seconds, and cixd
	 * is one epoll loop, so that walk is a latency floor for every
	 * other client while it runs.
	 *
	 * Measured on 192.168.15.95, 2026-09-13, from the watchdog's own
	 * slow-pass records: 84 of 116 slow passes -- 72% -- named that one
	 * request, with a worst pass of 3162ms. (The remainder are
	 * collateral rather than causes: `GET /v1/health`, which measures
	 * 1ms on its own, appears in the same list, because the activity
	 * field names whichever request was in flight when the loop was
	 * slow.)
	 *
	 * Going backwards is what makes an early stop correct: the newest
	 * segment's last k matches ARE the store's last k matches. Each
	 * segment is asked only for as many as are still needed, and its
	 * results land immediately before what is already held, so the
	 * output stays in chronological order. Older segments are never
	 * opened once the tail is full -- in the common case that is one
	 * segment read instead of eight.
	 *
	 * A filter that matches little still reads everything, which is
	 * correct and unavoidable without an index; it is also not the
	 * case that was hurting anyone.
	 */
	for (i = seg_count - 1; i >= 0 && need > 0; i--) {
		char path[600];
		FILE *fp;
		char *line = NULL;
		size_t line_cap = 0;
		ssize_t n;
		int seg_head = 0, seg_count_matched = 0;
		int j;

		segment_path(seqs[i], path, sizeof(path));
		fp = fopen(path, "r");
		if (fp == NULL)
			continue;

		while ((n = getline(&line, &line_cap, fp)) > 0) {
			struct json_value *root = json_parse(line, (size_t)n);
			const char *src, *lvl, *cont, *msg;
			int64_t ts;
			int idx;

			if (root == NULL)
				continue;
			ts = (int64_t)json_as_number(json_object_get(root, "ts"));
			src = json_as_string(json_object_get(root, "source"));
			lvl = json_as_string(json_object_get(root, "level"));
			cont = json_as_string(json_object_get(root, "container"));
			msg = json_as_string(json_object_get(root, "msg"));

			if ((source_filter != NULL && (src == NULL || strcmp(src, source_filter) != 0)) ||
			    (level_filter != NULL && (lvl == NULL || strcmp(lvl, level_filter) != 0)) ||
			    (container_filter != NULL &&
			     (cont == NULL || strcmp(cont, container_filter) != 0)) ||
			    (have_re && (msg == NULL || regexec(&re, msg, 0, NULL, 0) != 0)) ||
			    (since > 0 && ts < since)) {
				json_free(root);
				continue;
			}

			/* This segment's own ring, sized to what is still
			 * needed, living in the still-unused HEAD of the result
			 * array -- exactly `need` free slots are there by
			 * construction, so no second allocation. */
			if (seg_count_matched == need) {
				idx = seg_head;
				seg_head = (seg_head + 1) % need;
			} else {
				idx = seg_count_matched;
				seg_count_matched++;
			}
			ring[idx].ts = ts;
			snprintf(ring[idx].source, sizeof(ring[idx].source), "%s", src != NULL ? src : "");
			snprintf(ring[idx].level, sizeof(ring[idx].level), "%s", lvl != NULL ? lvl : "");
			snprintf(ring[idx].container, sizeof(ring[idx].container), "%s",
			         cont != NULL ? cont : "");
			snprintf(ring[idx].msg, sizeof(ring[idx].msg), "%s", msg != NULL ? msg : "");
			json_free(root);
		}
		free(line);
		fclose(fp);

		/*
		 * The segment ring only advances seg_head once it is FULL, so
		 * a partial segment (seg_count_matched < need) is already
		 * linear at ring[0..m-1] and seg_head is 0. A full one may be
		 * rotated; straighten it in place with the three-reversal
		 * trick rather than allocating a second buffer -- at the 5000
		 * cap a duplicate of this array would be 21MB.
		 */
		if (seg_head != 0)
			rotate_left(ring, need, seg_head);
		out_start -= seg_count_matched;
		if (out_start != 0)
			memmove(&ring[out_start], &ring[0],
			        (size_t)seg_count_matched * sizeof(*ring));
		need -= seg_count_matched;
	}
	ring_count = eff_limit - out_start;
	ring_head = out_start;

	jw_arr_open(w);
	for (i = 0; i < ring_count; i++) {
		int idx = ring_head + i;

		jw_obj_open(w);
		jw_key(w, "ts");
		jw_int(w, (long long)ring[idx].ts);
		jw_key(w, "source");
		jw_str(w, ring[idx].source);
		jw_key(w, "level");
		jw_str(w, ring[idx].level);
		jw_key(w, "container");
		jw_str(w, ring[idx].container);
		jw_key(w, "msg");
		jw_str(w, ring[idx].msg);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	free(ring);
	if (have_re)
		regfree(&re);
}
