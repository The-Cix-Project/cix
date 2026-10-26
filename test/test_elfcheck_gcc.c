/*
 * Issue #113: the GCC-producer detector.
 *
 * Both fixtures are built here rather than committed, so the test
 * stays honest as toolchains move: what matters is what THIS host's
 * tcc and gcc actually emit, not what they emitted when the test was
 * written.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
	/* Fixture written here, not borrowed from the host -- see
	 * test_elfcheck.c for why /etc/hostname was the wrong thing to
	 * read. */
	{
		char nonelf[] = "/tmp/elfcheckgcc_nonelf_XXXXXX";
		int fd = mkstemp(nonelf);

		check(fd >= 0, "could create a non-ELF fixture");
		if (fd >= 0) {
			(void)!write(fd, "not an ELF file at all\n", 23);
			close(fd);
			check(elfcheck_built_by_gcc(nonelf, ver, sizeof(ver)) == 0,
			      "a non-ELF file reports no GCC marker rather than an error");
			unlink(nonelf);
		}
	}
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

	/*
	 * Issue #653: thread-local data a shared library cannot reach.
	 *
	 * The same `__thread` source built both ways. tcc's -shared build
	 * is the defect (#650, libuuid 2.42.2-7) and must be refused; gcc's
	 * -fPIC build reaches its TLS through __tls_get_addr and must not
	 * be. Both compilers are declared tools of every recipe that runs
	 * this suite, so neither half is allowed to skip: a gate whose
	 * positive case quietly did not run proves nothing.
	 */
	{
		char tls_src[256], tls_tcc[256], tls_gcc[256], tls_exe[256];

		snprintf(tls_src, sizeof(tls_src), "%s/t.c", dir);
		snprintf(tls_tcc, sizeof(tls_tcc), "%s/libtls_tcc.so", dir);
		snprintf(tls_gcc, sizeof(tls_gcc), "%s/libtls_gcc.so", dir);
		snprintf(tls_exe, sizeof(tls_exe), "%s/tls_e", dir);
		f = fopen(tls_src, "w");
		check(f != NULL, "could write the thread-local fixture source");
		if (f != NULL) {
			fprintf(f, "__thread int counter;\n"
			           "int bump(void) { return ++counter; }\n"
			           "int main(void) { return bump() - 1; }\n");
			fclose(f);

			snprintf(cmd, sizeof(cmd), "tcc -shared -o %s %s", tls_tcc, tls_src);
			check(system(cmd) == 0 && access(tls_tcc, R_OK) == 0,
			      "tcc builds the thread-local shared fixture");
			check(elfcheck_unreachable_tls(tls_tcc) == 1,
			      "a tcc -shared object with __thread data is refused");

			/* By absolute path: a bare gcc computes a relative
			 * installation prefix here and cannot find cc1. */
			snprintf(cmd, sizeof(cmd), "/usr/bin/gcc -shared -fPIC -o %s %s", tls_gcc, tls_src);
			check(system(cmd) == 0 && access(tls_gcc, R_OK) == 0,
			      "gcc builds the thread-local shared fixture");
			check(elfcheck_unreachable_tls(tls_gcc) == 0,
			      "a gcc -fPIC object with __thread data is not refused");

			/* Local-exec is correct in an executable, which is
			 * what the same source is when tcc links it as one. */
			snprintf(cmd, sizeof(cmd), "tcc -o %s %s", tls_exe, tls_src);
			check(system(cmd) == 0 && access(tls_exe, R_OK) == 0,
			      "tcc builds the thread-local executable fixture");
			check(elfcheck_unreachable_tls(tls_exe) == 0,
			      "an executable with local-exec TLS is not refused");
			check(elfcheck_unreachable_tls("/no/such/path") == -1,
			      "an unreadable file is an error, distinct from clean");
			unlink(tls_src);
			unlink(tls_tcc);
			unlink(tls_gcc);
			unlink(tls_exe);
		}
	}

	/*
	 * And no shared object this suite's own build container carries is
	 * refused: glibc's, gcc's runtime and every declared tool's
	 * libraries are what a false positive would refuse first.
	 */
	{
		static const char *const libdirs[] = { "/usr/lib", "/lib", "/usr/lib64", "/lib64",
		                                       "/lib/x86_64-linux-gnu" };
		size_t k;
		int scanned = 0;

		for (k = 0; k < sizeof(libdirs) / sizeof(libdirs[0]); k++) {
			DIR *ld = opendir(libdirs[k]);
			struct dirent *de;

			if (ld == NULL)
				continue;
			while ((de = readdir(ld)) != NULL) {
				char path[1024];
				struct stat lst;

				if (strstr(de->d_name, ".so") == NULL)
					continue;
				snprintf(path, sizeof(path), "%s/%s", libdirs[k], de->d_name);
				if (lstat(path, &lst) != 0 || !S_ISREG(lst.st_mode))
					continue;
				scanned++;
				if (elfcheck_unreachable_tls(path) == 1) {
					fprintf(stderr, "FAIL: %s is refused as unreachable TLS\n", path);
					g_fail++;
				}
			}
			closedir(ld);
		}
		printf("  (unreachable-TLS check ran over %d shared objects in this container)\n", scanned);
		check(scanned > 0, "the container's own shared objects were found to scan");
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
