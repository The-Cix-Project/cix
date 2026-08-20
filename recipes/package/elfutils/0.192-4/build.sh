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
pkg_version="0.192-4"
pkg_source="https://sourceware.org/elfutils/ftp/0.192/elfutils-0.192.tar.bz2"
pkg_sha256="616099beae24aba11f9b63d86ca6cc8d566d968b802391334c91df54eab416b4"
pkg_depends=""

# 0.192's own bare `CC=tcc ./configure` failed outright at configure
# time on TCC's own genuine, unfixable gap: "checking for __thread
# support... no" -> "configure: error: __thread support required".
# Confirmed via a minimal standalone probe (`__thread int x = 5;
# int main(){ return x; }`): TCC's parser rejects `__thread` outright
# (`error: ';' expected (got "int")`), not just at link/runtime.
# Fixed the same way kernel.recipe's own HOSTCC/CC already does for
# its own TCC-incompatible host tooling: force real, already-installed
# gcc for this package's build instead.
#
# That got configure past __thread, but hit its own next real gap:
# "checking for __cxa_demangle in -lstdc++... no" -> "configure:
# error: __cxa_demangle not found in libstdc++, use --disable-demangler
# to disable demangler support." No libstdc++ package is staged in the
# dev image (gcc.recipe ships a C compiler only) -- the demangler is a
# real, optional eu-readelf/eu-nm feature (pretty-printing mangled C++
# symbol names), entirely orthogonal to this recipe's actual purpose
# (libelf, for the kernel's own tools/objtool). configure's own
# suggested flag disables exactly that one optional feature.
#
# With --disable-demangler, configure completed and `make` reached
# real compilation, then failed on lib/color.c: glibc's fortified
# wctomb() (bits/stdlib.h) hit `#error "Assumed value of MB_LEN_MAX
# wrong"`. Root-caused directly (probe-gcc-headers v3/v4, not
# guessed): color.c includes <argp.h> before <stdlib.h>, and
# /usr/include/argp.h itself `#include <limits.h>` (line 26) --
# reaching gcc's own private include-fixed/limits.h first in the
# search order (as always), which is the SAME broken,
# never-finished-by-fixincludes raw glimits.h template
# kernel.recipe's own -8 revision already root-caused for its PATH_MAX
# gap (issue #55) -- except here the symptom is different: that raw
# template's own placeholder line (`#ifndef MB_LEN_MAX #define
# MB_LEN_MAX 1 #endif`, confirmed via a direct grep of the file, not
# assumed) IS reached and taken, and its own trailing `#ifdef
# _GCC_NEXT_LIMITS_H #include_next <limits.h> #endif` never fires
# (that macro is never set), so glibc's own real limits.h (which
# would otherwise correctly redefine MB_LEN_MAX to 16) is never
# reached -- MB_LEN_MAX stays wrongly pinned at gcc's own raw
# placeholder value of 1, tripping bits/stdlib.h's own sanity check
# against its hardcoded-correct 16.
#
# Same real, standard fix as kernel.recipe -8 (this is literally what
# gcc's own gsyslimits.h template is designed to be, not a hack):
# replace the broken file with a minimal, correct pass-through that
# defines _GCC_LIMITS_H_ and #include_next's onward to glibc's real
# header immediately -- applied here as this recipe's own scoped,
# build-container-local workaround (this build's own upperdir, not a
# change to the shared toolchain image), same as kernel.recipe. This
# confirms the bug is a real gcc.recipe-level defect (issue #55)
# affecting the whole pkgbuild sandbox baseline, not kernel-specific --
# any future recipe compiling real code with CC=/usr/bin/gcc that
# touches <limits.h> (directly or transitively) should expect the
# same class of gap and reach for this same fix first.
pkg_build() {
	cat > /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/limits.h <<'EOF'
#ifndef _GCC_LIMITS_H_
#define _GCC_LIMITS_H_
#include_next <limits.h>
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
