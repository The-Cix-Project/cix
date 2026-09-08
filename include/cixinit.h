#ifndef CIXINIT_H
#define CIXINIT_H

/*
 * ADR-0260: the wire format between cixd and cix-init, the process that
 * is pid 1 in every container.
 *
 * This header is the single definition of that format and is included
 * by both sides: daemon/src (cixd builds the table and reads reports)
 * and init/src (cix-init reads the table and writes reports). It
 * includes NOTHING, on purpose -- cix-init is freestanding, compiled
 * -nostdlib with no libc and no system headers (probe-tcc-conformance
 * 25 measured that no libc.a exists on this platform, by ADR-0251's
 * design, and 26/27 measured that the pinned tcc builds a freestanding
 * executable that runs). So every type here is a plain C type both
 * sides agree on: both are TCC on x86_64 Linux, where int is 32 bits
 * and every field below is naturally aligned. There is no packing to
 * get wrong, which matters because TCC ignores __attribute__((packed))
 * entirely (ADR-0008). test_cix_init pins the struct sizes so a change
 * on one side cannot silently desynchronise the other.
 *
 * TRANSPORT. Two AF_UNIX SOCK_SEQPACKET sockets, created by cixd before
 * clone3() and inherited by cix-init, whose fd numbers cix-init learns
 * from its own argv (never fixed numbers -- at exec time the child
 * still holds the diag pipe, the network and userns sync pipes and the
 * overlay fds, any of which may already BE 3; and never the environment,
 * which every service would inherit):
 *
 *   argv[1]     the control socket: cixd -> cix-init. First a
 *               cixinit_hello, then hello.service_count cixinit_service
 *               records, then cixinit_command records for the life of
 *               the container. One record per message; the reader
 *               tells them apart by size.
 *   argv[2]     the report socket: cix-init -> cixd. cixinit_report
 *               records, one per message.
 *   argv[3 + i] service i's stdout/stderr pipe (a pipe, not a socket:
 *               output is a byte stream). cix-init dup2()s it onto 1 and
 *               2 in the child before execve(), which is what gives every
 *               captured line its service attribution at zero cost.
 *
 * SEQPACKET rather than a pipe for the two record streams because a
 * pipe is a byte stream: two 16-byte reports written back to back
 * arrive as one 32-byte read, and record-at-a-time logic would drop
 * the second. Message framing is the kernel's job here.
 *
 * cix-init's own stdout/stderr are whatever the container's were --
 * the daemon's ordinary capture pipe -- so its diagnostics land in the
 * container's captured output like any other line, attributed to
 * "cix-init".
 *
 * EXIT STATUS. Build containers and every test read the CONTAINER's
 * exit status, and with cix-init as pid 1 that status is cix-init's.
 * The rule, so container_decode_exit_status() is changed deliberately:
 *
 *   - ended by a service (nothing left to supervise, an on_exit of
 *     fail-container, a failed oneshot): cix-init exits with that
 *     service's exit code; a signal death becomes 128 + signal. The
 *     exact kind and value are in that service's CIXINIT_EV_EXITED
 *     report, which is where the daemon reads them from.
 *   - ended by an orderly shutdown (SIGTERM to pid 1, or
 *     CIXINIT_OP_SHUTDOWN): services are stopped in reverse dependency
 *     order and cix-init exits 0. The container did what it was asked.
 *   - cix-init itself could not start (bad argv, no table, a table the
 *     hello did not announce): exit CIXINIT_EXIT_BAD_TABLE with the
 *     reason on its stderr.
 *   - a service's execve() failed: the child exits 140 + errno on the
 *     same encoding src/container.c uses for its own execve() failure,
 *     127 if errno does not fit, with the reason on the service's own
 *     pipe. That is a service exit like any other. A fork() that fails
 *     before there is a child is reported as CIXINIT_FAIL_SPAWN and
 *     counts as the service exiting 126.
 *
 * container_wait() will therefore always see term_signal == 0 for a
 * container that got as far as cix-init.
 */

#define CIXINIT_MAGIC 0x31584943  /* "CIX1", little-endian */
#define CIXINIT_VERSION 1

#define CIXINIT_MAX_SERVICES 16
#define CIXINIT_NAME_MAX 32
/*
 * argv as one buffer: argv[0] NUL argv[1] NUL ... NUL NUL. At most
 * CIXINIT_ARGC_MAX entries. The daemon enforces both; cix-init refuses a
 * record that violates either rather than reading past the buffer.
 */
#define CIXINIT_ARGV_MAX 1024
#define CIXINIT_ARGC_MAX 32
#define CIXINIT_PATH_MAX 256

#define CIXINIT_EXIT_BAD_TABLE 125

/* cixinit_service.type */
#define CIXINIT_TYPE_ONESHOT 1  /* runs to completion and must exit 0 */
#define CIXINIT_TYPE_DAEMON 2   /* long-running, supervised */

