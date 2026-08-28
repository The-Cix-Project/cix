#
# dev -- a declared package list. ADR-0209 retired the whole-rootfs
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
image_packages="bash:pinned:5.2.37 bc:pinned:1.08.1 binutils:pinned:2.42-7 bison:pinned:3.8.2-2 bzip2:pinned:1.0.8 coreutils:pinned:9.11-3 elfutils:pinned:0.192-6 flex:pinned:2.6.4-2 gawk:pinned:5.3.0-2 grep:pinned:3.11-2 libc-dev:pinned:2.36 m4:pinned:1.4.19 make:pinned:4.4.1 sed:pinned:4.9-2 tar:pinned:1.35-5 tcc:pinned:0.9.27 xz:pinned:5.8.3-2 zlib:pinned:1.3.2-3"
