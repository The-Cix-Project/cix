#
# cix-builder -- one job: build Cix itself (cixd, cixctl, web/,
# mkbootroot) via `pkg hostbuild cix`. ADR-0208.
#
# 6.1.0 exists because ADR-0252 made this file authoritative, and two
# pins in 6.0.0 no longer described the image:
#
#   glibc  6.0.0 pinned 2.44-12; the image holds 2.44-14
#   zlib   6.0.0 pinned 1.3.2-9;  the image holds 1.3.2-10
#
# The glibc move is deliberate and is the point of ADR-0251: 2.44-14 is
# the first package rebuilt under the finalize policy -- 2114 files down
# to 1341, libc.so.6 from 11.42 MiB of sections to 2,011,912 bytes,
# libc.a no longer shipped three times. gcc was verified still able to
# compile, link and run an ordinary dynamic binary against it before
# this pin moved (probe-gcc-postglibc).
#
# The zlib difference is the one ADR-0252 quotes as its own live
# evidence: it was already there, on an image materialized from its own
# recipe minutes earlier. Under the old arrangement it would have sat
# unnoticed; under this one the recipe is what must move.
#
# 6.0.0s own note, still true: what is deliberately NOT here is gitea,
# go and go-bootstrap. The retired "toolchain" image carried all three,
# and they are why it could never be described honestly -- a service and
# its compiler, accumulated because someone needed them on the box once.
# Building Cix has never needed any of them.
#
# gcc stays, for exactly one output: build/cix-boot.efi, the UEFI boot
# manager (ADR-0215). Everything else cix builds is TCC, per ADR-0001.
# binutils stays for the same reason -- 2.42-10 keeps the
# --enable-targets=x86_64-pep configuration cix-boot.efi needs to link.
#
image_packages="bash:pinned:5.2.37-5 binutils:pinned:2.42-10 coreutils:pinned:9.11-7 diffutils:pinned:3.10-8 findutils:pinned:4.10.0-5 flex:pinned:2.6.4-5 gawk:pinned:5.3.0-9 gcc:pinned:16.2.0-13 gettext:pinned:1.0-17 glibc:pinned:2.44-14 grep:pinned:3.11-6 gzip:pinned:1.13-4 linux-headers:pinned:6.18.40-4 m4:pinned:1.4.20-5 make:pinned:4.4.1-6 ncurses:pinned:6.6-8 openssl:pinned:3.0.20-5 patch:pinned:2.8-5 sed:pinned:4.9-5 tar:pinned:1.35-6 tcc:pinned:0.9.28rc-29 util-linux:pinned:2.42.2-2 xz:pinned:5.8.3-8 zlib:pinned:1.3.2-10"
