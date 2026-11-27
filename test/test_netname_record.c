/*
 * How a container's veth names cross from cixd to the child (#757).
 *
 * The parent writes one container-side veth name per network attachment
 * into a pipe; the child reads them in the same order. A pipe is a byte
 * stream, not a message channel, and the child used to take each name
 * with a single read() of up to 15 bytes. When the parent had written two
 * names before the child read, that one read returned the first name and
 * the start of the second; the next "name" then began mid-name ("-3"),
 * if_nametoindex() failed with ENODEV, and the create failed with
 * "container_net_child_configure: No such device". Measured on the site
 * box on 2026-11-27: three creates of one six-network router container
 * failed that way, each stopping at a different attachment, while the
 * same create succeeded right after (the race decides who runs ahead).
 *
 * The fix sends each name as one fixed CONTAINER_NET_NAME_RECORD-byte
 * record and reads exactly one record at a time. The cases below are the
 * shapes the stream can arrive in, the first of them being the one that
 * broke the box: several records queued before the first read.
 */
#include "internal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void fail(const char *what)
{
	printf("FAIL: %s\n", what);
	failures++;
}

static void expect_name(int fd, const char *want, const char *what)
{
	char got[CONTAINER_NET_NAME_RECORD];

	memset(got, 'x', sizeof(got));
	if (container_net_name_record_read(fd, got) != 0) {
		printf("FAIL: %s: read failed: %s\n", what, strerror(errno));
		failures++;
		return;
	}
	if (strcmp(got, want) != 0) {
		printf("FAIL: %s: got \"%s\", want \"%s\"\n", what, got, want);
		failures++;
	}
}

/* #757 itself: the parent ran ahead and every name was queued before the
 * child's first read. Each read must still yield exactly one name. */
static void test_queued_records(void)
{
	static const char *names[] = { "vc7969-0", "vc7969-1", "vc7969-2", "vc7969-3", "vc7969-4",
	                               "vc31148-5" };
	int p[2];
	size_t i;

	if (pipe(p) != 0) {
		fail("pipe");
		return;
	}
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (container_net_name_record_write(p[1], names[i]) != 0)
			fail("queued: write");
	}
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		expect_name(p[0], names[i], "queued records each read whole");
	close(p[0]);
	close(p[1]);
}

/* A record arriving in two pieces is reassembled, not taken short. */
static void test_split_record(void)
{
	char rec[CONTAINER_NET_NAME_RECORD];
	struct timespec pause = { 0, 50 * 1000 * 1000 };
	int p[2];
	pid_t pid;

	if (pipe(p) != 0) {
		fail("pipe");
		return;
	}
	memset(rec, 0, sizeof(rec));
	memcpy(rec, "vc123-4", 7);
	pid = fork();
	if (pid < 0) {
		fail("fork");
		return;
	}
	if (pid == 0) {
		close(p[0]);
		if (write(p[1], rec, 5) != 5)
			_exit(1);
		nanosleep(&pause, NULL);
		if (write(p[1], rec + 5, sizeof(rec) - 5) != (ssize_t)(sizeof(rec) - 5))
			_exit(1);
		_exit(0);
	}
	close(p[1]);
	expect_name(p[0], "vc123-4", "a record split across two writes");
	waitpid(pid, NULL, 0);
	close(p[0]);
}

/* The parent closing before a whole record is a failure with a named
 * cause, never a short name. */
static void test_eof_mid_record(void)
{
	char got[CONTAINER_NET_NAME_RECORD];
	int p[2];

	if (pipe(p) != 0) {
		fail("pipe");
		return;
	}
	if (write(p[1], "vc9-0", 5) != 5)
		fail("eof: write");
	close(p[1]);
	errno = 0;
	if (container_net_name_record_read(p[0], got) == 0)
		fail("a partial record before EOF was accepted");
	else if (errno != EPIPE)
		printf("FAIL: EOF mid-record set errno %d, want EPIPE\n", errno), failures++;
	close(p[0]);
}

/* A name that cannot fit a record is refused, not truncated. */
static void test_name_too_long(void)
{
	int p[2];

	if (pipe(p) != 0) {
		fail("pipe");
		return;
	}
	errno = 0;
	if (container_net_name_record_write(p[1], "vc12345678901234-5") == 0)
		fail("an over-long name was written");
	else if (errno != ENAMETOOLONG)
		printf("FAIL: over-long name set errno %d, want ENAMETOOLONG\n", errno), failures++;
	close(p[0]);
	close(p[1]);
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	test_queued_records();
	test_split_record();
	test_eof_mid_record();
	test_name_too_long();

	if (failures > 0) {
		printf("test_netname_record: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_netname_record: all checks passed\n");
	return 0;
}
