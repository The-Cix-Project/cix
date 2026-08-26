#
# jumpbox -- pinned to a prebuilt whole-rootfs artifact, so a new host
# installs this image instead of compiling every package in it.
#
# Every entry is pinned AND image_artifact_sha256 is set, which is what
# selects ADR-0123's fast path: the daemon fetches
#   <artifact base_url>/images/jumpbox-<image_version>.tar.gz
# and extracts it as this image's entire rootfs.
#
# The checksum below is the trust anchor and it lives HERE, in git --
# never alongside the binary it validates. That separation is the whole
# point of ADR-0122's two-URL split: recipes are versioned, reviewable
# text in the repository; artifacts are opaque bytes on a plain HTTP
# server that is never itself trusted. A server that shipped both would
# be vouching for its own payload.
#
# The package list is exactly what the source host had installed, because
# the image version in the artifact's filename IS the hash of this
# manifest. Change one entry and the computed URL no longer resolves.
#
# Captured from 192.168.15.95, image version:
#   fe565aafb02a558ce244ea0976a8f6cc75e30fd6eacaec04b591a95bc0b462e0
#
image_packages="bash:pinned:5.2.37 coreutils:pinned:9.11 dhcpcd:pinned:10.5.2-5 libuuid:pinned:2.42.2 linux-pam:pinned:1.6.1-2 mtr:pinned:0.96-3 ncurses:pinned:6.6 nss-pam-ldapd:pinned:0.9.13-2 openldap-client:pinned:2.6.14 openssh:pinned:10.4p1-8 openssl:pinned:3.0.20 perl:pinned:5.40.1 procps:pinned:4.0.6-6 psmisc:pinned:23.7-1 screen:pinned:5.0.2 tar:pinned:1.35-5 vim:pinned:9.1.1428 zlib:pinned:1.3.2-2"
image_artifact_sha256="ed6ad5168fbd36ad20162acbfafe037ef5752af0a0aab73c9cc3b464194b8bbd"
