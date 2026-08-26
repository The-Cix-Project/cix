#
# gzip -- GNU gzip, the DEFLATE compressor CLI. A genuinely missing
# base tool confirmed empirically (ADR-0056): a real kernel hostbuild
# against the "dev" toolchain image failed at its very last step --
# compressing the linked vmlinux into arch/x86/boot/compressed/
# vmlinux.bin.gz on the way to a real bzImage -- with
# "gzip: command not found". Not part of coreutils (a separate GNU
# project), and nothing else in this recipe catalog happened to need
# it before now.
#
# Source is GNU's own canonical ftp.gnu.org release.
#
pkg_name="gzip"
pkg_version="1.13"
pkg_source="https://ftp.gnu.org/gnu/gzip/gzip-1.13.tar.xz"
pkg_sha256="7454eb6935db17c6655576c2e1b0fabefd38b4d0936e0f87f48cd062ce91a057"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/gzip-1.13.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="0e6ab1f0be10126a8a6228262f4c336e8919499ff90cc7fe683b4891b17c76b4"
pkg_depends=""

pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info"
}
