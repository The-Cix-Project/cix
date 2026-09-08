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
	 * not collapse into one answer.
	 *
	 * The fixture is WRITTEN here rather than borrowed from the host.
	 * This used to read /etc/hostname, which exists on a developer
	 * machine and does not in a composed build container -- so the
	 * test reported "clean was expected, got an error" when the real
	 * story was that the file was absent. A test that depends on an
	 * ambient host file is the same fragility this project has been
	 * removing from recipes, and it is what kept the suite from
	 * running on a Cix host at all. */
	{
		char nonelf[] = "/tmp/elfcheck_nonelf_XXXXXX";
		int fd = mkstemp(nonelf);

		check(fd >= 0, "could create a non-ELF fixture");
		if (fd >= 0) {
			(void)!write(fd, "not an ELF file at all\n", 23);
			close(fd);
			r = elfcheck_undefined_builtin(nonelf, sym, sizeof(sym));
			check(r == 0, "a non-ELF file is reported clean, not as an error");
			unlink(nonelf);
		}
	}

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

	/*
	 * ADR-0260: a freestanding executable has no .dynsym, and the gate
	 * must scan its .symtab rather than pass it unexamined. Two halves:
	 * the real cix-init is clean, and a relocatable with an undefined
	 * __builtin_ (the only static shape tcc will emit one into -- an
	 * executable link refuses it) is caught.
	 */
	{
		const char *init_bin = getenv("CIX_INIT_BIN") != NULL ? getenv("CIX_INIT_BIN") : "build/cix-init";
		char obj[256];

		if (access(init_bin, R_OK) == 0) {
			sym[0] = '\0';
			r = elfcheck_undefined_builtin(init_bin, sym, sizeof(sym));
			check(r == 0, "the freestanding cix-init is scanned and clean");
		} else {
			printf("  (no %s here -- the freestanding-clean half is skipped)\n", init_bin);
		}
		snprintf(obj, sizeof(obj), "%s/u.o", dir);
		snprintf(cmd, sizeof(cmd), "tcc -nostdlib -c -o %s %s 2>/dev/null", obj, src);
		if (system(cmd) == 0 && access(obj, R_OK) == 0) {
			sym[0] = '\0';
			r = elfcheck_undefined_builtin(obj, sym, sizeof(sym));
			check(r == 1, "an undefined __builtin_* in a .symtab-only object is detected");
			check(strcmp(sym, "__builtin_notreal") == 0, "and named");
			unlink(obj);
		} else {
			printf("  (could not build the freestanding object fixture -- that half is skipped)\n");
		}
	}

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
