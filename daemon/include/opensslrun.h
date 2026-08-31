#ifndef OPENSSLRUN_H
#define OPENSSLRUN_H

#include <stddef.h>

/*
 * Runs /usr/bin/openssl and captures its output.
 *
 * Lifted out of pki.c when a second, unrelated subsystem needed it
 * (releasekey.c, ADR-0220). Duplicating it would have been a parallel
 * implementation of a subprocess-with-capture primitive; linking pki.c
 * for it drags in DNS name validation and the container registry, which
 * a signature-format unit test has no business needing. One
 * implementation, in a translation unit that depends on nothing.
 *
 * Returns the child's exit status, or -1 if it could not be run.
 */
int pki_run_openssl(char *const argv[], char *out, size_t out_size);

#endif /* OPENSSLRUN_H */
