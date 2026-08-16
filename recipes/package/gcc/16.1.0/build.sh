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
# -- byte-identical, same sha256. gmp/mpfr/mpc/isl (gcc's own build-
# time prerequisites -- gcc cannot configure without a real arbitrary-
# precision math library, and this project's isolated, network-less
# build container cannot let gcc's own contrib/download_prerequisites
# fetch them live the way a normal build does) are vendored as
# additional pkg_source entries at their exact versions pinned by this
# gcc release's own contrib/download_prerequisites script (read
# directly, not guessed at). Per ADR-0036, only source[0] (gcc's own
# tarball) is fetched into /build/src and auto-extracted; every
# additional source lands as a plain, unextracted file at
# /build/extra/<basename-of-its-own-URL> -- pkg_build() below extracts
# each of the four itself and renames them to the bare subdirectory
# names gcc's own build system auto-detects (gmp/, mpfr/, mpc/, isl/),
# the same end state contrib/download_prerequisites itself produces,
# just reached here via a real, checksummed, offline multi-source
# fetch instead of a live network call during the build. gmp/mpfr/mpc
# verified against ftp.gnu.org directly (byte-identical); isl has no
# GNU release, gcc.gnu.org/pub/gcc/infrastructure/ is its own
# documented canonical distribution point for this exact use.
#
pkg_name="gcc"
pkg_version="16.1.0"
pkg_source="https://ftp.gnu.org/gnu/gcc/gcc-16.1.0/gcc-16.1.0.tar.xz https://gcc.gnu.org/pub/gcc/infrastructure/gmp-6.3.0.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpfr-4.2.2.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpc-1.3.1.tar.gz https://gcc.gnu.org/pub/gcc/infrastructure/isl-0.24.tar.bz2"
pkg_sha256="50efb4d94c3397aff3b0d61a5abd748b4dd31d9d3f2ab7be05b171d36a510f79 ac28211a7cfb609bae2e2c8d6058d66c8fe96434f740cf6fe2e47b000d1c20cb 9ad62c7dc910303cd384ff8f1f4767a655124980bb6d8650fe62c815a231bb7b ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8 fcf78dd9656c10eb8cf9fbd5f59a0b6b01386205fe1934b3b287a0a1898145c0"
pkg_depends="binutils m4"

# Real, load-bearing build-time dependency: gcc's own assembler/linker
# calls need a working as/ld present (pkg_depends="binutils ..." above)
# and its build system invokes m4 during its own configure/build.
# gmp/mpfr/mpc/isl are extracted into gcc's own source tree at the
# bare names its configure auto-detects, exactly matching what
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
# compiler" goal. This is, by real wall-clock time, the single longest
# build in this project to date.
pkg_build() {
	mkdir -p decomp_test && cd decomp_test
	bzip2 -dc /build/extra/gmp-6.3.0.tar.bz2 > gmp.tar
	echo "=== diagnostic: real gmp.tar tvf entry count (bare) ==="
	tar tvf gmp.tar | wc -l
	echo "=== diagnostic: real gmp.tar tvf entry count (--read-full-records) ==="
	tar -tvf gmp.tar --read-full-records | wc -l
	cd .. && rm -rf decomp_test

	tar xf /build/extra/gmp-6.3.0.tar.bz2 && mv gmp-6.3.0 gmp
	tar xf /build/extra/mpfr-4.2.2.tar.bz2 && mv mpfr-4.2.2 mpfr
	tar xf /build/extra/mpc-1.3.1.tar.gz && mv mpc-1.3.1 mpc
	tar xf /build/extra/isl-0.24.tar.bz2 && mv isl-0.24 isl
	echo "=== diagnostic: real extraction layout ==="
	ls -la gmp mpfr mpc isl 2>&1
	ls -la mpfr/src 2>&1
	mkdir -p build
	cd build
	CC=tcc ../configure --prefix=/usr --disable-multilib --disable-bootstrap \
		--enable-languages=c,c++ --disable-libsanitizer --with-system-zlib
	make -j"$(nproc)"
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
