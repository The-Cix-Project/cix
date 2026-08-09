#
# btrfs-progs -- mkfs.btrfs (DISKFORMAT_MKFS_BTRFS_BIN, daemon/src/
# diskformat.c, ADR-0103/ADR-0104: real btrfs support for the disk-
# format REST endpoint, opt-in alongside the existing ext4 default).
# Scoped to exactly this real, confirmed need -- btrfs-progs' own full
# suite (btrfs, btrfsck/btrfs check, btrfs-image, btrfstune, ~15 more
# tools) builds cleanly alongside it, but nothing in this project calls
# any of them today -- not staged here, matching e2fsprogs.recipe's own
# "don't build unused tools speculatively" precedent.
#
# Real, confirmed build-time dependencies (a real local build in this
# sandbox, `./configure` summary output + a real `ldd` on the resulting
# mkfs.btrfs): blkid, uuid, zlib via pkg-config (PKG_CHECK_MODULES in
# configure.ac, unconditional -- no --disable flag exists to skip any
# of the three). --disable-zstd/--disable-lzo avoid needing zstd/lzo
# (neither has a recipe in this catalog, and neither is needed for the
# one thing this project uses mkfs.btrfs for: formatting a container-
# storage disk, not restore/receive compression). --disable-convert
# avoids needing e2fsprogs' libext2fs (btrfs-convert, an ext2/3/4-to-
# btrfs in-place migration tool, not a capability this project offers
# for any filesystem). --disable-documentation/--disable-python avoid
# needing python3/sphinx. --disable-libudev avoids needing libudev
# (multipath device dedup, irrelevant to this project's own disk
# model, ADR-0099). --with-crypto=builtin avoids needing a crypto
# library (openssl/libgcrypt/etc) for btrfs's own checksum tree --
# confirmed via configure's own summary output: "crypto provider:
# builtin". The resulting mkfs.btrfs's own real NEEDED list (confirmed
# via ldd): libuuid.so.1, libblkid.so.1, libz.so.1, libc.so.6 -- the
# same three non-libc libraries e2fsprogs' own mke2fs already needs,
# already staged into every assembled boot image by mkbootroot.c's
# existing shelled_bin_libs[] list (added for mke2fs's own sake, Phase
# C) -- no new runtime library staging needed for this binary at all.
#
# Source is btrfs-progs' own canonical kernel.org release (the real
# maintainer-hosted mirror, same convention as e2fsprogs.recipe/
# kernel.recipe), cross-checked against the project's own GitHub
# releases page for the same tag (v7.1, confirmed the current latest
# via the GitHub releases API directly, not assumed).
#
pkg_name="btrfs-progs"
pkg_version="7.1"
pkg_source="https://www.kernel.org/pub/linux/kernel/people/kdave/btrfs-progs/btrfs-progs-v7.1.tar.xz"
pkg_sha256="d1f55cc2971398c9142eaa79d203e63d586a3b4b867f956664a1d68322cd4e34"
pkg_depends="zlib libuuid libblkid pkgconf"

pkg_build() {
	./configure --prefix=/usr --disable-documentation --disable-python \
	            --disable-convert --disable-libudev --disable-zoned \
	            --with-crypto=builtin --disable-zstd --disable-lzo
	make -j"$(nproc)" mkfs.btrfs
}

# Real file from this recipe's own build -- just mkfs.btrfs, matching
# e2fsprogs.recipe's own single-binary economy.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/sbin"
	cp mkfs.btrfs "$PKG_DESTDIR/usr/sbin/mkfs.btrfs"
}
