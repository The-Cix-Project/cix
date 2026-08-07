#include "logstore.h"
#include "persist.h"

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
static uint64_t g_next_seq = 1;
static int g_initialized;

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
	const struct json_value *jmax, *jseq;

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

void logstore_write(const char *source, const char *level, const char *fmt, ...)
{
	char msg[LOGSTORE_MSG_MAX];
	char src[LOGSTORE_SOURCE_MAX];
	char lvl[LOGSTORE_LEVEL_MAX];
	struct json_writer w;
	va_list ap;

	if (!g_initialized)
		return;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	snprintf(src, sizeof(src), "%s", source != NULL ? source : "kanxeod");
	snprintf(lvl, sizeof(lvl), "%s", level != NULL ? level : "info");

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "ts");
	jw_int(&w, (long long)time(NULL));
	jw_key(&w, "source");
	jw_str(&w, src);
	jw_key(&w, "level");
	jw_str(&w, lvl);
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

struct tail_entry {
	int64_t ts;
	char source[LOGSTORE_SOURCE_MAX];
	char level[LOGSTORE_LEVEL_MAX];
	char msg[LOGSTORE_MSG_MAX];
};

void logstore_tail(const char *source_filter, const char *level_filter, int64_t since,
                    int limit, struct json_writer *w)
{
	uint64_t seqs[LOGSTORE_SEGMENT_COUNT + 4];
	int seg_count = segment_seq_list(seqs, LOGSTORE_SEGMENT_COUNT + 4);
	int eff_limit = (limit > 0 && limit <= 5000) ? limit : 1000;
	struct tail_entry *ring;
	int ring_head = 0, ring_count = 0;
	int i;

	ring = calloc((size_t)eff_limit, sizeof(*ring));
	if (ring == NULL) {
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
			const char *src, *lvl, *msg;
			int64_t ts;
			int idx;

			if (root == NULL)
				continue;
			ts = (int64_t)json_as_number(json_object_get(root, "ts"));
			src = json_as_string(json_object_get(root, "source"));
			lvl = json_as_string(json_object_get(root, "level"));
			msg = json_as_string(json_object_get(root, "msg"));

			if ((source_filter != NULL && (src == NULL || strcmp(src, source_filter) != 0)) ||
			    (level_filter != NULL && (lvl == NULL || strcmp(lvl, level_filter) != 0)) ||
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
		jw_key(w, "msg");
		jw_str(w, ring[idx].msg);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	free(ring);
}
