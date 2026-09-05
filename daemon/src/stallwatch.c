#include "stallwatch.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/resource.h>
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
/*
 * How often the watchdog asks the daemon whether it is serving, how
 * long it waits for an answer, and how many consecutive refusals make
 * it a reportable stall (#247).
 *
 * Two failures rather than one: a single missed answer during a heavy
 * moment is not a wedge, and a diagnostic that cries wolf is one nobody
 * reads. Two at this spacing means roughly ten seconds with no answer,
 * which no healthy request on this daemon has ever taken.
 */
#define PROBE_INTERVAL_SECONDS 5
#define PROBE_TIMEOUT_MS 4000
#define PROBE_FAILURES_FOR_STALL 2

struct stall_shared {
	/* Monotonic seconds at the last heartbeat. Read by the child while
	 * the parent writes it; a torn read costs at most one cycle of
	 * delay, and there is nothing here worth a lock. */
	volatile long long heartbeat_monotonic;
	volatile unsigned long long heartbeat_seq;
	char activity[STALL_ACTIVITY_MAX];
	/*
	 * When the in-flight request started, or 0 for none (#229).
	 *
	 * The heartbeat answers "is the loop going round". This answers
	 * "is it getting anywhere", and they are not the same question. A
	 * daemon serving one slow request per iteration updates the
	 * heartbeat every pass and is, by that measure alone, perfectly
	 * healthy -- while being unusable from outside. Two wedges needing
	 * a manual reset produced no record for exactly that reason: there
	 * was correctly no stall, because the wrong thing was measured.
	 */
	volatile long long activity_started_monotonic;

	/*
	 * How long one pass of the event loop spent WORKING, excluding the
	 * epoll wait (#229).
	 *
	 * The two fields above still miss the case that actually took the
	 * box out. On 2026-09-01 a publish went unanswered for 247 seconds
	 * and this store recorded nothing at all -- correctly, by its own
	 * definitions. The loop never stopped (heartbeat fine) and no
	 * single request was in flight for long (activity fine); the loop
	 * simply spent seconds of every pass starting a rebuild, while
	 * everything else waited to be accepted.
	 *
	 * So the question neither of them asks: how long does one turn of
	 * the loop take? That is the latency floor for every client, and a
	 * run of slow passes is a daemon that is alive and unusable at the
	 * same time.
	 *
	 * Milliseconds, because the interesting range starts well below a
	 * second and the existing fields are seconds.
	 */
	volatile long long pass_started_millis;
	volatile long long pass_last_work_millis;
	volatile long long pass_worst_work_millis;
	volatile unsigned long long slow_pass_count;
	char pass_worst_activity[STALL_ACTIVITY_MAX];
	/*
	 * What this pass has served so far, cleared at the start of each
	 * one.
	 *
	 * Needed because `activity` above is cleared the moment a request
	 * finishes dispatching, and a pass is booked at its END -- so
	 * reading `activity` there always found it already wiped, and the
	 * one field that answers "what was it doing" would have been
	 * permanently empty. Caught on a real host: a 131 ms worst pass
	 * driven by a known endpoint reported no activity at all.
	 */
	char pass_activity[STALL_ACTIVITY_MAX];
	/*
	 * Where the watchdog should ask the daemon whether it is actually
	 * SERVING (#247).
	 *
	 * Published by the parent once the port is final rather than passed
	 * to stallwatch_start(), because the port is not known that early
	 * -- daemon_config_init() may still override it. Zero means "not
	 * yet, do not probe", which is also the right behaviour during
	 * startup.
	 */
	volatile int probe_port;
	char probe_host[64];
	/*
	 * When the service probe last got an answer, and how many it has
	 * sent. Reported by GET /system/stalls, because "no stall records"
	 * is ambiguous on its own: a probe that never runs and a probe that
	 * always succeeds both write nothing. A detector whose armed state
	 * cannot be seen is one nobody can trust -- the same lesson #246
	 * produced for the job-slot budget.
	 */
	volatile long long probe_last_ok_monotonic;
	volatile unsigned long long probe_count;
};

