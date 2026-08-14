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
pkg_name="xz"
pkg_version="5.8.3"
pkg_source="https://github.com/tukaani-project/xz/releases/download/v5.8.3/xz-5.8.3.tar.gz"
pkg_sha256="3d3a1b973af218114f4f889bbaa2f4c037deaae0c8e815eec381c3d546b974a0"
pkg_depends=""

# Plain autotools, confirmed directly. --disable-doc skips the real
# generated HTML/man doc tree (matching every other recipe's own
# doc-stripping convention) -- xz's own configure calls this "doc",
# not "docs" (confirmed via a real local `./configure --help`, unlike
# curl's own --disable-docs spelling above).
pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-doc
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
