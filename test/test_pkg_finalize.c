/*
 * ADR-0251: the package-finalize policy does what it says, and refuses
 * rather than skipping when it cannot.
 *
 * This runs daemon/policy/pkg-finalize.sh -- the exact file the daemon
 * embeds via build/generated/pkg_finalize.h -- against synthetic
 * DESTDIR trees. It is a gate rather than a demonstration: the policy
 * DELETES files out of every package this platform builds, so the set
 * of things it deletes, and the set it must leave alone, both need to
 * be nailed down where a change to either shows up as a test failure.
 *
 * The cases that matter are the ones it must NOT touch:
 *
 *   libtcc1.a         no shared counterpart -- dropping it kills the
 *                     platform's own compiler
 *   libc_nonshared.a  no shared counterpart -- gcc's dynamic links
 *                     need it
 *   libfreetype.a     a real package that ships an archive and no .so
 *   libpthread.a      eight bytes, no members, beside libpthread.so.0 --
 *                     the only thing `ld -lpthread` can resolve (#324)
 *   libc.so           glibc ships this as an ASCII ld script, not ELF
 *   a symlink         never followed, never stripped
 *   race_linux.syso   a relocatable object not named .o -- --strip-debug,
 *                     chosen by e_type; --strip-unneeded breaks go -race
 *
 * strip is stubbed rather than required, so the classification is
 * asserted without needing binutils here and without depending on what
 * a real strip does to a synthetic file. Whether a real strip produces
 * a working binary is a different question, answered by the real
 * package builds and recorded in ADR-0251.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define POLICY "daemon/policy/pkg-finalize.sh"

static int g_failures;

static void fail(const char *fmt, ...)
{
	va_list ap;

	fputs("  FAIL: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	g_failures++;
}

/* mkdir -p for the parents of path. */
static void mkparents(const char *path)
{
	char buf[512];
	char *p;

	snprintf(buf, sizeof(buf), "%s", path);
	p = strrchr(buf, '/');
	if (p == NULL)
		return;
	*p = '\0';
	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		mkdir(buf, 0755);
		*p = '/';
	}
	mkdir(buf, 0755);
}

static void wr(const char *path, const char *bytes, size_t len, mode_t mode)
{
	FILE *f;

	mkparents(path);
	f = fopen(path, "wb");
	if (f == NULL) {
		fail("cannot create %s: %s", path, strerror(errno));
		return;
	}
	if (len > 0 && fwrite(bytes, 1, len, f) != len)
		fail("short write to %s", path);
	fclose(f);
	chmod(path, mode);
}

static void wr_text(const char *path, const char *text)
{
	wr(path, text, strlen(text), 0644);
}

/*
 * An ELF file as far as the policy's four-byte magic test is concerned,
 * with the given e_type in the two little-endian bytes at offset 16 --
 * the field the policy chooses a strip by.
 */
#define ET_EXEC_TYPE 2
#define ET_REL_TYPE 1

static void wr_elf_typed(const char *path, int e_type)
{
	char buf[64];

	memset(buf, 0, sizeof(buf));
	memcpy(buf, "\177ELF", 4);
	buf[16] = (char)(e_type & 0xff);
	buf[17] = (char)((e_type >> 8) & 0xff);
	wr(path, buf, sizeof(buf), 0755);
}

static void wr_elf(const char *path)
{
	wr_elf_typed(path, ET_EXEC_TYPE);
}

static void wr_elf_rel(const char *path)
{
	wr_elf_typed(path, ET_REL_TYPE);
}

static void wr_ar(const char *path)
{
	char buf[64];

	memset(buf, 0, sizeof(buf));
	memcpy(buf, "!<arch>\n", 8);
	wr(path, buf, sizeof(buf), 0644);
}

/*
 * An archive with NO members: exactly the eight-byte magic and nothing
 * else, which is what glibc installs for its folded-in stubs. wr_ar()
 * above pads to 64 bytes, so it is a *non-empty* archive and keeps its
 * existing classification -- the two helpers are the whole distinction
 * this case turns on.
 */