/*
 * A pass doing more work than this made every waiting client wait at
 * least this long. Deliberately well above the cost of an ordinary
 * request (single-digit milliseconds, measured) and below anything a
 * person would call responsive.
 */
#define SLOW_PASS_MILLIS 750

/*
 * How many records GET /system/stalls can hold in flight while it reads
 * the tail of the record file. The newest this many survive; anything
 * older in the window is dropped (see stallwatch_write_json()).
 */
#define STALL_REPORT_MAX 256

static struct stall_shared *g_shared;
static char g_records_path[PATH_MAX];

/*
 * ADR-0246 item 2: whether a supervisor is present to act on what this
 * watchdog measures. Set from stallwatch_start(); read in the forked
 * child, which inherits it across the fork.
 *
 * Without a supervisor the watchdog behaves exactly as it always has --
 * it records and nothing else. Signalling pid 1 when pid 1 is the
 * daemon itself would be pointless at best.
 */
static int g_supervised;
static pid_t g_watchdog_pid = -1;

static long long monotonic_millis(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

static void append_line(const char *line, int len);

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

	append_line(line, len);
}

/*
 * One record to disk and to stderr. Split out of append_record() so the
 * slow-pass record below writes through exactly the same durability and
 * console path rather than a second copy of it (#229).
 */
/*
 * One line into the kernel ring buffer, best effort.
 *
 * /dev/kmsg is memory-backed: it needs no filesystem, cannot wait on a
 * block device, appears on the serial console as it happens, and is
 * read back into this daemon's own log store by the kernel log reader.
 * That makes it the only channel here that still works when the thing
 * being reported is the storage path -- which is why both the records
 * and, since #229, the failure to store them go through it.
 */
static void kmsg_write(const char *fmt, ...)
{
	char kline[512];
	va_list ap;
	int klen;
	int fd;

	va_start(ap, fmt);
	klen = vsnprintf(kline, sizeof(kline), fmt, ap);
	va_end(ap);
	if (klen <= 0)
		return;
	if (klen >= (int)sizeof(kline))
		klen = (int)sizeof(kline) - 1;

	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	{
		ssize_t ignored = write(fd, kline, (size_t)klen);

		(void)ignored;
	}
	close(fd);
}

