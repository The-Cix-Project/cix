#
# v2.55.6: AT_FDCWD, and mountns_pivot stops taking a root to walk.
#
# v2.55.5's gate refused it, and with exit_reason printed it named its
# own defect: "volume move_mount idmap: Bad file descriptor". Making the
# volume target relative left its destination dirfd as -1, and
# move_mount() ignores that argument only for an ABSOLUTE path -- for a
# relative one -1 is a real, invalid descriptor and the kernel answers
# EBADF, which reads exactly like a closed volume fd and is not one.
#
# mountns_pivot() no longer takes a root: the caller has already entered
# it, so put_old is created relative to cwd and nothing is resolved. A
# parameter that had to be "." for correctness is a trap, so it is gone
# rather than documented.
#
# test_userns_run now asserts the storage model itself -- the mount point
# the container's root creates lands as on-disk uid 0 under the id-map
# and as the subordinate base under copy+chown. Chowning the snapshot to
# the base would leave every container running and fail exactly there.
#
#
#
# v2.55.5: enter the new root before dropping privilege, not after.
#
# v2.55.4's own gate refused it. Dropping to the mapped root the moment
# the rootfs was attached left the child unable to TRAVERSE the host path
# to that rootfs -- a test daemon's data dir is a 0700 mkdtemp, so the
# volume mkdir died on its first component. /var/lib/cix is 0755 so it
# would not have shown here, but a privilege drop that silently needs o+x
# on someone else's directory is not a property to depend on.
#
# The child enters the new root by path once, while still host root, and
# everything after the drop is relative to that cwd: volume mount points
# and put_old both. Exit 126 names a chdir failure.
#
# Also: test_userns_run reports the child's own exit_reason. Exit 125
# covers four volume steps and 112 covers eight mkdir sites, so a status
# alone names a group and never a cause -- which is how the traversal
# failure was first misread as a wrong array index.
#
#
#
# v2.55.4: every workload on a btrfs host runs again (#321, #322).
#
# A container with the platform default userns:true died before it ran --
# "mkdir(put_old): Value too large for defined data type", exit 112 --
# while userns:false ran fine. It was the id-mapped presentation
# specifically, chosen by cix_btrfs_is_backing(), so it appeared the
# moment a host moved to the btrfs substrate.
#
# An id-mapped mount does not only present ownership for lookups: on a
# CREATE the kernel maps the caller's own fsuid back down through the map
# to pick the on-disk owner, and a caller outside [base, base+len) is
# EOVERFLOW. The child ran its whole setup as host uid 0, deliberately
# outside the map, so every mkdir into its own rootfs was refused. It is
# now the namespace's mapped root from the moment the rootfs is attached;
# caps survive that, since the kernel compares against <base>, not 0.
#
# #322, found in the same function: the volume loop iterates vi but
# subscripted volume_idmap_fds[i] -- the enclosing function's index, left
# at cap_add_count. Zero for a container with no cap_add, so one volume
# worked by coincidence and a second re-used the first's closed fd.
#
# Neither was reachable by any test: they all run on /tmp, which is not
# btrfs. test_userns_run now runs its whole body against BOTH rootfs
# presentations and attaches two volumes.
#
#
#
# v2.55.3: a container's overlay is unmounted on every teardown.
#
# Only DELETE unmounted it, so a stop, crash or restart left the mount
# live and the next overlay_create() on the same upperdir/workdir was a
# second live mount -- the kernel's "in-use as upperdir/workdir of
# another mount" warning, and an unbounded mount leak on a
# crash-restarting container.
#
#
# v2.55.2: the control-plane root keeps its library symlinks.
#
# mkbootroot staged with stat(), which follows links, so every
# libfoo.so -> .so.5 -> .so.5.8.3 chain was copied at each name -- zero
# symlinks and 30.93 MiB of duplicates in the root. lstat() now, with
# verify_platform_libs_intact() refusing a dangling link: that is a
# loader that cannot find libc, which panics at boot.
#
# Also: the log panel is resizable again (grid items ignore flex-basis).
#
#
# v2.55.1: installer media reports and budgets its own size.
#
# mkinstalleriso prints a per-component breakdown on every build (cixd
# already logs its stdout), and MEDIA_BUDGET_MIB=96 fails the build if
# the media exceeds it. The squashfs was measured and is already at its
# floor -- mksquashfs deduplicates.
#
#
# v2.55.0: the installer seed carries one recipe version per package.
#
# 11.9 MiB of the seed's 12.8 MiB recipe tree was superseded revisions
# -- 302 of cix alone -- which a fresh box cannot install from, having
# no artifact for them on the media. A seeded artifact's OWN version
# still ships even when superseded, or the box resolves to a recipe the
# media carries no artifact for.
#
#
# v2.54.9: GET /system/iso reports iso_bytes.
#
# The endpoint returned a path and no size, so the only way to learn an
# installer's size was to publish it and read the cache back -- which
# is how one grew from 71.7 MiB to 217.9 MiB with nobody counting.
#
#
# v2.54.8: the ISO builds, and one more stale copy of the lib layout.
#
# mkinstalleriso kept its own two-entry list of where libraries live --
# omitting usr/lib, where the isotools artifact actually puts them --
# both for staging closures and for the children's LD_LIBRARY_PATH.
# Both derive from CIX_LIB_DIRS_SEARCH now. PKG_ERR_NO_BUILD_IMAGE
# stops reporting a missing build_image as an invalid name. The ISO
# gets a dashboard on Deployment.
#
#
# v2.54.7: the rest of the blank-then-fetch instances.
#
# Running Config and Volumes cleared before awaiting; two <select>
# rebuilders repopulated on every poll and discarded an operator's
# in-progress choice. Also the schedule action field is `summary`.
#
#
# v2.54.6: refresh artefacts, reclaimed space, and the scheduler's UI.
#
# Nothing re-renders unless its data changed; Processes is
# fetch-on-demand again and legible; Services no longer blanks before
# every poll. The log panel spans the window. 25 page titles removed --
# the underlined tree row already says where you are. Swap moves to
# Host. The scheduler finally has a dashboard, on Control Plane, and
# three dead "#schedules" links now point at a page that exists.
#
#
# v2.54.5: the dashboard says when it is stale.
#
# The status bar version goes copper and clickable when this page's
# assets are older than the daemon answering them -- derived from the
# first /system/boot answer, not stamped at build time. Active tree row
# gets the tab bars' copper underline; content links go copper. Host
# reorders to Stats / Processes / Sessions / Running Config / Name /
# Log Store / Factory Reset.
#
#
# v2.54.4: Rolling Restart moves to Deployment.
#
# It is what happens once an update has landed. Pipeline is now purely
# the view of the whole thing -- the stage flow and what is stuck.
#
#
# v2.54.3: the Pipeline's stages split by what they do, not what they hold.
#
# Artifacts becomes Integration (Build / Local Packages / Local Images /
# Remote Cache); Catalogue takes the three recipe kinds -- promoted from
# sub-tabs three clicks deep -- plus Repo & Sync. Pipeline keeps
# Overview / Errors / Rolling Restart.
#
#
# v2.54.2: index.html well-formedness is gated.
#
# The Pipeline restructure left an orphan </p> behind. Second time a
# restructure of this file has left a stray close tag, and both times a
# browser silently repaired the tree, so neither showed in a screenshot
# or a DOM dump. test_web_tree now walks the tags with a real stack.
#
#
# v2.54.1: the Pipeline is a branch, and two navigation gates.
#
# Pipeline becomes a selectable group with Catalogue / Artifacts /
# Deployment under it; Boot Slots and Signing Keys move to a new
# Bootloader page under Host. SERVICE_TAB_VIEWS -- a hand-kept second
# copy of CATEGORY_VIEWS -- and two near-identical tab selectors
# collapse into one selectTabFor().
#
# The route-vs-page mistake's fourth and last location: fourteen
# renderers still sat in an else-if chain on the route. Measured against
# the live v2.54.0, 12 of 56 routes had a panel stuck on "Loading".
#
# Two new gates in test_web_tree, each proven by reintroducing its bug:
# every page-level tab must be an address (Pipeline > Reconcile was not),
# and the menu bar's topics must be the tree's top-level names in order
# (Pipeline was a submenu two levels inside Host).
#
#
# v2.54.0: renderCurrentView runs every renderer on the page.
#
# Third and last location of the route-vs-page mistake: an else-if chain
# meant only the addressed tab rendered, so the rest sat on "Loading"
# with their data fetched and cached.
#
# v2.53.99: renderers compare pages, not routes.
#
# 25 renderers guarded on parseHash().category === "<their old route>".
# With multi-tab pages the route is the TAB, so the render was skipped
# and panels sat on "Loading" while their data was fetched and thrown
# away. They now compare the page the route lands on.
#
# v2.53.98: a page refreshes all its tabs.
#
# Refreshers were keyed by route hash; a tab click does not change the
# route, so a tab's refresher never ran and the panel sat on "Loading".
# They are now resolved per page, derived from the route map. Daemon
# moves to Control Plane; Site becomes Name.
#
# v2.53.97: DHCP back under Services, Boot Slots to Kernel, and two
# regressions fixed -- four Pipeline tabs stuck on "Loading" (a dead
# view id in selectCatalogueTab), and the tree's container/network
# status colours, dropped in the tree rewrite.
#
# v2.53.96: System becomes Host, and clicking it shows the host.
#
# The group is selectable and lands on its own page, so there is no
# "Host > Host" child; Control Plane, Devices and Kernel sit under it.
#
# v2.53.95: clicking Services shows service health.
#
# Server Health was a sixth entry beside the five services, reading as
# a sibling. It is their overview, so the group itself lands on it.
#
# v2.53.94: tabs live on the page; the tree lists the live things.
#
# Five tree entries and two groups. Containers/Networks/Storage each
# open a tabbed page and list their real instances underneath; Services
# and System are groups of genuinely separate pages. Devices gains
# Named Mappings as a real tab, Pipeline tabs are in flow order.
#
# v2.53.93: no Monitoring section; no tree child repeats its parent.
#
# Monitoring dissolved into Host (stats/processes/logs), Control Plane
# (stalls) and Services (server health, now its own page). A page node
# is selectable itself, so "Networks > Networks" and "Storage >
# Storage" are gone, and the live network/disk subtrees are back.
#
# v2.53.92: the tree fold finished -- every child is a tab of its page.
#
# 20 sections remain (ten pages, five Services pages, six detail views),
# down from 56 tree destinations resolving to 18 views. Services is a
# group rather than a page, so five separate subjects do not become a
# second sixteen-tab drawer.
#
# v2.53.91: the dashboard tree is ten pages, and a page's children are
# its own tabs.
#
# The 16-tab view-daemon-config drawer is split; Routes moves to
# Networks, the storage tabs to Storage, boot/deploy tabs to Pipeline,
# Kernel Modules into Devices. New test_web_tree gate asserts the tree
# and the tab bars cannot drift apart again -- it found 7 real
# mismatches on its first run.
#
# v2.53.90: ADR-0258 -- the resource is Storage, not disks.
#
# /v1/disks -> /v1/storage, /v1/diskroles -> /v1/storage-roles, with
# operationIds, schemas, response keys, CLI commands and dashboard
# labels. Twelve endpoints, clean cut, no aliases.
#
# cixctl storage (placement) and cixctl disks (block devices) were two
# sibling commands for one noun; they merge into one.
#
# Also fixes a pre-existing link failure in test_diskpart, which the
# release build never noticed because it compiles named targets plus
# selftest, never `all`.
#
# v2.53.89: ADR-0257 part 2 -- the three intervals are gone.
#
# backup-config, volume-backup-config and repo-config each lose their
# interval_* field, and the two periodic timers behind them are deleted
# rather than left dormant. Three actions replace them: system.backup,
# volume.backup, pkg.sync.
#
# CONTRACT CHANGE. A removed field is refused with a 400 naming its
# replacement, never ignored. On the first boot after this, a config
# still carrying an interval becomes the equivalent schedule once and
# the field is dropped.
#
# v2.53.88: the scheduler's wall-clock jobs actually fire.
#
# v2.53.86/87 shipped a scheduler in which no daily or weekly job ever
# ran: schedule_next_run() returns the NEXT occurrence, always strictly
# in the future, and run_due() skipped anything whose "due" was after
# now -- which was every wall-clock job, always. The due test now
# compares the most recent occurrence against what the job has already
# accounted for.
#
# Two supporting fixes: an anchor, so a job created at 20:00 with
# "daily at 02:00" does not fire for this morning; and a skip that
# sticks and is recorded, rather than a one-tick deferral that ran the
# job anyway on the next tick.
#
# v2.53.87: cixctl argv fix for the two commands added today.
#
# dispatch_command() passes the command name separately, so argv[0] is
# the first argument AFTER it. cmd_schedule and cmd_pipeline both read
# one element too far: `schedule actions` fell through to `ls`,
# `schedule set NAME` hit the usage text, and `pipeline --all` ignored
# its only flag. Found against the live daemon.
#
# v2.53.86: ADR-0257 -- one scheduler, and a schedule is structured.
#
# Six periodic timers, four of them because an operator set an interval,
# become one scheduler with schedules as a first-class resource. The
# wire format is structured JSON rather than a cron-like string: the
# decisive property of a schedule syntax is its failure mode, and a
# typo'd cron expression still parses and means something else.
#
# Six new routes, so the count moves 283 -> 289.
#
# ADDITIVE. One action is registered (pkg.refresh-upstreams), and it has
# no existing timer, so nothing double-fires while the legacy timers are
# still alive. The four operator-set intervals migrate later, together,
# with their own contract change.
#
# v2.53.85: the pipeline flow is a grid, not a wrapping flex row.
#
# Verified by screenshotting the deployed v2.53.84 dashboard against the
# live daemon: "AUTHENTICATE" overflowed its own cell. A flex row sizes
# to the shortest name and clips the longest, which here is the one
# stage name a reader is least likely to guess. A grid gives every cell
# the same track width, so the longest name sets it for all of them.
#
# v2.53.84: v2.53.83 plus one line -- GET /pipeline declares
# x-cix-expose [cli, web], not [cli].
#
# The dashboard's Pipeline view calls getPipeline, and test_api_surfaces
# refused the build for exactly that: an endpoint a surface uses but the
# contract does not offer it. Caught by the release selftest on the box
# rather than locally, because the surface test was run before the web
# view was written and not again after.
#
# v2.53.83: ADR-0256 -- the pipeline is the model.
#
# Eleven stages and five statuses replace four unrelated vocabularies:
# the source catalogue's four states, enum pkg_failure_kind's five, a
# build-log line and a boot-entry filename. One (stage, status) pair is
# now spoken verbatim by the API, the CLI and the dashboard.
#
# GET /v1/pipeline is a read-time join over the catalogue, the package
# job records and this host's own boot entry. Route count 282 -> 283.
#
# CONTRACT CHANGE: GET /pkg/{name} and the package list no longer carry
# failure_kind. They carry stage and status. A field that changes
# meaning is worse than a field that is gone.
#
# BEHAVIOUR CHANGE, dormant on an already-installed box: confirming a
# boot now requires the configured uplink to be attached, not merely a
# bound socket. The code path is bootstrap_management_network()'s own
# fresh-install branch, so an existing host with a management network
# already flagged never reaches it and behaves exactly as before.
#
# v2.53.81 -- identical code to v2.53.80. This revision exists only to
# re-run a control-plane assembly, because cix-hosttools 2.2.0 added the
# glibc that image never had and assembly has no trigger of its own
# (#308): it runs as a side effect of a cix hostbuild completing, and a
# completed hostbuild entry cannot be cleared (#310). Two issues filed
# earlier today, both charging their cost here.
#
# v2.53.80 -- test_daemon_net stops hand-rolling cleanup and calls
# test_cleanup_containers_and_network(), which already retries a 409
# network delete. ADR-0180 makes a running container DELETE return 204
# before the child is reaped, so the network really is still in use --
# three hostbuilds were lost to that (#309).
#
# v2.53.79 -- v2.53.78 plus one declaration: probe-gcc-postglibc now
# states pkg_toolchain=gcc, which test_toolchain_policy required and
# which failed that build. ADR-0253 content is otherwise unchanged.
#
# A build output tree is created fresh (ADR-0253), which is
# what finally makes an installer ISO the size of what it carries.
#
# Three output trees were inherited across builds and each caused an
# incident: <artifacts>/cix (a web/ no recipe staged, supplied by an
# older build), the ISO staging tree (#307 -- the seed shipped twice,
# 64.6 MiB wanted plus 69.9 MiB duplicated of 217.9 MiB, and the STALE
# outer copy is the one cix-install installed), and the bootroot image
# root (a control-plane root carrying glibc objects from two builds,
# which panics pid 1).
#
# persist_fresh_output_dir() is the one operation all three now use.
#
# This revision is what puts the fixed mkbootroot and mkinstalleriso
# into the artifact the daemon actually execve()s, so the next assembly
# and the next ISO are built into empty trees.
#
pkg_name="cix"
pkg_version="v2.55.6"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.55.6.tar.gz"
pkg_sha256="dc4afd20c7e8034be3d5171bd2de9af02fc042285fd90217cbb87174f4e2e244"
pkg_artifact_sha256="986fb2f5378bcd5f3cb41f2a1099b9cc71eb0669da15cb2fd9b92eb036de2b13"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils"
pkg_build_caps="CAP_SYS_ADMIN"
# ADR-0208: cix-builder's one job is building Cix. This said
# "toolchain" -- an image no recipe in this repo describes, which
# existed only on one host and died with it. cix-builder is what the
# guide documents and what the taxonomy names (#305).
pkg_build_image="cix-builder"
pkg_depends=""

