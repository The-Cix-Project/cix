/*
 * Exec target for test_container_lifecycle.c's capture_output test
 * (ADR-0112): writes a fixed, greppable line to stdout and another to
 * stderr, then exits 0. Proves POST /v1/containers' "capture_output"
 * option actually captures real child output into the registry
 * entry's own "captured_output" field, not just that the container
 * ran.
 *
 * The stdout line also carries a real ANSI color escape (ESC 0x1b),
 * the same shape a colorized program (glauth's zerolog, confirmed
 * live on 192.168.15.95 during task #760) actually writes to its own
 * terminal. jw_escaped_string() (daemon/src/json.c) has to -
 * escape that raw control byte to stay valid JSON, and json_parse()
 * used to reject any \uXXXX escape outright -- silently failing the
 * ENTIRE response parse the moment any captured_output contained one
 * (found live: `cixctl ps` returned nothing against a real box
 * with LDAP containers running). This line exists so that regression
 * is caught here, locally, rather than only live again.
 */
#include <stdio.h>

int main(void)
{
	fprintf(stdout, "capture-test-stdout-line \x1b[31mcolor-marker\x1b[0m\n");
	fflush(stdout);
	fprintf(stderr, "capture-test-stderr-line\n");
	fflush(stderr);
	return 0;
}
