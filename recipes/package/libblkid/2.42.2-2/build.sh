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
pkg_version="2.42.2-2"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
pkg_depends="libuuid pkgconf"
# ADR-0199/0209: composed from exactly these, no fallback (#168). 2.42.2
# declared none and so could not be built, which blocked btrfs-progs --
# and therefore mkfs.btrfs on every installed host, and therefore
# ADR-0207 phase 4.
#
# The same set getopt 2.42.2-2 proved against this identical util-linux
# tarball, plus libuuid: libblkid links it, and a composed environment
# is built from pkg_build_depends ALONE, so it has to be present as
# headers and a .pc to compile against rather than merely installed
# alongside the result.
pkg_build_depends="bash coreutils make tcc libc-dev sed grep gawk binutils pkgconf findutils libuuid"

pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-all-programs --enable-libblkid
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/lib"/*.la
}
