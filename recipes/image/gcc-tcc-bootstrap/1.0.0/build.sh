#
# gcc-tcc-bootstrap -- pinned to a prebuilt whole-rootfs artifact, so a new host
# installs this image instead of compiling every package in it.
#
# Every entry is pinned AND image_artifact_sha256 is set, which is what
# selects ADR-0123's fast path: the daemon fetches
#   <artifact base_url>/images/gcc-tcc-bootstrap-<image_version>.tar.gz
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
#   739bcfc6e7d7e464f0cf1801bf744a502719103387fcdcb304b38fb0830d0ad9
#
image_packages="bash:pinned:5.2.37-2 binutils:pinned:2.42-2 coreutils:pinned:9.11-3 gcc:pinned:16.2.0-11 libc-dev:pinned:2.36-3 m4:pinned:1.4.19 tar:pinned:1.35-2 zlib:pinned:1.3.2-3"
image_artifact_sha256="48200fa6e4e694bf8c9b507ee4be74e1cf5a10e253fc2496b782d2d4f46c623a"
