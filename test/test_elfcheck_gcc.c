/*
 * Issue #113: the GCC-producer detector.
 *
 * Both fixtures are built here rather than committed, so the test
 * stays honest as toolchains move: what matters is what THIS host's
 * tcc and gcc actually emit, not what they emitted when the test was
 * written.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "elfcheck.h"

static int g_fail;

static void check(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", what);
		g_fail++;
	}
}

int main(void)
{
	char dir[] = "/tmp/elfgcc_XXXXXX";
	char src[256], tcc_so[256], gcc_so[256], cmd[768];
	char ver[256];
	FILE *f;
	int r;

	/* Non-ELF and unreadable stay distinguishable, same contract as
	 * the undefined-builtin check. */
	check(elfcheck_built_by_gcc("/etc/hostname", ver, sizeof(ver)) == 0,
	      "a non-ELF file reports no GCC marker rather than an error");
	check(elfcheck_built_by_gcc("/no/such/path", ver, sizeof(ver)) == -1,
	      "an unreadable file is an error, distinct from clean");

	if (mkdtemp(dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
		return 1;
	}
	snprintf(src, sizeof(src), "%s/p.c", dir);
	snprintf(tcc_so, sizeof(tcc_so), "%s/libtcc_p.so", dir);
	snprintf(gcc_so, sizeof(gcc_so), "%s/libgcc_p.so", dir);

	f = fopen(src, "w");
	if (f == NULL) {
		fprintf(stderr, "FAIL: could not write the fixture source\n");
		return 1;
	}
	fprintf(f, "int answer(void) { return 42; }\n");
	fclose(f);

	snprintf(cmd, sizeof(cmd), "tcc -shared -o %s %s 2>/dev/null", tcc_so, src);
	if (system(cmd) == 0 && access(tcc_so, R_OK) == 0) {
		ver[0] = '\0';
		r = elfcheck_built_by_gcc(tcc_so, ver, sizeof(ver));
		check(r == 0, "a TCC-built shared object carries no GCC marker");
	} else {
		printf("  (skipped the TCC fixture -- tcc could not build it here)\n");
	}

	snprintf(cmd, sizeof(cmd), "gcc -shared -fPIC -o %s %s 2>/dev/null", gcc_so, src);
	if (system(cmd) == 0 && access(gcc_so, R_OK) == 0) {
		ver[0] = '\0';
		r = elfcheck_built_by_gcc(gcc_so, ver, sizeof(ver));
		check(r == 1, "a GCC-built shared object is detected");
		/* The version string is the point: "GCC built this" is useful,
		 * "GCC 12.2.0 built this" is actionable. */
		check(strncmp(ver, "GCC: ", 5) == 0, "the reported marker is the producer string");
	} else {
		printf("  (skipped the GCC fixture -- gcc unavailable here)\n");
	}

	/*
	 * The case that produced a confident wrong answer: an EXECUTABLE.
	 *
	 * Cix's own glibc crt1.o carries a GCC marker, so every TCC-linked
	 * executable on the platform inherits one. A check that reported
	 * "GCC built this" for an executable would be true of everything
	 * and would distinguish nothing -- which is precisely the mistake
	 * this guard exists to prevent, so it is tested rather than
	 * trusted.
	 */
	{
		char exe_src[256], tcc_exe[256], gcc_exe[256];

		snprintf(exe_src, sizeof(exe_src), "%s/e.c", dir);
		snprintf(tcc_exe, sizeof(tcc_exe), "%s/tcc_e", dir);
		snprintf(gcc_exe, sizeof(gcc_exe), "%s/gcc_e", dir);
		f = fopen(exe_src, "w");
		if (f != NULL) {
			fprintf(f, "int main(void) { return 0; }\n");
			fclose(f);

			snprintf(cmd, sizeof(cmd), "tcc -o %s %s 2>/dev/null", tcc_exe, exe_src);
			if (system(cmd) == 0 && access(tcc_exe, R_OK) == 0) {
				check(elfcheck_built_by_gcc(tcc_exe, ver, sizeof(ver)) ==
				          ELFCHECK_GCC_INCONCLUSIVE,
				      "a TCC-built executable is INCONCLUSIVE, never a clean 'no'");
			}
			/* Even a genuinely GCC-built executable must come back
			 * inconclusive: the answer is unusable either way, and
			 * pretending otherwise is what went wrong. */
			snprintf(cmd, sizeof(cmd), "gcc -o %s %s 2>/dev/null", gcc_exe, exe_src);
			if (system(cmd) == 0 && access(gcc_exe, R_OK) == 0) {
				check(elfcheck_built_by_gcc(gcc_exe, ver, sizeof(ver)) ==
				          ELFCHECK_GCC_INCONCLUSIVE,
				      "even a GCC-built executable is INCONCLUSIVE, not a positive");
			}
			unlink(exe_src);
			unlink(tcc_exe);
			unlink(gcc_exe);
		}
	}

	unlink(src);
	unlink(tcc_so);
	unlink(gcc_so);
	rmdir(dir);

	if (g_fail > 0) {
		printf("ELFCHECK-GCC RESULT: FAIL (%d)\n", g_fail);
		return 1;
	}
	printf("ELFCHECK-GCC RESULT: PASS\n");
	return 0;
}
