#
# elfutils -- provides libelf, the ELF object file library. A
# genuinely missing dependency confirmed empirically (ADR-0056): a
# real kernel hostbuild against the "dev" toolchain image failed at
# `tools/objtool` (needed for ORC/stack-validation metadata) with
# "cannot find -lelf". Nothing else in this recipe catalog happened to
# need it before now.
#
# Compression support (zlib/bzlib/lzma) and debuginfod are all opt-in
# via --with-*/--enable-* flags (confirmed via ./configure --help) --
# a plain configure needs nothing beyond this project's existing
# libc-dev + gcc toolchain, no dependency cascade.
#
# Source is elfutils' own canonical sourceware.org release.
#
pkg_name="elfutils"
pkg_version="0.192-2"
pkg_source="https://sourceware.org/elfutils/ftp/0.192/elfutils-0.192.tar.bz2"
pkg_sha256="616099beae24aba11f9b63d86ca6cc8d566d968b802391334c91df54eab416b4"
pkg_depends=""

# 0.192's own bare `CC=tcc ./configure` failed outright at configure
# time: "checking for __thread support... no" -> "configure: error:
# __thread support required". Verified directly, not assumed: a
# minimal standalone probe (`__thread int x = 5; int main(){ return
# x; }`) compiled with this sandbox's own tcc fails to even parse --
# `error: ';' expected (got "int")` -- TCC's parser does not recognize
# `__thread` as a storage-class specifier at all. A genuine, hard
# language-conformance gap, not a link/runtime-only issue, and unlike
# every other TCC gap this recipe catalog has hit so far, no
# CFLAGS/LIBS-level workaround can paper over a parse failure.
#
# Fixed the same way kernel.recipe's own HOSTCC/CC already does for
# its own TCC-incompatible host tooling: force real, already-installed
# gcc for this package's build instead. elfutils' own configure output
# already showed it detecting and preferring g++ for parts of its
# build regardless, so this isn't fighting the package's own grain.
pkg_build() {
	CC=/usr/bin/gcc ./configure --prefix=/usr --disable-debuginfod --disable-libdebuginfod
	make -j"$(nproc)" CC=/usr/bin/gcc
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" CC=/usr/bin/gcc
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" \
	       "$PKG_DESTDIR/usr/lib"/*.a
}