static void wr_ar_empty(const char *path)
{
	wr(path, "!<arch>\n", 8, 0644);
}

static int exists(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0;
}

static void want(const char *dest, const char *rel, int should_exist)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", dest, rel);
	if (exists(path) == should_exist)
		return;
	if (should_exist)
		fail("%s was removed and must be kept", rel);
	else
		fail("%s survived and must be removed", rel);
}

/*
 * A stub strip that records how it was called, plus the real coreutils
 * the policy needs. PATH is set to this directory alone, so a real strip
 * on the machine cannot mask a policy that failed to invoke one -- which
 * also means every tool the policy declares has to be provided here, and
 * the policy refusing on a missing one is itself asserted below.
 */
static void real_tool(const char *bin, const char *name)
{
	char path[512];
	char script[512];
	char real[64];

	snprintf(real, sizeof(real), "/usr/bin/%s", name);
	if (access(real, X_OK) != 0)
		snprintf(real, sizeof(real), "/bin/%s", name);

	snprintf(script, sizeof(script), "#!/usr/bin/bash\nexec %s \"$@\"\n", real);
	snprintf(path, sizeof(path), "%s/%s", bin, name);
	wr(path, script, strlen(script), 0755);
}

static void make_stub_bin(const char *bin, const char *log, int with_strip)
{
	char path[512];
	char script[512];

	mkdir(bin, 0755);

	if (with_strip) {
		snprintf(script, sizeof(script),
		         "#!/usr/bin/bash\necho \"$@\" >> %s\nexit 0\n", log);
		snprintf(path, sizeof(path), "%s/strip", bin);
		wr(path, script, strlen(script), 0755);
	}

	/*
	 * rm does the pruning; wc measures an archive, because the shell
	 * cannot -- a NUL byte cannot live in a variable and read stops at
	 * one regardless of -N or -d '' (#324). All three are coreutils, so the
	 * policy asks for no package it did not already need.
	 */
	real_tool(bin, "rm");
	real_tool(bin, "wc");
	/* od reads an ELF file's e_type, which chooses the strip. */
	real_tool(bin, "od");
}

/*
 * Runs the policy the way the build container does (ADR-0307 clause 2):
 * as a PROGRAM with the staged root as its one argument, not sourced
 * with $PKG_DESTDIR set. That is not a detail of this harness -- CBS
 * invokes an embedder's finalizer as `execlp(cmd, cmd, staged_root,
 * NULL)`, so a test that kept sourcing it would be exercising a
 * calling convention nothing uses any more.
 *
 * Invoked through its own shebang rather than through an explicit
 * bash, so the file's interpreter line and its executable bit are both
 * part of what this asserts. Both have a real failure behind them:
 * these images ship usr/bin/bash and no /bin/bash, and a 0644 policy
 * fails with EACCES that CBS's runner reports as exit 127 -- an error
 * that reads as a missing file against one that is plainly there.
 */
static int run_policy(const char *dest, const char *bin, const char *errfile)
{
	char cmd[1024];
	int rc;

	snprintf(cmd, sizeof(cmd), "PATH='%s' %s '%s' 2>'%s'", bin, POLICY, dest, errfile);
	rc = system(cmd);
	return rc == -1 ? -1 : WEXITSTATUS(rc);
}

static int log_has(const char *log, const char *needle)
{
	char line[1024];
	FILE *f;
	int found = 0;

	f = fopen(log, "r");
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strstr(line, needle) != NULL) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

/* Whether one line of the log carries both needles -- a flag and a path. */
static int log_has_line(const char *log, const char *a, const char *b)
{
	char line[1024];
	FILE *f;
	int found = 0;

	f = fopen(log, "r");
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strstr(line, a) != NULL && strstr(line, b) != NULL) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

