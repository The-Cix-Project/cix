#
# toolchain -- a build image that is nothing but its declared packages.
#
# ADR-0225. Every build image on this platform today starts life as a
# wholesale copy of some host's /usr/{include,lib,lib64,bin,libexec}
# (pkg_bootstrap_build_image()), with packages installed on top. That
# seed is where 93% of cix-builder's 8 GB comes from -- rustup, cargo,
# chromium, node, a developer workstation shipped inside a build image
# and downloaded by every host that installs it (#168).
#
# The seed exists because Cix could not build its own toolchain. That
# stopped being true: everything below is a real Cix package, built from
# source on a Cix host. So this image is the experiment that decides
# whether the copy is still needed at all -- it is created empty and
# filled only from this manifest.
#
# Contents derived from what recipes actually declare, not guessed: the
# baseline that nearly every package in this set names in its own
# pkg_build_depends, plus the autotools that configure-driven recipes
# need, plus gcc and binutils because cix's own build needs them for
# cix-boot.efi (ADR-0215) and because gcc is the Tier 3 compiler for the
# packages that cannot use TCC (ADR-0224).
#
# Deliberately NOT pinned. cix-builder's own recipe pins every entry,
# which is right for a reproducible builder; this one wants whatever is
# current, because its whole job is to answer "can the current package
# set stand up a working build image on its own".
#
image_packages="tcc:rolling:0.9.28rc gcc:rolling:16.2.0 binutils:rolling:2.42 glibc:rolling:2.44 linux-headers:rolling:6.18.40 make:rolling:4.4.1 bash:rolling:5.2.37 coreutils:rolling:9.11 sed:rolling:4.9 grep:rolling:3.11 gawk:rolling:5.3.0 findutils:rolling:4.10.0 diffutils:rolling:3.10 m4:rolling:1.4.20 bison:rolling:3.8.2 flex:rolling:2.6.4 perl:rolling:5.40.1 pkgconf:rolling:3.0.5 autoconf:rolling:2.71 automake:rolling:1.16.5 libtool:rolling:2.4.7 patch:rolling:2.8 tar:rolling:1.35 gzip:rolling:1.13 xz:rolling:5.8.3 zlib:rolling:1.3.2 openssl:rolling:3.0.20"
