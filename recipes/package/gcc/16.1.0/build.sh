#
# gcc -- GNU Compiler Collection, built genuinely from source (gcc,
# g++, plus every real support tool: cpp, gcc-ar/nm/ranlib, gcov*,
# lto-dump). The centerpiece of this dev-toolchain recipe set --
# everything else in this batch (binutils, make, autoconf/automake/
# libtool, pkgconf) exists to support a real compiler; this recipe is
# that compiler.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against two independent mirrors (ftp.gnu.org and mirrors.kernel.org)
# -- byte-identical, same sha256. gmp/mpfr/mpc (gcc's own build-time
# prerequisites -- gcc cannot configure without a real arbitrary-
# precision math library, and this project's isolated, network-less
# build container cannot let gcc's own contrib/download_prerequisites
# fetch them live the way a normal build does) are vendored as
# additional pkg_source entries at their exact versions pinned by this
# gcc release's own contrib/download_prerequisites script (read
# directly, not guessed at). Per ADR-0036, only source[0] (gcc's own
# tarball) is fetched into /build/src and auto-extracted; every
# additional source lands as a plain, unextracted file at
# /build/extra/<basename-of-its-own-URL> -- pkg_build() below extracts
# each of the three itself and renames them to the bare subdirectory
# names gcc's own build system auto-detects (gmp/, mpfr/, mpc/), the
# same end state contrib/download_prerequisites itself produces, just
# reached here via a real, checksummed, offline multi-source fetch
# instead of a live network call during the build. All three verified
# against ftp.gnu.org directly (byte-identical).
#
# isl (Graphite loop-nest optimization's own dependency) is
# deliberately NOT vendored or built at all -- a real, fatal TCC
# compile error (`isl_options.c:110: error: unknown type size`, inside
# isl's own macro-heavy `ISL_ARGS_START`-family argument-parsing
# machinery) found the hard way, confirmed via GCC's own real
# configure source that `--with-isl=no` is the documented, graceful
# way to skip Graphite entirely (`graphite_requested=no`, no error) --
# the same "get a real working compiler, not every GCC feature"
# judgment call `--disable-lto`/`--disable-libsanitizer` already make
# for two other optional pieces.
#
pkg_name="gcc"
pkg_version="16.1.0"
pkg_source="https://ftp.gnu.org/gnu/gcc/gcc-16.1.0/gcc-16.1.0.tar.xz https://gcc.gnu.org/pub/gcc/infrastructure/gmp-6.3.0.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpfr-4.2.2.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpc-1.3.1.tar.gz"
pkg_sha256="50efb4d94c3397aff3b0d61a5abd748b4dd31d9d3f2ab7be05b171d36a510f79 ac28211a7cfb609bae2e2c8d6058d66c8fe96434f740cf6fe2e47b000d1c20cb 9ad62c7dc910303cd384ff8f1f4767a655124980bb6d8650fe62c815a231bb7b ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8"
pkg_depends="binutils m4"

# gmp/mpfr/mpc/isl are NOT extracted with the ambient `tar` on this
# build sandbox -- it has a real, confirmed, deeply strange bug found
# the hard way across many real, on-box diagnostics: it silently stops
# after the very first archive entry on any real multi-file tarball,
# reproduced with a freshly-decompressed, checksum-verified real GNU
# release tarball. Ruled out one at a time, each with real evidence,
# not guessed: compression format (bz2 and gz both affected), blocking
# factor, --read-full-records, flag/invocation style, running
# configure as root, a lazy-writeback race (sync+sleep still failed),
# the source/compiler pairing itself (a from-scratch local build of
# the exact same tar 1.35 source with CC=tcc extracts the same file
# correctly every time), raw open()+read() at the syscall level
# (a standalone probe returns correct, full-sized reads on every one
# of 8 consecutive calls), and fstat()'s own file-type detection
# (S_ISREG=1, correct size, not a tty) -- all correct. The final,
# decisive test: a tar binary built *fresh*, from that same known-good
# source, *inside this exact build sandbox*, with this project's own
# real, working binutils, STILL exhibits the identical bug -- yet a
# small, independent, purpose-built extractor using plain buffered
# fread() (not GNU tar's own custom rmtread() buffering) successfully
# extracted that freshly-built tar's own 2343-entry source tree in the
# very same build, moments earlier. That's real, already-collected
# proof the bug is specific to GNU tar's own internal I/O layer in
# this environment, not this project's toolchain, the kernel, or the
# archive data -- and that the simpler, already-working extractor is
# the practical fix, not something to work around further.
#
# pkg_build() below uses that exact real-USTAR-format extractor
# (regular files + directories only, read directly from tar's own
# src/tar.h header layout -- fixed 512-byte blocks, POSIX header,
# char-array fields only, so no `__attribute__((packed))` ambiguity)
# compiled from a heredoc with tcc, for gmp/mpfr/mpc/isl -- no
# fresh-tar bootstrap needed for them at all, since it never depends
# on tar being able to extract itself the way tar's own source did.
# Real mode bits (chmod()) and real mtimes (utime()) are both applied
# from each header's own fields, not left at extraction-time defaults
# -- confirmed necessary the hard way earlier in this same
# investigation (a lost +x broke ./configure outright; "now" as every
# mtime made a generated Makefile.in look stale relative to
# Makefile.am purely from extraction-order noise, triggering a real
# unwanted autotools regeneration).

