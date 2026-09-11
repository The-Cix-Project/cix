/*
 * cix-init -- pid 1 in every container (ADR-0260).
 *
 * It reads a table of services from cixd over an inherited socket,
 * starts them in dependency order, probes their readiness, restarts
 * the ones declared restartable, stops them in reverse order at
 * shutdown, and reports every state change back over a second socket.
 * It holds no policy of its own: every delay, signal, timeout and
 * probe it acts on is a number from the table (ADR-0260: "the executor
 * stays an executor"). It reads no file, opens no listener and does
 * nothing on the network beyond the ready probes it is told to run.
 *
 * THIS FILE INCLUDES NO HEADERS, ON PURPOSE. cix-init is freestanding:
 * compiled -nostdlib -static, with its own _start, its own syscall
 * trampoline and its own memcpy, and linked against nothing. That is
 * what makes it the one binary that cannot skew against whichever glibc
 * an image happens to carry -- and it is the only route to a static
 * binary on this platform at all, because ADR-0251 keeps libc.a out of
 * every artifact by design (probe-tcc-conformance 25, 26, 27 are the
 * measurements). If you find yourself adding an #include here, that is
 * the design being undone, not tidied. The syscall layer below is
 * cix-init's own and nothing else in Cix may use it: every other Cix
 * binary links glibc dynamically, by the toolchain rule in CLAUDE.md,
 * and a second freestanding program is not to appear by imitation.
 *
 * Two rules from the supervisor this descends from (ADR-0246's
 * cix-init, removed in 594ec1fc, whose reap loop and pending ring this
 * keeps) still govern every line:
 *
 *   1. Never return from cix_main(). pid 1 returning is a kernel panic
 *      in a container just as on a host ("Attempted to kill init!",
 *      #131); _start therefore ends in exit_group, never a return.
 *
 *   2. Never block indefinitely. The report socket is non-blocking and
 *      backed by a bounded ring, so a daemon that stops reading cannot
 *      hang the container's pid 1; every wait is WNOHANG; every poll
 *      has a timeout.
 *
 * The wire format is include/cixinit.h, shared with the daemon; that
 * header is the one place the two sides are defined.
 */

typedef unsigned long size_t;
typedef long ssize_t;

#include "cixinit.h"

/* ------------------------------------------------------------------ */
/* The three pieces that must be assembly: entry, syscall, sigreturn.  */

__asm__(
	".text\n"
	".globl _start\n"
	"_start:\n"
	"	xor %rbp, %rbp\n"
	"	mov (%rsp), %rdi\n"
	"	lea 8(%rsp), %rsi\n"
	"	and $-16, %rsp\n"
	"	call cix_main\n"
	"	mov %eax, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n"
	"	hlt\n"
);

/*
 * (n, a, b, c, d, e, f) arrive per the SysV ABI in rdi rsi rdx rcx r8 r9
 * and the stack; the kernel wants rax rdi rsi rdx r10 r8 r9. Written in
 * asm rather than as inline constraints so nothing depends on which
 * constraint letters the compiler implements.
 */
__asm__(
	".text\n"
	".globl cix_syscall6\n"
	"cix_syscall6:\n"
	"	mov %rdi, %rax\n"
	"	mov %rsi, %rdi\n"
	"	mov %rdx, %rsi\n"
	"	mov %rcx, %rdx\n"
	"	mov %r8, %r10\n"
	"	mov %r9, %r8\n"
	"	mov 8(%rsp), %r9\n"
	"	syscall\n"
	"	ret\n"
);
long cix_syscall6(long n, long a, long b, long c, long d, long e, long f);

/* The kernel returns from a signal handler through this (SA_RESTORER). */
__asm__(
	".text\n"
	".globl cix_sigreturn\n"
	"cix_sigreturn:\n"
	"	mov $15, %eax\n"
	"	syscall\n"
);
void cix_sigreturn(void);

/* ------------------------------------------------------------------ */
/* Linux x86_64 ABI: the numbers and shapes this file needs.           */

#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_poll 7
#define SYS_rt_sigaction 13
#define SYS_dup2 33
#define SYS_getpid 39
#define SYS_socket 41
#define SYS_connect 42
#define SYS_fork 57
#define SYS_execve 59
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_fcntl 72
#define SYS_setgid 106
#define SYS_setuid 105
#define SYS_setgroups 116
#define SYS_prctl 157
#define SYS_clock_gettime 228
#define SYS_exit_group 231
#define SYS_openat 257
#define SYS_pipe2 293
#define SYS_close_range 436
#define CLOSE_RANGE_CLOEXEC 4

#define EINTR 4
#define EAGAIN 11
#define EINPROGRESS 115

#define O_RDWR 2
#define O_NONBLOCK 04000
#define O_CLOEXEC 02000000
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
#define AT_FDCWD (-100)

#define WNOHANG 1
#define POLLIN 1
#define POLLNVAL 32

#define SIGHUP 1
#define SIGINT 2
#define SIGKILL 9
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17
#define SA_NOCLDSTOP 0x00000001UL
#define SA_RESTORER 0x04000000UL
#define SA_RESTART 0x10000000UL
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

#define AF_UNIX 1
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000

