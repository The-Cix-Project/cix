#
# cix -- self-builds cixd/cixctl (and stages the web
# dashboard) from this project's own self-hosted git remote (ADR-0057),
# closing Part 3 of the self-hosting plan: an operator with no separate
# dev machine can rebuild both the kernel (kernel.recipe, ADR-0056) and
# the control plane itself, entirely on-box.
#
# A hostbuild recipe (ADR-0056): pkg_install() places its output at
# fixed, well-known filenames ($PKG_DESTDIR/cixd, .../cixctl,
# .../web/) rather than merging into any container image's rootfs.
# --build-image= must be a real image with tcc + make + libc-dev
# already installed (a "cix-builder" image, built up via ordinary
# `pkg install` first -- see tcc.recipe) -- no gcc/binutils/autotools
# needed, this is a plain Makefile build with no configure step, and
# the root Makefile already hardcodes `CC := tcc` (ADR-0001), so no
# CC= override is needed here either.
#
# pkg_source is this repo's own self-hosted gitea instance's REST
# archive-download endpoint (confirmed empirically, ADR-0057 --
# gitea's usual /owner/repo/archive/<ref> *web* path 404s on this
# instance behind whatever reverse proxy fronts it; only /api/v1/...
# paths are reachable). <TAG> is a real, explicit git tag cut from a
# known-good, already-verified commit (this project's own established
# "pinned version, never floating main" convention, matching every
# other recipe's pkg_source) -- cix.recipe itself is deliberately
# NOT part of the tagged snapshot it fetches, the same way
# kernel.recipe's own pkg_source (a Linux kernel tarball) obviously
# doesn't contain kernel.recipe either.
#
# Re-pinned from v1.54.0 (ADR-0157, tasks #876-878): the real deploy
# target had drifted five real tags behind (v1.55.0 through v1.60.0,
# never before deployed) -- v1.60.0 brings kernel-module/sysctl REST
# management (ADR-0159/0160), USB composite-device metadata + hotplug
# (ADR-0161), and the full four-phase parallel-package-builds design
# (ADR-0157: concurrency raised 1->10, GET/PUT /v1/system/pkg-build-
# config, and a byte-identical concurrent-vs-serial merge-back proof).
#
# Re-pinned again to v1.61.0 (Phase 5 completion + the real ADR-0165
# production regression investigation, ROADMAP Part 165/168): carries
# real cgroup_create() per-failure diagnostics (needed live on
# 192.168.15.95 to actually root-cause that regression -- the box has
# no shell, so this is the only way to see it), plus the container
# resource-limit visibility fix (Part 163) and the container resource
# limits themselves (ADR-0165) for the first time on this exact box.
# Also the first real exercise of this recipe's own intended pkg sync
# workflow: this file itself synced from git via `pkg sync`, only the
# final real-token substitution below still needs a direct `pkg
# recipe add` (unavoidable -- see this file's own Auth section, a
# real access token is never committed).
#
# Re-pinned again to v1.62.0: v1.61.0's own diagnostics deployed and
# reproduced the regression with zero new log output, ruling
# cgroup_create() itself out (it's succeeding) -- this round adds the
# same perror()-per-step treatment to container_create()'s every
# other pre-fork failure path (bpf_attach, both pipe calls, ns_clone3,
# the two post-fork network-setup paths), needed for the same reason.
#
# Re-pinned again to v1.63.0: v1.62.0's own diagnostics ALSO produced
# zero log output, and the real reason turned out to be a methodology
# bug, not a wrong failure point -- perror() writes to real stderr,
# which nothing mirrors back into the queryable log store (only the
# reverse: logstore_write() mirrors OUT to stderr for boot
# visibility). Fixed properly this round: the actual logging moved to
# handle_pkg_fetch_event() in main.c (the one place in this whole
# call chain that links logstore.c), using strerror(errno) preserved
# correctly through registry_create()/container_create()'s own
# already-correct errno handling.
#
# Re-pinned again to v1.64.0: v1.63.0's own fix worked -- a real
# diagnostic finally reached the log store ("container_create failed:
# No such file or directory") -- but only at the outer, one-message-
# for-the-whole-function granularity, not which of container_create()'s
# several distinct syscalls actually produced ENOENT. This round adds
# container_create_last_error_step(), a precise parent-side equivalent
# of the existing child-side diag_pipe mechanism -- which pinpointed
# the real root cause (a missing CONFIG_CFS_BANDWIDTH kernel Kconfig
# symbol, ADR-0166; the kernel rebuild itself is deferred separately).
#
# Re-pinned again to v1.65.0: unrelated to the investigation above --
# carries the web dashboard's own real-ANSI-color log rendering fix
# (ROADMAP Part 169), reported directly by the user.
#
# Re-pinned again to v1.66.0: found immediately after deploying
# v1.65.0 -- pkg-build-config's own cpu_max silently reverted to its
# default on this exact reboot, despite having been explicitly
# cleared to unlimited moments earlier (the real, active mitigation
# this project relies on pending ADR-0166's kernel fix). Fixed
# (ROADMAP Part 170) and verified via a real restart cycle before
# this re-pin.
#
# Re-pinned again to v1.67.0: carries two more web dashboard fixes,
# both reported directly -- the log panel's collapse toggle not
# actually shrinking the pane (Part 172), and a menu-bar cleanup pass
# (tree-duplicated nav entries removed, dropdown arrows removed, logo
# text/size, plus a real hidden-attribute CSS bug found and fixed
# along the way, Part 173).
#
# Auth: the itdlabs org this repo lives under has "limited" visibility
# (signed-in users only) even for an individually-public repo, and
# this repo is kept private -- confirmed both must hold for a fetch to
# 401/404 unauthenticated, so a plain anonymous URL doesn't work here.
# curl (this daemon's own fetch mechanism, host-side, before any build
# container starts) supports HTTP basic auth embedded directly in the
# URL, which gitea accepts with a scoped access token as the password
# field -- no daemon/pkg.c code change needed. REPLACE_WITH_REAL_TOKEN
# below is a deliberate placeholder, never a real committed secret
# (same posture this project's own installer signing key already
# establishes, image/keys/cix-signing.key -- gitignored, must exist
# locally, never reproduced by a build step): generate a real,
# read-only, repository-scoped token (Settings -> Applications ->
# Generate New Token, scope "read:repository") for the account this
# daemon should fetch as, and substitute it here -- via `pkg recipe
# add`, not by committing a real token into this file.
#
pkg_name="cix"
pkg_version="v1.67.0"
pkg_source="https://osakka:REPLACE_WITH_REAL_TOKEN@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v1.67.0.tar.gz"
pkg_sha256="REPLACE_WITH_REAL_SHA256"
pkg_depends=""

