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
# dns -- the image dns-1/dns-2 (ADR-0089/0091) run from: a single
# dnsmasq install, real DNS/DHCP server (daemon/src/dns.c's own doc
# comment already assumes exactly this).
#
# 1.1.0: switches dnsmasq from pinned to rolling. Cix's own charter
# (CLAUDE.md's opening line) calls this "a custom, rolling-release
# hardware and workload orchestration platform" -- rolling is the
# default posture, not pinned; 1.0.0's blanket "pinned" was captured
# from what happened to be installed at the time, not a deliberate
# policy choice, and was corrected once asked about directly. See
# recipes/README.md for what "version" means for an image recipe
# (this file's own revision history, ADR-0149) versus an image's
# content-addressed build version (ADR-0108) -- unrelated axes.
#
image_packages="glibc:rolling:2.44 dnsmasq:rolling:2.90"
