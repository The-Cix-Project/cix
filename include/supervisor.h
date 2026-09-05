#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include <stdint.h>

/*
 * ADR-0246: the interface between the supervisor (cix-init, pid 1) and
 * the worker (cixd).
 *
 * It exists because of one consequence of making the worker disposable.
 * A container that survives a worker restart was reparented to the
 * supervisor when its previous parent died, so it is no longer the
 * worker's child: the worker can still watch it (a pidfd reports the
 * exit of any process, not just a child) but it can no longer REAP it,
 * and waitid() on a non-child fails with ECHILD. The exit status and
 * term signal are collected by the supervisor and would otherwise be
 * lost, leaving a re-adopted container that exits reporting a null
 * status with no reason -- a placeholder, not an answer.
 *
 * So the supervisor forwards every child it reaps. It forwards ALL of
 * them, unfiltered -- the dead worker, run_cmd() children orphaned by
 * it, stallwatch -- and lets the worker match them against what it
 * knows. Deciding which pids matter is the worker's job and needs the
 * registry, which the supervisor deliberately does not have.
 *
 * This header is the single definition of that wire format, included by
 * both sides. Every field is a fixed-width, naturally-aligned int32_t,
 * so the layout needs no packing pragma -- which matters here, because
 * TCC ignores __attribute__((packed)) entirely (ADR-0008) and a struct
 * that relied on it would be a silent ABI mismatch rather than a
 * compile error.
 */

/*
 * The name of the environment variable carrying the reap-channel fd
 * number into the worker. An fd rather than a path because the channel
 * is a socketpair created before the fork -- there is nothing in the
 * filesystem to name -- and an env var rather than a fixed fd number
 * because a hardcoded descriptor silently collides with whatever else
 * happens to be open.
 */
#define SUPERVISOR_REAP_FD_ENV "CIX_REAP_FD"

/*
 * The supervisor's read-only status port (ADR-0246 item 3). Deliberately
 * not 80/443: the whole point is that this answers when the worker,
 * which owns those, cannot. Nothing here is a control surface -- it
 * starts nothing and changes nothing -- so it is not a second
 * management path and does not touch the API-First Mandate.
 */
#define SUPERVISOR_STATUS_PORT 7847

/*
 * The field names deliberately avoid the si_* spellings.
 *
 * glibc's <signal.h> defines si_status and si_pid as MACROS expanding
 * into siginfo_t's internal union (si_status becomes
 * _sifields._sigchld.si_status), so a struct member of that name is
 * rewritten at every use and the compiler reports "field not found:
 * _sifields" against a struct that plainly has the field. Naming them
 * exit_kind/exit_value sidesteps the preprocessor entirely.
 */
struct supervisor_reap_record {
	int32_t pid;
	int32_t exit_kind;  /* CLD_EXITED, CLD_KILLED, CLD_DUMPED */
	int32_t exit_value; /* exit code when CLD_EXITED, else the signal */
	int32_t reserved;   /* keeps the record 16 bytes and explicitly named */
};

#endif /* SUPERVISOR_H */
