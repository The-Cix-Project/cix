#
# v2.57.193: gates the CONSUMER half of the quickjs ABI seam. The
# recipe already checks that the built library carries no libgcc
# TImode helper, which proves gcc took quickjs.h's JS_LIMB_BITS 32
# arm; nothing checked that tcc takes the same arm, and that gate
# would stay green if tcc ever grew __SIZEOF_INT128__ while the two
# sides diverged over the width of a JSValueUnion member.
# test_web_syntax.c now carries _Static_assert(JS_LIMB_BITS == 32),
# so this build passing IS the measurement.
#
# Also corrects five artefacts -- ADR-0294, the quickjs -3 recipe
# header, the changelog, CLAUDE.md and the Makefile comment -- which
# asserted that the v2.57.191 link failed on __udivti3. It did not:
# that build died earlier, at -ldl, and never reached symbol
# resolution. See CHANGELOG.md.
#
# v2.57.192: the web syntax gate actually links. v2.57.191 never
# built: its link line carried -ldl and -lpthread, and cix-builder's
# glibc ships libdl.so.2/libpthread.so.0 with no .so linker stub and
# no .a, so tcc failed with "library 'dl' not found" even though the
# symbols are in libc.so.6. quickjs 2026-06-04-2 drops the one archive
# member that wanted dlopen and -3 builds with -U__SIZEOF_INT128__,
# which removes gcc's TImode division helpers (tcc links libtcc1.a,
# which has none) and makes the gcc-built library agree with the
# tcc-compiled public header about JS_LIMB_BITS. The link line is
# -lquickjs -lm. The gate also covers index.html's inline script now,
# and treats an absent file as a failure rather than a note.
#
# v2.57.191 (NEVER BUILT -- see above): the dashboard JavaScript gets
# a real parser in SELFTESTS
# (#340), so quickjs is declared in pkg_build_depends. web/*.js was
# the only surface this project ships that no compiler ever read, and
# a dangling `else` left by a refactor took the whole web UI down on a
# deployed release. A textual check cannot tell that `else` from a
# legal one after a braceless `if`, so test_web_syntax links
# libquickjs.a and compiles each file with JS_EVAL_FLAG_COMPILE_ONLY
# -- parsed, never run.
#
# Also WITHDRAWS v2.57.190's compose_pid branch: it cannot be reached.
# The container name is assigned before the composer forks, so cancel
# always finds a container and never falls through to a pid. The slot
# release was verified correct as it stood. #339 stays open on the
# real gap rather than closed on a fix that does nothing.
#
# v2.57.190: `pkg cancel` can reach a build-environment composition
# (#339). fetch_pid is zeroed when the fetch child is reaped and a
# COMPOSER is then forked while the entry stays FETCHING, so for the
# whole of a composition cancel found no container and no fetch pid,
# logged "nothing running" and returned 200 having done nothing. The
# slot now records the composer's pid so cancel can end it, and
# chain_alloc() clears both pids so a reused slot cannot signal a pid
# from a job that finished long ago. See CHANGELOG.md.
#
#
# v2.57.189: the disks table updates its live cells in place (#432) --
# the one table the plain guard could not fix, because its I/O and
# usage counters are displayed and really do change every tick. The
# signature now asks only whether anything STRUCTURAL changed; when
# nothing has, the two moving numbers are written into their own cells
# and no row is touched. See CHANGELOG.md.
#
#
# v2.57.188: the rest of the refresh tick stops rebuilding unchanged
# panels (#432) -- thirteen more renderers, including four the first
# audit missed because its regex window was shorter than the longest
# functions, which are exactly the ones that render the most.
# unchangedAndRendered() now tests firstElementChild rather than a <tr>,
# so it serves the div and <select> panels too. See CHANGELOG.md.
#
#
# v2.57.187: the refresh tick stops rebuilding unchanged tables (#432).
# Blanking a table on every tick collapses the page and the browser
# clamps scrollTop, which is why opening a build log threw you back to
# the top. Seven more tick-path renderers now use unchangedAndRendered().
# Three of them read a search box, so their signatures carry the filter
# too -- guarding on the payload alone would have frozen the table
# while someone typed. See CHANGELOG.md.
#
#
# v2.57.186: the dashboard says what an empty dns_servers means (#451).
# The create form showed "(none)" and the container detail page showed
# "-", both of which read as "nothing configured" when they mean the
# container cannot resolve a name at all. No behaviour change --
# ADR-0143's explicit posture stands, it was the silence that was the
# defect. Also carries the measured walk-cost corrections to ADR-0293.
#
#
# v2.57.185: fixes the backoff v2.57.184 shipped. It was timed with
# time(), so every walk under a second measured as cost ZERO and the
# window collapsed to its floor -- a 1.25 GB tree re-walked every
# second while anything polled (measured: 14 measurements in 14 s).
# Now CLOCK_MONOTONIC milliseconds. Also corrects disk.upper_bytes'
# documented meaning: it reports the whole writable tree, not a diff
# from the image (#475). See CHANGELOG.md.
#
#
# v2.57.184: fixes v2.57.183's own selftest failure. A flat 60-second
# freshness window on the background disk measurement (#474) meant a
# growing container reported a size frozen at whatever it was first
# measured at -- test_container_stats appends to a file and requires
# the figure to advance, and it failed exactly as a user watching a
# container fill up would have noticed. The window is now proportional
# to what the walk cost. See CHANGELOG.md.
#
#
# v2.57.183: GET /containers/{name}/stats no longer walks the
# container's whole upperdir on the event loop (#474, ADR-0293). It did
# so on EVERY request, unbounded, on a path the dashboard polls. The
# walk moves into a helper_run() child and the endpoint serves the last
# figure: disk.upper_bytes is null until the first measurement lands,
# with disk.upper_measured_at saying when it was taken. See CHANGELOG.md.
#
#
# v2.57.182: #473 answered -- image_recipes/container_recipes stay
# `manual`, because recipes already arrive through pkg sync from the
# git repo named by package_repo, and reconciling them here would be a
# second, weaker path that the next sync would undo (sync never
# prunes). Also corrects the refusal message, which asserted a reason
# untrue for those two sections. See CHANGELOG.md.
#
#
# v2.57.181: fixes a v2.57.180 crash. An apigen edit silently did not
# apply, so every non-replace section was emitted CONFIG_APPLY_RECONCILE
# while the generated reconcile list correctly held eleven -- and the
# eleven without element operations reached a NULL dereference in the
# control plane. Any POST /config or /config/diff with a changed
# containers/volumes/networks/... section would have crashed cixd.
# Generator fixed, build_plan() guards it, test_apigen asserts the two
# generated lists agree. See CHANGELOG.md.
#
#
# v2.57.180: element-wise config apply (#470) for the eleven sections
# where an element is a small record. What the document names is
# created or updated; what it does not name is removed -- the same
# thing `replace` already means for a list. x-cix-config-apply gains
# `manual` for the eleven sections this document must not apply,
# ldap_users among them: its password renders as a marker, so
# reconciling it could only create accounts nobody can log in as while
# its removals would delete real ones. See CHANGELOG.md.
#
#
# v2.57.179: a config section declares what it OBSERVES (#471), the
# prerequisite #470 named. x-cix-config-state names the members that
# are reported rather than set -- a container's pid, a volume's
# created_at, zswap's kernel mirror, the whole of routes -- and they are
# left out of the comparison and may be omitted from a supplied
# section. Before this, one container restarting between fetch and
# apply made a whole document unusable. See CHANGELOG.md.
#
#
# v2.57.178: host swap works on btrfs (#472). swap_enable() sets
# FS_NOCOW_FL on the freshly created, still-empty file -- btrfs refuses
# a copy-on-write swapfile outright, which meant host swap could not be
# enabled on this platform at all. fallocate() stays: the kernel's own
# btrfs_swap_activate() does not reject preallocated extents, contrary
# to the usual advice. See CHANGELOG.md.
#
#
# v2.57.177: fixes two classes of false claim in v2.57.176's own
# ADR-0292 messages, both found by verifying the applied result on a
# real host rather than by re-reading the code. cfg_apply_swap()
# diagnosed a perfectly current document as stale for a size_mb change
# (the section has only enable/disable behind it and neither resizes),
# and three endpoint paths written from memory into refusal messages
# did not exist. Also: a misplaced `cixctl ... --json` is now an error
# instead of being silently ignored. See CHANGELOG.md.
#
#
# v2.57.176: fixes v2.57.175's own test_jsondiff expectation (the
# selftest failure that stopped that build): a root-level difference
# has an EMPTY path, and naming it "(section)" is api_config.c's job at
# render time, not the generic differ's. No daemon code changed.
#
#
# v2.57.175: a configuration document can be applied (ADR-0292).
# POST /config applies one, POST /config/diff says what one would
# change without touching anything, and `cixctl config apply|diff` drives
# both. Only the eleven sections a single setter owns are applied; the
# twenty-two whose application means reconciling individual live
# resources are diffed and refuse the whole request rather than
# half-applying it. The ConfigDocument schema now declares per section
# how it may be written, and apigen generates the applier list from it,
# so the schema and the code cannot drift. See CHANGELOG.md.
#
#
# v2.57.174: the dashboard can install a new boot manager binary
# (#468) -- the web-ux-guidelines.md design pass for GET/POST /system/
# boot-manager, which #469 shipped CLI-only. See CHANGELOG.md.
#
# v2.57.173: cix-boot.c honors loader.conf's "default" pattern (#467)
# -- the other half of what PUT /v1/system/esp writes; #469 fixed the
# one-shot half. A minimal freestanding glob matcher, a small
# loader.conf reader, and pick_entry() gaining an optional pattern
# narrowing its candidate set. "timeout" stays a deliberate no-op --
# this bootloader has no interactive menu. See CHANGELOG.md.
#
# v2.57.172: fixes a real bug in v2.57.171's own curlfetch_perform()
# (#410 follow-up): no User-Agent header was set, unlike the curl(1)
# CLI tool every call site used to be. Confirmed live: a real GNU
# mirror fetch that always worked through the old curl subprocess came
# back HTTP 403 through libcurl with no User-Agent. Fixed by setting
# CURLOPT_USERAGENT to "curl/" LIBCURL_VERSION, matching curl(1)'s own
# default identity exactly.
#
# v2.57.171: the twelve execve(curl, ...) call sites across main.c/
# pkg.c move to a shared in-process libcurl helper (curlfetch.c,
# #410) -- one implementation over the same async-fork/pidfd shape
# every call site already had, rather than a second HTTP client
# alongside cix-build-system's own. Preserves resume-on-retry for the
# large source-tarball fetch (ADR-0056), redacts the repo token before
# any libcurl error reaches a sidecar file (#405), and reports the real
# libcurl error where a call site previously only had a bare exit code.
# Declares curl in pkg_build_depends for its headers; libcurl.so.4 was
# already staged on cix-hosttools by curl's own package. See
# CHANGELOG.md for the full reasoning.
#
# v2.57.170: cix-boot.efi reads LoaderEntryOneShot (#469) -- it never
# did, despite esp_boot_next_set() genuinely writing it, confirmed dead
# end to end on 192.168.15.95 (armed cix-b.conf, real reboot per kmsg,
# came back on the running slot). New GET/POST /system/boot-manager
# gives a live host a way to actually receive this fix: no mechanism
# updated \EFI\BOOT\BOOTX64.EFI outside first-install/ISO-build before
# this. See ADR-0290 and CHANGELOG.md for the full reasoning.
#
# v2.57.169: fixes a real bug in v2.57.168's own libarchive extraction
# (#411 follow-up): ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS refused
# every single archive entry outright, silently, because dest_dir is
# always absolute -- confirmed live via a real failed `pkg install
# --name=probe-gnu-mirror`. Fixed by checking the archive's own entry
# name for an absolute path BEFORE dest_dir is prefixed onto it,
# instead of relying on that flag. Also adds real error logging
# (archive_error_string() to the log store on every failure path,
# where before there was none at all) and ARCHIVE_EXTRACT_OWNER (GNU
# tar's own default as real root, which the old tar -xf call always
# got for free). See #466 for the test-coverage gap this exposed.
#
# v2.57.168: tar extraction moves in-process via libarchive (#411's
# extraction half; creation is out of scope, byte-identity constrained
# -- see CHANGELOG.md). Also declares libarchive in pkg_build_depends
# (build-time headers) -- runtime needs libarchive installed onto
# cix-hosttools too, so mkbootroot's own CIX_LIB_DIRS_PLATFORM sweep
# bundles the .so into the control-plane root.
#
# v2.57.167: retire rm/sha256sum shell-outs, one openssl call, and the
# kmod-extra.config env var (#352, #351, #412). See CHANGELOG.md.
#
# Also declares sed, tar, gzip and grep in pkg_build_depends -- the
# build failed in turn on '/bin/sh: sed: command not found'
# (build/generated/pkg_finalize.h, ADR-0251, Makefile:714), then 'sh:
# tar: command not found' (test_artifact_export/test_image_recipe/
# test_images building their own fixtures), then 'sh: grep: command
# not found' (test_artifact_export's own tar-content assertions,
# revealed only once tar itself worked). All four tools are installed
# into cix-builder (6.1.0's own image_packages) yet were unavailable to
# this build's own composed environment. Cause not fully established
# -- whatever let earlier builds reach these ambiently stopped holding;
# declaring them is correct regardless (#168/ADR-0199: composed from
# declared tools, not from whatever an image happens to carry).
#
# v2.57.166: fix a use-after-free in the #446 ping_group_range 400
# message (it read freed JSON memory). The 400 logic itself was correct.
#
pkg_name="cix"
pkg_version="v2.57.193"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.193.tar.gz"
pkg_sha256="e6c929c3aa57ecd076baf90ad2fdcb22f837d360f3090b220b5e803e68a3b72d"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils minisign sed tar gzip grep libarchive curl quickjs"
pkg_build_caps="CAP_SYS_ADMIN"
# ADR-0208: cix-builder's one job is building Cix. This said
# "toolchain" -- an image no recipe in this repo describes, which
# existed only on one host and died with it. cix-builder is what the
# guide documents and what the taxonomy names (#305).
pkg_build_image="cix-builder"
# Reverted to empty (was briefly "openssl", #465): pkg_hostbuild_start()
# refuses ANY recipe with a non-empty pkg_depends outright
# (PKG_ERR_INVALID_RECIPE) -- "dependency resolution targets 'merge
# into an image', meaningless for a one-shot artifact harvest; every
# prerequisite must already be baked into build_image's own rootfs."
# cix's real deploy path is `pkg hostbuild cix --upgrade --wait
# --deploy` (installs into __hostbuild, elfcheck does not apply there),
# not an ordinary `pkg install --image=X` -- so this field must stay
# empty for the path that actually matters, even though elfcheck can
# legitimately refuse an ordinary `pkg install --name=cix --image=base`
# for the same missing-openssl reason. See #465.
pkg_depends=""

pkg_build() {
	make CIX_VERSION="$pkg_version" \
	    build/cix-init build/cixd build/cixctl build/mkbootroot build/cix-install \
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
	cp build/cix-init build/cixd build/cixctl build/mkbootroot build/cix-install \
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