# Real, load-bearing build-time dependency: gcc's own assembler/linker
# calls need a working as/ld present (pkg_depends="binutils ..." above)
# and its build system invokes m4 during its own configure/build.
# gmp/mpfr/mpc are extracted into gcc's own source tree at the bare
# names its configure auto-detects, exactly matching what
# contrib/download_prerequisites itself would do, just offline.
# --disable-bootstrap: this is a real, deliberate, single-stage build
# using the build container's own already-present host gcc 12.2 to
# compile this gcc -- not a 3-stage bootstrap (which exists to verify
# a compiler can rebuild itself bit-for-bit, valuable for gcc's own
# release engineering, not needed here since the host compiler is
# already a real, trusted, working gcc). --enable-languages=c,c++
# keeps this build to what's actually needed for a real C/C++ dev
# image -- Fortran/Ada/Go/D/Objective-C/M2 are real gcc frontends but
# out of scope for this recipe set. --disable-multilib (this project
# only ever targets x86_64, never 32-bit compat). --with-system-zlib
# uses the toolchain's own real zlib instead of vendoring another copy
# (LTO needs zlib; already real and present via the wholesale toolchain
# copy). --disable-libsanitizer skips the AddressSanitizer/etc runtime
# libraries -- a real, useful feature but a large, slow-to-build
# addition not needed for this recipe set's own "get a real working
# compiler" goal. --disable-lto: confirmed via a real captured build
# failure (`tcc: error: invalid option -- '--print-prog-name'`, then
# `liblto_plugin.ver: error: unrecognized file type`) that GCC's own
# `lto-plugin` subdirectory -- a real ELF-platform default, built
# unconditionally unless disabled -- needs both a `--print-prog-name`
# driver query and a linker version-script (`.ver`) neither of which
# TCC supports; confirmed via GCC's own real `configure` source
# (`enable_lto`/`configdirs` handling) that `--disable-lto` is the
# correct, documented way to skip that subdirectory entirely, not a
# workaround. `gcc -flto` itself already isn't a goal of this recipe
# set ("get a real working compiler," not a fully feature-complete
# one) -- the same judgment call `--disable-libsanitizer` above
# already makes for a different optional GCC feature.
#
# Real, working, single-stage `--disable-bootstrap` genuinely needs
# *two* compilers here, not one: TCC for the target compiler this
# recipe exists to produce, and the ambient host gcc/g++ for anything
# build-time-only. `genconstants`/`genenums` (gcc's own internal
# code-generator host tools, built once and run during this build,
# never shipped) are real C++ (`.cc`) files TCC cannot compile at all,
# so they correctly use real ambient g++ regardless -- but linking
# them failed with `undefined reference to '__va_start'`/`'__va_arg'`
# (TCC's own varargs calling convention, which real gcc's linker has
# no runtime support for), because `libiberty.a`'s own *build-tools*
# copy (`build-x86_64-pc-linux-gnu/libiberty/`, a real, separate tree
# GCC's build system already maintains apart from the target copy)
# still got compiled with TCC.
#
# Two real, sequential misdiagnoses on the way to the actual fix, both
# disproven with direct evidence rather than guessed past: (1)
# `CC_FOR_BUILD`/`CXX_FOR_BUILD` as environment variables to
# `../configure` had zero effect -- read directly from GCC's own real
# `configure` source, it only takes the branch that respects an
# environment `CC_FOR_BUILD` when `build != host`; since this project
# never cross-compiles (build=host=target always), it takes the
# unconditional `CC_FOR_BUILD="$(CC)"` branch instead, baking a
# literal `$(CC)` *make* variable reference into the generated
# Makefile regardless of the environment. (2) A one-shot `sed` patch
# on `build-x86_64-pc-linux-gnu/*/Makefile` run right after
# `../configure` *also* had zero effect -- confirmed directly (a real
# `find`/`grep` diagnostic showed the file didn't exist yet at that
# point at all): this project's own top-level `../configure` only
# generates the *top-level* Makefile; every subdirectory's own
# configure (including `build-x86_64-pc-linux-gnu/libiberty`'s) is
# triggered recursively *by `make` itself*, on demand, mid-build --
# the standard GNU "toplevel bootstrap" convention gcc/binutils-style
# super-projects use. Real fix: the same bounded retry loop
# `binutils.recipe`'s own TLS-macro fix already established for
# exactly this "needed file doesn't exist until a `make` attempt
# creates it" shape -- patch, attempt `make`, and if it fails, patch
# again (now that more subdirectory Makefiles exist) and retry, up to
# a bound, relying on `make`'s own resumable dependency tracking so
# already-built pieces (gmp, mpfr, mpc, ...) aren't redone each pass.
# One further real refinement, also confirmed with direct evidence (a
# retry that patched `CC =` correctly still failed, on a *different*
# host tool, `undefined reference` unchanged): `make`'s own dependency
# tracking only looks at file timestamps, not which compiler produced
# an object file -- an `.o` already built by TCC on an earlier,
# not-yet-patched pass looks perfectly up to date to `make` and is
# never recompiled just because the Makefile's `CC =` line changed
# underneath it. Each retry iteration also deletes every
# `.o`/`.a`/`.lo` already built under `build-x86_64-pc-linux-gnu/`
# (never the Makefiles themselves, which stay correctly patched) so a
# genuinely fresh, real-compiler recompile actually happens on the
# next `make` attempt, not a stale, silently-still-wrong reuse.
#
# The `build-x86_64-pc-linux-gnu/*` fix above only covers *build-time*
# host tools. A real, deeper, distinct version of the identical
# `__va_start`/`__va_arg` mismatch recurs almost an hour into a real
# build against `../libiberty/libiberty.a` -- the plain, *target*
# copy, correctly TCC-compiled (it feeds `cc1`, the actual point of
# this recipe), linked into `gcov` this time, not a build tool at all.
# GCC's own C++ frontend and its auxiliary programs (`cc1plus`,
# `gcov`, ...) have been real C++ (not C) since GCC 4.7 -- TCC has no
# C++ support whatsoever, so they always need real ambient g++,
# regardless of build-vs-target. Forcing the *target* libiberty.a
# itself to compile with real gcc (the same fix used for the
# build-tools copy) is not an option here -- `cc1` (plain C, correctly
# TCC-built) needs *that exact same archive* too, and doing so would
# just move the ABI mismatch onto `cc1` instead of removing it.
#
# Real fix: TCC's own `__va_start`/`__va_arg` (`lib/va_list.c` in
# TCC's own real source, verified against the exact upstream tarball
# `tcc.recipe` already uses, byte-identical) is a small, portable,
# genuinely ABI-compliant reimplementation of the real x86_64 SysV
# `va_list` layout (`gp_offset`/`fp_offset`/`overflow_arg_area`/
# `reg_save_area` -- the same struct shape real glibc/gcc use), not
# anything TCC-proprietary. Compiling that exact file with *real* gcc
# produces a portable `.o` implementing both symbols correctly for
# either compiler's own linker to resolve. Each retry iteration `ar
# r`s it directly into every `libiberty.a` found (both the
# build-tools and target copies, wherever they currently exist) --
# `ar r` replaces-or-adds, so this is safe to repeat every pass. TCC's
# own compiled callers keep working exactly as before (their own
# `libtcc1.a` already resolves these symbols independently, this
# archive addition changes nothing for them); real g++-linked
# consumers now find `__va_start`/`__va_arg` right there in the same
# archive they already link against, with zero Makefile surgery and
# without touching which compiler builds the rest of libiberty.a at
# all.
#
# The identical shape recurred once more, roughly fifty real minutes
# into the build, for a third TCC-runtime-only symbol: `undefined
# reference to 'alloca'` (`../libiberty/libiberty.a(regex.o)`, still
# linked into `gcov`) -- `alloca()` grows the *caller's own* stack
# frame, so it can never be an ordinary C function (its own prologue/
# epilogue would create and destroy a stack frame of its own instead);
# TCC's real `lib/alloca86_64.S` implements it as a small, standalone
# x86_64 SysV assembly routine (verified byte-identical against the
# same upstream tarball), not a C-callable library function at all.
# Same fix, same reasoning: assembled with real `gcc` (`.S` files go
# through the same driver as `.c`, no special handling needed) into a
# portable `.o`, `ar r`'d into every `libiberty.a` right alongside
# `va_list.o`. Any future `undefined reference` to a bare, TCC-runtime
# -only symbol surfacing further into this build should be looked for
# in TCC's own real `lib/` sources first (`alloca-arm.S`/`bcheck.c`/
# the `*-bt.S` bounds-checking variants are the other candidates
# already confirmed to exist there) rather than re-diagnosed from
# scratch -- this is now a proven, reusable pattern, not a one-off.
#
# A fourth gap turned out NOT to be a TCC-runtime symbol at all:
# `undefined reference to 'C_alloca'` (`make-relative-prefix.c`, linked
# into `gcc-ar`). Root cause lives in `include/libiberty.h`'s own
# `alloca()` macro selection (`GCC_VERSION >= 2000` picks
# `__builtin_alloca`, else falls back to a real, portable, public-
# domain `C_alloca()` implementation GCC itself ships in
# `libiberty/alloca.c` for exactly this situation) -- TCC never defines
# `__GNUC__`, so `GCC_VERSION` is always 0 there, and every TCC-compiled
# translation unit calling `alloca()` through this header needs
# `C_alloca` to actually exist. Fixed the same way as the two symbols
# above (find the real, already-present source, compile it, `ar r` it
# into every `libiberty.a`) but the source this time is GCC's own
# unmodified `libiberty/alloca.c`, not TCC's -- already sitting in the
# same extracted source tree `pkg_source` downloads, no separate fetch.
# Compiled with `tcc` itself (confirmed clean: plain ISO C89 on the
# non-Cray path), with `HAVE_STDLIB_H`/`HAVE_STRING_H` forced on the
# command line since its own `#ifdef HAVE_CONFIG_H` guard means it
# silently skips including `<stdlib.h>`/`<string.h>` when compiled
# standalone, ahead of libiberty's own generated config.h existing.
#
# A fifth gap, past all of libiberty: `undefined reference to
# '__floatundidf'` from `libgmp.a`/`libmpfr.a` (an unsigned-64-bit-to-
# double conversion helper), linked into `cc1`/`cc1plus` themselves.
# This in-tree GMP/MPFR build has no CPU-specific assembly tuning
# selected, so their portable C fallback paths emit a call to this
# symbol instead of inline conversion code -- exactly the same class of
# gap as `va_list.c`/`alloca86_64.S` above (a compiler-runtime helper
# real GCC gets from libgcc and TCC gets from its own
# `lib/libtcc1.c`), just a different missing family and a different
# pair of consumer archives. Extracted (not the whole libtcc1.c file,
# to avoid dragging in unrelated helpers like `__divdi3`/`__moddi3`
# that could collide with real libgcc's own copies once linked) as the
# self-contained float<->int64 conversion family plus the handful of
# typedefs/macros it depends on -- confirmed to compile clean with real
# gcc and export the whole family. Injected into `libgmp.a`/`libmpfr.a`
# as well as `libiberty.a` each retry pass, same as the other three.
# This is, by real wall-clock time, the single longest build in this
# project to date.
pkg_build() {
	cat > /build/miniextract.c <<'MINIEXTRACT'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>

#define BLK 512

static void mkdirs(const char *path)
{
	char buf[4096], *p;

	strncpy(buf, path, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	for (p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(buf, 0755);
			*p = '/';
		}
	}
}

int main(int argc, char **argv)
{
	FILE *f;
	unsigned char block[BLK];

	if (argc != 2)
		return 1;
	f = fopen(argv[1], "rb");
	if (!f)
		return 1;
	while (fread(block, 1, BLK, f) == BLK) {
		int allzero = 1, i;
		char name[101], sizeoct[13], prefix[156], fullpath[600];
		long size;
		char typeflag;

		for (i = 0; i < BLK; i++)
			if (block[i]) { allzero = 0; break; }
		if (allzero)
			break;
		char modeoct[9], mtimeoct[13];
		long mode;
		struct utimbuf ut;

		memcpy(name, block, 100);
		name[100] = 0;
		memcpy(modeoct, block + 100, 8);
		modeoct[8] = 0;
		mode = strtol(modeoct, NULL, 8) & 07777;
		memcpy(sizeoct, block + 124, 12);
		sizeoct[12] = 0;
		size = strtol(sizeoct, NULL, 8);
		memcpy(mtimeoct, block + 136, 12);
		mtimeoct[12] = 0;
		ut.actime = ut.modtime = strtol(mtimeoct, NULL, 8);
		typeflag = block[156];
		memcpy(prefix, block + 345, 155);
		prefix[155] = 0;
		if (prefix[0])
			snprintf(fullpath, sizeof(fullpath), "%s/%s", prefix, name);
		else
			snprintf(fullpath, sizeof(fullpath), "%s", name);
		mkdirs(fullpath);
		if (typeflag == '5') {
			mkdir(fullpath, (mode ? (mode_t)mode : 0755));
			utime(fullpath, &ut);
		} else if (typeflag == '0' || typeflag == 0) {
			FILE *out = fopen(fullpath, "wb");
			long remaining = size;

			while (remaining > 0) {
				size_t towrite;

				if (fread(block, 1, BLK, f) != BLK)
					return 1;
				towrite = remaining < BLK ? (size_t)remaining : BLK;
				if (out)
					fwrite(block, 1, towrite, out);
				remaining -= BLK;
			}
			if (out) {
				fclose(out);
				chmod(fullpath, (mode ? (mode_t)mode : 0644));
				utime(fullpath, &ut);
			}
		} else {
			long nblocks = (size + BLK - 1) / BLK, i2;

			for (i2 = 0; i2 < nblocks; i2++)
				if (fread(block, 1, BLK, f) != BLK)
					break;
		}
	}
	fclose(f);
	return 0;
}
MINIEXTRACT
	tcc /build/miniextract.c -o /build/miniextract

	bzip2 -dc /build/extra/gmp-6.3.0.tar.bz2 > /build/gmp.tar
	/build/miniextract /build/gmp.tar && mv gmp-6.3.0 gmp
	bzip2 -dc /build/extra/mpfr-4.2.2.tar.bz2 > /build/mpfr.tar
	/build/miniextract /build/mpfr.tar && mv mpfr-4.2.2 mpfr
	gzip -dc /build/extra/mpc-1.3.1.tar.gz > /build/mpc.tar
	/build/miniextract /build/mpc.tar && mv mpc-1.3.1 mpc
	rm -f /build/gmp.tar /build/mpfr.tar /build/mpc.tar /build/miniextract.c /build/miniextract

	cat > /build/va_list.c <<'VA_LIST_C'
/* va_list.c - tinycc support for va_list on X86_64 */

#if defined __x86_64__

/* Avoid include files, they may not be available when cross compiling */
extern void *memset(void *s, int c, __SIZE_TYPE__ n);
extern void abort(void);

/* This should be in sync with our include/stdarg.h */
enum __va_arg_type {
    __va_gen_reg, __va_float_reg, __va_stack
};

/* GCC compatible definition of va_list. */
typedef struct {
    unsigned int gp_offset;
    unsigned int fp_offset;
    union {
        unsigned int overflow_offset;
        char *overflow_arg_area;
    };
    char *reg_save_area;
} __va_list_struct;

void __va_start(__va_list_struct *ap, void *fp)
{
    memset(ap, 0, sizeof(__va_list_struct));
    *ap = *(__va_list_struct *)((char *)fp - 16);
    ap->overflow_arg_area = (char *)fp + ap->overflow_offset;
    ap->reg_save_area = (char *)fp - 176 - 16;
}

void *__va_arg(__va_list_struct *ap,
               enum __va_arg_type arg_type,
               int size, int align)
{
    size = (size + 7) & ~7;
    align = (align + 7) & ~7;
    switch (arg_type) {
    case __va_gen_reg:
        if (ap->gp_offset + size <= 48) {
            ap->gp_offset += size;
            return ap->reg_save_area + ap->gp_offset - size;
        }
        goto use_overflow_area;

    case __va_float_reg:
        if (ap->fp_offset < 128 + 48) {
            ap->fp_offset += 16;
            return ap->reg_save_area + ap->fp_offset - 16;
        }
        size = 8;
        goto use_overflow_area;

    case __va_stack:
    use_overflow_area:
        ap->overflow_arg_area += size;
        ap->overflow_arg_area = (char*)((long long)(ap->overflow_arg_area + align - 1) & -align);
        return ap->overflow_arg_area - size;

    default: /* should never happen */
        abort();
    }
}
#endif
VA_LIST_C
	/usr/bin/gcc -c -O2 -fPIC /build/va_list.c -o /build/va_list.o

	cat > /build/alloca86_64.S <<'ALLOCA_S'
/* ---------------------------------------------- */
/* alloca86_64.S */

.globl alloca

alloca:
    pop     %rdx
#ifdef _WIN32
    mov     %rcx,%rax
#else
    mov     %rdi,%rax
#endif
    add     $15,%rax
    and     $-16,%rax
    jz      p3

#ifdef _WIN32
p1:
    cmp     $4096,%rax
    jbe     p2
    test    %rax,-4096(%rsp)
    sub     $4096,%rsp
    sub     $4096,%rax
    jmp p1
p2:
#endif

    sub     %rax,%rsp
    mov     %rsp,%rax
p3:
    push    %rdx
    ret

/* ---------------------------------------------- */
ALLOCA_S
	/usr/bin/gcc -c /build/alloca86_64.S -o /build/alloca.o

	# A third, related but distinct gap surfaced past that one: undefined
	# reference to `C_alloca` (`make-relative-prefix.c`, linked into
	# `gcc-ar` this time). Not a TCC lib/ runtime-helper gap like the two
	# above -- this one traces to `include/libiberty.h` itself:
	#   #if GCC_VERSION >= 2000 && !defined USE_C_ALLOCA
	#   # define alloca(x) __builtin_alloca(x)
	#   #else
	#   # define alloca(x) C_alloca(x)
	#   #endif
	# `GCC_VERSION` (from ansidecl.h) is `__GNUC__ * 1000 + __GNUC_MINOR__`,
	# 0 when `__GNUC__` is undefined -- which TCC never defines. So every
	# libiberty.a translation unit compiled by TCC that calls alloca()
	# through this header gets redirected to `C_alloca()`, a real,
	# portable, public-domain fallback implementation that GCC itself
	# ships for exactly this situation (`libiberty/alloca.c`) -- already
	# present, unmodified, in the same extracted source tree `pkg_source`
	# already downloaded, no separate fetch needed. It compiles clean
	# under plain tcc (confirmed locally: pure ISO C89, no GCC extensions
	# on the non-Cray path) given `HAVE_STDLIB_H`/`HAVE_STRING_H` forced on
	# the command line (its own `#ifdef HAVE_CONFIG_H` guard means it
	# otherwise silently skips `<stdlib.h>`/`<string.h>` when compiled
	# standalone, before libiberty's own generated config.h exists).
	tcc -c -DHAVE_STDLIB_H=1 -DHAVE_STRING_H=1 -I include libiberty/alloca.c \
	    -o /build/c_alloca.o

	# A fourth gap, past all three libiberty ones: undefined reference to
	# `__floatundidf` from `libgmp.a`/`libmpfr.a` (unsigned-64-bit-to-
	# double conversion), linked into `cc1`/`cc1plus` themselves this
	# time -- not libiberty at all. GMP/MPFR's portable C fallback paths
	# (no native asm tuning is selected by this generic in-tree build)
	# emit a call to this symbol instead of inline conversion code,
	# exactly the kind of compiler-runtime helper real GCC provides via
	# libgcc and TCC provides via its own `lib/libtcc1.c` -- the same
	# runtime-helper-injection class of gap as `va_list.c`/
	# `alloca86_64.S` above, just a different missing family. Extracted
	# (not the whole file, to avoid dragging in unrelated libtcc1.c
	# helpers like `__divdi3`/`__moddi3` that could collide with real
	# libgcc's own copies once linked) as the self-contained
	# float<->int64 conversion family plus the handful of typedefs/
	# macros it depends on (`DWunion`, `XFtype`, the `EXP*`/`MANT*`/
	# `union *_long` machinery) -- confirmed to compile clean and
	# export the whole family with real gcc.
	cat > /build/floatdi.c <<'FLOATDI_C'
#define W_TYPE_SIZE   32
#define BITS_PER_UNIT 8

typedef int Wtype;
typedef unsigned int UWtype;
typedef unsigned int USItype;
typedef long long DWtype;
typedef unsigned long long UDWtype;

struct DWstruct {
    Wtype low, high;
};

typedef union
{
  struct DWstruct s;
  DWtype ll;
} DWunion;

typedef long double XFtype;

#define EXCESS		126
#define SIGNBIT		0x80000000
#define HIDDEN		(1 << 23)
#define SIGN(fp)	((fp) & SIGNBIT)
#define EXP(fp)		(((fp) >> 23) & 0xFF)
#define MANT(fp)	(((fp) & 0x7FFFFF) | HIDDEN)
#define PACK(s,e,m)	((s) | ((e) << 23) | (m))

#define EXCESSD		1022
#define HIDDEND		(1 << 20)
#define EXPD(fp)	(((fp.l.upper) >> 20) & 0x7FF)
#define SIGND(fp)	((fp.l.upper) & SIGNBIT)
#define MANTD(fp)	(((((fp.l.upper) & 0xFFFFF) | HIDDEND) << 10) | \
				(fp.l.lower >> 22))
