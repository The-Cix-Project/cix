#
# 6.0.0: the manifest describes the real image again, and the recipe is
# now the only place it exists.
#
# 5.0.0 named eleven packages. The live image held twenty, and the
# difference was not cosmetic: it pinned tcc at 0.9.27-9, nine
# revisions and one compiler upgrade behind the 0.9.28rc-29 the box was
# actually building with (ADR-0223). A recipe that cannot reproduce the
# image it names is not a recipe, and 5.0.0's own header says exactly
# this about 3.0.0 -- the drift came back because nothing checks it.
#
# Recovered from a GET /v1/system/backup bundle taken off 192.168.15.95
# before it was reinstalled, which is the only reason it was
# recoverable at all: the image itself is gone. That is the argument
# for this file existing rather than for capturing images by hand.
#
# Additions over 5.0.0, all of them real build dependencies that had
# been installed by hand into the live image and never written down:
# diffutils (cmp -- configure probes answer from it, and answer WRONG
# and silently when it is absent, #302), gawk, gettext (grub's own
# build calls into it), grep, linux-headers, ncurses, patch, sed,
# util-linux (sfdisk), and findutils' companions.
#
# gcc stays, for exactly one output: build/cix-boot.efi, the UEFI boot
# manager (ADR-0215). Everything else cix builds is TCC, per ADR-0001.
# binutils stays for the same reason -- 2.42-10 keeps the
# --enable-targets=x86_64-pep configuration cix-boot.efi needs to link
# at all.
#
# What is deliberately NOT here: gitea, go and go-bootstrap. The
# retired "toolchain" image carried all three, and they are why that
# image could never be described honestly -- they are a service and its
# compiler, accumulated into a build image because someone needed them
# on the box once. Building Cix has never needed any of them.
#
# cix-builder -- one job: build Cix itself (cixd, cixctl, web/,
# mkbootroot) via `pkg hostbuild cix`. ADR-0208.
#
image_packages="bash:pinned:5.2.37-5 binutils:pinned:2.42-10 coreutils:pinned:9.11-7 diffutils:pinned:3.10-8 findutils:pinned:4.10.0-5 flex:pinned:2.6.4-5 gawk:pinned:5.3.0-9 gcc:pinned:16.2.0-13 gettext:pinned:1.0-17 glibc:pinned:2.44-12 grep:pinned:3.11-6 gzip:pinned:1.13-4 linux-headers:pinned:6.18.40-4 m4:pinned:1.4.20-5 make:pinned:4.4.1-6 ncurses:pinned:6.6-8 openssl:pinned:3.0.20-5 patch:pinned:2.8-5 sed:pinned:4.9-5 tar:pinned:1.35-6 tcc:pinned:0.9.28rc-29 util-linux:pinned:2.42.2-2 xz:pinned:5.8.3-8 zlib:pinned:1.3.2-9"
