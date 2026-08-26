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
pkg_version="0.192-6"
pkg_source="https://sourceware.org/elfutils/ftp/0.192/elfutils-0.192.tar.bz2"
pkg_sha256="616099beae24aba11f9b63d86ca6cc8d566d968b802391334c91df54eab416b4"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/elfutils-0.192-6.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="383d042799208e8daf73ab583d38615f6b526d49178e2a0e681b4b95d0e10354"
pkg_depends=""

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
	cat > /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/syslimits.h <<'EOF'
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
}
