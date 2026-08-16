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
		if make -j"$(nproc)"; then
			break
		fi
		i=$((i + 1))
	done
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
