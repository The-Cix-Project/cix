#
# sed -- GNU sed, the stream editor. A genuinely missing base tool
# confirmed empirically (ADR-0056): a real kernel hostbuild against the
# "dev" toolchain image failed outright the moment its own build
# scripts (GNU Make's own Makefile machinery, scripts/kconfig/
# merge_config.sh) shelled out to `sed`, which this project had never
# packaged at all -- not part of coreutils (a separate GNU project),
# and nothing else in this recipe catalog happened to need it before
# now.
#
# Source is GNU's own canonical ftp.gnu.org release.
#
pkg_name="sed"
pkg_version="4.9"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""

# Plain autotools, no non-libc runtime dependencies (confirmed via ldd
# against the built binary). MAKEINFO=true skips texinfo doc
# generation, same convention every other from-source recipe here uses.
pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" \
	       "$PKG_DESTDIR/usr/share/locale" "$PKG_DESTDIR/usr/lib"/*.a
}
