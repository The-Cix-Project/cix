#
# tcc -- the Tiny C Compiler, this platform's own compiler for
# everything it builds.
#
# UPGRADED FROM THE 0.9.27 RELEASE TO A PINNED UPSTREAM SNAPSHOT.
#
# Why, and why now: issue #216. The 0.9.27 release (December 2017)
# gives two simultaneously-live locals the SAME stack slot when a case
# label of the enclosing switch sits inside the block that declares
# them. That miscompiled perl -- regexec.c:8131/8132 declare `SV *ret`
# and `REGEXP *re_sv`, both landed at -0x820(%rbp), and `re_sv = NULL`
# wiped the `ret` that had just been assigned, so miniperl segfaulted
# on every build. Root-caused from the real binary, reduced to thirty
# lines, and reproduced on this platform's own compiler; the full trail
# is in issue #216 and probe-tcc-conformance/7.
#
# It matters far beyond perl. The other TCC issues fail loudly -- a
# missing builtin is an undefined symbol, an unsupported flag is an
# error. This one hands two live variables the same memory, so wherever
# the colliding pair is only read and written rather than dereferenced,
# it corrupts values and exits 0. That is the shape of #122 (our tar
# listing one archive member and reporting success) and of the
# squashfs-tools endianness bug (every on-disk field byte-swapped,
# clean exit). Every package this compiler has ever built was in scope.
#
# THE SEARCH FOR A PATCH CAME BACK EMPTY, AND THAT IS THE ANSWER.
#
# Debian carries exactly four patches for tcc -- an i386 test, the
# stack protector in the runtime library, dead code in
# prepare_dynamic_rel, and implicit-int -- and not one touches codegen.
# There has been no tcc release since 0.9.27; 0.9.28 has been a release
# candidate for years. Debian does not patch around any of this: it
# pins a git snapshot (0.9.27+git20200814.62c30a4a). Upstream
# development continues on `mob`, where tccgen.c now has a real scope
# structure the 2017 release lacks -- cur_scope/prev_scope with
# vla.locorig, cleanup lists, pending_gotos -- i.e. a rewrite of
# precisely the area at fault.
#
# So the choice was: maintain a codegen patch by hand, forever, against
# a compiler nobody releases -- or pin a snapshot, as Debian does.
#
# MEASURED, NOT ASSUMED (probe-tcc-conformance/8, /9 and /10, all run
# on a real Cix host with Cix's own toolchain):
#
#   #216 stack-slot aliasing   FAIL on 0.9.27   ->  ok
#   #212 C11                   nothing          ->  stdatomic.h,
#                                                   stdalign.h,
#                                                   stdnoreturn.h,
#                                                   _Atomic,
#                                                   _Static_assert
#   #209 -pthread              FAILS            ->  accepted
#   #208 builtins              15/15 missing    ->  12/15 present
#   #211 compound literals     ok (our patch)   ->  ok natively
#
# and it BUILDS UNDER CIX'S OWN TCC, so the upgrade costs nothing in
# self-hosting: this compiler is seeded by the previous one exactly as
# before, and the three-stage bootstrap below is unchanged.
#
# WHAT THIS RECIPE STOPPED CARRYING, AND WHY EACH ONE IS SAFE TO DROP
#
# 0.9.27-14 carried six local changes. Probe 10 measured upstream mob
# for each property they provide, rather than dropping them hopefully:
#
#   lib/atomic.c + libtcc1.a  DROPPED. mob ships stdatomic.o and
#                             atomic.o itself, and a real C11 atomic
#                             program compiles and RUNS correctly.
#   dso_handle.o              DROPPED. mob ships dsohandle.o;
#                             __dso_handle resolves with no help.
#   bcheck.c malloc hooks     DROPPED. mob builds clean against this
#                             glibc with no such edit.
#   #122 do-while fix         DROPPED. mob evaluates the condition and
#                             runs the loop three times, unpatched.
#   #211 compound literals    DROPPED. mob handles them natively.
#   __has_include (-9)        DROPPED. mob implements it, and gets all
#                             three answers right.
#
# The one that stays:
#
#   __ATOMIC_* predefines     KEPT. mob does NOT predefine them, even
#                             though it supports C11 atomics --
#                             confirmed by probe 10, where the mob
#                             column fails an #ifndef __ATOMIC_SEQ_CST
#                             check outright. Code writes
#                             __ATOMIC_SEQ_CST without including
#                             anything, so no header can supply it.
#
# EVERY GATE IS KEPT, INCLUDING THE ONES FOR BUGS THIS COMPILER NO
# LONGER HAS. A gate is cheap and its whole value is catching a
# regression nobody predicted; removing the #122 and #211 checks
# because upstream fixed them would throw away the only evidence that
# they STAY fixed across future snapshots. A new gate for #216 joins
# them.
#
# ONE MIGRATION HAZARD, RECORDED RATHER THAN DISCOVERED LATER
#
# Inline linkage reverses direction on this upgrade:
#
#     0.9.27   'inline' -> LINK FAILED ; 'extern inline' -> ok
#     mob      'inline' -> ok          ; 'extern inline' -> LINK FAILED
#
# Bare `inline` working is exactly what gnulib needs (_GL_INLINE
# expands to it), and mob is arguably the more correct of the two under
# C99. But any recipe relying on `extern inline` will break.
# `static inline` works on both and is the spelling to prefer.
# CLAUDE.md carries this warning too.
#
# PINNED TO A COMMIT, NOT A BRANCH. `mob` moves; a moving source is not
# reproducible. The checksum was verified against two independent
# fetches of this exact commit.
#
pkg_name="tcc"
pkg_version="0.9.28rc-1"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_depends=""
#
# Unchanged from 0.9.27-14. tcc is its own build dependency: the
# three-stage bootstrap below needs a working compiler to produce
# stage 1, which is the point.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils"
pkg_changelog="0.9.28rc-1: upgrade from the 2017 0.9.27 release to pinned upstream mob 2ba12e83. Fixes #216 (two simultaneously-live locals sharing one stack slot, which miscompiled perl), brings real C11 (#212), fixes -pthread (#209) and 12 of 15 missing builtins (#208). Drops five of the six local patches this recipe carried -- upstream supplies all of them now, each verified individually rather than assumed -- and keeps the __ATOMIC_* predefines, which upstream still does not provide. Every gate is kept and one is added for #216"

