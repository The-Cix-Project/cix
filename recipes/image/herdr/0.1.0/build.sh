#
# herdr -- a container image for running coding agents (herdr.dev), the
# runtime platform that keeps claude-code / codex / other agent CLIs
# alive across disconnects. Built up in toolchain order:
#
#   0.1.0 (this): the Node.js runtime plus the base an agent CLI needs
#                 -- bash, coreutils, git, a terminal, and CA certs for
#                 the HTTPS the agents call. claude-code (an npm package)
#                 and herdr/codex (Rust) land in later revisions, once
#                 the Node and Rust toolchains this platform is growing
#                 are both in the cache (Build Provenance: every one is
#                 built on a Cix host, no prebuilt binary).
#
image_packages="bash:pinned:5.2.37-5 coreutils:pinned:9.11-7 ncurses:pinned:6.6-8 git:pinned:2.55.0-8 ca-certificates:pinned:2026.09.03-1 node:pinned:24.21.0-1"
