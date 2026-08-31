#
# 1.1.0: libc-dev -> glibc (#187, ADR-0217). Same job, different
# provider: libc-dev was Debian headers and CRT objects repackaged;
# glibc is the one this project builds from source (ADR-0216) and
# carries the same headers and the same crt1.o/crti.o/crtn.o. Pinned
# at 2.44-12, the revision every live builder on 192.168.15.95 already
# runs. Named explicitly because glibc is implicit only for a composed
# build environment (PKG_BASE_LIBC) and for the default image -- a
# named image gets exactly what its manifest names.
#
# This recipe is believed superseded (see issue filed alongside this
# change), but it is migrated rather than left pinned to a package
# that no longer exists: a recipe that cannot resolve is a worse
# resting state than one that is merely unused.
#
#
# gcc-tcc-bootstrap -- a declared package list. ADR-0209 retired the whole-rootfs
# artifact fast path this recipe used to opt into (every entry pinned
# plus an image_artifact_sha256, which made the daemon fetch and
# extract one tarball as the image's entire rootfs). That mechanism
# shipped one contaminated capture to every host that installed it
# (issue #168), and it duplicated something a package set plus a
# manifest already describes completely. Package artifacts are the only
# published binaries now, each checksummed and traceable to a recipe
# and the Cix host that built it.
#
# The package list below is unchanged and still authoritative; applying
# this recipe declares it into the image's manifest, and the packages
# are then installed individually from their own verified artifacts.
#
# Captured from 192.168.15.95.
#
image_packages="bash:pinned:5.2.37-2 binutils:pinned:2.42-2 coreutils:pinned:9.11-3 gcc:pinned:16.2.0-11 glibc:pinned:2.44-12 m4:pinned:1.4.19 tar:pinned:1.35-2 zlib:pinned:1.3.2-3"
