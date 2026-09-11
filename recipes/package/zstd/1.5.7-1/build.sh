#
# zstd -- the compression library CIXPKG is defined in terms of.
#
# Here because cix-build-system links -lzstd and neither it nor
# libarchive is packaged (cix-build-system#124), so CBS cannot be built
# on a Cix host at all. It is also load-bearing rather than incidental:
# CBS's package.c calls ZSTD_compress at level 19 and cixpkg.c reads
# frames back with ZSTD_getFrameContentSize/ZSTD_decompress, and CBS's
# own ADR-0012 fixes zstd as the format's compression rather than
# leaving it selectable. A package format's compressor is not a
# swappable dependency.
#
# The library only. zstd's CLI is a separate build under programs/ and
# nothing here needs it; `make -C lib` is the same scope CBS's own
# zstd.cbs declares.
#
# One TCC hazard is real for this package and is deliberately NOT
# pre-empted here. __builtin_bswap32/64 are still missing from the
# pinned compiler (#208) and are the dangerous kind -- an undefined
# symbol is legal in a shared object, so a broken .so links and installs
# and fails only when something calls it. zstd guards both behind
# `__GNUC__ >= 4.3 || (__clang__ && __has_builtin(...))` in
# lib/common/mem.h, with MEM_swap32_fallback/MEM_swap64_fallback as the
# other arm, so whether TCC reaches the builtin depends entirely on
# whether it advertises __GNUC__ -- which is a measurement, not
# something to guess at from a recipe header. The build answers it, and
# daemon/src/elfcheck.c's install-time gate is the backstop either way:
# it refused libnl for exactly these symbols.
#
pkg_name="zstd"
pkg_version="1.5.7-1"
pkg_source="https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz"
pkg_sha256="eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3"
pkg_build_image="cix-builder"
# A composed build environment contains precisely what is declared here.
# This is libsodium's list minus the autotools-only entries -- zstd has
# no configure script, so sed/gawk/diffutils are not reached; binutils
# stays because the static archive step needs ar.
pkg_build_depends="tcc make linux-headers bash coreutils grep binutils findutils"
pkg_depends=""
pkg_changelog="1.5.7-1: zstd, so cix-build-system can be built on a Cix host (cix-build-system#124). Library only, built with TCC."

pkg_build() {
	# CC=tcc explicitly, never a bare `cc`: the build sandbox's default
	# compiler has silently become real GCC before (#109).
	make -C lib CC=tcc -j"$(nproc)"
}

pkg_install() {
	# Refuse a static-only outcome rather than shipping a package whose
	# only consumer then fails to link one build cycle later and
	# somewhere else -- the same refusal zlib and libsodium make.
	if [ ! -f lib/libzstd.so ]; then
		echo "zstd: no shared library was built" >&2
		exit 1
	fi
	make -C lib install CC=tcc PREFIX=/usr LIBDIR=/usr/lib DESTDIR="$PKG_DESTDIR"
	rm -f "$PKG_DESTDIR/usr/lib"/*.a
}
