#
# e2fsprogs -- mkfs.ext4 (DISKFORMAT_MKFS_EXT4_BIN, daemon/src/
# diskformat.c, multi-disk management Phase C). Scoped to exactly this
# real, confirmed need -- mke2fs and its mkfs.ext2/mkfs.ext3/mkfs.ext4
# hardlinks (upstream's own convention, confirmed via a real local
# build+install: all four are the identical binary, same inode).
# e2fsprogs' own full suite (e2fsck, debugfs, tune2fs, resize2fs,
# dumpe2fs, badblocks, ~25 more tools) is real and builds cleanly
# alongside it, but nothing in this project calls any of them today --
# not staged here, matching this recipe set's own "don't build unused
# tools speculatively" precedent (curl.recipe/squashfs-tools.recipe).
#
# Source is e2fsprogs' own canonical kernel.org release (the real,
# actual upstream release host, not just a mirror), checksum verified
# against a second independent source: GitHub's own tag archive for
# the identical version -- content-identical (verified via a real
# `diff -r` of both extracted trees; GitHub's own auto-generated
# archive isn't byte-reproducible against the maintainer's own release
# tarball even for identical source, so a raw sha256 comparison would
# give a false mismatch).
#
pkg_name="e2fsprogs"
pkg_version="1.47.4"
pkg_source="https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.4/e2fsprogs-1.47.4.tar.gz"
pkg_sha256="da274408bebbfd13a5a2fc3cfc66e3ffff17c48534673aa67f88d49b99123b96"
pkg_depends=""

# Plain autotools (a pre-generated ./configure ships in the release
# tarball, confirmed -- no autoconf/automake needed to build it).
# --disable-nls skips gettext/locale infrastructure (matching this
# recipe set's own doc/locale-stripping convention elsewhere).
# Confirmed via a real local build: this default configuration links
# mke2fs against the system libuuid (util-linux's, already staged by
# mkbootroot.c's own existing shelled_bin_libs list) and nothing else
# beyond libc -- no libblkid/libext2fs/libe2p .so at all, since this
# default build statically links those into mke2fs itself.
pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-nls
	make -j"$(nproc)"
}

# Real files from this recipe's own build -- just mke2fs (mkfs.ext4 is
# the identical binary, upstream's own hardlink, confirmed via a real
# local install: all four mkfs.ext{2,3,4}/mke2fs share one inode).
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/sbin"
	cp misc/mke2fs "$PKG_DESTDIR/usr/sbin/mkfs.ext4"
}
