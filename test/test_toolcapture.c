/*
 * test_toolcapture -- diskpart keeps the END of a tool's output (#729).
 *
 * sfdisk prints the disk and its old table first and its reason last.
 * On gibsson-1 (2026-11-22) a refused grow reported the disk banner and
 * two partitions, because only the first 511 bytes were kept, and the
 * reason never reached the operator. This writes 2000 bytes whose last
 * line is the reason and checks that the captured text ends with it.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "diskpart.h"

int main(void)
{
	static const char reason[] = "sfdisk: Failed to resize partition #5.";
	char banner[2000];
	int fds[2];
	const char *got;
	size_t glen, rlen = strlen(reason);
	int ok = 1;

	memset(banner, 'x', sizeof(banner));
	if (pipe(fds) != 0) {
		fprintf(stderr, "FAIL: pipe\n");
		return 1;
	}
	if (write(fds[1], banner, sizeof(banner) - rlen - 1) < 0 ||
	    write(fds[1], "\n", 1) != 1 ||
	    write(fds[1], reason, rlen) != (ssize_t)rlen) {
		fprintf(stderr, "FAIL: write\n");
		return 1;
	}
	close(fds[1]);
	diskpart_capture_tool_output(fds[0]);
	close(fds[0]);

	got = diskpart_last_tool_error();
	glen = strlen(got);
	if (glen < rlen || strcmp(got + glen - rlen, reason) != 0) {
		fprintf(stderr, "FAIL: captured text does not end with the reason: ...%s\n",
		        glen > 60 ? got + glen - 60 : got);
		ok = 0;
	}
	if (strchr(got, '\n') != NULL) {
		fprintf(stderr, "FAIL: a newline survived the fold\n");
		ok = 0;
	}

	/* A short output is kept whole. */
	if (pipe(fds) != 0)
		return 1;
	if (write(fds[1], "short\n", 6) != 6)
		return 1;
	close(fds[1]);
	diskpart_capture_tool_output(fds[0]);
	close(fds[0]);
	if (strcmp(diskpart_last_tool_error(), "short ") != 0) {
		fprintf(stderr, "FAIL: short output: \"%s\"\n", diskpart_last_tool_error());
		ok = 0;
	}

	printf("TOOLCAPTURE: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