static void append_line(const char *line, int len)
{
	int fd;

	/*
	 * The kernel ring buffer FIRST, before anything that can touch a
	 * disk.
	 *
	 * The file write below fsyncs on purpose, and its own comment
	 * accepts that a stuck I/O path blocks it "until I/O recovers".
	 * That acceptance is wrong in the one case that matters most: if
	 * the wedge IS an I/O stall, then open() and fsync() both block on
	 * exactly the resource being reported and the record lands late or
	 * not at all.
	 *
	 * The evidence: three multi-minute wedges on 192.168.15.95 on
	 * 2026-09-04/05, and a record file whose newest entry is
	 * 2026-09-01 15:08:03 -- nothing at all from any of them.
	 *
	 * That reading was briefly retracted and is restored here, because
	 * the retraction was wrong and the way it went wrong is worth
	 * keeping. A second, unrelated bug (#284) had the reporting
	 * endpoint returning the OLDEST 256 records of its read window, so
	 * the newest record it could show was four days stale. From that
	 * it was concluded that the records existed and merely could not
	 * be read out. They did not. Fixing the reader moved the newest
	 * visible record forward by one day, to 2026-09-01 15:08:03, and
	 * there it stopped -- which is where the file genuinely ends.
	 *
	 * Two bugs with the same symptom, and the second one was assumed
	 * to explain the first without being measured against it.
	 *
	 * /dev/kmsg is memory-backed. It needs no filesystem, cannot wait
	 * on a block device, appears on the serial console as it happens,
	 * and is read back into this daemon's own log store by the kernel
	 * log reader -- so a record written here is visible three ways
	 * without depending on the thing that may be stuck, and it appears
	 * while the wedge is happening rather than after it.
	 *
	 * Best-effort: a diagnostic that refuses to run is not a
	 * diagnostic, so nothing here fails the watchdog. Since #229 the
	 * writes are still best-effort but no longer SILENT -- the file
	 * half reports its own failure through this channel, because four
	 * days of a quietly non-growing record file cost two
	 * investigations that reached opposite conclusions.
	 */
	kmsg_write("cix stallwatch: %.*s",
	            len > 0 && line[len - 1] == '\n' ? len - 1 : len, line);

	fd = open(g_records_path, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0) {
		/*
		 * Issue #229: say so, through the channel that works.
		 *
		 * Every write in this function was deliberately unchecked, on
		 * the reasoning that a diagnostic which refuses to run is not
		 * a diagnostic. That is right about not FAILING; it was wrong
		 * about not REPORTING, and the cost was four days of silence
		 * and two investigations that reached opposite conclusions.
		 *
		 * Measured on 192.168.15.95 on 2026-09-05: two slow-pass
		 * records reached /dev/kmsg and the log store at 17:42:10 and
		 * 17:42:40, from this same call -- while the record file's
		 * newest entry was still 2026-09-01 09:24:35. The kmsg half
		 * worked and the file half did not, silently, and nothing
		 * anywhere could say why.
		 *
		 * So the failure is reported where the record already goes,
		 * which needs no filesystem and cannot wait on the resource
		 * that may be stuck. Deliberately not via append_line(),
		 * which would recurse straight back into this branch.
		 */
		kmsg_write("cix stallwatch: record file write failed: %s: %s", g_records_path,
		            strerror(errno));
	}
	if (fd >= 0) {
		ssize_t wrote = write(fd, line, (size_t)len);

		if (wrote != (ssize_t)len) {
			/*
			 * A short or failed write is as invisible as a failed
			 * open was, and produces the same symptom -- a record
			 * file that quietly stops growing. Reported for the same
			 * reason and by the same route.
			 */
			kmsg_write("cix stallwatch: record file write short: %s: wrote %ld of %d: %s",
			            g_records_path, (long)wrote, len, strerror(errno));
		}
		/*
		 * Issue #229: fsync, because this file exists precisely to
		 * survive the event it is recording.
		 *
		 * A wedge that has to be resolved by resetting the machine is
		 * the case this watchdog was built for, and a plain write()
		 * leaves the record in the page cache -- where a reset loses
		 * it. That is not hypothetical: cixd wedged for roughly
		 * sixteen minutes, the box was reset by hand, and this file's
		 * newest entry was from sixteen hours earlier. The watchdog
		 * was running and the heartbeat had certainly stopped, so the
		 * records were written; they simply never reached the disk.
		 *
		 * The cost is nothing in the case that matters. Records are
		 * appended only during a stall -- one at detection, then one
		 * per thirty seconds -- so this is a handful of fsyncs during
		 * an outage, not a per-request cost.
		 *
		 * If the I/O path is itself what is stuck, this blocks. That
		 * is acceptable and deliberate: this is a separate process,
		 * so blocking here cannot make the daemon's own stall worse,
		 * and the record lands as soon as I/O recovers.
		 */
		if (fsync(fd) != 0)
			kmsg_write("cix stallwatch: record file fsync failed: %s: %s",
			            g_records_path, strerror(errno));
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

/*
 * The loop is going round and getting nowhere (#229).
 *
 * A distinct event from "stall" on purpose: a stall is the loop having
 * stopped, and this is the loop running flat out while every client
 * waits behind it. Reporting them the same way would lose the
 * difference that matters for diagnosis -- one is "what is it blocked
 * on", the other is "what is it spending itself on".
 */
static void append_slow_pass_record(long long worst_millis, unsigned long long count,
                                     const char *activity)
{
	char line[1024];
	char esc_activity[STALL_ACTIVITY_MAX * 2];
	int len;

	json_escape(activity, esc_activity, sizeof(esc_activity));
	len = snprintf(line, sizeof(line),
	               "{\"ts\":%lld,\"event\":\"slow-pass\",\"worst_pass_ms\":%lld,"
	               "\"slow_passes\":%llu,\"activity\":\"%s\"}\n",
	               (long long)time(NULL), worst_millis, count, esc_activity);
	if (len <= 0)
		return;
	append_line(line, len);
}

/*
 * One end-to-end liveness question, asked the way a client would.
 *
 * The watchdog's other signals are all inferences from shared memory:
 * the loop's heartbeat says it is turning, and the pass timer says how
 * long a turn takes. A daemon can satisfy both and serve nobody, which
 * is exactly what #247 was: the loop kept accepting connections and
 * answering none, so `quiet_for` stayed at zero and every pass looked
 * fast. Nothing was wrong with the measurements; they were answers to
 * different questions.
 *
 * This asks the only question that matters to a client, and it is
 * asked from a separate process that cannot itself be wedged by the
 * one it is watching. Returns 0 if the daemon answered, -1 otherwise.
 *
 * Deliberately the plain-HTTP health endpoint: no auth, no TLS, no
 * parsing beyond "did any byte come back". Anything more would be a
 * second HTTP client living in a diagnostic.
 */
static int probe_once(const char *host, int port, int timeout_ms)
{
	int fd;
	struct sockaddr_in sa;
	struct pollfd pfd;
	static const char req[] = "GET /v1/health HTTP/1.0\r\nConnection: close\r\n\r\n";
	char buf[64];
	int err = 0;
	socklen_t errlen = sizeof(err);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		/* A non-literal bind address (a name, or "*") is not something
		 * a diagnostic should be resolving. Probe the loopback the
		 * daemon is also listening on. */
		if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1)
			return -1;
	}

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 && errno != EINPROGRESS) {
		close(fd);
		return -1;
	}
	pfd.fd = fd;
	pfd.events = POLLOUT;
	if (poll(&pfd, 1, timeout_ms) != 1) {
		close(fd);
		return -1;
	}
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 || err != 0) {
		close(fd);
		return -1;
	}
	if (write(fd, req, sizeof(req) - 1) != (ssize_t)(sizeof(req) - 1)) {
		close(fd);
		return -1;
	}
	pfd.events = POLLIN;
	if (poll(&pfd, 1, timeout_ms) != 1) {
		close(fd);
		return -1;
	}
	if (read(fd, buf, sizeof(buf)) <= 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static void watchdog_main(pid_t watched)
{
	int in_stall = 0;
	long long stall_started_at = 0;
	long long last_reported = 0;
	unsigned long long last_slow_count = 0;
	long long last_slow_report = 0;
	long long last_probe = 0;
	long long unserved_since = 0;
	int probe_failures = 0;
	int in_service_stall = 0;

	/* Die with the parent: a watchdog outliving what it watches is
	 * just a stray process. */
	prctl(PR_SET_PDEATHSIG, SIGKILL);

	/*
	 * Issue #229: a watchdog that only runs when the machine is idle
	 * reports on the times nothing was wrong.
	 *
	 * The other live explanation for the sixteen-minute wedge leaving
	 * no record is that this process was starved alongside the daemon
	 * it watches -- two concurrent builds were running, and ADR-0165
	 * records builds starving cixd off the run queue before. Niceness
	 * is cheap insurance: this loop wakes twice a second, does two
	 * small /proc reads and usually nothing else, so giving it
	 * priority over ordinary work costs the machine nothing and buys
	 * the one observation that matters.
	 *
	 * Best-effort. A failure here is not worth refusing to watch at
	 * all, so the return value is deliberately not checked -- the
	 * watchdog is still useful at normal priority.
	 */
	(void)setpriority(PRIO_PROCESS, 0, -10);

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

		/*
		 * Separately from both: is the daemon actually SERVING? (#247)
		 *
		 * The two checks above ask whether the loop is turning and how
		 * long a turn takes. A daemon can pass both and answer nobody
		 * -- accepting connections and never replying keeps the
		 * heartbeat fresh and every pass short. That state left no
		 * record at all until this, and it is the state that requires
		 * a physical reset.
		 */
		if (g_shared->probe_port != 0 && now - last_probe >= PROBE_INTERVAL_SECONDS) {
			char host[sizeof(g_shared->probe_host)];
			int ok;

			last_probe = now;
			snprintf(host, sizeof(host), "%s", g_shared->probe_host);
			ok = probe_once(host, g_shared->probe_port, PROBE_TIMEOUT_MS) == 0;
			g_shared->probe_count++;
			if (ok) {
				g_shared->probe_last_ok_monotonic = now;
				if (in_service_stall) {
					append_record("service-recovered", now - unserved_since, watched);
					in_service_stall = 0;
				}
				probe_failures = 0;
			} else {
				if (probe_failures == 0)
					unserved_since = now;
				probe_failures++;
				if (!in_service_stall && probe_failures >= PROBE_FAILURES_FOR_STALL) {
					in_service_stall = 1;
					last_reported = now;
					append_record("service-stall", now - unserved_since, watched);
					/*
					 * ADR-0246 item 2: recording was already built;
					 * acting is the whole delta.
					 *
					 * The daemon has failed PROBE_FAILURES_FOR_STALL
					 * consecutive HTTP probes, which is the precise
					 * condition an operator cannot observe from
					 * anywhere else -- GET /v1/health is served by
					 * the loop that has stopped, so the endpoint
					 * whose job is to report this is structurally
					 * incapable of it.
					 *
					 * SIGUSR1 to pid 1 asks the supervisor to kill
					 * and restart the worker. Running containers
					 * survive that: they are reparented to the
					 * supervisor, and the next worker re-adopts them
					 * rather than starting duplicates.
					 */
					if (g_supervised) {
						append_record("restart-requested", now - unserved_since,
						               watched);
						if (kill(1, SIGUSR1) != 0)
							append_record("restart-request-failed",
							               now - unserved_since, watched);
					}
				} else if (in_service_stall && now - last_reported >= STALL_REPEAT_SECONDS) {
					last_reported = now;
					append_record("service-stall-continues", now - unserved_since, watched);
				}
			}
		}

		/*
		 * Separately from any stall: is the loop turning slowly?
		 *
		 * Reported at most once per STALL_REPEAT_SECONDS so a sustained
		 * bad patch produces a readable trail rather than a flood, and
		 * only when the count has actually moved -- a quiet daemon
		 * writes nothing at all.
		 */
		{
			unsigned long long slow_now = g_shared->slow_pass_count;

			if (slow_now != last_slow_count &&
			    (last_slow_report == 0 || now - last_slow_report >= STALL_REPEAT_SECONDS)) {
				char activity[STALL_ACTIVITY_MAX];

				snprintf(activity, sizeof(activity), "%s", g_shared->pass_worst_activity);
				append_slow_pass_record((long long)g_shared->pass_worst_work_millis, slow_now,
				                         activity);
				last_slow_report = now;
				last_slow_count = slow_now;
			}
		}
	}
}

