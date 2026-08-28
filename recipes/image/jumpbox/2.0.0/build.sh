#
# jumpbox -- a declared package list. ADR-0209 retired the whole-rootfs
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
image_packages="bash:pinned:5.2.37 coreutils:pinned:9.11 dhcpcd:pinned:10.5.2-5 libuuid:pinned:2.42.2 linux-pam:pinned:1.6.1-2 mtr:pinned:0.96-3 ncurses:pinned:6.6 nss-pam-ldapd:pinned:0.9.13-2 openldap-client:pinned:2.6.14 openssh:pinned:10.4p1-8 openssl:pinned:3.0.20 perl:pinned:5.40.1 procps:pinned:4.0.6-6 psmisc:pinned:23.7-1 screen:pinned:5.0.2 tar:pinned:1.35-5 vim:pinned:9.1.1428 zlib:pinned:1.3.2-2"
