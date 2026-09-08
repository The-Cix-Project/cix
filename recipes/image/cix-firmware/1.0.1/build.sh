#
# 1.0.1: rtw88-firmware 1 -> 2. Revision 1's header gate called `grep`,
# which that recipe did not declare, so it failed on the missing tool
# and reported the firmware as bad while printing a header that was
# correct. The blob never changed; only the check did.
#
#
# cix-firmware -- the device firmware blobs the HOST kernel loads, and
# nothing else (#30, extending ADR-0029).
#
# This image is not something a container ever runs. It exists purely
# so `cixd` has a well-known place to read firmware from when it
# assembles a control-plane root: spawn_cix_bootroot_assembly() resolves
# FIRMWARE_IMAGE's current version, takes its own lib/firmware, and
# hands that to mkbootroot as the firmware root to stage. The same
# convention, and the same purely-additive posture, as cix-hosttools
# (ADR-0078) -- a box that never builds this image is unaffected,
# because mkbootroot treats an empty firmware argument as "skip".
#
# Why the host root and not a container: request_firmware() is answered
# by the kernel during driver probe, out of the root filesystem the
# kernel booted, before any container exists. That is ADR-0029's
# original argument for staging amdgpu firmware into the control-plane
# root and it is unchanged; what changed is that a second device wanted
# a blob, so the mechanism became one recursive copy of a mirrored tree
# instead of one hardcoded subdirectory per driver.
#
# Both packages install under lib/firmware in the layout
# request_firmware() expects, so the image's own lib/firmware IS a
# faithful /lib/firmware and needs no rearranging:
#
#   rtw88-firmware    lib/firmware/rtw88/rtw8822b_fw.bin -- the RTL8822BU
#                     the site's USB adapter reports (rtw8822bu.c carries
#                     USB_DEVICE_AND_INTERFACE_INFO(0x2357, 0x012e), and
#                     rtw8822b.c:2614 declares exactly this one blob).
#                     Without it rtw88 probes, finds no firmware and
#                     brings up no interface.
#   wireless-regdb    lib/firmware/regulatory.db and regulatory.db.p7s --
#                     the regulatory database cfg80211 loads to know
#                     which channels and powers are legal where. An AP
#                     with no regdb is confined to the most restrictive
#                     world domain, which on 5 GHz means effectively no
#                     usable channels.
#
# The .p7s signature ships alongside deliberately. CFG80211_REQUIRE_SIGNED_REGDB
# is on in this platform's kernel and cannot be turned off from .config
# (its prompt is conditional on CFG80211_CERTIFICATION_ONUS, so
# olddefconfig restores its default y), so cfg80211 verifies the
# database against a key built into the kernel. wireless-regdb.recipe
# regenerates regulatory.db from db.txt and refuses to install unless
# the result is byte-identical to upstream's, which is what makes
# shipping upstream's signature over this platform's own build correct
# rather than a mismatch waiting to happen.
#
# rtw88-firmware is the one binary blob in this platform that is not
# built from source, because it is not buildable from source by anyone
# -- it is a signed image executed by the adapter's own processor. It is
# pinned by content: the recipe fetches one linux-firmware commit, its
# checksum covers those exact bytes, and pkg_build() asserts the
# 8822B header before installing.
#
image_packages="rtw88-firmware:rolling:2 wireless-regdb:rolling:2026.09.03"
