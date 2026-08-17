#
# chrony -- the image ntp-1/ntp-2 run from (ADR-0119, ADR-0168's own
# cap_add: ["CAP_SYS_TIME"] container-level need). Captured live from
# 192.168.15.95's own real, organic `pkg install --image=chrony`
# history (issue #18), not hand-authored.
#
# libc-dev is a real, deliberate dependency here, not build-tooling
# leftover: chrony.recipe pins CC=tcc via a thin tcc-nopthread wrapper
# (CLAUDE.md's own Environment notes -- TCC mishandles the -pthread
# driver flag), and needs a real libc-dev present in this image for
# that build to link against.
#
image_packages="libc-dev:rolling:2.36 chrony:rolling:4.8"
