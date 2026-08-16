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
# Re-pinned to -2 (no real upstream change -- ROADMAP Part 171): a
# deliberate, trivial re-pin purely to verify the rolling-release
# "an image with follow_rolling picks up a newer recipe automatically"
# mechanism end to end against a real box, requested directly by the
# user. Same pkg_source/pkg_sha256 as 1.3.2, genuinely rebuilt under
# a newer version string -- the whole point of this revision is to be
# an otherwise-unremarkable version bump.
#
pkg_name="zlib"
pkg_version="1.3.2-2"
pkg_source="https://zlib.net/zlib-1.3.2.tar.gz"
pkg_sha256="bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"
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
