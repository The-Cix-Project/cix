#
# kmod -- modprobe/depmod/insmod/rmmod/lsmod/modinfo, real software (this
# project's own established "reuse real software as workload, never
# hand-roll dependency resolution" posture, e.g. bird/keepalived/coreutils)
# rather than reimplementing modprobe's own dependency-graph resolution in
# raw C. Part 3 of the bare-metal-readiness plan (kernel module loading) --
# cixd's own boot_init() shells out to this real, freshly-staged
# modprobe for a curated boot-critical module list, the exact same
# subprocess pattern every other host tool this project already invokes
# (curl, sfdisk, openssl) uses, no new mechanism needed.
#
# Source is kernel.org's own canonical release tarball. All four optional
# compressed-module/PKCS7-signature backends (zstd/xz/zlib/openssl) are
# left at their real upstream default (disabled) -- this project's own
# kernel config (image/kernel/qemu-part1.config) never enables
# CONFIG_MODULE_COMPRESS, so plain .ko files are all modprobe ever needs
# to find here; adding those libraries as build dependencies for a
# capability nothing in this project uses would be unjustified scope.
# --disable-manpages needs no separate docs toolchain (scdoc) for man
# pages this project's images never ship anyway -- the same
# BUILD_MANS=false precedent iputils.recipe already established.
#
pkg_name="kmod"
pkg_version="34.2"
pkg_source="https://www.kernel.org/pub/linux/utils/kernel/kmod/kmod-34.2.tar.xz"
pkg_sha256="5a5d5073070cc7e0c7a7a3c6ec2a0e1780850c8b47b3e3892226b93ffcb9cb54"
pkg_depends=""

# Plain autotools, confirmed directly (a local build against this exact
# tarball). --sbindir=/usr/bin: this project's own images have no /sbin
# or /usr/sbin at all (usr/bin-only FHS convention, see pkg.h) -- kmod's
# own install-exec-hook symlinks depmod/insmod/lsmod/modprobe/modinfo/
# rmmod into $(sbindir) by default, which would otherwise land somewhere
# nothing on this project's own PATH (/usr/bin:/bin) ever looks.
pkg_build() {
	CC=tcc ./configure --prefix=/usr --sbindir=/usr/bin --disable-manpages
	make -j"$(nproc)"
}

# Confirmed via ldd against a real build: the installed tools/kmod
# multi-call binary links against nothing but libc (its own libkmod is
# pulled in statically at link time, not as a runtime .so dependency) --
# no extra runtime libraries to stage. Bash/fish/zsh completions and
# libkmod's own pkg-config/header/library files are stripped -- none of
# this project's own tooling (cixctl, the daemon) ever links against
# libkmod or uses shell completions.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/bash-completion" "$PKG_DESTDIR/usr/share/fish" \
	       "$PKG_DESTDIR/usr/share/zsh" "$PKG_DESTDIR/usr/share/pkgconfig" \
	       "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/lib"
}
