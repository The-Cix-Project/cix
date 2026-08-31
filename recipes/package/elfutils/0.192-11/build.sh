#
# elfutils -- provides libelf, the ELF object file library. A
# genuinely missing dependency confirmed empirically (ADR-0056): a
# real kernel hostbuild against the "dev" toolchain image failed at
# `tools/objtool` (needed for ORC/stack-validation metadata) with
# "cannot find -lelf". Nothing else in this recipe catalog happened to
# need it before now.
#
# Source is elfutils' own canonical sourceware.org release.
#
pkg_name="elfutils"
pkg_version="0.192-11"
pkg_source="https://sourceware.org/elfutils/ftp/0.192/elfutils-0.192.tar.bz2"
pkg_sha256="616099beae24aba11f9b63d86ca6cc8d566d968b802391334c91df54eab416b4"
# libelf.so.1 links against all three, for the compressed-section
# support that is most of the reason to use libelf at all. Read off the
# built library rather than reasoned about:
#
#   readelf -d /usr/lib/libelf.so.1
#     NEEDED  libz.so.1
#     NEEDED  liblzma.so.5
#     NEEDED  libbz2.so.1.0
#     NEEDED  libc.so.6
#
# Through -6 this said "" -- so `dev`, the image the kernel is built
# in, has been carrying a libelf that cannot load for as long as
# nothing happened to check. It surfaced when the kernel hostbuild for
# issue #114 linked tools/objtool against it:
#
#   ld: warning: liblzma.so.5, needed by .../libelf.so, not found
#   ld: warning: libbz2.so.1.0, needed by .../libelf.so, not found
#
# A confirmed instance of #110: this package used to stage the build
# host's Debian copies of those libraries, so they were present by
# accident rather than by declaration, and stopped being present the
# moment that stopped happening. Both are real packages in this
# catalog, so the honest fix is to name them.
pkg_depends="zlib xz bzip2"
# ADR-0199/0209: the build environment is composed from exactly these
# and nothing else -- there is no fallback to inherit a missing tool
# from (#168). -8 declared nothing at all, so it could not be built at
# ALL any more: the install fails before fetching a byte, naming the
# recipe. That is why elfutils-0.192-8 is absent from the artifact
# cache while every other kernel-builder pin is present, and why
# kernel-builder could not be composed.
#
# Read off this recipe's own build steps and elfutils' own configure,
# not copied from a similar recipe:
#   bash coreutils   the recipe functions, plus nproc/cat/test
#   make             `make -C lib` / `make -C libelf`
#   gcc binutils     CC=/usr/bin/gcc is forced here (TCC has a real
#                    __thread-parsing gap, see -5's history), and gcc
#                    shells out to as/ld
#   libc-dev         headers and CRT
#   sed grep gawk findutils diffutils
#                    what an autoconf configure reaches for on
#                    essentially every substitution and feature test
#   pkgconf          configure runs PKG_PROG_PKG_CONFIG even with
#                    --disable-debuginfod, which is what removes the
#                    libcurl PKG_CHECK_MODULES but not the probe itself
#   bison flex m4    configure.ac calls AC_PROG_YACC and AC_PROG_LEX
#                    unconditionally -- they are probed even though
#                    only lib/ and libelf/ are actually built here,
#                    and bison drives m4
#   zlib xz bzip2    named here as well as in pkg_depends: a composed
#                    environment is built from pkg_build_depends ALONE,
#                    so the headers and libraries libelf links for its
#                    compressed-section support have to be present to
#                    compile against, not merely installed alongside
#                    the result. Same reason sbsigntools names gnu-efi/
#                    libuuid/binutils-dev twice.
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
pkg_build_depends="bash coreutils make gcc binutils linux-headers sed grep gawk findutils diffutils pkgconf bison flex m4 zlib xz bzip2"
pkg_changelog="0.192-11: ships libelf.pc, without which pkg-config cannot see a libelf that is genuinely installed -- iproute2 silently built with ELF support off as a result. 0.192-10: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

