#
# chrony -- the image ntp-1/ntp-2 run from (ADR-0119, ADR-0168's own
# cap_add: ["CAP_SYS_TIME"] container-level need).
#
# 1.1.0: libc-dev -> glibc (#187, ADR-0217).
#
# 1.0.0's header called libc-dev "a real, deliberate dependency here,
# not build-tooling leftover", and that reasoning still holds exactly
# as written -- chrony.recipe pins CC=tcc via a thin tcc-nopthread
# wrapper (CLAUDE.md's Environment notes: TCC mishandles the -pthread
# driver flag), and needs a real libc present in this image for that
# build to link against. What changed is only which package provides
# it. libc-dev was Debian's headers and CRT objects repackaged; glibc
# is the one this project builds from source (ADR-0216), and it carries
# the same headers, the same crt1.o/crti.o/crtn.o, and a libc that is
# actually ours.
#
# glibc is NOT implicit for an image the way it is for a composed build
# environment (PKG_BASE_LIBC, ADR-0216) -- pkg_seed_default_image_libc()
# seeds only the default image. A named image gets what its manifest
# names, so it is named here.
#
image_packages="glibc:pinned:2.44-12 chrony:rolling:4.8"
