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
# Re-pinned again to v1.99.7 (#118): a package file replaces a symlink
# instead of writing through it -- gcc's usr/bin/c++ landed on a
# Debian-alternatives link and the open followed it. The failure was
# the lucky case; a live target would have been overwritten silently.
# Plus: the exec endpoint refuses an over-long argv entry instead of
# truncating it and running the result.
#
# Re-pinned again to v1.99.6: observability. A stalled build reports
# itself with what every process in its container is blocked on;
# /v1/system/processes carries state and wchan; exit 143 is no longer
# misreported as "overlay mount or exec failed"; merge_tree() names the
# file and errno it failed on (#118).
#
# Re-pinned again to v1.99.5 (#109): a command that fails inside a
# recipe fails the package. Without set -e, libcap was recorded as
# installed with four of its binaries missing because `make install`
# died and the `rm -rf` after it succeeded.
#
# Re-pinned again to v1.99.4 (#109): a declared build tool resolves to
# its NEWEST installed version, compared the way a person reads a
# version -- libc-dev resolved to 2.36 (no libm.so) while 2.36-3 sat
# installed elsewhere, and the build died looking like a broken
# toolchain.
#
# Re-pinned again to v1.99.3 (#109): a build environment holds each
# declared tool AND that tool's own runtime dependencies -- ar arrived
# without libz and could not start.
#
# Re-pinned again to v1.99.2 (#109): a declared build tool resolves to
# an image that actually exists -- bash matched a row naming the long-
# gone cix-builder image while sitting installed in three healthy
# ones.
#
# Re-pinned again to v1.99.1 (#109/#40): a composed build environment
# that merely EXISTS is no longer mistaken for one that was filled --
# a failed composition used to leave an empty image behind that every
# later build accepted, then died at execve(/usr/bin/bash) on nothing.
# And the flat sandbox migration finally works: it was failing on a
# merged-/usr shape difference (/lib symlink vs real /lib directory),
# not a content conflict.
#
# Re-pinned again to v1.99.0 (#109, cont.): every failure branch of a
# build actually fails the package -- three of five wrote the error
# string and left the state at "building" forever -- and the shared
# copy-forward path (image_produce_new_version()) names its failing
# step and errno instead of returning a bare -1 from nine silent exits.
# Deployed specifically to diagnose why v1.98.0 could not compose a
# build environment on this box.
#
# Re-pinned again to v1.98.0: recipes declare their own build tools
# (#109, ADR-0199) -- pkg_build_depends names the exact tools a build
# needs, and the build container is composed from precisely those
# packages' own recorded file lists instead of the shared, accreted
# sandbox that had grown to 79,438 files of which two thirds were an
# undeclared Rust+Go toolchain nothing declared or wanted. Plus the
# contained DHCP service (ADR-0197), zswap control (ADR-0196), and
# per-container swap limits.
#
# Re-pinned again to v1.97.0 (quality sprint wave, cont.): {{REPO_TOKEN}}
# pkg_source substitution (#60 -- self-fetching recipes committable in
# final form, no more live-token dance; NOTE this recipe still uses the
# REPLACE_WITH_REAL_TOKEN placeholder because the DEPLOYING daemon
# (v1.83.0) predates #60 -- v1.85.0 onward can use {{REPO_TOKEN}}) and
# per-network auto-IP allocation window + management .1-skip (#70).
#
# Re-pinned again to v1.83.0 (quality sprint wave 2/3): LDAP client
# login centralization fully closed (#66) -- /ldap/config stores
# URI/base-DN/bind-DN/write-only credential, recipes resolve {{LDAP:*}}
# tokens, and `ldap_login: true` auto-stages nsswitch/nslcd from that
# config with URI auto-derivation from registered servers; plus user-
# namespace subordinate-ID allocator (#29 phase 1, subid.c) wired and
# tested (the CLONE_NEWUSER flip itself stays gated on bare-metal
# verification).
#
# Re-pinned again to v1.82.0 (quality sprint wave 1): limits fully
# visible + strict -- cpuset_cpus/disk_quota_bytes read back in GET
# (#49), unknown create/recipe fields are a 400 naming the offender
# (#68), cixctl ps grows cpuset/disk_quota/caps columns (#48), the
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
# Real build naming: the Makefile's new CIX_VERSION override
# (passed below -- this recipe knows exactly which tag it fetched)
# replaces the "unknown" every on-box build used to stamp, since a
# Gitea archive tarball has no .git for git describe to read
# (user-reported via `cixctl boot`).
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
# field -- no daemon/pkg.c code change needed. The credential itself is
# never in this file: {{REPO_TOKEN}} below is substituted at fetch time
# from the daemon's own stored repo token (#60, `cixctl pkg repo-
# config set --token=...`), so this recipe is committable exactly as
# written and the token never reaches the catalog or any log. (Versions
# through v1.97.0 carried a REPLACE_WITH_REAL_TOKEN placeholder needing
# a live substitution dance instead -- the deploying daemon there
# predated #60.)
#
pkg_name="cix"
pkg_version="v2.1.5"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.1.5.tar.gz"
pkg_sha256="c5dd69c4fb09f8ac69c07780c534913e1e9554ea2afefa456f895dfb176e2f1e"
#
# The artifact tier (ADR-0122/ADR-0201) matters more for this recipe
# than for any other one here, and for a reason specific to it: a host
# that needs a cixd update is, by definition, running the cixd it is
# trying to replace. Building from source on the box requires that
# older daemon to fetch pkg_source over HTTPS by hostname -- which is
# exactly what a host with a broken resolver (#138) cannot do, and a
# host with a broken resolver is a prime candidate for needing an
# update. The artifact comes from the configured cache instead, which
# is reachable by literal IP, so recovering such a host does not
# depend on the thing that is broken on it.
#
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="ef592cec98e4e596e6ca543b659c791fbad64874926bf709636c5eb09c086c6d"
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
	make CIX_VERSION="$pkg_version" \
	    build/cixd build/cixctl build/mkbootroot build/cix-install build/cix-recover build/mkinstalleriso
}

pkg_install() {
	cp build/cixd build/cixctl build/mkbootroot build/cix-install build/cix-recover build/mkinstalleriso \
	   "$PKG_DESTDIR/"
	mkdir -p "$PKG_DESTDIR/web"
	cp -r web/* "$PKG_DESTDIR/web/"
}
