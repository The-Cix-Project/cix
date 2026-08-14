#ifndef DUAL_CONSOLE_H
#define DUAL_CONSOLE_H

#include <stddef.h>

/*
 * ADR-0042: mirrors a program's own status output and one interactive
 * child process's I/O across two consoles at once, instead of
 * whatever single one the kernel happened to bind /dev/console to.
 * Extracted out of image/src/kanxeo-install.c (its own real, only
 * caller in production) into its own module specifically so
 * test/test_dual_console.c can link and exercise the real relay logic
 * directly -- not a re-implementation of it (No Parallel
 * Implementations) -- against two throwaway PTYs standing in for the
 * two real consoles.
 */

/*
 * Opens path0/path1 as the two consoles every function below writes
 * to / relays through -- kanxeo-install.c passes the real device
 * paths ("/dev/tty0"/"/dev/ttyS0"); test_dual_console.c passes two
 * PTY slave paths instead. Tolerant per-path: either failing to open
 * just makes that one unavailable to every helper below, not fatal by
 * itself (there is no caller-visible error return -- see the .c file
 * for the full reasoning on why "neither available" still isn't
 * something this function itself can usefully report).
 */
void dual_console_open(const char *path0, const char *path1);

/* printf-style, mirrored to whichever of the two consoles are open. */
void dual_printf(const char *fmt, ...);

/* perror()-equivalent, mirrored the same way. */
void dual_perror(const char *s);

/*
 * Runs bin via fork()+execve(), relaying its I/O through a PTY across
 * both open consoles simultaneously -- input from whichever console
 * has it, output to both. Same 0-clean-exit/-1-otherwise contract
 * every other run_subprocess*() helper in this project already uses.
 */
int run_subprocess_dual_console(const char *bin, char *const argv[]);

/*
 * Blocks until a byte arrives on whichever console still has one open,
 * discards it, and returns 0 -- kanxeo-install.c's own end-of-install
 * "press Enter to reboot" prompt, so an operator watching either
 * console can proceed without needing to know which one is "live."
 * Returns -1 if neither console is open (nothing left to wait on).
 */
int dual_console_wait_for_key(void);

#endif /* DUAL_CONSOLE_H */
