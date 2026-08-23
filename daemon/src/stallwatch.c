#include "stallwatch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * How long the loop may go without a heartbeat before it counts as a
 * stall. The loop wakes at least once a second (main.c's epoll timeout
 * exists for this), so five seconds is two orders of magnitude above
 * normal and still far below the minutes-long silence that prompted
 * this. A legitimate slow synchronous operation crossing it is not a
 * false positive -- it is exactly the thing worth knowing about.
 */
#define STALL_THRESHOLD_SECONDS 5
/* While a stall continues, one further record every this often, so a
 * long wedge leaves a trail with timestamps rather than one line whose
 * end is unknown. */
#define STALL_REPEAT_SECONDS 30
#define STALL_ACTIVITY_MAX 192

struct stall_shared {
	/* Monotonic seconds at the last heartbeat. Read by the child while
	 * the parent writes it; a torn read costs at most one cycle of
	 * delay, and there is nothing here worth a lock. */
	volatile long long heartbeat_monotonic;
	volatile unsigned long long heartbeat_seq;
	char activity[STALL_ACTIVITY_MAX];
};

static struct stall_shared *g_shared;
static char g_records_path[PATH_MAX];
static pid_t g_watchdog_pid = -1;

static long long monotonic_seconds(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (long long)ts.tv_sec;
}

/* /proc/<pid>/wchan: the kernel function the process is sleeping in.
 * Empty for a running process, which is itself informative -- a spin is
 * a different animal from a block. */
static void read_wchan(pid_t pid, char *out, size_t out_size)
{
	char path[64];
	int fd;
	ssize_t n;

	snprintf(out, out_size, "%s", "");
	snprintf(path, sizeof(path), "/proc/%d/wchan", (int)pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return;
	n = read(fd, out, out_size - 1);
	close(fd);
	if (n < 0)
		n = 0;
	out[n] = '\0';
	/* wchan has no trailing newline, but be defensive about one. */
	{
		char *nl = strchr(out, '\n');

		if (nl != NULL)
			*nl = '\0';
	}
}

/* The single-letter process state from /proc/<pid>/stat: R running, S
 * interruptible sleep, D uninterruptible (the one that cannot be
 * killed and wedges hardest), T stopped, Z zombie. */
static char read_proc_state(pid_t pid)
{
	char path[64];
	char buf[512];
	int fd;
	ssize_t n;
	char *close_paren;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return '?';
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return '?';
	buf[n] = '\0';
	/* comm is parenthesised and may itself contain spaces/parens, so
	 * the state is the character two past the LAST ')'. */
	close_paren = strrchr(buf, ')');
	if (close_paren == NULL || close_paren[1] == '\0')
		return '?';
	return close_paren[2];
}

static void json_escape(const char *in, char *out, size_t out_size)
{
	size_t o = 0;
	size_t i;

	for (i = 0; in[i] != '\0' && o + 2 < out_size; i++) {
		unsigned char c = (unsigned char)in[i];

		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = (char)c;
		} else if (c < 0x20) {
			/* Control characters would make the record unparseable;
			 * a space keeps the line readable and honest about length. */
			out[o++] = ' ';
		} else {
			out[o++] = (char)c;
		}
	}
	out[o] = '\0';
}

