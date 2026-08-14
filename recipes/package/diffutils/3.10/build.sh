#
# diffutils -- GNU diff/cmp/diff3/sdiff. A genuinely missing base tool
# confirmed empirically (ADR-0056): a real kernel hostbuild against
# the "dev" toolchain image printed repeated non-fatal
# "diff: command not found" warnings from scripts/sync-check.sh (its
# tools/ vs kernel-source ABI-header sync check). Not part of
# coreutils (a separate GNU project), and nothing else in this recipe
# catalog happened to need it before now.
#
# Source is GNU's own canonical ftp.gnu.org release.
#
pkg_name="diffutils"
pkg_version="3.10"
pkg_source="https://ftp.gnu.org/gnu/diffutils/diffutils-3.10.tar.xz"
pkg_sha256="90e5e93cc724e4ebe12ede80df1634063c7a855692685919bfe60b556c9bd09e"
pkg_depends=""

pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" \
	       "$PKG_DESTDIR/usr/share/locale"
}
