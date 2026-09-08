#
# wifi_router -- the image ar-1 runs from: a wireless access point.
#
# hostapd is the whole job. An AP is one daemon that owns a wireless
# interface, authenticates clients and bridges them onto a network; the
# kernel does the rest, exactly as it does for the `router` image, whose
# forwarding needs no software at all.
#
# WHAT THIS IMAGE DOES NOT CONTAIN, AND WHY.
#
# No bash. The `router` image carries one because two daemons used to be
# supervised by a `wait -n` start script, and that reason is gone:
# ADR-0260 made cix-init pid 1 in every container and services a
# declaration, so nothing here needs a shell to be its own init. An AP
# is a single service in any case.
#
# No iproute2 and no wireless-tools. This project talks to the kernel
# over rtnetlink and never shells out to ip(8), and the wireless
# interface arrives already moved into this container's own netns by the
# runtime (container.h's `interfaces[]`, container_net.c's
# rtnl_link_set_netns_pid) rather than being configured from inside.
#
# iw IS here, and it is the exception worth stating. hostapd drives the
# radio over nl80211 by itself and does not need iw to run -- but an AP
# that is not working is diagnosed by asking the radio what it thinks
# it is doing (`iw dev`, `iw phy info`, regulatory domain, channel
# survey), and ADR-0261 means a container can only be debugged with
# what it declares. Shipping iw is choosing, at build time, to be able
# to answer that question later; leaving it out means finding out at
# the worst moment that you cannot. That is the trade ADR-0261 names
# explicitly as the cost of removing free-text exec, and this is the
# first image to pay it deliberately rather than by accident.
#
# hostapd's own pkg_depends (libnl, openssl) come in behind it and are
# not restated here -- a manifest names what this image is FOR, and the
# resolver already knows what that needs. Restating them would be a
# second, drifting copy.
#
# Rolling throughout, because this platform is rolling-release by
# charter and a pin here would quietly hold the AP back from a libc fix
# every other image took.
#
# NOT YET PROVEN ON HARDWARE. The kernel this image runs under carries
# no wireless support at all -- image/kernel/qemu-part1.config has zero
# CFG80211/MAC80211/WLAN symbols, which is why the site's USB adapter
# (2357:012e, "Realtek 802.11ac NIC") binds to the generic usb driver
# and no wlan interface exists. This image is the userspace half; the
# kernel half is tracked in #30 and the driver is being measured rather
# than guessed (recipes/package/probe-wifi-driver). Building this image
# is useful before that lands -- it proves the packages compose -- but
# an ar-1 container cannot associate a client until the kernel can see
# the radio.
#
image_packages="glibc:rolling:2.44 hostapd:rolling:2.11 iw:rolling:6.17"
