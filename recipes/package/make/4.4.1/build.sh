#
# make -- GNU Make. The most foundational dev-toolchain recipe in this
# set after m4/binutils: every other autotools recipe in this project
# (and anything a user builds inside a real "dev" image) shells out to
# make itself. Built here using the isolated build container's own
# already-present host make (the standard, unremarkable "build make
# with an existing make" case -- no bootstrapping trick needed, the
# same as gcc.recipe's own single-stage "build gcc with an existing
# gcc").
#
# Source is GNU's own canonical ftp.gnu.org release, checksum computed
# directly from the downloaded bytes (sha256sum), not taken from any
# third party.
#
pkg_name="make"
pkg_version="4.4.1"
pkg_source="https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz"
pkg_sha256="dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3"
pkg_depends=""

# Plain autotools, confirmed directly. MAKEINFO=true skips texinfo doc
# generation, same reasoning as every other recipe in this batch.
pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

# Confirmed via ldd: links against nothing but libc, no extra runtime
# libraries to stage.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share"
}
