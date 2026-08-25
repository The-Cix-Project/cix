#
# syslog -- the image syslog-1/syslog-2 (ADR-0127) run from: sysklogd,
# this platform's own reference syslog-forwarding receiver.
#
# 1.1.0: switches sysklogd from pinned to rolling, matching Cix's
# own rolling-release charter (CLAUDE.md's opening line) -- see
# recipes/image/dns/1.1.0/build.sh's own header comment for the full
# reasoning, identical here. See recipes/README.md for what "version"
# means for an image recipe (ADR-0149) versus an image's
# content-addressed build version (ADR-0108).
#
image_packages="sysklogd:rolling:2.7.0"
