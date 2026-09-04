#
# v2.41.0 -- a container can opt into samepage merging (#260): ksm true on
# create, applied with prctl(PR_SET_MEMORY_MERGE) before exec. The half
# that makes the v2.40.0 scanner actually save anything.
#
#
# v2.40.0 -- GET/PUT /v1/system/ksm and cixctl ksm: the host half of
# samepage merging (#50), off by default and explicit that the container
# opt-in does not exist yet, so enabling the scanner alone costs CPU and
# saves nothing.
#
#
# v2.37.0 -- leftover cgroups are a TREE, so remove them depth-first.
# v2.35.0 removed only immediate children, rmdir refused every one of
# them, and the build container could still not start.
#
#
# v2.35.0 -- a reused container cgroup is reset to a clean leaf before a
# process is cloned into it. Without this, one nested run left
# subtree_control set on a build slot's cgroup and every later build in
# that slot failed with ns_clone3: Device or resource busy.
#
#
# v2.33.0 -- a nested cixd can delegate cgroup controllers (#258). The
# cgroup v2 no-internal-process rule blocked it: the daemon sat in its own
# namespace root, so that root could never enable controllers for children.
# It now moves itself into a leaf and retries.
#
#
# v2.32.0 -- containers that run containers get a cgroup2 mount (#257).
# Without it a nested cixd had no cgroup tree to create under, which was
# 25 of the 48 failures the first full suite run found.
#
#
# v2.31.0 -- cixctl completion reaches the end of a command (#151) and
# output goes through a pager (#152). The command surface is now a table,
# cli/src/cmdtree.h, guarded by test_clitree, which re-derives it from
# main.c and fails the build if the two disagree -- that check immediately
# found the hand-kept shell list had drifted to 59 entries against 62
# routed commands.
#
#
# v2.30.0 -- a disk is identified by its filesystem UUID rather than its
# kernel name (#255, ADR-0236), and a placement whose disk is missing
# degrades instead of refusing to boot (#256, ADR-0237). Both found by
# taking this box down: #242's kernel renamed sda to sdb, every record
# naming it went stale at once, and cixd would not start on a host with
# no shell to recover through.
#
#
# v2.29.0 -- the dashboard works on a phone (#233). The mobile CSS sat
# ahead of the rules it overrides, and a media query adds no specificity,
# so most of it never applied: opening the dashboard on a phone showed
# about twenty rows of navigation tree and no content at all. The blocks
# move to the end of the stylesheet, the tree goes behind a Browse
# disclosure, and the status bar's ellipsis finally works (a flex item
# will not shrink below its content without min-width: 0 -- the same bug
# at desktop width, with room to hide it).
#
#
# v2.28.2 -- v2.28.1 did not compile: the new 409 branch called a function
# that does not exist in main.c. Same change, corrected.
#
#
# v2.28.1 -- the partition carrying the data directory is refused an unmount
# by rule rather than by luck (#249). diskformat_unmount() had no rule about
# it and relied on umount2(2) returning EBUSY, which happens only because the
# daemon holds files open under its own data directory -- a coincidence of
# the moment. An idle daemon would have let the unmount succeed, leaving the
# control plane running against whatever empty directory the mountpoint was
# covering, on a host with no shell. Also corrects the storage guide, which
# told operators cix-containers could be unmounted like any other partition.
#
#
# v2.28.0 -- build capacity can no longer be lost (#246, ADR-0235). A chain
# slot was free when its name was the empty string, and clearing it was an
# action better than twenty separate assignments had to remember, with no
# chain_free() for the compiler to enforce. A path returning without
# clearing held its slot for the life of the process. Measured on this box:
# repeated failed builds took it from zero to nine of ten slots held with
# nothing running. One more and every package operation refuses 409 until a
# reboot, on a host with no shell. Capacity is derived from the owning
# package's own state now -- the same predicate issue #98's stale-slot
# discriminator already uses -- and a reclaim logs at warn rather than
# healing silently.
#
#
# v2.15.4 -- a btrfs partition can be grown (#163), which needed btrfs-progs
# 7.1-8: the package shipped ONLY mkfs.btrfs, so a platform whose storage
# substrate is btrfs had nothing on any host that could maintain one. Also
# #239: a stuck fetch can be cancelled instead of holding the chain slot
# forever and making every install 409.
#
# v2.14.6 -- a partition's GPT label could be read off the WRONG DISK (#240):
# disk_enumerate() picked which disk to open from an uninitialised parent_name.
# Invisible until a host had two partitioned disks, because the leftover value
# was always the OS disk. Read-only, so nothing was ever miswritten.
# Also #9: the OS disk's reserved free space is finally usable -- appending a
# partition there was already allowed, but role assignment and format still
# refused it for inheriting is_os_disk from its parent.
#
# v2.14.5 -- starting a rebuild no longer makes everything else wait
# (#238, ADR-0228). Composing a build environment copies every file of every
# declared build tool and ran inline in the event loop: eleven publishes,
# ten at 8-57ms and one at 247,591ms. It runs in a forked child now, watched
# on a pidfd like the fetch child, and the install resumes when it exits.
# The stall store recorded nothing for those four minutes -- the loop kept
# heartbeating while it was unusable, which is #229 with a real case.
#
# v2.14.4 -- a client that stops reading can no longer freeze the daemon
# (#237, ADR-0227). Responses were written blocking, inline in the single
# event loop, so one unauthenticated GET left unread held the whole control
# plane: health went 7ms -> hard timeout for as long as such a client was
# attached, and back to 14ms the instant it closed. Responses are buffered
# and drained on EPOLLOUT now, with a 30s no-progress deadline. This is the
# real cause of the resets that were being chased in the rebuild queue.
#
# v2.9.8 -- a host with no resolvers of its own now borrows its configured
# DNS forwarders (#135), which breaks the fresh-install deadlock: no recipes
# without DNS, no DNS without recipes. The operator's idea; smaller than
# shipping a starter recipe set and it removes the cycle rather than
# routing around it.
#
#
# v2.9.6 -- GET /v1/diskroles no longer reads a superblock off every block
# device to answer a question that never used the result (#215). Third
# endpoint fixed by reading stallwatch's records rather than guessing.
#
#
# v2.9.4 -- v2.9.3's tag was moved after its recipe had been published,
# which broke the checksum the recipe pins. A published recipe names an
# exact tarball, so a git tag is as immutable as a recipe version once one
# references it. Same content, correctly numbered.
#
#
# v2.9.3 -- the build now runs the test suite. Seventeen tests that need
# only a filesystem are built and executed as part of pkg_build(), so a cixd
# that fails its own contract guards cannot become a package (#224). Nothing
# ran any test on a Cix host before this.
#
#
# v2.9.2 -- a deduplicated image rebuild now says so (#218), and a failed
# per-container storage migration now reports why (#172). Both were silent
# correct-looking outcomes, which is the failure mode this session kept
# finding.
#
#
# v2.9.1 -- GET /v1/pkg no longer rescans every recipe directory on the
# event loop. stallwatch attributed 17 of 22 recorded wedges to that one
# endpoint (#215); the drift check now validates against the recipe
# directory's mtime instead of re-parsing 145 build.sh files per request.
#
#
# v2.9.0 -- GET /v1/pkg/drift (#217). available_version answered this
# per package all along; nothing aggregated it, so 34 of 139 installed
# packages had drifted unseen on this very box. Reports only -- the
# verb is still POST /pkg/update-all.
#
#
# v2.8.2 -- the second latent bug the stricter compiler found: four
# files used fixed-width integer types with no <stdint.h> in scope,
# relying on a transitive include that tcc 0.9.28rc resolves
# differently. Found by scanning every .c and .h rather than fixing
# only the one the build happened to reach first.
#
#
# v2.8.1 -- one commit: the tcc upgrade found a real latent bug in our
# own code.
#
# tcc@0.9.28rc-7 (issue #216) is stricter than the 2017 release. The
# forward declaration of artifact_export_start() names
# `enum artifact_export_kind` as a parameter type, and the enum was
# defined 350 lines further down. C has no forward declaration for an
# enum the way it has for a struct, so that tag was an incomplete type
# and a call through it is not valid C. GCC accepts it and tcc 0.9.27
# accepted it, which is why it survived five releases; the new compiler
# reports `cast to incomplete type` and refuses, correctly.
#
#
# v2.8.0 -- 44 commits since v2.7.3, and a new REST endpoint, so a minor
# bump rather than a patch.
#
# The reason this deploy exists: an in-flight package build could not be
# stopped. Two perl builds sat on this box for 110 and 125 minutes each,
# holding both build slots, emitting nothing, with no way to clear them
# short of restarting the daemon. POST /v1/pkg/cancel ends that (#213).
#
# What those perl builds were actually stuck on is fixed here too: the
# recipe's tcc wrapper rewrote a bare -E (preprocess and stop) as though
# it were -Wl,-E (export dynamic symbols), so Configure's preprocessor
# probe was handed a link flag, concluded no C preprocessor existed, and
# stopped to ask a human to name one. Nothing was going to answer
# (#214).
#
# Also carries: the TCC compound-literal fix (#211) and the three-stage
# bootstrap gate that now guards the compiler, nftables and iproute2's
# 'ip vrf' restored on the back of it, six netlink libraries packaged
# from source (#207), the 'dev' image retired (#204), and 87 of 96
# recipes now declaring their build tools (#206).
#
#
# v2.3.0 -- and the version number itself is part of the change.
#
# v2.2.0 shipped, and then rc31..rc35 were tagged AFTER it: a release
# candidate that postdates the release it is a candidate for. That was
# drift rather than a decision -- v2.0.0 and all eight v2.1.x releases
# carried no rc tags at all. It also reads backwards to anything
# following SemVer, where a pre-release sorts BEFORE its release, so a
# host running "v2.2.0-rc35" reported a version that a SemVer-aware
# reader ranks as older than a release it actually contains. (This
# platform's own pkg_version_cmp() happens to rank it the other way,
# which made the contradiction invisible from inside.)
#
# Ordinary versioning resumes here. 52 commits since v2.2.0, including
# the generated dispatcher (ADR-0218), the libc-dev retirement
# (ADR-0217) and the completed gcc bootstrap -- a minor bump, not a
# patch.
#
#
# Re-pinned to v2.2.0-rc35: a package that declares ITSELF as a build
# tool can now be upgraded in place (#166). This is not a nicety --
# gcc.recipe names gcc in its own pkg_build_depends, correctly, because
# a GCC bootstrap is seeded by a GCC, and until this the composer
# refused with "declared build tool gcc is not installed anywhere"
# while the package sat there installed. The gcc bootstrap this box is
# about to run cannot start without this daemon.
#
# Also carries: treecopy_recursive() failing on a missing source root
# instead of reporting an empty success (#194), and the m4 build fix
# (#198).
#
#
# Re-pinned to v2.2.0-rc4: issue #164 -- compressed archives are built
# by piping tar into gzip directly instead of asking tar to spawn the
# compressor, which it does through /bin/sh, which this platform's own
# control-plane root does not have. Until this, every image/artifact
# export and every package-cache save failed on this box.
#
# Re-pinned to v2.2.0-rc3: adds the export-failure stderr capture
# (found live on this box during phase 4 -- every image/artifact
# export failed as a bare "tar failed (status 0x200)" with the real
# complaint invisible). rc2 before it was rc1 with a corrected
# embedded token (recipe versions are immutable once published,
# ADR-0107, so a bad registration is fixed by a bump, never an edit).
#
# Re-pinned to v2.2.0-rc1 (ADR-0207, epic #157, branch adr-0207-btrfs):
# the btrfs-substrate release candidate for the .95 migration -- image
# versions as subvolumes, container rootfs as writable snapshots (no
# overlay on btrfs), userns on by default with the resolved mode pinned
# at creation and pre-flip definitions migrated to userns:false at
# load, btrfs install default with btrfs-then-ext4 tolerant boot
# mounts (an unmigrated ext4 box like this one runs it unchanged).
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
pkg_version="v2.53.9"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.53.9.tar.gz"
pkg_sha256="b64a4a59c5be49f54d3954b164a009d80077121b1730fc6908370db3e4f77cff"
# ADR-0199/0209: composed from exactly these, with no fallback
# environment to inherit anything missing (#168). Cix is the first
# recipe to need this declaration and it found the rule the hard way:
# the no-fallback build refuses an undeclared recipe, so the control
# plane could not build itself until it said what it uses.
#
# Read off the two function bodies rather than copied: pkg_build()
# runs make, which drives tcc (the Makefile's only compiler) against
# glibc's headers and CRT objects from libc-dev; pkg_install() runs cp
# and mkdir from coreutils; recipe.sh itself is executed by bash.
# Nothing here reaches for sed, grep, awk or a linker -- tcc links its
# own output, and this project builds no static archives.
# openssl: new in rc11, and NOT a new dependency -- cixd has linked
# -lssl -lcrypto and included <openssl/ssl.h> since Phase 9. It built
# anyway because libc-dev 2.36-5 staged `cp -a /usr/include/.`, the
# whole tree, so OpenSSL's headers arrived as a side effect of a
# package that has nothing to do with OpenSSL. libc-dev 2.36-8 stages
# 106 named glibc headers and nothing else (#169), so the side effect
# is gone and this recipe has to say what it actually needs.
#
# Found by reading rather than by a failed build: every non-glibc
# system header included anywhere in this repository's own sources was
# swept, and openssl/{ssl,bio,err,pem}.h are the only ones. So this is
# the whole of the correction, not the first instalment of it.
#
# NOTE: this is a HOSTBUILD recipe, and for a hostbuild the build
# container's lowerdir is --build-image='s own rootfs -- not a
# composed environment (ADR-0056). pkg_build_depends therefore
# DOCUMENTS what the build needs; it does not deliver it. Everything
# listed here has to be installed into cix-builder itself first.
#
# Learned the hard way, and the failure said so plainly once read
# correctly: adding gcc here changed nothing, and the build died at
#
#   make: /usr/bin/gcc: No such file or directory
#
# while the daemon's log showed a 12-tool composed environment for an
# entirely different, non-hostbuild job.
#
# gcc and binutils are needed for exactly one output: build/cix-boot.efi,
# the UEFI boot manager (ADR-0215). Everything else here is still TCC,
# per the root Makefile's own CC := tcc. They are not a preference:
# UEFI uses the Microsoft calling convention and TCC implements no
# __builtin_ms_va_list at all (ADR-0211), so it cannot compile EFI code.
# The link is `ld -m i386pep`, a PE/COFF emulation that exists only in
# binutils 2.42-9 and later here -- earlier revisions were configured
# for the default target alone.
#
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils"
#
# The suite includes tests that create real containers and network
# namespaces, which a build container is refused without this (#224).
# Measured: 35 of 86 tests pass in a plain build container, 36 with it.
# It also delegates this container's own cgroup subtree, which is what
# makes nesting possible at all.
#
pkg_build_caps="CAP_SYS_ADMIN"
#
# This recipe is a hostbuild and says which image it belongs in (#182).
# An operator no longer has to remember, and naming a different one is
# refused at the API boundary instead of failing inside a container.
#
pkg_build_image="toolchain"
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
# No pkg_artifact_sha256: nothing has published bytes for this version
# yet. rc5 carried rc4's checksum forward, which the Build Provenance
# Mandate forbids outright -- a checksum approves one specific byte
# sequence and can only be added after a real Cix host has produced and
# published exactly those bytes. It did no harm there (the URL 404'd
# and the build fell through to real source, confirmed directly), but a
# stale approval sitting in a recipe is a trap waiting for whoever
# publishes that filename next.
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
	    build/cixd build/cixctl build/mkbootroot build/cix-install build/cix-recover \
	    build/mkinstalleriso build/cix-boot.efi build/cix-xorriso

	#
	# Run the test suite -- the part of it that can run here.
	#
	# Until now nothing ran ANY test on a Cix host. The dev sandbox
	# builds only cixctl by policy, and this recipe named eight
	# binaries rather than `all`, so the tests were neither built nor
	# executed anywhere. Changes were verified by compiling.
	#
	# `make selftest` builds and runs the seventeen tests that need
	# nothing but a filesystem: no daemon to fork, no containers, no
	# privileged namespaces. They are the contract guards -- the
	# generated REST surface and its deliberate route count, the
	# documentation indexes, the gcc-exception count that enforces
	# ADR-0224, the ELF install gate, treecopy's device-node handling.
	#
	# Failing here fails the build, which is the point: a cixd that
	# does not pass its own tests should not become a package, let
	# alone reach a boot slot.
	#
	# The other sixty-seven tests fork a real cixd and need root and a
	# writable data directory. Whether a build container can host that
	# is a real question with a real answer, and it is not assumed
	# here -- see issue #224.
	#
	make CIX_VERSION="$pkg_version" selftest

	#
	# #224 probe: can a build container run a DAEMON-LINKED test?
	#
	# 14 of 85 tests run in this gate. The other 67 run nowhere, and
	# they are the ones covering what actually breaks -- test_daemon,
	# test_dns, test_pkg, test_networks. The issue records three
	# possible homes for them and says the first costs one probe to
	# settle. This is that probe.
	#
	# There is a reason to expect better than the issue assumes: this
	# recipe already declares pkg_build_caps="CAP_SYS_ADMIN", so the
	# container is not the unprivileged one the issue pictured.
	#
	# Two tests, because they separate three different answers:
	#
	#   test_daemon    forks a real cixd, binds a port, writes a data
	#                  directory -- no privilege beyond that.
	#   test_networks  also creates real bridges and containers, which
	#                  is where privilege actually bites.
	#
	# Both pass and the 67 move here nearly as they are. Only the first
	# passes and the suite splits, with most of it still landing.
	# Neither passes and option 1 is dead, which is worth knowing
	# before designing a privileged runner or a test host.
	#
	# DELIBERATELY NON-FATAL. This measures; it does not gate. A probe
	# able to fail the build it rides in on would make every future cix
	# release hostage to an experiment.
	#
	echo "=== #224: daemon-linked tests in a build container ==="
	echo "  uid=$(id -u)"
	for t in test_daemon test_networks; do
		if ! make CIX_VERSION="$pkg_version" "build/$t" >/dev/null 2>&1; then
			echo "  $t: DID NOT BUILD"
			continue
		fi
		if "./build/$t" >"/tmp/224-$t.log" 2>&1; then
			echo "  $t: PASS"
		else
			rc=$?
			echo "  $t: FAIL rc=$rc -- first lines:"
			head -12 "/tmp/224-$t.log" | sed 's/^/      /'
		fi
		rm -f "/tmp/224-$t.log"
	done
	echo "=== end #224 probe ==="
}

pkg_install() {
	cp build/cixd build/cixctl build/mkbootroot build/cix-install build/cix-recover \
	   build/mkinstalleriso build/cix-boot.efi build/cix-xorriso "$PKG_DESTDIR/"

	# Asserted against the real bytes: this has to be a PE32+ image or
	# the firmware will not load it, and a wrong format would surface
	# only as a machine that does not boot after an install. "MZ" is the
	# DOS header every PE file begins with.
	magic=$(dd if="$PKG_DESTDIR/cix-boot.efi" bs=1 count=2 2>/dev/null)
	case "$magic" in
	MZ) ;;
	*)
		echo "cix-boot.efi is not a PE image (magic '$magic')" >&2
		exit 1
		;;
	esac
	mkdir -p "$PKG_DESTDIR/web"
	cp -r web/* "$PKG_DESTDIR/web/"
}
