#
# ldap -- the image ldap-1/ldap-2 (ADR-0144) run from: glauth, this
# platform's own standard integrable LDAP provider.
#
# 1.2.0: declares glibc. glauth is a cgo binary (its embedded SQLite is
# compiled C) and links against libc dynamically, and an image whose
# manifest names no libc cannot start a container at all -- cixd refuses
# it outright: image "ldap" has no C library. 1.1.0 worked only while
# an earlier baseline seeded libc implicitly; the manifest is now the one
# statement of what the image contains, so it has to say so. Same form
# as recipes/image/chrony. Both rolling, per the charter.
#
# See recipes/README.md for what "version" means for an image recipe
# (ADR-0149) versus an image's content-addressed build version (ADR-0108).
#
image_packages="glibc:rolling:2.44 glauth:rolling:2.4.0"
