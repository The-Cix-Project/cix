#
# jumpbox -- the image jumpbox1 (ADR-0144 task #838) runs from: real
# live-LDAP SSH login, both password (pam_ldap.so's bind-as-user check)
# and pubkey (a live AuthorizedKeysCommand ldapsearch). Captured as an
# image recipe (ADR-0123) after the fact, from the real, complete
# package set the box's own jumpbox1 container was actually built
# with this session (openssh:10.4p1-8 specifically -- see that
# recipe's own header comment for why -7 isn't enough) -- see
# recipes/README.md. 1.0.0 above is this recipe's own directory
# version (ADR-0149), not the image's content-addressed build version
# (ADR-0108).
#
# Every entry here is a real, confirmed top-level OR transitive
# dependency actually installed onto the image (`pkg ls` was used to
# confirm the exact set, not assumed from pkg_depends= alone) -- this
# recipe's own image_packages= is meant to represent the full,
# realized package set, not just what an operator would type by hand
# (pkg's own dependency resolver would pull the rest in regardless,
# but a recipe applied via `image apply-recipe` bulk-declares the
# manifest directly, with no dependency-resolution pass of its own).
#
image_packages="openssl:pinned:3.0.20 zlib:pinned:1.3.2 libuuid:pinned:2.42.2 openldap-client:pinned:2.6.14 linux-pam:pinned:1.6.1 nss-pam-ldapd:pinned:0.9.13-2 openssh:pinned:10.4p1-8 bash:pinned:5.2.37 coreutils:pinned:9.11 perl:pinned:5.40.1"
