#
# elfutils -- provides libelf, the ELF object file library. A
# genuinely missing dependency confirmed empirically (ADR-0056): a
# real kernel hostbuild against the "dev" toolchain image failed at
# `tools/objtool` (needed for ORC/stack-validation metadata) with
# "cannot find -lelf". Nothing else in this recipe catalog happened to
# need it before now.
#
# Compression support (zlib/bzlib/lzma) and debuginfod are all opt-in
# via --with-*/--enable-* flags (confirmed via ./configure --help) --
# a plain configure needs nothing beyond this project's existing
# libc-dev + gcc toolchain, no dependency cascade.
#
# Source is elfutils' own canonical sourceware.org release.
#
pkg_name="elfutils"
pkg_version="0.192-5"
pkg_source="https://sourceware.org/elfutils/ftp/0.192/elfutils-0.192.tar.bz2"
pkg_sha256="616099beae24aba11f9b63d86ca6cc8d566d968b802391334c91df54eab416b4"
pkg_depends=""

# 0.192's own bare `CC=tcc ./configure` failed on TCC's own genuine,
# unfixable gap: "checking for __thread support... no" -> "configure:
# error: __thread support required". Confirmed via a minimal standalone
# probe (`__thread int x = 5; int main(){ return x; }`): TCC's parser
# rejects `__thread` outright (`error: ';' expected (got "int")`).
# Fixed the same way kernel.recipe's own HOSTCC/CC already does for
# its own TCC-incompatible host tooling: force real, already-installed
# gcc instead.
#
# That reached configure's own next real gap: no libstdc++ staged in
# this dev image (gcc.recipe ships a C compiler only), so its optional
# eu-readelf/eu-nm C++-symbol demangler support can't be built --
# --disable-demangler turns off exactly that one optional feature,
# unrelated to this recipe's actual purpose (libelf).
#
# That got past configure entirely, into real compilation, which then
# failed on lib/color.c: glibc's fortified wctomb() (bits/stdlib.h)
# hit `#error "Assumed value of MB_LEN_MAX wrong"`. Root-caused fully
# (probe-gcc-headers v3 through v6, not guessed at any step):
#
#   - color.c includes <argp.h> before <stdlib.h>, and argp.h itself
#     `#include <limits.h>` -- reaching gcc's own private
#     include-fixed/limits.h first, as always.
#   - That file's own content is CORRECT and matches a real working
#     reference gcc (Debian 12.2.0's own gcc limits.h) byte-for-byte
#     at every structural point checked -- it is NOT broken or
#     truncated, contrary to this recipe catalog's earlier working
#     theory (kernel.recipe -8's own "replace include-fixed/limits.h
#     wholesale" fix, issue #55).
#   - The real, actual defect: include-fixed/syslimits.h -- confirmed
#     via direct md5sum -- is a byte-for-byte IDENTICAL COPY of
#     limits.h, not the short, distinct chaining wrapper a complete
#     fixincludes/mkheaders run is supposed to generate there
#     (`#ifndef _GCC_NEXT_LIMITS_H / #define _GCC_NEXT_LIMITS_H /
#     #include_next <limits.h> / #undef _GCC_NEXT_LIMITS_H / #endif`).
#     gcc's own limits.h unconditionally `#include "syslimits.h"`
#     right after defining _GCC_LIMITS_H_ (see its own line 34) --
#     since syslimits.h is really just limits.h again, _GCC_LIMITS_H_
#     is already defined by the time it's re-entered, so it takes the
#     opposite (`#else`) branch, finds `_GCC_NEXT_LIMITS_H` was never
#     set (only the real wrapper sets it), and does nothing -- glibc's
#     real /usr/include/limits.h is never reached at all through this
#     path. gcc's own placeholder `#ifndef MB_LEN_MAX #define
#     MB_LEN_MAX 1 #endif` therefore always wins (glibc's own correct,
#     unconditional `#define MB_LEN_MAX 16` never gets the chance to
#     run first and be seen by that #ifndef).
#
# Real, minimal, correct fix: replace ONLY syslimits.h with the
# standard wrapper content above -- limits.h itself is untouched, so
# every ISO limit macro it already correctly derives from compiler
# builtins (UCHAR_MAX, INT_MAX, etc. -- the exact things kernel.recipe
# -8's cruder "replace limits.h wholesale" fix silently discarded,
# surfaced here as elfutils 0.192-4's own real "UCHAR_MAX undeclared"
# build failure) keeps working, while the one broken link in the
# chain (syslimits.h never actually chaining to glibc) is fixed so
# MB_LEN_MAX/PATH_MAX/etc. from glibc's real header are also reached,
# same as any complete, working gcc installation. Applied here as this
# recipe's own scoped, build-container-local workaround (this build's
# own upperdir), same posture as kernel.recipe -8. gcc.recipe's own
# permanent fix (issue #55, updated with this fuller root cause) still
# needs its mkheaders/fixincludes step re-run properly, needing a full
# --enable-bootstrap rebuild to verify -- not done here.
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
	make -j"$(nproc)" CC=/usr/bin/gcc
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" CC=/usr/bin/gcc
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" \
	       "$PKG_DESTDIR/usr/lib"/*.a
}