/*
 * Tells the watchdog where to ask (#247). Called once the bind address
 * and port are final -- which is later than stallwatch_start(), since
 * daemon_config_init() can still override the port.
 */
static char g_pending_probe_host[64];
static int g_pending_probe_port;

void stallwatch_set_probe(const char *host, int port)
{
	/*
	 * Order-independent on purpose. This is called when the port
	 * becomes final, which is ~380 lines before stallwatch_start() runs
	 * -- so the first version silently returned with g_shared still
	 * NULL, and stallwatch_start()'s own memset would have wiped the
	 * value even if it had not. The probe never armed, and the only
	 * reason that was noticed at all is that the armed state is
	 * reported (#247); a detector that cannot say whether it is running
	 * is indistinguishable from one that is running and finding
	 * nothing.
	 *
	 * Remembering it until the shared page exists costs two statics and
	 * removes the ordering constraint entirely, rather than moving one
	 * call and leaving the trap for the next person.
	 */
	snprintf(g_pending_probe_host, sizeof(g_pending_probe_host), "%s",
	         host != NULL ? host : "127.0.0.1");
	g_pending_probe_port = port;
	if (g_shared == NULL)
		return;
	snprintf((char *)g_shared->probe_host, sizeof(g_shared->probe_host), "%s",
	         g_pending_probe_host);
	g_shared->probe_port = port;
}

