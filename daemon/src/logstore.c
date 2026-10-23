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
/*
 * #503: the store's own write failures, kept where a full disk
 * cannot erase them. Sticky from the first failure until a write
 * succeeds again, and reported by logstore_tail_ex() on every read.
 */
static int g_write_fail_errno;
static long long g_write_fail_first_ts;
static long g_write_fail_count;

/*
 * ADR-0342 phase 2 (#616): A SEGMENT'S SUMMARY -- its largest ts, and
 * every source, level and container in it.
 *
 * A filtered read skips a closed segment its summary proves cannot hold
 * a match: no entry at or after `since` (exact whatever the clock did,
 * because it is the LARGEST ts, not the last), or no entry with the
 * named source, level or container. "This container's last 40 lines"
 * read every segment before this, and the store may be 100 GB.
 *
 * A summary is exact or absent, never a guess. A set too large to keep
 * is recorded as unknown, which skips nothing; a summary is written only
 * when it covers its segment from the first byte; and one is used only
 * when the segment's seq, device, inode and size are the ones it was
 * made from (summary_load()).
 */
#define SUM_SOURCES_MAX 32
#define SUM_LEVELS_MAX 16
#define SUM_CONTAINERS_MAX 256

struct seg_summary {
	int complete;     /* every entry since the segment's first byte is in it */
	long long entries;
	int64_t max_ts;
	int n_src, n_lvl, n_cont;
	int src_over, lvl_over, cont_over; /* too many, or too long, to keep: unknown */
	char src[SUM_SOURCES_MAX][LOGSTORE_SOURCE_MAX];
	char lvl[SUM_LEVELS_MAX][LOGSTORE_LEVEL_MAX];
	char cont[SUM_CONTAINERS_MAX][LOGSTORE_CONTAINER_MAX];
};

/* The segment being written. */
static struct seg_summary g_cur_sum;

static void sum_reset(struct seg_summary *s, int complete)
{
	memset(s, 0, sizeof(*s));
	s->complete = complete;
}

/* Adds n bytes of name to a set of `max` slots of `slot` bytes each. */
static void sum_set_add(char *set, int *count, int *over, int max, size_t slot, const char *name,
                        size_t n)
{
	int i;

	if (*over)
		return;
	if (n >= slot) {
		*over = 1;
		return;
	}
	for (i = 0; i < *count; i++) {
		const char *have = set + (size_t)i * slot;

		if (memcmp(have, name, n) == 0 && have[n] == '\0')
			return;
	}
	if (*count >= max) {
		*over = 1;
		return;
	}
	memcpy(set + (size_t)(*count) * slot, name, n);
	set[(size_t)(*count) * slot + n] = '\0';
	(*count)++;
}

/* One entry. A NULL field is one the entry does not have, which no
 * filter on that field can match, so it adds nothing to that set. */
static void sum_add(struct seg_summary *s, int64_t ts, const char *src, size_t src_n,
                    const char *lvl, size_t lvl_n, const char *cont, size_t cont_n)
{
	if (s->entries == 0 || ts > s->max_ts)
		s->max_ts = ts;
	s->entries++;
	if (src != NULL)
		sum_set_add(&s->src[0][0], &s->n_src, &s->src_over, SUM_SOURCES_MAX,
		            sizeof(s->src[0]), src, src_n);
	if (lvl != NULL)
		sum_set_add(&s->lvl[0][0], &s->n_lvl, &s->lvl_over, SUM_LEVELS_MAX,
		            sizeof(s->lvl[0]), lvl, lvl_n);
	if (cont != NULL)
		sum_set_add(&s->cont[0][0], &s->n_cont, &s->cont_over, SUM_CONTAINERS_MAX,
		            sizeof(s->cont[0]), cont, cont_n);
}

/* 1 if the set may hold name: an unknown set may hold anything. */
static int sum_set_may_hold(const char *set, int count, int over, size_t slot, const char *name)
{
	int i;

	if (over)
		return 1;
	for (i = 0; i < count; i++) {
		if (strcmp(set + (size_t)i * slot, name) == 0)
			return 1;
	}
	return 0;
}

