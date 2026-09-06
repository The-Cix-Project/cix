#
# libblkid -- block device identification library, part of the same
# util-linux source tree libuuid.recipe already builds standalone from.
# Real, confirmed build-time dependency of btrfs-progs.recipe
# (BTRFS_CFLAGS/BTRFS_LIBS -- its own configure.ac does
# PKG_CHECK_MODULES(BLKID, [blkid]) unconditionally, no --disable
# flag exists to skip it: mkfs.btrfs itself uses libblkid to detect
# an existing filesystem signature on the target device before
# formatting over it).
#
# Reuses libuuid.recipe's own already-staged libuuid (pkg_depends
# below) via pkg-config rather than rebuilding it a second time from
# the same source tree -- confirmed via a real local build: util-linux's
# own configure with only --enable-libblkid (no --enable-libuuid)
# resolves uuid.h/libuuid.so entirely through the already-installed
# libuuid.recipe image content (its usr/include/uuid.h + usr/lib/
# pkgconfig/uuid.pc), producing just libblkid.so + blkid.pc -- no
# redundant libuuid.so rebuilt or shipped a second time.
#
pkg_name="libblkid"
pkg_version="2.42.2-5"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
pkg_artifact_sha256="eca239b9eb3a96ae0d23b4bb2cc761ae034f6050c7b2e090aefd36e387e710cb"
pkg_depends="libuuid pkgconf"
# ADR-0199/0209: composed from exactly these, no fallback (#168). 2.42.2
# declared none and so could not be built, which blocked btrfs-progs --
# and therefore mkfs.btrfs on every installed host, and therefore
# ADR-0207 phase 4.
#
# Tier 3, and for a defect rather than a preference: -2 built with TCC,
# installed cleanly, and shipped a BROKEN library. TCC does not
# implement __builtin_clz, and instead of failing it emits the builtin
# as an ordinary undefined external symbol:
#
#   nm -D libblkid.so.1.1.0 | grep builtin
#     U __builtin_clz
#
# Nothing catches that at build time. The library loads, and any call
# path reaching that code fails on an unresolved symbol at run time --
# or, as here, takes down the first thing that tries to link against it:
#
#   ld: libblkid.so: undefined reference to `__builtin_clz'
#   ld: libblkid.so: .dynsym local symbol at index 0 (>= sh_info of 0)
#
# kmod.recipe already records that TCC lacks this builtin -- kmod's own
# configure probes for it and aborts, which is the loud version of the
# same gap. util-linux never probes, so it got the silent one. Swept the
# other TCC-built artifacts in this chain (libuuid, zlib, keyutils,
# getopt) for undefined __builtin_* symbols: all clean, so this is
# specific to code that uses it, not general breakage.
#
# The same set getopt 2.42.2-2 proved against this identical util-linux
# tarball, plus libuuid: libblkid links it, and a composed environment
# is built from pkg_build_depends ALONE, so it has to be present as
# headers and a .pc to compile against rather than merely installed
# alongside the result.
pkg_toolchain="gcc"
pkg_toolchain_reason="codegen/correctness: TCC emits __builtin_clz as an undefined external instead of failing, so the .so built, installed, verified and was BROKEN (#176, #208)"
pkg_build_depends="bash coreutils make gcc linux-headers sed grep gawk binutils pkgconf findutils libuuid"
pkg_changelog="2.42.2-5: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"

pkg_build() {
	CC=/usr/bin/gcc ./configure --prefix=/usr --disable-all-programs --enable-libblkid
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/lib"/*.la
}
