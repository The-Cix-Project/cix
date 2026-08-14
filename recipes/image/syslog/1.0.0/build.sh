#
# syslog -- the image syslog-1/syslog-2 (ADR-0127) run from: sysklogd,
# this platform's own reference syslog-forwarding receiver. Captured
# as an image recipe (ADR-0123) after the fact, from the real package
# the box's own syslog-1/syslog-2 containers were actually built with
# this session -- see recipes/README.md. 1.0.0 above is this recipe's
# own directory version (ADR-0149), not the image's content-addressed
# build version (ADR-0108).
#
image_packages="sysklogd:pinned:2.7.0"
