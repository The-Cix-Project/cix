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
pkg_version="34.2-5"
pkg_source="https://www.kernel.org/pub/linux/utils/kernel/kmod/kmod-34.2.tar.xz"
pkg_sha256="5a5d5073070cc7e0c7a7a3c6ec2a0e1780850c8b47b3e3892226b93ffcb9cb54"
pkg_depends=""
# ADR-0199/0209: the build environment is composed from exactly these
# and nothing else -- there is no fallback to inherit a missing tool
# from (#168). -2 declared none, so it could not be built at all, and
# failed naming itself. That is why kernel-builder could not be
# composed even though kmod-34.2-2 sits in the artifact cache: a recipe
# with no pkg_artifact_sha256 never consults the cache tier, so an
# already-published artifact does not save a recipe that cannot build.
#
#   bash coreutils   the recipe functions, nproc, rm
#   make gcc binutils libc-dev
#                    the build proper; CC=/usr/bin/gcc is forced above
#                    for the __builtin_clz() gap, and gcc shells out to
#                    as/ld
#   sed grep gawk findutils diffutils
#                    what an autoconf configure reaches for
#   pkgconf zlib xz  kmod detects module-compression support through
#                    pkg-config and silently builds WITHOUT it when the
#                    libraries are absent. Declared deliberately rather
#                    than omitted as "not strictly required": a kmod
#                    that cannot read a compressed module is a real
#                    functional gap in an image whose entire job is
#                    building kernels, and under-declaring here removes
#                    a feature quietly instead of failing loudly.
pkg_toolchain="gcc"
pkg_toolchain_reason="missing language feature: configure requires __builtin_clz and aborts without it (#208)"
pkg_build_depends="bash coreutils make gcc binutils linux-headers sed grep gawk findutils diffutils pkgconf zlib xz"
pkg_changelog="34.2-5: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"

# Plain autotools, confirmed directly (a local build against this exact
# tarball). --sbindir=/usr/bin: this project's own images have no /sbin
# or /usr/sbin at all (usr/bin-only FHS convention, see pkg.h) -- kmod's
# own install-exec-hook symlinks depmod/insmod/lsmod/modprobe/modinfo/
# rmmod into $(sbindir) by default, which would otherwise land somewhere
# nothing on this project's own PATH (/usr/bin:/bin) ever looks.
pkg_build() {
	# -2: built with gcc, not TCC. kmod's own configure requires the
	# __builtin_clz() compiler builtin and aborts without it:
	#
	#   configure: error: *** builtin function not found: __builtin_clz()
	#
	# TCC does not provide it. That is a genuine capability gap, not a
	# flag or header problem, so this is a real Tier-3 exception rather
	# than a workaround worth engineering around.
	#
	# What makes the exception acceptable now, when it would not have
	# been before: /usr/bin/gcc in this project's own build images is no
	# longer an ambient Debian compiler. It is gcc 16.2.0, built by this
	# project's own chain -- TCC -> 4.7.4 (3-stage, byte-compared) ->
	# 9.5 -> 16.2 -- so "not TCC" no longer means "not ours". The
	# self-hosting property #36 exists to protect is preserved; only the
	# choice of which of our own compilers builds this package changes.
	#
	# Absolute path deliberately: a bare `gcc` makes GCC compute a
	# RELATIVE installation prefix and then fail to find cc1 with a
	# misleading posix_spawnp error (documented in CLAUDE.md).
	CC=/usr/bin/gcc ./configure --prefix=/usr --sbindir=/usr/bin --disable-manpages
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
pkg_artifact_sha256="f4a7deb61471ba981aa5f58765fab1d841b3b505d867febd2725299a4d0a0f1b"