/* Case 1: a package shaped like glibc -- what goes, what stays. */
static void case_classification(const char *root)
{
	char dest[512], bin[512], log[512], err[512], path[512];
	int rc;

	snprintf(dest, sizeof(dest), "%s/c1", root);
	snprintf(bin, sizeof(bin), "%s/c1bin", root);
	snprintf(log, sizeof(log), "%s/c1.striplog", root);
	snprintf(err, sizeof(err), "%s/c1.err", root);

	snprintf(path, sizeof(path), "%s/usr/lib/libc.so.6", dest);
	wr_elf(path);
	snprintf(path, sizeof(path), "%s/usr/lib/libfoo.so.1", dest);
	wr_elf(path);
	snprintf(path, sizeof(path), "%s/usr/bin/prog", dest);
	wr_elf(path);
	snprintf(path, sizeof(path), "%s/usr/lib/crt1.o", dest);
	wr_elf_rel(path);
	snprintf(path, sizeof(path), "%s/usr/lib/mod.ko", dest);
	wr_elf_rel(path);
	/*
	 * A relocatable object whose name says nothing: Go's race runtime.
	 * Stripped with --strip-unneeded, as a name-based classifier did, it
	 * makes every `go build -race` fail to link (probe-go-race@1,
	 * 192.168.15.95, 2026-09-24).
	 */
	snprintf(path, sizeof(path), "%s/usr/lib/go/src/runtime/race/internal/amd64v1/race_linux.syso",
	         dest);
	wr_elf_rel(path);

	/* archives WITH a shared counterpart -- dropped */
	snprintf(path, sizeof(path), "%s/usr/lib/libc.a", dest);
	wr_ar(path);
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu/libc.a", dest);
	wr_ar(path);
	snprintf(path, sizeof(path), "%s/usr/lib/libfoo.a", dest);
	wr_ar(path);

	/*
	 * A compiler runtime WITH a shared counterpart -- kept anyway
	 * (ADR-0310, #521). libstdc++.a is not a duplicate of libstdc++.so;
	 * it is what `gcc -static-libstdc++` links, and dropping it made
	 * that flag impossible platform-wide while -static-libgcc kept
	 * working, because libgcc.a has no libgcc.so beside it. Both stems
	 * are staged here so the pair is asserted together below.
	 */
	snprintf(path, sizeof(path), "%s/usr/lib64/libstdc++.so.6", dest);
	wr_elf(path);
	snprintf(path, sizeof(path), "%s/usr/lib64/libstdc++.a", dest);
	wr_ar(path);
	snprintf(path, sizeof(path), "%s/usr/lib64/libgcc.a", dest);
	wr_ar(path);

	/* archives WITHOUT one -- kept, and this is the important half */
	snprintf(path, sizeof(path), "%s/usr/lib/libtcc1.a", dest);
	wr_ar(path);
	snprintf(path, sizeof(path), "%s/usr/lib/libc_nonshared.a", dest);
	wr_ar(path);
	snprintf(path, sizeof(path), "%s/usr/lib/libfreetype.a", dest);
	wr_ar(path);
	/* go-bootstrap's shape: an ar archive of Go objects, not ELF. */
	snprintf(path, sizeof(path), "%s/usr/lib/go-bootstrap/pkg/linux_amd64/runtime.a", dest);
	wr_ar(path);

	/*
	 * #324: an EMPTY archive beside a shared object of the same stem.
	 * glibc >= 2.34 folds pthread into libc.so.6 and still installs an
	 * eight-byte libpthread.a plus a libpthread.so.0 runtime stub, and
	 * no libpthread.so -- so that archive is the only thing `ld
	 * -lpthread` can resolve against. Clause 2 dropped it on the stem
	 * match and every gcc package passing -pthread stopped linking.
	 * libc.a above is the control: same stem match, real members, still
	 * dropped.
	 */
	snprintf(path, sizeof(path), "%s/usr/lib/libpthread.a", dest);
	wr_ar_empty(path);
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu/libpthread.so.0", dest);
	wr_elf(path);

	/* glibc ships libc.so as an ASCII linker script, not ELF */
	snprintf(path, sizeof(path), "%s/usr/lib/libc.so", dest);
	wr_text(path, "GROUP ( libc.so.6 libc_nonshared.a )\n");

	snprintf(path, sizeof(path), "%s/usr/lib/libfoo.la", dest);
	wr_text(path, "# libtool metadata\n");

	snprintf(path, sizeof(path), "%s/usr/share/man/man1/x.1", dest);
	wr_text(path, "man\n");
	snprintf(path, sizeof(path), "%s/usr/share/info/x.info", dest);
	wr_text(path, "info\n");
	snprintf(path, sizeof(path), "%s/usr/share/doc/README", dest);
	wr_text(path, "doc\n");
	snprintf(path, sizeof(path), "%s/usr/share/doc/zlib/COPYING", dest);
	wr_text(path, "Permission is granted to anyone to use this software\n");
	snprintf(path, sizeof(path), "%s/usr/share/locale/en/m.mo", dest);
	wr_text(path, "mo\n");
	snprintf(path, sizeof(path), "%s/usr/share/i18n/locales/en_US", dest);
	wr_text(path, "locale source\n");
	snprintf(path, sizeof(path), "%s/usr/share/cix/keep.dat", dest);
	wr_text(path, "not a doc tree\n");

	snprintf(path, sizeof(path), "%s/usr/lib/libc.so.6.link", dest);
	if (symlink("libc.so.6", path) != 0)
		fail("cannot create symlink: %s", strerror(errno));

	make_stub_bin(bin, log, 1);

	rc = run_policy(dest, bin, err);
	if (rc != 0)
		fail("policy exited %d on a well-formed tree", rc);

	/*
	 * ADR-0306: the finalize phase no longer deletes documentation or
	 * locale trees. These five assertions were `0` under ADR-0251's
	 * clause 4 and are deliberately inverted rather than deleted -- the
	 * fixture still stages all five, so a reintroduced prune fails here
	 * instead of shipping quietly.
	 *
	 * The COPYING line is the one that matters: it is what clause 4
	 * actually cost. usr/share/doc is where GNU packages install their
	 * licence, and deleting the tree deleted the licence with it.
	 */
	want(dest, "usr/share/man/man1/x.1", 1);
	want(dest, "usr/share/info/x.info", 1);
	want(dest, "usr/share/doc/README", 1);
	want(dest, "usr/share/doc/zlib/COPYING", 1);
	want(dest, "usr/share/locale/en/m.mo", 1);
	want(dest, "usr/share/i18n/locales/en_US", 1);
	want(dest, "usr/share/cix/keep.dat", 1);

	want(dest, "usr/lib/libfoo.la", 0);

	want(dest, "usr/lib/libc.a", 0);
	want(dest, "lib/x86_64-linux-gnu/libc.a", 0);
	want(dest, "usr/lib/libfoo.a", 0);

	/* #324: empty archive beside its own shared stem -- kept. */
	want(dest, "usr/lib/libpthread.a", 1);
	want(dest, "lib/x86_64-linux-gnu/libpthread.so.0", 1);

	/*
	 * #521: the compiler runtime survives its own shared counterpart.
	 * libgcc.a is the control -- it was always kept, by the accident
	 * that the shared one is libgcc_s.so rather than libgcc.so, and a
	 * rule that kept only it would leave the asymmetry in place.
	 */
	want(dest, "usr/lib64/libstdc++.a", 1);
	want(dest, "usr/lib64/libstdc++.so.6", 1);
	want(dest, "usr/lib64/libgcc.a", 1);

	want(dest, "usr/lib/libtcc1.a", 1);
	want(dest, "usr/lib/libc_nonshared.a", 1);
	want(dest, "usr/lib/libfreetype.a", 1);
	want(dest, "usr/lib/go-bootstrap/pkg/linux_amd64/runtime.a", 1);
	if (log_has(log, "runtime.a"))
		fail("a Go archive was passed to strip");

	want(dest, "usr/lib/libc.so", 1);
	want(dest, "usr/lib/libc.so.6", 1);
	want(dest, "usr/lib/libc.so.6.link", 1);
	want(dest, "usr/bin/prog", 1);

	if (!log_has(log, "--strip-unneeded"))
		fail("nothing was stripped with --strip-unneeded");
	if (!log_has(log, "--strip-debug"))
		fail("nothing was stripped with --strip-debug");
	if (!log_has(log, "--strip-unneeded /") || !log_has(log, "usr/lib/libc.so.6"))
		fail("libc.so.6 was not stripped as a shared object");
	if (!log_has(log, "usr/bin/prog"))
		fail("the executable was not stripped");
	if (!log_has_line(log, "--strip-debug", "usr/lib/crt1.o"))
		fail("crt1.o was not stripped with --strip-debug");
	if (!log_has_line(log, "--strip-debug", "usr/lib/mod.ko"))
		fail("the kernel module was not stripped with --strip-debug");
	if (!log_has_line(log, "--strip-debug", "race_linux.syso"))
		fail("a relocatable object not named .o was not stripped with --strip-debug -- "
		     "the strip must follow e_type, not the file name");
	if (log_has_line(log, "--strip-unneeded", "race_linux.syso"))
		fail("a relocatable object was stripped with --strip-unneeded, which breaks "
		     "linking against it (go build -race)");
	if (log_has(log, "usr/lib/libtcc1.a"))
		fail("a kept archive was passed to strip -- go-bootstrap ships Go .a "
		     "files that strip rejects, so kept archives are left alone");
	if (log_has(log, "libc.so.6.link"))
		fail("a symlink was passed to strip");
	if (log_has(log, "usr/lib/libc.so\n"))
		fail("the ASCII linker script was passed to strip");
}

