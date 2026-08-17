#
# dev -- the general-purpose build-tooling image (binutils, m4, and
# soon gcc once its own bootstrap succeeds -- ADR-0166's kernel rebuild
# depends on it). Captured live from 192.168.15.95's own real, organic
# `pkg install --image=dev` history (issue #18), not hand-authored.
#
# gcc itself is deliberately NOT included yet: its own bootstrap build
# was still in progress (ADR-0168's Part 180 build, and the earlier
# genpreds/libgcc_s.so.1 segfault investigation) at the time this
# recipe was captured. Bump this recipe to 1.1.0 adding
# "gcc:pinned:16.1.0" once that build genuinely succeeds and is
# verified (compiles a real C and C++ program) -- not before.
#
image_packages="binutils:rolling:2.42-2 m4:rolling:1.4.19"
