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
# TCC DOES NOT DEFINE __GNUC__, and libarchive is where that stops being
# trivia. libarchive/archive_blake2.h selects its struct-packing
# directive three ways:
#
#   #if defined(_MSC_VER)    __pragma(pack(push, 1)) x __pragma(pack(pop))
#   #elif defined(__GNUC__)  x __attribute__((packed))
#   #else                    _Pragma("pack 1") x _Pragma("pack 0")
#
# and the -1 build took the third arm, failing with
# "archive_blake2.h:106: error: identifier expected" -- TCC's pragma is
# spelled pack(1)/pack(), not pack 1/pack 0. That is the good outcome.
# The tempting fix is to define __GNUC__ so the second arm is taken, and
# it would compile: TCC IGNORES __attribute__((packed)) ENTIRELY while
# honouring #pragma pack (ADR-0008, and the reason every kernel-uapi
# struct this project touches has a #pragma pack replacement). A silently
# unpacked blake2s_param is 36 bytes where BLAKE2 specifies 32, which is
# a wrong RAR5 content hash reported as a clean verification -- the
# squashfs endianness bug again, exactly.
#
# So the edit below rewrites only the spelling, keeping that arm's own
# intent, and a _Static_assert gate proves the packing actually took
# before a single object is compiled. The header is the only consumer of
# BLAKE2_PACKED and rar5 is the only consumer of the header.
#
pkg_name="libarchive"
pkg_version="3.8.1-2"
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
pkg_changelog="3.8.1-2: give archive_blake2.h the pragma spelling TCC implements, gated by a _Static_assert on both param struct sizes. The -1 build died at archive_blake2.h:106 because TCC defines neither _MSC_VER nor __GNUC__ and so took the header pack 1 arm, which is not TCC syntax. 3.8.1-1: libarchive, the second package cix-build-system needs to build on a Cix host (cix-build-system#124). Library only, zlib/lzma/zstd filters, every other optional dependency explicitly disabled."

pkg_build() {
	# Same directive, TCC's spelling. pack(1) sets 1-byte alignment and
	# pack() restores the default, which is what pack 1 / pack 0 mean in
	# the old-style syntax that arm was written for.
	sed -i \
	    -e 's/_Pragma("pack 1")/_Pragma("pack(1)")/' \
	    -e 's/_Pragma("pack 0")/_Pragma("pack()")/' \
	    libarchive/archive_blake2.h

	# Prove it took. BLAKE2 specifies these two parameter blocks as
	# exactly 32 and 64 bytes; unpacked they are 36 and 72, and nothing
	# downstream would ever report the difference -- a wrong hash verifies
	# as cleanly as a right one.
	cat > /run/blake2-pack-gate.c <<'GATE'
#include "libarchive/archive_blake2.h"
_Static_assert(sizeof(blake2s_param) == 32, "blake2s_param is not packed");
_Static_assert(sizeof(blake2b_param) == 64, "blake2b_param is not packed");
int main(void) { return 0; }
GATE
	tcc -I. -c /run/blake2-pack-gate.c -o /run/blake2-pack-gate.o

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
