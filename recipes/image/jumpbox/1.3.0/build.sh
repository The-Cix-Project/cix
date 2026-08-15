#
# jumpbox -- the image jumpbox1 (ADR-0144 task #838) runs from: real
# live-LDAP SSH login, both password (pam_ldap.so's bind-as-user check)
# and pubkey (a live AuthorizedKeysCommand ldapsearch), plus real
# admin tooling for day-to-day use of the box.
#
# 1.3.0, one real addition from 1.2.0: adds xz:5.8.3-2 (a genuinely
# useful admin tool, and the specific vehicle used to close a real,
# separate bug found live tonight -- task #865's own final leg).
#
# The real bug this addition exists to close: jumpbox1's own image had
# gone 22 versions without ever picking up ADR-0150's /dev/ptmx
# baseline symlink fix, despite pkg_seed_image_baseline() supposedly
# "self-healing on its very next install" (its own doc comment).
# Root-caused live: ADR-0108's image-version hash is a MANIFEST hash
# (sorted name@version pairs), not a content hash -- reinstalling an
# already-present package at its own unchanged version reproduces the
# byte-identical manifest hash, so image_produce_new_version() just
# REPOINTS current_version back to the pre-existing directory for that
# hash (ADR-0108's own dedup) rather than ever writing (and thus
# baseline-reseeding) a fresh tree. A same-manifest reinstall cycle
# (confirmed live, twice: psmisc and screen both) genuinely cannot
# trigger this self-heal -- only a manifest that has *never before*
# existed in this image's history can. xz was picked specifically
# because it's a real, useful tool jumpbox didn't already have, and
# because tonight's own squashfs-tools/e2fsprogs TCC-compatibility
# work already had it freshly rebuilt and verified working. Confirmed
# live afterward: the resulting new image version's own /dev/ptmx
# exists (a direct exec("/dev/ptmx") on it correctly fails with
# EACCES, not ENOENT), and jumpbox1 (follow_rolling:true) picked up
# the new version automatically.
#
# See recipes/README.md for what "version" means for an image recipe
# (this file's own revision history, ADR-0149) versus an image's
# content-addressed build version (ADR-0108).
#
# Every entry here is a real, confirmed top-level OR transitive
# dependency actually installed onto the image (`pkg ls` was used to
# confirm the exact set, not assumed from pkg_depends= alone).
#
image_packages="openssl:rolling:3.0.20 zlib:rolling:1.3.2 libuuid:rolling:2.42.2 openldap-client:rolling:2.6.14 linux-pam:rolling:1.6.1-2 nss-pam-ldapd:rolling:0.9.13-2 openssh:rolling:10.4p1-8 bash:rolling:5.2.37 coreutils:rolling:9.11 perl:rolling:5.40.1 ncurses:rolling:6.6 psmisc:rolling:23.7-1 screen:rolling:5.0.2 htop:rolling:3.5.2 inetutils:rolling:2.5 mtr:rolling:0.96 iproute2:rolling:6.18.0 procps:rolling:4.0.6 xz:rolling:5.8.3-2"
