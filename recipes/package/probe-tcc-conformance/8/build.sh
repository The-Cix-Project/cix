#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# Exits nonzero at the end so its output lands in cixd's build-failure
# log without producing an artifact (probe-gcc-headers' pattern).
#
# THE QUESTION THIS ANSWERS
#
# #216: Cix's tcc (0.9.27, the Dec 2017 release) gives two
# simultaneously-live locals the same stack slot when a case label of
# the enclosing switch sits inside the block that declares them.
# Reduced and confirmed in probe-tcc-conformance/7.
#
# Before writing a patch, find out whether upstream already fixed it.
# Searching found no ready-made patch: Debian ships only four patches
# and none touches codegen, and there has been no tcc release since
# 0.9.27 -- 0.9.28 has been a release candidate for years. But upstream
# development continues on the `mob` branch, and current tccgen.c has a
# real scope structure that 0.9.27 does not:
#
#     static struct scope {
#         struct scope *prev;
#         struct { int loc, locorig, num; } vla;
#         struct { Sym *s; int n; } cl;
#         int *bsym, *csym;
#         Sym *lstk, *llstk;
#     } *cur_scope, *loop_scope, *root_scope;
#
# plus `pending_gotos` and cleanup lists. That is a rewrite of exactly
# the area at fault, so it MAY fix this -- and "may" is the reason for
# this probe rather than a guess. Debian's own tcc is not the release
# either; it pins a 2020 git snapshot, which is the same escape route.
#
# So: build upstream mob here, with Cix's own tcc, and run the same
# A/B/C reduction under both compilers. If the new one passes, the fix
# is an upgrade rather than a patch we write and maintain.
#
# Pinned to one commit, not to `mob`, because a branch is not a
# reproducible source. Checksum verified against two independent
# fetches of that exact commit.
#
pkg_name="probe-tcc-conformance"
pkg_version="8"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_depends=""
pkg_build_depends="tcc gcc binutils make linux-headers bash coreutils sed grep gawk findutils diffutils"
pkg_changelog="8: build upstream tcc mob (2ba12e83, 2026-08-08) with Cix's tcc and re-run the #216 reduction under it, to decide upgrade-vs-patch. No ready-made patch exists: Debian carries four patches, none touching codegen, and there has been no tcc release since 0.9.27"

pkg_build() {
	echo "=== the compiler under test today ==="
	tcc -v 2>&1 | head -2

	#
	# The reduction, written once and run against every compiler.
	# A: the bug. B: control, same locals without the case label.
	# C: print both addresses, which is the direct evidence.
	#
	mkdir -p /run/probe216 && cd /run/probe216

	cat > r.c <<'EOF'
#include <stdio.h>

void *opaque(int);
int   pick(void);

static int A(int which)
{
	switch (which) {
	case 0:
		return -1;
	{
		void *p;
		void *q;
	case 1:
		if (pick()) p = opaque(1); else p = opaque(2);
		q = NULL;
		if (p == NULL) { printf("  A FAIL  p is NULL after q=NULL (shared slot)\n"); return 1; }
		if (q != NULL) { printf("  A FAIL  q is not NULL\n"); return 1; }
		printf("  A ok\n");
		return 0;
	}
	}
	return -1;
}

static int B(void)
{
	{
		void *p;
		void *q;
		if (pick()) p = opaque(1); else p = opaque(2);
		q = NULL;
		if (p == NULL) { printf("  B FAIL  p is NULL after q=NULL\n"); return 1; }
		printf("  B ok\n");
		return 0;
	}
}

static int C(int which)
{
	switch (which) {
	case 0:
		return -1;
	{
		void *p;
		void *q;
	case 1:
		if (pick()) p = (void *)1; else p = (void *)2;
		q = NULL;
		if ((void *)&p == (void *)&q) {
			printf("  C FAIL  &p == &q == %p  SAME SLOT\n", (void *)&p);
			return 1;
		}
		printf("  C ok    &p=%p &q=%p distinct\n", (void *)&p, (void *)&q);
		return 0;
	}
	}
	return -1;
}

void *opaque(int n) { static char buf[8]; return buf + (n & 1); }
int   pick(void)    { return 1; }

int main(void)
{
	int bad = 0;
	bad |= A(1);
	bad |= B();
	bad |= C(1);
	printf("  => %s\n", bad ? "AFFECTED by #216" : "not affected");
	return bad;
}
EOF

	echo
	echo "=== 1. Cix's current tcc (0.9.27 release) ==="
	if tcc -o r_old r.c 2>&1; then ./r_old || true; else echo "  (did not compile)"; fi

	echo
	echo "=== 2. gcc, as the reference answer ==="
	if /usr/bin/gcc -O0 -o r_gcc r.c 2>&1; then ./r_gcc || true; else echo "  (did not compile)"; fi

	echo
	echo "=== 3. building upstream tcc mob 2ba12e83 with Cix's tcc ==="
	cd /build/src
	if ./configure --prefix=/run/newtcc --cc=tcc > /run/probe216/cfg.log 2>&1; then
		echo "  configure ok"
	else
		echo "  configure FAILED:"; tail -20 /run/probe216/cfg.log; exit 1
	fi
	if make -j"$(nproc)" > /run/probe216/make.log 2>&1; then
		echo "  make ok"
	else
		echo "  make FAILED (tail):"; tail -30 /run/probe216/make.log; exit 1
	fi
	make install > /run/probe216/inst.log 2>&1 || true
	ls -la /run/newtcc/bin/tcc 2>&1 | sed 's/^/  /'

	echo
	echo "=== 4. upstream mob tcc, same reduction ==="
	/run/newtcc/bin/tcc -v 2>&1 | head -2 | sed 's/^/  /'
	cd /run/probe216
	if /run/newtcc/bin/tcc -B/run/newtcc/lib/tcc -o r_new r.c 2>&1; then
		./r_new || true
	else
		echo "  (did not compile)"
	fi

	echo
	echo "=== probe complete -- failing on purpose so this log is kept ==="
	exit 1
}

pkg_install() {
	:
}
