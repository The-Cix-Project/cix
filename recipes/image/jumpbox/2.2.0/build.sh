#
# jumpbox -- the image the `jump` container runs: an SSH entry point that
# authenticates against this platform's own LDAP live, rather than against
# copied-in files (ADR-0144/ADR-0145).
#
# 2.2.0: declares htop. Asked for directly, alongside btop and screen --
# screen was already here since 2.0.0, and btop needs a package recipe that
# does not exist yet, so it arrives in a later bump rather than being
# promised in this one.
#
# Declaring it here, rather than leaving the ad-hoc `pkg install --image=`
# that was tried first, is the difference between a package that is in the
# image and one that only looks installed. An image's manifest is a
# DECLARATION, and the image's version is a hash of it (ADR-0108/ADR-0155).
# A package installed without a manifest entry leaves the hash unchanged,
# so image_produce_new_version() dedups against the pre-existing directory
# and the freshly built tree is discarded -- the install reports success,
# the registry records it, and the file never reaches the rootfs. Confirmed
# on 192.168.15.95: `GET /v1/pkg/htop@jumpbox` said installed with a real
# file list while `ls /usr/bin/htop` in the running container said no such
# file. The manifest is what makes an install stick.
#
# 2.1.0: declares glibc. Every binary here links libc dynamically, and cixd
# refuses to start a container from an image whose manifest names no C
# library -- 2.0.0 worked only while an earlier baseline seeded one
# implicitly, which is the identical gap recipes/image/ldap/1.2.0 closed.
# The manifest is the one statement of what the image contains, so it has
# to say so.
#
# See recipes/README.md for what "version" means for an image recipe
# (ADR-0149) versus an image's content-addressed build version (ADR-0108).
#
image_packages="glibc:rolling:2.44 bash:pinned:5.2.37 coreutils:pinned:9.11 dhcpcd:pinned:10.5.2-5 htop:pinned:3.5.2-7 libuuid:pinned:2.42.2 linux-pam:pinned:1.6.1-2 mtr:pinned:0.96-3 ncurses:pinned:6.6 nss-pam-ldapd:pinned:0.9.13-2 openldap-client:pinned:2.6.14 openssh:pinned:10.4p1-8 openssl:pinned:3.0.20 perl:pinned:5.40.1 procps:pinned:4.0.6-6 psmisc:pinned:23.7-1 screen:pinned:5.0.2 tar:pinned:1.35-5 vim:pinned:9.1.1428 zlib:pinned:1.3.2-2"