static void append_record(const char *event, long long seconds, pid_t watched)
{
	char line[1024];
	char wchan[128];
	char activity[STALL_ACTIVITY_MAX];
	char esc_activity[STALL_ACTIVITY_MAX * 2];
	char esc_wchan[256];
	int fd;
	int len;

	read_wchan(watched, wchan, sizeof(wchan));
	snprintf(activity, sizeof(activity), "%s", g_shared->activity);
	json_escape(activity, esc_activity, sizeof(esc_activity));
	json_escape(wchan, esc_wchan, sizeof(esc_wchan));

	len = snprintf(line, sizeof(line),
	               "{\"ts\":%lld,\"event\":\"%s\",\"seconds\":%lld,\"state\":\"%c\","
	               "\"wchan\":\"%s\",\"activity\":\"%s\"}\n",
	               (long long)time(NULL), event, seconds, read_proc_state(watched), esc_wchan,
	               esc_activity);
	if (len <= 0)
		return;

	fd = open(g_records_path, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
	if (fd >= 0) {
		ssize_t ignored = write(fd, line, (size_t)len);

		(void)ignored;
		close(fd);
	}
	/* Also to stderr: on a box being watched over a serial console this
	 * is the only thing that will be visible WHILE the stall is
	 * happening, which is when it matters most. */
	{
		ssize_t ignored = write(2, line, (size_t)len);

		(void)ignored;
	}
}

static void watchdog_main(pid_t watched)
{
	int in_stall = 0;
	long long stall_started_at = 0;
	long long last_reported = 0;

	/* Die with the parent: a watchdog outliving what it watches is
	 * just a stray process. */
	prctl(PR_SET_PDEATHSIG, SIGKILL);

	/*
	 * Drop every inherited descriptor. The parent's listening sockets
	 * are among them, and a watchdog still holding one after the daemon
	 * dies would keep the port bound -- turning a diagnostic into the
	 * reason the daemon cannot be restarted. Nothing here needs an
	 * inherited fd: the record file is opened per write, /proc is
	 * opened on demand, and stderr is deliberately kept so a stall is
	 * visible on a serial console while it is happening.
	 */
	{
		int fd;

		for (fd = 3; fd < 1024; fd++)
			close(fd);
	}
	if (getppid() != watched)
		_exit(0); /* parent already gone between fork and prctl */

	for (;;) {
		struct timespec half = { 0, 500 * 1000 * 1000 };
		long long now, quiet_for;

		nanosleep(&half, NULL);
		if (getppid() != watched)
			_exit(0);

		now = monotonic_seconds();
		quiet_for = now - (long long)g_shared->heartbeat_monotonic;

		if (!in_stall && quiet_for >= STALL_THRESHOLD_SECONDS) {
			in_stall = 1;
			stall_started_at = now;
			last_reported = now;
			append_record("stall", quiet_for, watched);
			continue;
		}
		if (in_stall) {
			if (quiet_for < STALL_THRESHOLD_SECONDS) {
				append_record("recovered", now - stall_started_at + STALL_THRESHOLD_SECONDS,
				               watched);
				in_stall = 0;
				continue;
			}
			if (now - last_reported >= STALL_REPEAT_SECONDS) {
				last_reported = now;
				append_record("stall-continues", quiet_for, watched);
			}
		}
	}
}

int stallwatch_start(const char *records_path)
{
	pid_t pid;

	snprintf(g_records_path, sizeof(g_records_path), "%s", records_path);

	g_shared = mmap(NULL, sizeof(*g_shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
	                 -1, 0);
	if (g_shared == MAP_FAILED) {
		perror("stallwatch: mmap");
		g_shared = NULL;
		return -1;
	}
	memset((void *)g_shared, 0, sizeof(*g_shared));
	g_shared->heartbeat_monotonic = monotonic_seconds();

	pid = fork();
	if (pid < 0) {
		perror("stallwatch: fork");
		return -1;
	}
	if (pid == 0) {
		watchdog_main(getppid());
		_exit(0);
	}
	g_watchdog_pid = pid;
	return 0;
}

void stallwatch_heartbeat(void)
{
	if (g_shared == NULL)
		return;
	g_shared->heartbeat_monotonic = monotonic_seconds();
	g_shared->heartbeat_seq++;
}

void stallwatch_activity(const char *what)
{
	if (g_shared == NULL || what == NULL)
		return;
	snprintf(g_shared->activity, STALL_ACTIVITY_MAX, "%s", what);
}

void stallwatch_activity_clear(void)
{
	if (g_shared == NULL)
		return;
	g_shared->activity[0] = '\0';
}

/*
 * Reads the record file back. Deliberately re-read per request rather
 * than cached: the file is written by another process, and a cache
 * would be a second, staler copy of something whose whole value is
 * being the real one.
 */
void stallwatch_write_json(struct json_writer *w, int limit)
{
	char *buf = NULL;
	long size;
	FILE *f;
	char *lines[256];
	int count = 0;
	char *p;
	int i;

	jw_arr_open(w);
	f = fopen(g_records_path, "r");
	if (f == NULL) {
		jw_arr_close(w);
		return;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	if (size < 0) {
		fclose(f);
		jw_arr_close(w);
		return;
	}
	/* Only the tail is ever interesting, and a record file that has
	 * grown huge is itself a story the newest lines still tell. */
	if (size > 256 * 1024) {
		fseek(f, size - 256 * 1024, SEEK_SET);
		size = 256 * 1024;
	} else {
		fseek(f, 0, SEEK_SET);
	}
	buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		jw_arr_close(w);
		return;
	}
	size = (long)fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[size] = '\0';

	for (p = strtok(buf, "\n"); p != NULL && count < (int)(sizeof(lines) / sizeof(lines[0]));
	     p = strtok(NULL, "\n")) {
		if (p[0] == '{')
			lines[count++] = p;
	}
	/*
	 * Newest first: the record anyone wants is the last one written.
	 * Spliced verbatim -- each line is already a complete JSON object,
	 * written by the watchdog, and re-parsing it here only to re-emit
	 * it would put a second formatter in the path of the one thing
	 * that has to be trustworthy.
	 */
	{
		int emitted = 0;

		for (i = count - 1; i >= 0 && (limit <= 0 || count - i <= limit); i--) {
			/* The separator is ours to place: nothing else writes into
			 * this array, and jw_raw_text() deliberately splices bytes
			 * without touching the writer's own item bookkeeping. */
			if (emitted > 0)
				jw_raw_text(w, ",", 1);
			jw_raw_text(w, lines[i], strlen(lines[i]));
			emitted++;
		}
	}

	free(buf);
	jw_arr_close(w);
}
