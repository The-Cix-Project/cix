/*
 * Exec target for test_container_lifecycle.c's capture_output test
 * (Phase: ordinary-container stdout/stderr capture, ADR pending):
 * writes a fixed, greppable line to stdout and another to stderr,
 * then exits 0. Proves POST /v1/containers' "capture_output" option
 * actually captures real child output into the registry entry's own
 * "captured_output" field, not just that the container ran.
 */
#include <stdio.h>

int main(void)
{
	fprintf(stdout, "capture-test-stdout-line\n");
	fflush(stdout);
	fprintf(stderr, "capture-test-stderr-line\n");
	fflush(stderr);
	return 0;
}
