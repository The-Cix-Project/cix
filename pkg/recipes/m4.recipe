#
# m4 -- GNU M4, the macro processor autoconf/automake/libtool/bison all
# invoke internally (autoconf itself is essentially a large collection
# of m4 macros run through this exact program). The first, most
# foundational recipe in this project's own dev-toolchain set -- every
# other autotools-based recipe below (autoconf, automake, libtool,
# bison) needs a real m4 present in the target image's own PATH to be
# usable there, the same way this project's own build host needs one to
# build any of them.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum computed
# directly from the downloaded bytes (sha256sum), not taken from any
# third party.
#
pkg_name="m4"
pkg_version="1.4.19"
pkg_source="https://ftp.gnu.org/gnu/m4/m4-1.4.19.tar.xz"
pkg_sha256="63aede5c6d33b6d9b13511cd0be2cac046f2e70fd0a07aa9573a04a82783af96"
pkg_depends=""

# Plain autotools, confirmed directly -- ./configure && make with no
# unusual flags needed. --prefix=/usr matches every other recipe in
# this project.
pkg_build() {
	./configure --prefix=/usr
	make -j"$(nproc)"
}

# Confirmed via ldd against a real build: m4 links against nothing but
# libc -- no extra runtime libraries to stage, unlike most of this
# project's other recipes.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
}