#define HIDDEND_LL	((long long)1 << 52)
#define MANTD_LL(fp)	((fp.ll & (HIDDEND_LL-1)) | HIDDEND_LL)
#define PACKD_LL(s,e,m)	(((long long)((s)+((e)<<20))<<32)|(m))

#define EXCESSLD	16382
#define EXPLD(fp)	(fp.l.upper & 0x7fff)
#define SIGNLD(fp)	((fp.l.upper) & 0x8000)

union ldouble_long {
    long double ld;
    struct {
        unsigned long long lower;
        unsigned short upper;
    } l;
};

union double_long {
    double d;
    struct {
        unsigned int lower;
        int upper;
    } l;
    long long ll;
};

union float_long {
    float f;
    unsigned int l;
};

float __floatundisf(unsigned long long a)
{
    DWunion uu;
    XFtype r;

    uu.ll = a;
    if (uu.s.high >= 0) {
        return (float)uu.ll;
    } else {
        r = (XFtype)uu.ll;
        r += 18446744073709551616.0;
        return (float)r;
    }
}

double __floatundidf(unsigned long long a)
{
    DWunion uu;
    XFtype r;

    uu.ll = a;
    if (uu.s.high >= 0) {
        return (double)uu.ll;
    } else {
        r = (XFtype)uu.ll;
        r += 18446744073709551616.0;
        return (double)r;
    }
}

