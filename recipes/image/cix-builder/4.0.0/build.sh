#
# 4.0.0: the manifest describes the image again, and gains the two
# packages cix now genuinely needs.
#
# gcc and binutils are here for exactly one output -- build/cix-boot.efi,
# the UEFI boot manager (ADR-0215). Everything else cix builds is still
# TCC. They have to be installed INTO this image rather than merely
# declared in cix.recipe's pkg_build_depends, because a hostbuild's
# build container uses --build-image='s own rootfs and never a composed
# environment (ADR-0056) -- declaring them changed nothing and the
# build died at "make: /usr/bin/gcc: No such file or directory".
#
# binutils is pinned at 2.42-9 specifically: that is the first revision
# configured with --enable-targets=x86_64-pep, and without the PE/COFF
# linker emulation cix-boot.efi cannot be linked at all.
#
# 3.0.0's manifest had drifted badly from the real image -- it named
# eleven packages, of which the live image had a different set entirely.
# This one was read back off the running host.
#
#
# cix-builder -- one job: build Cix itself (cixd, cixctl, web/,
# mkbootroot) via `pkg hostbuild cix`. ADR-0208.
#
# gcc is GONE. 2.0.0 carried gcc:9.5.0-6, and that was never a
# decision: these manifests were captured from whatever a real host
# held (see 2.0.0's own header, "Captured from 192.168.15.95"), and gcc
# was resident there from the gcc-bootstrap work, so the snapshot took
# it. It has to go, because ADR-0001 makes TCC the exclusive toolchain
# for this project's own code and an available gcc is how that gets
# violated silently: a configure that does not pin CC=tcc selects the
# real compiler instead. Not hypothetical -- that is how an sshd was
# once produced with -lpam on its link line and no PAM recorded in the
# resulting ELF at all.
#
# THIS LIST IS THE SET THAT DEMONSTRABLY BUILDS CIX, not a capture and
# not a guess. An earlier draft of 3.0.0 was 2.0.0-minus-gcc, carrying
# openssl/perl/bc/flex/coreutils/m4/tar/zlib inherited from that old
# snapshot -- but the image actually building Cix on 192.168.15.95
# today holds none of them and builds v2.2.0-rc4 successfully, which is
# stronger evidence than any manifest. Every entry below earns its
# place:
#
#   tcc       compiles everything (ADR-0001); 0.9.27-9 adds
#             __has_include on top of -7's do-while codegen fix
#   make      drives the build
#   libc-dev  headers and the CRT startup objects (crt1.o/crti.o/
#             crtn.o); 2.36-5 for the libdl.a that -ldl needs and the
#             mirror fix of issue #167
#   bash      glibc's popen() hardcodes /bin/sh with no override
#   binutils  ar/ld for anything reaching past tcc's own linker;
#             2.42-8 because it is the first version to declare its own
#             zlib dependency, without which a composed build
#             environment holds an `ar` that cannot start
#   sed,grep,gawk  the text tools build steps shell out to
#
# Anything genuinely missing surfaces as a real, specific build failure
# naming exactly what it wants -- which is how it should be found, one
# real gap at a time, never speculatively.
#
# There is no image artifact to publish: ADR-0209 retired the
# whole-rootfs tier -- the accumulation this image suffered from (#168)
# is exactly what it enabled to spread.
#
#   zlib,flex,m4  not asked for directly -- binutils 2.42-8 declares
#             zlib and flex (that declaration is exactly why it is
#             pinned here), and flex needs m4. Named because the image
#             version IS the hash of this list, so a manifest short of
#             what the image holds computes an artifact URL that does
#             not resolve.
#
image_packages="bash:pinned:5.2.37-2 binutils:pinned:2.42-9 coreutils:pinned:9.11-3 flex:pinned:2.6.4-4 gcc:pinned:16.2.0-11 libc-dev:pinned:2.36-8 m4:pinned:1.4.19-2 make:pinned:4.4.1-4 openssl:pinned:3.0.20-2 tcc:pinned:0.9.27-9 zlib:pinned:1.3.2-6"
