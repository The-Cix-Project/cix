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
pkg_version="2.42.2"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
pkg_depends="libuuid pkgconf"

pkg_build() {
	./configure --prefix=/usr --disable-all-programs --enable-libblkid
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/lib"/*.la
}
