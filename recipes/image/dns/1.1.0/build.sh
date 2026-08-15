#
# dns -- the image dns-1/dns-2 (ADR-0089/0091) run from: a single
# dnsmasq install, real DNS/DHCP server (daemon/src/dns.c's own doc
# comment already assumes exactly this).
#
# 1.1.0: switches dnsmasq from pinned to rolling. thinC's own charter
# (CLAUDE.md's opening line) calls this "a custom, rolling-release
# hardware and workload orchestration platform" -- rolling is the
# default posture, not pinned; 1.0.0's blanket "pinned" was captured
# from what happened to be installed at the time, not a deliberate
# policy choice, and was corrected once asked about directly. See
# recipes/README.md for what "version" means for an image recipe
# (this file's own revision history, ADR-0149) versus an image's
# content-addressed build version (ADR-0108) -- unrelated axes.
#
image_packages="dnsmasq:rolling:2.90"
