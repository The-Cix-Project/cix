#
# router -- the image cr-1/cr-2 run from: a forwarding router pair with
# a VRRP-managed floating gateway between them.
#
# Deliberately two packages. Forwarding itself needs NO software: it is
# net.ipv4.ip_forward inside the container's own netns, which the
# daemon sets from the container's first-class ip_forward field, and
# the connected routes for each attached subnet are installed by the
# address assignment itself (src/container_net.c). So the only thing
# this image has to carry that the baseline does not is keepalived,
# which owns the one job the kernel cannot do alone -- deciding which
# of the two routers currently answers for the virtual address.
#
# keepalived's own pkg_depends (openssl, libnl, libnftnl, libmnl,
# iptables, ipset) come in behind it and are not restated here: a
# manifest names what this image is FOR, and the resolver already
# knows what that needs. Restating them would be a second, drifting
# copy of keepalived's own dependency declaration.
#
# No iproute2, no shell tooling for "debugging". This project talks to
# the kernel over rtnetlink and never shells out to ip(8), so a router
# image carrying iproute2 would be carrying it for a human who has no
# shell on the host anyway -- and every package in an image is surface
# that has to be built, rebuilt and trusted. The container's console
# (ADR-0248) is how you get inside one of these.
#
# glibc is named explicitly for the same reason chrony's own image
# recipe names it: pkg_seed_default_image_libc() seeds only the default
# image, so a named image gets exactly what its manifest names.
# Rolling, because this platform is rolling-release by charter and a
# pin here would quietly hold the routers back from a libc fix every
# other image took.
#
image_packages="glibc:rolling:2.44 keepalived:rolling:2.3.4"
