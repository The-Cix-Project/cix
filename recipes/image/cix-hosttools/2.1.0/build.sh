#
# cix-hosttools -- the binaries the daemon itself execve()s on the host,
# outside any container: mkbootroot's staging set, and the tools the
# disk and artifact paths shell out to (ADR-0078).
#
# 2.1.0 adds btrfs-progs, and it is not an enhancement -- it is a fix
# for an image that could not do its job. mkbootroot stages
# /usr/bin/btrfs into an assembled control-plane root, sourced from
# this image, and 2.0.0 never contained it. On a freshly installed host
# that has nothing else to fall back on, every control-plane assembly
# failed:
#
#   cix bootroot assembly: mkbootroot exited 1
#   cix bootroot assembly: output: /usr/bin/btrfs: No such file or directory
#
# So a box could build cix perfectly and still not be able to deploy it.
# mkbootroot's own comment already anticipated this exactly -- "no real
# box may have built btrfs-progs.recipe onto its cix-hosttools image
# yet" -- and the answer to an anticipated gap is to close it in the
# recipe, which under ADR-0252 is the only place image membership is
# decided.
#
# The pins move forward to the newest revision of each package that has
# a published, checksum-approved artifact. That is deliberate and is the
# cold-boot property: a package whose newest revision has no artifact
# has to be built, and a host that is assembling its first control plane
# has nothing to build with. mkbootroot needs squashfs-tools in
# particular, which is why it is pinned to a revision that can be
# installed rather than merely named.
#
image_packages="bash:pinned:5.2.37-5 btrfs-progs:pinned:7.1-10 bzip2:pinned:1.0.8-4 coreutils:pinned:9.11-7 curl:pinned:8.21.0-4 gzip:pinned:1.13-4 openssl:pinned:3.0.20-5 perl:pinned:5.40.1-7 squashfs-tools:pinned:4.7.5-12 tar:pinned:1.35-6 xz:pinned:5.8.3-8 zlib:pinned:1.3.2-10"
