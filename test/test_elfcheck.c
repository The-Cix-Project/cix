/*
 * Issue #176: the undefined-builtin detector.
 *
 * Exercised against real ELF files rather than hand-built fixtures --
 * the bug this catches was found in a real published library, and a
 * synthetic ELF would only prove the parser agrees with whatever this
 * test also assumed.
 *
 * The positive case is built here rather than committed: an object
 * with an undefined symbol named `__builtin_something` is exactly what
 * TCC produces when it meets a builtin it does not implement, and
 * producing one on demand keeps the fixture honest as toolchains move.
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
	char sym[128];
	char dir[] = "/tmp/elfcheck_XXXXXX";
	char src[256], so[256], cmd[768];
	FILE *f;
	int r;

	/* A non-ELF file is "clean", not an error: this runs over every
	 * file of every install, so "not my concern" and "unreadable" must
	 * not collapse into one answer. */
	r = elfcheck_undefined_builtin("/etc/hostname", sym, sizeof(sym));
	check(r == 0, "a non-ELF file is reported clean, not as an error");

	r = elfcheck_undefined_builtin("/no/such/file/at/all", sym, sizeof(sym));
	check(r == -1, "an unreadable file is an error, distinct from clean");

	/* A real, ordinary system binary must not trip the check --
	 * otherwise the gate would reject every install. */
	r = elfcheck_undefined_builtin("/bin/sh", sym, sizeof(sym));
	check(r == 0, "an ordinary system binary is clean");

	if (mkdtemp(dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
		return 1;
	}
	snprintf(src, sizeof(src), "%s/u.c", dir);
	snprintf(so, sizeof(so), "%s/libu.so", dir);

	/*
	 * The positive case. Calling an undeclared `__builtin_notreal`
	 * leaves exactly the artifact TCC leaves for a builtin it cannot
	 * expand: an undefined external of that name in .dynsym.
	 */
	f = fopen(src, "w");
	if (f == NULL) {
		fprintf(stderr, "FAIL: could not write the fixture source\n");
		return 1;
	}
	fprintf(f, "int __builtin_notreal(int);\n"
	           "int use(int x) { return __builtin_notreal(x); }\n");
	fclose(f);

	snprintf(cmd, sizeof(cmd), "tcc -shared -o %s %s 2>/dev/null", so, src);
	if (system(cmd) != 0 || access(so, R_OK) != 0) {
		printf("ELFCHECK RESULT: SKIP (could not build the shared-object fixture)\n");
		return 0;
	}

	sym[0] = '\0';
	r = elfcheck_undefined_builtin(so, sym, sizeof(sym));
	check(r == 1, "an undefined __builtin_* symbol is detected");
	check(strcmp(sym, "__builtin_notreal") == 0, "the reported symbol is the offending name");

	unlink(src);
	unlink(so);
	rmdir(dir);

	if (g_fail > 0) {
		printf("ELFCHECK RESULT: FAIL (%d)\n", g_fail);
		return 1;
	}
	printf("ELFCHECK RESULT: PASS\n");
	return 0;
}
