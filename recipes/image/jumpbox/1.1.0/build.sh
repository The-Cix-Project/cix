#
# jumpbox -- the image jumpbox1 (ADR-0144 task #838) runs from: real
# live-LDAP SSH login, both password (pam_ldap.so's bind-as-user check)
# and pubkey (a live AuthorizedKeysCommand ldapsearch), plus real
# admin tooling for day-to-day use of the box.
#
# 1.1.0, two real changes from 1.0.0:
#  - Every entry switches from pinned to rolling, matching thinC's
#    own rolling-release charter (CLAUDE.md's opening line) -- 1.0.0's
#    blanket "pinned" was captured from what happened to be installed
#    at the time, not a deliberate policy choice, corrected once asked
#    about directly. See recipes/README.md for what "version" means
#    for an image recipe (this file's own revision history, ADR-0149)
#    versus an image's content-addressed build version (ADR-0108).
#  - linux-pam bumped to 1.6.1-2 (adds pam_mkhomedir -- task #864, a
#    real "Could not chdir to home directory" gap found live) and six
#    real admin tools added: psmisc, screen, htop, inetutils, mtr,
#    iproute2 (plus their shared ncurses dependency) -- task #842's
#    jumpbox tooling audit.
#
# Every entry here is a real, confirmed top-level OR transitive
# dependency actually installed onto the image (`pkg ls` was used to
# confirm the exact set, not assumed from pkg_depends= alone).
#
image_packages="openssl:rolling:3.0.20 zlib:rolling:1.3.2 libuuid:rolling:2.42.2 openldap-client:rolling:2.6.14 linux-pam:rolling:1.6.1-2 nss-pam-ldapd:rolling:0.9.13-2 openssh:rolling:10.4p1-8 bash:rolling:5.2.37 coreutils:rolling:9.11 perl:rolling:5.40.1 ncurses:rolling:6.6 psmisc:rolling:23.7-1 screen:rolling:5.0.2 htop:rolling:3.5.2 inetutils:rolling:2.5 mtr:rolling:0.96 iproute2:rolling:6.18.0"
