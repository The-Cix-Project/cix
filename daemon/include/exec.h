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
int exec_into_container(pid_t target_pid, char *const cmd_argv[],
                         int *out_pty_master_fd, pid_t *out_child_pid);

/*
 * Issue #62: the same namespace entry, but the command's output goes to
 * a PIPE and it gets no controlling terminal. A pty exists to carry an
 * interactive session and is precisely what makes the console a poor
 * diagnostic tool -- its line discipline echoes and edits what passes
 * through, which is how a piped one-liner came back visibly corrupted
 * during the Part 201 hang investigation and fed a wrong diagnosis. A
 * pipe carries exactly the bytes the command wrote.
 *
 * *out_read_fd is the readable end; *out_child_pid is the grandchild's
 * outer-namespace pid, reaped by the caller via pidfd like every other
 * child this daemon tracks. Returns -1 with errno set on failure.
 */
int exec_into_container_piped(pid_t target_pid, char *const cmd_argv[], int *out_read_fd,
                              pid_t *out_child_pid);

#endif /* EXEC_H */
