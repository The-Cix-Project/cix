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
pkg_version="2.42-4"
pkg_source="https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz"
pkg_sha256="f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 2.42.
# ./configure && make out of tree; findutils because its Makefiles use find during install.
# Tier-3 TCC exception (ADR-0211), built with the Cix-built gcc
# 16.2.0-11 from our own artifact cache. gnu-efi needs MS-ABI variadics
# (__builtin_ms_va_list) for the EFI calling convention, which TCC does
# not implement -- reduced to a one-line probe. binutils-dev follows it
# as sbsigntools' other dependency; its own TCC parse failure in bfd.c
# is NOT root-caused, so if it turns out to build under TCC once its
# declaration is right, it should come back off the list.
pkg_build_depends="bash coreutils make gcc linux-headers sed grep gawk binutils findutils"
pkg_changelog="2.42-4: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

pkg_build() {
	mkdir -p build
	cd build
	CC=/usr/bin/gcc ../configure --prefix=/usr --disable-multilib --disable-gold \
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