#define CLOCK_MONOTONIC 1
#define PR_SET_CHILD_SUBREAPER 36

struct k_sigaction {
	void (*handler)(int);
	unsigned long flags;
	void (*restorer)(void);
	unsigned long mask;
};

struct pollfd {
	int fd;
	short events;
	short revents;
};

struct timespec {
	long tv_sec;
	long tv_nsec;
};

struct sockaddr_in {
	unsigned short sin_family;
	unsigned short sin_port; /* network order */
	unsigned int sin_addr;   /* network order */
	unsigned char sin_zero[8];
};

struct sockaddr_un {
	unsigned short sun_family;
	char sun_path[108];
};

static long sc0(long n) { return cix_syscall6(n, 0, 0, 0, 0, 0, 0); }
static long sc1(long n, long a) { return cix_syscall6(n, a, 0, 0, 0, 0, 0); }
static long sc2(long n, long a, long b) { return cix_syscall6(n, a, b, 0, 0, 0, 0); }
static long sc3(long n, long a, long b, long c) { return cix_syscall6(n, a, b, c, 0, 0, 0); }
static long sc4(long n, long a, long b, long c, long d) { return cix_syscall6(n, a, b, c, d, 0, 0); }

/* ------------------------------------------------------------------ */
/* What the compiler calls behind our back: struct copies and clears   */
/* become memcpy/memset. Defined here so the link has them.            */

void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;

	while (n--)
		*dp++ = *sp++;
	return d;
}

void *memset(void *d, int c, size_t n)
{
	unsigned char *dp = d;

	while (n--)
		*dp++ = (unsigned char)c;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;

	if (dp < sp) {
		while (n--)
			*dp++ = *sp++;
	} else {
		dp += n;
		sp += n;
		while (n--)
			*--dp = *--sp;
	}
	return d;
}

/* ------------------------------------------------------------------ */
/* Diagnostics: to fd 2, which is the container's own capture pipe.    */
/* No varargs anywhere -- with -nostdlib there is no libtcc1 to lean   */
/* on for va_arg, and a supervisor's messages do not need it.          */

static size_t cix_strlen(const char *s)
{
	size_t n = 0;

	while (s[n] != '\0')
		n++;
	return n;
}

static void fd_str(int fd, const char *s)
{
	size_t len = cix_strlen(s);
	size_t off = 0;

	while (off < len) {
		long n = sc3(SYS_write, fd, (long)(s + off), (long)(len - off));

		if (n == -EINTR)
			continue;
		if (n <= 0)
			return;
		off += (size_t)n;
	}
}

static void fd_num(int fd, long v)
{
	char buf[24];
	int i = 23;
	int neg = v < 0;

	buf[i] = '\0';
	if (v == 0)
		buf[--i] = '0';
	if (neg)
		v = -v;
	while (v > 0) {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	}
	if (neg)
		buf[--i] = '-';
	fd_str(fd, buf + i);
}

static void say(const char *a) { fd_str(2, "cix-init: "); fd_str(2, a); fd_str(2, "\n"); }
static void say2(const char *a, const char *b) { fd_str(2, "cix-init: "); fd_str(2, a); fd_str(2, b); fd_str(2, "\n"); }
static void say_num(const char *a, long n, const char *b)
{
	fd_str(2, "cix-init: ");
	fd_str(2, a);
	fd_num(2, n);
	fd_str(2, b);
	fd_str(2, "\n");
}

static void die(const char *why)
{
	say(why);
	sc1(SYS_exit_group, CIXINIT_EXIT_BAD_TABLE);
	for (;;)
		;
}

/* ------------------------------------------------------------------ */
/* Time and signals.                                                   */

/*
 * #361: MILLISECONDS, not seconds.
 *
 * Every deadline here used to come off a whole-second clock, so a
 * declared delay was honoured only to within a second of itself: the
 * true range for a declared N was (N-1, N]. Harmless at N=2 and not at
 * N=1, where an exit late in a second restarts after ~0 and the backoff
 * stops existing -- which is exactly the hammering it is there to
 * prevent, on a service crash-looping against a missing dependency.
 * Measured at v2.57.28: a declared 2-second delay restarted after
 * 1262 ms.
 *
 * The same truncation applied to the ready probe, the command probe and
 * the stop timeout, where a 1-second stop timeout could SIGKILL a
 * service that was about to exit cleanly.
 *
 * Every deadline field below is in these units, and a declared
 * *_seconds is multiplied by 1000 where it is STORED, never where it is
 * compared -- one conversion site per deadline, so a comparison cannot
 * be left in the wrong unit. The wire is unaffected: see
 * CIXINIT_EV_RESTART_IN's own site for the one field that reports
 * seconds outward.
 *
 * The main loop polls on a fixed 250 ms timeout rather than one derived
 * from these deadlines, so nothing else needed converting with them --
 * and that 250 ms is what gives a millisecond deadline real effect.
 *
 * long is 64-bit here, so a millisecond monotonic clock has no range
 * concern worth guarding.
 */
