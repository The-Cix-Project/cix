#
# ldap -- the image ldap-1/ldap-2 (ADR-0144) run from: glauth, this
# platform's own standard integrable LDAP provider. Captured as an
# image recipe (ADR-0123) after the fact, from the real package the
# box's own ldap-1/ldap-2 containers were actually built with this
# session -- see recipes/README.md. 1.0.0 above is this recipe's own
# directory version (ADR-0149), not the image's content-addressed
# build version (ADR-0108).
#
image_packages="glauth:pinned:2.4.0"
