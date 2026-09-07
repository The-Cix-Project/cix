#
# 1.2.0: declare glibc. This image had no C library at all, so
# `container apply-recipe dns-1` was refused outright -- "image \"dns\"
# has no C library -- install a libc package (glibc) into it before
# running a container from it". dnsmasq declares pkg_depends="", so
# nothing pulled glibc in transitively, and pkg_seed_image_baseline()
# stages a loader and libnss_files, not a libc. Found restoring the
# fleet after the btrfs migration, when these images were built from
# their recipes for the first time rather than surviving as directories
# that had been populated by hand before the recipe existed.
#
# ldap/1.2.0 and chrony/1.1.0 already declare it; these two were the
# outliers, not a different design.
#
# syslog -- the image syslog-1/syslog-2 (ADR-0127) run from: sysklogd,
# this platform's own reference syslog-forwarding receiver.
#
# 1.1.0: switches sysklogd from pinned to rolling, matching Cix's
# own rolling-release charter (CLAUDE.md's opening line) -- see
# recipes/image/dns/1.1.0/build.sh's own header comment for the full
# reasoning, identical here. See recipes/README.md for what "version"
# means for an image recipe (ADR-0149) versus an image's
# content-addressed build version (ADR-0108).
#
image_packages="glibc:rolling:2.44 sysklogd:rolling:2.7.0"