long double __floatundixf(unsigned long long a)
{
    DWunion uu;
    XFtype r;

    uu.ll = a;
    if (uu.s.high >= 0) {
        return (long double)uu.ll;
    } else {
        r = (XFtype)uu.ll;
        r += 18446744073709551616.0;
        return (long double)r;
    }
}

unsigned long long __fixunssfdi (float a1)
{
    register union float_long fl1;
    register int exp;
    register unsigned long l;

    fl1.f = a1;

    if (fl1.l == 0)
	return (0);

    exp = EXP (fl1.l) - EXCESS - 24;

    l = MANT(fl1.l);
    if (exp >= 41)
	return (unsigned long long)-1;
    else if (exp >= 0)
        return (unsigned long long)l << exp;
    else if (exp >= -23)
        return l >> -exp;
    else
        return 0;
}

long long __fixsfdi (float a1)
{
    long long ret; int s;
    ret = __fixunssfdi((s = a1 >= 0) ? a1 : -a1);
    return s ? ret : -ret;
}

unsigned long long __fixunsdfdi (double a1)
{
    register union double_long dl1;
    register int exp;
    register unsigned long long l;

    dl1.d = a1;

    if (dl1.ll == 0)
	return (0);

    exp = EXPD (dl1) - EXCESSD - 53;

    l = MANTD_LL(dl1);

    if (exp >= 12)
	return (unsigned long long)-1;
    else if (exp >= 0)
        return l << exp;
    else if (exp >= -52)
        return l >> -exp;
    else
        return 0;
}

