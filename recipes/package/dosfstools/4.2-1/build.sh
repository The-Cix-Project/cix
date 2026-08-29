#
# dosfstools -- mkfs.fat, the FAT32 formatter for the EFI System
# Partition.
#
# Every machine this project installs boots UEFI, and a UEFI ESP is
# FAT32 by definition, so cix-install.c formats it with mkfs.vfat on
# every single install. image/src/mkinstalleriso.c staged that binary
# by reading /usr/sbin/mkfs.fat off whatever machine ran the ISO build
# -- a path that exists on a Debian development box and on no Cix
# control-plane root (confirmed against mkbootroot.c's own staged-binary
# list, which is authoritative: the root is built from it, and
# dosfstools is not in it and has no reason to be, since cixd itself
# never formats FAT).
#
# Upstream needs no libraries beyond libc: it does its own FAT
# structure writing and takes character-set conversion from glibc's
# built-in iconv. That matters here because the installer stages this
# binary into a minimal initramfs, where every extra .so is another
# file mkinstalleriso has to know about and keep in step -- asserted
# below rather than assumed, the same check fdisk 2.42.2-2 makes.
#
pkg_name="dosfstools"
pkg_version="4.2-1"
pkg_source="https://github.com/dosfstools/dosfstools/releases/download/v4.2/dosfstools-4.2.tar.gz"
pkg_sha256="64926eebf90092dca21b14259a5301b7b98e7b1943e8a201c7d726084809b527"
pkg_depends=""

# ADR-0199/0209: composed from exactly these, no fallback (#168).
pkg_build_depends="bash coreutils make tcc libc-dev sed grep gawk binutils pkgconf findutils diffutils"

pkg_build() {
	# --enable-compat-symlinks is deliberately NOT passed: it installs
	# mkdosfs/mkfs.msdos/mkfs.vfat aliases this project has no use for.
	# mkinstalleriso reads the canonical mkfs.fat and stages it under
	# the name cix-install actually calls (mkfs.vfat), so the aliasing
	# already happens once, in the one place that decides it.
	CC=tcc ./configure --prefix=/usr --sbindir=/usr/sbin
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	test -x "$PKG_DESTDIR/usr/sbin/mkfs.fat" || {
		echo "dosfstools: mkfs.fat missing from install output" >&2
		exit 1
	}

	# Documentation is not an installer input; the ISO stages binaries
	# into a size-constrained initramfs.
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"

	needed=$(readelf -d "$PKG_DESTDIR/usr/sbin/mkfs.fat" | grep NEEDED)
	echo "mkfs.fat DT_NEEDED:"
	echo "$needed"
	# libc only. Anything else means a closure mkinstalleriso does not
	# stage, and a binary that cannot start inside the installer.
	case "$needed" in
	*libc.so.6*) ;;
	*)
		echo "dosfstools: mkfs.fat is not linked against libc as expected" >&2
		exit 1
		;;
	esac
}
