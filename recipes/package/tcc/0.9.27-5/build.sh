#
# tcc -- the Tiny C Compiler, the exclusive toolchain this project's
# own daemon/CLI are built with (ADR-0001). An ordinary recipe (built
# inside the shared toolchain sandbox's own real gcc, not a hostbuild)
# -- installed onto a "thinc-builder" image so thinc.recipe (ADR-0057)
# has a real tcc to build thincd/thincctl with via hostbuild.
#
# Source is tinycc's own canonical download.savannah.nongnu.org
# release. Version matches this sandbox's own already-installed system
# tcc (0.9.27, confirmed via `tcc -v`) -- the same compiler already
# proven, this whole project long, to build this codebase cleanly.
#
pkg_name="tcc"
pkg_version="0.9.27-5"
pkg_source="https://download.savannah.nongnu.org/releases/tinycc/tcc-0.9.27.tar.bz2"
pkg_sha256="de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c"
# Nothing at runtime: tcc links against libc alone, and its own
# libtcc1.a is part of this package.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD,
# declared rather than inherited from whatever a shared sandbox
# happened to accumulate. The build container is composed from
# exactly these and nothing else, so this list is not
# documentation -- it is the environment.
#
# Each entry earns its place:
#   tcc       the compiler this recipe pins with --cc=tcc -- tcc builds
#             itself. Through -2 it did not: tcc's configure is hand
#             written and ignores a CC= environment variable entirely,
#             taking --cc= instead, so every tcc this project has ever
#             installed was compiled by whatever ambient gcc the shared
#             sandbox happened to carry. Visible only once the
#             environment held exactly what was declared: the build
#             printed `C compiler  gcc (0.0)` and then died with
#             `make: gcc: No such file or directory`
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln, used throughout configure,
#             config.status and the Makefile
#   sed       configure rewrites its own output with sed constantly
#   grep      every feature test that greps a compiler or header
#   gawk      AC_PROG_AWK equivalents in the Makefile
#   binutils  the Makefile archives libtcc1.a with ar
#             (tcc -ar does the archiving itself, but configure
#             still probes for a real one)
#
# Sufficiency is enforced by the build itself. Minimality is
# review, not enforcement (ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils"