long long __fixdfdi (double a1)
{
    long long ret; int s;
    ret = __fixunsdfdi((s = a1 >= 0) ? a1 : -a1);
    return s ? ret : -ret;
}

unsigned long long __fixunsxfdi (long double a1)
{
    register union ldouble_long dl1;
    register int exp;
    register unsigned long long l;

    dl1.ld = a1;

    if (dl1.l.lower == 0 && dl1.l.upper == 0)
	return (0);

    exp = EXPLD (dl1) - EXCESSLD - 64;

    l = dl1.l.lower;

    if (exp > 0)
	return (unsigned long long)-1;
    else if (exp >= -63)
        return l >> -exp;
    else
        return 0;
}

long long __fixxfdi (long double a1)
{
    long long ret; int s;
    ret = __fixunsxfdi((s = a1 >= 0) ? a1 : -a1);
    return s ? ret : -ret;
}
FLOATDI_C
	/usr/bin/gcc -c -O2 -fPIC /build/floatdi.c -o /build/floatdi.o

	mkdir -p build
	cd build
	CC=tcc ../configure --prefix=/usr --disable-multilib --disable-bootstrap \
		--enable-languages=c,c++ --disable-libsanitizer --disable-lto --with-isl=no \
		--with-system-zlib

	i=0
	while [ "$i" -lt 10 ]; do
		find . -path './build-*' -name Makefile -exec sed -i \
		    -e 's|^CC = .*|CC = /usr/bin/gcc|' \
		    -e 's|^CXX = .*|CXX = /usr/bin/g++|' \
		    {} \;
		find . -path './build-*' \( -name '*.o' -o -name '*.a' -o -name '*.lo' \) -delete
		find . \( -name 'libiberty.a' -o -name 'libgmp.a' -o -name 'libmpfr.a' \) \
		    -exec /usr/bin/ar r {} \
		    /build/va_list.o /build/alloca.o /build/c_alloca.o /build/floatdi.o \;
		if make -j"$(nproc)"; then
			break
		fi
		i=$((i + 1))
	done

	# From here on, any failure is a genuinely new class of problem (past
	# all four injected-symbol gaps above), and autoconf-driven configure
	# failures at this depth (e.g. target libgcc's own configure, run
	# against the freshly self-built xgcc) only print "See config.log for
	# more details" to stdout/stderr -- the actual compiler invocation
	# and error text never reach the captured build log otherwise, which
	# is exactly the kind of blind-guessing trap this project's own
	# diagnose-before-perror discipline exists to avoid. Dump only the
	# single MOST RECENTLY WRITTEN config.log (the one the failing
	# configure step itself just produced) on final failure -- not every
	# config.log on disk (a full GCC tree has dozens, one per subproject
	# configured so far, and dumping all of them was tried first: it
	# overflowed thincd's own fixed-size captured-build-output buffer
	# before reaching the actually-relevant one, landing the truncated
	# tail on some unrelated, already-successful subproject's log
	# instead). `ls -t` ranks by mtime; the newest is the one that
	# matters.
	if ! make -j"$(nproc)"; then
		latest_config_log=$(find . -name config.log -printf '%T@ %p\n' | \
		    sort -rn | head -1 | cut -d' ' -f2-)
		if [ -n "$latest_config_log" ]; then
			echo "=== $latest_config_log (error context) ==="
			# A plain tail landed on config.log's own trailing
			# cache-variable/confdefs.h summary, not the actual
			# failing test -- config.log is chronological, and
			# that summary is appended after EVERY test, whether
			# or not it's the one that failed. grep straight for
			# the real "configure: error" line plus generous
			# context before it (the actual failing compiler
			# invocation and its real stdout/stderr, which is
			# what's actually needed) instead.
			grep -n -B 30 -A 5 '^configure: error' \
			    "$latest_config_log" | tail -n 150
		fi
		exit 1
	fi
}

