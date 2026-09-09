#ifndef EXEC_H
#define EXEC_H

#include <sys/types.h>

/*
 * Execs `cmd_argv` (argv[0] conventionally the same path as the
 * command itself; NULL-terminated) interactively inside an
 * already-running container's own mount, UTS, network, and pid
 * namespaces -- container's own IPC namespace is never isolated in
 * the first place (see src/container.c's clone3() flags), so there is
 * none to join here either. Mirrors exactly how `nsenter`/`docker
 * exec` do this.
 *
 * The PTY is allocated in THIS daemon's own namespace, before any
 * setns() call -- deliberately, not incidentally: the exec'd process's
 * slave fd is simply inherited across fork(), so it never needs to
 * open /dev/pts by path from inside the target container's own mount
 * namespace, which may not even have a working devpts instance
 * (containers get no devpts mount of their own today).
 *
 * On success, returns 0 and fills *out_pty_master_fd (O_RDWR, CLOEXEC)
 * and *out_child_pid -- the exec'd process's pid as seen from THIS
 * daemon's own, ancestor, pid namespace. Pid namespaces are
 * hierarchical, the same reasoning that already makes
 * container_handle.pid usable this way (include/container.h) --
 * setns()-then-fork() into an existing pid namespace returns exactly
 * such an outer-visible pid to the caller, not a namespace-local one.
 * The caller owns both the fd and eventual reaping (pidfd_open() the
 * pid, the same convention every other child this daemon tracks
 * already uses -- see daemon/src/registry.c).
 *
 * Returns -1 on failure, errno set by whichever step failed (opening
 * a /proc/<target_pid>/ns/* fd -- ESRCH if the container has since
 * exited, PTY allocation, or fork/pipe).
 */
/*
 * How the exec'd program is told what kind of terminal it has, and how
 * big it is. Both matter only for the pty path below: a full-screen
 * program (htop, vim) calls ioctl(TIOCGWINSZ) to find the screen size
 * and reads $TERM to find the terminfo entry describing what the
 * terminal can do. Before this existed the pty was created at its
 * kernel default of 0x0 and never sized, and $TERM was whatever cixd
 * itself inherited as pid 1 from the bootloader -- measured on a real
 * host as "linux", the kernel console's own type, regardless of which
 * client was actually attached.
 *
 * That does not produce an error, which is what made it easy to miss:
 * ncurses falls back to the terminfo entry's own lines#/cols#, so a
 * full-screen program starts normally and then draws into a fixed
 * 80x24 corner of whatever the operator's terminal really is, with no
 * resize ever reaching it.
 *
 * cols/rows of 0, and a NULL or empty term, each mean "the caller does
 * not know" and take the defaults below rather than being passed
 * through as-is. 0x0 is never a legitimate terminal size, so there is
 * no ambiguity to resolve here.
 */
struct exec_term {
	unsigned short cols;
	unsigned short rows;
	const char *term;
};

/*
 * The defaults live here, next to the struct, so the daemon's query
 * parsing and the exec itself cannot disagree about what an unspecified
 * size or terminal type means (One Source of Truth). 80x24 is the
 * historical vt100 default every terminal-handling program already
 * expects to see when nothing better is known; xterm-256color is the
 * entry a client that bothered to ask would almost always name, and
 * ncurses ships it (usr/share/terminfo/x/xterm-256color) in the same
 * package that provides the library reading it.
 */
#define EXEC_TERM_COLS_DEFAULT 80
#define EXEC_TERM_ROWS_DEFAULT 24
#define EXEC_TERM_NAME_DEFAULT "xterm-256color"

/*
 * cgroup_procs_fd: a writable fd on the container's own
 * cgroup.procs, opened by the caller before any setns() so it stays
 * usable after the child has moved into the container's mount
 * namespace (where the host's /sys/fs/cgroup is not reachable). The
 * exec'd process writes "0" to it and becomes a real member of the
 * container's cgroup.
 *
 * This is not a detail. ADR-0262's virtualised /proc answers every
 * question from the READER's own cgroup, so a session that joined the
 * container's namespaces but not its cgroup sees the container's
 * bound /proc files and reads the HOST's memory and cpu count out of
 * them -- htop in a console reporting 7.71 GiB against a 1 GiB limit,
 * measured. Namespaces decide which files are visible; the cgroup
 * decides what they say. Pass -1 to skip (the container has no
 * cgroup), which restores the old, wrong-answer behaviour and is
 * meant only for that case.
 */
int exec_into_container(pid_t target_pid, int cgroup_procs_fd, char *const cmd_argv[],
                         const struct exec_term *term,
                         int *out_pty_master_fd, pid_t *out_child_pid);

/*
 * Resizes an already-running session's pty. The kernel raises SIGWINCH
 * on the pty's own foreground process group as a direct result, which
 * is how a running full-screen program learns to redraw -- so this one
 * ioctl is the entire server side of a resize, with no signal for this
 * daemon to send itself.
 *
 * Returns 0 on success, -1 with errno set otherwise. Rejects a 0
 * dimension rather than passing it to the kernel: a resize to 0x0 is
 * always a client bug, and honouring it would silently reintroduce
 * exactly the unsized-terminal state struct exec_term exists to end.
 */
int exec_resize_pty(int pty_master_fd, unsigned short cols, unsigned short rows);

#endif /* EXEC_H */
