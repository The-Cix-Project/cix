#
# grep -- GNU grep, the pattern-matching filter. A genuinely missing
# base tool confirmed empirically (ADR-0056): a real kernel hostbuild
# against the "dev" toolchain image failed outright the moment its
# own build scripts (scripts/kconfig/merge_config.sh) shelled out to
# `grep`, which this project had never packaged at all -- like sed, a
# separate GNU project, not part of coreutils, and nothing else in
# this recipe catalog happened to need it before now.
#
# Source is GNU's own canonical ftp.gnu.org release.
#
pkg_name="grep"
pkg_version="3.11"
pkg_source="https://ftp.gnu.org/gnu/grep/grep-3.11.tar.gz"
pkg_sha256="1f31014953e71c3cddcedb97692ad7620cb9d6d04fbdc19e0d8dd836f87622bb"
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