/* Case 2: a package with no ELF at all needs no strip and must succeed. */
static void case_data_only(const char *root)
{
	char dest[512], bin[512], err[512], path[512];
	int rc;

	snprintf(dest, sizeof(dest), "%s/c2", root);
	snprintf(bin, sizeof(bin), "%s/c2bin", root);
	snprintf(err, sizeof(err), "%s/c2.err", root);

	snprintf(path, sizeof(path), "%s/usr/share/ca-certificates/ca.crt", dest);
	wr_text(path, "-----BEGIN CERTIFICATE-----\n");
	snprintf(path, sizeof(path), "%s/usr/share/man/x.1", dest);
	wr_text(path, "man\n");

	make_stub_bin(bin, "", 0);

	rc = run_policy(dest, bin, err);
	if (rc != 0)
		fail("a package with no ELF needed strip (exit %d)", rc);
	want(dest, "usr/share/ca-certificates/ca.crt", 1);
	want(dest, "usr/share/man/x.1", 1);	/* ADR-0306 */
}

/* Case 3: ELF produced and no strip -- ADR-0250, fail, do not skip. */
static void case_missing_strip(const char *root)
{
	char dest[512], bin[512], err[512], path[512], line[512];
	int rc, named = 0;
	FILE *f;

	snprintf(dest, sizeof(dest), "%s/c3", root);
	snprintf(bin, sizeof(bin), "%s/c3bin", root);
	snprintf(err, sizeof(err), "%s/c3.err", root);

	snprintf(path, sizeof(path), "%s/usr/lib/libbar.so.1", dest);
	wr_elf(path);

	make_stub_bin(bin, "", 0);

	rc = run_policy(dest, bin, err);
	if (rc == 0)
		fail("a package with ELF and no strip was allowed to succeed");

	f = fopen(err, "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			if (strstr(line, "binutils") != NULL)
				named = 1;
		}
		fclose(f);
	}
	if (!named)
		fail("the failure did not name binutils as the fix");
}