# A real, genuinely new class of dependency this recipe set hasn't hit
# before: gcc does not compile C itself -- it's a driver program that
# shells out to real subprocess binaries for the actual work. cc1 (C),
# cc1plus (C++), and lto1 (link-time optimization) live under
# usr/libexec/gcc/x86_64-pc-linux-gnu/16.1.0/, found via a real build's
# own `make install` output, not guessed at -- without this exact
# directory, `gcc -c foo.c` fails outright with "cc1: not found", not a
# degraded-but-working state the way a missing optional runtime lib
# would be elsewhere in this project. Likewise usr/lib/gcc/
# x86_64-pc-linux-gnu/16.1.0/{crtbegin.o,crtend.o,crtbeginS.o,crtendS.o,
# crtbeginT.o,crtfastmath.o,crtprec32.o,crtprec64.o,crtprec80.o} (the
# real C runtime startup/teardown object files gcc links into every
# program it builds) and that same directory's libgcc.a/libgcc_eh.a/
# libgcov.a (gcc's own static support libraries) plus its include/ and
# include-fixed/ subdirectories (gcc's own freestanding headers --
# stdarg.h, float.h, etc. -- consulted before the system's own libc
# headers) are equally required, not optional. All of this is kept in
# full; every one of these paths was confirmed to actually exist in a
# real build's own `make install DESTDIR=...` output before being
# named here, not assumed from gcc's general reputation. cc1/cc1plus/
# lto1 (and every other binary here) are unstripped by default (~400MB
# each) -- stripped to their real working size (confirmed stripping
# doesn't affect gcc's own ability to invoke them) since none of this
# project's other recipes ship debug symbols either.
#
# A second, equally real private-search-path finding, caught only by
# actually compiling and linking a real C program inside a real running
# container (not just this recipe's own local build+install
# verification): collect2 (gcc's own linker-invocation driver) looks
# for `ld` via the same private COMPILER_PATH search as cc1/cc1plus,
# not via $PATH -- confirmed directly, `collect2: fatal error: cannot
# find 'ld'` on a real link attempt despite /usr/bin/ld genuinely
# existing and being on $PATH (binutils is a real pkg_depends= above),
# fixed by symlinking it into gcc's own private prefix directory below.
# Not gcc.recipe's own binary to ship (binutils.recipe already
# provides the real one) -- just a symlink to where gcc's own search
# convention expects to find it.
#
# Runtime libraries a program built with this gcc actually needs,
# confirmed via ldd against a real compiled/linked test binary: libc.so
# already covered globally; libgcc_s.so.1 (unwinding/exception support,
# needed by virtually anything, C included, once -fexceptions or stack
# unwinding on signals is involved) and libstdc++.so.6 (needed by any
# real C++ program) are the two that matter for typical use, staged
# with their SONAME symlink chains the same way every other recipe's
# runtime libs already are; libatomic/libgomp/libitm/libquadmath/
# libssp (OpenMP, transactional memory, quad-precision math, stack
# protector) are real, less commonly needed but genuinely part of a
# complete gcc runtime, kept too since dropping them would silently
# break any real program that does need them, and they're not large.
# C++ standard library headers (usr/include/c++/16.1.0/) are kept in
# full -- without them g++ cannot compile any real C++ program at all,
# the same "load-bearing, not documentation" reasoning already applied
# to autoconf/automake/perl's own runtime data directories.
#
# Triplet-prefixed duplicate binaries (x86_64-pc-linux-gnu-gcc, etc --
# confirmed via `ls -la` to be real hardlinks of the plain-named ones,
# so dropping them costs zero extra disk, just directory-entry
# cleanliness) are skipped, matching binutils.recipe's own precedent --
# this project never cross-compiles. gdb pretty-printer scripts
# (usr/share/gcc-16.1.0/python), the install-only fixincl/fixinc.sh/
# mkheaders tools (used solely during gcc's own build, not afterward),
# the gdb-JIT-compile plugin directory (libcc1plugin/libcp1plugin,
# ~13MB, a real but niche gdb integration feature), and man/info/locale
# docs are all dropped.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	find "$PKG_DESTDIR" -type f -executable -exec sh -c \
		'file "$1" | grep -q "not stripped" && strip --strip-unneeded "$1"' _ {} \;
	rm -f "$PKG_DESTDIR"/usr/bin/x86_64-pc-linux-gnu-*
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" "$PKG_DESTDIR/usr/share/locale" \
	       "$PKG_DESTDIR/usr/share/gcc-16.1.0" \
	       "$PKG_DESTDIR/usr/libexec/gcc/x86_64-pc-linux-gnu/16.1.0/install-tools" \
	       "$PKG_DESTDIR/usr/lib/gcc/x86_64-pc-linux-gnu/16.1.0/plugin" \
	       "$PKG_DESTDIR/usr/lib/gcc/x86_64-pc-linux-gnu/16.1.0/install-tools"
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp -a "$PKG_DESTDIR/usr/lib64/libgcc_s.so.1" \
	   "$PKG_DESTDIR/usr/lib64/libstdc++.so.6" "$PKG_DESTDIR/usr/lib64/libstdc++.so.6.0.35" \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
	ln -sf /usr/bin/ld "$PKG_DESTDIR/usr/libexec/gcc/x86_64-pc-linux-gnu/16.1.0/ld"
}
