#ifndef STALLWATCH_H
#define STALLWATCH_H

#include "json.h"

/*
 * Issue #100: proof that the control plane stopped answering, written
 * by something that is not the control plane.
 *
 * This daemon is a single-threaded event loop, and on an installed host
 * it is the only way in -- no SSH, no shell (ADR-0034). It has been
 * observed accepting TCP while answering nothing for minutes at a time,
 * then recovering on its own, and the log store had NOTHING from inside
 * the window. That silence is structural: the loop that would record "I
 * am stuck" is the one that is stuck, so the only trace a wedge leaves
 * is its own absence, and nothing about it can be analysed afterwards.
 *
 * So the watcher is a separate PROCESS, forked at startup, and not a
 * signal handler: a handler runs on the stuck thread's stack under
 * async-signal-safety rules, which rules out doing anything genuinely
 * useful. A child can take its time, and can read what the kernel
 * already knows -- /proc/<pid>/wchan names the kernel function the
 * parent is blocked in, which is the single most useful fact about a
 * wedge and is unavailable from inside it.
 *
 * The parent heartbeats through a shared page; the child watches the
 * clock. Nothing here can block the parent: the shared page is written
 * with plain stores, and every record is written by the child.
 */

/*
 * Forks the watchdog. records_path is a JSON-lines file the child
 * appends to -- it outlives the stall, the daemon and a reboot, which
 * is the entire point. Returns 0 on success, -1 if the watchdog could
 * not be started (the daemon carries on regardless: a diagnostic that
 * refuses to let the system run is not a diagnostic).
 */
/*
 * ADR-0246: `supervised` says whether a supervisor (cix-init) is running
 * as pid 1 and can be asked to restart a wedged worker. When it is, a
 * sustained service stall sends SIGUSR1 to pid 1 in addition to being
 * recorded; when it is not, the watchdog only records, as before.
 */
int stallwatch_start(const char *records_path, int supervised);

/*
 * Where the watchdog should ask whether the daemon is serving (#247).
 * Call once the bind address and port are final; until then the
 * watchdog does not probe. A wedge that keeps accepting connections and
 * answers none satisfies every other signal this module has, so this is
 * the only one that can see it.
 */
void stallwatch_set_probe(const char *host, int port);

/* Called once per event-loop iteration. A plain store to shared memory
 * -- no syscall, no lock, nothing that can itself stall. */
void stallwatch_heartbeat(void);

/*
 * Ends a loop pass, booking how long its WORK took (the epoll wait
 * excluded). Paired with stallwatch_heartbeat(), which marks the start.
 *
 * This is the measurement that catches a daemon which is alive and
 * unusable at once -- the loop turning steadily while spending seconds
 * of every pass on its own work, with every client queued behind it.
 * The heartbeat cannot see that by construction, and did not: a
 * 247-second outage produced no record at all (#229).
 */
void stallwatch_pass_end(void);

/*
 * Writes what the loop itself has been costing: the worst single pass
 * seen, how many passes crossed the slow threshold, and the most recent
 * pass -- all in milliseconds. Distinct from the stall records, which
 * say when it stopped; this says how well it is running when it has
 * not.
 */
void stallwatch_write_loop_json(struct json_writer *w);

/*
 * What the loop is working on right now, so a stall record can name it.
 * Set as a request begins and cleared when it finishes; anything left
 * set while the loop goes quiet is, by construction, the thing that did
 * not come back.
 */
void stallwatch_activity(const char *what);
void stallwatch_activity_clear(void);

/* Writes the most recent records (newest first) as a JSON array. */
void stallwatch_write_json(struct json_writer *w, int limit);

#endif /* STALLWATCH_H */
