#
# cix-builder -- the toolchain image cix.recipe's own self-hosted
# rebuild (ADR-0057) needs: tcc to compile, make to drive the build,
# libc-dev for headers/CRT objects, bash (glibc's popen() hardcodes
# /bin/sh), coreutils (the root Makefile's own `mkdir -p build`),
# openssl (cixd itself links -lssl -lcrypto, ADR-0059), and perl
# (a real build-time dependency of some of this set's own recipes).
# See docs/guides/building-cix.md's own "1. Build a toolchain image"
# section for the full narrative. Captured live from 192.168.15.95's
# own real, organic `pkg install --image=cix-builder` history
# (issue #18), not hand-authored.
#
image_packages="tcc:rolling:0.9.27 libc-dev:rolling:2.36 make:rolling:4.4.1 bash:rolling:5.2.37 coreutils:rolling:9.11 perl:rolling:5.40.1 openssl:rolling:3.0.20"
