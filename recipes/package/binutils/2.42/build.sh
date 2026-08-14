#
# binutils -- the GNU binary utilities: as (assembler), ld/ld.bfd
# (linker), ar/ranlib (static archives), objcopy/objdump/readelf/nm/
# size/strings/strip/addr2line/c++filt/elfedit/gprof (object-file
# inspection and manipulation). The second dev-toolchain recipe (after
# m4) -- gcc.recipe needs a real as/ld present in the target image to
# actually produce working binaries; without this, a staged gcc could
# compile but not link anything.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against two independent mirrors (ftp.gnu.org and mirrors.kernel.org)
# -- byte-identical, same sha256.
#
pkg_name="binutils"
pkg_version="2.42"
pkg_source="https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz"
pkg_sha256="f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800"
pkg_depends=""

# Out-of-tree build (binutils' own documented convention, avoids a real,
# known issue building certain subdirs in-tree). --disable-gold (the
# alternative ELF-only linker; ld.bfd already covers every real use
# case this project has, and gold needs a C++ toolchain this recipe set
# doesn't assume yet) and --disable-gprofng (a newer profiler needing
# extra deps this project has no use for) keep the build to the tools
# actually needed. MAKEINFO=true skips texinfo doc generation --
# `makeinfo` isn't part of this toolchain and none of this project's
# own images ship info pages for anything else either (same reasoning
# iputils.recipe's own -DBUILD_MANS=false already used).
pkg_build() {
	mkdir -p build
	cd build
	CC=tcc ../configure --prefix=/usr --disable-multilib --disable-gold \
		--disable-gprofng --enable-deterministic-archives
	make -j"$(nproc)" MAKEINFO=true
}

# Confirmed via ldd against every one of the 16 real tools this build
# produces: every single one links against nothing but libc -- binutils
# statically links its own libbfd/libopcodes/libctf/libsframe into each
# tool, so no extra runtime libraries need staging here (unlike most of
# this project's other recipes). Static archives (.a/.la), headers, and
# docs (info/man/locale) are dropped -- nothing else in this project's
# image set links against libbfd directly. The triplet-prefixed
# duplicate copies under usr/x86_64-pc-linux-gnu/{bin,lib} (binutils'
# own cross-compilation convention) are skipped too -- this project
# never cross-compiles, so only the plain usr/bin/* names are staged.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/x86_64-pc-linux-gnu" "$PKG_DESTDIR/usr/share" \
	       "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/lib"/*.a "$PKG_DESTDIR/usr/lib"/*.la
}