int stallwatch_start(const char *records_path, int supervised)
{
	pid_t pid;

	g_supervised = supervised;

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
	/* Apply a probe target set before the shared page existed. */
	if (g_pending_probe_port != 0) {
		snprintf((char *)g_shared->probe_host, sizeof(g_shared->probe_host), "%s",
		         g_pending_probe_host);
		g_shared->probe_port = g_pending_probe_port;
	}

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

/*
 * Ends a loop pass and books what it cost (#229).
 *
 * Paired with stallwatch_heartbeat(), which marks the pass start right
 * after epoll_wait() returns -- so the interval measured here is the
 * work, with the wait excluded. That distinction is the whole point: an
 * idle daemon sleeps a second per pass and is perfectly healthy, while
 * a daemon spending a second of every pass on its own housekeeping is
 * one every client is queued behind.
 *
 * Cheap enough to call unconditionally: one clock read and, on the rare
 * slow pass, a few stores.
 */
void stallwatch_pass_end(void)
{
	long long work;

	if (g_shared == NULL || g_shared->pass_started_millis == 0)
		return;

	work = monotonic_millis() - g_shared->pass_started_millis;
	if (work < 0)
		return;
	g_shared->pass_last_work_millis = work;

	/*
	 * The high-water mark tracks EVERY pass, not only slow ones.
	 *
	 * Gating it on the threshold was the first version and it was
	 * wrong: on a healthy box the figure stayed 0, which is
	 * indistinguishable from the measurement being broken -- and that
	 * ambiguity showed up immediately, on a real host, where zero after
	 * a full package install could not be told apart from a bug. A
	 * number that only ever appears once something is already wrong
	 * cannot be trusted at the moment it finally appears.
	 *
	 * Tracking it always makes the reading meaningful when things are
	 * fine ("worst pass: 40 ms") and self-verifying: a permanent zero
	 * now means the timing is not running, which is worth knowing on
	 * its own.
	 */
	if (work > g_shared->pass_worst_work_millis) {
		g_shared->pass_worst_work_millis = work;
		/* What it was doing, if anything named itself. Copied at the
		 * moment of the worst pass rather than read later, when it
		 * would have moved on. */
		snprintf(g_shared->pass_worst_activity, STALL_ACTIVITY_MAX, "%s",
		         g_shared->pass_activity);
	}
	if (work >= SLOW_PASS_MILLIS)
		g_shared->slow_pass_count++;
}

void stallwatch_heartbeat(void)
{
	if (g_shared == NULL)
		return;
	g_shared->heartbeat_monotonic = monotonic_seconds();
	g_shared->heartbeat_seq++;
	g_shared->pass_started_millis = monotonic_millis();
	g_shared->pass_activity[0] = '\0';
}

void stallwatch_activity(const char *what)
{
	if (g_shared == NULL || what == NULL)
		return;
	/*
	 * Time first, then the string. The watchdog treats a non-empty
	 * activity as "a request is in flight" and reads the start time to
	 * age it, so writing them the other way round leaves a window
	 * where it would age a request against a zero timestamp and report
	 * a stall of about fifty-six years.
	 */
	g_shared->activity_started_monotonic = monotonic_seconds();
	snprintf(g_shared->activity, STALL_ACTIVITY_MAX, "%s", what);
	/* Survives activity_clear(), so the pass can still be attributed
	 * when it is booked at the end (#229). */
	snprintf(g_shared->pass_activity, STALL_ACTIVITY_MAX, "%s", what);
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
void stallwatch_write_loop_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "worst_pass_ms");
	jw_int(w, g_shared != NULL ? (long long)g_shared->pass_worst_work_millis : 0);
	jw_key(w, "last_pass_ms");
	jw_int(w, g_shared != NULL ? (long long)g_shared->pass_last_work_millis : 0);
	jw_key(w, "slow_passes");
	jw_int(w, g_shared != NULL ? (long long)g_shared->slow_pass_count : 0);
	jw_key(w, "slow_pass_threshold_ms");
	jw_int(w, SLOW_PASS_MILLIS);
	jw_key(w, "worst_pass_activity");
	jw_str(w, (g_shared != NULL && g_shared->pass_worst_activity[0] != '\0')
	                  ? g_shared->pass_worst_activity
	                  : "");
	/*
	 * Whether the service probe is actually armed, and when it last got
	 * an answer (#247).
	 *
	 * Without this, "no service-stall records" means either "the daemon
	 * has been answering" or "nothing has ever asked it" -- and those
	 * are opposite conclusions. An operator reading this should be able
	 * to tell a working detector from a silent one without reading the
	 * source.
	 */
	jw_key(w, "service_probe_armed");
	jw_bool(w, g_shared != NULL && g_shared->probe_port != 0);
	jw_key(w, "service_probes_sent");
	jw_int(w, g_shared != NULL ? (long long)g_shared->probe_count : 0);
	jw_key(w, "service_last_ok_seconds_ago");
	if (g_shared != NULL && g_shared->probe_last_ok_monotonic != 0)
		jw_int(w, monotonic_seconds() - (long long)g_shared->probe_last_ok_monotonic);
	else
		jw_null(w);
	jw_obj_close(w);
}

