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
pkg_version="1.0.8-3"
pkg_changelog="1.0.8-3: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 1.0.8-2: declare build tools so this can be rebuilt from source (#206)"
pkg_source="https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz"
pkg_sha256="ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/bzip2-1.0.8.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="1edac0dc3adf872e5015c9484d9444fce8bfe753f01ddcde5f72c12a42810ec4"
pkg_depends=""

# pkg_build_depends added (#206): this recipe declared none, so
# ADR-0199 refused it outright and it could not be rebuilt at all.
# The set is the baseline the already-declaring recipes converge on for
# a package of this shape, and nothing this recipe's own pkg_build()
# does asks for more. If that turns out to be incomplete the build says
# so by name -- which is how libxcrypt and findutils were found for the
# first three conversions.
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils"

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
