/*
 * Exec target used by test_daemon.c's container-lifecycle tests.
 * argv[1] (default 0): seconds to sleep before exiting -- lets the
 * test create a container that's still running long enough to be
 * DELETEd while alive. argv[2] (default 5): exit behaviour -- a value
 * >= 0 is an exit code (so the test can verify a specific exit_status
 * made it back through the API); a NEGATIVE value means "die by a
 * SIGNAL rather than a normal exit", so a test can verify the
 * term_signal path (issue #78) that distinguishes a signal death from
 * a real exit code.
 *
 * That deliberate fault renames itself to "cix-test-segv" first, so the
 * kernel line it produces on the host says which it is -- a real crash
 * of this binary still reports as daemon_child.
 *
 * The signal is produced by a genuine NULL dereference (SIGSEGV, signal
 * 11), NOT raise() -- because this process is PID 1 of the container's
 * own PID namespace, and PID 1 IGNORES any signal it has no handler for
 * (SIGKILL/SIGSTOP included) unless it comes from an ancestor namespace;
 * raise(SIGKILL) on itself is therefore silently swallowed. A
 * synchronous CPU fault like SIGSEGV is force-delivered even to PID 1,
 * so it's the one signal death a container's own init can reliably
 * inflict on itself. (A real stop/delete's SIGKILL works because the
 * daemon sends it from the ancestor namespace -- that path is covered
 * live, not here.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int sleep_s = argc > 1 ? atoi(argv[1]) : 0;
	int code = argc > 2 ? atoi(argv[2]) : 5;

	if (sleep_s > 0)
		sleep((unsigned int)sleep_s);
	if (code < 0) {
		volatile int *p = (volatile int *)0;

		/*
		 * Label the fault before causing it, because the kernel is
		 * about to log it on the host and an operator reading that log
		 * has no way to tell a deliberate test fault from a real
		 * crash:
		 *
		 *   daemon_child[2075]: segfault at 0 ip ... error 6
		 *
		 * The kernel prints the process comm, so renaming ourselves is
		 * the only way to label the KERNEL's own line rather than
		 * merely adding a note near it. comm is capped at 16 bytes
		 * including the NUL.
		 *
		 * Deliberately only on this path. A genuine crash in this
		 * binary still reports as daemon_child, so the rename means
		 * exactly one thing: this fault was on purpose.
		 *
		 * The stderr line is for the container's own captured output,
		 * which is forwarded into the log store -- so the explanation
		 * lands next to the kernel line rather than only in a comment
		 * nobody reading a log will see.
		 */
		(void)prctl(PR_SET_NAME, "cix-test-segv", 0, 0, 0);
		fprintf(stderr, "cix-test-segv: deliberate NULL write to verify the term_signal "
		                "path (#78) -- this segfault is expected\n");
		fflush(stderr);

		*p = 1; /* SIGSEGV (signal 11), force-delivered even to PID 1 */
		return 111; /* unreachable */
	}
	return code;
}