void stallwatch_write_json(struct json_writer *w, int limit)
{
	char *buf = NULL;
	long size;
	FILE *f;
	char *lines[STALL_REPORT_MAX];
	int count = 0;                  /* records held, never above the ring */
	int head = 0;                   /* ring slot the next record goes into */
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

	/*
	 * A RING, not a prefix.
	 *
	 * This loop used to stop at the array's size, which kept the OLDEST
	 * records in the window and silently discarded every newer one --
	 * the exact opposite of what a watchdog report is for, and it hid
	 * itself perfectly: the endpoint answered 200 with a plausible
	 * array of real records every time. Measured on 192.168.15.95:
	 * GET /v1/system/stalls?limit=1000 returned exactly 256 records --
	 * the array bound, hit precisely -- whose newest was 2026-08-31
	 * 22:59:09. With the ring, the same call returns records ending
	 * 2026-09-01 15:08:03. A full extra day of real records had been
	 * unreachable.
	 *
	 * What this bug did NOT explain, though it was briefly taken to:
	 * the wedges of 2026-09-04/05 are absent from the file itself, not
	 * merely from the report. The reader was fixed on the theory that
	 * it was hiding them; it was hiding a day of records, and the
	 * wedges are still missing (#229). Two bugs, one symptom, and the
	 * cheaper one was assumed to account for both.
	 *
	 * A report that drops what it cannot fit must drop the oldest.
	 */
	for (p = strtok(buf, "\n"); p != NULL; p = strtok(NULL, "\n")) {
		if (p[0] != '{')
			continue;
		lines[head] = p;
		head = (head + 1) % STALL_REPORT_MAX;
		if (count < STALL_REPORT_MAX)
			count++;
	}
	/*
	 * Newest first: the record anyone wants is the last one written.
	 * Spliced verbatim -- each line is already a complete JSON object,
	 * written by the watchdog, and re-parsing it here only to re-emit
	 * it would put a second formatter in the path of the one thing
	 * that has to be trustworthy.
	 */
	{
		int want = (limit > 0 && limit < count) ? limit : count;

		for (i = 0; i < want; i++) {
			/* Newest first: walk back from the slot before head. The
			 * bias keeps the index non-negative for every reachable
			 * head/i pair without a branch. */
			char *rec = lines[(head - 1 - i + 2 * STALL_REPORT_MAX) % STALL_REPORT_MAX];

			/* The separator is ours to place: nothing else writes into
			 * this array, and jw_raw_text() deliberately splices bytes
			 * without touching the writer's own item bookkeeping. */
			if (i > 0)
				jw_raw_text(w, ",", 1);
			jw_raw_text(w, rec, strlen(rec));
		}
	}

	free(buf);
	jw_arr_close(w);
}
