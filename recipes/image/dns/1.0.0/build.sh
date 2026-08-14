#
# dns -- the image dns-1/dns-2 (ADR-0089/0091) run from: a single
# dnsmasq install, real DNS/DHCP server (daemon/src/dns.c's own doc
# comment already assumes exactly this). Captured as an image recipe
# (ADR-0123) after the fact, from the real package the box's own
# dns-1/dns-2 containers were actually built with this session --
# see recipes/README.md for how to keep this current if the pinned
# version below is ever bumped, and 1.0.0 above (this recipe's own
# directory version, ADR-0149) for what changing the package list
# means for the version number -- distinct from the image's own
# content-addressed build version (ADR-0108), which this file never
# names.
#
image_packages="dnsmasq:pinned:2.90"
