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
image_packages="bash:pinned:5.2.37-2 binutils:pinned:2.42-2 coreutils:pinned:9.11-3 gcc:pinned:16.2.0-11 libc-dev:pinned:2.36-3 m4:pinned:1.4.19 tar:pinned:1.35-2 zlib:pinned:1.3.2-3"
