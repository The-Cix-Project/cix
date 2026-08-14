#
# ldap -- the image ldap-1/ldap-2 (ADR-0144) run from: glauth, this
# platform's own standard integrable LDAP provider.
#
# 1.1.0: switches glauth from pinned to rolling, matching Kanxeo's own
# rolling-release charter (CLAUDE.md's opening line) -- see
# recipes/image/dns/1.1.0/build.sh's own header comment for the full
# reasoning, identical here. See recipes/README.md for what "version"
# means for an image recipe (ADR-0149) versus an image's
# content-addressed build version (ADR-0108).
#
image_packages="glauth:rolling:2.4.0"
