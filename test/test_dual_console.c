/*
 * ADR-0042: proves image/src/dual_console.c's own relay logic directly
 * -- the exact same code image/src/kanxeo-install.c links and runs in
 * production (No Parallel Implementations: this is not a re-
 * implementation of the relay, it's the real thing) -- against two
 * throwaway PTYs standing in for the two real consoles (/dev/tty0,
 * /dev/ttyS0), since this sandbox's own QEMU harness can only scrape
 * serial output and can't exercise a real video console at all (see
 * docs/ROADMAP.md's Phase 14 own already-accepted boundary).
 *
 * A forked "driver" process plays the role kanxeo-install.c itself
 * plays in production: it opens the two PTY slaves as its own pair of
 * consoles (dual_console_open()) and calls run_subprocess_dual_console()
 * against dual_console_child (test/dual_console_child.c), which just
 * echoes each input line back with a fixed marker. This process (the
 * test itself) plays the "two operators" role: it holds both PTY
 * masters and drives input into first one, then the other, checking
 * the echoed marker lands on BOTH masters each time -- proving input
 * from either console reaches the child, and output reaches both.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dual_console.h"

#define DUAL_CONSOLE_CHILD_BIN "build/dual_console_child"

static int open_test_pty(int *out_master, char *out_slave_path, size_t slave_path_size)
{
	int master;
	char path[64];

	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0) {
		perror("posix_openpt");
		return -1;
	}
	if (grantpt(master) != 0 || unlockpt(master) != 0) {
		perror("grantpt/unlockpt");
		close(master);
		return -1;
	}
	if (ptsname_r(master, path, sizeof(path)) != 0) {
		perror("ptsname_r");
		close(master);
		return -1;
	}
	snprintf(out_slave_path, slave_path_size, "%s", path);
	*out_master = master;
	return 0;
}

/* Reads from master until marker appears in the accumulated output, or
 * deadline_ms elapses. Mirrors test/test_disk_image.c's own deadline-
 * based poll()/read() pattern. */
static int wait_for_marker(int master, const char *marker, char *out, size_t out_size,
                            int deadline_ms)
{
	struct timespec start, now;
	size_t total = 0;

	out[0] = '\0';
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		struct pollfd pfd;
		int elapsed_ms, remaining_ms, rc;
		ssize_t n;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed_ms = (int)((now.tv_sec - start.tv_sec) * 1000 +
		                    (now.tv_nsec - start.tv_nsec) / 1000000);
		remaining_ms = deadline_ms - elapsed_ms;
		if (remaining_ms <= 0)
			return -1;

		pfd.fd = master;
		pfd.events = POLLIN;
		rc = poll(&pfd, 1, remaining_ms);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return -1;
		}
		if (rc == 0)
			continue;
		if (!(pfd.revents & POLLIN))
			continue;

		n = read(master, out + total, out_size - total - 1);
		if (n <= 0)
			continue;
		total += (size_t)n;
		out[total] = '\0';
		if (strstr(out, marker) != NULL)
			return 0;
	}
}

static int write_line(int master, const char *line)
{
	size_t len = strlen(line);
	ssize_t n;

	n = write(master, line, len);
	return (n == (ssize_t)len) ? 0 : -1;
}

int main(void)
{
	int master_a, master_b;
	char slave_a[64], slave_b[64];
	pid_t driver_pid;
	int status;
	int ok = 1;
	char buf_a[4096], buf_b[4096];

	if (open_test_pty(&master_a, slave_a, sizeof(slave_a)) != 0 ||
	    open_test_pty(&master_b, slave_b, sizeof(slave_b)) != 0) {
		fprintf(stderr, "FAIL: could not create the two stand-in PTYs\n");
		return 1;
	}

	driver_pid = fork();
	if (driver_pid < 0) {
		perror("fork");
		return 1;
	}
	if (driver_pid == 0) {
		/* Plays kanxeo-install.c's own role: opens the two consoles
		 * (here, the PTY slaves standing in for /dev/tty0/ttyS0) and
		 * relays dual_console_child's I/O across both. Doesn't need
		 * the masters at all -- those belong to the "operator" side
		 * (the parent, below). */
		char *argv[] = { (char *)DUAL_CONSOLE_CHILD_BIN, NULL };

		close(master_a);
		close(master_b);
		dual_console_open(slave_a, slave_b);
		_exit(run_subprocess_dual_console(DUAL_CONSOLE_CHILD_BIN, argv) == 0 ? 0 : 1);
	}

	/* 1. Input via console A must reach the child, and the echoed
	 * output must land on BOTH consoles -- not just A. */
	if (write_line(master_a, "hello-from-a\n") != 0) {
		fprintf(stderr, "FAIL: could not write to master_a\n");
		ok = 0;
	} else if (wait_for_marker(master_a, "ECHO:hello-from-a", buf_a, sizeof(buf_a), 5000) != 0) {
		fprintf(stderr, "FAIL: echo of hello-from-a never appeared on console A\n");
		ok = 0;
	} else if (wait_for_marker(master_b, "ECHO:hello-from-a", buf_b, sizeof(buf_b), 5000) != 0) {
		fprintf(stderr,
		        "FAIL: echo of hello-from-a appeared on console A but not console B -- "
		        "output isn't reaching both consoles\n");
		ok = 0;
	} else {
		printf("input via console A, echo confirmed on both consoles\n");
	}

	/* 2. Input via console B (the OTHER console) must also reach the
	 * same still-running child -- proving input relay isn't limited to
	 * whichever console happened to be used first. */
	if (ok) {
		if (write_line(master_b, "hello-from-b\n") != 0) {
			fprintf(stderr, "FAIL: could not write to master_b\n");
			ok = 0;
		} else if (wait_for_marker(master_a, "ECHO:hello-from-b", buf_a, sizeof(buf_a), 5000) !=
		           0) {
			fprintf(stderr, "FAIL: echo of hello-from-b never appeared on console A\n");
			ok = 0;
		} else if (wait_for_marker(master_b, "ECHO:hello-from-b", buf_b, sizeof(buf_b), 5000) !=
		           0) {
			fprintf(stderr, "FAIL: echo of hello-from-b never appeared on console B\n");
			ok = 0;
		} else {
			printf("input via console B, echo confirmed on both consoles\n");
		}
	}

	if (write_line(master_a, "QUIT\n") != 0) {
		fprintf(stderr, "FAIL: could not write QUIT to end the driver child\n");
		ok = 0;
	}

	if (waitpid(driver_pid, &status, 0) != driver_pid) {
		perror("waitpid driver");
		ok = 0;
	} else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "FAIL: driver process (kanxeo-install stand-in) exited abnormally "
		                "(status 0x%x)\n",
		        (unsigned)status);
		ok = 0;
	}

	close(master_a);
	close(master_b);

	printf(ok ? "DUAL CONSOLE RESULT: PASS\n" : "DUAL CONSOLE RESULT: FAIL\n");
	return ok ? 0 : 1;
}