pkg_build() {
	make CIX_VERSION="$pkg_version" \
	    build/cixd build/cixctl build/mkbootroot build/cix-install \
	    build/cix-recover build/mkinstalleriso build/cix-boot.efi build/cix-xorriso
	#
	# The contract guards: the generated REST surface and its route
	# count, the documentation indexes, the ADR-0224 gcc-exception
	# count, the ELF install gate, treecopy's device-node handling.
	# Failing here fails the build, which is the point.
	#
	make CIX_VERSION="$pkg_version" selftest
	#
	# #296: prove the new console-input scenario actually catches the
	# bug it was written for.
	#
	# The selftest above ran test_console_exec, which now includes an
	# INPUT scenario: it writes a keystroke as a binary websocket frame
	# and requires a real exec'd process to echo it back. A pass proves
	# nothing on its own -- the whole reason #294 shipped is that this
	# file was green while the console ignored every keystroke.
	#
	# v2.53.60 tried to prove it by DELETING the memset, and the test
	# still passed, so that build correctly failed. The reason is worth
	# keeping: not zeroing a malloc() only reproduces #294 when the
	# memory happens to be non-zero, and fresh kernel pages are zeroed,
	# so pty_out_len came up 0 and the code worked. That is also why
	# #294 was environment-dependent rather than constant.
	#
	# So the struct is POISONED instead of merely left unzeroed, which
	# is what uninitialised memory actually looked like on the host that
	# hit this: every field the code below assigns is still assigned,
	# and every field it forgot -- pty_out_len, the buffer -- is
	# garbage. That is exactly #294.
	#
	echo "=== #296: poisoning the console session struct, the input test must now FAIL ==="
	sed -i 's@^\tmemset(sess, 0, sizeof(\*sess));$@\tmemset(sess, 0xff, sizeof(*sess)); /* #296 proof */@' daemon/src/main.c
	grep -q "#296 proof" daemon/src/main.c || {
		echo "could not inject the #294 condition -- the proof is not being run" >&2
		exit 1
	}
	make CIX_VERSION="$pkg_version" build/cixd
	rc=0
	./build/test_console_exec >/tmp/c296.log 2>&1 || rc=$?
	tail -30 /tmp/c296.log | sed 's/^/  /'
	if [ "$rc" = "0" ]; then
		echo "=== the console-input test PASSED against a build carrying the #294 bug" >&2
		echo "=== it does not detect what it was written for; failing this build" >&2
		exit 1
	fi
	echo "=== the input test detected the injected bug (rc=$rc), so its pass above is real ==="
	sed -i 's@^\tmemset(sess, 0xff, sizeof(\*sess)); /\* #296 proof \*/$@\tmemset(sess, 0, sizeof(*sess));@' daemon/src/main.c
	grep -q "memset(sess, 0, sizeof(\*sess));" daemon/src/main.c || {
		echo "could not restore the #294 fix -- refusing to ship" >&2
		exit 1
	}
	make CIX_VERSION="$pkg_version" build/cixd
}

pkg_install() {
	cp build/cixd build/cixctl build/mkbootroot build/cix-install \
	   build/cix-recover build/mkinstalleriso build/cix-boot.efi build/cix-xorriso \
	   "$PKG_DESTDIR/"
	# The dashboard mkbootroot copies into the assembled root.
	cp -r web "$PKG_DESTDIR/web"
	for f in index.html app.js style.css api.js; do
		if [ ! -f "$PKG_DESTDIR/web/$f" ]; then
			echo "web/$f missing from the artifact -- the assembled control plane would serve a blank dashboard" >&2
			exit 1
		fi
	done
	# Asserted against the real bytes: this has to be a PE32+ image or
	# the firmware will not load it, and a wrong format would surface
	# only as a machine that does not boot after an install. MZ is the
	# DOS header every PE file begins with.
	magic=$(dd if="$PKG_DESTDIR/cix-boot.efi" bs=1 count=2 2>/dev/null)
	case "$magic" in
	MZ) ;;
	*)
		echo "cix-boot.efi is not a PE image (magic: $magic)" >&2
		exit 1
		;;
	esac
}
