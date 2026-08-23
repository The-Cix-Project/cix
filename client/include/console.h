#ifndef CONSOLE_H
#define CONSOLE_H

#include "httpclient.h"

/*
 * Opens a real, fully-interactive console session into a running
 * container over this project's own hand-rolled WebSocket
 * (GET /v1/containers/{name}/console -- daemon/src/websocket.c,
 * daemon/src/exec.c, ADR-0043). Puts the LOCAL terminal (stdin/stdout,
 * if a tty) into raw mode for the session's duration, restoring it
 * before returning regardless of how the session ended -- first use
 * of raw mode on the CLI's own terminal in this codebase (distinct
 * from image/src/dual_console.c, which puts the *host's* two physical
 * consoles into raw mode while relaying a subprocess *through* them).
 *
 * Blocks until the remote shell exits or the connection drops. There
 * is no local escape sequence -- Ctrl-C is delivered as a literal
 * byte and passed straight through to the remote shell, not
 * intercepted here (raw mode disables ISIG, so this process never
 * even sees it as a signal), matching ssh/docker exec.
 *
 * cmd may be NULL for the daemon's own default (/usr/bin/bash).
 * Returns 0 on a clean session end, -1 on a connection/handshake
 * failure (a message is already printed to stderr).
 */
int thinc_console_run(const struct thinc_client *c, const char *container_name, const char *cmd);

/*
 * Live-tails the currently in-flight pkg build's own stdout/stderr
 * over GET /v1/pkg/build/log (task #676, ADR-0101) -- a one-way
 * relay, not an interactive session: no local raw mode, nothing is
 * ever sent back to the daemon but an eventual close. Prints each
 * chunk straight to stdout as it arrives (already includes whatever
 * of the build's output was captured before this attached). Blocks
 * until the build finishes (the daemon sends a real WS close frame)
 * or the connection drops. Returns 0 on a clean stream end, -1 on a
 * connection/handshake failure (e.g. no build currently in progress
 * -- a message is already printed to stderr).
 */
int thinc_pkg_build_log_run(const struct thinc_client *c);

#endif /* CONSOLE_H */
