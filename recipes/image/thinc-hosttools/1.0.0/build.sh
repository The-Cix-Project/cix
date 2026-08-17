#
# thinc-hosttools -- ADR-0078: every real binary thincd itself shells
# out to at runtime (openssl for PKI, curl for pkg_source fetches,
# tar/gzip/bzip2/xz for source extraction, unsquashfs for toolchain
# import), built from source rather than borrowed from whatever
# machine happens to run mkbootroot. See docs/guides/building-thinc.md's
# own "1b. Build the host tools image" section for the full narrative
# (this set is a strict superset of what mkbootroot.c currently has a
# wiring point for -- coreutils/gzip/openssl/curl are the four it
# actually uses today; the rest are staged ahead of a future wiring
# pass, per that same guide's own note). Captured live from
# 192.168.15.95's own real, organic `pkg install --image=thinc-hosttools`
# history (issue #18), not hand-authored.
#
image_packages="coreutils:rolling:9.11 gzip:rolling:1.13 perl:rolling:5.40.1 openssl:rolling:3.0.20 zlib:rolling:1.3.2 curl:rolling:8.21.0 tar:rolling:1.35 bzip2:rolling:1.0.8 xz:rolling:5.8.3-2 squashfs-tools:rolling:4.7.5-5"