pkg_build() {
	#
	# The one local change that survives the upgrade. The order
	# constants, predefined exactly as GCC predefines them: code says
	# __ATOMIC_SEQ_CST without including anything, so a header cannot
	# supply these, and probe 10 confirmed upstream mob does not.
	#
	sed -i 's|    tcc_define_symbol(s, "__STDC_HOSTED__", NULL);|    tcc_define_symbol(s, "__STDC_HOSTED__", NULL);\n    tcc_define_symbol(s, "__ATOMIC_RELAXED", "0");\n    tcc_define_symbol(s, "__ATOMIC_CONSUME", "1");\n    tcc_define_symbol(s, "__ATOMIC_ACQUIRE", "2");\n    tcc_define_symbol(s, "__ATOMIC_RELEASE", "3");\n    tcc_define_symbol(s, "__ATOMIC_ACQ_REL", "4");\n    tcc_define_symbol(s, "__ATOMIC_SEQ_CST", "5");|' \
	    libtcc.c
	#
	# Verify the edit landed. A sed whose anchor moved in a newer
	# snapshot silently does nothing, and this recipe has been bitten
	# by exactly that before (0.9.27-11 edited tccgen.c after make had
	# already run, so the patch was real and the shipped compiler did
	# not have it).
	#
	grep -q '__ATOMIC_SEQ_CST' libtcc.c || {
		echo "tcc: failed to predefine the __ATOMIC_* macros -- the anchor in libtcc.c moved" >&2
		exit 1
	}

	./configure --prefix=/usr --cc=tcc
	make -j"$(nproc)"

	#
	# ---- gates ----
	#
	# Each of these runs the compiler that was just built and checks
	# VALUES, not exit status. That distinction is the lesson of #122:
	# a miscompiled tar exited 0 on every archive while listing one
	# member.
	#

	# __has_include: all three answers must be RIGHT, not merely
	# "it compiled".
	cat > has_include_check.c <<'CHECK_EOF'
#if __has_include(<stdio.h>)
int present = 1;
#else
#error "__has_include said an existing system header is absent"
#endif
#if __has_include(<cix_definitely_not_a_real_header.h>)
#error "__has_include said a nonexistent header is present"
#endif
#if __has_include("cix_definitely_not_here_either.h")
#error "__has_include said a nonexistent quoted header is present"
#endif
int main(void) { return present ? 0 : 1; }
CHECK_EOF
	./tcc -B. has_include_check.c -o has_include_check
	./has_include_check || { echo "tcc: __has_include is present but wrong" >&2; exit 1; }

	# The __atomic_* builtins, used the way a caller uses them: no
	# header, no library flag.
	cat > atomic_check.c <<'CHECK_EOF'
static char mutex;
int main(void)
{
	if (__atomic_test_and_set(&mutex, __ATOMIC_SEQ_CST))
		return 1;                       /* was free, must report free */
	if (!__atomic_test_and_set(&mutex, __ATOMIC_SEQ_CST))
		return 2;                       /* now held, must report held */
	__atomic_clear(&mutex, __ATOMIC_SEQ_CST);
	if (__atomic_test_and_set(&mutex, __ATOMIC_SEQ_CST))
		return 3;                       /* released, must report free */
	return 0;
}
CHECK_EOF
	./tcc -B. atomic_check.c -o atomic_check
	./atomic_check || { echo "tcc: __atomic_* runtime is present but wrong" >&2; exit 1; }

	# C11 <stdatomic.h> through to a real answer. New with this
	# upgrade (#212) and gated from the start, so a later snapshot
	# that loses it fails here rather than in whatever package needed
	# it next.
	cat > stdatomic_check.c <<'CHECK_EOF'
#include <stdatomic.h>
int main(void)
{
	atomic_int a;
	atomic_init(&a, 1);
	atomic_fetch_add(&a, 41);
	return atomic_load(&a) == 42 ? 0 : 1;
}
CHECK_EOF
	./tcc -B. stdatomic_check.c -o stdatomic_check
	./stdatomic_check || { echo "tcc: C11 stdatomic is present but wrong (#212)" >&2; exit 1; }

	# __dso_handle resolves out of the runtime archive with no help
	# from the caller. Seven recipes each carried their own stub
	# before this was guaranteed; upstream supplies it now, and this
	# gate is what keeps that true.
	printf 'extern void *__dso_handle;\nint main(void){return __dso_handle != (void *)0;}\n' \
	    > dso_check.c
	./tcc -B. dso_check.c -o dso_check
	./dso_check || { echo "tcc: __dso_handle did not resolve from libtcc1.a" >&2; exit 1; }

	# do-while regression gate (issue #122). The loop must run 3 times
	# AND the condition must be evaluated 3 times, through a
	# continue-inside-switch whose body end is unreachable. A compiler
	# with the old bug runs it once, never calls the condition, and
	# exits 1. Upstream fixed this; the gate stays so a future
	# snapshot cannot quietly unfix it.
	cat > dowhile_check.c <<'CHECK_EOF'
static int calls;
static int cond(void) { calls++; return calls >= 3; }
static int next_status(void) { static int n; return (n++ < 10) ? 1 : 2; }
int main(void)
{
	int status, iterations = 0;
	do {
		iterations++;
		status = next_status();
		switch (status) {
		case 0: break;
		case 1: continue;
		case 2: break;
		}
		break;
	} while (!cond());
	return (iterations == 3 && calls == 3) ? 0 : 1;
}
CHECK_EOF
	./tcc -B. dowhile_check.c -o dowhile_check
	./dowhile_check || { echo "tcc: do-while continue codegen is broken (issue #122)" >&2; exit 1; }

	# #211 regression gate. Compiles AND RUNS, and checks VALUES.
	# Case D is the one a naive fix breaks -- a parenthesised scalar
	# under brace elision must stay two elements, not four.
	cat > initlit_check.c <<'INITLIT_EOF'
struct o { int a; int b; };
enum { I0, I1, I2 };
static struct o A[] = { (struct o){.a=1,.b=2}, (struct o){.a=3,.b=4}, (struct o){.a=5,.b=6} };
static struct o B[] = { [I2]=(struct o){.a=30,.b=31}, [I0]=(struct o){.a=10,.b=11}, [I1]=(struct o){.a=20,.b=21} };
static struct o C[] = { 1,2, 3,4, 5,6 };
static struct o D[] = { (1),2, (3),4 };
static int G[][2] = { 1,2, 3,4, 5,6 };
static char H[] = "hello";
int main(void)
{
	if (sizeof A / sizeof A[0] != 3) return 1;
	if (A[0].a != 1 || A[1].a != 3 || A[2].b != 6) return 2;
	if (sizeof B / sizeof B[0] != 3) return 3;
	if (B[0].a != 10 || B[1].a != 20 || B[2].a != 30) return 4;
	if (sizeof C / sizeof C[0] != 3) return 5;
	if (C[0].a != 1 || C[1].a != 3 || C[2].b != 6) return 6;
	if (sizeof D / sizeof D[0] != 2) return 7;
	if (D[0].a != 1 || D[0].b != 2 || D[1].a != 3 || D[1].b != 4) return 8;
	if (sizeof G / sizeof G[0] != 3) return 9;
	if (G[0][0] != 1 || G[1][1] != 4 || G[2][0] != 5) return 10;
	if (sizeof H != 6 || H[0] != 'h' || H[4] != 'o') return 11;
	{
		struct o v[] = { (struct o){.a=42,.b=43}, (struct o){.a=44,.b=45} };
		if (v[1].a != 44) return 12;
	}
	return 0;
}
INITLIT_EOF
	./tcc -B. initlit_check.c -o initlit_check
	./initlit_check || {
		echo "tcc: designated compound literal initializers are broken (issue #211, case $?)" >&2
		exit 1
	}

	# #216 regression gate -- the reason this upgrade exists.
	#
	# Two simultaneously-live locals must not share a stack slot when
	# a case label of the enclosing switch sits inside the block that
	# declares them. Checked two ways, because either alone can pass
	# for the wrong reason: the value must survive (which is how perl
	# died), and the addresses must actually differ (which is the
	# defect itself, and is true even where no value happens to be
	# clobbered).
	cat > slotalias_check.c <<'SLOT_EOF'
void *opaque(int);
int   pick(void);

static int value_survives(int which)
{
	switch (which) {
	case 0:
		return -1;
	{
		void *p;
		void *q;
	case 1:
		if (pick())
			p = opaque(1);
		else
			p = opaque(2);
		q = NULL;
		if (p == NULL)
			return 1;               /* p was clobbered by q */
		if (q != NULL)
			return 2;
		return 0;
	}
	}
	return 3;
}

static int addresses_differ(int which)
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
		if ((void *)&p == (void *)&q)
			return 1;               /* one slot for two live locals */
		return (p != (void *)0 && q == (void *)0) ? 0 : 2;
	}
	}
	return 3;
}