/*
 * Case 4: wc missing -- ADR-0250 again, and the reason it earns its own
 * case rather than riding on case 3. wc is what measures an archive, so
 * without it the prune cannot tell an eight-byte glibc stub from a real
 * one (#324) -- and a policy that silently skipped that check would drop
 * libpthread.a again, which is precisely the bug. Fail, do not skip.
 */
static void case_missing_wc(const char *root)
{
	char dest[512], bin[512], err[512], path[512], line[512];
	int rc, named = 0;
	FILE *f;

	snprintf(dest, sizeof(dest), "%s/c4", root);
	snprintf(bin, sizeof(bin), "%s/c4bin", root);
	snprintf(err, sizeof(err), "%s/c4.err", root);

	snprintf(path, sizeof(path), "%s/usr/share/man/x.1", dest);
	wr_text(path, "man\n");

	/* Everything the policy needs except wc. */
	mkdir(bin, 0755);
	real_tool(bin, "rm");

	rc = run_policy(dest, bin, err);
	if (rc == 0)
		fail("the policy ran without wc, so no archive could be measured");

	f = fopen(err, "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			if (strstr(line, "coreutils") != NULL)
				named = 1;
		}
		fclose(f);
	}
	if (!named)
		fail("the failure did not name coreutils as the fix");
}

