/*
 * Exec target for test_container_pty.c. Runs as PID 1 inside the
 * container, post pivot_root, and allocates a real pty exactly the
 * way an interactive SSH session does (posix_openpt + grantpt/
 * unlockpt + ptsname_r + open the slave) -- proving mountns_pivot()'s
 * own devpts mount (src/mountns.c) actually works, not just that the
 * container starts. Reports results via /status.txt in upperdir.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
	FILE *f = fopen("/status.txt", "w");
	int master, slave;
	char slave_path[64];
	struct stat st;

	if (f == NULL)
		return 126;

	fprintf(f, "DEV_PTS_PTMX_EXISTS=%s\n", stat("/dev/pts/ptmx", &st) == 0 ? "yes" : "no");

	master = posix_openpt(O_RDWR | O_NOCTTY);
	fprintf(f, "POSIX_OPENPT=%s\n", master >= 0 ? "ok" : strerror(errno));
	if (master < 0) {
		fclose(f);
		return 1;
	}
	if (grantpt(master) != 0 || unlockpt(master) != 0 ||
	    ptsname_r(master, slave_path, sizeof(slave_path)) != 0) {
		fprintf(f, "GRANT_UNLOCK_PTSNAME=%s\n", strerror(errno));
		fclose(f);
		return 1;
	}

	slave = open(slave_path, O_RDWR | O_NOCTTY);
	fprintf(f, "OPEN_SLAVE=%s\n", slave >= 0 ? "ok" : strerror(errno));
	if (slave < 0) {
		fclose(f);
		return 1;
	}

	/* Canonical (cooked) mode is the pty default -- a read() blocks
	 * until a full line lands, so the write needs a trailing newline. */
	if (write(master, "x\n", 2) == 2) {
		char rb[8] = { 0 };

		fprintf(f, "ROUNDTRIP=%s\n",
		        (read(slave, rb, sizeof(rb)) >= 1 && rb[0] == 'x') ? "ok" : "bad");
	} else {
		fprintf(f, "ROUNDTRIP=write_failed\n");
	}

	fclose(f);
	return 42;
}