# lib/bcheck.c (TCC's own optional bounds-checking runtime, only used
# by programs compiled with tcc's own -b flag -- not needed to build
# thincd/thincctl, which never pass it) unconditionally defines
# CONFIG_TCC_MALLOC_HOOKS on Linux and references glibc's
# __malloc_hook/__free_hook/etc. directly. Real, confirmed upstream/
# environment incompatibility, not a project bug: TCC 0.9.27 was
# released in 2017, years before glibc 2.34 (2021) removed those
# hooks' declarations from <malloc.h> entirely -- every real distro
# packaging tcc 0.9.27 against a modern glibc carries an equivalent
# patch. Confirmed empirically: the exact same source built clean
# before this patch on distro/gcc conditions predating glibc 2.34, and
# fails identically to this on any glibc >= 2.34 without it (this
# build host's own glibc is 2.36). Guards the existing
# CONFIG_TCC_MALLOC_HOOKS definition with the same glibc-version check
# real downstream packages use, mirroring the file's own existing
# BSD/Windows exclusion block for the identical reason (those
# platforms never had these hooks to begin with).
# TCC 0.9.27 implements neither the __atomic_* builtins nor the
# __ATOMIC_* memory-order macros that GCC and Clang have provided for
# over a decade. Real code uses them with no header and no library flag,
# precisely because they are builtins -- libcap's own internal spinlock
# is two lines of exactly that -- so any such package simply does not
# compile against this compiler.
#
# Supplying them from libtcc1.a is the same answer GCC gives with
# libgcc: the compiler ships its own runtime, and every TCC-linked
# binary already gets this archive. The alternative -- each affected
# recipe carrying its own copy -- would be the same code written N
# times, in the wrong place.
#
# XCHG with a memory operand asserts the LOCK signal implicitly on x86
# (Intel SDM Vol 2A, XCHG), so one xchgb is a full-barrier atomic
# read-modify-write: exactly the sequentially consistent semantics both
# builtins are specified to have, and what GCC emits for them. Verified
# by disassembly (`xchg %al,(%rcx)`) and by running the resulting
# spinlock, not by reading the manual alone.
#
# Known limitation, deliberately not papered over: TCC 0.9.27 has no
# mechanism for predeclaring a builtin's prototype (the tccdefs.h that
# would allow it arrived after this release), so a caller still gets
# "warning: implicit declaration". Harmless for an int-returning
# function, and a package built with -Werror would reject it -- but
# such a package does not build at all today, so this is strictly
# better, never a regression. Tracked rather than accepted silently.
pkg_build() {
	cat > lib/atomic.c <<'ATOMIC_EOF'
/*
 * Atomic builtins TCC 0.9.27 does not implement, supplied as real
 * functions in the compiler's own runtime library. See tcc.recipe for
 * the full reasoning.
 *
 * memorder is accepted and ignored because seq_cst is the strongest
 * ordering: answering every weaker request with a stronger guarantee
 * is always correct.
 */

int __atomic_test_and_set(volatile void *ptr, int memorder)
{
	unsigned char prev = 1;

	(void)memorder;
	__asm__ __volatile__("xchgb %0, %1"
	                     : "+q"(prev), "+m"(*(volatile unsigned char *)ptr)
	                     :
	                     : "memory");
	return prev != 0;
}

void __atomic_clear(volatile void *ptr, int memorder)
{
	unsigned char zero = 0;

	(void)memorder;
	__asm__ __volatile__("xchgb %0, %1"
	                     : "+q"(zero), "+m"(*(volatile unsigned char *)ptr)
	                     :
	                     : "memory");
}
ATOMIC_EOF

	# __dso_handle belongs in the compiler runtime for the same reason
	# the atomics do, and its absence has been paid for repeatedly:
	# sed, grep, coreutils, m4, bison, gawk and sysklogd each compile
	# and link their own one-line copy of this, because glibc's static
	# destructor bookkeeping expects a symbol that GCC supplies from
	# crtbegin.o and a bare TCC link has nothing to supply. Seven
	# identical stubs is the definition of solving a problem more than
	# once.
	#
	# Strong, not weak, and that is the whole trick. A weak definition
	# does not cause archive extraction -- confirmed directly here, and
	# the reason every one of those recipes passes a bare object path
	# rather than -l anything. A strong definition in an archive is
	# extracted only when nothing else already defines the symbol,
	# which is exactly the wanted behaviour: a program (or a GCC
	# crtbegin) carrying its own __dso_handle simply never pulls this
	# member in. Both paths verified: a program that needs it links,
	# and a program defining its own links with no conflict.
	printf 'void *__dso_handle = (void *)0;\n' > lib/dso_handle.c

	# Build both into libtcc1.a alongside the runtime pieces already
	# there. Purely additive: no existing object or symbol changes, so
	# this cannot alter how anything already built behaves.
	sed -i 's|^X86_64_O = libtcc1.o alloca86_64.o alloca86_64-bt.o$|X86_64_O = libtcc1.o alloca86_64.o alloca86_64-bt.o atomic.o dso_handle.o|' \
	    lib/Makefile
	grep -q 'atomic\.o dso_handle\.o' lib/Makefile || {
		echo "tcc: failed to add atomic.o/dso_handle.o to libtcc1.a" >&2
		exit 1
	}

	# The order constants, predefined by the compiler exactly as GCC
	# predefines them -- code says __ATOMIC_SEQ_CST without including
	# anything, so a header cannot supply these.
	sed -i 's|    tcc_define_symbol(s, "__STDC_HOSTED__", NULL);|    tcc_define_symbol(s, "__STDC_HOSTED__", NULL);\n    tcc_define_symbol(s, "__ATOMIC_RELAXED", "0");\n    tcc_define_symbol(s, "__ATOMIC_CONSUME", "1");\n    tcc_define_symbol(s, "__ATOMIC_ACQUIRE", "2");\n    tcc_define_symbol(s, "__ATOMIC_RELEASE", "3");\n    tcc_define_symbol(s, "__ATOMIC_ACQ_REL", "4");\n    tcc_define_symbol(s, "__ATOMIC_SEQ_CST", "5");|' \
	    libtcc.c
	grep -q '__ATOMIC_SEQ_CST' libtcc.c || { echo "tcc: failed to predefine the __ATOMIC_* macros" >&2; exit 1; }

	sed -i '/^#define HAVE_MEMALIGN$/a\
#if defined(__GLIBC__) \&\& (__GLIBC__ > 2 || (__GLIBC__ == 2 \&\& __GLIBC_MINOR__ >= 34))\
#undef CONFIG_TCC_MALLOC_HOOKS\
#endif' lib/bcheck.c
	./configure --prefix=/usr --cc=tcc
	make -j"$(nproc)"

	# Prove the compiler that was just built actually provides them,
	# the way a caller uses them: no header, no library flag. A tcc
	# that silently lost this would otherwise only be discovered by
	# whatever package needed it next.
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

	# And that __dso_handle resolves out of the archive with no help
	# from the caller -- the whole point of moving it here.
	printf 'extern void *__dso_handle;\nint main(void){return __dso_handle != (void *)0;}\n' \
	    > dso_check.c
	./tcc -B. dso_check.c -o dso_check
	./dso_check || { echo "tcc: __dso_handle did not resolve from libtcc1.a" >&2; exit 1; }
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"
}
