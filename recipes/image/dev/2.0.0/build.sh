#
# dev -- pinned to a prebuilt whole-rootfs artifact, so a new host
# installs this image instead of compiling every package in it.
#
# Every entry is pinned AND image_artifact_sha256 is set, which is what
# selects ADR-0123's fast path: the daemon fetches
#   <artifact base_url>/images/dev-<image_version>.tar.gz
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
#   3a9b50b85a2333e0e60c80c1c441000f0bbd6b8c8f1434ae77095919b8b6f2da
#
image_packages="bash:pinned:5.2.37 bc:pinned:1.08.1 binutils:pinned:2.42-7 bison:pinned:3.8.2-2 bzip2:pinned:1.0.8 coreutils:pinned:9.11-3 elfutils:pinned:0.192-6 flex:pinned:2.6.4-2 gawk:pinned:5.3.0-2 grep:pinned:3.11-2 libc-dev:pinned:2.36 m4:pinned:1.4.19 make:pinned:4.4.1 sed:pinned:4.9-2 tar:pinned:1.35-5 tcc:pinned:0.9.27 xz:pinned:5.8.3-2 zlib:pinned:1.3.2-3"
image_artifact_sha256="11110fe3361ae7ea667938f8ce1a4714a24461b4d2a11b8ed0e9a045b3e1ad91"
