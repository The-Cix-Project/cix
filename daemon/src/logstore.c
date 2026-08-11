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
 * are accepted as synonyms for "err"/"warning" since kanxeod/audit's
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

	if (g_kmsg_fd < 0)
		return;

	for (;;) {
		n = read(g_kmsg_fd, buf, sizeof(buf) - 1);
		if (n <= 0)
			return; /* EAGAIN (nothing new right now) or a real error */
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

	snprintf(src, sizeof(src), "%s", source != NULL ? source : "kanxeod");
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

	for (i = 0; i < seg_count; i++) {
		char path[600];
		FILE *fp;
		char *line = NULL;
		size_t line_cap = 0;
		ssize_t n;

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

			if (ring_count == eff_limit) {
				idx = ring_head;
				ring_head = (ring_head + 1) % eff_limit;
			} else {
				idx = ring_count;
				ring_count++;
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
	}

	jw_arr_open(w);
	for (i = 0; i < ring_count; i++) {
		int idx = (ring_head + i) % eff_limit;

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