/* 1 if no entry in the summarised segment can match these filters. */
static int sum_excludes(const struct seg_summary *s, const char *source_filter,
                        const char *level_filter, const char *container_filter, int64_t since)
{
	if (s->entries == 0)
		return 1;
	if (since > 0 && s->max_ts < since)
		return 1;
	if (source_filter != NULL &&
	    !sum_set_may_hold(&s->src[0][0], s->n_src, s->src_over, sizeof(s->src[0]), source_filter))
		return 1;
	if (level_filter != NULL &&
	    !sum_set_may_hold(&s->lvl[0][0], s->n_lvl, s->lvl_over, sizeof(s->lvl[0]), level_filter))
		return 1;
	if (container_filter != NULL &&
	    !sum_set_may_hold(&s->cont[0][0], s->n_cont, s->cont_over, sizeof(s->cont[0]),
	                      container_filter))
		return 1;
	return 0;
}

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

/* "sum-", not "seg-": segment_seq_list()'s sscanf() would count any
 * name that starts with a segment's prefix and number as a segment. */
static void summary_path(uint64_t seq, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/sum-%020llu.json", g_dir, (unsigned long long)seq);
}

static void sum_write_set(struct json_writer *w, const char *key, const char *set, int count,
                          int over, size_t slot)
{
	int i;

	if (over)
		return; /* absent is unknown */
	jw_key(w, key);
	jw_arr_open(w);
	for (i = 0; i < count; i++)
		jw_str(w, set + (size_t)i * slot);
	jw_arr_close(w);
}

/*
 * Writes seq's summary, made from the segment file whose fstat() is st.
 * The device and inode are strings because a JSON number is a double
 * here, and an inode need not fit in 53 bits.
 *
 * Two read children may write the same summary at once, and
 * persist_atomic_write()'s temporary name is fixed. That is safe: both
 * summarised the same immutable bytes, so the content is identical, and
 * a reader that meets a torn file fails json_parse() and scans.
 */
