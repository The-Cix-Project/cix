#
# binutils-dev -- binutils' own development files: bfd.h and friends,
# plus the static libbfd.a/libopcodes.a/libiberty.a/libctf.a/
# libsframe.a archives -- deliberately dropped by binutils.recipe's own
# pkg_install() (see its comment: "nothing else in this project's
# image set links against libbfd directly"), the same runtime/dev
# split libc.recipe/libc-dev.recipe already established. Needed now:
# sbsigntools (Part 5, bare-metal-readiness plan) genuinely does link
# against libbfd -- its own configure.ac hard-fails (AC_MSG_ERROR) if
# bfd.h isn't found, confirmed directly, not assumed.
#
# Real, from-source build -- not a copy from this build container's
# own toolchain the way libc-dev.recipe works, since (unlike glibc)
# binutils genuinely is built from source in this project already
# (binutils.recipe). Same source tarball/version/configure flags as
# binutils.recipe, kept in sync deliberately -- see that recipe if
# this one's own build step ever needs updating to match.
#
pkg_name="binutils-dev"
pkg_version="2.42"
pkg_source="https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz"
pkg_sha256="f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800"
pkg_depends=""

pkg_build() {
	mkdir -p build
	cd build
	../configure --prefix=/usr --disable-multilib --disable-gold \
		--disable-gprofng --enable-deterministic-archives
	make -j"$(nproc)" MAKEINFO=true
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/lib"
	# pkg_build() ended with `cd build` (binutils' own documented out-of-
	# tree convention) -- this function inherits that cwd, confirmed the
	# hard way: an earlier version of this recipe assumed pkg_install()
	# started back at the source root and referenced build/include (a
	# nonexistent double-nested path from here) and include (which
	# doesn't exist directly under build/ either). The real source-tree
	# static headers (ansidecl.h, libiberty.h, bfdlink.h, ...) live one
	# level up; bfd.h/bfdver.h themselves are generated inside this
	# build dir's own bfd/ subdirectory, not shipped as static source.
	cp -a ../include/. "$PKG_DESTDIR/usr/include/"
	find . -name 'bfd.h' -o -name 'bfdver.h' | while read -r f; do
		cp -a "$f" "$PKG_DESTDIR/usr/include/"
	done
	find . -name '*.a' -exec cp -a {} "$PKG_DESTDIR/usr/lib/" \;
}
