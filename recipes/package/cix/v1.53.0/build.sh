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
# Re-pinned from v1.49.0 (task #852/#865): picks up everything through
# v1.53.0, most relevantly ADR-0150's real devpts/PTY mount fix
# (task #865's own "PTY allocation request failed" gap on jumpbox1),
# plus container recipes (ADR-0151), host-auth session listing/revoke
# (ADR-0152), and live single-file container updates (ADR-0153).
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
pkg_version="v1.53.0"
pkg_source="https://osakka:REPLACE_WITH_REAL_TOKEN@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v1.53.0.tar.gz"
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
