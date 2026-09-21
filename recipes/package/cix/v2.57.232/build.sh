#
# v2.57.232: v2.57.231 plus ADR-0307's status line.
#
# test_docindex requires an ADR status to start with a bare
# Proposed/Accepted/Superseded/Deprecated; the flip to Accepted was
# written "**Accepted and implemented**" and failed the selftest.
# Fourth guard catch in this series, each on something genuinely new.
#
# v2.57.231: cbs is required in the control-plane root, and the
# identity reads need a credential (#487 ADR-0307 clause 6, #490).
#
# THIS ASSEMBLY NOW FAILS WITHOUT cbs. mkbootroot refuses to seal a
# root that has no /usr/bin/cbs, staged from cix-hosttools -- a root
# without it cannot install any package built by a PBS recipe, and 25
# are already published in that format. If an assembly fails here:
#   pkg install --image=cix-hosttools cbs
# then reassemble. Checked twice, at staging and again before the
# seal, for the reason verify_platform_libs_intact() exists.
#
# And GET /v1/system/hostauth/sessions and GET /v1/ldap/users stop
# answering without a token once gating is active. Reads, gated by
# intent: one tracks a live operator's session window under polling,
# the other is a profile per account. Name resolution is unaffected --
# nslcd resolves over LDAP against the directory server, not through
# the REST endpoint.
#
# v2.57.230: v2.57.229 plus the second blocking-wait budget line.
#
# The guard caught handle_pkg_unpack_event()'s reap of the forked
# `cbs extract` -- main.c 23 -> 24, total 56 -> 57. That one needs no
# argument: a pidfd callback's child has already exited. Third
# release in this series stopped by a count guard, which is three
# times the guards have done exactly their job.
#
# v2.57.229: a PBS recipe publishes a .cixpkg, and unpacking one does
# not stop the control plane (ADR-0307 clauses 2 and 3, #487, #497).
#
# `cbs build --output /build/artifact.cixpkg`: the container packages
# what it built and cixd takes the file rather than tarring the tree.
# Less host work, not more -- pkg_cache_save() forks tar and gzip and
# waits on both, on the reactor, and this is a rename.
#
# Unpacking a cached .cixpkg runs in a pidfd-tracked child, because
# the one place an artifact becomes a tree runs on the epoll loop.
# The tarball path is untouched: libarchive is a library, so there is
# no child to wait on.
#
# EXPECT 25 PACKAGES TO REBUILD FROM SOURCE after this deploys. They
# are the converted PBS recipes whose approval covers tarball bytes;
# an approval is never replaced, so each rejoins the artifact tier at
# its next revision bump. #497, decided rather than discovered.
#
# v2.57.228: the finalize policy is a program, and CBS runs it
# (ADR-0307 clause 2, first half).
#
# pkg-finalize.sh was sourced, reading $PKG_DESTDIR from the
# environment. It is now executed with the staged root as $1, and a
# PBS build reaches it through `cbs build --finalize-command` rather
# than as a shell step after cbs -- because CBS invokes an embedder's
# finalizer as execlp(cmd, cmd, staged_root, NULL), so a program
# taking one argument is the only shape that can stay ONE definition
# across both recipe languages.
#
# Ordering measured at tag v0.1.29 rather than assumed: CBS finalizes
# at src/package.c:374 and packages at :389, and the finalizer runs
# even with no --output -- so this is correct while cixd still
# packages the tree itself. The --output half waits for the change
# that also does clause 3.
#
# Watch the two 127s if this build fails inside finalize: the shebang
# is /usr/bin/bash (these images have no /bin/bash) and the file is
# written 0755 (a 0644 file with a good shebang fails EACCES, which
# execlp also reports as 127).
#
# v2.57.227: the explain sweep reports where an operator looks, and a
# publish refusal says what it was (#487, #496).
#
# Both found by running v2.57.226 rather than by reading it. The sweep
# re-derived every stale identity correctly -- zlib@1.3.2-14 and
# vim@9.1.1428-4, published before the newer engine landed, now report
# artifact_format cixpkg -- and said nothing anywhere, because
# pkg_init() runs ~100 lines before logstore_init(). Fourth time for
# that lesson here; it now records and reports like
# log_degraded_placements() does.
#
# And the tar.gz refusal worked while telling the caller the wrong
# thing: the log named the real cause, the HTTP body said the content
# failed to parse. It parsed perfectly.
#
# v2.57.226: v2.57.225 plus the deliberate blocking-wait budget.
#
# v2.57.225 built and failed its own selftest:
#
#   test_blocking_waits  FAIL
#   daemon/src/pkg.c has 8 blocking waits, budget is 7
#   55 blocking waits across the tracked files, ceiling is 54
#
# The second guard in two releases to catch a genuinely new call site,
# which is what both exist for. cbs_engine_version() waits for
# `cbs --version`; the raise is argued in the budget table rather than
# nudged -- a single printf of a compile-time constant, no file, socket
# or lock, read end closed before the wait, and it runs from pkg_init()
# before the epoll loop exists.
#
# v2.57.225: the daemon reads the artifact format a PBS recipe declares,
# and rebuilds a derived identity whose engine has changed (#487, #496,
# ADR-0307 clauses 1 and 7).
#
# parse_pbs_recipe() reads `format` out of explain.json, and a PBS
# recipe declaring tar.gz is refused at publish -- `cbs build` fails
# any recipe not declaring cixpkg regardless of an output path, so
# storing one means an immutable version that fails at build time
# every time. The value is read rather than assumed precisely so that
# refusal is possible.
#
# The documents already on disk did not carry the field: explain.json
# is derived once at publish by whichever cbs was running, and `format`
# only exists from v0.1.26. It is therefore treated as the derived
# cache it already claims to be and rebuilt when its producer changes,
# keyed on `cbs --version` against the version the last sweep recorded.
# Once at startup, which is complete rather than merely cheap -- the
# engine lives in the read-only control-plane root, so it cannot change
# while the daemon runs.
#
# GET /pkg/recipes' `format` becomes `language`, with `artifact_format`
# alongside it.
#
# v2.57.224: v2.57.223 plus the deliberate curl-guard count.
#
# v2.57.223 built and then failed its own selftest:
#
#   test_curl_guards  FAIL
#   found 10 curlfetch_perform() call sites, expected 9
#
# That is the guard doing its job -- the count is asserted precisely so
# a new fetch cannot arrive unnoticed. The new site (seed_place_artifact)
# sets .connect_timeout and passed the guard itself; only the number
# needed changing, and it is changed with the site named beside it.
# v2.57.223 never produced an artifact and is left published rather
# than moved, because a published recipe version is immutable.
#
# v2.57.223: an ISO build no longer refuses over a seed artifact this
# host can reach, and seed staging moved off the event loop (#495).
#
# POST /v1/system/iso refused with "the installer seed needs
# glibc@2.44-14 and its artifact is not in this host's cache". Three
# defects behind that one message, all measured on 192.168.15.95:
#
#  - the seeded version was whichever entry g_packages held first,
#    i.e. whichever image sorted first. Ten images carry glibc, five
#    at 2.44-16, and the pick was cix-builder's 2.44-14. For zlib the
#    pick was worse than arbitrary: cix-builder's 1.3.2-10 has neither
#    a cached artifact nor an approved checksum, jumpbox's 1.3.2-14
#    has both.
#  - the check was a stat() of the local cache, while the configured
#    cache at 192.168.15.31:8080 holds glibc-2.44-14, glibc-2.44-16
#    and dnsmasq-2.90-3 with digests matching those recipes'
#    pkg_artifact_sha256 exactly. Approved, reachable, refused.
#  - staging ran in the request handler, so a 12.8 MB recipe-tree copy
#    happened inside cixd's epoll loop -- which is why the fetch could
#    not be added where the check was.
#
# seed_choose() now takes the newest installed version across every
# image that is cached or approved, pkg_seed_preflight() keeps the
# synchronous 400 for what a table scan and a stat can settle, and the
# rest runs in the ISO build child (exit 126 when staging fails, which
# the reaper reports as staging rather than as "mkinstalleriso
# failed"). An uncached artifact is fetched into the seed directory
# itself and verified against the approval before it reaches media.
#
# v2.57.222: a recipe version whose ARTIFACT name another version
# already owns is refused at publish (#494).
#
# The artifact server reads a missing release as release 1, so
# `wget@1.25.0` and `wget@1.25.0-1` resolve to one object there --
# measured on 192.168.15.31, both wget-1.25.0-x86_64.tar.gz and
# wget-1.25.0-1-x86_64.tar.gz return 200 with an identical
# 211753-byte body. Two separately published, immutable recipe
# versions; one artifact name.
#
# Found converting wget to CPDL. The second version built perfectly,
# its push was refused 409 ("that name already holds different
# bytes"), so nothing was published, so #492's approval writer had
# nothing to approve, so the package rebuilds from source on every
# install on every host -- exactly the cost #492 exists to remove --
# while GET /v1/pkg reported `installed` with the right version and
# files throughout.
#
# Every existing defence held and none could help: the signature
# comment is compared in full against `cix pkg <name>@<version>
# sha256=`, the cached artifact's read `cix pkg wget@1.25.0`, so it
# was refused and the build ran. The problem is only that by the time
# the push is refused, the version is published and immutable.
#
# So it is refused at PUBLISH, where picking another release number is
# free. PKG_ERR_ARTIFACT_NAME_TAKEN and not PKG_ERR_DUPLICATE: this
# version is NOT published, and saying it is sends the author looking
# for a recipe that does not exist.
#
# Also corrects a comment in the same function that had gone false --
# it still said a PBS recipe has nowhere to carry an approval
# (cix-build-system#161, closed).
#
# v2.57.221: whether a PBS recipe is already approved is asked of its
# derived explain.json, not of the recipe text (#492).
#
# v2.57.220's writer tested strstr(stored, "\"artifact_sha256\"") and
# returned silently on a hit. The probe written to gate it tripped
# exactly that: its header quotes the key while explaining what an
# approval looks like, so the check hit a COMMENT and the approval was
# never written. A comment mentioning a declaration is not a
# declaration, and a real recipe documenting its own approval would
# have been permanently unapprovable.
#
# Also fixes an uninitialised candidate path: several refusals reach
# the cleanup label before it is built, and the cleanup unlinks it.
#
# v2.57.220: cixd writes an artifact approval into a PBS recipe (#492).
#
# It could read one and not write one, so every converted package
# rebuilt from source on every install forever unless the approval was
# added by hand -- and 88 of 148 current recipes carry one, which made
# this the gate on converting the corpus at any volume.
#
# It writes TWO files, and that is the part that is easy to miss:
# parse_pbs_recipe() reads the derived explain.json beside the recipe,
# never the recipe itself, so an approval written only into the .cbs
# would be invisible to every build. Recipe first, then explain.json --
# a crash between them reads as unapproved, which is the safe way to
# fail.
#
# The guard is an explain diff (jsondiff_equal_ignoring on
# "artifact_sha256"), not a line diff: CPDL's declarations are keys in
# a block rather than lines. One rule, asked of each format's own
# authority.
#
# It refuses rather than guesses when there is no `metadata { }` block
# to insert into. recipes/package/probe-approve-pbs is the A/B gate:
# release 1 has no block and must come back unmodified, release 2 is
# identical plus the block and must gain the approval.
#
# v2.57.219: a PBS recipe's capability NAMES and its metadata block are
# read (cix-build-system#161 and #162, both closed upstream 2026-09-18
# and verified here via cbs v0.1.25-6, built from their main at
# 170dc744d).
#
# pbs_explain_capabilities() writes the names in the same shape
# pkg_build_caps= gives the shell path, so both recipe formats hand
# pkg_build_container_spec() one string. It reports a distinct result
# when the installed cbs still answers with a COUNT, and both the
# publish path and the build-time parse refuse on that rather than
# reading it as zero -- cixd execs whichever cbs is on the host, and an
# older one would turn a declared CAP_SYS_ADMIN into no capability at
# all in silence.
#
# pbs_explain_metadata() reads artifact_sha256 and changelog out of the
# opaque metadata block, into the same fields the shell keys land in.
#
# NOTE ON DEPLOY ORDER: the host's own cbs is staged by mkbootroot from
# cix-hosttools, not from cix-builder, so cbs was upgraded THERE before
# this hostbuild -- otherwise the assembled root would carry v0.1.25-1,
# `cbs explain` at publish would still answer with a count, and a
# capability-declaring recipe would be refused by the very code this
# revision adds.
#
# Still missing, filed as #492: cixd READS a PBS artifact approval and
# cannot WRITE one, so a converted package rebuilds from source on
# every install unless its approval is hand-written.
#
# v2.57.218: a build that stages nothing is a failed build, not a
# package (#486). staged_tree_has_content() counts non-directory
# entries in the staged tree before the hostbuild/install branch, and
# a count of zero fails the install instead of publishing an empty
# artifact.
#
# Two empty packages reached the fleet before this: probe-pbs 1-1, an
# 87-byte artifact pushed to the SHARED cache and taken as a cache hit
# by every later install, and probe-minisign 2, whose recipe stages a
# README that ADR-0251 clause 4 deleted on the way out of the build.
# The second is why the gate is in the daemon and not in the build.
#
# Gated by recipes/package/probe-empty-install/1, which stages
# directories and no files and is EXPECTED TO FAIL its install.
#
# v2.57.217: a package keeps its documentation, and therefore its
# licence (ADR-0306). ADR-0251's clause 4 removed
# usr/share/{man,info,doc,locale,i18n} from every staged tree; it is
# withdrawn in full, and daemon/policy/pkg-finalize.sh no longer
# deletes a directory because nothing reads it.
#
# The other three rules stand, and the line between them is the
# decision: remove what the platform CANNOT use, never what it merely
# does not read. usr/share/doc/<package>/COPYING is where GNU packages
# install their licence, so the prune had been deleting it -- measured
# across the 171 installed package entries on this host on 2026-09-18,
# 25 licence files survive anywhere at all and every one survives by
# living somewhere clause 4 did not look.
#
# What to check after this deploys: rebuild a package that ships one
# (bison installs usr/share/doc/bison/COPYING) and read its file list
# back from GET /v1/pkg. The licence should be in it.
#
# v2.57.216: one definition of where a recipe's install lands, and cixd can run one
# written in CPDL (ADR-0305).
#
# v2.57.208 was the same change and did not compile: the cbs
# invocation passes --arch, and pkg_host_arch() is defined six
# thousand lines further down. One error in the whole change, found
# by the build rather than by reading -- which is the price of this
# sandbox compiling nothing but cixctl, and is cheaper than the rule
# it pays for.
#
# v2.57.209 then compiled and failed its own selftest, which is the
# gate working: test_system_backup asserts ADR-0120's recipe key
# shape, and the backup change had made every key carry a filename
# segment. An older daemon rejects the whole restore document over
# one key it cannot read as <name>/<version>, so that would have
# cost every recipe in a backup restored onto the other boot slot.
# The segment is now emitted only for a build.cbs.
#
# v2.57.211 then failed two more gates, both worth the cycle.
# test_docindex: the ADR used a bullet-list header, the shape
# this corpus drifted into and no longer accepts.
# test_blocking_waits: run_cbs_explain() held TWO blocking
# waitpid() calls against a budget of 6 -- a gate that exists
# because a wait-after-signal froze this control plane for 366
# seconds. Restructured to one wait, and the budget raised with
# the argument that gate demands: `cbs explain` opens no socket,
# takes no lock, forks nothing, and its pipe is closed before
# the wait, so a child still writing dies rather than blocking.
#
# v2.57.212 deployed and published the first PBS recipe this
# platform has ever had -- probe-pbs, version string 1-1 fused
# from CPDL's version and release, format pbs, its identity
# derived by a real `cbs explain --json` run host-side. Its
# BUILD then failed with a two-line log: cbs's own rejection
# message and nothing else, because without --events a failing
# phase prints nothing. This revision passes --events human.
#
# And --events was not the cause. cbs_workspace_prepare() creates
# root/src, root/build, root/dest and root/cache -- not root -- so
# --staged against a directory cixd had not created failed at
# mkdir(ENOENT), upstream of cbs's own build-begin event, which is
# why even the events flag reported nothing. One mkdir; found by
# reading workspace.c after two cycles of theorising. Filed as
# cix-build-system#163 because the silence will meet the next
# embedder too.
#
# And one maxim fix, asked for directly: the string ".cbs" was
# in three places -- the daemon, cixctl and the toolchain gate --
# each independently knowing what makes a recipe PBS. Three
# literals spelling one rule is a parallel implementation of the
# rule ADR-0305 exists to state. Now one definition, in
# include/recipe_format.h, shared rather than in the daemon's own
# API header because cixctl is a pure REST client.
#
# v2.57.215 then built the first PBS package end to end and
# published it with ZERO files, reporting "installed". The
# install phase ran and wrote its file; cixd harvested somewhere
# else. Two definitions of one destination -- PKG_DESTDIR pointed
# at cbs's workspace dest, correctly, while pkg_build_completed()
# went on reading build/pkg-dest. The same maxim, failing the same
# way, one revision after being asked about. Now pkg_dest_rel() is
# the only thing that knows, and the entry remembers what its
# build was told rather than the harvest re-deriving it.
#
# v2.57.210 is deliberately skipped: a quoting error in a commit
# message tagged it at the wrong commit, and a retag would have
# invalidated an already-fetched tarball's checksum. A skipped
# number costs nothing; an ambiguous tag costs a debugging session.
#
# Stage 1 of the flip to PBS. cixd learns to publish and build a
# recipe written in CPDL 0.1 -- build.cbs beside the shell build.sh,
# one per version and never both -- while still finalizing and
# packaging it exactly as it does a shell recipe.
#
# cixd does not parse CPDL: identity comes from `cbs explain --json`,
# derived once at publish into an explain.json beside the recipe,
# because re-parsing on every read would fork once per recipe file and
# there are ~1400 of them. And cixd EXECS cbs rather than linking
# libcbs -- it is PID 1 on an installed host, where a segfault in
# linked third-party code is a kernel panic rather than a failed build.
#
# This is the build whose assembly first stages /usr/bin/cbs into the
# control-plane root, from cix-hosttools. Tolerantly: a box whose
# hosttools image has no cbs simply has no CPDL engine, and publishing
# a build.cbs there is refused with the install that fixes it rather
# than dying at execve().
#
# v2.57.207: the first cix recipe with no pkg_build_image at all.
#
# v2.57.206 retired the field (#482, ADR-0304) and had to keep it one
# more version, because the daemon that built v2.57.206 still read it.
# The daemon that builds this one composes its build container from
# pkg_build_depends below, for a hostbuild exactly as for an ordinary
# install -- so this recipe is also the proof of that path: a build of
# it that succeeds is a control plane built in an environment assembled
# from its own declaration, which is what #482 was about.
#
# Note a build of the PREVIOUS version could not have proved it. A
# hostbuild for a version already in the local package cache takes the
# #144 no-op arm -- measured: artifact_cached true, a 0-byte build log,
# no container run at all -- so the proof needs a version the cache has
# never seen.
#
# v2.57.206: a hostbuild composes its build environment like every
# other build (#482, ADR-0304).
#
# pkg_build_container_spec()'s if-chain opened with is_hostbuild,
# rooted the container on --build-image's rootfs and returned, so
# ADR-0199's composition arm two branches below was unreachable for a
# hostbuild. This recipe and the kernel's -- the control plane and the
# thing it boots -- therefore declared build tools that nothing read.
# The kernel recipe's own 7.2.3-8 entry had diagnosed that correctly
# and written the workaround into an image instead of fixing it.
#
# There is no hostbuild arm now, and pkg_build_image is retired rather
# than enforced: enforcing it passes today for both recipes and would
# have left perl and wireless-regdb silently available exactly as
# before. Only composition makes an incomplete declaration fail, which
# it did twice while completing the kernel's.
#
# The field below survives for this one version because the daemon
# that builds this recipe still reads it. See its own comment.
#
# v2.57.203: a package job's version is decided once, not re-derived at
# the end (#326, ADR-0302).
#
# GET /v1/pkg reported state=installed version="" error="build failed
# (exit status 1)" for a glibc that really was installed. Only the
# record was wrong and nothing cleared it. The version a job installs
# was decided twice: start_fetch_for() parses the recipe and knows it,
# and pkg_build_completed() threw that away and looked it up again --
# writing version/depends only IF the lookup succeeded, a dozen lines
# above an unconditional state = INSTALLED. The second lookup also
# asked a different question, since NULL means "highest available", so
# a revision published mid-build would be recorded as what the job had
# installed.
#
# Captured once on the chain and read at completion.
# pkg_resume_build() sets it too (it reuses a slot by index and never
# calls start_fetch_for(), so a stale value would have been written
# confidently), chain_alloc() clears it on handout, and
# pkg_record_outcome() logs an INSTALLED entry with no version rather
# than repairing it. Also fixes build logs named -unknown- for every
# unpinned build.
#
#
# v2.57.202: disk.upper_* becomes the nested disk.usage (#475, ADR-0301).
#
# upper_ was accurate when ADR-0054 chose it -- a container's writable
# layer really was an overlayfs upperdir over its image's rootfs, so
# "how big is the upper" was both the question and the mechanism.
# ADR-0207 moved the substrate to btrfs, where the writable tree is a
# subvolume SEEDED from the image: no upper, no lower, no overlay. The
# name outlived the thing it named, and v2.57.201 is what that cost --
# the field measured a whole tree as though it were a diff while
# documenting itself as excluding a layer that no longer existed.
#
# Now disk.usage{bytes, source, measured_at}. Nested rather than flat
# because disk also carries read_bytes/write_bytes, beside which a bare
# bytes reads as their total; otherwise deliberately the same name and
# shape as GET /volumes/{name}/usage, for the same question on another
# resource. Clean cut-over, no alias: daemon, cixctl, dashboard, tests
# and contract in one commit.
#
#
# v2.57.201: two measured wrong answers, both about state the platform
# reports about itself (#339, #475).
#
# #339 -- a build slot is validated against the registry, not against
# the entry that holds it. chain_reap_stale() decided whether a job was
# over by reading e->state, the entry's own account of itself. Ten
# entries on .95 said "building" with no build behind them, pinned all
# ten slots, and refused every install and hostbuild; __pkgbuild-2 did
# not exist at all. The reap could not help because it trusted the one
# field that was wrong. main.c now answers liveness through a
# registered predicate (pkg.c still never links registry.h), and
# absence from the registry is sound because container_exit_finalize()
# calls pkg_build_completed() BEFORE registry_remove(). The entry is
# pkg_fail()ed too -- freeing the slot alone would unwedge the box and
# leave the report wrong. The stuck-entry cause is still unestablished;
# a recurrence now self-heals and says so. ADR-0300.
#
# #475 -- disk.upper_bytes reported the whole writable tree while
# documenting itself as the diff from the image. dns-1 and dns-2 both
# read exactly 29360580; two containers cannot agree to the byte on
# what each has written. On btrfs a container's writable tree is a
# subvolume SEEDED from its image, so the walk summed the whole rootfs.
# Now prefers the subvolume's qgroup (exclusive extents), the decision
# ADR-0267 already made for volumes, with a new disk.upper_source
# saying which of the two answered.
#
#
# v2.57.200: the control-plane root is written atomically, and its own
# bytes answer for its freshness (#481).
#
# mkbootroot pointed mksquashfs straight at its final output path,
# having first unlink()ed it -- so cixd-root.squashfs, the artifact
# every deploy and every installer ISO consumes, WAS the write target
# for the whole multi-minute xz compression. Anything that interrupted
# it destroyed the root that was there and left a fragment in its
# place. Found by causing it: a reboot issued while an assembly was
# running, whose POST /system/update then failed 401 on an expired
# token -- the only reason a possibly-half-written root was not staged
# and booted.
#
# run_mksquashfs() now writes <out>.partial, then fsyncs it, rename(2)s
# it onto the final name and fsyncs the directory. The unlink targets
# the partial and never the final path: the previous root surviving a
# failed assembly is the point, since it is the root the machine is
# running from.
#
# Two more defects lined up with it. ADR-0105's freshness counters are
# in-memory and a deploy ends in a reboot, so a host that just booted
# the root it assembled reports {running:false, started:0, completed:0}
# -- identical to a host that never assembled anything; GET
# /system/assembly now also stat()s the artifact (image_present,
# image_complete, image_bytes, image_mtime), derived and never
# persisted for recover_iso_state()'s reason. And the ISO builder's
# required[] checked access(R_OK) where the update path checks squashfs
# magic, so the path producing signed, published installer media
# validated LESS than live staging; one squashfs_image_check() now
# serves all three readers.
#
# ADR-0299.
#
#
# v2.57.199: the ISO build refuses media missing a declared NIC driver
# (#442). stage_nic_modules() walks modules.dep for the five names in
# CIX_NIC_MODULES; a driver ABSENT from the tree matched no line,
# staged nothing, and left the count merely lower -- and
# stage_module_file() returns 0 for a source file that does not exist.
# So "staged 11 NIC module files" could not tell all five plus their
# closure from four plus their closure, and an ISO missing the exact
# driver a machine needs reported success. Now fatal and naming the
# module, so a kernel that stops building one breaks the build instead
# of shipping media blind to that chipset -- and a successful ISO build
# becomes the proof that all five are there, which is the part that was
# missing when v2.57.198's ISO staged 11 files for kernel 7.2.3.
#
#
# v2.57.198: the installer says WHY its interface list is empty instead
# of asserting a cause it no longer has (#442).
#
# A real bare-metal install listed only `lo`, and the installer printed
# above that list: "its driver is a kernel module and this installer
# carries no module tree". True when written, false 22 minutes later --
# #429 staged the drivers and modprobe the same afternoon -- so the
# install came with a confident account of a mechanism that had just
# stopped existing, and #442 was filed as "cause not established".
#
# load_nic_modules() also captured each modprobe error and discarded
# all five, calling them "ordinary". Measured on 192.168.15.95 (virtio,
# no Broadcom): `kmod load tg3` returns 0 and goes Live with used_by=0
# -- modprobe succeeds on a machine lacking the chipset, so with a
# correct tree all five loads return 0 everywhere and a non-zero is
# always a real media defect. That text is now kept and quoted.
#
# image/src/nicreport.c classifies the empty list into the three states
# that can produce it, and test_nicreport (SELFTESTS) covers every one.
# It is a separate translation unit because cix-install is pid 1 on
# real media and test_installer is in no selftest list (#480), so an
# assertion there would never run.
#
# By-products filed, not fixed: #479 (igc, Intel I225/I226 2.5G, absent
# from the kernel config entirely) and #480 (test_installer runs in no
# gate).
#
#
# v2.57.197: fixes v2.57.196's own selftest failure, and the failure
# is the measurement for #477. test_container_restart's new bindsplit
# gate asserted a "ready" service state; the API has none. registry.c
# maps CIXINIT_EV_READY to REGISTRY_SVC_RUNNING and
# CIXINIT_EV_STARTED to REGISTRY_SVC_STARTING, so "starting" is a
# service whose probe has not passed and "running" is one whose has.
# v2.57.196 reported loopsvc/running and netsvc/running with no
# probe-timeout on either -- so a service bound ONLY to 172.60.0.200
# reached ready, which could not happen before #477. Everything .196
# carried is here.
#
#
# v2.57.196: three fixes in one build.
#
# #477 -- a `ready: {tcp_port: N}` probe tries EVERY address the
# container has (ADR-0298), loopback first and then each attachment,
# one candidate per 250 ms supervision turn. It only ever tried
# 127.0.0.1: the daemon built cix-init's service table beside the
# services[] parse, hundreds of lines above the loop that assigns
# net_count, so the address argument was 0 for every container that has
# ever run. Nothing surfaced because a service bound to 0.0.0.0 answers
# on loopback -- jump/sshd is the only tcp_port probe on the box and
# /proc/net/tcp inside it shows one listener on 0.0.0.0:22. The
# addresses move onto cixinit_hello, ready_addr_be is gone, wire
# version 2, sizes pinned at hello 272 / service 1372. Three comments
# and one changelog entry asserting the old mechanism are corrected.
#
# #476 -- `pkg build-log --name=cix` reaches a running host build
# (ADR-0297). An absent ?image= was normalised to "base" and a host
# build's chain is filed under __hostbuild, so the command answered
# 404 "no build in progress" for the whole of one. On a query about a
# job that already exists, an absent image now means "whichever image
# is building that package"; two images building one package is a 400
# naming ?image=. The 404s are split and named: a lookup miss says
# what IS building, and the transient no-build-container case says to
# retry. All three chain lookups reap a stale slot first, so a
# finished job cannot be reported as transient.
#
# #478 -- a comment, no behaviour change: why the nsswitch read buffer
# is 512 bytes and why a larger file is deliberately rewritten rather
# than read fully.
#
#
# v2.57.195: an omitted container dns_servers defaults to the
# registered DNS servers sharing a network with it (#451, ADR-0295).
# An explicit [] still means none, and is now the only way to say it.
# The default reads each server's persisted DEFINITION rather than the
# live registry: autostart order is arbitrary among the depends_on: []
# definitions this box has, so a registry lookup would have made the
# symptom intermittent instead of fixing it. A container that is itself
# a DNS server is excluded -- its upstream is dns_forwarders_set().
#
# v2.57.194: ADR-0294 in the format test_docindex enforces. v2.57.193
# failed its selftest on exactly that -- the H1 must be "# NNNN - Title"
# with the number matching the filename, and the status must be a
# "## Status" heading rather than the bullet list this corpus drifted
# into and no longer accepts. Everything .193 carried is here.
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
pkg_version="v2.57.232"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.232.tar.gz"
pkg_sha256="5506f5f4e9626a8daa967acb40f0dea27f963dafeb485773fa74f072340af38d"
pkg_artifact_sha256="d4703f52420ed5491dc3df4e8e4475b88ff98455166d2daf177da3bcda3a52da"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils minisign sed tar gzip grep libarchive curl quickjs"
pkg_build_caps="CAP_SYS_ADMIN"
# What cixd needs in order to RUN. Measured, not guessed: cixd's link
# line is `-lssl -lcrypto -larchive -lcurl` (Makefile), and these three
# packages are what supply those -- openssl ships usr/lib/libssl.so.3
# and usr/lib/libcrypto.so.3, and all three are installed in
# cix-builder, the image this recipe is built in (checked against
# GET /v1/pkg on 192.168.15.95, 2026-09-17).
#
# This line was `pkg_depends=""` for the whole life of this recipe, and
# not by choice: pkg_hostbuild_start() refused any recipe with a
# non-empty pkg_depends outright, so declaring what cixd links made
# `pkg hostbuild cix` -- the only way this platform builds itself --
# reject the recipe as invalid, while an ordinary `pkg install cix`
# into an image needs exactly that declaration to pass the #389
# undeclared-link gate. ADR-0303 (#465) removed the refusal: a
# hostbuild now carries the declaration onto the entry and does not
# resolve it, since resolving means "install the closure into an image"
# and a hostbuild has no image to merge into.
#
# So this is metadata that travels with the artifact, and it is what
# ADR-0291's cix-boot design needs in order to declare `cix` as an
# ordinary manifest entry. A hostbuild does NOT install these: this
# field says what cixd links, and pkg_build_depends above says what
# builds it. That second half changed in the build this recipe
# produces -- a hostbuild now COMPOSES its build container from
# pkg_build_depends (#482, ADR-0304) instead of rooting it on
# --build-image=, which is retired. So a build prerequisite is no
# longer "whatever the named image happened to carry"; it is declared
# or it is absent.
pkg_depends="openssl libarchive curl"

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
