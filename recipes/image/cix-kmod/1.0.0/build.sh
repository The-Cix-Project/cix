#
# cix-kmod -- the kernel module tools the HOST needs, and nothing else
# (#347, same convention as cix-firmware and cix-hosttools).
#
# Like cix-firmware, this image is not something a container ever runs.
# It exists so `cixd` has a well-known place to read modprobe from when
# it assembles a control-plane root: spawn_cix_bootroot_assembly()
# resolves KMOD_IMAGE's current version, takes its own usr/bin, and
# hands that to mkbootroot as the kmod tool directory to stage. Purely
# additive, exactly like the other two -- a box that never builds this
# image is unaffected, because mkbootroot treats an empty kmod argument
# as "skip".
#
# Why an image of its own, rather than adding kmod to cix-hosttools:
# mkbootroot stages this directory with its FLAT copy (every file in
# it, into usr/bin), not the named-absolute-path copy that hosttools
# gets. cix-hosttools' own usr/bin carries curl, tar, perl and bash
# among others, so pointing the flat copy at it would put a shell in
# the control-plane root -- a property this platform deliberately does
# not have. A one-package image's usr/bin holds exactly what kmod
# installs:
#
#   usr/bin/kmod  and modprobe, depmod, insmod, lsmod, modinfo, rmmod,
#                 the last six being symlinks to it. kmod dispatches on
#                 argv[0], and mkbootroot's flat copy uses stat() rather
#                 than lstat(), so each name lands as a real, working
#                 binary instead of a symlink into a directory that does
#                 not exist in the assembled root.
#
# daemon/src/kmod.c shells out to /usr/bin/modprobe and /usr/bin/modinfo
# by compiled-in absolute path, and those two paths are what this image
# puts there. Without it, POST /v1/system/kmod/<name> cannot work on an
# installed host at all -- which was half of #347; the other half was
# the module tree itself, which comes from the kernel package's own
# artifact rather than from here.
#
image_packages="kmod:rolling:34.2"
