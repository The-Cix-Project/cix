#
# cix-builder -- a declared package list. ADR-0209 retired the whole-rootfs
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
image_packages="bash:pinned:5.2.37 bc:pinned:1.08.1-2 binutils:pinned:2.42-8 coreutils:pinned:9.11 flex:pinned:2.6.4-4 gcc:pinned:9.5.0-6 libc-dev:pinned:2.36 m4:pinned:1.4.19-2 make:pinned:4.4.1 openssl:pinned:3.0.20 perl:pinned:5.40.1 tar:pinned:1.35-5 tcc:pinned:0.9.27-7 zlib:pinned:1.3.2-6"
