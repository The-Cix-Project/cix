#
# squashfs-tools -- unsquashfs, this project's real, only need from
# this project (PKG_UNSQUASHFS_BIN, daemon/src/pkg.c's
# pkg_bootstrap_from_toolchain() import path). mksquashfs/sqfstar/
# sqfscat come along for free from the same build (one shared object
# file set) and are staged too, though nothing in this project's own
# code currently shells out to them -- kanxeod does its own squashfs
# assembly via image/src/mkbootroot.c/mktoolchainimage.c's own
# execve("/usr/bin/mksquashfs", ...) calls, which is a real, separate
# staged binary already, not this recipe's concern.
#
# Deliberately built with ONLY XZ_SUPPORT=1 -- a direct grep of every
# "-comp" flag this project's own code ever passes to mksquashfs
# (image/src/mkbootroot.c, image/src/mktoolchainimage.c) confirms
# "xz" is the only compression format this project ever produces or
# needs to read back. The upstream Makefile defaults ALL of gzip/lzo/
# lz4/zstd/xattr support to on, which would need three more from-
# source recipes (liblzo2, liblz4, libzstd) for compression backends
# this project never uses -- matching curl.recipe's own "don't build
# unused backends" reasoning. COMP_DEFAULT=xz keeps the build from
# refusing to compile with gzip (the Makefile's own hardcoded default)
# disabled.
#
# Source is squashfs-tools' own canonical GitHub repo (its only real
# distribution point -- no separate release-tarball site, confirmed),
# checksum verified against a second independent source (Debian's own
# orig tarball) -- byte-identical content (verified via `diff -r` on
# both extracted trees; GitHub's own auto-generated tag archive isn't
# byte-reproducible against Debian's repackaging, so a raw sha256
# comparison would give a false mismatch even for identical content).
#
pkg_name="squashfs-tools"
pkg_version="4.7.5"
pkg_source="https://github.com/plougher/squashfs-tools/archive/refs/tags/4.7.5.tar.gz"
pkg_sha256="547b7b7f4d2e44bf91b6fc554664850c69563701deab9fd9cd7e21f694c88ea6"
pkg_depends="xz"

# Plain hand-written Makefile (no autotools), confirmed via a real
# local build. Real, empirically confirmed via `readelf -d` on the
# resulting binaries: unsquashfs/mksquashfs need only libm/liblzma/
# libc -- no gzip/lzo/lz4/zstd/xattr library dependency at all with
# these flags.
pkg_build() {
	cd squashfs-tools
	make -j"$(nproc)" GZIP_SUPPORT=0 XZ_SUPPORT=1 LZO_SUPPORT=0 \
	                  LZ4_SUPPORT=0 ZSTD_SUPPORT=0 XATTR_SUPPORT=0 \
	                  COMP_DEFAULT=xz
}

# No DESTDIR-aware install target in this Makefile (confirmed) --
# files copied directly. sqfscat/sqfstar are real symlinks to
# unsquashfs/mksquashfs respectively (upstream's own convention,
# confirmed via `ls -la` on the build output), kept as-is.
#
# No "squashfs-tools/" prefix here (confirmed live: an earlier version
# of this line had one and every real build failed at this exact step,
# "cannot stat 'squashfs-tools/mksquashfs'") -- pkg_build() and
# pkg_install() run as two calls in the *same* shell session (pkg.c's
# own build invocation: ". recipe.sh; cd /build/src && pkg_build &&
# pkg_install"), so pkg_build()'s own "cd squashfs-tools" is still in
# effect here; this function is already inside that directory, not
# back at /build/src.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp -a mksquashfs unsquashfs sqfscat sqfstar "$PKG_DESTDIR/usr/bin/"
}
