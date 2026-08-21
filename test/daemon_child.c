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
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int sleep_s = argc > 1 ? atoi(argv[1]) : 0;
	int code = argc > 2 ? atoi(argv[2]) : 5;

	if (sleep_s > 0)
		sleep((unsigned int)sleep_s);
	if (code < 0) {
		volatile int *p = (volatile int *)0;

		*p = 1; /* SIGSEGV (signal 11), force-delivered even to PID 1 */
		return 111; /* unreachable */
	}
	return code;
}
