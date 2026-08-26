#
# cix-hosttools -- pinned to a prebuilt whole-rootfs artifact, so a new host
# installs this image instead of compiling every package in it.
#
# Every entry is pinned AND image_artifact_sha256 is set, which is what
# selects ADR-0123's fast path: the daemon fetches
#   <artifact base_url>/images/cix-hosttools-<image_version>.tar.gz
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
#   e5521e540229b2c1e5f7db79e5a16565e1d3bfa3fbbcbc52a7bda4eec2bde5f2
#
image_packages="bash:pinned:5.2.37-2 bzip2:pinned:1.0.8 coreutils:pinned:9.11 curl:pinned:8.21.0 gzip:pinned:1.13 openssl:pinned:3.0.20 perl:pinned:5.40.1 squashfs-tools:pinned:4.7.5-5 tar:pinned:1.35-5 xz:pinned:5.8.3-2 zlib:pinned:1.3.2"
image_artifact_sha256="643b737ccd3a35765ca262a64d62170f5509f04053c3bca95cc51fa2aa54ef95"