#
# build/mkbootroot is built here too, alongside cixd/cixctl, not
# just those two: the daemon's own server-side assembly step (ADR-0057
# -- triggered here, never CLI-invoked, per the API-First Mandate)
# needs a real mkbootroot binary to package this exact hostbuild's own
# artifacts into a fresh control-plane squashfs, and a real deployed
# box has never had any reason to carry one before now (mkbootroot has
# only ever been a dev-machine build tool, never staged onto a running
# system). Self-contained, no bootstrap-order dependency on some
# earlier round's own mkbootroot still being present anywhere: this
# round's freshly built copy assembles this exact round's own output.
#
# build/cix-install and build/mkinstalleriso join the same round
# for the identical reason (ADR-0064): closing the REST-driven-ISO-
# assembly gap ADR-0063 deliberately left open needs a real, on-box
# mkinstalleriso binary (the daemon's own POST /v1/system/iso handler
# execve()s it server-side, same shape as spawn_cix_bootroot_
# assembly() already established) and a real cix-install binary to
# embed into the ISO it produces -- neither has ever had a reason to
# be staged onto a running system before now either.
#
# build/cix-recover joins the same round (ADR-0146): mkinstalleriso
# takes it as a required argument (image/src/mkinstalleriso.c), so a
# self-built ISO needs it staged here to produce a recovery-capable
# image, exactly like cix-install already is.
pkg_build() {
	make build/cixd build/cixctl build/mkbootroot build/cix-install build/cix-recover build/mkinstalleriso
}

pkg_install() {
	cp build/cixd build/cixctl build/mkbootroot build/cix-install build/cix-recover build/mkinstalleriso \
	   "$PKG_DESTDIR/"
	mkdir -p "$PKG_DESTDIR/web"
	cp -r web/* "$PKG_DESTDIR/web/"
}