static int summary_write(uint64_t seq, const struct stat *st, const struct seg_summary *s)
{
	char path[600];
	char num[32];
	struct json_writer w;
	int rc;

	summary_path(seq, path, sizeof(path));
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "seq");
	jw_int(&w, (long long)seq);
	jw_key(&w, "dev");
	snprintf(num, sizeof(num), "%llu", (unsigned long long)st->st_dev);
	jw_str(&w, num);
	jw_key(&w, "ino");
	snprintf(num, sizeof(num), "%llu", (unsigned long long)st->st_ino);
	jw_str(&w, num);
	jw_key(&w, "bytes");
	jw_int(&w, (long long)st->st_size);
	jw_key(&w, "entries");
	jw_int(&w, s->entries);
	jw_key(&w, "max_ts");
	jw_int(&w, (long long)s->max_ts);
	sum_write_set(&w, "sources", &s->src[0][0], s->n_src, s->src_over, sizeof(s->src[0]));
	sum_write_set(&w, "levels", &s->lvl[0][0], s->n_lvl, s->lvl_over, sizeof(s->lvl[0]));
	sum_write_set(&w, "containers", &s->cont[0][0], s->n_cont, s->cont_over,
	              sizeof(s->cont[0]));
	jw_obj_close(&w);
	rc = persist_atomic_write(path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static void sum_load_set(const struct json_value *v, char *set, int *count, int *over, int max,
                         size_t slot)
{
	size_t i;

	if (v == NULL || v->type != JSON_ARRAY) {
		*over = 1;
		return;
	}
	for (i = 0; i < v->u.array.count; i++) {
		const char *name = json_as_string(v->u.array.items[i]);

		if (name == NULL) {
			*over = 1;
			return;
		}
		sum_set_add(set, count, over, max, slot, name, strlen(name));
	}
}

/* 0 with *s filled when seq has a summary made from exactly the file
 * whose fstat() is st; -1 otherwise, and the segment is scanned. */
static int summary_load(uint64_t seq, const struct stat *st, struct seg_summary *s)
{
	char path[600];
	char num[32];
	char *buf;
	size_t len;
	struct json_value *root;
	const char *dev, *ino;
	int ok;

	summary_path(seq, path, sizeof(path));
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return -1;
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return -1;
	dev = json_as_string(json_object_get(root, "dev"));
	ino = json_as_string(json_object_get(root, "ino"));
	ok = json_object_get(root, "seq") != NULL &&
	     (uint64_t)json_as_number(json_object_get(root, "seq")) == seq &&
	     json_object_get(root, "bytes") != NULL &&
	     (off_t)json_as_number(json_object_get(root, "bytes")) == st->st_size &&
	     json_object_get(root, "entries") != NULL && json_object_get(root, "max_ts") != NULL &&
	     dev != NULL && ino != NULL;
	if (ok) {
		snprintf(num, sizeof(num), "%llu", (unsigned long long)st->st_dev);
		ok = strcmp(dev, num) == 0;
	}
	if (ok) {
		snprintf(num, sizeof(num), "%llu", (unsigned long long)st->st_ino);
		ok = strcmp(ino, num) == 0;
	}
	if (!ok) {
		json_free(root);
		return -1;
	}
	sum_reset(s, 1);
	s->entries = (long long)json_as_number(json_object_get(root, "entries"));
	s->max_ts = (int64_t)json_as_number(json_object_get(root, "max_ts"));
	sum_load_set(json_object_get(root, "sources"), &s->src[0][0], &s->n_src, &s->src_over,
	             SUM_SOURCES_MAX, sizeof(s->src[0]));
	sum_load_set(json_object_get(root, "levels"), &s->lvl[0][0], &s->n_lvl, &s->lvl_over,
	             SUM_LEVELS_MAX, sizeof(s->lvl[0]));
	sum_load_set(json_object_get(root, "containers"), &s->cont[0][0], &s->n_cont,
	             &s->cont_over, SUM_CONTAINERS_MAX, sizeof(s->cont[0]));
	json_free(root);
	return 0;
}

/* Every summary older than the oldest segment kept: one whose segment
 * was evicted, one a read child wrote after its segment was evicted,
 * and the .tmp of one whose child was killed mid-write. */
static void sweep_summaries(uint64_t oldest_kept)
{
	DIR *d = opendir(g_dir);
	struct dirent *de;

	if (d == NULL)
		return;
	while ((de = readdir(d)) != NULL) {
		unsigned long long seq;
		char path[600];

		if (strncmp(de->d_name, "sum-", 4) != 0 || sscanf(de->d_name + 4, "%llu", &seq) != 1 ||
		    seq >= oldest_kept)
			continue;
		snprintf(path, sizeof(path), "%s/%s", g_dir, de->d_name);
		unlink(path);
	}
	closedir(d);
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
	if (n > 0)
		sweep_summaries(seqs[n > LOGSTORE_SEGMENT_COUNT ? n - LOGSTORE_SEGMENT_COUNT : 0]);
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
		/*
		 * #616: the closing segment's summary, if this process saw the
		 * whole of it. Every write was flushed as it was made, so the
		 * fstat() is of the final bytes. On the loop, once per segment,
		 * beside save_state()'s identical write-and-fsync just below.
		 *
		 * The file must also be exactly the bytes this process wrote. A
		 * forked child inherits the open segment, and one that logs
		 * without logstore_hold() appends to it behind this process's
		 * accounting -- only credcheck.c holds (grep, 2026-10-19), and
		 * the build-environment composer, among others, does not. Its
		 * lines are not in g_cur_sum, so a summary would let a read
		 * skip them. Only the size can tell, and it does.
		 */
		if (g_cur_sum.complete && fstat(fileno(g_current_fp), &st) == 0 &&
		    st.st_size == (off_t)g_current_size)
			summary_write(g_current_seq, &st, &g_cur_sum);
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
		/* What it held before this process opened it is not in the
		 * summary, so it gets none at rotation; a filtered read that
		 * scans it whole makes one later. */
		sum_reset(&g_cur_sum, g_current_size == 0);
	}

	if (need_new) {
		g_current_seq = g_next_seq;
		g_next_seq++;
		if (save_state() != 0)
			return -1;
		segment_path(g_current_seq, path, sizeof(path));
		/*
		 * A seq is new unless state.json was lost, and then an old file
		 * may carry it. Unlinked rather than truncated, so the new
		 * segment is a new inode, and its old summary goes with it:
		 * otherwise a summary of the old bytes could match the new file
		 * on size alone.
		 */
		unlink(path);
		{
			char spath[600];

			summary_path(g_current_seq, spath, sizeof(spath));
			unlink(spath);
		}
		/* "a", as the reopen above: the path is new after the unlink,
		 * so this creates it, and O_APPEND means a line anyone else
		 * appends is never overwritten by this process's own offset. */
		g_current_fp = fopen(path, "a");
		if (g_current_fp == NULL)
			return -1;
		g_current_size = 0;
		sum_reset(&g_cur_sum, 1);
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

static void replay_early(void);

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
	replay_early();
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

/*
 * ADR-0340: logstore_hold()'s buffer, in a forked child. The first four
 * bytes count the entries that did not fit; after them, each held entry
 * is its source, level, container and message, each NUL-terminated.
 */
#define HOLD_HEADER_BYTES 4
static char *g_hold_buf;
static size_t g_hold_size;
static size_t g_hold_len;

void logstore_hold(char *buf, size_t size)
{
	g_hold_buf = NULL;
	if (buf == NULL || size <= HOLD_HEADER_BYTES)
		return;
	memset(buf, 0, size); /* an empty source after the last entry is what ends a replay */
	g_hold_buf = buf;
	g_hold_size = size;
	g_hold_len = HOLD_HEADER_BYTES;
}

static void hold_entry(const char *src, const char *lvl, const char *cont, const char *msg)
{
	const char *parts[4];
	size_t need = 0;
	int i;

	parts[0] = src;
	parts[1] = lvl;
	parts[2] = cont;
	parts[3] = msg;
	for (i = 0; i < 4; i++)
		need += strlen(parts[i]) + 1;
	if (need > g_hold_size - g_hold_len) {
		uint32_t dropped;

		memcpy(&dropped, g_hold_buf, sizeof(dropped));
		dropped++;
		memcpy(g_hold_buf, &dropped, sizeof(dropped));
		return;
	}
	for (i = 0; i < 4; i++) {
		size_t n = strlen(parts[i]) + 1;

		memcpy(g_hold_buf + g_hold_len, parts[i], n);
		g_hold_len += n;
	}
}

static void write_entry(const char *source, const char *level, const char *container,
                         const char *msg);

/*
 * What was written before logstore_init(), in the same format as a
 * helper's hold. boot_init(), pkg_init(), hostauth_init(),
 * devicemap_init() and the start-up module loads all run before it, and
 * a line written then used to be dropped: the ESP check's outcome
 * (#638), and boot_init()'s efivarfs and resolv.conf failures, never
 * reached the store. Replayed by logstore_init(), so it is a held line's
 * replay time, seconds after it was said, that its ts records.
 */
static char g_early_buf[131072];

static void replay_early(void)
{
	size_t len = g_hold_len;

	if (g_hold_buf != g_early_buf)
		return;
	g_hold_buf = NULL;
	logstore_replay(g_early_buf, len);
}

void logstore_replay(const char *held, size_t len)
{
	uint32_t dropped;
	size_t off = HOLD_HEADER_BYTES;

	if (held == NULL || len < HOLD_HEADER_BYTES)
		return;
	memcpy(&dropped, held, sizeof(dropped));
	/* An entry's source is never empty (write_entry() defaults it to
	 * "cixd"), so an empty one is the unused rest of the buffer. */
	while (off < len && held[off] != '\0') {
		const char *parts[4];
		int i;

		for (i = 0; i < 4; i++) {
			const char *end = memchr(held + off, '\0', len - off);

			if (end == NULL)
				return; /* a torn record: what came before it is written, nothing after */
			parts[i] = held + off;
			off = (size_t)(end - held) + 1;
			if (off >= len && i < 3)
				return;
		}
		write_entry(parts[0], parts[1], parts[2], parts[3]);
	}
	if (dropped > 0) {
		char msg[96];

		snprintf(msg, sizeof(msg), "a held log was full -- %u more entr%s dropped",
		         (unsigned)dropped, dropped == 1 ? "y was" : "ies were");
		write_entry("cixd", "warn", NULL, msg);
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
	long long now;

	snprintf(lvl, sizeof(lvl), "%s", level != NULL ? level : "info");
	snprintf(src, sizeof(src), "%s", source != NULL ? source : "cixd");
	snprintf(cont, sizeof(cont), "%s", container != NULL ? container : "");
	if (!g_initialized) {
		/* Held, not dropped: replayed at the end of logstore_init(),
		 * where the configured floor applies. The floor is not known
		 * yet, so every level is kept. */
		if (g_hold_buf == NULL)
			logstore_hold(g_early_buf, sizeof(g_early_buf));
		hold_entry(src, lvl, cont, msg);
		return;
	}
	if (level_rank(lvl) > level_rank(g_min_level))
		return; /* less severe than the configured floor -- dropped before any write */
	if (g_hold_buf != NULL) {
		hold_entry(src, lvl, cont, msg);
		return;
	}

	now = (long long)time(NULL);
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "ts");
	jw_int(&w, now);
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
		int failed;

		errno = 0;
		if (fwrite(w.buf, 1, w.len, g_current_fp) != w.len)
			failed = 1;
		else if (fputc('\n', g_current_fp) == EOF)
			failed = 1;
		else
			failed = (fflush(g_current_fp) != 0);

		/*
		 * #503: these three return values were all discarded, and
		 * that is how a full disk became invisible.
		 *
		 * When /var/lib/cix reached 0.0 GiB free, this function kept
		 * being called and kept writing nothing. The store froze at
		 * the exact second the failing build started, so the one
		 * place an operator looks -- and the place `image gc` tells
		 * them to look, "see the log store" -- had no record of the
		 * thing that broke it. A logger that goes quiet exactly when
		 * things break is worse than no logger, because silence
		 * reads as "nothing happened".
		 *
		 * The failure is kept IN MEMORY, which is the only place
		 * left that still works, and logstore_tail_ex() reports it
		 * on every read.
		 */
		if (failed) {
			if (g_write_fail_count == 0) {
				g_write_fail_errno = errno;
				g_write_fail_first_ts = (long long)time(NULL);
			}
			g_write_fail_count++;
			clearerr(g_current_fp);
			/* Some of this entry may be on disk, so the summary can no
			 * longer say what the segment holds (#616). */
			g_cur_sum.complete = 0;
		} else {
			if (g_write_fail_count != 0) {
				/* Recovered. Say so in the store itself, now that
				 * it can hold it, and stop reporting. */
				g_write_fail_count = 0;
				g_write_fail_errno = 0;
				g_write_fail_first_ts = 0;
			}
			g_current_size += (long)w.len + 1;
			sum_add(&g_cur_sum, now, src, strlen(src), lvl, strlen(lvl), cont, strlen(cont));
		}
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
 * #503: appends the store's own "I cannot write" entry, if it cannot.
 *
 * Called from every exit of logstore_tail_ex(), including the two
 * that return an empty array early, because a reader who gets `[]`
 * is exactly the reader most likely to conclude nothing happened.
 */
static void emit_write_failure_entry(struct json_writer *w)
{
	char msg[LOGSTORE_MSG_MAX];

	if (g_write_fail_count == 0)
		return;
	snprintf(msg, sizeof(msg),
	         "log store cannot write: %s (%ld entr%s lost since %lld). Entries are missing "
	         "from this response and from disk; free space on the data directory's "
	         "filesystem is the usual cause.",
	         strerror(g_write_fail_errno), g_write_fail_count,
	         g_write_fail_count == 1 ? "y" : "ies", g_write_fail_first_ts);
	jw_obj_open(w);
	jw_key(w, "ts");
	jw_int(w, (long long)time(NULL));
	jw_key(w, "source");
	jw_str(w, "logstore");
	jw_key(w, "level");
	jw_str(w, "err");
	jw_key(w, "container");
	jw_str(w, "");
	jw_key(w, "msg");
	jw_str(w, msg);
	jw_obj_close(w);
}

/*
 * ADR-0342 (#612): a segment is read BACKWARDS, newest line first.
 *
 * The reader before this one (#450) read segments newest first but each
 * one from its start, json_parse()ing every line, and kept the last N in
 * a ring. So an unfiltered `tail=1` paid for the whole newest segment --
 * up to max_bytes / 8, which is 12.5 MB at the default and 12.5 GB at
 * LOGSTORE_MAX_MAX_BYTES -- and the dashboard's log panel is sampled
 * in-process every 2 s (ADR-0339). Reading from the end makes an
 * unfiltered tail cost O(limit) whatever the store's size, which is what
 * lets it stay on the event loop. A filtered read can still have to read
 * everything, and so runs in a helper child (logread.h).
 *
 * The block is a page-cache read size, not a line limit: a line longer
 * than one block (a 4 KB message can escape to several times that) is
 * assembled across reads. TAIL_LINE_MAX bounds that assembly, because a
 * span with no newline in it that long holds no entry this store wrote,
 * and following it would let a damaged file make one read hold the
 * whole segment in memory.
 */
#define TAIL_BLOCK 65536
#define TAIL_LINE_MAX (256 * 1024)

struct back_reader {
	int fd;
	off_t pos;  /* file offset of buf[0] */
	char *buf;  /* bytes [pos, pos + len) of the file, none returned yet */
	size_t len;
	size_t cap;
};

/* Prepends the block before pos. 0, or -1 on a read error or no memory. */
static int back_fill(struct back_reader *r)
{
	size_t want = r->pos < TAIL_BLOCK ? (size_t)r->pos : TAIL_BLOCK;
	size_t got = 0;

	if (r->len + want > r->cap) {
		size_t cap = r->cap != 0 ? r->cap : TAIL_BLOCK;
		char *nb;

		while (cap < r->len + want)
			cap *= 2;
		nb = realloc(r->buf, cap);
		if (nb == NULL)
			return -1;
		r->buf = nb;
		r->cap = cap;
	}
	memmove(r->buf + want, r->buf, r->len);
	while (got < want) {
		ssize_t n = pread(r->fd, r->buf + got, want - got, r->pos - (off_t)want + (off_t)got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		got += (size_t)n;
	}
	r->pos -= (off_t)want;
	r->len += want;
	return 0;
}

/*
 * The newest line not yet returned, without its newline, valid until the
 * next call. 1 with *line/*n set; 0 when the file's start is reached; -1
 * on a read error. A trailing line with no newline -- an entry being
 * written as the segment was opened -- is returned like any other, and
 * json_parse() refuses it if it is incomplete.
 */
static int back_next_line(struct back_reader *r, const char **line, size_t *n)
{
	for (;;) {
		size_t end, i;
		const char *nl;

		if (r->len == 0) {
			if (r->pos == 0)
				return 0;
			if (back_fill(r) != 0)
				return -1;
			continue;
		}
		end = r->len;
		if (r->buf[end - 1] == '\n')
			end--; /* this line's own terminator */
		/* memrchr, as getline() finds newlines with memchr. Measured on
		 * 192.168.15.95 with a byte loop here: a whole-store scan took
		 * 2.03 s, against 1533 ms for the forward reader it replaced
		 * (#612); the transport to the child costs at most 17 ms. */
		nl = end > 0 ? memrchr(r->buf, '\n', end) : NULL;
		i = nl != NULL ? (size_t)(nl - r->buf) + 1 : 0;
		if (i > 0 || r->pos == 0) {
			*line = r->buf + i;
			*n = end - i;
			r->len = i; /* buf[i - 1], if any, terminates the line before */
			return 1;
		}
		if (r->len >= TAIL_LINE_MAX) {
			/* No entry is this long: what is held is damage, and is
			 * dropped as one unparseable line. The next block's own
			 * newline, if it has one, ends the line before it. */
			*line = r->buf;
			*n = 0;
			r->len = 0;
			return 1;
		}
		if (back_fill(r) != 0)
			return -1;
	}
}

/*
 * #616: AN ENTRY'S HEAD, READ WITHOUT json_parse().
 *
 * write_entry() writes the fields in one order --
 * {"ts":N,"source":"..","level":"..","container":"..","msg":".. -- and
 * a full scan spends its time in json_parse() (#612: 1.61 s for a warm
 * 100 MB store on 192.168.15.95, with the newline search already fast).
 * So a line in exactly that shape is filtered on source, level,
 * container and since first, and only a line that passes is parsed.
 *
 * It is exact because it only ever REJECTS, and only on values equal to
 * the ones json_parse() would give: a span with a backslash in it is
 * escaped, so such a line falls back to json_parse(); and every other
 * byte jw_str() writes is the byte itself. json_object_get() takes a
 * key's first occurrence, which is the one read here. A line in any
 * other shape -- damage, a truncated entry, a format this store never
 * wrote -- returns 0 and is parsed as before.
 */
struct line_head {
	int64_t ts;
	const char *src, *lvl, *cont;
	size_t src_n, lvl_n, cont_n;
};

/* key, then a string with no backslash in it; returns the byte after
 * its closing quote, or NULL. */
static const char *head_str(const char *q, const char *end, const char *key, const char **s,
                            size_t *n)
{
	size_t k = strlen(key);
	const char *p;

	if ((size_t)(end - q) < k || memcmp(q, key, k) != 0)
		return NULL;
	q += k;
	for (p = q; p < end && *p != '"'; p++) {
		if (*p == '\\')
			return NULL;
	}
	if (p >= end)
		return NULL;
	*s = q;
	*n = (size_t)(p - q);
	return p + 1;
}

static int line_head(const char *line, size_t n, struct line_head *h)
{
	const char *end = line + n;
	const char *q = line;
	int neg = 0, digits = 0;
	int64_t v = 0;

	if (n < 6 || memcmp(q, "{\"ts\":", 6) != 0)
		return 0;
	q += 6;
	if (q < end && *q == '-') {
		neg = 1;
		q++;
	}
	while (q < end && *q >= '0' && *q <= '9') {
		if (++digits > 18)
			return 0;
		v = v * 10 + (*q - '0');
		q++;
	}
	if (digits == 0)
		return 0;
	h->ts = neg ? -v : v;
	q = head_str(q, end, ",\"source\":\"", &h->src, &h->src_n);
	if (q != NULL)
		q = head_str(q, end, ",\"level\":\"", &h->lvl, &h->lvl_n);
	if (q != NULL)
		q = head_str(q, end, ",\"container\":\"", &h->cont, &h->cont_n);
	return q != NULL && (size_t)(end - q) >= 8 && memcmp(q, ",\"msg\":\"", 8) == 0;
}

static int span_is(const char *s, size_t n, const char *want)
{
	return strlen(want) == n && memcmp(s, want, n) == 0;
}

static struct logstore_read_stats g_read_stats;

void logstore_last_read_stats(struct logstore_read_stats *out)
{
	*out = g_read_stats;
}

void logstore_tail_ex(const char *source_filter, const char *level_filter,
                       const char *container_filter, const char *msg_regex, int64_t since,
                       int limit, struct json_writer *w)
{
	uint64_t seqs[LOGSTORE_SEGMENT_COUNT + 4];
	int seg_count = segment_seq_list(seqs, LOGSTORE_SEGMENT_COUNT + 4);
	int eff_limit = (limit > 0 && limit <= 5000) ? limit : 1000;
	struct tail_entry *ring;
	/* Filled from the END backwards, because lines arrive newest first.
	 * out_start is where the held run begins. */
	int out_start;
	int i;
	regex_t re;
	int have_re = 0;
	/* #616: one summary, reused for each segment's load or build. */
	struct seg_summary *sum = NULL;
	int prefilter = source_filter != NULL || level_filter != NULL || container_filter != NULL ||
	                since > 0;

	memset(&g_read_stats, 0, sizeof(g_read_stats));
	if (msg_regex != NULL && msg_regex[0] != '\0') {
		if (regcomp(&re, msg_regex, REG_EXTENDED | REG_NOSUB | REG_ICASE) != 0) {
			/* An invalid pattern matches nothing, rather than crashing
			 * or silently ignoring the filter -- the caller (logread.c)
			 * already 400s a malformed regex before ever reaching
			 * here; this is just defense in depth. */
			jw_arr_open(w);
			emit_write_failure_entry(w);
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
		emit_write_failure_entry(w);
		jw_arr_close(w);
		return;
	}
	out_start = eff_limit;

	/*
	 * #616: summaries are read and made only by a FILTERED read, which is
	 * the kind logread_query_bounded() sends to a helper child. A read
	 * with no filter can skip nothing by them, and runs on the event
	 * loop, where a lazily made summary would be a write and an fsync.
	 */
	if (source_filter != NULL || level_filter != NULL || container_filter != NULL || have_re ||
	    since > 0)
		sum = malloc(sizeof(*sum));

	/*
	 * Newest segment first, newest line first, and stop as soon as the
	 * tail is full. The first `limit` matches met this way ARE the
	 * store's last `limit` matches, so filling the result from its end
	 * leaves it in chronological order with no reordering at all.
	 *
	 * `since` is a filter and never a stop condition (ADR-0342 item 3):
	 * ts is time(NULL) at write and this daemon sets the clock, so a
	 * segment can hold a step backwards, and ending the walk at the
	 * first ts below `since` would drop the entries after the step.
	 */
	for (i = seg_count - 1; i >= 0 && out_start > 0; i--) {
		char path[600];
		struct back_reader r;
		struct stat st;
		const char *line;
		size_t n;
		int rc = 1;
		/* #616: making this segment's summary as it is read. */
		int build = 0;

		segment_path(seqs[i], path, sizeof(path));
		memset(&r, 0, sizeof(r));
		r.fd = open(path, O_RDONLY | O_CLOEXEC);
		if (r.fd < 0)
			continue; /* evicted since it was listed */
		if (fstat(r.fd, &st) != 0) {
			close(r.fd);
			continue;
		}
		r.pos = st.st_size;

		/*
		 * The segment being written has no summary and gets none: the
		 * newest one listed, and the one this process appends to, which
		 * differ only if state.json was lost. A wrong answer here could
		 * not make a read wrong -- a summary is used only for the exact
		 * size it was made at -- only pointless.
		 */
		if (sum != NULL && i < seg_count - 1 &&
		    !(g_current_fp != NULL && seqs[i] == g_current_seq)) {
			if (summary_load(seqs[i], &st, sum) == 0) {
				if (sum_excludes(sum, source_filter, level_filter, container_filter, since)) {
					g_read_stats.skipped++;
					close(r.fd);
					continue;
				}
			} else {
				sum_reset(sum, 1);
				build = 1;
			}
		}
		g_read_stats.segments++;

		while (out_start > 0 && (rc = back_next_line(&r, &line, &n)) == 1) {
			struct json_value *root;
			const char *src, *lvl, *cont, *msg;
			int64_t ts;
			struct tail_entry *e;
			struct line_head h;
			int headed;

			if (n == 0)
				continue;
			/* Only a field filter or `since` can reject on the head. A
			 * regex-only read parses every line anyway, so it reads no
			 * head, and a summary it builds takes the parsed fields. */
			headed = prefilter && line_head(line, n, &h);
			if (headed) {
				if (build)
					sum_add(sum, h.ts, h.src, h.src_n, h.lvl, h.lvl_n, h.cont, h.cont_n);
				if ((source_filter != NULL && !span_is(h.src, h.src_n, source_filter)) ||
				    (level_filter != NULL && !span_is(h.lvl, h.lvl_n, level_filter)) ||
				    (container_filter != NULL &&
				     !span_is(h.cont, h.cont_n, container_filter)) ||
				    (since > 0 && h.ts < since))
					continue;
			}
			g_read_stats.lines_parsed++;
			root = json_parse(line, n);
			if (root == NULL)
				continue;
			ts = (int64_t)json_as_number(json_object_get(root, "ts"));
			src = json_as_string(json_object_get(root, "source"));
			lvl = json_as_string(json_object_get(root, "level"));
			cont = json_as_string(json_object_get(root, "container"));
			msg = json_as_string(json_object_get(root, "msg"));
			if (build && !headed)
				sum_add(sum, ts, src, src != NULL ? strlen(src) : 0, lvl,
				        lvl != NULL ? strlen(lvl) : 0, cont, cont != NULL ? strlen(cont) : 0);

			if ((source_filter != NULL && (src == NULL || strcmp(src, source_filter) != 0)) ||
			    (level_filter != NULL && (lvl == NULL || strcmp(lvl, level_filter) != 0)) ||
			    (container_filter != NULL &&
			     (cont == NULL || strcmp(cont, container_filter) != 0)) ||
			    (have_re && (msg == NULL || regexec(&re, msg, 0, NULL, 0) != 0)) ||
			    (since > 0 && ts < since)) {
				json_free(root);
				continue;
			}

			e = &ring[--out_start];
			e->ts = ts;
			snprintf(e->source, sizeof(e->source), "%s", src != NULL ? src : "");
			snprintf(e->level, sizeof(e->level), "%s", lvl != NULL ? lvl : "");
			snprintf(e->container, sizeof(e->container), "%s", cont != NULL ? cont : "");
			snprintf(e->msg, sizeof(e->msg), "%s", msg != NULL ? msg : "");
			json_free(root);
		}
		/* Read to its first byte: the summary covers the whole file. A
		 * walk that stopped because the tail was full saw only part. */
		if (build && rc == 0 && summary_write(seqs[i], &st, sum) == 0)
			g_read_stats.summarised++;
		free(r.buf);
		close(r.fd);
	}
	free(sum);

	jw_arr_open(w);
	for (i = out_start; i < eff_limit; i++) {
		jw_obj_open(w);
		jw_key(w, "ts");
		jw_int(w, (long long)ring[i].ts);
		jw_key(w, "source");
		jw_str(w, ring[i].source);
		jw_key(w, "level");
		jw_str(w, ring[i].level);
		jw_key(w, "container");
		jw_str(w, ring[i].container);
		jw_key(w, "msg");
		jw_str(w, ring[i].msg);
		jw_obj_close(w);
	}
	emit_write_failure_entry(w);
	jw_arr_close(w);
	free(ring);
	if (have_re)
		regfree(&re);
}
