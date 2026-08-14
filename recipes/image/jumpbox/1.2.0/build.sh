#
# jumpbox -- the image jumpbox1 (ADR-0144 task #838) runs from: real
# live-LDAP SSH login, both password (pam_ldap.so's bind-as-user check)
# and pubkey (a live AuthorizedKeysCommand ldapsearch), plus real
# admin tooling for day-to-day use of the box.
#
# 1.2.0, one real fix from 1.1.0: adds procps (ps/top/free/kill/etc.,
# already an existing recipe since Phase 31 -- pkg_depends="", no new
# transitive deps needed). This was installed live onto the real box's
# jumpbox image (task #835) to close a real "psmisc has pstree, not
# ps" gap the user hit directly, but was never added here -- a real
# One Source of Truth violation (the same class task #837 already
# fixed once for a different set of recipes): this recipe, not the
# live box, is the thing a fresh or rebuilt jumpbox actually gets
# built from, so an addition that only ever happened live is invisible
# to every future deployment. Found auditing this file directly
# against the real box's own `pkg ls --image=jumpbox` output
# (task #842).
#
# See recipes/README.md for what "version" means for an image recipe
# (this file's own revision history, ADR-0149) versus an image's
# content-addressed build version (ADR-0108).
#
# Every entry here is a real, confirmed top-level OR transitive
# dependency actually installed onto the image (`pkg ls` was used to
# confirm the exact set, not assumed from pkg_depends= alone).
#
image_packages="openssl:rolling:3.0.20 zlib:rolling:1.3.2 libuuid:rolling:2.42.2 openldap-client:rolling:2.6.14 linux-pam:rolling:1.6.1-2 nss-pam-ldapd:rolling:0.9.13-2 openssh:rolling:10.4p1-8 bash:rolling:5.2.37 coreutils:rolling:9.11 perl:rolling:5.40.1 ncurses:rolling:6.6 psmisc:rolling:23.7-1 screen:rolling:5.0.2 htop:rolling:3.5.2 inetutils:rolling:2.5 mtr:rolling:0.96 iproute2:rolling:6.18.0 procps:rolling:4.0.6"
