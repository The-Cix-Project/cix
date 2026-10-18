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

		snprintf(msg, sizeof(msg), "a helper's held log was full -- %u more entr%s dropped",
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

	if (!g_initialized)
		return;
	snprintf(lvl, sizeof(lvl), "%s", level != NULL ? level : "info");
	if (level_rank(lvl) > level_rank(g_min_level))
		return; /* less severe than the configured floor -- dropped before any write */

	snprintf(src, sizeof(src), "%s", source != NULL ? source : "cixd");
	snprintf(cont, sizeof(cont), "%s", container != NULL ? container : "");
	if (g_hold_buf != NULL) {
		hold_entry(src, lvl, cont, msg);
		return;
	}

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
		} else {
			if (g_write_fail_count != 0) {
				/* Recovered. Say so in the store itself, now that
				 * it can hold it, and stop reporting. */
				g_write_fail_count = 0;
				g_write_fail_errno = 0;
				g_write_fail_first_ts = 0;
			}
			g_current_size += (long)w.len + 1;
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
		for (i = end; i > 0 && r->buf[i - 1] != '\n'; i--)
			;
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

		while (out_start > 0 && back_next_line(&r, &line, &n) == 1) {
			struct json_value *root;
			const char *src, *lvl, *cont, *msg;
			int64_t ts;
			struct tail_entry *e;

			if (n == 0)
				continue;
			root = json_parse(line, n);
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

			e = &ring[--out_start];
			e->ts = ts;
			snprintf(e->source, sizeof(e->source), "%s", src != NULL ? src : "");
			snprintf(e->level, sizeof(e->level), "%s", lvl != NULL ? lvl : "");
			snprintf(e->container, sizeof(e->container), "%s", cont != NULL ? cont : "");
			snprintf(e->msg, sizeof(e->msg), "%s", msg != NULL ? msg : "");
			json_free(root);
		}
		free(r.buf);
		close(r.fd);
	}

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
