#
# cix-builder -- pinned to a prebuilt whole-rootfs artifact, so a new host
# installs this image instead of compiling every package in it.
#
# Every entry is pinned AND image_artifact_sha256 is set, which is what
# selects ADR-0123's fast path: the daemon fetches
#   <artifact base_url>/images/cix-builder-<image_version>.tar.gz
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
#   cf38daf76730449b147d32d0b919a70e4069c83cae417057cf1885978c0009ec
#
image_packages="bash:pinned:5.2.37 bc:pinned:1.08.1-2 binutils:pinned:2.42-8 coreutils:pinned:9.11 flex:pinned:2.6.4-4 gcc:pinned:9.5.0-6 libc-dev:pinned:2.36 m4:pinned:1.4.19-2 make:pinned:4.4.1 openssl:pinned:3.0.20 perl:pinned:5.40.1 tar:pinned:1.35-5 tcc:pinned:0.9.27-7 zlib:pinned:1.3.2-6"
image_artifact_sha256="d87e3722db2c896723e5f1be7e1eb2414c111190ea5a384e05506a7abc176de9"
