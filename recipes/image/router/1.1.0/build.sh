#
# router -- the image cr-1/cr-2 run from: a forwarding router pair with
# a VRRP-managed floating gateway between them, now also speaking RIPv2
# to the management LAN.
#
# Forwarding itself needs NO software: it is net.ipv4.ip_forward inside
# the container's own netns, which the daemon sets from the container's
# first-class ip_forward field, and the connected routes for each
# attached subnet are installed by the address assignment itself
# (src/container_net.c). keepalived owns the one job the kernel cannot
# do alone -- deciding which of the two routers currently answers for
# the virtual address.
#
# bird is the second thing the kernel cannot do alone: advertising the
# services subnet to the management LAN so the rest of the site can
# reach it. Measured before it was added -- the host had no route to
# 192.168.150.0/24 at all, and every address behind the routers timed
# out while the routers' own management addresses answered in 0.05 ms.
#
# bash is here for a real reason, and NOT the one the 1.0.0 comment
# ruled out. That comment refused "shell tooling for debugging" and
# still stands: nothing here is for a human poking around. But this
# image now runs TWO daemons, and this project's way of supervising two
# peers in one container is a start script that backgrounds both and
# `wait -n`s, so either exiting takes the container down as a unit and
# the restart policy brings both back together (jump does the same for
# sshd and nslcd). That script needs a shell to be its PID 1. Without
# it the container dies instantly with
# `execve(/usr/bin/bash): No such file or directory` -- which is
# exactly how this was found.
#
# Each package's own pkg_depends come in behind it and are not restated
# here (bird pulls ncurses and readline; keepalived pulls openssl,
# libnl, libnftnl, libmnl, iptables, ipset). A manifest names what this
# image is FOR, and the resolver already knows what that needs.
# Restating them would be a second, drifting copy.
#
# Still no iproute2. This project talks to the kernel over rtnetlink and
# never shells out to ip(8); the container's console (ADR-0248) is how
# you get inside one of these.
#
# Rolling throughout, because this platform is rolling-release by
# charter and a pin here would quietly hold the routers back from a
# libc fix every other image took.
#
image_packages="glibc:rolling:2.44 keepalived:rolling:2.3.4 bird:rolling:2.19.1 bash:rolling:5.2.37"