/*
 * Case 5: no staged root given at all (ADR-0307 clause 2).
 *
 * The argument replaced $PKG_DESTDIR, and the failure mode to refuse
 * is the quiet one: `test -d ""` is false, so an empty argument would
 * otherwise take the "nothing staged" early return and report SUCCESS
 * having finalized nothing. A build whose policy silently did not run
 * is exactly what ADR-0251 exists to prevent, and it would be
 * invisible -- the package installs, and only its size says anything.
 */
static void case_no_argument(const char *root)
{
	char bin[512], err[512], cmd[1024];
	int rc;

	snprintf(bin, sizeof(bin), "%s/c5bin", root);
	snprintf(err, sizeof(err), "%s/c5.err", root);
	make_stub_bin(bin, "", 0);

	snprintf(cmd, sizeof(cmd), "PATH='%s' %s 2>'%s'", bin, POLICY, err);
	rc = system(cmd);
	rc = rc == -1 ? -1 : WEXITSTATUS(rc);
	if (rc == 0)
		fail("the policy reported success with no staged root -- it finalized nothing "
		     "and said so was fine");
	if (!log_has(err, "no staged root"))
		fail("the policy refused with no argument but did not say why");
}

int main(void)
{
	char root[] = "/tmp/cix_finalize_XXXXXX";

	/*
	 * X_OK as well as R_OK since ADR-0307 clause 2: the policy is
	 * execve()d, by this test and by CBS's --finalize-command alike,
	 * so losing the executable bit in the repo breaks every CBS build
	 * on the box with an exit 127 that names nothing useful. Checked
	 * here, where the message can say so, rather than discovered as a
	 * failing system() below.
	 */
	if (access(POLICY, X_OK) != 0) {
		fprintf(stderr,
		        "PKG FINALIZE: %s is not executable -- it is run as a program with the "
		        "staged root as $1 (ADR-0307 clause 2), so it needs its executable bit\n",
		        POLICY);
		return 1;
	}
	if (access(POLICY, R_OK) != 0) {
		fprintf(stderr, "PKG FINALIZE: cannot read %s -- run from the repo root\n", POLICY);
		return 1;
	}
	if (mkdtemp(root) == NULL) {
		fprintf(stderr, "PKG FINALIZE: mkdtemp: %s\n", strerror(errno));
		return 1;
	}

	case_classification(root);
	case_data_only(root);
	case_missing_strip(root);
	case_missing_wc(root);
	case_no_argument(root);

	if (g_failures > 0) {
		fprintf(stderr, "PKG FINALIZE: FAIL (%d) -- tree kept at %s\n", g_failures, root);
		return 1;
	}

	printf("PKG FINALIZE: ok (classification, data-only, missing-strip, missing-wc, no-argument)\n");
	return 0;
}
