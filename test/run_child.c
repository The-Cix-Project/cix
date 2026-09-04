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
 *   42  both checks passed             -- correct
 *   43  /run rejected the write        -- #264
 *   45  a staged 0600 file was unreadable -- #265
 *   46  an attached volume rejected a write -- #266
 *   44  something else went wrong      -- neither, report it rather than guess
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Staged by the test at mode 0600 with no explicit owner. */
#define STAGED_PATH "/etc/staged_probe.conf"
/* A real volume the test attaches -- id-mapped for a userns container. */
#define VOLUME_PATH "/vol"

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

	/*
	 * #265, the same question about a different mechanism: a file the
	 * daemon STAGED into this container before it started. The test
	 * stages one at mode 0600 with no owner, which means "the container's
	 * root" -- so if the container's root cannot read it, its ownership
	 * landed outside the container's mapped range. 0600 is the point:
	 * a world-readable staged file is readable no matter who owns it,
	 * which is why this went unnoticed for so long.
	 */
	if (stat(STAGED_PATH, &st) != 0) {
		printf("STAGED_STAT_FAILED %s\n", strerror(errno));
		return 44;
	}
	printf("STAGED_OWNER uid=%u gid=%u\n", (unsigned)st.st_uid, (unsigned)st.st_gid);
	fd = open(STAGED_PATH, O_RDONLY);
	if (fd < 0) {
		if (errno == EACCES || errno == EPERM) {
			printf("STAGED_READ denied: %s\n", strerror(errno));
			return 45;
		}
		printf("STAGED_READ unexpected: %s\n", strerror(errno));
		return 44;
	}
	close(fd);
	printf("STAGED_READ ok\n");

	/*
	 * #266, the same question again for a volume. A userns container's
	 * volume is presented through an id-mapped mount, and the flag that
	 * turns that on was being cleared before the runtime read it -- so
	 * every volume came up owned by the overflow uid and rejected every
	 * write. jump's own /home was one of them.
	 */
	if (stat(VOLUME_PATH, &st) != 0) {
		printf("VOLUME_STAT_FAILED %s\n", strerror(errno));
		return 44;
	}
	printf("VOLUME_OWNER uid=%u gid=%u\n", (unsigned)st.st_uid, (unsigned)st.st_gid);
	fd = open(VOLUME_PATH "/probe.tmp", O_CREAT | O_WRONLY, 0644);
	if (fd < 0) {
		if (errno == EACCES || errno == EPERM || errno == EOVERFLOW) {
			printf("VOLUME_WRITE denied: %s\n", strerror(errno));
			return 46;
		}
		printf("VOLUME_WRITE unexpected: %s\n", strerror(errno));
		return 44;
	}
	close(fd);
	unlink(VOLUME_PATH "/probe.tmp");
	printf("VOLUME_WRITE ok\n");
	return 42;
}