static long now_ms(void)
{
	struct timespec ts;

	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	sc2(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static volatile int g_sigchld;
static volatile int g_sigterm;

static void on_sigchld(int sig) { (void)sig; g_sigchld = 1; }
static void on_sigterm(int sig) { (void)sig; g_sigterm = 1; }

static void install_signal(int sig, void (*handler)(int))
{
	struct k_sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.handler = handler;
	sa.flags = SA_RESTORER | SA_RESTART | SA_NOCLDSTOP;
	sa.restorer = cix_sigreturn;
	sc4(SYS_rt_sigaction, sig, (long)&sa, 0, 8);
}

static void set_nonblock(int fd)
{
	long fl = sc2(SYS_fcntl, fd, F_GETFL);

	if (fl >= 0)
		sc3(SYS_fcntl, fd, F_SETFL, fl | O_NONBLOCK);
}

/* ------------------------------------------------------------------ */
/* The service table and its runtime state.                            */

enum svc_state {
	ST_PENDING = 0,  /* not started yet: waiting for its `after` set, or for a START */
	ST_STARTING,     /* running, probe not yet passed */
	ST_READY,        /* running and ready (daemon), the state dependents wait for */
	ST_EXITED,       /* exited and not restarting: a oneshot that finished, or on_exit: stop */
	ST_RESTART_WAIT, /* exited; respawning at restart_at */
	ST_STOPPED,      /* held stopped by an operator (ADR-0260: a visible, bounded override) */
	ST_FAILED        /* cannot run: a failed oneshot, a dead dependency, or fork() failing */
};

struct svc {
	struct cixinit_service def;
	enum svc_state state;
	long pid;             /* the running child, or 0 */
	int out_fd;           /* its stdout/stderr pipe, from argv */
	long probe_deadline;  /* monotonic ms: report CIXINIT_FAIL_PROBE after this */
	long probe_pid;       /* a running CIXINIT_READY_COMMAND child, or 0 */
	long probe_pid_deadline; /* monotonic ms */
	long restart_at;      /* monotonic ms: ST_RESTART_WAIT respawns at this point */
	long stop_sent_at;    /* monotonic ms: when stop_signal went out, 0 = not sent */
	int killed;           /* SIGKILL already sent after stop_timeout_seconds */
	int operator_stopped; /* CIXINIT_OP_STOP in force */
	int restart_after_stop; /* CIXINIT_OP_RESTART: go straight back to ST_PENDING on exit */
	int exit_kind;
	int exit_value;
	int restarts;
};

static struct svc g_svc[CIXINIT_MAX_SERVICES];
static int g_count;
static int g_control_fd = -1;
static int g_report_fd = -1;
static char **g_envp;

static int g_shutting_down;
static int g_exit_code; /* what cix-init exits with when it is done */

/* ------------------------------------------------------------------ */
/* Reports: a bounded ring drained without ever blocking.              */

#define PENDING_MAX 256
static struct cixinit_report g_pending[PENDING_MAX];
static int g_pending_head, g_pending_count, g_pending_dropped;

static void report(int service, int event, int a, int b)
{
	struct cixinit_report r;
	int slot;

	r.service = service;
	r.event = event;
	r.a = a;
	r.b = b;
	if (g_pending_count == PENDING_MAX) {
		g_pending_head = (g_pending_head + 1) % PENDING_MAX;
		g_pending_count--;
		g_pending_dropped++;
	}
	slot = (g_pending_head + g_pending_count) % PENDING_MAX;
	g_pending[slot] = r;
	g_pending_count++;
}

static void report_flush(void)
{
	while (g_pending_count > 0 && g_report_fd >= 0) {
		const struct cixinit_report *r = &g_pending[g_pending_head];
		long n = sc3(SYS_write, g_report_fd, (long)r, (long)sizeof(*r));

		if (n == (long)sizeof(*r)) {
			g_pending_head = (g_pending_head + 1) % PENDING_MAX;
			g_pending_count--;
			continue;
		}
		if (n == -EINTR)
			continue;
		return; /* EAGAIN: the daemon is not reading right now; next turn */
	}
}

/* ------------------------------------------------------------------ */
/* Reading the table.                                                  */

static int str_ok(const char *s, size_t max)
{
	size_t i;

	for (i = 0; i < max; i++)
		if (s[i] == '\0')
			return i > 0;
	return 0;
}

/* Counts a NUL-separated, double-NUL-terminated argv; -1 if malformed. */
static int argv_count(const char *buf, size_t max)
{
	size_t i = 0;
	int argc = 0;

	for (;;) {
		size_t start = i;

		while (i < max && buf[i] != '\0')
			i++;
		if (i >= max)
			return -1;
		if (i == start)
			return argc; /* the empty string that ends the list */
		argc++;
		if (argc > CIXINIT_ARGC_MAX)
			return -1;
		i++;
	}
}

static void argv_unpack(const char *buf, char **out, int max)
{
	int argc = 0;
	const char *p = buf;

	while (*p != '\0' && argc < max - 1) {
		out[argc++] = (char *)p;
		p += cix_strlen(p) + 1;
	}
	out[argc] = (char *)0;
}

/* One SEQPACKET message into buf; waits up to timeout_ms; returns bytes, 0 on EOF, <0 on error/timeout. */
static long control_read(void *buf, size_t size, int timeout_ms)
{
	struct pollfd p;
	long n;

	p.fd = g_control_fd;
	p.events = POLLIN;
	p.revents = 0;
	for (;;) {
		n = sc3(SYS_poll, (long)&p, 1, timeout_ms);
		if (n == -EINTR)
			continue;
		if (n <= 0)
			return -1;
		break;
	}
	for (;;) {
		n = sc3(SYS_read, g_control_fd, (long)buf, (long)size);
		if (n == -EINTR)
			continue;
		return n;
	}
}

static void validate_service(int idx, const struct cixinit_service *d)
{
	if (!str_ok(d->name, sizeof(d->name)))
		die("service record with an empty or unterminated name");
	if (argv_count(d->argv, sizeof(d->argv)) < 1)
		die("service record with no argv, or an argv that is not double-NUL terminated");
	if (d->type != CIXINIT_TYPE_ONESHOT && d->type != CIXINIT_TYPE_DAEMON)
		die("service record with an unknown type");
	if (d->after_mask >> idx != 0)
		die("service record depending on itself or on a later service -- the daemon must order the table");
	if (d->ready_kind < CIXINIT_READY_NONE || d->ready_kind > CIXINIT_READY_COMMAND)
		die("service record with an unknown ready kind");
	if (d->ready_kind == CIXINIT_READY_SOCKET && !str_ok(d->ready_path, sizeof(d->ready_path)))
		die("socket probe with no path");
	if (d->ready_kind == CIXINIT_READY_COMMAND && argv_count(d->ready_path, sizeof(d->ready_path)) < 1)
		die("command probe with no argv");
	if (d->on_exit < CIXINIT_ON_EXIT_RESTART || d->on_exit > CIXINIT_ON_EXIT_FAIL_CONTAINER)
		die("service record with an unknown on_exit");
	if (d->stop_signal < 1 || d->stop_signal > 64)
		die("service record with an invalid stop signal");
}

static void read_table(void)
{
	struct cixinit_hello hello;
	long n;
	int i;

	n = control_read(&hello, sizeof(hello), 5000);
	if (n != (long)sizeof(hello))
		die("no hello on the control socket within 5 seconds");
	if (hello.magic != CIXINIT_MAGIC)
		die("hello with the wrong magic");
	if (hello.version != CIXINIT_VERSION)
		die("hello with a version this cix-init does not speak");
	if (hello.service_count < 1 || hello.service_count > CIXINIT_MAX_SERVICES)
		die("hello announcing an impossible service count");
	g_count = hello.service_count;

	for (i = 0; i < g_count; i++) {
		struct svc *s = &g_svc[i];

		n = control_read(&s->def, sizeof(s->def), 5000);
		if (n != (long)sizeof(s->def))
			die("a service record did not arrive, or arrived the wrong size");
		validate_service(i, &s->def);
		s->state = ST_PENDING;
		s->pid = 0;
		s->probe_pid = 0;
		s->stop_sent_at = 0;
		s->killed = 0;
		s->operator_stopped = 0;
		s->restart_after_stop = 0;
		s->restarts = 0;
	}
}

/* ------------------------------------------------------------------ */
/* Starting a service.                                                 */

static void child_exec(struct svc *s)
{
	char *argv[CIXINIT_ARGC_MAX + 1];
	long rc;

	/* Its own output pipe becomes stdout and stderr; cix-init's stay behind. */
	sc2(SYS_dup2, s->out_fd, 1);
	sc2(SYS_dup2, s->out_fd, 2);
	/*
	 * An init gives its children default dispositions. Handlers do
	 * not survive execve but SIG_IGN does, and cix-init ignores
	 * SIGPIPE for its own sake -- a service inheriting that would
	 * have `producer | head` spin in every shell script it runs.
	 */
	install_signal(SIGPIPE, SIG_DFL);

	if (s->def.gid >= 0) {
		if (sc2(SYS_setgroups, 0, 0) != 0 || sc1(SYS_setgid, s->def.gid) != 0) {
			fd_str(2, "cix-init: cannot set the declared gid\n");
			sc1(SYS_exit_group, 126);
		}
	}
	if (s->def.uid >= 0 && sc1(SYS_setuid, s->def.uid) != 0) {
		fd_str(2, "cix-init: cannot set the declared uid\n");
		sc1(SYS_exit_group, 126);
	}

	argv_unpack(s->def.argv, argv, CIXINIT_ARGC_MAX + 1);
	rc = sc3(SYS_execve, (long)argv[0], (long)argv, (long)g_envp);
	/*
	 * Only reached on failure. 140 + errno is src/container.c's own
	 * encoding for the same event, so the daemon decodes both alike.
	 */
	fd_str(2, "cix-init: execve(");
	fd_str(2, argv[0]);
	fd_str(2, ") failed, errno ");
	fd_num(2, -rc);
	fd_str(2, "\n");
	if (rc < 0 && -rc <= 115)
		sc1(SYS_exit_group, 140 + (-rc));
	sc1(SYS_exit_group, 127);
}

static void start_service(int idx)
{
	struct svc *s = &g_svc[idx];
	long pid;

	report(idx, CIXINIT_EV_STARTING, 0, 0);
	pid = sc0(SYS_fork);
	if (pid < 0) {
		s->state = ST_FAILED;
		s->exit_kind = CIXINIT_EXIT_KIND_EXITED;
		s->exit_value = 126;
		g_exit_code = 126;
		report(idx, CIXINIT_EV_FAILED, CIXINIT_FAIL_SPAWN, (int)pid);
		say2("fork failed for service ", s->def.name);
		return;
	}
	if (pid == 0) {
		child_exec(s);
		/* not reached */
	}
	s->pid = pid;
	s->state = ST_STARTING;
	s->stop_sent_at = 0;
	s->killed = 0;
	s->probe_pid = 0;
	s->probe_deadline = now_ms() +
	                    (s->def.ready_timeout_seconds > 0 ? s->def.ready_timeout_seconds : 30) *
	                            1000L;
	report(idx, CIXINIT_EV_STARTED, (int)pid, 0);
	if (s->def.ready_kind == CIXINIT_READY_NONE) {
		s->state = ST_READY;
		report(idx, CIXINIT_EV_READY, 0, 0);
	}
}

/* ------------------------------------------------------------------ */
/* Readiness probes.                                                   */

static int probe_connect(int family, const void *addr, long addrlen)
{
	long fd = sc3(SYS_socket, family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	long rc;

	if (fd < 0)
		return 0;
	rc = sc3(SYS_connect, fd, (long)addr, addrlen);
	sc1(SYS_close, fd);
	/*
	 * A local connect completes synchronously on success or refusal;
	 * EINPROGRESS only means "not this turn", and the next turn tries
	 * again from scratch rather than tracking a half-open socket in
	 * pid 1.
	 */
	return rc == 0;
}

static void probe_command_spawn(struct svc *s)
{
	long pid = sc0(SYS_fork);

	if (pid < 0)
		return; /* try again next turn */
	if (pid == 0) {
		char *argv[CIXINIT_ARGC_MAX + 1];
		long devnull = sc4(SYS_openat, AT_FDCWD, (long)"/dev/null", O_RDWR, 0);

		if (devnull >= 0) {
			sc2(SYS_dup2, devnull, 1);
			sc2(SYS_dup2, devnull, 2);
		}
		install_signal(SIGPIPE, SIG_DFL);
		if (s->def.gid >= 0) {
			sc2(SYS_setgroups, 0, 0);
			sc1(SYS_setgid, s->def.gid);
		}
		if (s->def.uid >= 0)
			sc1(SYS_setuid, s->def.uid);
		argv_unpack(s->def.ready_path, argv, CIXINIT_ARGC_MAX + 1);
		sc3(SYS_execve, (long)argv[0], (long)argv, (long)g_envp);
		sc1(SYS_exit_group, 127);
	}
	s->probe_pid = pid;
	s->probe_pid_deadline = now_ms() + 5000L; /* ADR-0260: the command probe's own timeout */
}

static void probe_service(int idx, long now)
{
	struct svc *s = &g_svc[idx];
	int ready = 0;

	if (s->def.ready_kind == CIXINIT_READY_TCP) {
		struct sockaddr_in a;

		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_port = (unsigned short)(((s->def.ready_port & 0xff) << 8) | ((s->def.ready_port >> 8) & 0xff));
		a.sin_addr = s->def.ready_addr_be != 0 ? s->def.ready_addr_be : 0x0100007f; /* 127.0.0.1 */
		ready = probe_connect(AF_INET, &a, (long)sizeof(a));
	} else if (s->def.ready_kind == CIXINIT_READY_SOCKET) {
		struct sockaddr_un a;
		size_t len = cix_strlen(s->def.ready_path);

		memset(&a, 0, sizeof(a));
		a.sun_family = AF_UNIX;
		if (len >= sizeof(a.sun_path))
			len = sizeof(a.sun_path) - 1;
		memcpy(a.sun_path, s->def.ready_path, len);
		ready = probe_connect(AF_UNIX, &a, (long)(2 + len + 1));
	} else if (s->def.ready_kind == CIXINIT_READY_COMMAND) {
		if (s->probe_pid == 0)
			probe_command_spawn(s);
		else if (now >= s->probe_pid_deadline)
			sc2(SYS_kill, s->probe_pid, SIGKILL);
		return; /* the answer arrives through wait4 */
	}

	if (ready) {
		s->state = ST_READY;
		report(idx, CIXINIT_EV_READY, 0, 0);
	} else if (now >= s->probe_deadline) {
		/*
		 * Same behaviour as the container-level readiness check one
		 * layer up: say so, and let dependents proceed rather than
		 * hold the whole graph on a probe. The report is what makes
		 * the difference visible.
		 */
		report(idx, CIXINIT_EV_FAILED, CIXINIT_FAIL_PROBE, 0);
		say2("ready probe timed out, treating as ready: ", s->def.name);
		s->state = ST_READY;
		report(idx, CIXINIT_EV_READY, 1, 0);
	}
}

/* ------------------------------------------------------------------ */
/* Dependencies.                                                       */

static int dep_ready(const struct svc *d)
{
	if (d->def.type == CIXINIT_TYPE_ONESHOT)
		return d->state == ST_EXITED && d->exit_kind == CIXINIT_EXIT_KIND_EXITED && d->exit_value == 0;
	return d->state == ST_READY;
}

/* A dependency that can never become ready: nothing waiting on it will ever start. */
static int dep_dead(const struct svc *d)
{
	if (d->state == ST_FAILED || d->state == ST_STOPPED)
		return 1;
	if (d->state == ST_EXITED && !dep_ready(d))
		return 1;
	return 0;
}

static int deps_satisfied(int idx, int *dead_dep)
{
	const struct svc *s = &g_svc[idx];
	int j;

	*dead_dep = -1;
	for (j = 0; j < idx; j++) {
		if ((s->def.after_mask & (1u << j)) == 0)
			continue;
		if (dep_dead(&g_svc[j])) {
			*dead_dep = j;
			return 0;
		}
		if (!dep_ready(&g_svc[j]))
			return 0;
	}
	return 1;
}

static int has_running_dependent(int idx)
{
	int j;

	for (j = idx + 1; j < g_count; j++)
		if (g_svc[j].pid > 0 && (g_svc[j].def.after_mask & (1u << idx)) != 0)
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Stopping, restarting, shutting down.                                */

static void send_stop(struct svc *s, long now)
{
	if (s->pid <= 0 || s->stop_sent_at != 0)
		return;
	sc2(SYS_kill, s->pid, s->def.stop_signal);
	s->stop_sent_at = now;
}

static void escalate_stop(struct svc *s, long now)
{
	long timeout_ms = (s->def.stop_timeout_seconds > 0 ? s->def.stop_timeout_seconds : 10) * 1000L;

	if (s->pid <= 0 || s->stop_sent_at == 0 || s->killed)
		return;
	if (now - s->stop_sent_at >= timeout_ms) {
		sc2(SYS_kill, s->pid, SIGKILL);
		s->killed = 1;
	}
}

static void begin_shutdown(int exit_code)
{
	int i;

	if (g_shutting_down)
		return;
	g_shutting_down = 1;
	g_exit_code = exit_code;
	report(-1, CIXINIT_EV_SHUTDOWN, 0, 0);
	for (i = 0; i < g_count; i++) {
		struct svc *s = &g_svc[i];

		if (s->state == ST_PENDING || s->state == ST_RESTART_WAIT)
			s->state = ST_EXITED; /* nothing new starts now */
		if (s->probe_pid > 0)
			sc2(SYS_kill, s->probe_pid, SIGKILL);
	}
}

static int mapped_status(const struct svc *s)
{
	if (s->exit_kind == CIXINIT_EXIT_KIND_EXITED)
		return s->exit_value;
	return 128 + s->exit_value;
}

static void service_exited(int idx, int kind, int value, long now)
{
	struct svc *s = &g_svc[idx];

	s->pid = 0;
	s->exit_kind = kind;
	s->exit_value = value;
	s->stop_sent_at = 0;
	s->killed = 0;
	if (s->probe_pid > 0) {
		sc2(SYS_kill, s->probe_pid, SIGKILL);
		s->probe_pid = 0;
	}
	report(idx, CIXINIT_EV_EXITED, kind, value);

	if (g_shutting_down) {
		/*
		 * The exit code was fixed when the shutdown began -- 0 for an
		 * orderly one, the failing service's for fail-container -- and
		 * the services being stopped on the way out do not change it.
		 */
		s->state = ST_EXITED;
		return;
	}
	g_exit_code = mapped_status(s);
	if (s->restart_after_stop) {
		s->restart_after_stop = 0;
		s->state = ST_PENDING;
		return;
	}
	if (s->operator_stopped) {
		s->state = ST_STOPPED;
		report(idx, CIXINIT_EV_STOPPED, 0, 0);
		return;
	}

	if (s->def.type == CIXINIT_TYPE_ONESHOT) {
		if (kind == CIXINIT_EXIT_KIND_EXITED && value == 0) {
			s->state = ST_EXITED;
			return;
		}
		report(idx, CIXINIT_EV_FAILED, CIXINIT_FAIL_ONESHOT, mapped_status(s));
		say2("oneshot failed: ", s->def.name);
		if (s->def.on_exit == CIXINIT_ON_EXIT_FAIL_CONTAINER)
			begin_shutdown(mapped_status(s));
		else
			s->state = ST_FAILED;
		return;
	}

	switch (s->def.on_exit) {
	case CIXINIT_ON_EXIT_RESTART:
		s->state = ST_RESTART_WAIT;
		s->restart_at = now +
		                (s->def.restart_delay_seconds > 0 ? s->def.restart_delay_seconds : 1) *
		                        1000L;
		s->restarts++;
		/* The wire field stays in SECONDS -- cixd and test_cix_init both
		 * read it as one -- so only the internal deadline changed unit.
		 * The division is exact: restart_at - now is the declared seconds
		 * multiplied by 1000 and nothing else. */
		report(idx, CIXINIT_EV_RESTART_IN, (int)((s->restart_at - now) / 1000), s->restarts);
		break;
	case CIXINIT_ON_EXIT_FAIL_CONTAINER:
		begin_shutdown(mapped_status(s));
		break;
	default:
		s->state = ST_EXITED;
		break;
	}
}

static void reap_all(long now)
{
	for (;;) {
		int status = 0;
		long pid = sc4(SYS_wait4, -1, (long)&status, WNOHANG, 0);
		int i;

		if (pid == -EINTR)
			continue;
		if (pid <= 0)
			return;

		for (i = 0; i < g_count; i++) {
			struct svc *s = &g_svc[i];

			if (s->pid == pid) {
				int kind, value;

				if ((status & 0x7f) == 0) {
					kind = CIXINIT_EXIT_KIND_EXITED;
					value = (status >> 8) & 0xff;
				} else if ((status & 0x7f) != 0x7f) {
					kind = (status & 0x80) ? CIXINIT_EXIT_KIND_DUMPED : CIXINIT_EXIT_KIND_KILLED;
					value = status & 0x7f;
				} else {
					break; /* stopped, not exited -- SA_NOCLDSTOP makes this rare */
				}
				service_exited(i, kind, value, now);
				break;
			}
			if (s->probe_pid == pid) {
				s->probe_pid = 0;
				if (s->state == ST_STARTING && status == 0) {
					s->state = ST_READY;
					report(i, CIXINIT_EV_READY, 0, 0);
				}
				break;
			}
		}
		/* Anything else is an orphan reparented to pid 1; reaping it was the job. */
	}
}

static void handle_command(const struct cixinit_command *c, long now)
{
	struct svc *s;

	if (c->magic != CIXINIT_MAGIC) {
		say("command with the wrong magic, ignored");
		return;
	}
	if (c->op == CIXINIT_OP_SHUTDOWN) {
		begin_shutdown(0);
		return;
	}
	if (c->service < 0 || c->service >= g_count) {
		say("command naming a service that is not in the table, ignored");
		return;
	}
	s = &g_svc[c->service];
	switch (c->op) {
	case CIXINIT_OP_START:
		s->operator_stopped = 0;
		if (s->pid <= 0 && s->state != ST_PENDING) {
			s->state = ST_PENDING;
			s->restart_after_stop = 0;
		}
		break;
	case CIXINIT_OP_STOP:
		s->operator_stopped = 1;
		s->restart_after_stop = 0;
		if (s->pid > 0) {
			send_stop(s, now);
		} else if (s->state != ST_STOPPED) {
			s->state = ST_STOPPED;
			report(c->service, CIXINIT_EV_STOPPED, 0, 0);
		}
		break;
	case CIXINIT_OP_RESTART:
		s->operator_stopped = 0;
		if (s->pid > 0) {
			s->restart_after_stop = 1;
			send_stop(s, now);
		} else {
			s->state = ST_PENDING;
		}
		break;
	default:
		say("unknown command op, ignored");
		break;
	}
}

static void drain_control(long now)
{
	for (;;) {
		union {
			struct cixinit_command cmd;
			unsigned char raw[sizeof(struct cixinit_service)];
		} u;
		long n = sc3(SYS_read, g_control_fd, (long)&u, (long)sizeof(u));

		if (n == -EINTR)
			continue;
		if (n == -EAGAIN)
			return;
		if (n == 0) {
			/*
			 * The daemon closed its end. In a real container this is
			 * not survivable for long: src/container.c sets
			 * PR_SET_PDEATHSIG SIGKILL before execve and cix-init
			 * inherits it, so a dead cixd means the kernel kills pid 1
			 * next -- the same fate today's payload has. Under the
			 * unit test there is no such tie, and either way the
			 * right thing is to log once, forget the socket and keep
			 * supervising for as long as the kernel allows.
			 */
			say("control socket closed by the daemon -- no further commands can arrive");
			sc1(SYS_close, g_control_fd);
			g_control_fd = -1;
			return;
		}
		if (n < 0)
			return;
		if (n == (long)sizeof(struct cixinit_command))
			handle_command(&u.cmd, now);
		else
			say("control message of an unexpected size, ignored");
	}
}

/* ------------------------------------------------------------------ */
/* One turn of the loop.                                               */

static void schedule(long now)
{
	int i;

	for (i = 0; i < g_count; i++) {
		struct svc *s = &g_svc[i];
		int dead;

		switch (s->state) {
		case ST_PENDING:
			if (s->operator_stopped)
				break;
			if (deps_satisfied(i, &dead)) {
				start_service(i);
			} else if (dead >= 0) {
				s->state = ST_FAILED;
				report(i, CIXINIT_EV_FAILED, CIXINIT_FAIL_DEPENDENCY, dead);
				say2("cannot start, a service it is after will never be ready: ", s->def.name);
				if (s->def.on_exit == CIXINIT_ON_EXIT_FAIL_CONTAINER)
					begin_shutdown(g_svc[dead].state == ST_FAILED ? mapped_status(&g_svc[dead]) : 1);
			}
			break;
		case ST_RESTART_WAIT:
			if (now >= s->restart_at)
				start_service(i);
			break;
		case ST_STARTING:
			if (s->stop_sent_at == 0)
				probe_service(i, now);
			escalate_stop(s, now);
			break;
		case ST_READY:
			escalate_stop(s, now);
			break;
		default:
			break;
		}
	}
}

static void shutdown_step(long now)
{
	int i;

	for (i = g_count - 1; i >= 0; i--) {
		struct svc *s = &g_svc[i];

		if (s->pid <= 0)
			continue;
		if (has_running_dependent(i))
			continue; /* its dependents go first: reverse dependency order */
		send_stop(s, now);
		escalate_stop(s, now);
	}
}

/* The one exit: flush what can be flushed, say what could not be, go. */
static void finish(void)
{
	report_flush();
	if (g_pending_dropped > 0)
		say_num("exiting with ", g_pending_dropped, " reports dropped because the daemon was not reading");
	if (g_pending_count > 0)
		say_num("exiting with ", g_pending_count, " reports undelivered");
	sc1(SYS_exit_group, g_exit_code);
	for (;;)
		;
}

static int anything_left(void)
{
	int i;

	for (i = 0; i < g_count; i++) {
		const struct svc *s = &g_svc[i];

		if (s->pid > 0 || s->state == ST_RESTART_WAIT)
			return 1;
		if (s->state == ST_PENDING && !s->operator_stopped) {
			int dead;

			if (deps_satisfied(i, &dead) || dead < 0)
				return 1; /* startable now, or waiting on something still alive */
		}
	}
	return 0;
}

int cix_main(long argc, char **argv)
{
	int i;

	g_envp = argv + argc + 1;

	if (argc < 4)
		die("usage: cix-init <control-fd> <report-fd> <service-0-out-fd> ... -- this is exec'd by cixd, not by hand");

	{
		long fds[2 + CIXINIT_MAX_SERVICES];
		int nfd = (int)argc - 1;

		if (nfd > 2 + CIXINIT_MAX_SERVICES)
			die("more fds on argv than services can exist");
		for (i = 0; i < nfd; i++) {
			const char *p = argv[1 + i];
			long v = 0;

			if (*p == '\0')
				die("an empty fd argument");
			while (*p >= '0' && *p <= '9') {
				v = v * 10 + (*p - '0');
				p++;
			}
			if (*p != '\0')
				die("an fd argument that is not a number");
			fds[i] = v;
		}
		/*
		 * Nothing cix-init holds reaches a service. Every fd from 3 up
		 * is marked close-on-exec here, once: the ones the daemon
		 * handed over -- a service must not hold the control socket
		 * (it could read commands), the report socket (it could forge
		 * reports), or another service's output pipe (the daemon would
		 * never see that pipe's EOF until every service had exited) --
		 * and anything else that leaked in from whoever exec'd
		 * cix-init, which probe-cix-init/2 caught for real: a build
		 * container's shell had fds 4 and 5 open, the test inherited
		 * them, and a service listed them. dup2() clears the flag on
		 * the 1 and 2 a service is given, so a child keeps exactly
		 * what it should. One close_range() does the whole table; the
		 * loop is for a kernel without it.
		 */
		if (sc3(SYS_close_range, 3, ~0U, CLOSE_RANGE_CLOEXEC) != 0) {
			for (i = 3; i < 4096; i++)
				sc3(SYS_fcntl, i, F_SETFD, FD_CLOEXEC);
		}
		g_control_fd = (int)fds[0];
		g_report_fd = (int)fds[1];
		set_nonblock(g_report_fd);

		install_signal(SIGPIPE, SIG_IGN);
		install_signal(SIGCHLD, on_sigchld);
		install_signal(SIGTERM, on_sigterm);
		install_signal(SIGINT, on_sigterm);
		sc2(SYS_prctl, PR_SET_CHILD_SUBREAPER, 1);

		read_table();
		if (nfd - 2 != g_count)
			die("the number of output fds on argv does not match the table's service count");
		for (i = 0; i < g_count; i++)
			g_svc[i].out_fd = (int)fds[2 + i];
		set_nonblock(g_control_fd);
	}

	report(-1, CIXINIT_EV_UP, CIXINIT_VERSION, g_count);

	for (;;) {
		long now = now_ms();
		struct pollfd p;
		long rc;

		/*
		 * Reap unconditionally, not only when the SIGCHLD flag is set:
		 * standard signals coalesce, and a signal landing between the
		 * reap and the flag clear would be lost. WNOHANG costs nothing
		 * when there is nothing to reap, and pid 1 accumulating zombies
		 * is the one failure an init must not have.
		 */
		g_sigchld = 0;
		reap_all(now);

		if (g_sigterm) {
			g_sigterm = 0;
			begin_shutdown(0);
		}
		if (g_control_fd >= 0)
			drain_control(now);

		if (g_shutting_down) {
			shutdown_step(now);
			if (!anything_left()) {
				report(-1, CIXINIT_EV_SHUTDOWN, 1, 0);
				finish();
			}
		} else {
			schedule(now);
			if (!anything_left()) {
				/*
				 * Nothing left to supervise (ADR-0260): a supervisor
				 * idling over nothing would report the container
				 * running while it does nothing at all.
				 */
				finish();
			}
		}

		report_flush();

		p.fd = g_control_fd >= 0 ? g_control_fd : -1;
		p.events = POLLIN;
		p.revents = 0;
		rc = sc3(SYS_poll, (long)&p, g_control_fd >= 0 ? 1 : 0, 250);
		(void)rc; /* EINTR from SIGCHLD is the point; everything else is re-polled */
	}
	/* Not reached, and must not be: returning here is a kernel panic. */
}
