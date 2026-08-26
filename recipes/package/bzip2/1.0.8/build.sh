#
# bzip2 -- the DEFLATE-alternative block-sorting compressor CLI. GNU
# tar (tar.recipe) has no compression code of its own and shells out
# to a bare "bzip2" resolved via $PATH for every .tar.bz2 source
# (confirmed via a real strace on a live extraction, documented in
# mkbootroot.c's own header comment for this exact gap).
#
# bzip2 has no autotools build -- a plain, hand-written Makefile
# (static bzip2, no DESTDIR support) plus a separate Makefile-libbz2_so
# for the shared library. Confirmed via a real local build: running
# plain `make` first and then `make -f Makefile-libbz2_so` without a
# `make clean` between them fails ("relocation ... can not be used
# when making a shared object; recompile with -fPIC") because the
# plain target's own non-PIC .o files are still lying around --
# pkg_build() below runs `make clean` between the two for exactly this
# reason. No pkg_depends: needs nothing beyond libc.
#
# Source has no independently-hosted second copy at sourceware.org
# itself (single canonical host) -- cross-checked instead against
# Debian's own orig tarball (deb.debian.org), byte-identical, same
# sha256.
#
pkg_name="bzip2"
pkg_version="1.0.8"
pkg_source="https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz"
pkg_sha256="ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/bzip2-1.0.8.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="94057e0f7d43c131b3db7fc94c6a8db6353f276270b8ec44a5f1e49c5a877a8a"
pkg_depends=""

pkg_build() {
	make clean
	make -f Makefile-libbz2_so -j"$(nproc)" CC=tcc
}

# No `make install` target supports DESTDIR here (a real, confirmed
# limitation of bzip2's own hand-written Makefile) -- files are copied
# directly. bzip2-shared (dynamically linked against libbz2.so.1.0,
# confirmed live via a real compress+decompress round trip) becomes
# the installed "bzip2" -- bzip2.recover and the statically-linked
# "bzip2" from the plain `make` target (wiped by `make clean` above,
# never built here) are neither present nor needed: nothing in this
# project calls bzip2recover.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin" "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp bzip2-shared "$PKG_DESTDIR/usr/bin/bzip2"
	cp libbz2.so.1.0.8 "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
	ln -s libbz2.so.1.0.8 "$PKG_DESTDIR/lib/x86_64-linux-gnu/libbz2.so.1.0"
}
