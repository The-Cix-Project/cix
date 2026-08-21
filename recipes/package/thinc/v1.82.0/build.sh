#
# thinc -- self-builds thincd/thincctl (and stages the web
# dashboard) from this project's own self-hosted git remote (ADR-0057),
# closing Part 3 of the self-hosting plan: an operator with no separate
# dev machine can rebuild both the kernel (kernel.recipe, ADR-0056) and
# the control plane itself, entirely on-box.
#
# A hostbuild recipe (ADR-0056): pkg_install() places its output at
# fixed, well-known filenames ($PKG_DESTDIR/thincd, .../thincctl,
# .../web/) rather than merging into any container image's rootfs.
# --build-image= must be a real image with tcc + make + libc-dev
# already installed (a "thinc-builder" image, built up via ordinary
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
# other recipe's pkg_source) -- thinc.recipe itself is deliberately
# NOT part of the tagged snapshot it fetches, the same way
# kernel.recipe's own pkg_source (a Linux kernel tarball) obviously
# doesn't contain kernel.recipe either.
#
# (See v1.76.0's own header for the full re-pin history through that
# version -- ADR-0157 concurrency, ADR-0165/0166 cgroup diagnostics,
# ADR-0168 capability drop, issue #18 recipe capture, issue #22 create-
# form fields, ADR-0169 swap placement, ADR-0173 disk unmount, ADR-0172
# hostapd rebuild, ADR-0171 https default, issue #34's real workdir
# project-quota fix, ADR-0175 keep_on_failure.)
#
# Re-pinned again to v1.77.0 (ADR-0177, issue #46): pkg resume -- a new
# POST /v1/pkg/resume continues a keep_on_failure-preserved build
# container (ADR-0175) in place under a fixed recipe version, without
# re-fetching or re-extracting its source tree, eliminating the real,
# repeated cost of restarting a multi-hour bootstrap build from scratch
# after every recipe fixup. Built specifically to continue debugging
# issue #32's own gcc bootstrap on this exact box without paying for
# another full restart, deployed here to actually do that.
#
# Re-pinned again to v1.78.0 (ADR-0178): url_basename() didn't strip a
# URL's query string, breaking every extra-source recipe using the
# git-raw-file ?ref=<commit> pkg_source pattern (kernel.recipe's own
# config-file fetch) -- found live the first time that exact mechanism
# was ever actually exercised end-to-end, deploying kernel/6.18.40-3.
#
# Re-pinned again to v1.82.0 (quality sprint wave 1): limits fully
# visible + strict -- cpuset_cpus/disk_quota_bytes read back in GET
# (#49), unknown create/recipe fields are a 400 naming the offender
# (#68), thincctl ps grows cpuset/disk_quota/caps columns (#48), the
# dashboard's container Hardware tab carries the full resource
# envelope (#69); plus last_output_seconds_ago on building pkg rows
# (#58) and boot-time reconciliation of rows stranded mid-fetch/build
# by a daemon death (#56).
#
# Re-pinned again to v1.81.0 (ADR-0180, issue #67): asynchronous
# container teardown -- DELETE/stop of a running container no longer
# runs an unbounded waitid() inside the single epoll loop (the wedge
# that froze this box's whole control plane twice in one day, both
# times needing a hard reset); durable intent lands synchronously, the
# SIGKILL is sent without waiting, and the existing crash-detection
# reactor completes teardown when the process actually dies. Also the
# unconditional thaw-before-kill hardening and the transient
# "deleting"/"stopping" statuses.
#
# Re-pinned again to v1.80.0: (1) the rolling-candidate badge now marks
# the daemon's real ADR-0107 resolution instead of the newest-published
# row -- the created_at shortcut shipped in v1.79.0 lied for
# out-of-order version histories (gcc: badge sat on 6.4.0-5 while the
# daemon resolves 16.2.0-5; caught by the user asking what the
# selection criteria actually was), and the Recipe tab could show one
# version's number over another version's content the same way. (2)
# Real build naming: the Makefile's new THINC_VERSION override
# (passed below -- this recipe knows exactly which tag it fetched)
# replaces the "unknown" every on-box build used to stamp, since a
# Gitea archive tarball has no .git for git describe to read
# (user-reported via `thincctl boot`).
#
# Re-pinned again to v1.79.0: ships the web dashboard's package
# Versions-tab fix (per-version "View..." action + a "(rolling
# candidate)" badge on the newest row -- a user-reported navigation
# gap) plus the Part 201 documentation (the "Debugging a remote build
# in depth" guide section and CHANGELOG record of the toolchain-
# bootstrap investigation). No daemon/CLI code changes since v1.78.0
# -- every other commit in the range is recipe-catalog or docs work.
#
# Auth: the itdlabs org this repo lives under has "limited" visibility
# (signed-in users only) even for an individually-public repo, and
# this repo is kept private -- confirmed both must hold for a fetch to
# 401/404 unauthenticated, so a plain anonymous URL doesn't work here.
# curl (this daemon's own fetch mechanism, host-side, before any build
# container starts) supports HTTP basic auth embedded directly in the
# URL, which gitea accepts with a scoped access token as the password
# field -- no daemon/pkg.c code change needed. 6de349585e160690ecbdebbfa6161a99fa4d6c8b
# below is a deliberate placeholder, never a real committed secret
# (same posture this project's own installer signing key already
# establishes, image/keys/thinc-signing.key -- gitignored, must exist
# locally, never reproduced by a build step): generate a real,
# read-only, repository-scoped token (Settings -> Applications ->
# Generate New Token, scope "read:repository") for the account this
# daemon should fetch as, and substitute it here -- via `pkg recipe
# add`, not by committing a real token into this file.
#
pkg_name="thinc"
pkg_version="v1.82.0"
pkg_source="https://osakka:6de349585e160690ecbdebbfa6161a99fa4d6c8b@git.home.arpa/api/v1/repos/itdlabs/thinc/archive/v1.82.0.tar.gz"
pkg_sha256="667c9adc62704a32ce524ae0e29f144afaa60e4efc8826acdb5b266c23fc9ed5"
pkg_depends=""

#
# build/mkbootroot is built here too, alongside thincd/thincctl, not
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
# build/thinc-install and build/mkinstalleriso join the same round
# for the identical reason (ADR-0064): closing the REST-driven-ISO-
# assembly gap ADR-0063 deliberately left open needs a real, on-box
# mkinstalleriso binary (the daemon's own POST /v1/system/iso handler
# execve()s it server-side, same shape as spawn_thinc_bootroot_
# assembly() already established) and a real thinc-install binary to
# embed into the ISO it produces -- neither has ever had a reason to
# be staged onto a running system before now either.
#
# build/thinc-recover joins the same round (ADR-0146): mkinstalleriso
# takes it as a required argument (image/src/mkinstalleriso.c), so a
# self-built ISO needs it staged here to produce a recovery-capable
# image, exactly like thinc-install already is.
pkg_build() {
	make THINC_VERSION="$pkg_version" \
	    build/thincd build/thincctl build/mkbootroot build/thinc-install build/thinc-recover build/mkinstalleriso
}

pkg_install() {
	cp build/thincd build/thincctl build/mkbootroot build/thinc-install build/thinc-recover build/mkinstalleriso \
	   "$PKG_DESTDIR/"
	mkdir -p "$PKG_DESTDIR/web"
	cp -r web/* "$PKG_DESTDIR/web/"
}
