#
# 1.3.0: libc-dev -> glibc (#187, ADR-0217). Same job, different
# provider: libc-dev was Debian headers and CRT objects repackaged;
# glibc is the one this project builds from source (ADR-0216) and
# carries the same headers and the same crt1.o/crti.o/crtn.o. Pinned
# at 2.44-12, the revision every live builder on 192.168.15.95 already
# runs. Named explicitly because glibc is implicit only for a composed
# build environment (PKG_BASE_LIBC) and for the default image -- a
# named image gets exactly what its manifest names.
#
#
# 1.2.0: the manifest describes the image again. 1.1.0 named 29
# packages while the real image carried 39 -- ten had been installed by
# hand as each ISO gap was found (shim, mokutil and their efivar/
# keyutils/libxcrypt closure; libblkid/ncurses/pkgconf pulled in as
# build dependencies) and never written back. An image recipe that
# does not reproduce its own image is not one source of truth, and the
# gap only shows up when someone tries to rebuild the image on another
# box and finds isotools failing its preflight.
#
# One of them is genuinely new here: dosfstools 4.2-1, the ESP
# formatter. It joins for the same reason shim and mokutil did --
# mkinstalleriso used to read mkfs.fat from an absolute path that
# exists only on a Debian development box, so isotools now harvests it
# into its artifact and needs it installed here first. (An fdisk
# package briefly joined alongside it, for cix-install's interactive
# partitioning; ADR-0214 removed that path before this manifest was
# ever published.)
#
#
# iso-builder -- one job: hold the ISO toolchain so
# `pkg hostbuild isotools` can harvest it into the artifact cixd uses
# to assemble installer ISOs (ADR-0063/0064). ADR-0208.
#
# This replaces the image called `dev`. The rename is the point. "dev"
# named no job, so it got used for anything -- most damagingly by
# docs/guides/kernel-build-and-ab-updates.md, which told operators to
# build kernels with --build-image=dev while `dev`'s manifest held
# neither gcc nor kmod. A name that means "general" cannot be wrong,
# which is precisely why it was never noticed. Kernels now build in
# kernel-builder; this image builds ISOs.
#
# grub/sbsigntools/xorriso/mtools are IN the manifest rather than
# installed on top afterwards. The old guidance was four `pkg install`
# calls followed by a hostbuild, which left the image's identity
# implicit -- an "iso-builder" without the ISO tools is not one. Naming
# them here makes the image self-describing and makes its version hash
# actually cover the thing it exists to provide.
#
# All four build with CC=tcc (confirmed in their own recipes), so no
# gcc is needed or wanted here -- the toolchain entries below are the
# same TCC set cix-builder uses, plus what those four configure scripts
# reach for.
#
# There is no image artifact to publish: ADR-0209 retired the
# whole-rootfs tier entirely. This list is declared into the image's
# manifest and each package installs from its own verified artifact.
#
# 1.1.0: this manifest described an image that could not be built.
#
# Five of its twenty pins named versions absent from the artifact cache
# -- grub:2.14, sbsigntools:0.9.5, mtools:4.0.49, xorriso:1.5.8.pl02 and
# libc-dev:2.36-6 -- so applying it would have failed on each in turn.
# Meanwhile fourteen packages the four ISO tools genuinely need were not
# named here at all; they were present only because they arrived as
# pkg_depends of something else. An image whose manifest IS its identity
# (ADR-0208) cannot be either of those things.
#
# The four tools move to the versions that actually build, every one of
# which is now in the cache:
#   grub 2.14-6, xorriso 1.5.8.pl02-3, mtools 4.0.49-2,
#   sbsigntools 0.9.5-9
# and openssl 3.0.20-2 joins them explicitly. sbsign links libcrypto,
# and isotools.recipe copies /lib/x86_64-linux-gnu/libcrypto.so.3 out of
# this image when it harvests -- so without openssl named here, an
# isotools hostbuild fails at that copy. That is not hypothetical: it
# was removed from this image during the #175 repair and nothing in the
# manifest said it had to come back.
#
# gawk moves to 5.3.0-7 for the same reason kernel-builder 1.0.2 does:
# 5.3.0-2 links the build host's Debian libreadline.
#
# This list is derived from the image that actually works, so it is true
# by construction rather than by review. It is almost certainly larger
# than the job strictly requires -- several entries are build-time tools
# a composed build environment (ADR-0199) would supply anyway -- and
# trimming it is worth doing deliberately, against a real isotools
# hostbuild, rather than by guessing which of them nothing needs.
image_packages="autoconf:pinned:2.71-2 automake:pinned:1.16.5-2 bash:pinned:5.2.37-2 binutils:pinned:2.42-8 binutils-dev:pinned:2.42-3 bison:pinned:3.8.2-2 coreutils:pinned:9.11-3 diffutils:pinned:3.10-6 dosfstools:pinned:4.2-1 efivar:pinned:39-3 flex:pinned:2.6.4-4 gawk:pinned:5.3.0-7 gcc:pinned:16.2.0-11 getopt:pinned:2.42.2-2 gnu-efi:pinned:3.0.18-4 grep:pinned:3.11-4 grub:pinned:2.14-6 gzip:pinned:1.13-3 keyutils:pinned:1.6.3-6 libblkid:pinned:2.42.2-3 glibc:pinned:2.44-12 libuuid:pinned:2.42.2 libxcrypt:pinned:4.4.36-4 m4:pinned:1.4.19-2 make:pinned:4.4.1-4 mokutil:pinned:0.7.2-2 mtools:pinned:4.0.49-2 ncurses:pinned:6.6-5 openssl:pinned:3.0.20-2 patch:pinned:2.8-3 perl:pinned:5.40.1 pkgconf:pinned:3.0.5-4 python:pinned:3.13.5-4 sbsigntools:pinned:0.9.5-9 shim:pinned:16.1-1 tar:pinned:1.35-5 xorriso:pinned:1.5.8.pl02-3 zlib:pinned:1.3.2-6"
