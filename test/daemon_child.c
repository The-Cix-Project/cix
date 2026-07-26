/*
 * Exec target used by test_daemon.c's container-lifecycle tests.
 * argv[1] (default 0): seconds to sleep before exiting -- lets the
 * test create a container that's still running long enough to be
 * DELETEd while alive. argv[2] (default 5): exit code, so the test
 * can verify a specific exit_status made it back through the API.
 */
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int sleep_s = argc > 1 ? atoi(argv[1]) : 0;
	int code = argc > 2 ? atoi(argv[2]) : 5;

	if (sleep_s > 0)
		sleep((unsigned int)sleep_s);
	return code;
}
