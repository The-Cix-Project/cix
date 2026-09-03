/*
 * Exec target for test_userns_run.c (#264).
 *
 * Answers one question from inside a real container: can the container's
 * OWN root write to /run? That is not a question about permissions bits --
 * /run is mode 0755 either way -- but about who owns the tmpfs the platform
 * mounts there. If it is mounted by a process that is unmapped in the
 * container's user namespace, its root inode ends up owned by the overflow
 * uid and the container's root is locked out of its own scratch directory.
 *
 * Deliberately tiny and dependency-free: it runs inside a minimal fixture
 * image whose only staged runtime is glibc.
 *
 * Exit status is the result, and each value is distinct on purpose so the
 * test can tell the bug apart from the environment:
 *   42  /run is writable          -- correct
 *   43  /run rejected the write    -- the bug
 *   44  something else went wrong  -- neither, report it rather than guess
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
	struct stat st;
	int fd;

	if (stat("/run", &st) != 0) {
		printf("RUN_STAT_FAILED %s\n", strerror(errno));
		return 44;
	}
	/* Printed unconditionally: on a failure this is the single most
	 * useful fact, and 65534 here names the cause outright. */
	printf("RUN_OWNER uid=%u gid=%u\n", (unsigned)st.st_uid, (unsigned)st.st_gid);
	printf("SELF uid=%u gid=%u\n", (unsigned)getuid(), (unsigned)getgid());

	fd = open("/run/probe.tmp", O_CREAT | O_WRONLY, 0644);
	if (fd < 0) {
		if (errno == EACCES || errno == EPERM) {
			printf("RUN_WRITE denied: %s\n", strerror(errno));
			return 43;
		}
		printf("RUN_WRITE unexpected: %s\n", strerror(errno));
		return 44;
	}
	close(fd);
	unlink("/run/probe.tmp");
	printf("RUN_WRITE ok\n");
	return 42;
}
