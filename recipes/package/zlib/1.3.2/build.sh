#
# zlib -- the DEFLATE compression library. A genuinely missing
# dependency confirmed empirically (ADR-0056): elfutils' libelf.so
# (elfutils.recipe) links against zlib for compressed-ELF-section
# support (linked at build time only because the shared toolchain
# sandbox happens to carry the real host's own zlib -- but zlib was
# never staged into any *target* image, so libelf.so ended up with an
# unresolvable runtime dependency: "libz.so.1 ... not found" and
# undefined references to inflate/deflate/etc. at kernel-hostbuild
# link time). Nothing else in this recipe catalog happened to need it
# as a target-image package before now.
#
# Source is zlib's own canonical zlib.net release.
#
pkg_name="zlib"
pkg_version="1.3.2"
pkg_source="https://zlib.net/zlib-1.3.2.tar.gz"
pkg_sha256="bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/zlib-1.3.2.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="ff317e0daa44e68a62d0d1c7c33c377070fc82919e069596d42ced7c7bc18fec"
pkg_depends=""

# zlib's own configure is a hand-written script, not autoconf, but
# accepts the same --prefix= convention.
pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/lib"/*.a
}
