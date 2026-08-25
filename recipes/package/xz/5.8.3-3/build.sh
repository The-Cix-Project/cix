#
# xz -- the LZMA2 compressor CLI. GNU tar (tar.recipe) has no
# compression code of its own and shells out to a bare "xz" resolved
# via $PATH for every .tar.xz source (the same real gap this recipe
# set's own bzip2.recipe/gzip.recipe already close for their own
# formats).
#
# **IMPORTANT security note carried forward from upstream's own
# release announcement**: 5.8.3 fixes CVE-2026-34743 (a buffer
# overflow in lzma_index_append() reachable only via a narrow,
# unlikely API-usage pattern -- xz's own release notes call it
# unlikely to be triggerable by any real-world application, including
# this project's own plain CLI extraction use). Pinned here anyway,
# as the current, real, non-vulnerable release.
#
# Source is xz's own canonical GitHub release (the project's actual
# upstream host, tukaani-project/xz), checksum verified against a
# second independent source: Debian's own orig tarball content
# (different compression format, .tar.xz vs. this recipe's .tar.gz --
# verified via a real `diff -r` of both extracted trees, byte-for-byte
# identical, rather than a raw file checksum since the two container
# formats can never produce the same sha256 regardless of content).
#
# 5.8.3-2, one real fix from 5.8.3 -- the first real build-image
# rebuild of this recipe (building cix-hosttools, task #865) hit a
# genuine TCC gap that 5.8.3's own "not rebuild-verified" disclosure
# (task #845's CC=tcc audit) had flagged as a real, open risk:
# src/liblzma/common/common.c's own HAVE_SYMBOL_VERSIONS_LINUX branch
# expands LZMA_SYMVER_API() into a raw __asm__(".symver ...") ELF
# symbol-versioning directive TCC's assembler does not implement
# ("error: unknown opcode '.symver'"), gated behind configure's own
# --enable-symbol-versions probe (on by default when the target looks
# ELF/glibc-like, which this one genuinely is -- the probe itself
# isn't wrong, TCC's own assembler support is just narrower than
# real GNU as). Fixed the correct way -- a real, upstream-documented
# `./configure --disable-symbol-versions` flag (confirmed via a local
# `./configure --help` against this exact source tree), not a source
# patch: this project never modifies unmodified upstream code, only
# ever configures it correctly for the toolchain actually building it.
# Symbol versioning only matters for a shared library shipping
# multiple, simultaneously-installed ABI-compatible versions side by
# side on a glibc system with versioned symbols already in play --
# irrelevant here, this recipe's own liblzma.so is the only copy ever
# present on any image it's installed onto.
#
pkg_name="xz"
pkg_version="5.8.3-3"
pkg_source="https://github.com/tukaani-project/xz/releases/download/v5.8.3/xz-5.8.3.tar.gz"
pkg_sha256="3d3a1b973af218114f4f889bbaa2f4c037deaae0c8e815eec381c3d546b974a0"
# Nothing at runtime: liblzma links against libc alone.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD,
# declared rather than inherited from whatever a shared sandbox
# happened to accumulate. The build container is composed from exactly
# these and nothing else, so this list is not documentation -- it is
# the environment.
#
# Each entry earns its place:
#   tcc       the compiler this recipe pins with CC=tcc
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln, used throughout configure,
#             config.status and the Makefile
#   sed       an autoconf configure rewrites its own output with sed on
#             essentially every substitution it makes
#   grep      the same, for every feature test that greps a compiler or
#             header for a pattern
#   gawk      config.status generates every Makefile through awk --
#             measured, not assumed: a build environment without it
#             fails at `config.status: line NNNN: awk: command not
#             found` (see diffutils 3.10-3)
#   binutils  gnulib is archived into a static convenience library
#             before the final link, which needs ar and ranlib
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils"


# Plain autotools, confirmed directly. --disable-doc skips the real
# generated HTML/man doc tree (matching every other recipe's own
# doc-stripping convention) -- xz's own configure calls this "doc",
# not "docs" (confirmed via a real local `./configure --help`, unlike
# curl's own --disable-docs spelling above). --disable-symbol-versions
# is the real TCC-compatibility fix this revision adds (see header).
pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-doc --disable-symbol-versions
	make -j"$(nproc)"
}

# Real files from this recipe's own build. usr/share (man pages in a
# dozen languages) is dropped, matching every other recipe's own
# doc-stripping convention. The lzcat/lzma/unlzma/etc. compatibility
# symlinks and xzdiff/xzgrep/xzless shell-script wrappers are real,
# genuinely part of upstream's own install, and kept -- they're plain
# files/symlinks alongside the real xz binary, no extra weight to
# justify stripping them individually the way the large translated doc
# tree is.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share"
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	mv "$PKG_DESTDIR/usr/lib/liblzma.so.5.8.3" "$PKG_DESTDIR/usr/lib/liblzma.so.5" \
	   "$PKG_DESTDIR/usr/lib/liblzma.so" "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
	rm -f "$PKG_DESTDIR/usr/lib/liblzma.a" "$PKG_DESTDIR/usr/lib/liblzma.la"
	rm -rf "$PKG_DESTDIR/usr/lib/pkgconfig"
}