void *opaque(int n) { static char buf[8]; return buf + (n & 1); }
int   pick(void)    { return 1; }

int main(void)
{
	if (value_survives(1) != 0)
		return 1;
	if (addresses_differ(1) != 0)
		return 2;
	return 0;
}
SLOT_EOF
	./tcc -B. slotalias_check.c -o slotalias_check
	./slotalias_check || {
		echo "tcc: two live locals share a stack slot (issue #216, case $?)" >&2
		exit 1
	}

	#
	# ---- three-stage bootstrap ----
	#
	# Everything above built ONE compiler, using whatever tcc happened
	# to be installed. That is a real gap in an argument about
	# correctness: this package is its own build dependency, so a
	# compiler that miscompiles compilers could produce a working
	# binary that then builds subtly wrong ones. Building stage 3 with
	# stage 2 and requiring the two to be byte-identical closes it --
	# and it matters more on this upgrade than on any before it, since
	# the whole point is that 0.9.27's codegen could not be trusted.
	#
	stage_snapshot() {
		mkdir -p "$1"
		cp tcc "$1/tcc"
		cp -r lib "$1/lib" 2>/dev/null || true
		cp libtcc1.a "$1/" 2>/dev/null || true
		printf '#!/usr/bin/bash\nexec %s/tcc -B%s "$@"\n' "$1" "$(pwd)" > "$1/cc"
		chmod +x "$1/cc"
	}

	# What was just built and gated is stage 2 (stage 1 being the
	# already-installed tcc that compiled it).
	stage_snapshot /run/tcc-stage2

	make clean
	./configure --cc=/run/tcc-stage2/cc --prefix=/usr
	make -j"$(nproc)"

	# sha256sum rather than cmp: cmp lives in diffutils, which this
	# recipe does not declare, and 0.9.27-13 failed for exactly that
	# reason. It failed CLOSED -- unable to compare was treated as
	# "differ" -- which is the right way round for a gate, but it is
	# still a gate failing on its own missing tool rather than on the
	# thing it checks.
	s2sum=$(sha256sum < /run/tcc-stage2/tcc | cut -d' ' -f1)
	s3sum=$(sha256sum < tcc | cut -d' ' -f1)
	echo "bootstrap: stage 2 sha256 $s2sum"
	echo "bootstrap: stage 3 sha256 $s3sum"
	if [ "$s2sum" != "$s3sum" ]; then
		echo "tcc: stage 2 and stage 3 differ -- the compiler does not reproduce itself" >&2
		ls -l /run/tcc-stage2/tcc tcc >&2
		exit 1
	fi
	echo "bootstrap: stage 2 and stage 3 are byte-identical ($(stat -c%s tcc) bytes)"

	# Re-run the codegen gates against stage 3, since stage 3 is what
	# ships. They are identical binaries, so this cannot fail on its
	# own -- it fails only if the comparison above was somehow wrong,
	# which is worth a second to rule out.
	./tcc -B. dowhile_check.c -o dowhile_check3
	./dowhile_check3 || { echo "tcc: stage 3 fails the do-while gate (issue #122)" >&2; exit 1; }
	./tcc -B. initlit_check.c -o initlit_check3
	./initlit_check3 || { echo "tcc: stage 3 fails the initializer gate (issue #211)" >&2; exit 1; }
	./tcc -B. slotalias_check.c -o slotalias_check3
	./slotalias_check3 || { echo "tcc: stage 3 fails the stack-slot gate (issue #216)" >&2; exit 1; }
	./tcc -B. stdatomic_check.c -o stdatomic_check3
	./stdatomic_check3 || { echo "tcc: stage 3 fails the C11 atomics gate (#212)" >&2; exit 1; }
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"
}
