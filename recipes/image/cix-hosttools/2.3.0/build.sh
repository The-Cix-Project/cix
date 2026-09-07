#
# 2.3.0: openssl 3.0.20-5 -> 3.0.20-6, so this image carries the
# stripped build (#328).
#
# The pin is the whole point of the bump. ADR-0251's finalize policy
# strips ELF, but it only runs when a package is BUILT, and the
# artifact tier correctly installs an already-approved cached artifact
# instead -- so 3.0.20-5, published before the policy landed, kept
# shipping its debug sections into every installer ISO. Rebuilding the
# package is only half of it: this image PINS its versions, so a
# materialize would put the old bytes straight back.
#
# btrfs-progs moves to 7.1-11 as well. It could not build when this
# revision was first drafted -- it links -lpthread through gcc and the
# installed glibc shipped no libpthread.a -- and glibc 2.44-16 restored
# those member-less archives (#324), after which 7.1-11 built on the
# first attempt. mkfs.btrfs was 80% debug and is the single largest
# piece of the 3.93 MiB this set of rebuilds exists to remove.
#
#
# cix-hosttools -- the binaries the daemon itself execve()s on the host,
# outside any container: mkbootroot's staging set, and the tools the
# disk and artifact paths shell out to (ADR-0078).
#
# 2.2.0 adds glibc, and like 2.1.0's btrfs-progs this is a fix for an
# image that could not do its job rather than an enhancement. The image
# listed sixteen packages and not one of them was a C library, so every
# binary in it depended on resolving libc from somewhere else. On a
# fresh host there is no somewhere else:
#
#   mksquashfs: error while loading shared libraries: libmvec.so.1:
#   cannot open shared object file: No such file or directory
#   mksquashfs failed (status 32512)
#
# mkbootroot points LD_LIBRARY_PATH at THIS image and, when the image
# has one, invokes the tool through THIS image's own dynamic loader --
# deliberately, because ld.so and libc.so.6 share a version-locked
# private interface and taking one from each tree fails in ways that
# are hard to read (ADR-0154, and the __pointer_chk_guard failure its
# comment records). An image that supplies the tools but not the libc
# they load makes that arrangement impossible to satisfy: there is no
# loader to invoke and no libc to point at.
#
# glibc is pinned to 2.44-14, the first revision built under ADR-0251's
# finalize policy -- 2114 files down to 1341, libc.so.6 from 11.42 MiB
# of sections to 2,011,912 bytes -- so adding a C library here costs
# roughly a sixth of what the same addition would have cost before it.
#
# 2.1.0s note, still true: btrfs-progs is here because mkbootroot stages
# /usr/bin/btrfs and 2.0.0 never contained it, so every control-plane
# assembly on a fresh host failed. Pins track the newest revision of
# each package that has a published, checksum-approved artifact, which
# is the cold-boot property: a package whose newest revision has no
# approved artifact must be BUILT, and a host assembling its first
# control plane has nothing to build with.
#
image_packages="glibc:pinned:2.44-14 bash:pinned:5.2.37-5 btrfs-progs:pinned:7.1-11 bzip2:pinned:1.0.8-4 coreutils:pinned:9.11-7 curl:pinned:8.21.0-4 gzip:pinned:1.13-4 openssl:pinned:3.0.20-6 perl:pinned:5.40.1-7 squashfs-tools:pinned:4.7.5-12 tar:pinned:1.35-6 xz:pinned:5.8.3-8 zlib:pinned:1.3.2-10"