/* cixinit_service.ready_kind */
#define CIXINIT_READY_NONE 0    /* ready when started */
#define CIXINIT_READY_TCP 1     /* a TCP connect to ready_addr_be:ready_port succeeds */
#define CIXINIT_READY_SOCKET 2  /* a connect to the unix socket at ready_path succeeds */
#define CIXINIT_READY_COMMAND 3 /* the argv in ready_path exits 0 */

/* cixinit_service.on_exit -- what cix-init does when the service exits */
#define CIXINIT_ON_EXIT_RESTART 1        /* daemons: restart after restart_delay_seconds */
#define CIXINIT_ON_EXIT_STOP 2           /* leave it exited; the container carries on */
#define CIXINIT_ON_EXIT_FAIL_CONTAINER 3 /* stop everything else, exit with its status */

struct cixinit_hello {
	int magic;         /* CIXINIT_MAGIC */
	int version;       /* CIXINIT_VERSION */
	int service_count; /* 1..CIXINIT_MAX_SERVICES; that many cixinit_service records follow */
	int reserved;
};

struct cixinit_service {
	char name[CIXINIT_NAME_MAX];
	char argv[CIXINIT_ARGV_MAX];
	int type;
	/*
	 * bit i set: this service starts only once service i is ready --
	 * a daemon that passed its probe (or started, if it has none), or
	 * a oneshot that exited 0. The daemon guarantees i < this record's
	 * own index, so the graph is acyclic by construction and cix-init
	 * never has to detect a cycle.
	 */
	unsigned int after_mask;
	int ready_kind;
	unsigned int ready_addr_be; /* CIXINIT_READY_TCP: the address to connect to, network order */
	int ready_port;             /* CIXINIT_READY_TCP */
	int ready_timeout_seconds;  /* how long to keep probing before reporting CIXINIT_FAIL_PROBE */
	char ready_path[CIXINIT_PATH_MAX]; /* SOCKET: the path; COMMAND: NUL-separated argv */
	int on_exit;
	int restart_delay_seconds; /* CIXINIT_ON_EXIT_RESTART: declared, never computed (ADR-0260) */
	int stop_signal;           /* sent first at shutdown; SIGKILL follows after stop_timeout_seconds */
	int stop_timeout_seconds;
	int uid; /* -1: inherit cix-init's own (the container's root) */
	int gid;
	int reserved[4];
};

/* cixinit_command.op */
#define CIXINIT_OP_START 1    /* start a stopped or exited service (clears an operator stop) */
#define CIXINIT_OP_STOP 2     /* stop it and hold it stopped: an operator override (ADR-0260) */
#define CIXINIT_OP_RESTART 3  /* stop then start */
#define CIXINIT_OP_SHUTDOWN 4 /* the orderly shutdown SIGTERM also triggers */

struct cixinit_command {
	int magic;   /* CIXINIT_MAGIC */
	int op;
	int service; /* index into the table; ignored by CIXINIT_OP_SHUTDOWN */
	int reserved;
};

/* cixinit_report.event */
#define CIXINIT_EV_UP 1         /* service == -1: cix-init has read the table. a = version */
#define CIXINIT_EV_STARTING 2   /* about to fork+exec */
#define CIXINIT_EV_STARTED 3    /* a = pid */
#define CIXINIT_EV_READY 4      /* the probe passed, or it started and has none */
#define CIXINIT_EV_EXITED 5     /* a = CIXINIT_EXIT_KIND_*, b = exit code or signal */
#define CIXINIT_EV_RESTART_IN 6 /* a = seconds until the declared restart */
#define CIXINIT_EV_STOPPED 7    /* held stopped by an operator's CIXINIT_OP_STOP */
#define CIXINIT_EV_FAILED 8     /* a = CIXINIT_FAIL_* */
#define CIXINIT_EV_SHUTDOWN 9   /* service == -1: a = 0 beginning, 1 complete */

/* cixinit_report.a for CIXINIT_EV_EXITED -- the CLD_* values, restated so
 * cix-init need not include <signal.h> */
#define CIXINIT_EXIT_KIND_EXITED 1
#define CIXINIT_EXIT_KIND_KILLED 2
#define CIXINIT_EXIT_KIND_DUMPED 3

/* cixinit_report.a for CIXINIT_EV_FAILED */
#define CIXINIT_FAIL_PROBE 1   /* the ready probe did not pass in ready_timeout_seconds */
#define CIXINIT_FAIL_ONESHOT 2 /* a oneshot exited non-zero (b in the preceding EXITED) */
#define CIXINIT_FAIL_SPAWN 3      /* fork() itself failed; b = -errno; the service's status is 126 */
#define CIXINIT_FAIL_DEPENDENCY 4 /* a service it is `after` can never become ready; b = that index */

struct cixinit_report {
	int service; /* index into the table, or -1 for cix-init itself */
	int event;
	int a;
	int b;
};

#endif /* CIXINIT_H */
