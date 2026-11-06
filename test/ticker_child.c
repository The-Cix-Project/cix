/*
 * Exec target for test_handover.c (ADR-0375). Writes one line every
 * three seconds, in two halves: "handover-tick N begin", a pause, then
 * " end" and the newline. Nearly all of its life is spent with a line
 * started and not ended, so a handover lands mid-line nearly every
 * time. A cixd that lost the first half would capture " end" as a line
 * of its own, which is what the test looks for.
 */
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	unsigned long n;

	for (n = 1;; n++) {
		printf("handover-tick %lu begin", n);
		fflush(stdout);
		sleep(3);
		printf(" end\n");
		fflush(stdout);
	}
	return 0;
}
