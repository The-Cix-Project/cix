#
# libarchive -- source-archive reading for cix-build-system.
#
# The second of the two packages cix-build-system#124 names. CBS's
# archive.c is the only consumer and its use is narrow: read a tar, pax
# or zip member stream, reject anything that can escape its extraction
# root, and write out regular files and directories. Fifteen symbols,
# all archive_read_*/archive_entry_*, no writer API at all.
#
# Narrow use does not make the filters optional, though, and that is the
# one real decision in this recipe. CBS calls
# archive_read_support_filter_all(), so which compressed source tarballs
# it can open is decided here rather than in CPDL. Enabled: zlib
# (.tar.gz), lzma (.tar.xz) and zstd (.tar.zst) -- between them every
# source form this platform's recipe corpus actually fetches. Each is
# already packaged and present in cix-builder, measured before writing
# this:
#
#   zlib 1.3.2-10  usr/include/zlib.h, usr/lib/libz.so
#   xz   5.8.3-8   usr/include/lzma.h, usr/lib/liblzma.so
#   zstd 1.5.7-3   usr/include/zstd.h, usr/lib/libzstd.so
#
# bz2lib is disabled because bzip2 is NOT installed in cix-builder
# (GET /v1/pkg/bzip2@cix-builder -> "no such package"); enabling a
# filter whose library is absent would either fail configure or, worse,
# silently produce a libarchive that cannot open a .tar.bz2 while
# claiming filter_all. If a recipe ever needs one, bzip2 is an ordinary
# install away and this becomes a -2.
#
# Everything else is off deliberately rather than by omission. ADR-0276
# says declare every linked library, and the inverse is the same
# discipline: libxml2, expat, iconv, lz4, openssl, acl, attr, libb2,
# nettle and pcre2 are all optional libarchive dependencies that
# configure will happily pick up from whatever happens to be in the
# sandbox. A build environment is composed from pkg_build_depends, so
# today none of them are there -- but "absent today" is not a decision,
# and an undeclared optional dependency appearing later would change
# this package's linkage with nothing in the recipe having changed.
#
# The command-line tools are off too (--disable-bsdtar and friends):
# CBS links the library and this platform already has tar.
#
pkg_name="libarchive"
pkg_version="3.8.1-1"
pkg_source="https://github.com/libarchive/libarchive/releases/download/v3.8.1/libarchive-3.8.1.tar.gz"
pkg_sha256="bde832a5e3344dc723cfe9cc37f8e54bde04565bfe6f136bc1bd31ab352e9fab"
pkg_build_image="cix-builder"
# A composed build environment contains precisely what is declared here.
# This is the autotools list (libsodium's, which is chrony's) plus the
# three filter libraries. zstd 1.5.7-3's own build is the reminder that
# "this build has no configure script so sed is not reached" is a guess
# and not a measurement -- here there IS a configure script, so the full
# shell-tool set is not even a judgement call.
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils zlib xz zstd"
pkg_depends="zlib xz zstd"
pkg_changelog="3.8.1-1: libarchive, the second package cix-build-system needs to build on a Cix host (cix-build-system#124). Library only, zlib/lzma/zstd filters, every other optional dependency explicitly disabled."

pkg_build() {
	# CC=tcc explicitly, never a bare cc: the build sandbox's default
	# compiler has silently become real GCC before (#109).
	CC=tcc ./configure \
	    --prefix=/usr \
	    --libdir=/usr/lib \
	    --disable-static \
	    --disable-bsdtar \
	    --disable-bsdcpio \
	    --disable-bsdcat \
	    --disable-bsdunzip \
	    --with-zlib \
	    --with-lzma \
	    --with-zstd \
	    --without-bz2lib \
	    --without-lz4 \
	    --without-xml2 \
	    --without-expat \
	    --without-iconv \
	    --without-openssl \
	    --without-nettle \
	    --without-libb2 \
	    --without-pcre2posix
	make -j"$(nproc)"
}

pkg_install() {
	# Refuse a static-only outcome rather than shipping a package whose
	# only consumer then fails to link one build cycle later and
	# somewhere else -- the same refusal zlib, libsodium and zstd make.
	if [ ! -f .libs/libarchive.so ]; then
		echo "libarchive: no shared library was built" >&2
		exit 1
	fi
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/lib"/*.a
}