# 0.192's own bare `CC=tcc ./configure` failed on TCC's own genuine
# `__thread`-parsing gap (confirmed via a minimal standalone probe --
# see 0.192-5's own history for the full trail); fixed by forcing real
# gcc for this recipe's build, same pattern kernel.recipe's own
# HOSTCC/CC already establishes.
#
# `CC=gcc` alone reached a real, previously-undiscovered defect in
# this pkgbuild sandbox's own gcc install (root-caused via
# probe-gcc-headers v3 through v6, fully documented in 0.192-5's own
# history): include-fixed/syslimits.h is a byte-for-byte identical
# copy of limits.h instead of the short, distinct `#include_next`
# chaining wrapper a complete fixincludes/mkheaders run generates
# there, so glibc's own real /usr/include/limits.h (MB_LEN_MAX=16,
# PATH_MAX, etc.) is never actually reached through <limits.h> at all.
# Fixed the same way 0.192-5 already established: replace ONLY
# syslimits.h with the correct wrapper (limits.h itself is untouched
# and already correct).
#
# With that fixed, 0.192-5's own full `make all` reached real linking
# and failed there instead: elfutils' own src/ tools (readelf, nm,
# strip, ...) and libdw both try to link against liblzma/libbz2/zlib
# for compressed-debuginfo support ("undefined reference to
# `lzma_end'", "not found (try using -rpath)", one real ABI mismatch
# `/lib/libz.so.1: undefined reference to `__va_start'`) -- none of
# which this recipe actually needs: the kernel's own tools/objtool
# only ever links against libelf itself (`-lelf`), never
# libdw/libasm/libebl or any of the src/ CLI tools. Rather than chase
# a compression-library ABI problem this recipe has no real use for,
# this version builds and installs ONLY libelf (`make -C lib` first --
# libelf.so's own build rule genuinely depends on ../lib/libeu.a per
# libelf/Makefile.am's own libelf_so_DEPS, confirmed by reading it
# directly -- then `make -C libelf`), skipping libdw/libasm/backends/
# debuginfod/src/tests entirely. --disable-debuginfod/
# --disable-libdebuginfod/--disable-demangler are kept at configure
# time regardless, since configure itself still probes for them even
# though this build never reaches the subdirectories that would need
# them.
pkg_build() {
	# -8: the gcc version is asked for, not assumed. -7 wrote this file
	# to a hardcoded .../12.5.0/include-fixed/ path -- the gcc that
	# happened to be installed when the fix was written -- so the
	# moment a build environment carried a different gcc the recipe
	# failed with a bare "No such file or directory" naming a path
	# nobody had thought about since. Found building the new
	# kernel-builder image (ADR-0208), which pins gcc 16.2.0.
	GCC_VERSION=$(/usr/bin/gcc -dumpversion) || {
		echo "elfutils: no /usr/bin/gcc in this build environment -- it is required" >&2
		exit 1
	}
	GCC_FIXED_DIR="/usr/lib/gcc/x86_64-pc-linux-gnu/$GCC_VERSION/include-fixed"
	test -d "$GCC_FIXED_DIR" || {
		echo "elfutils: gcc $GCC_VERSION has no $GCC_FIXED_DIR to correct" >&2
		exit 1
	}
	cat > "$GCC_FIXED_DIR/syslimits.h" <<'EOF'
#ifndef _GCC_NEXT_LIMITS_H
#define _GCC_NEXT_LIMITS_H
#include_next <limits.h>
#undef _GCC_NEXT_LIMITS_H
#endif
EOF

	CC=/usr/bin/gcc ./configure --prefix=/usr --disable-debuginfod \
	    --disable-libdebuginfod --disable-demangler
	make -C lib -j"$(nproc)" CC=/usr/bin/gcc
	make -C libelf -j"$(nproc)" CC=/usr/bin/gcc
}

pkg_install() {
	make -C libelf install DESTDIR="$PKG_DESTDIR" CC=/usr/bin/gcc

	#
	# libelf.pc is generated by configure at config/libelf.pc but
	# INSTALLED by config/Makefile, which this recipe never runs -- it
	# installs only the libelf subdirectory. So a fully installed libelf
	# (headers, .so, SONAME symlinks) was invisible to pkg-config.
	#
	# That is not cosmetic. iproute2's configure probes for libelf via
	# pkg-config and degrades silently when it is not found: it printed
	# "ELF support: no" and would have built an `ip` without BPF program
	# loading, exiting 0 the whole way. The guide's rule is that a .pc
	# must be a TRUE claim about what the package ships -- this package
	# ships the headers and the library, so the claim is true and the
	# file belongs here.
	#
	# Copied individually rather than via `make -C config install`,
	# because that target also installs libdw.pc and libdebuginfod.pc,
	# describing two libraries this package deliberately does NOT ship.
	# Those would be false claims, which the same rule forbids and which
	# fail later and more confusingly than a missing file.
	#
	mkdir -p "$PKG_DESTDIR/usr/lib/pkgconfig"
	cp config/libelf.pc "$PKG_DESTDIR/usr/lib/pkgconfig/libelf.pc"

	# Assert the claim it makes is actually satisfied by this package.
	test -e "$PKG_DESTDIR/usr/lib/pkgconfig/libelf.pc"
	test -e "$PKG_DESTDIR/usr/include/libelf.h"
	test -e "$PKG_DESTDIR/usr/lib/libelf.so"
	for stale in libdw.pc libdebuginfod.pc; do
		if [ -e "$PKG_DESTDIR/usr/lib/pkgconfig/$stale" ]; then
			echo "$stale describes a library this package does not ship" >&2
			exit 1
		fi
	done
}
