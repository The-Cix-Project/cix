#ifndef PROCTITLE_H
#define PROCTITLE_H

/*
 * setproctitle-style rewriting of this process's argv area, so that
 * /proc/<pid>/cmdline -- and the command_line field of
 * GET /v1/system/processes -- shows a chosen string (#456).
 *
 * A forked child that never exec()s inherits the parent's cmdline
 * verbatim ("/bin/cixd --init-mode --slot=b"), which says nothing about
 * what the child actually is: the procfuse server and the watchdog both
 * report the daemon's own command line. prctl(PR_SET_NAME) already
 * fixes /proc/<pid>/comm for them; this fixes the longer command_line
 * field, which is a different string read from a different place.
 *
 * proctitle_init() must be called once from main(), with the real
 * argc/argv, before any fork whose child will set a title -- it records
 * the writable extent of the argv strings. proctitle_set() overwrites
 * that extent in the CALLING process only: in a forked child it changes
 * just the child's cmdline (copy-on-write), never the parent's. It is a
 * safe no-op when proctitle_init() was never called, so a unit test
 * that links a caller without a real main() is unaffected.
 */
void proctitle_init(int argc, char **argv);
void proctitle_set(const char *title);

#endif /* PROCTITLE_H */
