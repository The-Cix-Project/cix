#
# jumpbox -- the image jumpbox1 (ADR-0144 task #838) runs from: real
# live-LDAP SSH login, both password (pam_ldap.so's bind-as-user check)
# and pubkey (a live AuthorizedKeysCommand ldapsearch), plus real
# admin tooling for day-to-day use of the box.
#
# 1.4.0 -- real operator tooling for day-to-day use of the box, and an
# honesty pass over what this file claims.
#
# Added, each confirmed installed and working on the real image (not
# assumed from the recipe declaring them):
#   procps:4.0.6-6  ps/free/kill/pgrep/pkill/pidof/pmap/pwdx/tload/
#                   uptime/vmstat/w/watch/slabtop/hugetop + sysctl
#   vim:9.1.1428    a real editor
#   mtr:0.96-2      network path diagnosis
#
# Two entries 1.3.0 declared are REMOVED rather than left aspirational:
# htop and inetutils/iproute2/xz were listed there but had never actually
# installed onto this image -- a live `pkg ls` showed the image carrying
# 13 packages while the recipe named 19. A recipe that declares packages
# the image does not have is worse than one that declares fewer: it reads
# as a guarantee and silently is not one. htop currently fails to build
# under this toolchain (two real TCC gaps: an ambient-gcc/limits.h
# interaction, then a NAN constant-initializer difference between this
# project's own two TCCs) and is tracked as its own issue; it goes back
# in here the moment it genuinely builds, not before.
#
# procps deliberately ships WITHOUT `top` -- see procps' own recipe for
# why (top is the only threaded binary in the package, and the __thread
# workaround its build needs would be a real data race there). htop was
# meant to cover that role; until it builds, `ps`/`vmstat`/`slabtop` do.
#
# See recipes/README.md for what "version" means for an image recipe
# (this file's own revision history, ADR-0149) versus an image's
# content-addressed build version (ADR-0108).
#
# Every entry here is a real, confirmed top-level OR transitive
# dependency actually installed onto the image (`pkg ls` was used to
# confirm the exact set, not assumed from pkg_depends= alone).
#
image_packages="openssl:rolling:3.0.20 zlib:rolling:1.3.2-2 libuuid:rolling:2.42.2 openldap-client:rolling:2.6.14 linux-pam:rolling:1.6.1-2 nss-pam-ldapd:rolling:0.9.13-2 openssh:rolling:10.4p1-8 bash:rolling:5.2.37 coreutils:rolling:9.11 perl:rolling:5.40.1 ncurses:rolling:6.6 psmisc:rolling:23.7-1 screen:rolling:5.0.2 vim:rolling:9.1.1428 procps:rolling:4.0.6-6 mtr:rolling:0.96-3"
