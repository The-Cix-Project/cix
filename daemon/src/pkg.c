#include "libdirs.h"
#include "pkg.h"
#include "squashfsimg.h"
#include "nsswitch.h"
#include "curlfetch.h"
#include "osrelease.h"
#include "version.h"
#include "targz.h"
#include "pkgpolicy.h"
#include "hostproc.h"
#include "image.h"
#include "btrfs.h"
#include "ldap.h"
#include "linux_compat.h"
#include "logstore.h"
#include "elfcheck.h"
#include "namecheck.h"
#include "pbsrecipe.h"
#include "recipe_format.h"
#include "persist.h"
#include "treecopy.h"
#include "pki.h"
#include "releasekey.h"
#include "test_image_fixture.h"

#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h> /* FICLONE, #236 */
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

extern char **environ;

/* Exit code the fetch child uses when it never got as far as running
 * curl, so the reported error does not claim a curl status that never
 * happened. Chosen outside curl's own range of documented codes. */
#define PKG_FETCH_EXIT_PRECONDITION 91

#define PKG_UNSQUASHFS_BIN "/usr/bin/unsquashfs"

/*
 * The CPDL engine (ADR-0305). Staged into the control-plane root by
 * mkbootroot from the cix-hosttools image, and TOLERANTLY -- a box
 * whose hosttools image predates `pkg install --image=cix-hosttools
 * cbs` simply has no engine, which is a normal state. So every use of
 * this path checks it is there first and says what to install when it
 * is not, rather than failing at execve() with ENOENT.
 */
#define PKG_CBS_BIN "/usr/bin/cbs"

/*
 * Ceiling on one `cbs explain --json` document (ADR-0305).
 *
 * Generous rather than tight, and heap-allocated rather than a stack
 * buffer, because this runs on the event loop's own stack. The
 * largest CPDL recipe upstream ships is 31 KB of source (tcc.cbs) and
 * its explained form is far smaller -- explain reports each phase's
 * name and an operation COUNT, not its operations -- so the real
 * documents are a few kilobytes. run_cbs_explain() refuses a filled
 * buffer explicitly rather than handing on a truncated object, since
 * truncated JSON fails to parse with a message about syntax instead
 * of about size.
 */
#define PKG_EXPLAIN_MAX 65536

/* Left with headroom under LOGSTORE_MSG_MAX (4096) once the surrounding
 * "pkg %s@%s: build output: " prefix and name/image (up to
 * PKG_NAME_MAX/PKG_IMAGE_NAME_MAX, 64 each) are accounted for --
 * logstore_write() truncates safely via vsnprintf() regardless, this
 * just keeps that truncation rare rather than routine. Raised from an
 * original 300 (paired with LOGSTORE_MSG_MAX's own original 512,
 * 2026-08-08) alongside switching the capture below from "first N
 * bytes read" to "last N bytes of the whole output" -- a real build's
 * own output routinely runs to several KB of "Compiling x"/progress
 * lines before the actual error, so capturing only the head (as
 * either original value did) reliably captured everything EXCEPT the
 * one line that mattered. */
#define PKG_BUILD_OUTPUT_CAPTURE_MAX 3800
/*
 * Issue #302: the longest tool name and the longest line the
 * missing-tool scanner keeps. A line longer than this is kept by its
 * TAIL, because the shell puts the tool name immediately before
 * ": command not found" -- "/bin/sh: line 1: gzip: command not found".
 */
#define PKG_MISSING_TOOL_MAX 64
#define PKG_TOOLSCAN_LINE_MAX 512

struct pkg_entry {
	char name[PKG_NAME_MAX];
	char image[PKG_IMAGE_NAME_MAX];
	char version[PKG_VERSION_MAX];
	char depends[PKG_DEPENDS_MAX];
	enum pkg_state state;
	char error[PKG_ERROR_MAX];
	/*
	 * ADR-0256: WHERE in the pipeline this entry stands, and WHAT
	 * happened there. Set by pkg_fail()/pkg_fail_cancelled(), which
	 * remain the only ways a package becomes FAILED (issue #101's rule,
	 * kept -- only the vocabulary changed).
	 *
	 * `status` is PIPELINE_OK whenever nothing is wrong, and `stage` is
	 * then meaningless rather than false: absence of a failure is not a
	 * position in the pipeline.
	 */
	enum pipeline_stage stage;
	enum pipeline_status status;
	char **files;
	int file_count;
	int files_cap;
	int in_use;

	/*
	 * ADR-0157: build-transient fields, meaningful only while state is
	 * FETCHING or BUILDING -- moved here from bare module statics (this
	 * is the one specific pkg_entry any given build concerns) so builds
	 * can run concurrently with no second parallel table (see the ADR's
	 * own Decision section 1). Phase 3 raised the real ceiling to
	 * PKG_MAX_CONCURRENT_JOBS == 10, operator-configurable down to 1 via
	 * `max_concurrent_jobs` (GET/PUT /v1/system/pkg-build-config); each
	 * concurrent build gets its own __pkgbuild-<chain index> container.
	 * (Phase 1, where this relocation first landed, still ran one at a
	 * time -- that is the state this comment used to describe and no
	 * longer does.)
	 */
	int cache_hit;
	/* ADR-0157 Phase 2: this build's own real, distinct registry
	 * container name ("__pkgbuild-<chain index>") -- computed once in
	 * pkg_fetch_completed() and referenced by spec_out->ns.hostname/
	 * cg.name for the rest of this entry's own build, which need a
	 * pointer that stays valid past pkg_fetch_completed() returning
	 * (container_create()/registry_create() read it synchronously
	 * from the caller in main.c right after, but a stack buffer
	 * wouldn't survive that call boundary the way an entry-owned one
	 * does). */
	char build_container_name[PKG_NAME_MAX];
	char build_lowerdir[PATH_MAX], build_upperdir[PATH_MAX];
	/* Issue #85: "<parent>/<container>" -- the cgroup path this build's
	 * own leaf lives at, under the shared budget parent. Stored on the
	 * entry because spec_out->cg.name must stay valid until clone3(). */
	char build_cgroup_path[PATH_MAX];
	char build_workdir[PATH_MAX], build_merged[PATH_MAX];
	char build_argv_cmd[512];
	char *build_argv[4];
	/*
	 * PKG_DESTDIR for this build, held on the entry because it is not
	 * the same for every recipe language (ADR-0305).
	 *
	 * A shell recipe's pkg_install() stages into /build/pkg-dest, a
	 * fixed path. A PBS recipe stages wherever CBS's workspace puts
	 * it: `cbs build --staged W` creates W/src, W/build, W/dest and
	 * W/cache, and an install phase's ${dest} is W/dest. Pointing the
	 * variable at that subdirectory is one assignment; the
	 * alternative was moving the tree afterwards in the build
	 * command, which would have made every PBS recipe depend on mv
	 * and rmdir being present -- tools a recipe declares, or does
	 * not.
	 */
	char build_destdir_env[PATH_MAX + 16];
	/* #412: build_envp's own 4th entry (ADR-0159 Phase B) used to carry
	 * an optional CIX_KMOD_EXTRA_SYMBOLS=<value> for the recipe to loop
	 * over; cixd now writes /build/extra/kmod-extra.config itself
	 * (write_kmod_extra_config()), so entry [3] stays NULL always. */
	char *build_envp[5];
	/* See pkg_fetch_completed()'s own comment for why this is a pipe
	 * at all (ADR-0087). -1 when this entry has no build output pipe
	 * currently open. */
	int build_output_rd;
	char build_output_captured[PKG_BUILD_OUTPUT_CAPTURE_MAX + 1];
	int build_output_captured_len;
	/*
	 * Issue #57: the complete build output, teed to a real file as it
	 * streams. build_output_captured above is a deliberate ~4KB TAIL,
	 * and `pkg build-log` is live-only, so until now the full output of
	 * a finished build survived nowhere -- which cost two multi-hour
	 * round trips on gcc alone: once when a verbose configure swamped
	 * the tail and hid the real error, and again when the workaround
	 * (redirecting to a file inside the container) silenced the live
	 * stream and a healthy 71-minute build was misread as hung and
	 * killed by hand. Both failures were the same missing primitive.
	 */
	int build_log_fd;
	char build_log_path[PATH_MAX];
	/*
	 * Issue #302: a build that cannot find a tool it shells out to
	 * does not necessarily fail. gettext's build environment was
	 * missing find, gzip, cmp and xargs; gzip stopped the build at
	 * Error 127 eighteen thousand lines in, and the other three did
	 * not stop it at all -- libtool silently produced a static
	 * archive with the convenience-archive objects left out, and
	 * configure silently answered two feature probes from a tool
	 * that was not there. Exit 0, wrong output, nothing said.
	 *
	 * So the daemon reads its own build output for the shell's own
	 * report and refuses the result. Scanned here rather than by a
	 * gate inside each recipe: this is a property of the build
	 * ENVIRONMENT, and a per-recipe check can only ever cover the
	 * tools someone already thought of.
	 */
	char toolscan_line[PKG_TOOLSCAN_LINE_MAX];
	int toolscan_len;
	char missing_tool[PKG_MISSING_TOOL_MAX];
	int missing_tool_count;
	/* Issue #58: wall-clock time of the last byte drained from this
	 * entry's build-output pipe -- the daemon already owns that pipe,
	 * so "is the build actually producing output?" is answerable
	 * first-class instead of by load-average guesswork (which caused
	 * one real mistaken kill of a healthy 71-minute build, and one
	 * real wedge going unnoticed). 0 until the first byte. */
	time_t last_output_at;
	time_t build_started_at;
	/*
	 * ADR-0272: when this entry's current run began, and what caused
	 * it. Set once at the single point a job starts (state ->
	 * FETCHING) and cleared when the run is closed, so a non-zero
	 * run_started_at is exactly "this entry has an open run" -- which
	 * is what stops an outcome being recorded twice for one job.
	 *
	 * Distinct from build_started_at above, which is the BUILD's own
	 * clock: a run that never got past fetching has no build start at
	 * all, and those are among the runs most worth keeping.
	 */
	time_t run_started_at;
	char run_trigger[16];
	int stall_reported;   /* so a stall is reported once, not every tick */
	/* ADR-0175/issue #35: non-empty exactly when the most recent build
	 * attempt failed with keep_on_failure requested and its build
	 * container was deliberately left registered/mounted instead of
	 * torn down (see pkg_build_completed()) -- the exact name to pass
	 * to GET /v1/containers/{name}/files?path=... or DELETE
	 * /v1/containers/{name} afterward. Cleared at the start of every
	 * fresh fetch attempt (see the "e->error[0] = '\0';" reset in
	 * start_fetch_for()) so a stale name never survives past the next
	 * attempt on this same entry. */
	char kept_build_container[PKG_NAME_MAX];
	/*
	 * Issue #213: an operator asked for this build to stop.
	 *
	 * Set by pkg_cancel() just before it kills the build container.
	 * pkg_build_completed() then sees the container die like any other
	 * build death and consults this to describe the outcome honestly:
	 * without it, a cancel is indistinguishable from the build being
	 * killed by anything else, and would be reported as
	 * "killed by signal 9".
	 *
	 * A flag rather than pkg_cancel() writing the failure itself,
	 * because the completion path is going to run regardless -- two
	 * writers for one outcome would race over which description
	 * survives.
	 */
	int cancel_requested;
};

struct pkg_recipe {
	char name[PKG_NAME_MAX];
	char version[PKG_VERSION_MAX];
	/*
	 * Which language this recipe is written in (ADR-0305). Set by
	 * parse_recipe() from the file's own name, because the filename
	 * IS the format -- so it travels with the parsed recipe rather
	 * than being re-derived from a path at each of the places that
	 * need it, which is how the two could disagree.
	 */
	int is_pbs;
	/* source[0]/sha256[0] is "the" source, extracted into /build/src;
	 * source[1..source_count-1] are plain files copied into
	 * /build/extra/<basename> (ADR-0036). Every recipe before this one
	 * has source_count == 1. */
	char source[PKG_MAX_SOURCES][PKG_URL_MAX];
	char sha256[PKG_MAX_SOURCES][PKG_SHA256_MAX];
	int source_count;
	char depends[PKG_DEPENDS_MAX];
	/*
	 * Issue #109: the tools that must be present to BUILD this package,
	 * as opposed to `depends` above, which is what the built thing
	 * needs at runtime and is merged into the target image.
	 *
	 * Declaring it is what makes a build reproducible: the build
	 * container is composed from exactly these packages and nothing
	 * else, so what a build ran against is a property of the recipe
	 * rather than of this box's install history. Empty means the old
	 * shared-sandbox behaviour, which is fungible by construction and
	 * on the way out -- see ADR-0199.
	 */
	char build_depends[PKG_DEPENDS_MAX];
	/*
	 * ADR-0255: how this package discovers what upstream published.
	 *
	 * Names a discovery kind (srcupstream.h), e.g. "kernel.org". Empty
	 * means the package does not roll -- it is PINNED, which is a
	 * permanent first-class answer rather than a gap: plenty of
	 * software publishes no machine-readable release list and no signed
	 * checksums, and this platform deliberately holds some things
	 * still. Absence being the safe default is what makes "cannot
	 * resolve" loud by construction: a package that never said how it
	 * discovers releases is never silently left behind, because it was
	 * never trying to move.
	 *
	 * WHICH channel and how far back are NOT here. Those are operator
	 * state (srcpolicy.h), because a recipe cannot know which line a
	 * particular box is meant to sit on -- the same split ADR-0188
	 * already established for artifact policy.
	 */
	char upstream[PKG_NAME_MAX];
	/*
	 * Capabilities this recipe's BUILD container needs, space
	 * separated, normally empty (issue #224).
	 *
	 * Exists for exactly one shape of recipe: one whose build has to
	 * create containers. This platform's own test suite is that -- 50
	 * of its tests create real containers, bridges and namespaces, and
	 * a build container cannot, measured directly (no CLONE_NEWNET, no
	 * CLONE_NEWNS, no mount, no cgroup tree).
	 *
	 * Declared in the recipe rather than configured on the host,
	 * because it is a property of what the build DOES, and a recipe is
	 * immutable and reviewable. It grants nothing a recipe did not
	 * already effectively have: a build script already runs as uid 0
	 * inside a user-namespaced container, so this widens what that
	 * confined root may do to ITSELF, not what it may do to the host.
	 */
	char build_caps[PKG_BUILD_CAPS_MAX];
	/* ADR-0122: optional -- empty means this recipe never opts into
	 * precompiled-artifact fetch, always builds from source. When set,
	 * it's the ONLY thing that makes a fetched artifact trustworthy: a
	 * git-tracked, versioned expectation the daemon verifies a fetched
	 * <name>-<version>.tar.gz against (pkg_run_capture_sha256(), same
	 * verify-before-trust discipline pkg_source/pkg_sha256 already has)
	 * before ever treating it as real -- the plain HTTP artifact server
	 * (pkg_artifact_*, below) is never itself a trust boundary. */
	char artifact_sha256[PKG_SHA256_MAX];
	/* ADR-0176: optional, empty for any recipe that doesn't set it. */
	char changelog[PKG_CHANGELOG_MAX];
};

static struct pkg_entry g_packages[PKG_MAX_PACKAGES];
static char g_pkg_dir[PATH_MAX];

/*
 * ADR-0272: pipeline runs -- what has happened to an atom, as opposed
 * to where it stands now.
 *
 * A record is appended when the daemon stops working on a
 * (package, image) pair and is never touched again. Nothing reads
 * these to compute a position: GET /v1/pipeline still derives every
 * position from live state at read time and stores nothing, which is
 * ADR-0256's whole point and is not weakened by keeping a log beside
 * it any more than the audit trail weakens it.
 *
 * Sized from measurement rather than taste. On 192.168.15.95 on
 * 2026-09-10 the build-log directory held 41 files across two days,
 * 1.4 MB, mean 34 KB each -- because PKG_BUILD_LOG_KEEP caps it at 40
 * and a busy day evicts the rest. A run record is a few hundred bytes,
 * so the history this keeps outlives the logs it points at by a factor
 * of roughly thirty for a fraction of the space. That asymmetry is the
 * reason the two have separate retentions.
 */
#define PKG_RUN_MAX 2000            /* hard ceiling; retention is configurable below it */
#define PKG_RUN_KEEP_DEFAULT 1000
#define PKG_RUN_ERROR_MAX 192       /* a summary, not the build output -- that is the log */
#define PKG_RUN_LOG_MAX 96
#define PKG_RUN_TRIGGER_REQUEST "request"
#define PKG_RUN_TRIGGER_ROLLING "rolling"

struct pkg_run {
	char name[PKG_NAME_MAX];
	char image[PKG_IMAGE_NAME_MAX];
	char version[PKG_VERSION_MAX];
	char trigger[16];
	/*
	 * The build log's basename, or "" for a run that never opened one
	 * (a fetch that failed before any build started). Allowed to name a
	 * file that has since been pruned -- see pkg_runs_write_json(),
	 * which reports that rather than hiding the run.
	 */
	char log[PKG_RUN_LOG_MAX];
	char error[PKG_RUN_ERROR_MAX];   /* "" on success */
	time_t started_at;
	time_t ended_at;
	enum pipeline_stage stage;
	enum pipeline_status status;
};

/*
 * Heap, not BSS: 2000 records is a megabyte, and a control-plane daemon
 * should not carry that unconditionally for a feature a host may never
 * exercise. Allocated on the first close and on load; if the allocation
 * fails, runs are simply not recorded -- best-effort exactly like the
 * build logs they accompany, and never a reason for an install to fail.
 */
static struct pkg_run *g_runs;
static int g_run_count;
static int g_run_keep = PKG_RUN_KEEP_DEFAULT;
static char g_next_run_trigger[16] = PKG_RUN_TRIGGER_REQUEST;

/*
 * ADR-0273: the three gates, and the approvals that let one held change
 * through.
 *
 * Three because there are three points where a change escapes its own
 * blast radius -- an artifact reaching the shared cache, an image
 * rolling to a version nobody asked for, and a boot slot being written.
 * Not one per stage: ten of the eleven stages hold a change inside the
 * blast radius it already has, where there is nobody to ask.
 *
 * All default OFF. A host that has not turned one on behaves exactly as
 * it did before this ADR -- the hold is one test at the head of each
 * drain and one refusal in the update handler, and nothing else.
 *
 * An approval is an INPUT, in the same class as g_run_keep above, not a
 * stored position: what is WAITING is derived from these queues at read
 * time and stored nowhere, and only what has been ALLOWED is kept.
 */
#define PKG_APPROVAL_MAX 32
#define PKG_APPROVAL_TARGET_MAX 192
#define PKG_GATE_PUBLISH "publish"
#define PKG_GATE_ROLL "roll"
#define PKG_GATE_DEPLOY "deploy"

struct pkg_approval {
	char gate[10];
	char target[PKG_APPROVAL_TARGET_MAX];
	char who[40];
	time_t at;
};

static int g_gate_publish;
static int g_gate_roll;
static int g_gate_deploy;
static struct pkg_approval g_approvals[PKG_APPROVAL_MAX];
static int g_approval_count;
static char g_recipes_dir[PATH_MAX];

/*
 * Where adopted release-signing public keys live (ADR-0279).
 *
 * Set once at startup. A sync copies docs/keys/ out of the repository
 * tarball into it, which is how this host learns which keys may approve
 * an artifact -- the owner's decision being that the key comes from git
 * via sync, so git stays the trust root and holds one published key
 * rather than a checksum line per recipe.
 *
 * Defined HERE and not beside the push queue that fills it: the install
 * path reads it some four thousand lines earlier, and a static defined
 * after its first use is a compile error rather than a warning -- which
 * is exactly how v2.57.92's first build ended.
 */
static char g_trusted_keys_dir[PATH_MAX];
static char g_sources_dir[PATH_MAX];
static char g_installed_state_path[PATH_MAX];
static char g_containers_dir[PATH_MAX];
/* Raw images_dir, stored (not just used transiently at init) so any
 * job's own target image's rootfs path can be computed on demand --
 * unlike the old single hardcoded "base" destination, this now varies
 * per install call. */
static char g_images_dir[PATH_MAX];
/*
 * Issue #40: the shared build sandbox is a real, ordinary, versioned
 * image now, resolved exactly the way a hostbuild's own --build-image=
 * already is -- not a flat, unversioned directory living beside the
 * image mechanism as a second concept.
 *
 * It is `cix-builder`, the image that already served the structurally
 * identical "toolchain lowerdir for a build container" role for
 * hostbuilds. Operator's decision, made deliberately over a separate
 * name, and it has a real consequence worth stating rather than
 * discovering: this image is now grown by EVERY successful ordinary
 * install, not only by an explicit `pkg install --image=cix-builder`.
 * A hostbuild's own inputs therefore move when unrelated things are
 * installed -- which is exactly why it now has real version history to
 * see that in, and to roll back to.
 */
#define PKG_BUILD_SANDBOX_IMAGE "cix-builder"

/*
 * Issue #85: the one parent cgroup every build container lives under. Its
 * limits are the real, aggregate build budget -- see build_spec_init()'s
 * own comment for why a per-container limit was not one.
 */
/* Issue #86: nested INSIDE the workload parent, not beside it. Beside
 * it would be two ceilings that add up to more than the machine -- the
 * same class of mistake #85 itself was. The build budget still applies
 * on its own at this level; the level above simply bounds builds and
 * ordinary containers together. */
#define PKG_BUILD_CGROUP_PARENT "cix-workload/cix-pkgbuild"

static void pkg_build_ensure_parent_cgroup(void)
{
	struct cgroup_limits lim;

	memset(&lim, 0, sizeof(lim));
	lim.name = PKG_BUILD_CGROUP_PARENT;
	/* -1 = leave memory.swap.max alone; 0 would forbid swapping
	 * entirely, which is not what "no swap limit configured" means.
	 * See struct cgroup_limits. */
	lim.memory_swap_max = -1;
	lim.memory_max = pkg_build_get_memory_max();
	lim.cpu_max = pkg_build_get_cpu_max();
	/* Re-applied on every build so a runtime change to the configured
	 * budget takes effect rather than leaving a stale ceiling behind. */
	cgroup_create_parent(&lim);
}

/* Where a hostbuild job's own harvested output lands (ADR-0056) --
 * <g_artifacts_dir>/<name>/..., a plain host directory, never
 * container-visible. */
static char g_artifacts_dir[PATH_MAX];

/*
 * ADR-0157 Phase 1: everything genuinely per-*request* rather than
 * per-package -- a dependency chain's own resolved install order and
 * where its result is headed -- moved onto this struct from bare
 * module statics (was g_chains[chain_idx].name/_image/_is_hostbuild/
 * _build_image/_target_version, g_chains[chain_idx].dep_queue/_count/_pos/_is_upgrade).
 * Sized to PKG_MAX_CONCURRENT_JOBS (pkg.h -- shared with main.c, see
 * that definition's own comment for why it lives there).
 *
 * ADR-0157 Phase 2 gave pkg_image_recipe_apply_start() a dedicated
 * busy flag, because as a bulk artifact fetch it was a third, unrelated
 * job kind with no pkg_entry of its own. ADR-0209 retired that fetch:
 * applying a recipe is now synchronous bulk-declare, so it holds
 * nothing across an event-loop turn and the flag is gone.
 */

struct pkg_chain {
	/* Was g_chains[chain_idx].name -- "" means this chain slot is idle. */
	char name[PKG_NAME_MAX];
	/*
	 * ADR-0272: what caused this job, carried for the whole chain.
	 *
	 * It lives here and not in a module static consumed at the
	 * run-open site, because that consumption was wrong in two ways
	 * that a single-package test could not show. pkg_install_start()
	 * has four early returns BEFORE any run opens (bad name, busy, no
	 * recipe, already installed), so a rolling rebuild that failed to
	 * start left "rolling" behind for the next operator-requested
	 * install to pick up. And resolve_chain() means the ambient value
	 * is consumed by dep_queue[0] alone, so in a rolling rebuild that
	 * pulled dependencies -- most of them -- every atom after the
	 * first recorded itself as "request". A chain has one cause, so
	 * the chain is where it belongs.
	 */
	char run_trigger[16];
	/* The target image the in-flight job (name above, plus every
	 * dependency it pulls in) merges into -- always normalized (never
	 * empty; see normalize_image()), valid exactly when name is
	 * non-empty. For a hostbuild job this is always
	 * PKG_HOSTBUILD_IMAGE, where the resulting pkg_entry is filed. It is
	 * not where the build container comes from: that is composed from
	 * pkg_build_depends for every job alike (ADR-0304). */
	char image[PKG_IMAGE_NAME_MAX];
	/* True exactly while this chain's job is a hostbuild (ADR-0056) --
	 * set explicitly at the start of every job (pkg_install_start()
	 * clears it, pkg_hostbuild_start() sets it), never left stale from
	 * a prior job, so it's safe to read any time name is non-empty. */
	int is_hostbuild;
	/*
	 * Issue #168: the composed build environment this chain is using
	 * (a "__buildenv-<hash>" image), empty only for a cache hit, which
	 * needs none. This said "empty for a hostbuild or a cache hit"
	 * until ADR-0304 (#482) made a hostbuild compose like every other
	 * build; a hostbuild now has one exactly like an ordinary install.
	 * Recorded so it can be torn down when the build finishes: a build
	 * environment exists for one build and should not outlive it. Kept
	 * here rather than derived again later because the recipe it was
	 * composed from may have been superseded by then.
	 */
	char buildenv_image[PKG_IMAGE_NAME_MAX];
	/* ADR-0107: the caller-requested explicit version pin for the
	 * single top-level package this job actually installs/hostbuilds
	 * (empty == no pin, resolve to the highest available version, the
	 * pre-existing behavior every caller before this design got).
	 * Never applies to any dependency the chain pulls in -- see
	 * current_fetch_effective_version(). */
	char target_version[PKG_VERSION_MAX];

	/* The resolved install order for this job: dependencies first
	 * (post-order), the originally-requested package last.
	 * dep_queue_pos is the index currently fetching/building;
	 * dep_queue_is_upgrade is true only when the LAST entry is an
	 * explicit upgrade of an already-installed package (dependencies
	 * are only ever fresh-installed if missing, never auto-upgraded as
	 * a side effect). */
	char dep_queue[PKG_MAX_DEP_CHAIN][PKG_NAME_MAX];
	int dep_queue_count;
	int dep_queue_pos;
	int dep_queue_is_upgrade;

	/* ADR-0159 Phase B: only ever set by pkg_hostbuild_start() (empty
	 * for every ordinary pkg_install_start() job) -- a pre-validated,
	 * space-joined string of bare CONFIG_* symbol names, carried across
	 * to pkg_fetch_completed() where the actual build container's own
	 * environment is assembled. */
	char hostbuild_extra_config_symbols[PKG_HOSTBUILD_EXTRA_SYMBOLS_MAX];

	/*
	 * The fetch subprocess, while one is running; 0 otherwise (#239).
	 *
	 * pkg_cancel() used to accept a FETCHING entry, set the cancel flag
	 * and do nothing else, on the reasoning that "a fetch is a
	 * host-side subprocess, not a container -- there is simply nothing
	 * for the caller to kill". The flag was then supposed to be honoured
	 * by "whichever completion path runs next".
	 *
	 * That reasoning has a hole: if the fetch never completes, no
	 * completion path ever runs. A fetch retrying an unreachable
	 * upstream held its chain slot indefinitely and every later install
	 * was refused with 409, with cancel returning 200 and changing
	 * nothing. Only a reboot cleared it -- a heavy remedy for a network
	 * timeout on a shell-less host.
	 *
	 * It is a real forked child with a real pid, so it can simply be
	 * killed. Doing so makes its pidfd readable, which drives the
	 * ordinary completion path, which honours the cancel flag: the
	 * existing machinery finishes the job once something ends the child.
	 */
	pid_t fetch_pid;
	/* ADR-0175/issue #35: this chain's own copy of pkg_install_start()'s/
	 * pkg_hostbuild_start()'s keep_on_failure argument -- read by
	 * pkg_build_completed() at the moment a failure is decided, before
	 * this chain slot is cleared for reuse. */
	int keep_on_failure;
	/*
	 * What start_fetch_for() RESOLVED this job's recipe to, captured at
	 * the one moment it is known for certain (#326).
	 *
	 * pkg_build_completed() used to re-derive this at the end by
	 * re-reading the recipe off disk, and wrote e->version and
	 * e->depends only `if` that lookup and parse both succeeded --
	 * while marking the entry INSTALLED unconditionally a few lines
	 * later. A recipe revision withdrawn while its build was in flight
	 * therefore produced an entry that was INSTALLED with no version at
	 * all, which is how `state=installed version="" error=...` was
	 * measured on 192.168.15.95: three fields that cannot all be true
	 * at once, and no operation short of uninstalling a working
	 * package that would reset them.
	 *
	 * Deciding it once at the start and reading it at the end is the
	 * One Source of Truth answer: the version a job is installing
	 * cannot change halfway through, so it must not be looked up twice.
	 * Memory-only, like the rest of this struct -- nothing here is
	 * persisted, so the entry's own on-disk shape is unchanged.
	 */
	char fetch_resolved_version[PKG_VERSION_MAX];
	char fetch_resolved_depends[PKG_DEPENDS_MAX];
};

static struct pkg_chain g_chains[PKG_MAX_CONCURRENT_JOBS];

/*
 * ADR-0157 Phase 2: pkg_image_recipe_apply_start() (a bulk image-
 * artifact fetch, unrelated to any single pkg_entry) used to reuse
 * g_chains[0].name/.image directly as its own coarse busy lock -- a
 * real, working trick with exactly one chain slot, but not once more
 * than one exists: chain 0 being free no longer means "nothing is
 * running" (chain 1 could easily be mid-build), and conversely a real
 * pkg_entry-based job must also be refused while an image-apply is in
 * flight, which the old shared-field trick gave for free but a
 * decoupled flag doesn't automatically. See pkg_any_job_busy() below,
 * used by every one of the three "can a new top-level job start"
 * gates (pkg_install_start(), pkg_hostbuild_start(),
 * pkg_image_recipe_apply_start() itself).
 */

static struct pkg_entry *pkg_find(const char *name, const char *image);
/* Defined below, needed by chain_reap_stale()'s #339 recovery. */
static void pkg_fail(struct pkg_entry *e, int keep_installed, enum pipeline_stage stage,
                     const char *fmt, ...);

/*
 * Issue #246: reclaim any slot whose job is demonstrably over.
 *
 * Build capacity is derived from g_chains[i].name being non-empty, and
 * that field is cleared by more than twenty separate assignments spread
 * across the fetch, build-environment, build and resume paths. Every one
 * of them is a place the slot can be lost: a path that returns without
 * clearing holds its slot for the lifetime of the daemon, and once all
 * ten are held the host cannot build anything at all until it is
 * rebooted. That was measured -- repeated failed builds of one package
 * took a box from zero to nine of ten held, with no job running.
 *
 * Rather than audit the clear sites and hope the next one is not missed,
 * liveness is derived from the truth the rest of this file already
 * trusts. A slot's owner is in exactly one of four states, and only two
 * of them are a running job: FETCHING (set when the fetch starts, and
 * held across a forked build-environment composition) and BUILDING (set
 * when the build container starts). INSTALLED and FAILED are terminal --
 * pkg_fail() sets one or the other on every failure, including an
 * upgrade failure, which keeps the old version INSTALLED.
 *
 * This is the same predicate pkg_fetch_completed() and
 * pkg_buildenv_completed() already use to discard a late event that
 * landed on a reused slot (issue #98): an entry that is no longer
 * FETCHING cannot be the one whose child just exited. Using it here
 * makes capacity a function of state rather than of remembering to
 * clear a string, so a missed clear site becomes self-correcting
 * instead of permanent.
 *
 * An entry that has vanished entirely counts as stale for the same
 * reason. Reclaiming is logged: it means a clear site was missed, and a
 * silent self-heal would hide the defect it is compensating for.
 */
static int (*g_build_container_live_fn)(const char *container_name);

void pkg_set_build_container_live_fn(int (*fn)(const char *container_name))
{
	g_build_container_live_fn = fn;
}

/*
 * Is a BUILDING entry's build container still going to report back?
 *
 * The discriminator above asks the entry what it is doing. This asks
 * whether anyone is still going to tell it otherwise, which is the
 * question that actually decides whether the slot is recoverable.
 *
 * Safe because of the ORDER in container_exit_finalize() (main.c):
 * pkg_build_completed() is called BEFORE registry_remove(). So a
 * container that is gone from the registry has already driven its
 * completion, and there is no window in which it is absent while an
 * exit event for it is still pending -- releasing on absence cannot
 * hand a slot back from under a job that still owns it, which is the
 * #98 hazard chain_release_if_job_over() exists to avoid. This is the
 * same fact cancel's own already-gone branch relies on; that branch is
 * where a leaked slot was recovered, and the defect was that an
 * operator had to call cancel to trigger it.
 *
 * Only BUILDING is judged. A FETCHING entry's liveness is its
 * fetch_pid, which pkg.c can see for itself -- but no leak of that kind
 * has been measured since #239 gave cancel a way to kill a stuck fetch,
 * and inventing a second unmeasured mechanism here would be guessing at
 * a failure rather than fixing one.
 */
static int build_job_can_still_report(const struct pkg_entry *e)
{
	if (e->state != PKG_STATE_BUILDING)
		return 1;
	if (g_build_container_live_fn == NULL)
		return 1; /* nobody registered an answer -- do not judge */
	if (e->build_container_name[0] == '\0')
		return 1;
	return g_build_container_live_fn(e->build_container_name);
}

static void chain_reap_stale(void)
{
	int i;

	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
		struct pkg_entry *e;
		const char *why;

		if (g_chains[i].name[0] == '\0')
			continue;
		e = pkg_find(g_chains[i].name, g_chains[i].image);
		if (e != NULL &&
		    (e->state == PKG_STATE_FETCHING || e->state == PKG_STATE_BUILDING)) {
			/*
			 * #339: the entry says it is still building. That is not
			 * evidence, it is a claim -- and a build that reported
			 * success in its own log left ten entries making it, each
			 * pinning a slot, until the box was rebooted. Check whether
			 * anything is still coming for it.
			 */
			if (build_job_can_still_report(e))
				continue;
			why = "whose build container is gone, so no completion is coming (#339)";
			/*
			 * The entry is fixed here as well as the slot, because
			 * leaving it in BUILDING is the lie that produced the
			 * measured state: ten slots, ten entries all still
			 * claiming to be building the same package, and an
			 * operator with no way to tell a live build from a dead
			 * one. Freeing the slot alone would unwedge the box and
			 * leave the report wrong forever.
			 *
			 * pkg_fail() rather than pkg_fail_cancelled(): nobody
			 * cancelled this, it stopped reporting. is_upgrade is not
			 * knowable this late -- the chain slot is being reclaimed
			 * precisely because its bookkeeping is unreliable -- so
			 * the installed record is kept (1), which cannot lose a
			 * version that really is installed and at worst preserves
			 * one this dead build was going to replace.
			 */
			pkg_fail(e, 1, PIPELINE_BUILD,
			         "the build container %s is gone and never reported an exit, so this "
			         "build cannot complete; its job slot has been reclaimed (#339)",
			         e->build_container_name);
		} else {
			why = "whose job is no longer running (#246) -- a chain slot was not "
			      "released on some path";
		}
		logstore_write("cixd", "warn", "pkg: reclaimed build slot %d, held by %s@%s %s", i,
		                g_chains[i].name, g_chains[i].image, why);
		g_chains[i].name[0] = '\0';
		g_chains[i].dep_queue_count = 0;
	}
}

/*
 * Release a chain slot unless the job holding it is still running
 * (#254).
 *
 * A slot belongs to a job, and a job that is neither FETCHING nor
 * BUILDING has finished -- so nothing will come back to release the
 * slot later and this is the last moment anyone knows it is free. That
 * is exactly the rule chain_reap_stale() applies; using it here is what
 * makes the reap a safety net rather than the thing actually doing the
 * work.
 *
 * The two completion paths that need this claimed in their comments to
 * use "the same stale-slot discriminator" and did not. pkg_fetch_
 * completed() returned on a non-FETCHING entry WITHOUT clearing, so a
 * job whose entry had already left FETCHING -- a failure, most of all
 * -- left its slot named forever; six such slots, all held by
 * tcc@toolchain, is what #246's reap was reclaiming. pkg_buildenv_
 * completed() cleared UNCONDITIONALLY, which is wrong in the other
 * direction: a slot reused by a job that is now BUILDING would be
 * handed back while that job still owns it, which is the #98 hazard
 * the discriminator exists to avoid.
 *
 * One function, so they cannot drift apart again while their comments
 * say they agree.
 */
static void chain_release_if_job_over(int chain_idx, const struct pkg_entry *e)
{
	if (e != NULL && (e->state == PKG_STATE_FETCHING || e->state == PKG_STATE_BUILDING))
		return;
	g_chains[chain_idx].name[0] = '\0';
	g_chains[chain_idx].dep_queue_count = 0;
}

/* Finds a free chain slot (name[0] == '\0'). Returns its index, or -1
 * if every slot is already in use. */
static int chain_alloc(void)
{
	int i;
	int max_jobs = pkg_build_get_max_jobs();

	chain_reap_stale();
	for (i = 0; i < max_jobs; i++) {
		if (g_chains[i].name[0] == '\0') {
			/*
			 * fetch_pid belongs to the job that just ended, and slots
			 * are reused constantly. Clearing it here is what makes
			 * "a stale pid must never be signalled" true of a REUSED
			 * slot and not only of a completed one: a cancel matches
			 * on name and image, so a slot that came back round to the
			 * same target could otherwise have signalled a pid
			 * belonging to a job that finished long ago.
			 */
			g_chains[i].fetch_pid = 0;
			/*
			 * #326: and for the same reason -- the captured version
			 * belongs to the job that just ended. Cleared on handout
			 * so a path that forgets to set it produces a MISSING
			 * version (the old, visible failure) rather than another
			 * package's version written confidently onto this entry.
			 * Both real start paths set it; this is what keeps a
			 * future third one from being silently wrong.
			 */
			g_chains[i].fetch_resolved_version[0] = '\0';
			g_chains[i].fetch_resolved_depends[0] = '\0';
			/* ADR-0272: requested unless a caller says otherwise, so
			 * a hostbuild and every other direct entry point are
			 * right without having to remember to say so. */
			snprintf(g_chains[i].run_trigger, sizeof(g_chains[i].run_trigger), "%s",
			         PKG_RUN_TRIGGER_REQUEST);
			return i;
		}
	}
	return -1;
}

/* True if there is no room for a new top-level job right now -- every
 * chain slot in use. Image-recipe-apply used to be counted here too;
 * ADR-0209 made it fully synchronous, so it can never be "in flight"
 * across an event-loop turn and has no flag left to check. */
static int pkg_any_job_busy(void)
{
	return chain_alloc() < 0;
}

/*
 * True when a chain slot is already running a job for `image` (#382).
 *
 * A chain's image is valid exactly while its name is non-empty (see
 * struct pkg_chain), and chain_reap_stale() is what releases a slot
 * once its entry stops fetching or building -- so this answers "is
 * this image converging right now" from the slots themselves, without
 * consulting any entry. Both sides are normalized, never-empty image
 * names (normalize_image() for a chain, pkg_rebuild_queue_add()'s own
 * empty check for the queue), so a plain compare is the whole test.
 */
static int image_has_job_in_flight(const char *image)
{
	int i;

	chain_reap_stale();
	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
		if (g_chains[i].name[0] == '\0')
			continue;
		if (strcmp(g_chains[i].image, image) == 0)
			return 1;
	}
	return 0;
}

/*
 * ADR-0157 Phase 1/2: which pkg_entry currently owns the open build-
 * output capture pipe (ADR-0087), one slot per concurrent chain --
 * deliberately a separate pointer, not derived from g_chains[idx].
 * name/image: pkg_build_output_close() can run (via pkg_build_spawn_
 * failed()) AFTER the owning chain has already been cleared back to
 * idle, so looking the entry up by name at that point would fail. Set
 * the instant a build's own output pipe is opened (pkg_fetch_
 * completed()), cleared back to NULL once pkg_build_output_close()
 * actually closes it. NULL means no build output pipe is currently
 * open for that chain (pkg_build_output_fd(idx) returns -1).
 */
static struct pkg_entry *g_build_output_entries[PKG_MAX_CONCURRENT_JOBS];

/*
 * ADR-0157 Phase 2: the build container's own registry name now
 * carries the owning chain's index (e.g. "__pkgbuild-0",
 * "__pkgbuild-1") -- a real, distinct container per concurrent build,
 * exactly like any other container, instead of the single fixed name
 * every build used to share (which made a second genuinely concurrent
 * build container impossible: registry_create() rejects a duplicate
 * name outright). PKG_BUILD_CONTAINER_NAME itself (pkg.h) is now a
 * prefix, not a complete name.
 */
void pkg_build_container_name(int chain_idx, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s-%d", PKG_BUILD_CONTAINER_NAME, chain_idx);
}

/*
 * The inverse -- called unconditionally for every container's exit
 * (pkg_build_completed()'s own contract, mirroring exactly how DNS/
 * PKI's owner-forgetting callbacks are already called unconditionally
 * on every container delete), so this must cleanly reject the
 * overwhelming majority of names that are simply a real, unrelated
 * container and never touched g_chains[] at all. Returns -1 for
 * anything that doesn't parse as "PKG_BUILD_CONTAINER_NAME-<digits>"
 * with digits in [0, PKG_MAX_CONCURRENT_JOBS).
 *
 * Exported (non-static): main.c's own handle_stop() needs the exact
 * same "is this really a build container" shape test to replace what
 * used to be a plain strcmp() against the single fixed name -- reusing
 * this rather than duplicating the prefix-parse logic a second time.
 */
int pkg_build_container_chain_index(const char *container_name)
{
	static const char prefix[] = PKG_BUILD_CONTAINER_NAME "-";
	size_t prefix_len = sizeof(prefix) - 1;
	long idx;
	char *endptr;

	if (strncmp(container_name, prefix, prefix_len) != 0)
		return -1;
	if (container_name[prefix_len] == '\0')
		return -1;
	idx = strtol(container_name + prefix_len, &endptr, 10);
	if (*endptr != '\0' || idx < 0 || idx >= PKG_MAX_CONCURRENT_JOBS)
		return -1;
	return (int)idx;
}

/* ADR-0107: images awaiting a rolling-manifest rebuild, queued by
 * pkg_recipe_add() and drained by pkg_try_start_queued_rebuild() --
 * a small FIFO reusing this daemon's own existing "one fetch/build job
 * in flight" v1 constraint rather than inventing parallel job tracking.
 * A real deployment queuing more than this many images at once for a
 * single new recipe version is a future problem, the same pragmatic
 * bound every other daemon-owned array here already uses. */
#define PKG_REBUILD_QUEUE_MAX 32
static char g_rebuild_queue[PKG_REBUILD_QUEUE_MAX][PKG_IMAGE_NAME_MAX];
static int g_rebuild_queue_count;

/*
 * The queue, for GET /v1/pkg/rebuilds (#236).
 *
 * Publishing a recipe is documented as a metadata write and also
 * commits the host to rebuilding every image tracking that package
 * `rolling`. Nothing reported that, so an operator could neither see
 * what a publish had started nor count what was outstanding -- which,
 * before the loop stopped blocking on it, was minutes of a box that
 * looked broken for reasons nothing named.
 */
void pkg_rebuild_queue_write_json(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "queued");
	jw_arr_open(w);
	for (i = 0; i < g_rebuild_queue_count; i++)
		jw_str(w, g_rebuild_queue[i]);
	jw_arr_close(w);
	jw_key(w, "depth");
	jw_int(w, g_rebuild_queue_count);
	jw_key(w, "capacity");
	jw_int(w, PKG_REBUILD_QUEUE_MAX);
	jw_obj_close(w);
}

static int pkg_name_is_valid(const char *name)
{
	return simple_name_is_valid(name, PKG_NAME_MAX);
}

static int pkg_image_is_valid(const char *image)
{
	return simple_name_is_valid(image, PKG_IMAGE_NAME_MAX);
}

/* Every public entry point accepts NULL/"" to mean "the default image"
 * (PKG_DEFAULT_IMAGE) -- internal code past this point always works
 * with the normalized, never-empty form, so every comparison/lookup
 * is a plain exact string match, never a second "is this empty"
 * special case scattered throughout the file. */
static const char *normalize_image(const char *image)
{
	return (image != NULL && image[0] != '\0') ? image : PKG_DEFAULT_IMAGE;
}

/*
 * Is a job already running for this exact target, or (hostbuild=1) is
 * ANY hostbuild already running?  #362 and #384.
 *
 * Both are admission questions and both belong in the request handler
 * rather than in pkg_install_start(): the rolling drain treats a failed
 * start as "try the rest of this image's manifest" and then pops the
 * image as caught up if nothing started, so a refusal down there would
 * drop a converging image out of the queue (#384 says this explicitly,
 * having nearly made that mistake).
 *
 * The hostbuild arm is deliberately coarser than the target arm. Two
 * hostbuilds 13 seconds apart put 192.168.15.95 into a kernel-panic
 * reboot loop -- cixd is pid 1 and the command line carries panic=10,
 * so a cixd death is three reboots and a lost build (#362). A hostbuild
 * compiles the whole control plane, which is the heaviest thing this
 * platform ever runs, and ADR-0165's shared-parent cgroup budget was in
 * place and did not prevent it. So one at a time, whatever package it
 * names -- not merely one per package, which would have accepted that
 * exact pair had they been different packages.
 *
 * Ordinary concurrent installs are untouched: ADR-0157's parallel build
 * slots stay exactly as they are, and only a SECOND job for the SAME
 * target is refused.
 *
 * Reads the chain slots, like image_has_job_in_flight() above, so the
 * answer comes from what is actually running rather than from an entry
 * that may lag it.
 */
int pkg_job_in_flight_for(const char *name, const char *image, int hostbuild)
{
	int i;

	chain_reap_stale();
	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
		if (g_chains[i].name[0] == '\0')
			continue;
		if (hostbuild) {
			if (g_chains[i].is_hostbuild)
				return 1;
			continue;
		}
		if (name != NULL && image != NULL && strcmp(g_chains[i].name, name) == 0 &&
		    strcmp(g_chains[i].image, normalize_image(image)) == 0)
			return 1;
	}
	return 0;
}

/*
 * ADR-0157 Phase 2: which in-flight chain (if any) is building the
 * given top-level target -- used by main.c's own GET /v1/pkg/build/log
 * WebSocket-upgrade handler to resolve a caller-supplied ?name=&image=
 * pair to a concrete chain_idx, now that a fixed single build no
 * longer exists to attach to implicitly. image may be NULL/empty, in
 * which case normalize_image()'s own default applies (matching every
 * other image-optional entry point in this file). Returns -1 if no
 * chain is currently building that target.
 */
int pkg_chain_index_for_target(const char *name, const char *image)
{
	const char *norm_image = normalize_image(image);
	int i;

	/* A slot can still carry a name after its job ended, when some
	 * path failed to release it -- #246 exists because six did. Every
	 * "what is in flight" question reaps first (pkg_job_in_flight_for()
	 * and pkg_active_chain_names() already did), or the three of them
	 * answer differently about the same slot. It costs a warn log the
	 * one time a leak is reclaimed, which is the point. */
	chain_reap_stale();
	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
		if (g_chains[i].name[0] != '\0' &&
		    strcmp(g_chains[i].name, name) == 0 &&
		    strcmp(g_chains[i].image, norm_image) == 0)
			return i;
	}
	return -1;
}

/*
 * The same lookup when the caller named a package and NO image (#476).
 *
 * pkg_chain_index_for_target() cannot answer this, and deliberately:
 * normalize_image() turns an absent image into PKG_DEFAULT_IMAGE,
 * which is the right default for every other image-optional entry
 * point (an install with no image installs into "base"). But a chain
 * is filed under the image it builds for, and a hostbuild's is
 * PKG_HOSTBUILD_IMAGE -- so `?name=cix` alone matched nothing while a
 * hostbuild of cix was running, and the endpoint answered "no build in
 * progress" about a build that was in progress. Measured on
 * 192.168.15.95, 2026-09-16, during the v2.57.195 hostbuild:
 * `cixctl pkg build-log --name=cix` returned 404 "no build in
 * progress" throughout, while GET /v1/pkg reported the cix job
 * building. The same hole applies to any non-default image, not just
 * hostbuild; hostbuild is only how it was found, since it is the one
 * image an operator never types.
 *
 * "No image" means "whichever image, as long as that is unambiguous",
 * so the count comes back too: the caller turns 0 into a 404 and >1
 * into a 400 asking for ?image=, rather than picking one arbitrarily.
 * Returns the index on exactly one match, -1 otherwise.
 */
int pkg_chain_index_for_name(const char *name, int *out_match_count)
{
	int i, found = -1, count = 0;

	/* Reaped for the same reason pkg_chain_index_for_target() is, and
	 * it matters more here: matching a slot whose job has finished
	 * would send the caller to the "no live output right now, retry"
	 * 404, which asserts transience about a build that is over. */
	chain_reap_stale();
	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
		if (g_chains[i].name[0] == '\0' || strcmp(g_chains[i].name, name) != 0)
			continue;
		count++;
		if (found < 0)
			found = i;
	}
	if (out_match_count != NULL)
		*out_match_count = count;
	return (count == 1) ? found : -1;
}

/*
 * ADR-0157 Phase 2: how many chains are genuinely in flight right now,
 * and which ones -- lets a caller with no explicit target (e.g. a
 * pre-Phase-3 CLI/web client hitting GET /v1/pkg/build/log with no
 * ?name=) fall back to "the one build in progress" when that's
 * unambiguous, exactly like the old single-build behavior, without
 * pkg.c itself having to guess what "no target specified" should mean.
 * out_indices must have room for at least PKG_MAX_CONCURRENT_JOBS ints.
 * Returns the number of busy chains found (0..PKG_MAX_CONCURRENT_JOBS).
 */
/*
 * The names of the chains currently holding a slot, comma-separated
 * (#246).
 *
 * Nothing reported this, which is why a leaked slot could only ever be
 * inferred from an unrelated endpoint's error message. A slot is a
 * finite resource -- chain_alloc() hands out max_concurrent_jobs of
 * them and pkg_any_job_busy() refuses every new job once none is free
 * -- so "what is holding them" has to be answerable directly.
 *
 * Returns the number of busy chains, and writes an empty string when
 * there are none.
 */
int pkg_active_chain_names(char *out, size_t out_size)
{
	int i, count = 0;

	/* #246: report the capacity a caller would actually get, not the
	 * slots that merely still carry a name. Without this an operator
	 * reads "10 of 10 in use" from a box where nothing is running. */
	chain_reap_stale();
	if (out_size > 0)
		out[0] = '\0';
	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
		size_t used;

		if (g_chains[i].name[0] == '\0')
			continue;
		count++;
		if (out_size == 0)
			continue;
		used = strlen(out);
		if (used + strlen(g_chains[i].name) + strlen(g_chains[i].image) + 5 < out_size)
			snprintf(out + used, out_size - used, "%s%s@%s", used > 0 ? ", " : "",
			         g_chains[i].name, g_chains[i].image);
	}
	return count;
}

int pkg_active_chain_indices(int *out_indices)
{
	int i, count = 0;

	/* Same reap as its two siblings above. POST /images/gc refuses to
	 * run while this returns > 0, so a leaked slot used to block image
	 * collection with no way to clear it short of a restart. */
	chain_reap_stale();
	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
		if (g_chains[i].name[0] != '\0')
			out_indices[count++] = i;
	}
	return count;
}

/*
 * Where start_fetch_for()'s child stashes curl's own real stderr text
 * on a failed fetch, for pkg_fetch_completed() (running later, in the
 * real daemon process, once the async job's exit status arrives) to
 * read back. A plain integer exit status alone was a genuine, silent
 * diagnostic gap here -- unlike the build path (ADR-0087's build
 * output pipe), a bare "curl exit status 1" gives no way to tell
 * "unsupported protocol" apart from a DNS failure, a TLS failure, or
 * anything else curl's own -S flag would have reported on stderr.
 * A small sidecar file (not a pipe+epoll registration like the build
 * output capture) is deliberately proportionate here: curl's own
 * error output is always a few short lines, never the megabytes of
 * output a long-running build can produce, so there is no deadlock
 * risk to design around and no need for incremental draining.
 */
static void fetch_error_sidecar_path(const char *name, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/.fetcherr-%s", g_sources_dir, name);
}

/*
 * Sibling of the error sidecar for something worth saying even when the
 * fetch SUCCEEDS -- currently an artifact that downloaded cleanly but
 * failed its checksum (#149). The error sidecar is only ever read on
 * failure, so a note about a fallback that then worked would be lost
 * there.
 */
static void fetch_note_sidecar_path(const char *name, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/.fetchnote-%s", g_sources_dir, name);
}



/*
 * Issue #101: the one place a package records a failure.
 *
 * Every site that used to write `e->state = ... FAILED` plus an error
 * string by hand now comes through here, and the kind is a required
 * parameter rather than something inferred afterwards from the message.
 * That is the whole point: a failure that does not say whether the
 * source could not be reached, the build did not work, or the recipe is
 * unreadable is one nobody can act on -- and a string every caller has
 * to pattern-match is the same problem wearing a disguise.
 *
 * keep_installed carries the pre-existing upgrade rule unchanged: a
 * failed UPGRADE leaves the package installed at the version it already
 * had, because that version is still there and still working. The error
 * and the kind are recorded either way, so "the upgrade did not happen,
 * and here is why" survives.
 */
static void rebuild_queue_remove(const char *image);
static void rebuild_queue_remove_at(int idx); /* ADR-0273 */

static void pkg_run_close(struct pkg_entry *e); /* ADR-0272 */
static void pkg_runs_save(void);                /* ADR-0272 */
/* ADR-0273: both drains sit above these in this file. */
static int gate_holds(const char *gate, const char *target);
static void approval_consume(const char *gate, const char *target);
static void approval_forget(const char *gate, const char *target);
static void pkg_runs_load(void);           /* ADR-0272 */

static void pkg_record_outcome(struct pkg_entry *e, int keep_installed,
                                enum pipeline_stage stage, enum pipeline_status status,
                                const char *fmt, va_list ap)
{
	e->state = keep_installed ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
	e->stage = stage;
	e->status = status;
	vsnprintf(e->error, sizeof(e->error), fmt, ap);
	/*
	 * The invariant #326 was a violation of, asserted rather than
	 * assumed: an entry that says INSTALLED must name what is
	 * installed. Three fields that cannot all be true at once is a
	 * second source of truth about the host, and the measured case --
	 * state=installed, version="", a stale error -- had no operation
	 * that would reset it short of uninstalling a working package.
	 *
	 * The cause is fixed upstream of here (the version is captured once
	 * at fetch start instead of re-read at completion), so this should
	 * be unreachable. It logs rather than repairs: a silent correction
	 * would hide whichever new path reopened the hole, and this
	 * function is the one funnel every failure reaches (ADR-0272), so
	 * it is the right place to notice.
	 */
	if (e->state == PKG_STATE_INSTALLED && e->version[0] == '\0')
		logstore_write("cixd", "error",
		               "pkg %s@%s: recorded INSTALLED with no version -- a failed attempt kept "
		               "an install this entry cannot name (#326); the record is inconsistent "
		               "and how it got here is a bug",
		               e->name, e->image);
	/*
	 * ADR-0272: the one funnel every failure and cancellation reaches,
	 * so the run is closed here rather than at each of the two dozen
	 * call sites that reach it -- none of which can be forgotten if
	 * the only way to fail is through this function.
	 */
	pkg_run_close(e);
}

/*
 * ADR-0256: stopped by an operator, not by its own merits. The same
 * stage, a different outcome -- which is exactly why status is a
 * second axis rather than another entry in the stage list.
 */
static void pkg_fail_cancelled(struct pkg_entry *e, int keep_installed,
                                enum pipeline_stage stage, const char *fmt, ...)
{
	va_list ap;

	if (e == NULL)
		return;
	va_start(ap, fmt);
	pkg_record_outcome(e, keep_installed, stage, PIPELINE_CANCELLED, fmt, ap);
	va_end(ap);
	rebuild_queue_remove(e->image);
}

static void pkg_fail(struct pkg_entry *e, int keep_installed, enum pipeline_stage stage,
                     const char *fmt, ...)
{
	va_list ap;

	if (e == NULL)
		return;
	va_start(ap, fmt);
	pkg_record_outcome(e, keep_installed, stage, PIPELINE_FAILED, fmt, ap);
	va_end(ap);

	/*
	 * This image cannot be brought up to date right now, so stop trying
	 * (#243).
	 *
	 * pkg_try_start_queued_rebuild() pops an image only when every
	 * manifest entry is already satisfied. A started-but-FAILED install
	 * therefore left the image at the front of the queue, still
	 * unsatisfied, to be started again on the very next pass -- with no
	 * attempt count, no backoff and no memory of the failure. A package
	 * that cannot install at all became an unbounded retry loop that
	 * never drained and never yielded the single job slot: measured at
	 * 219 consecutive gcc fetch failures against an unreachable
	 * upstream, with every other package operation refused 409 for the
	 * duration and a reboot the only escape.
	 *
	 * Cancelling does not help and it is worth saying why: cancel frees
	 * the slot correctly, and the drain immediately starts the next
	 * attempt on the same still-queued image.
	 *
	 * Dropped on ANY failure, not only one that leaves the package
	 * uninstalled. An upgrade failure keeps the old version installed
	 * -- which is exactly the gcc case -- and the image is no more
	 * satisfiable for it.
	 *
	 * Nothing is lost by dropping it: the failure is durably recorded
	 * on this entry (state, stage, status, error), which is the useful
	 * record, and a later publish re-queues the image naturally. The
	 * queue is deliberately not persisted for the same reason -- the
	 * intent is always re-derivable.
	 */
	rebuild_queue_remove(e->image);
}

static struct pkg_entry *pkg_find(const char *name, const char *image)
{
	int i;
	const char *norm_image = normalize_image(image);

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (g_packages[i].in_use && strcmp(g_packages[i].name, name) == 0 &&
		    strcmp(g_packages[i].image, norm_image) == 0)
			return &g_packages[i];
	}
	return NULL;
}

static void pkg_entry_free_files(struct pkg_entry *e)
{
	int i;

	for (i = 0; i < e->file_count; i++)
		free(e->files[i]);
	free(e->files);
	e->files = NULL;
	e->file_count = 0;
	e->files_cap = 0;
}

/* Unlinks every one of e's manifested files from rootfs -- does NOT
 * touch e->files itself (caller decides when to forget the list, e.g.
 * only once a replacement build has actually succeeded for an
 * in-place upgrade). Shared by pkg_delete() and the upgrade path in
 * pkg_build_completed(), both via image_produce_new_version()'s own
 * mutate() callback -- rootfs is always a scratch copy-forward staging
 * directory (ADR-0107/0108), never a version's own immutable,
 * possibly-already-in-use-by-a-running-container rootfs directly. */
/*
 * Does any OTHER installed package in the same image also claim `rel`?
 *
 * Issue #175. Two packages legitimately install the same path -- glibc
 * and linux-headers both own parts of usr/include, and any package
 * built from a shared upstream tree overlaps its siblings -- and the
 * LAST one installed is what is actually on disk. So removing a
 * package's manifest blindly deletes files that belong to a package
 * nobody touched.
 *
 * That is not hypothetical. Uninstalling libc-dev to make way for a
 * self-built glibc silently gutted linux-headers, which shares
 * usr/include with it, and the damage only surfaced later and
 * elsewhere:
 *
 *   build environment: copying usr/include/asm-generic/resource.h from
 *   declared tool linux-headers@6.18.40-4 (image cix-builder) failed:
 *   No such file or directory
 *
 * -- a package reported as installed, with a recorded manifest, whose
 * files were gone. Nothing failed at the time of the delete.
 *
 * Scoped to one image because that is the unit a manifest describes:
 * the same package installed into two images owns two independent sets
 * of files, and one image's contents say nothing about another's.
 */
static int path_claimed_by_another_package(const struct pkg_entry *self, const char *rel)
{
	const char *self_image = normalize_image(self->image);
	int i, f;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		const struct pkg_entry *o = &g_packages[i];

		if (o == self || !o->in_use || o->state != PKG_STATE_INSTALLED)
			continue;
		if (strcmp(normalize_image(o->image), self_image) != 0)
			continue;
		for (f = 0; f < o->file_count; f++) {
			if (strcmp(o->files[f], rel) == 0)
				return 1;
		}
	}
	return 0;
}

/*
 * Removes the files a package's manifest claims -- except any that
 * another installed package in the same image also claims, which are
 * left alone and reported (issue #175).
 *
 * Leaving a shared path behind is the conservative direction on
 * purpose. The file demonstrably belongs to something still installed,
 * so keeping it costs disk; deleting it breaks a package that was
 * never touched, in a way that surfaces far from the cause. Where two
 * packages disagree about a path's CONTENT the last install wins, as
 * it always has -- this changes only who may delete it.
 */
static void unlink_manifest_files(const struct pkg_entry *e, const char *rootfs)
{
	int i, kept = 0;

	for (i = 0; i < e->file_count; i++) {
		char path[PATH_MAX];

		if (path_claimed_by_another_package(e, e->files[i])) {
			kept++;
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", rootfs, e->files[i]);
		unlink(path);
	}
	if (kept > 0)
		logstore_write("cixd", "info",
		               "pkg %s@%s: kept %d of %d file(s) that another installed "
		               "package in this image also owns",
		               e->name, normalize_image(e->image), kept, e->file_count);
}

static int pkg_entry_add_file(struct pkg_entry *e, const char *relpath)
{
	/*
	 * NULL means "plain recursive copy, no manifest" -- e.g. the
	 * pkgbuild-sandbox-population caller (never a real package's own
	 * install). merge_tree()'s hostbuild caller (ADR-0056) passes a
	 * real e instead (issue #6) purely so GET can report what a
	 * hostbuild actually produced -- unlink_manifest_files() is never
	 * reached for that entry regardless (see that call site's own
	 * comment), so this still never becomes a real delete-manifest for
	 * a hostbuild artifact, just visibility.
	 */
	if (e == NULL)
		return 0;
	if (e->file_count >= e->files_cap) {
		int new_cap = e->files_cap == 0 ? 32 : e->files_cap * 2;
		char **new_files = realloc(e->files, (size_t)new_cap * sizeof(char *));

		if (new_files == NULL)
			return -1;
		e->files = new_files;
		e->files_cap = new_cap;
	}
	e->files[e->file_count] = strdup(relpath);
	if (e->files[e->file_count] == NULL)
		return -1;
	e->file_count++;
	return 0;
}

/*
 * Finds "<key>" at the start of a line in buf and returns a pointer
 * just past it, or NULL. Never executed/sourced -- pure text scan.
 */
static const char *find_key_line(const char *buf, const char *key)
{
	const char *p = buf;
	size_t keylen = strlen(key);
	int at_line_start = 1;

	while (*p != '\0') {
		if (at_line_start && strncmp(p, key, keylen) == 0)
			return p + keylen;
		at_line_start = (*p == '\n');
		p++;
	}
	return NULL;
}

static int extract_line_value(const char *buf, const char *key, char *out, size_t out_size)
{
	const char *val = find_key_line(buf, key);
	const char *end;
	size_t len;
	char quote = '\0';

	out[0] = '\0';
	if (val == NULL)
		return -1;
	if (*val == '"' || *val == '\'') {
		quote = *val;
		val++;
	}
	end = val;
	while (*end != '\0' && *end != '\n' && (quote == '\0' || *end != quote))
		end++;
	len = (size_t)(end - val);
	if (len >= out_size)
		return -1;
	memcpy(out, val, len);
	out[len] = '\0';
	return 0;
}

/* Splits raw (modified in place, same strtok_r(..., " \t", &save)
 * convention resolve_chain() already uses for pkg_depends) into up to
 * max_entries fixed-size strings at dest, elem_size apart -- shared by
 * both pkg_source=/pkg_sha256= tokenizing below (ADR-0036). Returns
 * the entry count, or -1 on overflow (too many entries, or one too
 * long for its own fixed-size slot) -- never silently truncates. */
static int tokenize_into(char *raw, char *dest, size_t elem_size, int max_entries)
{
	char *save;
	char *tok;
	int n = 0;

	tok = strtok_r(raw, " \t", &save);
	while (tok != NULL) {
		if (n >= max_entries || strlen(tok) >= elem_size)
			return -1;
		strcpy(dest + (size_t)n * elem_size, tok);
		n++;
		tok = strtok_r(NULL, " \t", &save);
	}
	return n;
}

/* Issue #60: the daemon's stored repo auth token (or "" if none) --
 * defined further down alongside g_repo_auth_token; forward-declared
 * here because parse_recipe() above the definition uses it. */
static const char *pkg_repo_token(void);

/*
 * Issue #60: replace every occurrence of needle with repl inside buf
 * (a NUL-terminated string in a fixed cap-byte array), in place. A
 * replacement that would overflow cap is skipped (buf left as far as
 * it got, still NUL-terminated) rather than truncating mid-token --
 * safe by construction here, where the only caller substitutes a
 * <=256-byte token into a 512-byte URL buffer. No allocation.
 */
static void str_replace_all(char *buf, size_t cap, const char *needle, const char *repl)
{
	size_t needle_len = strlen(needle);
	size_t repl_len = strlen(repl);
	char *p;

	if (needle_len == 0)
		return;
	while ((p = strstr(buf, needle)) != NULL) {
		size_t cur_len = strlen(buf);
		size_t tail_len = strlen(p + needle_len);

		if (cur_len - needle_len + repl_len >= cap)
			return;
		memmove(p + repl_len, p + needle_len, tail_len + 1);
		memcpy(p, repl, repl_len);
	}
}

/*
 * The daemon's own repo token, put back as the placeholder it came from.
 *
 * #405: the token is substituted into a source URL by parse_recipe()
 * and must never leave this process in any other form. It did.
 * GET /v1/pkg/recipes/{name} serves a recipe's raw stored bytes, it
 * needs no authentication, and 24 stored recipes on 192.168.15.95
 * carried the live token in their own text -- so a token with push
 * access to the private repository was readable with one curl and no
 * credentials. Measured 2026-09-11: cix v2.55.12 through v2.57.7,
 * kernel 7.2.3-3 through -7, and two probe recipes.
 *
 * Those were written by hand before #60 gave recipes {{REPO_TOKEN}};
 * the current kernel recipe and every cix release from v2.57.8 already
 * carry the placeholder, so nothing writes them any more. That is why
 * the redaction is on BOTH sides of persistence and not just one. On
 * the way in, so a recipe carrying a live token is never stored again
 * -- the write path is fixed by convention today and a convention is
 * not a guarantee. On the way out, so the recipes that already exist
 * stop being served: ADR-0107 makes a published recipe immutable, so
 * those files stay exactly as they are and the endpoint stops handing
 * out what is inside them.
 *
 * Shrinks in place -- a token is 40 characters and the placeholder is
 * 14, so no reallocation and no truncation is possible here.
 */
static void redact_repo_token(char *buf, size_t cap)
{
	const char *tok = pkg_repo_token();

	if (buf == NULL || tok == NULL || tok[0] == '\0')
		return;
	str_replace_all(buf, cap, tok, "{{REPO_TOKEN}}");
}

/*
 * Issue #60/#405: substitutes the daemon's own stored repo token for
 * the {{REPO_TOKEN}} placeholder in every source URL.
 *
 * Shared by every recipe format deliberately, and that is the point
 * rather than tidiness. This is what lets a recipe that self-fetches
 * from the private Gitea be committed in its final, working form, and
 * the substituted URL exists only in this transient parsed struct --
 * never persisted, never logged. A format that skipped it would fetch
 * with the literal placeholder and fail with a 404 naming a URL that
 * has "{{REPO_TOKEN}}" in it, which reads as a broken recipe rather
 * than a missing step. One function means a new format cannot skip it
 * by omission.
 *
 * A recipe with no placeholder, or an empty stored token, is left
 * byte-for-byte unchanged.
 */
static void substitute_repo_token(struct pkg_recipe *out)
{
	const char *tok = pkg_repo_token();
	int i;

	if (tok == NULL || tok[0] == '\0')
		return;
	for (i = 0; i < out->source_count; i++)
		str_replace_all(out->source[i], sizeof(out->source[i]), "{{REPO_TOKEN}}", tok);
}

static int parse_shell_recipe(const char *path, struct pkg_recipe *out)
{
	char *buf;
	size_t len;
	int rc = 0;
	char raw_source[PKG_MAX_SOURCES * PKG_URL_MAX];
	char raw_sha256[PKG_MAX_SOURCES * PKG_SHA256_MAX];
	int sha256_count;

	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return -1;

	memset(out, 0, sizeof(*out));
	if (extract_line_value(buf, "pkg_name=", out->name, sizeof(out->name)) != 0)
		rc = -1;
	if (extract_line_value(buf, "pkg_version=", out->version, sizeof(out->version)) != 0)
		rc = -1;
	if (extract_line_value(buf, "pkg_source=", raw_source, sizeof(raw_source)) != 0)
		rc = -1;
	if (extract_line_value(buf, "pkg_sha256=", raw_sha256, sizeof(raw_sha256)) != 0)
		rc = -1;
	/* depends, artifact_sha256, and changelog are all optional -- fine
	 * if absent */
	extract_line_value(buf, "pkg_depends=", out->depends, sizeof(out->depends));
	extract_line_value(buf, "pkg_build_depends=", out->build_depends, sizeof(out->build_depends));
	extract_line_value(buf, "pkg_build_caps=", out->build_caps, sizeof(out->build_caps));
	extract_line_value(buf, "pkg_artifact_sha256=", out->artifact_sha256,
	                    sizeof(out->artifact_sha256));
	extract_line_value(buf, "pkg_upstream=", out->upstream, sizeof(out->upstream));
	extract_line_value(buf, "pkg_changelog=", out->changelog, sizeof(out->changelog));
	free(buf);

	if (rc == 0) {
		out->source_count = tokenize_into(raw_source, (char *)out->source, PKG_URL_MAX,
		                                   PKG_MAX_SOURCES);
		sha256_count =
		    tokenize_into(raw_sha256, (char *)out->sha256, PKG_SHA256_MAX, PKG_MAX_SOURCES);
		/* Positionally paired (ADR-0036) -- a mismatched count is a
		 * real, non-negotiable recipe error, not silently zipped
		 * short against whichever list is shorter. */
		if (out->source_count <= 0 || sha256_count <= 0 || out->source_count != sha256_count)
			rc = -1;
	}

	/*
	 * Issue #60: substitute the {{REPO_TOKEN}} placeholder in each
	 * source URL with the daemon's own stored repo auth token
	 * (pkg repo-config --token=). This is what lets a recipe that
	 * self-fetches from the private Gitea be committed in its final,
	 * working form -- no more the temp-real-token-substitute-then-
	 * revert dance the kernel/cix recipes needed on every re-pin
	 * (documented at length in remote-development.md). The token is
	 * never persisted into any recipe or the catalog, and the
	 * substituted URL only ever exists in this transient parsed struct,
	 * handed straight to the fetch child -- redacted from logs the same
	 * way the whole source string already is not echoed on success.
	 * A recipe with no {{REPO_TOKEN}} token, or an empty stored token,
	 * is left byte-for-byte unchanged.
	 */
	substitute_repo_token(out);

	if (rc != 0 || !pkg_name_is_valid(out->name))
		return -1;
	return 0;
}

/*
 * Runs `cbs explain --json` over one CPDL document and returns its
 * stdout (ADR-0305).
 *
 * This is the ONLY place this daemon learns what a PBS recipe
 * declares, and it runs exactly once per published version, at
 * publish. cixd does not parse CPDL: a second parser here would be a
 * parallel implementation of the language being adopted, and the two
 * would diverge on exactly the documents where the grammar is subtle.
 *
 * Two pipes, because the interesting output is on both streams and
 * merging them would corrupt the JSON with a warning. On success
 * stdout carries the document and stderr is empty; on failure stdout
 * is empty and stderr carries a CPDL diagnostic naming the error code
 * and line (CPDL-E3001 and friends), which is the single most useful
 * thing an operator can be handed. Draining them in sequence cannot
 * deadlock while one of the two is empty, which is the only shape
 * `explain` produces -- and both would have to exceed a 64 KB pipe
 * buffer for it to matter.
 *
 * The diagnostic is put in err rather than left on stderr
 * deliberately: this daemon's stderr is not mirrored into the log
 * store, so anything written there is simply lost (the mistake #132
 * was, and the one the project's own notes warn about).
 */
static int run_cbs_explain(const char *recipe_path, char *out, size_t out_size, size_t *out_len,
                            char *err, size_t err_size)
{
	int outfd[2];
	int errfd[2];
	pid_t pid;
	int status = 0;
	size_t total = 0;
	int truncated;

	if (out_size == 0)
		return -1;
	out[0] = '\0';
	if (out_len != NULL)
		*out_len = 0;

	if (access(PKG_CBS_BIN, X_OK) != 0) {
		snprintf(err, err_size,
		         "this host has no CPDL engine: %s is absent. Install it with `pkg install "
		         "--image=cix-hosttools cbs` and redeploy the control plane, which is what "
		         "stages it (ADR-0305)",
		         PKG_CBS_BIN);
		return -1;
	}
	if (pipe2(outfd, O_CLOEXEC) != 0) {
		snprintf(err, err_size, "could not create a pipe for cbs: %s", strerror(errno));
		return -1;
	}
	if (pipe2(errfd, O_CLOEXEC) != 0) {
		snprintf(err, err_size, "could not create a pipe for cbs: %s", strerror(errno));
		close(outfd[0]);
		close(outfd[1]);
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		snprintf(err, err_size, "could not fork to run cbs: %s", strerror(errno));
		close(outfd[0]);
		close(outfd[1]);
		close(errfd[0]);
		close(errfd[1]);
		return -1;
	}
	if (pid == 0) {
		char *argv[5];

		argv[0] = (char *)"cbs";
		argv[1] = (char *)"explain";
		argv[2] = (char *)recipe_path;
		argv[3] = (char *)"--json";
		argv[4] = NULL;
		dup2(outfd[1], STDOUT_FILENO);
		dup2(errfd[1], STDERR_FILENO);
		execve(PKG_CBS_BIN, argv, environ);
		_exit(127);
	}
	close(outfd[1]);
	close(errfd[1]);

	while (total + 1 < out_size) {
		ssize_t n = read(outfd[0], out + total, out_size - total - 1);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}
	out[total] = '\0';
	close(outfd[0]);

	/*
	 * A filled buffer means the document was cut off, and a truncated
	 * JSON object fails to parse with a message about syntax rather
	 * than about size. Noted here and reported below, after the single
	 * wait -- not returned from early, so that this function has
	 * exactly ONE waitpid() on every path (ADR-0247's budget, and
	 * test_blocking_waits' own rule that a raise has to argue for
	 * itself: one bounded wait is easier to argue for than two).
	 */
	truncated = total + 1 >= out_size;

	{
		char diag[512];
		size_t dtotal = 0;

		while (dtotal + 1 < sizeof(diag)) {
			ssize_t n = read(errfd[0], diag + dtotal, sizeof(diag) - dtotal - 1);

			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (n == 0)
				break;
			dtotal += (size_t)n;
		}
		diag[dtotal] = '\0';
		close(errfd[0]);

		if (waitpid(pid, &status, 0) != pid) {
			snprintf(err, err_size, "could not wait for cbs: %s", strerror(errno));
			out[0] = '\0';
			return -1;
		}
		if (truncated) {
			snprintf(err, err_size, "cbs explain produced more than %zu bytes of output",
			         out_size - 1);
			out[0] = '\0';
			return -1;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			size_t i;

			/* Collapse newlines so a multi-line CPDL diagnostic
			 * survives a single-line error field and one log entry --
			 * the same treatment diskpart gives sfdisk's. */
			for (i = 0; i < dtotal; i++)
				if (diag[i] == '\n' || diag[i] == '\r')
					diag[i] = ' ';
			if (!WIFEXITED(status))
				snprintf(err, err_size, "cbs explain was killed by a signal");
			else if (WEXITSTATUS(status) == 127)
				snprintf(err, err_size, "cbs explain could not be executed (%s)",
				         PKG_CBS_BIN);
			else if (dtotal > 0)
				snprintf(err, err_size, "%s", diag);
			else
				snprintf(err, err_size, "cbs explain exited %d with no diagnostic",
				         WEXITSTATUS(status));
			out[0] = '\0';
			return -1;
		}
	}

	if (out_len != NULL)
		*out_len = total;
	return 0;
}

/*
 * Is this path a PBS recipe (ADR-0305)?
 *
 * Not a heuristic: the filename IS the format, so this reads the one
 * source of truth rather than guessing from content. `.cbs` is CBS's
 * own requirement and not our preference -- has_cbs_extension()
 * refuses any other path, in both `cbs explain` and `cbs build`.
 */
static int recipe_path_is_pbs(const char *path)
{
	size_t len;

	if (path == NULL)
		return 0;
	len = strlen(path);
	if (len < sizeof(PKG_RECIPE_PBS_SUFFIX) - 1)
		return 0;
	return strcmp(path + len - (sizeof(PKG_RECIPE_PBS_SUFFIX) - 1), PKG_RECIPE_PBS_SUFFIX) == 0;
}

/*
 * The derived-identity file beside a build.cbs.
 *
 * `cbs explain --json` runs exactly once per published version, at
 * publish, and its output is stored here (ADR-0305). Re-running it on
 * every read would fork a child once per recipe file, and there are
 * ~1400 of them in this repo; #236 already measured the event loop
 * blocked for 10981 ms doing far cheaper shell parses on a comparable
 * walk, which needed the host reset by hand. Twice.
 *
 * Derived state beside a source of truth is normally forbidden here.
 * It is correct in this one case because ADR-0107 makes a published
 * (name, version) immutable, so the document this file describes
 * cannot change under it.
 */
static void pbs_explain_path(const char *recipe_path, char *out, size_t out_size)
{
	const char *slash = strrchr(recipe_path, '/');

	if (slash == NULL) {
		snprintf(out, out_size, "explain.json");
		return;
	}
	snprintf(out, out_size, "%.*s/explain.json", (int)(slash - recipe_path), recipe_path);
}

/* Appends a space-separated word list to dst, which may already hold
 * one. Returns -1 if the result would not fit -- never a truncated
 * tool list, which would compose a build environment missing exactly
 * the tools at the end of the declaration. */
static int append_words(char *dst, size_t cap, const char *words)
{
	size_t used = strlen(dst);
	int n;

	if (words == NULL || words[0] == '\0')
		return 0;
	n = snprintf(dst + used, cap - used, "%s%s", used > 0 ? " " : "", words);
	if (n < 0 || (size_t)n >= cap - used)
		return -1;
	return 0;
}

/*
 * Maps a PBS recipe's declarations onto this daemon's own fields, out
 * of the explain.json written at publish (ADR-0305).
 *
 * Three fields are deliberately left EMPTY rather than invented, each
 * for a reason filed upstream:
 *
 *   build_caps       `cbs explain --json` reports a capability COUNT,
 *                    not the names (cix-build-system#162), and a count
 *                    of 1 does not say whether the recipe asked for
 *                    CAP_SYS_ADMIN or CAP_NET_ADMIN. A non-zero count
 *                    is refused at PUBLISH rather than dropped here,
 *                    so a recipe whose capability would go missing
 *                    never becomes a stored file.
 *   artifact_sha256  CPDL rejects unknown package keys, so there is
 *                    nowhere in a build.cbs to approve one specific
 *                    published byte sequence (cix-build-system#161).
 *                    The consequence is real and is not hidden: a PBS
 *                    recipe cannot take a cache hit and rebuilds from
 *                    source on every install.
 *   changelog        Same gap, same ticket. Reports as null rather
 *                    than as an empty string pretending to be one.
 *
 * `bootstrap` tools are folded into build_depends alongside `build`
 * ones because CBS itself treats them as build-time: its
 * role_selected() applies both roles to every phase except install.
 * A name appearing in both roles is harmless -- buildenv_add_tool()
 * dedups by name, and a recipe's own pin still wins the slot.
 */
static int parse_pbs_recipe(const char *path, struct pkg_recipe *out)
{
	char explain_path[PATH_MAX];
	char err[256];
	char *buf = NULL;
	size_t len = 0;
	struct pbs_explain *ex;
	int count;
	int i;

	pbs_explain_path(path, explain_path, sizeof(explain_path));
	if (persist_read_file(explain_path, &buf, &len) != 0 || buf == NULL) {
		logstore_write("cixd", "error",
		               "pkg: %s has no derived identity beside it (%s) -- it was not "
		               "published by this daemon",
		               path, explain_path);
		return -1;
	}
	ex = pbs_explain_parse(buf, len, err, sizeof(err));
	free(buf);
	if (ex == NULL) {
		logstore_write("cixd", "error", "pkg: %s: %s", explain_path, err);
		return -1;
	}

	snprintf(out->name, sizeof(out->name), "%s", pbs_explain_name(ex));
	if (pbs_explain_version(ex, out->version, sizeof(out->version)) != 0) {
		logstore_write("cixd", "error", "pkg: %s: version and release do not fit a version string",
		               explain_path);
		pbs_explain_free(ex);
		return -1;
	}

	count = pbs_explain_source_count(ex);
	if (count <= 0 || count > PKG_MAX_SOURCES) {
		logstore_write("cixd", "error", "pkg: %s: %d sources, but 1..%d is supported",
		               explain_path, count, PKG_MAX_SOURCES);
		pbs_explain_free(ex);
		return -1;
	}
	for (i = 0; i < count; i++) {
		if (pbs_explain_source(ex, i, out->source[i], PKG_URL_MAX, out->sha256[i],
		                        PKG_SHA256_MAX) != 0) {
			logstore_write("cixd", "error", "pkg: %s: source %d has no usable url/sha256 pair",
			               explain_path, i);
			pbs_explain_free(ex);
			return -1;
		}
	}
	out->source_count = count;

	if (pbs_explain_requires(ex, "runtime", "package", out->depends, sizeof(out->depends)) != 0) {
		logstore_write("cixd", "error", "pkg: %s: the runtime package list does not fit",
		               explain_path);
		pbs_explain_free(ex);
		return -1;
	}

	out->build_depends[0] = '\0';
	{
		static const char *const roles[] = { "build", "bootstrap" };
		static const char *const kinds[] = { "compiler", "tool" };
		size_t ri;
		size_t ki;

		for (ri = 0; ri < sizeof(roles) / sizeof(roles[0]); ri++) {
			for (ki = 0; ki < sizeof(kinds) / sizeof(kinds[0]); ki++) {
				char one[PKG_DEPENDS_MAX];

				if (pbs_explain_requires(ex, roles[ri], kinds[ki], one, sizeof(one)) != 0 ||
				    append_words(out->build_depends, sizeof(out->build_depends), one) != 0) {
					logstore_write("cixd", "error",
					               "pkg: %s: the %s/%s list does not fit the build "
					               "declaration",
					               explain_path, roles[ri], kinds[ki]);
					pbs_explain_free(ex);
					return -1;
				}
			}
		}
	}

	snprintf(out->upstream, sizeof(out->upstream), "%s", pbs_explain_upstream(ex));

	pbs_explain_free(ex);
	return 0;
}

/*
 * One recipe, whichever language it is written in (ADR-0305).
 *
 * Every caller in this file goes through here and none of them knows
 * which format it got, which is the whole design: a PBS recipe reaches
 * dependency resolution, the build container, the artifact name and
 * the pipeline as the same struct a shell recipe does. The formats
 * differ in how a declaration is WRITTEN, never in how it is resolved
 * -- resolution stays resolve_chain() and buildenv_add_tool().
 */
static int parse_recipe(const char *path, struct pkg_recipe *out)
{
	if (recipe_path_is_pbs(path)) {
		memset(out, 0, sizeof(*out));
		out->is_pbs = 1;
		if (parse_pbs_recipe(path, out) != 0)
			return -1;
		substitute_repo_token(out);
		if (!pkg_name_is_valid(out->name))
			return -1;
		return 0;
	}
	return parse_shell_recipe(path, out);
}

/*
 * Natural-sort/dpkg-style version comparison (ADR-0107): walks both
 * strings left to right, alternating between runs of digits (compared
 * numerically) and runs of non-digits (compared byte-wise). See pkg.h's
 * own doc comment for the two real recipes ("v1.4.0", "1.5.8.pl02")
 * that ruled out a naive per-component atoi() split.
 */
int pkg_version_compare(const char *a, const char *b)
{
	while (*a != '\0' || *b != '\0') {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			long na = 0, nb = 0;

			while (isdigit((unsigned char)*a)) {
				na = na * 10 + (*a - '0');
				a++;
			}
			while (isdigit((unsigned char)*b)) {
				nb = nb * 10 + (*b - '0');
				b++;
			}
			if (na != nb)
				return (na < nb) ? -1 : 1;
		} else {
			unsigned char ca = (unsigned char)*a;
			unsigned char cb = (unsigned char)*b;

			if (ca != cb)
				return (ca < cb) ? -1 : 1;
			if (ca != '\0') {
				a++;
				b++;
			}
		}
	}
	return 0;
}

/*
 * The recipe file inside one version directory, whichever language it
 * is written in (ADR-0305): build.sh for a shell recipe, build.cbs
 * for a PBS one. Fills out_path and, when out_created is non-NULL,
 * the file's mtime -- which ADR-0107 immutability makes a real
 * "first published" timestamp (see recipe_created_at()).
 *
 * Returns -1 when the directory holds neither, and ALSO when it holds
 * both. A version carrying two recipes has no single answer to "what
 * will this build do", and this function deliberately refuses to be
 * the place that picks one: choosing by if-order, invisibly, is
 * precisely the defect ADR-0304 was written about. Publish refuses
 * the second format, so reaching this state means something wrote the
 * recipes directory behind the daemon's back -- which is worth a log
 * line rather than a silent preference.
 */
int pkg_recipe_file_in(const char *version_dir, char *out_path, size_t out_path_size,
                        long *out_created, const char **out_filename)
{
	char shell_path[PATH_MAX];
	char pbs_path[PATH_MAX];
	struct stat shell_st;
	struct stat pbs_st;
	int have_shell;
	int have_pbs;

	snprintf(shell_path, sizeof(shell_path), "%s/%s", version_dir, PKG_RECIPE_SHELL_FILE);
	have_shell = stat(shell_path, &shell_st) == 0 && S_ISREG(shell_st.st_mode);
	snprintf(pbs_path, sizeof(pbs_path), "%s/%s", version_dir, PKG_RECIPE_PBS_FILE);
	have_pbs = stat(pbs_path, &pbs_st) == 0 && S_ISREG(pbs_st.st_mode);

	if (have_shell && have_pbs) {
		logstore_write("cixd", "error",
		               "pkg: %s holds both a build.sh and a build.cbs -- refusing to choose "
		               "a recipe language (ADR-0305)",
		               version_dir);
		return -1;
	}
	if (have_shell) {
		snprintf(out_path, out_path_size, "%s", shell_path);
		if (out_created != NULL)
			*out_created = (long)shell_st.st_mtime;
		if (out_filename != NULL)
			*out_filename = PKG_RECIPE_SHELL_FILE;
		return 0;
	}
	if (have_pbs) {
		snprintf(out_path, out_path_size, "%s", pbs_path);
		if (out_created != NULL)
			*out_created = (long)pbs_st.st_mtime;
		if (out_filename != NULL)
			*out_filename = PKG_RECIPE_PBS_FILE;
		return 0;
	}
	return -1;
}

/*
 * Resolves name (and optional specific version) to the path of its
 * recipe under ADR-0107's version-keyed layout:
 * <g_recipes_dir>/<name>/<version>/build.sh, or build.cbs for a PBS
 * recipe (ADR-0305 -- pkg_recipe_file_in() above decides which, and it is
 * the only place that does). version NULL or ""
 * resolves to the highest available version for name
 * (pkg_version_compare()-ordered) -- the "rolling implicit" default
 * every pre-existing, non-manifest-aware caller (plain `pkg install
 * NAME`, dependency resolution, hostbuild, update-candidate checks)
 * relies on. Returns 0 and fills out_path on success, -1 if name has
 * no published recipe at all (or the specific requested version
 * doesn't exist).
 */
static int find_recipe_path(const char *name, const char *version, char *out_path,
                             size_t out_path_size)
{
	char name_dir[PATH_MAX];
	char version_dir[PATH_MAX];

	snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, name);

	if (version != NULL && version[0] != '\0') {
		snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, version);
		return pkg_recipe_file_in(version_dir, out_path, out_path_size, NULL, NULL);
	}

	/*
	 * Issue #64: which version an omitted version resolves to is a
	 * per-package operator policy, resolved HERE and nowhere else --
	 * this is the one function every implicit resolution in this daemon
	 * goes through (plain install, dependency resolution, hostbuild,
	 * update-candidate checks, follow_rolling rebuilds), so a policy
	 * applied here is applied everywhere by construction rather than by
	 * remembering to consult it in thirteen call sites.
	 */
	{
		enum pkg_policy_kind policy;
		char pinned[PKG_VERSION_MAX];

		policy = pkgpolicy_get(name, pinned, sizeof(pinned));
		if (policy == PKG_POLICY_PINNED && pinned[0] != '\0') {
			/* A pin is a HOLD: if the pinned version is not published,
			 * that is an error, not a reason to drift to another one.
			 * Silently resolving elsewhere is the exact behaviour a pin
			 * exists to prevent. */
			snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, pinned);
			return pkg_recipe_file_in(version_dir, out_path, out_path_size, NULL, NULL);
		}

		{
			DIR *d = opendir(name_dir);
			struct dirent *de;
			char best[PKG_VERSION_MAX];
			long best_created = 0;
			int have_best = 0;

			if (d == NULL)
				return -1;
			while ((de = readdir(d)) != NULL) {
				char candidate[PATH_MAX];
				long created = 0;
				int wins;

				if (de->d_name[0] == '.')
					continue;
				snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, de->d_name);
				if (pkg_recipe_file_in(version_dir, candidate, sizeof(candidate), &created, NULL) != 0)
					continue;
				if (policy == PKG_POLICY_NEWEST) {
					/* A recipe version's file is written exactly once
					 * and never touched again (ADR-0107 immutability),
					 * so its mtime really is "first published" -- see
					 * recipe_created_at()'s own comment. Ties fall back
					 * to the version order, so the answer is stable
					 * rather than dependent on readdir() order. */
					wins = !have_best || created > best_created ||
					       (created == best_created &&
					        pkg_version_compare(de->d_name, best) > 0);
				} else {
					wins = !have_best || pkg_version_compare(de->d_name, best) > 0;
				}
				if (wins) {
					snprintf(best, sizeof(best), "%s", de->d_name);
					best_created = created;
					have_best = 1;
				}
			}
			closedir(d);
			if (!have_best)
				return -1;
			snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, best);
			return pkg_recipe_file_in(version_dir, out_path, out_path_size, NULL, NULL);
		}
	}
}

/*
 * The discovery kind a package's recipe declares, or "" if it declares
 * none (ADR-0255).
 *
 * Resolves the same way every other omitted-version lookup does, so the
 * answer is about the recipe this box would actually build. A package
 * with no recipe at all is not an error here -- it simply does not
 * roll, which is the same outcome as declaring no upstream and is what
 * the caller has to handle either way.
 */
int pkg_recipe_upstream(const char *name, char *out, size_t out_size)
{
	char recipe_path[PATH_MAX];
	struct pkg_recipe recipe;

	if (out == NULL || out_size == 0)
		return -1;
	out[0] = '\0';
	if (name == NULL || !pkg_name_is_valid(name))
		return -1;
	if (find_recipe_path(name, NULL, recipe_path, sizeof(recipe_path)) != 0)
		return -1;
	if (parse_recipe(recipe_path, &recipe) != 0)
		return -1;
	snprintf(out, out_size, "%s", recipe.upstream);
	return 0;
}


/*
 * A recipe version's own file is written exactly once
 * (persist_atomic_write(), never rewritten -- ADR-0107's immutability
 * rule) and never copied/hardlinked/touched again by anything in this
 * codebase, so its own mtime *is* an accurate, permanent "first
 * published" timestamp with no separate persisted field needed
 * (ADR-0108). Returns 0 (unknown/stat failed) rather than failing the
 * caller -- a missing timestamp is display-only, never load-bearing.
 */
static long recipe_created_at(const char *recipe_path)
{
	struct stat st;

	if (stat(recipe_path, &st) != 0)
		return 0;
	return (long)st.st_mtime;
}

/*
 * FICLONE, spelled out rather than included (#236).
 *
 * <linux/fs.h> is exactly the kind of kernel uapi header CLAUDE.md
 * warns about pulling in beside glibc's own, and this is one constant
 * with a stable, documented value. Declaring it here costs a line and
 * avoids a header fight for no benefit.
 */
#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif

/*
 * Copies one file, by reference where the filesystem can (#236).
 *
 * This is the innermost operation of composing a build environment, and
 * composing one copies every file of every declared tool -- for a
 * toolchain that is gcc, glibc and binutils, thousands of files and
 * hundreds of megabytes. It runs in the daemon's own event loop, so
 * every byte moved here is a byte during which nothing else is served.
 * Measured: publishing one recipe that made an image unsatisfied took
 * 12257 ms and made the host need a manual reset twice.
 *
 * Every image on this platform lives under the same rebuildable-storage
 * filesystem, which is btrfs (ADR-0207), so source and destination are
 * on one filesystem that supports reflinks. FICLONE then shares the
 * extents instead of duplicating them: the new file is a full,
 * independent copy semantically -- copy-on-write handles later
 * divergence -- at the cost of metadata rather than data.
 *
 * The read/write loop stays as the fallback and is unchanged. FICLONE
 * fails cleanly (EOPNOTSUPP, EXDEV, EINVAL) on a filesystem that cannot
 * do it or across two that differ, which is exactly the dev sandbox and
 * any future non-btrfs deployment. Nothing depends on which path ran:
 * the resulting file is identical either way.
 */
static int copy_file_simple(const char *src, const char *dst)
{
	int in, out;
	char buf[4096];
	ssize_t n;

	in = open(src, O_RDONLY);
	if (in < 0)
		return -1;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (out < 0) {
		close(in);
		return -1;
	}
	if (ioctl(out, FICLONE, in) == 0) {
		close(in);
		close(out);
		return 0;
	}
	/* Not clonable here -- same file, moved the long way. */
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			close(in);
			close(out);
			return -1;
		}
	}
	close(in);
	close(out);
	return n < 0 ? -1 : 0;
}

/*
 * ADR-0251 -- the one definition of what a package artifact carries.
 *
 * This lives here, in the daemon, rather than in any recipe, because a
 * recipe convention is exactly what produced the mess it replaces: of
 * 115 recipes, 37 pruned anything at all and they did it in twenty-one
 * different spellings, while glibc shipped libc.so.6 with 9.46 MiB of
 * debug sections and libc.a three times over. The scope of a universal
 * rule was being re-guessed 115 times.
 *
 * Written into the build container beside recipe.sh and sourced by the
 * build command after pkg_install() returns.
 *
 * The policy text itself is shell, and lives in its own file
 * (daemon/policy/pkg-finalize.sh) rather than as an escaped string
 * here, so it can be read, reviewed and run directly -- test_pkg_finalize
 * executes that same file. It is generated into a C string so the policy
 * travels inside the daemon binary and can never be missing at runtime.
 * Same posture as generated/api_routes.h beside it: one source, one
 * generated consumer.
 */
#include "generated/pkg_finalize.h"

/*
 * Stage PKG_FINALIZE_SH into a build container's own /build.
 *
 * Same shape as the recipe.sh copy beside it, and called from the same
 * two places, so a build can never run with one and not the other.
 */
static int write_finalize_script(const char *upperdir)
{
	char path[PATH_MAX];
	size_t len = sizeof(PKG_FINALIZE_SH) - 1;
	ssize_t n;
	int fd;

	if ((size_t)snprintf(path, sizeof(path), "%s/build/finalize.sh", upperdir) >= sizeof(path))
		return -1;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	n = write(fd, PKG_FINALIZE_SH, len);
	if (close(fd) != 0 || n < 0 || (size_t)n != len)
		return -1;
	return 0;
}

/*
 * The one build command, used by both the ordinary build path and the
 * hostbuild path. It was written out twice, identically, which is the
 * same duplication ADR-0251 is about: two copies of a rule is two
 * chances for them to diverge.
 *
 * `set -e`, and semicolons rather than `&&`, both deliberately.
 *
 * Without set -e a command that fails partway through pkg_build() or
 * pkg_install() does not fail the package: the function keeps going and
 * returns the status of whatever ran last. libcap 2.78-3 shipped that
 * way -- `make install` died with Error 2, the `rm -rf` after it
 * succeeded, and the package was recorded as installed with four of its
 * binaries missing. A package that quietly contains less than it should
 * is worse than one that fails, because nothing downstream can tell.
 *
 * The separators matter as much as the flag. POSIX suspends set -e for
 * any command that is part of an && list except the last, and that
 * suspension applies inside a function called from there too -- so
 * `pkg_build && pkg_install` would leave every failure inside
 * pkg_build() ignored, which is precisely the case that needs catching.
 *
 * finalize.sh runs last and is sourced, not executed, so a non-zero
 * return from it fails the build under the same set -e.
 */
#define PKG_BUILD_CMD \
	"set -e; . /build/recipe.sh; cd /build/src; pkg_build; pkg_install; " \
	". /build/finalize.sh"

/*
 * A PBS recipe's build (ADR-0305).
 *
 * `cbs build --staged W` treats W as a WORKSPACE, not as the staged
 * tree: it creates W/src, W/build, W/dest and W/cache under it, and an
 * install phase's ${dest} is W/dest. That is why PKG_DESTDIR is set
 * per-entry rather than being the fixed /build/pkg-dest a shell recipe
 * uses -- pointing the variable at the subdirectory costs one
 * assignment, where moving the tree afterwards would have made every
 * PBS recipe depend on mv and rmdir being among the tools it declared.
 *
 * No --output, deliberately: this is stage 1, cixd still finalizes and
 * packages exactly as it does for a shell recipe, and CBS writes no
 * artifact when no package path is given. --cache points at the
 * directory cixd fills with the sources it has already fetched and
 * checksum-verified, so CBS finds every source as a cache hit and
 * never reaches for the network -- it fetches over a dlopen()ed
 * libcurl otherwise, and the build container has neither a route nor
 * the repo token, nor should it.
 *
 * --events human, and this was learned the hard way rather than
 * chosen: without it a failing phase prints NOTHING. The first PBS
 * build on a real host produced a two-line log -- cbs's own "build
 * failed: recipe, staged tree, or package output was rejected" and
 * nothing else -- which is the same defect as #484's silent kernel
 * link, in a path written the same afternoon that issue was filed.
 * A build that cannot say why it failed is not finished work.
 *
 * "human" rather than "jsonl": the event stream is a dup of stderr,
 * which is exactly where cixd's build log reads from, so jsonl would
 * interleave machine records into the text an operator reads. Ingesting
 * the jsonl properly -- phase timings and cache-hit counts into the
 * REST progress fields -- is worth doing and is not this.
 *
 * finalize.sh runs last and is sourced, under the same set -e, exactly
 * as in the shell form above.
 */
/*
 * Forward declaration: the architecture is passed to `cbs build`
 * (--arch), and the build container is prepared some six thousand
 * lines above where this is defined. Declared rather than moved --
 * pkg_host_arch() sits with the artifact-naming code it exists for,
 * and #183's own comment about being the single place a future port
 * would learn a mapping is worth more than adjacency to one caller.
 */
static const char *pkg_host_arch(void);

#define PKG_CBS_WORKSPACE "/build/cbsws"
#define PKG_CBS_CACHE_DIR "/build/cbscache"

/*
 * One file of a package's own recorded manifest, copied into a rootfs
 * being composed (issue #109). Unlike copy_file_simple() above this has
 * to reproduce the thing faithfully rather than just move bytes: a
 * build environment whose compiler lost its executable bit, or whose
 * `cc` symlink became a copy of `gcc`, is not the environment the
 * package declared.
 *
 * Missing parents are created, because a manifest lists files and not
 * the directories on the way to them.
 */
static int copy_one_file_preserving(const char *src, const char *dst)
{
	struct stat st;
	char parent[PATH_MAX];
	char *slash;

	if (lstat(src, &st) != 0)
		return -1;

	snprintf(parent, sizeof(parent), "%s", dst);
	slash = strrchr(parent, '/');
	if (slash != NULL) {
		*slash = '\0';
		if (persist_mkdir_p(parent) != 0)
			return -1;
	}

	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		ssize_t n = readlink(src, target, sizeof(target) - 1);

		if (n < 0)
			return -1;
		target[n] = '\0';
		unlink(dst);
		return symlink(target, dst);
	}
	if (S_ISDIR(st.st_mode))
		return persist_mkdir_p(dst);
	if (!S_ISREG(st.st_mode)) {
		/* Device nodes and the like come from pkg_seed_image_baseline(),
		 * not from a package's manifest -- one arriving here is a
		 * surprise worth failing on rather than silently skipping. */
		return -1;
	}
	if (copy_file_simple(src, dst) != 0)
		return -1;
	return chmod(dst, st.st_mode & 07777);
}

/*
 * Issue #132: how much of a failing child's stderr to keep.
 *
 * A tool explains its own failures on stderr, and this daemon threw all
 * of it away -- so the log recorded THAT something failed and never
 * WHY. On a host with no shell that is unrecoverable: the explanation
 * existed, was printed, and could not be reached by any API.
 *
 * The cost was real and repeated. "unsquashfs: exited with status 2"
 * took four rebuild-and-retry cycles over a 1.4 GB image and the actual
 * trigger was still never identified -- unsquashfs names the file and
 * the reason for every non-fatal error it hits, on the stream that was
 * being discarded. Two other diagnoses the same session were slow for
 * exactly this reason.
 *
 * A few KB is plenty: the useful part of a tool's complaint is at the
 * end (the last thing it said before giving up), so when output
 * overflows the buffer the TAIL is kept, not the head.
 */
#define RUN_SUBPROCESS_ERR_MAX 4096

static int run_subprocess(const char *bin, char *const argv[])
{
	pid_t pid;
	int status;
	int errpipe[2];
	char errbuf[RUN_SUBPROCESS_ERR_MAX];
	size_t errlen = 0;

	errbuf[0] = '\0';
	if (pipe2(errpipe, O_CLOEXEC) != 0) {
		/* Not fatal -- losing the diagnostic is bad, losing the whole
		 * operation because of it would be worse. */
		errpipe[0] = -1;
		errpipe[1] = -1;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		logstore_write("cixd", "error", "run_subprocess %s: fork failed: %s", bin,
		                strerror(errno));
		if (errpipe[0] >= 0) {
			close(errpipe[0]);
			close(errpipe[1]);
		}
		return -1;
	}
	if (pid == 0) {
		if (errpipe[1] >= 0) {
			/* stderr only: stdout is left alone because callers like
			 * run_subprocess_capture() rely on it, and a tool's
			 * diagnosis lives on stderr anyway. */
			dup2(errpipe[1], STDERR_FILENO);
			close(errpipe[1]);
			close(errpipe[0]);
		}
		execve(bin, argv, environ);
		perror(bin);
		_exit(127);
	}
	if (errpipe[1] >= 0)
		close(errpipe[1]);

	/*
	 * Drain BEFORE waitpid(): a child writing more than a pipe buffer
	 * would block forever on a full pipe while the parent waited for
	 * it to exit -- a deadlock this project has already hit once, in
	 * the pkg build-output path.
	 */
	if (errpipe[0] >= 0) {
		for (;;) {
			ssize_t n;

			if (errlen >= sizeof(errbuf) - 1) {
				/* Full: keep the TAIL. Drop the first half and carry
				 * on reading, so the last thing the tool said always
				 * survives. */
				size_t keep = sizeof(errbuf) / 2;

				memmove(errbuf, errbuf + (errlen - keep), keep);
				errlen = keep;
			}
			n = read(errpipe[0], errbuf + errlen, sizeof(errbuf) - 1 - errlen);
			if (n <= 0)
				break;
			errlen += (size_t)n;
		}
		errbuf[errlen] = '\0';
		close(errpipe[0]);
		/* Trim trailing newlines so the log line reads as one line. */
		while (errlen > 0 && (errbuf[errlen - 1] == '\n' || errbuf[errlen - 1] == '\r'))
			errbuf[--errlen] = '\0';
	}

	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		logstore_write("cixd", "error", "run_subprocess %s: waitpid failed: %s", bin,
		                strerror(errno));
		return -1;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	if (WIFEXITED(status)) {
		fprintf(stderr, "%s: exited with status %d\n", bin, WEXITSTATUS(status));
		/* Status 127 is execve()'s own _exit(127) above -- bin itself
		 * could not be run at all (missing/not executable/bad
		 * interpreter), distinct from bin running and failing on its
		 * own terms. Worth calling out explicitly since it's the
		 * class of bug this project has hit before (ADR-0056's
		 * /bin/sh -> /usr/bin/bash lesson): a wrong hardcoded path is
		 * invisible from the exit status alone otherwise. */
		if (WEXITSTATUS(status) == 127)
			logstore_write("cixd", "error",
			                "run_subprocess %s: exec failed (missing binary or bad path?)%s%s",
			                bin, errbuf[0] != '\0' ? " -- " : "", errbuf);
		else
			logstore_write("cixd", "error", "run_subprocess %s: exited with status %d%s%s",
			                bin, WEXITSTATUS(status), errbuf[0] != '\0' ? " -- " : "", errbuf);
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr, "%s: killed by signal %d\n", bin, WTERMSIG(status));
		logstore_write("cixd", "error", "run_subprocess %s: killed by signal %d%s%s", bin,
		                WTERMSIG(status), errbuf[0] != '\0' ? " -- " : "", errbuf);
	}
	return -1;
}

/*
 * Whether every entry in tarball_path's own listing shares the same
 * single top-level path component -- the shape --strip-components=1
 * assumes (a real release tarball's own "foo-1.2.3/" wrapping
 * directory). Confirmed a real, previously-undiscovered gap (ADR-0093):
 * git archive without --prefix= (gitea's own archive-download REST
 * endpoint, used by cix.recipe's own self-build source snapshot)
 * produces a tarball with NO such wrapping directory at all -- every
 * top-level file/directory of the real source tree sits at depth 0.
 * Blindly stripping one component there silently drops or misplaces
 * real top-level content instead of failing loudly.
 *
 * #411: walks the archive's own header stream via libarchive (no size
 * cap, no forked tar -tf/pipe-drain dance -- reading a header costs
 * nothing regardless of how many entries a real release tarball has).
 * A mismatch found anywhere is conclusive, exactly as before
 * (git-archive-without-prefix's own top-level entries diverge
 * immediately -- Makefile, daemon/, docs/, ... -- never needs the
 * whole archive to detect); an entry whose own path starts with '/'
 * (slash_len == 0) is treated the same as the old code's identical
 * case -- no real top-level component to agree on, not a common dir.
 */
static int tarball_has_common_top_dir(const char *tarball_path)
{
	struct archive *a;
	struct archive_entry *entry;
	char top[PATH_MAX] = { 0 };
	size_t top_len = 0;
	int rc = 0;

	a = archive_read_new();
	if (a == NULL)
		return 0;
	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);
	if (archive_read_open_filename(a, tarball_path, 262144) != ARCHIVE_OK) {
		archive_read_free(a);
		return 0;
	}
	for (;;) {
		const char *name;
		const char *slash;
		size_t slash_len;
		int r = archive_read_next_header(a, &entry);

		if (r == ARCHIVE_EOF) {
			rc = top_len > 0;
			break;
		}
		if (r != ARCHIVE_OK) {
			rc = 0;
			break;
		}
		name = archive_entry_pathname(entry);
		if (name == NULL || name[0] == '\0')
			continue;
		slash = strchr(name, '/');
		slash_len = slash != NULL ? (size_t)(slash - name) : strlen(name);
		if (slash_len == 0 || slash_len >= sizeof(top)) {
			rc = 0; /* no real top-level component, or one too long to compare */
			break;
		}
		if (top_len == 0) {
			memcpy(top, name, slash_len);
			top[slash_len] = '\0';
			top_len = slash_len;
		} else if (slash_len != top_len || memcmp(top, name, slash_len) != 0) {
			rc = 0; /* real mismatch -- no common top dir */
			break;
		}
	}
	archive_read_close(a);
	archive_read_free(a);
	return rc;
}

/*
 * #411: extracts archive_path into dest_dir in-process via libarchive
 * (replacing the forked `tar -xf`/`-xzf`), which autodetects both
 * container format and compression from the archive's own bytes, the
 * same as tar's own autodetection -- callers never distinguish -xf
 * from -xzf.
 *
 * strip_first_component mirrors tar's own --strip-components=1: an
 * entry's first '/'-separated segment is dropped, and an entry with
 * nothing to strip to (no '/' at all) is skipped entirely, exactly
 * tar's own documented behavior for that flag.
 *
 * ARCHIVE_EXTRACT_SECURE_NODOTDOT/SECURE_SYMLINKS refuse a member that
 * would escape dest_dir via a ../ path or a symlink already on disk
 * that redirects a later write outside the tree -- this project's own
 * recipes fetch source archives from third-party upstreams, so this is
 * untrusted input, not merely unfamiliar input.
 *
 * NOT ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS -- confirmed live (a real
 * failed extraction on 192.168.15.95, every entry refused with no
 * diagnostic until this was traced): that flag refuses ANY absolute
 * pathname on the entry libarchive is about to write, and dest_dir
 * itself is always absolute, so it refused every single entry outright
 * the moment `full` (below) replaced the archive's own relative name.
 * The actual property that flag is for -- an archive entry that names
 * an absolute path itself, e.g. claiming to write "/etc/passwd" -- is
 * checked explicitly instead, against the archive's own name BEFORE
 * dest_dir is prefixed onto it, which is the only place that check is
 * meaningful.
 */
static int extract_archive_to(const char *archive_path, const char *dest_dir,
                               int strip_first_component)
{
	struct archive *a;
	struct archive *ext;
	struct archive_entry *entry;
	int rc = -1;

	a = archive_read_new();
	if (a == NULL)
		return -1;
	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);

	ext = archive_write_disk_new();
	if (ext == NULL) {
		archive_read_free(a);
		return -1;
	}
	/*
	 * ARCHIVE_EXTRACT_OWNER: GNU tar's own default when the process is
	 * real root (cixd always is) is to restore the archive's declared
	 * uid/gid -- "same-owner" needs no flag to enable as root, only
	 * --no-same-owner disables it, which the old `tar -xf` call never
	 * passed. Matched here rather than left to this write's own
	 * default (the current process's uid/gid) so this is parity with
	 * the old behavior, not a silent change to it.
	 */
	archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
	                                        ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS |
	                                        ARCHIVE_EXTRACT_OWNER |
	                                        ARCHIVE_EXTRACT_SECURE_NODOTDOT |
	                                        ARCHIVE_EXTRACT_SECURE_SYMLINKS);
	archive_write_disk_set_standard_lookup(ext);

	if (archive_read_open_filename(a, archive_path, 262144) != ARCHIVE_OK) {
		logstore_write("cixd", "error", "extract %s: open failed: %s", archive_path,
		                archive_error_string(a));
		goto out;
	}

	for (;;) {
		const char *name;
		char full[PATH_MAX];
		int r = archive_read_next_header(a, &entry);

		if (r == ARCHIVE_EOF)
			break;
		if (r != ARCHIVE_OK) {
			logstore_write("cixd", "error", "extract %s: read header failed: %s", archive_path,
			                archive_error_string(a));
			goto out;
		}

		/*
		 * Only the absolute-path case needs its own check here --
		 * ARCHIVE_EXTRACT_SECURE_NODOTDOT (above) already refuses a
		 * real ".." path COMPONENT once it's written into `full`
		 * below, correctly, since dest_dir itself never contains one.
		 * A plain substring check for ".." would also reject a
		 * legitimate name that merely contains two consecutive dots
		 * without ever being a traversal.
		 */
		name = archive_entry_pathname(entry);
		if (name == NULL || name[0] == '/') {
			logstore_write("cixd", "error", "extract %s: refusing absolute member path \"%s\"",
			                archive_path, name != NULL ? name : "(null)");
			goto out;
		}

		if (strip_first_component) {
			const char *slash = strchr(name, '/');

			if (slash == NULL)
				continue; /* nothing to strip to -- tar skips these too */
			name = slash + 1;
		}

		if ((size_t)snprintf(full, sizeof(full), "%s/%s", dest_dir, name) >= sizeof(full))
			goto out;
		archive_entry_set_pathname(entry, full);

		if (archive_write_header(ext, entry) != ARCHIVE_OK) {
			logstore_write("cixd", "error", "extract %s: write header for \"%s\" failed: %s",
			                archive_path, full, archive_error_string(ext));
			goto out;
		}
		if (archive_entry_size(entry) > 0) {
			const void *buf;
			size_t size;
			int64_t offset;

			for (;;) {
				r = archive_read_data_block(a, &buf, &size, &offset);
				if (r == ARCHIVE_EOF)
					break;
				if (r != ARCHIVE_OK) {
					logstore_write("cixd", "error", "extract %s: read data for \"%s\" failed: %s",
					                archive_path, full, archive_error_string(a));
					goto out;
				}
				if (archive_write_data_block(ext, buf, size, offset) != ARCHIVE_OK) {
					logstore_write("cixd", "error",
					                "extract %s: write data for \"%s\" failed: %s", archive_path,
					                full, archive_error_string(ext));
					goto out;
				}
			}
		}
		if (archive_write_finish_entry(ext) != ARCHIVE_OK) {
			logstore_write("cixd", "error", "extract %s: finish entry \"%s\" failed: %s",
			                archive_path, full, archive_error_string(ext));
			goto out;
		}
	}
	rc = 0;
out:
	archive_write_close(ext);
	archive_write_free(ext);
	archive_read_close(a);
	archive_read_free(a);
	return rc;
}

static int extract_tarball(const char *tarball_path, const char *dest_dir)
{
	return extract_archive_to(tarball_path, dest_dir, tarball_has_common_top_dir(tarball_path));
}

/* Last '/'-separated segment of a source URL -- where an extra
 * (non-index-0) source lands under /build/extra/ (ADR-0036). Pointer
 * into url itself, never allocates. */
/*
 * The basename of a URL's path component, with any trailing query
 * string (a literal '?' onward) stripped -- a git-raw-file pkg_source
 * entry needs a `?ref=<commit>` query parameter (Gitea's own raw-file
 * API convention, used by kernel.recipe/cix.recipe's own multi-
 * source config-file fetches) for reproducibility, and this function's
 * own prior naive strrchr('/')-only basename left that query string
 * attached to the staged /build/extra/<basename> filename, silently
 * breaking every recipe's own pkg_build() reference to the plain
 * filename it expected -- confirmed live: kernel.recipe's own
 * qemu-part1.config staged as
 * "qemu-part1.config?ref=<40 hex chars>" instead of plain
 * "qemu-part1.config", so its own `cp /build/extra/qemu-part1.config
 * .config` line failed with a bare "No such file or directory" the
 * first time this exact mechanism was ever actually exercised
 * (kernel.recipe 6.18.40-2's own predecessor, committed but never
 * actually built until now, per that recipe's own re-pin history).
 */
static void url_basename(const char *url, char *out, size_t out_size)
{
	const char *slash = strrchr(url, '/');
	const char *name = slash != NULL ? slash + 1 : url;
	const char *query = strchr(name, '?');
	size_t len = query != NULL ? (size_t)(query - name) : strlen(name);

	if (len >= out_size)
		len = out_size - 1;
	memcpy(out, name, len);
	out[len] = '\0';
}

/*
 * Not every package's source is an archive. A CA certificate bundle,
 * a single-file script, a firmware blob: upstream publishes one plain
 * file and there is nothing to unpack. Before this, source[0] was
 * unconditionally handed to tar, so such a package failed at
 * "extract source tarball" -- an error naming a tarball that never
 * existed, which reads as a corrupt download rather than a source
 * that was never an archive in the first place.
 *
 * The decision is made from the file's own leading bytes, not from
 * the URL's extension: a URL is a claim and the bytes are the fact,
 * and this project has already been bitten once by pinning the
 * checksum of a 99-byte error page that a URL promised was a tarball.
 * tar itself autodetects the compression, so this only has to answer
 * "is it an archive at all".
 */
static int file_is_archive(const char *path)
{
	unsigned char h[262];
	size_t n;
	FILE *f = fopen(path, "rb");

	if (f == NULL)
		return 0;
	n = fread(h, 1, sizeof(h), f);
	fclose(f);

	if (n >= 2 && h[0] == 0x1f && h[1] == 0x8b)
		return 1; /* gzip */
	if (n >= 6 && memcmp(h, "\xfd" "7zXZ\x00", 6) == 0)
		return 1; /* xz */
	if (n >= 3 && memcmp(h, "BZh", 3) == 0)
		return 1; /* bzip2 */
	if (n >= 4 && h[0] == 0x28 && h[1] == 0xb5 && h[2] == 0x2f && h[3] == 0xfd)
		return 1; /* zstd */
	if (n >= 262 && memcmp(h + 257, "ustar", 5) == 0)
		return 1; /* uncompressed tar */
	return 0;
}

/*
 * Put source[0] where pkg_build() expects to find it: unpacked into
 * /build/src for an archive, or laid down as /build/src/<basename>
 * for a single plain file.
 */
static int stage_main_source(const char *src_path, const char *dest_dir, const char *url)
{
	char base[PKG_URL_MAX];
	char dst[PATH_MAX];

	if (file_is_archive(src_path))
		return extract_tarball(src_path, dest_dir);

	url_basename(url, base, sizeof(base));
	if (base[0] == '\0')
		return -1;
	if ((size_t)snprintf(dst, sizeof(dst), "%s/%s", dest_dir, base) >= sizeof(dst))
		return -1;
	return copy_file_simple(src_path, dst);
}

/*
 * Each concurrent build owns a distinct __pkgbuild-<chain index>
 * container path (ADR-0157 -- builds run in parallel, up to
 * max_concurrent_jobs) -- without wiping it first, a build's
 * /build/pkg-dest would silently inherit leftover files from whatever
 * ran in that same slot's upperdir before (a chained dependency, or an
 * unrelated earlier install that used the same index). Called before
 * every build's prep, guaranteeing each one starts from a genuinely
 * clean container directory regardless of what ran there before.
 */
static int reset_build_container_dir(const char *container_base)
{
	return cix_btrfs_subvol_delete_or_rmtree(container_base);
}

/*
 * #352: cixd already links -lcrypto (Makefile), so this forked no
 * subprocess to get a hash the linked library computes directly --
 * every call site's contract (out_size >= 65, a lowercase 64-hex-char
 * digest, 0/-1) is unchanged, so none of them needed to move.
 */
int pkg_run_capture_sha256(const char *path, char *out, size_t out_size)
{
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_len = 0;
	EVP_MD_CTX *ctx;
	int fd;
	unsigned char buf[65536];
	ssize_t n;
	unsigned int i;

	if (out_size < 65)
		return -1;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	ctx = EVP_MD_CTX_new();
	if (ctx == NULL) {
		close(fd);
		return -1;
	}
	if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
		EVP_MD_CTX_free(ctx);
		close(fd);
		return -1;
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (EVP_DigestUpdate(ctx, buf, (size_t)n) != 1) {
			EVP_MD_CTX_free(ctx);
			close(fd);
			return -1;
		}
	}
	close(fd);
	if (n < 0 || EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1 || digest_len != 32) {
		EVP_MD_CTX_free(ctx);
		return -1;
	}
	EVP_MD_CTX_free(ctx);

	for (i = 0; i < digest_len; i++)
		snprintf(out + i * 2, 3, "%02x", digest[i]);
	out[64] = '\0';
	return 0;
}

/* Recursively copies src_root/<relpath> into dst_root/<relpath>,
 * recording every regular file or symlink copied into e's manifest.
 * A symlink is recreated verbatim (readlink() the recipe-produced
 * target string, symlink() it at the destination) rather than
 * followed/copied-as-a-file -- real, not a hypothetical: iptables'
 * own `make install` installs iptables/ip6tables/iptables-save/...
 * as symlinks to a single xtables-legacy-multi binary, and the first
 * version of this function silently dropped every one of them,
 * caught by iptables.recipe's own real end-to-end verification, not
 * anticipated up front. unlink_manifest_files()'s existing unlink()
 * call already removes a symlink correctly (removes the link itself,
 * never follows it), so no separate removal-path change was needed. */
/*
 * Every failure exit of merge_tree() goes through this. It used to
 * return a bare -1 from eight different places, so "failed to merge
 * installed files into the target image" was the entire diagnosis --
 * no path, no errno, no indication of which of the eight. Installing a
 * freshly fixed gcc into the shared build sandbox failed exactly that
 * way (issue #118), and the same shape of silence cost hours on #40
 * and #109 before those were instrumented.
 *
 * The path is relative to the tree being merged, which is what a
 * reader can actually act on -- an absolute staging path names a
 * scratch directory that will not exist by the time anyone looks.
 */
static int merge_fail(const char *relpath, const char *step, int use_errno)
{
	if (use_errno)
		logstore_write("cixd", "error", "merge: %s failed for \"%s\": %s", step,
		               relpath != NULL && relpath[0] != '\0' ? relpath : "(tree root)",
		               strerror(errno));
	else
		logstore_write("cixd", "error", "merge: %s failed for \"%s\"", step,
		               relpath != NULL && relpath[0] != '\0' ? relpath : "(tree root)");
	return -1;
}

static int merge_tree(const char *src_root, const char *dst_root, const char *relpath,
                       struct pkg_entry *e)
{
	char src_dir[PATH_MAX];
	DIR *d;
	struct dirent *de;

	snprintf(src_dir, sizeof(src_dir), "%s%s%s", src_root, relpath[0] ? "/" : "", relpath);
	d = opendir(src_dir);
	if (d == NULL) {
		/* A missing tree root is "nothing to merge", not a failure --
		 * a package that installed no files at all is legitimate. Any
		 * other level going missing mid-walk is not. */
		if (relpath[0] == '\0')
			return 0;
		return merge_fail(relpath, "opendir", 1);
	}

	while ((de = readdir(d)) != NULL) {
		char child_rel[PATH_MAX];
		char src_path[PATH_MAX], dst_path[PATH_MAX];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(child_rel, sizeof(child_rel), "%s%s%s", relpath, relpath[0] ? "/" : "",
		         de->d_name);
		snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, de->d_name);

		if (lstat(src_path, &st) != 0) {
			closedir(d);
			return merge_fail(child_rel, "lstat", 1);
		}

		if (S_ISDIR(st.st_mode)) {
			snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_root, child_rel);
			persist_mkdir_p(dst_path);
			if (merge_tree(src_root, dst_root, child_rel, e) != 0) {
				closedir(d);
				return -1; /* already reported, one level down */
			}
		} else if (S_ISREG(st.st_mode)) {
			char dst_parent[PATH_MAX], *slash;

			snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_root, child_rel);
			snprintf(dst_parent, sizeof(dst_parent), "%s", dst_path);
			slash = strrchr(dst_parent, '/');
			if (slash != NULL) {
				*slash = '\0';
				persist_mkdir_p(dst_parent);
			}
			/*
			 * Replace whatever is there; never write through it.
			 *
			 * copy_file_simple() opens the destination O_CREAT|O_TRUNC,
			 * which FOLLOWS an existing symlink. Installing a package
			 * file onto a path that is currently a symlink therefore
			 * wrote to the symlink's target instead of replacing the
			 * link -- and the visible failure was the lucky case:
			 * gcc's own usr/bin/c++ landed on a Debian-alternatives
			 * symlink left in the build sandbox
			 * (usr/bin/c++ -> /etc/alternatives/c++, which does not
			 * exist there), so the open failed ENOENT and the merge
			 * stopped. Had that target existed, the install would have
			 * silently overwritten an unrelated file and reported
			 * success.
			 *
			 * The symlink branch below has always unlinked first for
			 * the same reason. This one should have too.
			 */
			/*
			 * Issue #176: refuse a binary carrying an undefined
			 * `__builtin_*`.
			 *
			 * Checked on the SOURCE, before anything is copied, so a
			 * rejected build leaves nothing of itself behind.
			 *
			 * TCC does not implement every GCC builtin and does not
			 * fail on the ones it lacks -- it emits them as ordinary
			 * undefined externals. `libblkid.so` was published that
			 * way with an undefined `__builtin_clz`: it compiled,
			 * installed and uploaded without one error, and only broke
			 * much later when something tried to link against it. Same
			 * shape as the do-while miscompile (#122) -- a clean exit
			 * and a wrong artifact -- except this one is mechanically
			 * detectable, so it is detected.
			 *
			 * No compiler ever DEFINES a `__builtin_*` symbol; a
			 * builtin is expanded inline by definition. So a hit is
			 * never a false alarm, which is what makes it safe to fail
			 * the whole install rather than merely warn.
			 */
			{
				char bad_sym[128];

				if (elfcheck_undefined_builtin(src_path, bad_sym, sizeof(bad_sym)) == 1) {
					closedir(d);
					logstore_write("cixd", "error",
					               "pkg install: %s carries an undefined compiler builtin '%s' -- "
					               "the compiler did not implement it and emitted it as an "
					               "external symbol instead, so this binary is broken (#176)",
					               child_rel, bad_sym);
					return merge_fail(child_rel, "carrying an undefined compiler builtin", 0);
				}
			}

			unlink(dst_path);
			if (copy_file_simple(src_path, dst_path) != 0) {
				closedir(d);
				return merge_fail(child_rel, "copying a regular file", 1);
			}
			chmod(dst_path, st.st_mode & 0777);
			if (pkg_entry_add_file(e, child_rel) != 0) {
				closedir(d);
				return merge_fail(child_rel, "recording the file in the package manifest", 0);
			}
		} else if (S_ISLNK(st.st_mode)) {
			char target[PATH_MAX];
			ssize_t len;
			char dst_parent[PATH_MAX], *slash;

			len = readlink(src_path, target, sizeof(target) - 1);
			if (len < 0) {
				closedir(d);
				return merge_fail(child_rel, "readlink", 1);
			}
			target[len] = '\0';

			snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_root, child_rel);
			snprintf(dst_parent, sizeof(dst_parent), "%s", dst_path);
			slash = strrchr(dst_parent, '/');
			if (slash != NULL) {
				*slash = '\0';
				persist_mkdir_p(dst_parent);
			}
			/*
			 * A symlink in the source landing where the destination
			 * already has a real DIRECTORY is not a conflict about
			 * content -- it is the two trees disagreeing about
			 * merged-/usr shape (a flat sandbox seeded from a
			 * merged-/usr host has /lib as a symlink to usr/lib; an
			 * image built up by this project's own recipes has a real
			 * /lib directory). unlink() cannot remove a directory, so
			 * the symlink() that followed failed EEXIST and took the
			 * whole migration down with it -- issue #40's own
			 * confirmed, previously unattributed failure.
			 *
			 * Keeping the directory is the answer that loses nothing:
			 * everything reachable through the source symlink is
			 * reachable through its target path too, which this same
			 * walk merges on its own. Replacing it would discard
			 * whatever the destination already had there.
			 */
			{
				struct stat dst_st;

				if (lstat(dst_path, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
					logstore_write("cixd", "info",
					               "merge: keeping the existing directory at %s rather than "
					               "replacing it with a symlink to %s (merged-/usr shape "
					               "difference, not a content conflict)",
					               child_rel, target);
					continue;
				}
			}
			unlink(dst_path); /* EEXIST tolerance for a re-install/upgrade,
			                    * same spirit copy_file_simple()'s own
			                    * O_CREAT|O_TRUNC already has for regular files */
			if (symlink(target, dst_path) != 0) {
				closedir(d);
				return merge_fail(child_rel, "creating a symlink", 1);
			}
			if (pkg_entry_add_file(e, child_rel) != 0) {
				closedir(d);
				return merge_fail(child_rel, "recording the file in the package manifest", 0);
			}
		}
	}
	closedir(d);
	return 0;
}

/*
 * Recursively reproduces src_root's own tree at dst_root -- directories
 * created fresh, symlinks recreated fresh (readlink+symlink, matching
 * merge_tree()'s own symlink handling -- hard-linking a symlink's own
 * dentry works on Linux but is needless fragility for a file that's a
 * few bytes either way), and every other type (regular files, and the
 * char device nodes pkg_seed_image_baseline() creates) HARD-LINKED
 * (link(2), not copied). This is ADR-0107/0108's own "copy-forward"
 * mechanism -- an unchanged file occupies zero new disk space or copy
 * time, only the genuinely new/changed files a subsequent merge_tree()
 * call writes ever land on a new inode. This is the actual fix the
 * package/image versioning epic exists to deliver: a version's own
 * rootfs, once produced, is never written to again -- copy_tree_hardlink()
 * is always followed by mutation of a freshly named STAGING directory,
 * never of src_root itself.
 */
static int copy_tree_hardlink(const char *src_root, const char *dst_root)
{
	DIR *d;
	struct dirent *de;

	d = opendir(src_root);
	if (d == NULL)
		return -1;

	while ((de = readdir(d)) != NULL) {
		char src_path[PATH_MAX], dst_path[PATH_MAX];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(src_path, sizeof(src_path), "%s/%s", src_root, de->d_name);
		snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_root, de->d_name);

		if (lstat(src_path, &st) != 0) {
			closedir(d);
			return -1;
		}

		if (S_ISDIR(st.st_mode)) {
			if (persist_mkdir_p(dst_path) != 0 || copy_tree_hardlink(src_path, dst_path) != 0) {
				closedir(d);
				return -1;
			}
		} else if (S_ISLNK(st.st_mode)) {
			char target[PATH_MAX];
			ssize_t len = readlink(src_path, target, sizeof(target) - 1);

			if (len < 0) {
				closedir(d);
				return -1;
			}
			target[len] = '\0';
			if (symlink(target, dst_path) != 0) {
				closedir(d);
				return -1;
			}
		} else {
			if (link(src_path, dst_path) != 0) {
				closedir(d);
				return -1;
			}
		}
	}
	closedir(d);
	return 0;
}

/* PKG_MAX_PACKAGES entries at up to PKG_NAME_MAX+PKG_VERSION_MAX bytes
 * each is a multi-KB working set -- a file-scope static buffer for a
 * single-job-at-a-time result, not a large per-call stack frame. */
#define PKG_MANIFEST_STRING_MAX (PKG_MAX_PACKAGES * (PKG_NAME_MAX + PKG_VERSION_MAX + 2))
static char g_manifest_string_buf[PKG_MANIFEST_STRING_MAX];

struct manifest_ref {
	const char *name;
	const char *version;
};

static int manifest_ref_cmp(const void *a, const void *b)
{
	return strcmp(((const struct manifest_ref *)a)->name, ((const struct manifest_ref *)b)->name);
}

/*
 * The canonical, sorted "name@version,name@version,..." string
 * (ADR-0108) covering every package currently PKG_STATE_INSTALLED
 * against image -- the exact set the resulting rootfs will actually
 * contain once the caller's own mutate() has already been applied to
 * g_packages (image_produce_new_version() calls this only AFTER
 * mutate() returns, never before).
 */
static const char *build_image_manifest_string(const char *image)
{
	struct manifest_ref refs[PKG_MAX_PACKAGES];
	int count = 0, i;
	size_t pos = 0;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (g_packages[i].in_use && g_packages[i].state == PKG_STATE_INSTALLED &&
		    strcmp(g_packages[i].image, image) == 0) {
			refs[count].name = g_packages[i].name;
			refs[count].version = g_packages[i].version;
			count++;
		}
	}
	qsort(refs, (size_t)count, sizeof(refs[0]), manifest_ref_cmp);

	g_manifest_string_buf[0] = '\0';
	for (i = 0; i < count; i++) {
		int n = snprintf(g_manifest_string_buf + pos, sizeof(g_manifest_string_buf) - pos,
		                  "%s%s@%s", (i > 0) ? "," : "", refs[i].name, refs[i].version);

		if (n < 0 || (size_t)n >= sizeof(g_manifest_string_buf) - pos)
			break; /* truncated -- PKG_MANIFEST_STRING_MAX already sized for
			        * PKG_MAX_PACKAGES worst case, so unreachable in practice */
		pos += (size_t)n;
	}
	return g_manifest_string_buf;
}

/*
 * #398: write down what this version holds, beside the version itself.
 *
 * The same installed set build_image_manifest_string() above turns into
 * the version hash -- so this records the version's own identity rather
 * than a second, independently-drifting account of it. Deliberately NOT
 * the declared manifest.json: that is live operator intent, it moves
 * whenever someone changes what the image should contain, and a
 * `rolling` entry's version there is a floor rather than a fact.
 *
 * A container records the image version it runs from and keeps running
 * it until it is recreated, so this is the only thing that can answer
 * "what is in that container". Reading the live manifest for that
 * question describes the image as it is now -- a different question
 * wearing the same shape, and one that fails as a plausible,
 * up-to-date-looking list rather than as an error (#395).
 *
 * Returns 0, or -1 with the caller deciding how much that matters.
 */
static int pkg_version_snapshot_write(const char *image, const char *version)
{
	struct manifest_ref refs[PKG_MAX_PACKAGES];
	int count = 0, i, rc;
	struct json_writer w;
	char path[PATH_MAX];

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (g_packages[i].in_use && g_packages[i].state == PKG_STATE_INSTALLED &&
		    strcmp(g_packages[i].image, image) == 0) {
			refs[count].name = g_packages[i].name;
			refs[count].version = g_packages[i].version;
			count++;
		}
	}
	qsort(refs, (size_t)count, sizeof(refs[0]), manifest_ref_cmp);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "version");
	jw_str(&w, version);
	jw_key(&w, "packages");
	jw_arr_open(&w);
	for (i = 0; i < count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "package");
		jw_str(&w, refs[i].name);
		jw_key(&w, "version");
		jw_str(&w, refs[i].version);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);

	image_version_manifest_path(image, version, path, sizeof(path));
	rc = persist_atomic_write(path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

/*
 * The one shared copy-forward mechanism (ADR-0107/0108) behind every
 * mutation of an image's own package set -- pkg_build_completed()'s
 * install/upgrade merge and pkg_delete()'s uninstall both go through
 * this rather than each hand-rolling their own stage/hash/finalize
 * sequence (No Parallel Implementations). Ensures image exists
 * (implicit creation -- the same convention pkg install has always had
 * for a bare image name never explicitly POSTed to /v1/images first),
 * resolves its current version's own rootfs, hardlinks a full copy
 * into a scratch staging directory (image's real version rootfs is
 * NEVER written to directly -- an already-running container may have
 * it open as its own overlay lowerdir right now), lets mutate() apply
 * exactly the one change this install/delete needs against the
 * staging copy (and update g_packages to match), then hashes the
 * image's own post-mutation manifest identity to name the result. If
 * that name already exists in this image's own version history (an
 * install/delete cycle landing back on a previously seen package set
 * -- real file content may differ in insignificant ways like an
 * embedded build timestamp, ADR-0108's own accepted tradeoff for
 * manifest-identity hashing over full-content hashing), the staging
 * copy is discarded and current_version is simply repointed to the
 * existing, already-immutable directory; otherwise the staging copy
 * itself becomes the new version's permanent rootfs (a same-filesystem
 * rename(2), not a second copy). Returns 0 on success.
 */
/*
 * Every failure exit of image_produce_new_version() goes through this,
 * so a caller's own "could not produce a new image version" never
 * again has to stand alone as the whole explanation. Two of this
 * project's own real investigations (issue #40's flat-sandbox
 * migration, and #109's first live build-environment composition) both
 * stalled on exactly that: a bare -1 from a nine-exit function, with
 * the caller left to guess which step and which errno. The step name
 * and errno are the two things that make it a one-read diagnosis.
 */
/*
 * The reason the last image_produce_new_version() gave up, so a caller
 * can report it instead of blaming whatever package happened to be
 * installing (#180).
 *
 * Without this, an image that could not snapshot itself forward failed
 * as "zlib failed to merge installed files into the target image" --
 * naming a package that was entirely innocent, while the actual cause
 * ("snapshotting the current rootfs forward failed: Invalid argument")
 * reached only the log store. That is the same mistake childdiag.c
 * exists to prevent: the message an operator reads should name what
 * actually went wrong.
 */
static char g_produce_fail_reason[256];

const char *pkg_last_image_produce_failure(void)
{
	return g_produce_fail_reason[0] != '\0' ? g_produce_fail_reason : NULL;
}

static int produce_fail(const char *image, const char *staging, const char *step, int use_errno)
{
	if (use_errno) {
		snprintf(g_produce_fail_reason, sizeof(g_produce_fail_reason),
		         "%s failed: %s", step, strerror(errno));
		logstore_write("cixd", "error",
		               "image %s: could not produce a new version -- %s failed: %s", image,
		               step, strerror(errno));
	} else {
		snprintf(g_produce_fail_reason, sizeof(g_produce_fail_reason), "%s failed", step);
		logstore_write("cixd", "error",
		               "image %s: could not produce a new version -- %s failed", image, step);
	}
	if (staging != NULL)
		cix_btrfs_subvol_delete_or_rmtree(staging);
	return -1;
}

/*
 * Did this package's own files actually reach the image's current
 * rootfs? (#281)
 *
 * image_produce_new_version() can return success and leave the freshly
 * built tree on the floor. An image version is a hash of the installed
 * package set (ADR-0108), so a set that hashes to a version already on
 * disk is DEDUPED: current_version repoints at the existing tree and
 * the new one is deleted (ADR-0155). When the existing tree really does
 * hold that content, that is correct and cheap. When it does not -- an
 * earlier merge interrupted, a repaired artifact reinstalled at the
 * same version, a baseline that has since changed -- the install
 * reports success and the files are simply absent.
 *
 * That is not a hypothetical. #281 was filed on exactly it: GET
 * /v1/pkg said htop was installed into jumpbox while the container
 * answered "bash: htop: command not found", and nothing anywhere
 * connected the two. The operator's first evidence was a missing
 * binary, which names neither the image nor the dedup.
 *
 * So the claim is checked rather than assumed. The package already
 * records every path it installed, and the image's current rootfs is a
 * real directory -- stat()ing one against the other is the whole test.
 * It costs one stat per installed file, against a merge that has just
 * copied an entire tree, and it is the difference between a loud
 * failure here and a "command not found" days later.
 *
 * Returns the number of recorded files missing, and writes the first
 * one into `first_missing` so the report names something specific.
 */
static int installed_files_missing(const char *image, const struct pkg_entry *e,
                                    char *first_missing, size_t first_missing_size)
{
	char version[IMAGE_VERSION_MAX];
	char rootfs[PATH_MAX];
	char path[PATH_MAX];
	struct stat st;
	int missing = 0;
	int i;

	if (first_missing != NULL && first_missing_size > 0)
		first_missing[0] = '\0';
	if (e == NULL || e->file_count == 0)
		return 0;
	if (image_current_version(image, version, sizeof(version)) != IMAGE_OK)
		return 0; /* Not this check's failure to report -- the caller
		           * already treats a missing current version as its own
		           * error, and guessing here would report the wrong one. */
	image_version_rootfs_path(image, version, rootfs, sizeof(rootfs));

	for (i = 0; i < e->file_count; i++) {
		const char *rel = e->files[i];

		while (*rel == '/')
			rel++;
		if (snprintf(path, sizeof(path), "%s/%s", rootfs, rel) >= (int)sizeof(path))
			continue; /* Cannot be checked, so not counted against it. */
		/* lstat, not stat: a dangling symlink is a file the package
		 * installed and is present, and resolving it would call it
		 * missing for pointing at something not built yet. */
		if (lstat(path, &st) != 0) {
			if (missing == 0 && first_missing != NULL && first_missing_size > 0)
				snprintf(first_missing, first_missing_size, "%s", rel);
			missing++;
		}
	}
	return missing;
}

/*
 * Does every header this package installed actually resolve? (#289)
 *
 * A package that installs headers is shipping an INTERFACE, and an
 * interface that cannot be included is broken whether or not anything
 * currently uses it. linux-pam shipped exactly that for six revisions:
 * it installed security/pam_misc.h, whose very first include is
 * security/pam_client.h, and never built the libpamc directory that
 * header comes from. Nothing linked pam_misc, so nothing noticed --
 * until util-linux ran a compile test on it while packaging login(1),
 * concluded PAM was unusable, and refused to build. The message named
 * PAM, not the missing header, and not linux-pam.
 *
 * This recipe set builds directory-by-directory on purpose, so any
 * recipe that installs SOME of an upstream project's headers can carry
 * the same defect silently. The check is the cheap half of what a
 * compiler would do: for each installed header, does every angle-
 * bracket include it makes unconditionally resolve to a file that
 * exists in this image's own include tree?
 *
 * Four deliberate limits, because a check with false positives is one
 * that gets switched off -- and the first version of this had them.
 * Run against 202 real installed packages it flagged 48, essentially
 * all noise: <stddef.h> and <stdarg.h> are FREESTANDING headers, which
 * the compiler provides from its own directory rather than
 * /usr/include, so they never resolve there and are never a defect.
 *
 *   - Angle-bracket includes only. A quoted include is relative to the
 *     including file and follows different rules.
 *   - UNCONDITIONAL includes only. An include inside #if/#ifdef is
 *     frequently meant not to resolve on this platform, and treating
 *     those as defects would flag correct headers constantly. A file's
 *     own include guard is not such a conditional and is skipped, or
 *     nothing in a guarded header would ever be checked -- which is
 *     every header worth checking.
 *   - SAME-PROJECT includes only: the include must sit in the same
 *     directory under usr/include/ as the header making it. That is
 *     the exact shape of the defect -- pam_misc.h reaching for
 *     pam_client.h, both under security/, one directory built and the
 *     other not -- and it is the shape a RECIPE is responsible for,
 *     since a project's own headers are the ones its own build either
 *     installs or does not. It also drops every false positive above
 *     in one move, without a list of compiler headers to keep current.
 *
 *     What it gives up, stated rather than discovered later: a header
 *     reaching into another PACKAGE that is not installed in this
 *     image goes unreported. That is a dependency question -- and an
 *     image-dependent one, since the same header is fine in an image
 *     that has the dependency -- not "this recipe did not build a
 *     directory it ships headers from".
 *   - Existence, not compilability. Whether the resolved header itself
 *     parses is the compiler's question. This one answers "is it even
 *     there", which is the failure that actually happened.
 *
 * Reported, never fatal to an install, and that is a judgement rather
 * than timidity: elfcheck refuses a shared library with undefined
 * symbols because that check has essentially no false positives, and
 * this one -- reading C without a preprocessor -- cannot make the same
 * claim. It says so loudly instead, in the log at install time and in
 * GET /v1/pkg/verify afterwards.
 *
 * Returns the number of unresolved includes, and writes the first as
 * "header: <include>" so the report names something checkable.
 */
static int header_includes_unresolved(const char *image, const struct pkg_entry *e, char *first_bad,
                                       size_t first_bad_size)
{
	char version[IMAGE_VERSION_MAX];
	char rootfs[PATH_MAX];
	char path[PATH_MAX];
	struct stat st;
	int unresolved = 0;
	int i;

	if (first_bad != NULL && first_bad_size > 0)
		first_bad[0] = '\0';
	if (e == NULL || e->file_count == 0)
		return 0;
	if (image_current_version(image, version, sizeof(version)) != IMAGE_OK)
		return 0;
	image_version_rootfs_path(image, version, rootfs, sizeof(rootfs));

	for (i = 0; i < e->file_count; i++) {
		const char *rel = e->files[i];
		char *buf = NULL;
		size_t len = 0;
		char *line, *save;
		int depth = 0;
		int guard_pending = 0;   /* saw #ifndef, waiting to see if a #define follows */
		int guard_depth = -1;    /* the depth the include guard occupies, once known */
		size_t rel_len;

		while (*rel == '/')
			rel++;
		rel_len = strlen(rel);
		if (rel_len < 3 || strcmp(rel + rel_len - 2, ".h") != 0)
			continue;
		if (strncmp(rel, "usr/include/", 12) != 0)
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", rootfs, rel) >= (int)sizeof(path))
			continue;
		if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
			continue; /* Absent is installed_files_missing()'s question, not this one. */

		for (line = strtok_r(buf, "\n", &save); line != NULL;
		     line = strtok_r(NULL, "\n", &save)) {
			const char *p = line;

			while (*p == ' ' || *p == '\t')
				p++;
			if (*p != '#')
				continue;
			p++;
			while (*p == ' ' || *p == '\t')
				p++;

			if (guard_pending) {
				/* The guard is only a guard if its #ifndef is
				 * immediately followed by the matching #define. */
				guard_pending = 0;
				if (strncmp(p, "define", 6) == 0)
					guard_depth = 1;
			}

			if (strncmp(p, "ifndef", 6) == 0 || strncmp(p, "ifdef", 5) == 0 ||
			    strncmp(p, "if", 2) == 0) {
				depth++;
				if (depth == 1 && guard_depth < 0 && strncmp(p, "ifndef", 6) == 0)
					guard_pending = 1;
				continue;
			}
			if (strncmp(p, "endif", 5) == 0) {
				if (depth > 0)
					depth--;
				continue;
			}
			if (strncmp(p, "include", 7) != 0)
				continue;
			/* Unconditional means depth 0, or depth 1 when that one
			 * level is the file's own include guard. */
			if (depth > (guard_depth > 0 ? guard_depth : 0))
				continue;
			p += 7;
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == '<') {
				char inc[256];
				const char *close = strchr(p + 1, '>');
				const char *inc_slash, *hdr_dir;
				size_t n, dirlen;

				if (close == NULL)
					continue;
				n = (size_t)(close - (p + 1));
				if (n == 0 || n >= sizeof(inc))
					continue;
				memcpy(inc, p + 1, n);
				inc[n] = '\0';

				/* Same directory under usr/include/ as the header
				 * making the include, or it is not this recipe's
				 * business (see this function's own comment). */
				hdr_dir = rel + 12; /* past "usr/include/" */
				inc_slash = strchr(inc, '/');
				if (inc_slash == NULL || strchr(hdr_dir, '/') == NULL)
					continue;
				dirlen = (size_t)(inc_slash - inc);
				if (strncmp(hdr_dir, inc, dirlen) != 0 || hdr_dir[dirlen] != '/')
					continue;

				if (snprintf(path, sizeof(path), "%s/usr/include/%s", rootfs, inc) >=
				    (int)sizeof(path))
					continue;
				if (lstat(path, &st) != 0) {
					if (unresolved == 0 && first_bad != NULL && first_bad_size > 0)
						snprintf(first_bad, first_bad_size, "%s: <%s>", rel, inc);
					unresolved++;
				}
			}
		}
		free(buf);
	}
	return unresolved;
}

static int image_produce_new_version(const char *image,
                                      int (*mutate)(const char *staging_rootfs, void *ctx),
                                      void *ctx, const char *extra_identity)
{
	char old_version[IMAGE_VERSION_MAX];
	char old_rootfs[PATH_MAX];
	char staging[PATH_MAX];
	char new_rootfs[PATH_MAX];
	char version_dir[PATH_MAX], *slash;
	char new_version[IMAGE_VERSION_MAX];
	const char *manifest_str;
	enum image_error ierr;
	struct stat st;

	ierr = image_create(image);
	if (ierr != IMAGE_OK && ierr != IMAGE_ERR_DUPLICATE)
		return produce_fail(image, NULL, "image_create", 0);

	if (image_current_version(image, old_version, sizeof(old_version)) != IMAGE_OK)
		return produce_fail(image, NULL, "resolving the current version", 0);
	image_version_rootfs_path(image, old_version, old_rootfs, sizeof(old_rootfs));

	snprintf(staging, sizeof(staging), "%s/%s/.staging.%d", g_images_dir, image, (int)getpid());
	/* Clear any leftover from a prior crashed attempt. Subvolume-aware:
	 * on btrfs the leftover staging is a subvolume that rmdir cannot
	 * remove (ADR-0207); on ext4 this is the same rmtree as before. */
	cix_btrfs_subvol_delete_or_rmtree(staging);

	/*
	 * ADR-0207 phase 1: on btrfs, staging is a writable snapshot of the
	 * current version's subvolume -- an O(1), copy-on-write clone,
	 * which is the whole point of the substrate change (production
	 * costs only the mutate's changed extents, not a copy of the
	 * rootfs). On ext4 the fast hardlink copy is kept exactly as
	 * before. Both leave `staging` a writable tree the mutate step and
	 * the atomic rename below treat identically.
	 */
	if (cix_btrfs_is_backing(old_rootfs)) {
		if (cix_btrfs_snapshot_or_copy(old_rootfs, staging) != 0)
			return produce_fail(image, NULL, "snapshotting the current rootfs forward", 1);
	} else {
		if (persist_mkdir_p(staging) != 0)
			return produce_fail(image, NULL, "creating the staging directory", 1);
		if (copy_tree_hardlink(old_rootfs, staging) != 0)
			return produce_fail(image, staging, "hardlink-copying the current rootfs forward",
			                     1);
	}

	if (mutate(staging, ctx) != 0)
		return produce_fail(image, staging, "the caller's own mutate step", 0);

	manifest_str = build_image_manifest_string(image);
	/*
	 * extra_identity exists for the one caller whose change is real but
	 * invisible to the manifest: merging a package into the shared
	 * build sandbox does not add a manifest entry, so the hash above
	 * would be byte-identical to the previous version's, the staging
	 * copy would be discarded as "already produced", and the merge
	 * would be silently lost. That is not hypothetical -- it is
	 * ADR-0155's own confirmed same-manifest-reinstall trap, arrived at
	 * from the other direction.
	 *
	 * Folding the OLD version in as well as the caller's own string
	 * makes the identity a real chain (v(n+1) = H(manifest, v(n),
	 * change)), so two different histories that happen to merge the
	 * same package last still get different versions. Every other
	 * caller passes NULL and is hashed exactly as before.
	 */
	if (extra_identity != NULL) {
		char identity[512];

		snprintf(identity, sizeof(identity), "%s\n%s\n%s", manifest_str, old_version,
		         extra_identity);
		if (image_hash_manifest_string(identity, new_version, sizeof(new_version)) != 0)
			return produce_fail(image, staging, "hashing the chained version identity", 0);
	} else if (image_hash_manifest_string(manifest_str, new_version, sizeof(new_version)) != 0) {
		return produce_fail(image, staging, "hashing the manifest identity", 0);
	}

	image_version_rootfs_path(image, new_version, new_rootfs, sizeof(new_rootfs));
	if (stat(new_rootfs, &st) == 0) {
		/* Already produced before -- discard this build, trust the
		 * existing immutable copy (see this function's own comment).
		 * Subvolume-aware: on btrfs `staging` is a snapshot. */
		/*
		 * SAY SO. Issue #218.
		 *
		 * This branch is correct and load-bearing -- an identical
		 * manifest really does describe the same image, and rebuilding
		 * it byte-for-byte would waste space for nothing. What was
		 * wrong is that it happened in silence: the build ran, the
		 * package reported "installed", and the tree it produced was
		 * deleted with nothing recorded anywhere.
		 *
		 * ADR-0155 documents this being hit live twice -- a repaired
		 * artifact was installed three times with no effect and no
		 * message. Both times the operator's next move was to doubt
		 * the fix rather than the mechanism, which is the cost of a
		 * silent correct answer.
		 *
		 * An image version is a hash of the package MANIFEST, not of
		 * content, so reinstalling the same versions always lands
		 * here. The message says what to do about it rather than only
		 * what happened, because "deduplicated" is not actionable and
		 * "bump a version" is.
		 */
		logstore_write("cixd", "info",
		               "image %s: the rebuilt tree was DISCARDED -- its manifest hashes to "
		               "%s, a version that already exists, so current_version repoints at "
		               "the existing tree (ADR-0155). If you expected the content to change, "
		               "the manifest did not: an image version is a hash of package "
		               "name@version pairs, so reinstalling the same versions can never "
		               "produce a new one. Bump a package revision to make the rebuild real.",
		               image, new_version);
		cix_btrfs_subvol_delete_or_rmtree(staging);
	} else {
		snprintf(version_dir, sizeof(version_dir), "%s", new_rootfs);
		slash = strrchr(version_dir, '/'); /* drop the trailing "/rootfs" component */
		if (slash != NULL)
			*slash = '\0';
		if (persist_mkdir_p(version_dir) != 0)
			return produce_fail(image, staging, "creating the new version directory", 1);
		if (rename(staging, new_rootfs) != 0)
			return produce_fail(image, staging, "renaming staging into place", 1);
	}

	if (image_record_version(image, new_version) != IMAGE_OK)
		return produce_fail(image, NULL, "recording the new version", 0);
	/*
	 * #398: keep the manifest this version was hashed from, beside the
	 * version itself. Written AFTER image_record_version() so it can
	 * never describe a version that failed to become real, and after
	 * the dedup branch above as well -- a version reached that way
	 * already has its own snapshot from when it was first produced,
	 * and the manifest is identical by construction (it is what the
	 * hash is OF), so rewriting it would be a second write of the same
	 * bytes.
	 *
	 * Non-fatal. The version exists and the image is usable without
	 * it; losing the ability to answer "what was in this version" is
	 * worth a logged warning, never a failed install.
	 */
	if (pkg_version_snapshot_write(image, new_version) != 0)
		logstore_write("cixd", "warn",
		               "image %s: could not record the package snapshot for version %s -- the "
		               "version is fine, but what it holds will not be answerable once the "
		               "installed set moves on (#398)",
		               image, new_version);
	return 0;
}

/*
 * The latest published version of a recipe, cached against the recipe
 * directory's own mtime.
 *
 * Why this exists: GET /v1/pkg was the single largest cause of the
 * daemon going unresponsive. stallwatch (#100) attributed 17 of 22
 * recorded stalls to it, and the reason is arithmetic. The drift check
 * below runs per installed package, and without this it did, for EACH
 * of them, an opendir of that package's recipe directory, a stat per
 * published version, and a full parse of a multi-kilobyte build.sh.
 * With 145 installed packages over 683 published recipe versions that
 * is roughly 145 directory scans and 145 shell-file parses -- on the
 * single-threaded event loop, for a request the dashboard issues every
 * two seconds.
 *
 * The old comment said "re-read fresh from disk every call -- One
 * Source of Truth, no cached comparison to keep in sync". The intent
 * was right and the cost was not paid for: freshness does not require
 * re-READING, only re-CHECKING. One stat of the directory answers
 * "has anything changed" for the whole package.
 *
 * Correctness rests on a property this platform already enforces
 * elsewhere: recipe versions are IMMUTABLE (ADR-0107, and the daemon
 * answers 409 to a republish). So a build.sh cannot change under a
 * directory whose mtime is unchanged -- only publishing or deleting a
 * version alters the set, and both change the directory's mtime. If
 * immutability ever stopped being enforced, this cache would be wrong,
 * which is why the dependency is written down here rather than left
 * implicit.
 *
 * Bounded and self-evicting: one slot per package name, oldest use
 * replaced when full. A miss costs exactly what every call used to.
 */
#define RECIPE_CACHE_SLOTS 256

struct recipe_cache_entry {
	char name[PKG_NAME_MAX];
	time_t dir_mtime;
	off_t dir_size;
	char version[PKG_VERSION_MAX];
	unsigned long used;
	int in_use;
};

static struct recipe_cache_entry g_recipe_cache[RECIPE_CACHE_SLOTS];
static unsigned long g_recipe_cache_clock;

/*
 * Fills out_version with the pkg_version= of the recipe that would be
 * resolved for this name. Returns 0 on success, -1 if there is no
 * usable recipe.
 */
static int recipe_latest_version(const char *name, char *out_version, size_t out_size)
{
	char name_dir[PATH_MAX], recipe_path[PATH_MAX];
	struct pkg_recipe recipe;
	struct stat dst;
	struct recipe_cache_entry *slot = NULL, *oldest = NULL;
	int i;

	snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, name);
	if (stat(name_dir, &dst) != 0)
		return -1;

	for (i = 0; i < RECIPE_CACHE_SLOTS; i++) {
		struct recipe_cache_entry *e = &g_recipe_cache[i];

		if (e->in_use && strcmp(e->name, name) == 0) {
			slot = e;
			break;
		}
		if (!e->in_use) {
			if (oldest == NULL || oldest->in_use)
				oldest = e;
		} else if (oldest == NULL || (oldest->in_use && e->used < oldest->used)) {
			oldest = e;
		}
	}

	/* Both mtime and size: a directory whose entry count changes but
	 * whose mtime granularity hides it is exactly the case a
	 * same-second publish would produce. */
	if (slot != NULL && slot->dir_mtime == dst.st_mtime && slot->dir_size == dst.st_size) {
		slot->used = ++g_recipe_cache_clock;
		snprintf(out_version, out_size, "%s", slot->version);
		return 0;
	}

	if (find_recipe_path(name, NULL, recipe_path, sizeof(recipe_path)) != 0)
		return -1;
	if (parse_recipe(recipe_path, &recipe) != 0)
		return -1;
	snprintf(out_version, out_size, "%s", recipe.version);

	if (slot == NULL)
		slot = oldest;
	if (slot != NULL) {
		snprintf(slot->name, sizeof(slot->name), "%s", name);
		slot->dir_mtime = dst.st_mtime;
		slot->dir_size = dst.st_size;
		snprintf(slot->version, sizeof(slot->version), "%s", recipe.version);
		slot->used = ++g_recipe_cache_clock;
		slot->in_use = 1;
	}
	return 0;
}

/*
 * Is this installed package behind the recipe on disk, and if so, what
 * is the recipe's version?
 *
 * ONE definition, three callers: write_pkg_json()'s available_version
 * field, pkg_find_update_candidate() (POST /pkg/update-all), and
 * pkg_write_drift_json() (GET /pkg/drift). ADR-0031 said this
 * comparison had been extracted so its call sites shared it rather
 * than keeping two copies; that was the intent and not what the code
 * did -- both sites carried their own find_recipe_path/parse_recipe/
 * strcmp, and a third was about to be added. Now it really is shared.
 *
 * Re-CHECKED on every call, via recipe_latest_version() above: one
 * stat of the recipe directory, and a full re-read only when something
 * published. The answer is never stale; it is simply not re-derived
 * from scratch 145 times a request.
 *
 * Only meaningful once installed: a package still fetching or building
 * was resolved from the current recipe by definition, so it cannot be
 * behind it.
 */
static int pkg_entry_drift(const struct pkg_entry *e, char *out_available, size_t out_size)
{
	char latest[PKG_VERSION_MAX];

	if (e == NULL || !e->in_use || e->state != PKG_STATE_INSTALLED)
		return 0;
	if (recipe_latest_version(e->name, latest, sizeof(latest)) != 0)
		return 0;
	if (strcmp(latest, e->version) == 0)
		return 0;
	if (out_available != NULL && out_size > 0)
		snprintf(out_available, out_size, "%s", latest);
	return 1;
}

/*
 * One mapping from state to its wire name. Extracted when a second
 * caller arrived (pkg_write_json_config, ADR-0206) rather than copied,
 * because two switches over the same enum are exactly the pair that
 * drifts the next time a state is added -- one gets the new case and
 * the other silently reports "failed".
 */
static const char *pkg_state_name(enum pkg_state state)
{
	switch (state) {
	case PKG_STATE_FETCHING:
		return "fetching";
	case PKG_STATE_BUILDING:
		return "building";
	case PKG_STATE_INSTALLED:
		return "installed";
	case PKG_STATE_FAILED:
	default:
		return "failed";
	}
}

static void write_pkg_json(const struct pkg_entry *e, struct json_writer *w)
{
	int i;
	const char *state_str = pkg_state_name(e->state);
	char available_version[PKG_VERSION_MAX];
	int has_available = 0;

	has_available = pkg_entry_drift(e, available_version, sizeof(available_version));

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, e->name);
	jw_key(w, "image");
	jw_str(w, e->image);
	jw_key(w, "version");
	jw_str(w, e->version);
	/*
	 * What the recipe declared this needs in order to RUN, as captured
	 * once at fetch time (ADR-0302) -- not re-read from whatever recipe
	 * revision is highest now.
	 *
	 * Added with ADR-0303 (#465), which turns on this field being
	 * observable: a hostbuild now carries a declared pkg_depends
	 * without resolving it, and "carried" is only distinguishable from
	 * "silently ignored" if something can read it back. It was already
	 * persisted by save_state() and already read by the install path's
	 * own gate -- measured on 192.168.15.95, 2026-09-17: the REST
	 * entry had no `depends` key at all, so the declaration was
	 * invisible to every client of the API.
	 */
	jw_key(w, "depends");
	jw_str(w, e->depends);
	jw_key(w, "state");
	jw_str(w, state_str);
	/* is_hostbuild/artifact_path are derived from e->image, never
	 * stored -- One Source of Truth, no separate flag that could drift
	 * from what pkg_find(name, PKG_HOSTBUILD_IMAGE) itself already
	 * means (ADR-0056). */
	jw_key(w, "is_hostbuild");
	jw_bool(w, strcmp(e->image, PKG_HOSTBUILD_IMAGE) == 0);
	jw_key(w, "artifact_path");
	if (strcmp(e->image, PKG_HOSTBUILD_IMAGE) == 0 && e->state == PKG_STATE_INSTALLED) {
		char artifact_dir[PATH_MAX];

		snprintf(artifact_dir, sizeof(artifact_dir), "%s/%s", g_artifacts_dir, e->name);
		jw_str(w, artifact_dir);
	} else {
		jw_null(w);
	}
	/*
	 * Issue #171: whether this package's artifact tarball exists in
	 * this host's own cache -- i.e. whether it can be published right
	 * now, and whether those bytes exist anywhere but this disk.
	 *
	 * Computed live, never stored: it is a question about a file, and
	 * a stored flag would be one more thing that can disagree with the
	 * filesystem (same reasoning is_hostbuild/artifact_path above give
	 * for deriving rather than storing).
	 *
	 * This is the field whose absence let a real gap hide for five
	 * releases. `cix` sat at v2.2.0-rc30 in the artifact cache while
	 * the host that built it ran rc35, because a hostbuild enqueued a
	 * push for a tarball nothing had built (#200) -- and nothing in
	 * this view could distinguish "published" from "exists only here".
	 * It was found by comparing two systems by hand.
	 *
	 * What it does NOT claim: that the artifact is on the configured
	 * server. Another host may have published those bytes, and this
	 * host may have pushed successfully and since had its cache
	 * evicted. Answering that truthfully needs either a network probe
	 * per read or persisted push results -- see #171, deliberately not
	 * half-built here. This field answers only what the host can know
	 * for free, and its name says so.
	 */
	jw_key(w, "artifact_cached");
	jw_bool(w, e->state == PKG_STATE_INSTALLED &&
	                  pkg_artifact_cache_has(e->name, e->version));
	jw_key(w, "error");
	if (e->error[0] != '\0')
		jw_str(w, e->error);
	else
		jw_null(w);
	/*
	 * ADR-0256: WHERE this package stands in the pipeline and WHAT
	 * happened there, so a caller deciding whether to retry does not
	 * have to read prose. Both null when there is nothing wrong, rather
	 * than "none"/"ok" -- absence of a failure is not a position in the
	 * pipeline, and a stage name here would invite a reader to believe
	 * the package is sitting at it.
	 */
	jw_key(w, "stage");
	if (e->status != PIPELINE_OK)
		jw_str(w, pipeline_stage_name(e->stage));
	else
		jw_null(w);
	jw_key(w, "status");
	if (e->status != PIPELINE_OK)
		jw_str(w, pipeline_status_name(e->status));
	else
		jw_null(w);
	/*
	 * Issue #423: when this job started, and how long it has been
	 * going. Both clocks already existed on the entry -- ADR-0272's
	 * run_started_at and the stall detector's build_started_at -- and
	 * neither was ever reported, so an operator watching a build could
	 * see that output was 60 seconds old and not that the build was in
	 * its fortieth minute.
	 *
	 * Two of them because they answer different questions. run_started_at
	 * is when the JOB began, fetching included, which is what "how long
	 * has this been running" means to somebody waiting for it;
	 * build_started_at is when compilation began, and a run that never
	 * got past fetching has no build start at all. Absolute epoch
	 * seconds, so a client can render a real start time -- something a
	 * relative "seconds ago" cannot do.
	 *
	 * run_seconds is the same span computed HERE, and it is not
	 * redundant. A dashboard ticking a counter from an absolute
	 * timestamp differences the daemon's clock against the browser's,
	 * so any skew between them shows up as a wrong elapsed time from
	 * the very first frame. Handing the client a server-computed
	 * baseline to tick locally has no skew in it, which is the same
	 * reason last_output_seconds_ago below is relative.
	 *
	 * Gated on an OPEN run (run_started_at != 0, which ADR-0272 defines
	 * as exactly that) rather than on a state, so a finished entry
	 * reports null instead of a start time for a job that is over --
	 * the same discipline build_container below is gated by, and for
	 * the same reason: a stale timestamp reads as a live one.
	 */
	jw_key(w, "run_started_at");
	if (e->run_started_at > 0)
		jw_int(w, (long long)e->run_started_at);
	else
		jw_null(w);
	jw_key(w, "build_started_at");
	if (e->run_started_at > 0 && e->build_started_at > 0)
		jw_int(w, (long long)e->build_started_at);
	else
		jw_null(w);
	jw_key(w, "run_seconds");
	if (e->run_started_at > 0)
		jw_int(w, (long long)(time(NULL) - e->run_started_at));
	else
		jw_null(w);
	/* Issue #58: the first-class hang-vs-slow signal -- null unless a
	 * build is in flight and has produced at least one byte. */
	jw_key(w, "last_output_seconds_ago");
	if (e->state == PKG_STATE_BUILDING && e->last_output_at > 0)
		jw_int(w, (long long)(time(NULL) - e->last_output_at));
	else
		jw_null(w);
	/*
	 * Issue #407: the container this build is running in RIGHT NOW, so
	 * an operator watching one of up to max_concurrent_jobs builds can
	 * reach it through the container API -- stats, processes, console --
	 * instead of guessing which chain slot it took. The name has always
	 * existed on the entry (build_container_name, ADR-0157 Phase 2);
	 * only kept_build_container below was ever reported, which is the
	 * failure case.
	 *
	 * Gated on PKG_STATE_BUILDING, and that gate is the whole
	 * correctness of the field rather than a convenience. Nothing
	 * clears build_container_name when a build ENDS -- it is cleared at
	 * line ~7394 only when a later entry claims the same chain slot --
	 * so emitting it unconditionally would name a torn-down container
	 * for an installed package, and after slot reuse would name a
	 * container running somebody else's build. Same gate
	 * last_output_seconds_ago uses, for the same reason.
	 *
	 * Null during PKG_STATE_FETCHING too: the container does not exist
	 * until pkg_fetch_completed() creates it.
	 */
	jw_key(w, "build_container");
	if (e->state == PKG_STATE_BUILDING && e->build_container_name[0] != '\0')
		jw_str(w, e->build_container_name);
	else
		jw_null(w);
	/* ADR-0175/issue #35: the exited, still-registered build
	 * container's name, exactly when keep_on_failure preserved one for
	 * this entry's most recent failed attempt -- null otherwise. */
	jw_key(w, "kept_build_container");
	if (e->kept_build_container[0] != '\0')
		jw_str(w, e->kept_build_container);
	else
		jw_null(w);
	jw_key(w, "available_version");
	if (has_available)
		jw_str(w, available_version);
	else
		jw_null(w);
	jw_key(w, "files");
	jw_arr_open(w);
	for (i = 0; i < e->file_count; i++)
		jw_str(w, e->files[i]);
	jw_arr_close(w);
	jw_obj_close(w);
}

static int save_state(void)
{
	struct json_writer w;
	int rc, i, j;

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		struct pkg_entry *e = &g_packages[i];

		if (!e->in_use || e->state != PKG_STATE_INSTALLED)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, e->name);
		jw_key(&w, "image");
		jw_str(&w, e->image);
		jw_key(&w, "version");
		jw_str(&w, e->version);
		jw_key(&w, "depends");
		jw_str(&w, e->depends);
		jw_key(&w, "files");
		jw_arr_open(&w);
		for (j = 0; j < e->file_count; j++)
			jw_str(&w, e->files[j]);
		jw_arr_close(&w);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	rc = persist_atomic_write(g_installed_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int parse_persisted_entry(const struct json_value *item, struct pkg_entry *slot)
{
	const char *name = json_as_string(json_object_get(item, "name"));
	const char *image = json_as_string(json_object_get(item, "image"));
	const char *version = json_as_string(json_object_get(item, "version"));
	const char *depends = json_as_string(json_object_get(item, "depends"));
	const struct json_value *jfiles = json_object_get(item, "files");
	size_t i;

	if (!pkg_name_is_valid(name) || version == NULL || jfiles == NULL ||
	    jfiles->type != JSON_ARRAY)
		return -1;
	/* Absent on entries persisted before per-image tracking existed --
	 * normalize_image() correctly defaults that to "base", preserving
	 * every pre-existing state file's meaning exactly. */
	if (image != NULL && image[0] != '\0' && !pkg_image_is_valid(image))
		return -1;

	memset(slot, 0, sizeof(*slot));
	strncpy(slot->name, name, sizeof(slot->name) - 1);
	strncpy(slot->image, normalize_image(image), sizeof(slot->image) - 1);
	strncpy(slot->version, version, sizeof(slot->version) - 1);
	if (depends != NULL)
		strncpy(slot->depends, depends, sizeof(slot->depends) - 1);
	slot->state = PKG_STATE_INSTALLED;
	slot->in_use = 1;

	for (i = 0; i < jfiles->u.array.count; i++) {
		const char *f = json_as_string(jfiles->u.array.items[i]);

		if (f == NULL || pkg_entry_add_file(slot, f) != 0)
			return -1;
	}
	return 0;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int rc = 0;
	int count = 0;

	if (persist_read_file(g_installed_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0;

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted pkg state\n", g_installed_state_path);
		return -1;
	}
	if (root->u.array.count > PKG_MAX_PACKAGES) {
		json_free(root);
		fprintf(stderr, "%s: more packages persisted than PKG_MAX_PACKAGES\n",
		        g_installed_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count; i++) {
		int j, dup = 0;

		if (parse_persisted_entry(root->u.array.items[i], &g_packages[count]) != 0) {
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_installed_state_path, i);
			rc = -1;
			break;
		}
		for (j = 0; j < count; j++) {
			if (strcmp(g_packages[j].name, g_packages[count].name) == 0 &&
			    strcmp(g_packages[j].image, g_packages[count].image) == 0) {
				dup = 1;
				break;
			}
		}
		if (dup) {
			fprintf(stderr, "%s: duplicate name at index %zu\n", g_installed_state_path, i);
			rc = -1;
			break;
		}
		/*
		 * Issue #56: a daemon death mid-job (hard reset, kill -9)
		 * used to strand the row in fetching/building forever -- the
		 * in-memory chain slot it referenced no longer exists after a
		 * restart, so `pkg build-log` 400/404'd and the row's own
		 * state never resolved without hand-editing. A freshly-loaded
		 * state file describing an in-flight job is by definition
		 * describing a job that no longer exists: settle it to failed
		 * with an honest reason, at the one place every restart passes
		 * through.
		 */
		if (g_packages[count].state == PKG_STATE_FETCHING ||
		    g_packages[count].state == PKG_STATE_BUILDING) {
			const char *phase =
			    g_packages[count].state == PKG_STATE_FETCHING ? "fetch" : "build";

			/* Issue #101: the phase it died in IS the stage -- a job
			 * killed mid-fetch stands at `fetch` to anyone deciding
			 * what to do about it, and the same for a build. */
			pkg_fail(&g_packages[count], 0,
			         g_packages[count].state == PKG_STATE_FETCHING ? PIPELINE_FETCH
			                                                        : PIPELINE_BUILD,
			         "interrupted by a daemon restart mid-%s", phase);
		}
		count++;
	}
	json_free(root);
	return rc;
}

/*
 * ADR-0141 Phase 4: repoints every REBUILDABLE_DIR-derived path this
 * module caches, without touching in-memory package state, any
 * in-flight job, or image_recipe_init()'s own last-apply-status
 * fields -- see network_repoint()'s own doc comment (daemon/src/
 * network.c) for the shared reasoning every STATE_DIR-backed module's
 * *_repoint() already established; this module's own multiple
 * concerns (main package state, repo-sync config, cache config,
 * artifact config, image-recipe-apply state) each get the identical
 * treatment below via their own repoint functions. containers_dir is
 * deliberately not a parameter here -- CONTAINERS_DIR never moves as
 * part of a rebuildable-storage migration, nothing to repoint.
 */
void pkg_repoint(const char *pkg_dir, const char *installed_state_path, const char *images_dir,
                  const char *artifacts_dir)
{
	snprintf(g_pkg_dir, sizeof(g_pkg_dir), "%s", pkg_dir);
	snprintf(g_recipes_dir, sizeof(g_recipes_dir), "%s/recipes", pkg_dir);
	snprintf(g_sources_dir, sizeof(g_sources_dir), "%s/sources", pkg_dir);
	snprintf(g_installed_state_path, sizeof(g_installed_state_path), "%s", installed_state_path);
	snprintf(g_images_dir, sizeof(g_images_dir), "%s", images_dir);
	snprintf(g_artifacts_dir, sizeof(g_artifacts_dir), "%s", artifacts_dir);
	image_recipe_repoint(pkg_dir);
	container_recipe_repoint(pkg_dir);
}

int pkg_init(const char *pkg_dir, const char *installed_state_path, const char *containers_dir,
              const char *images_dir, const char *artifacts_dir)
{
	if (snprintf(g_pkg_dir, sizeof(g_pkg_dir), "%s", pkg_dir) >= (int)sizeof(g_pkg_dir))
		return -1;
	if (snprintf(g_recipes_dir, sizeof(g_recipes_dir), "%s/recipes", pkg_dir) >=
	    (int)sizeof(g_recipes_dir))
		return -1;
	if (snprintf(g_sources_dir, sizeof(g_sources_dir), "%s/sources", pkg_dir) >=
	    (int)sizeof(g_sources_dir))
		return -1;
	if (snprintf(g_installed_state_path, sizeof(g_installed_state_path), "%s",
	             installed_state_path) >= (int)sizeof(g_installed_state_path))
		return -1;
	if (snprintf(g_containers_dir, sizeof(g_containers_dir), "%s", containers_dir) >=
	    (int)sizeof(g_containers_dir))
		return -1;
	if (snprintf(g_images_dir, sizeof(g_images_dir), "%s", images_dir) >= (int)sizeof(g_images_dir))
		return -1;
	if (snprintf(g_artifacts_dir, sizeof(g_artifacts_dir), "%s", artifacts_dir) >=
	    (int)sizeof(g_artifacts_dir))
		return -1;

	memset(g_packages, 0, sizeof(g_packages));
	memset(g_chains, 0, sizeof(g_chains));
	memset(g_build_output_entries, 0, sizeof(g_build_output_entries));
	image_recipe_init(pkg_dir);
	container_recipe_init(pkg_dir);
	/*
	 * ADR-0272: before load_state(), and deliberately not inside it.
	 * load_state() returns early when the installed-state file is not
	 * there, which is every fresh install -- a run history hooked in
	 * after that would silently never load on exactly the hosts whose
	 * first runs are most worth keeping.
	 */
	pkg_runs_load();
	return load_state();
}

/*
 * Live-copy fallback: stages a toolchain by copying from whatever host
 * cixd itself happens to be running on. Fine for dev/test convenience
 * (this build sandbox has a real toolchain); silently produces an empty,
 * non-functional pkgbuild rootfs on a real minimal install, which has
 * none of this under its own /usr -- confirmed live (a real `pkg
 * install` on a fresh install has nothing to build with). The correct
 * production path is pkg_bootstrap_from_toolchain() below, importing a
 * real, portable artifact instead -- this fallback stays for the dev/
 * test case where reaching into the live host genuinely works, and so
 * the existing bare `POST /v1/pkg/bootstrap` (no body) keeps behaving
 * exactly as it always has.
 */
/* image_produce_new_version()'s mutate() for the dev/test seed. */
enum pkg_error pkg_bootstrap_build_image(void)
{
	/*
	 * Issue #168: refused, permanently.
	 *
	 * This used to copy the build host's whole /usr/{include,lib,
	 * lib64,bin,libexec} into an image. On any real host that is
	 * everything installed on it -- the image it produced held
	 * rustup, cargo, chromium, node, go, erlang, qemu, sudo and
	 * several gcc trees, 88,798 entries of which 93% belonged to no
	 * package in its manifest.
	 *
	 * It cannot be narrowed into correctness, because copying another
	 * operating system's files is the thing that is wrong, not the
	 * amount copied. Everything Cix ships is built on a Cix host with
	 * Cix's own toolchain; a live copy of whatever this machine
	 * happens to have installed is the opposite of that.
	 *
	 * Nothing needs it any more either. Build environments are
	 * composed from a recipe's declared tools (ADR-0199), and a fresh
	 * box gets its first packages from checksum-verified artifacts
	 * built by a Cix host -- which needs no build environment at all
	 * (the cache-hit path). The remaining honest seed is
	 * pkg_bootstrap_from_toolchain(), an explicit, checksummed
	 * artifact the operator names.
	 */
	return PKG_ERR_INVALID_TOOLCHAIN;
}

struct toolchain_seed_ctx {
	const char *toolchain_path;
};

/*
 * -no-xattrs: the build sandbox is build tooling, not something needing
 * POSIX capabilities/ACLs preserved on its own binaries -- confirmed
 * directly that without this, unsquashfs exits nonzero on a destination
 * filesystem that cannot store xattrs (e.g. tmpfs, boot_init()'s own
 * containers-partition fallback) even though the extraction fully
 * succeeds; the tool's own diagnostic names this exact flag as the fix.
 *
 * -no-exit-code is the general form of that same problem, and the
 * reason this call does not simply use run_subprocess()'s "any nonzero
 * is failure" rule. unsquashfs documents three distinct exit codes:
 *
 *   0  extracted OK
 *   1  FATAL -- corruption or I/O error; it aborted partway
 *   2  NON-FATAL -- "unsquashfs continued and did not abort"; e.g. it
 *      could not write permissions, or the output filesystem does not
 *      support something in the archive
 *
 * Treating 2 as failure throws away a complete, correct extraction.
 * That is not hypothetical: a real bootstrap on a freshly installed
 * host failed with "fetched toolchain failed to stage (invalid
 * squashfs?)" on a toolchain image that extracts perfectly -- verified
 * by running this exact binary, with these exact flags, against the
 * same archive, which exits 0 in one environment and 2 in another while
 * producing the same tree. The -no-xattrs comment above had already
 * observed this behaviour once; only the xattr instance of it got
 * fixed.
 *
 * So the exit status is inspected here rather than delegated: 1 stays
 * fatal, 2 is accepted and said out loud, so a real degradation is
 * visible in the log instead of either failing the boot or passing
 * silently.
 */
static int toolchain_seed_mutate(const char *staging_rootfs, void *ctx_v)
{
	struct toolchain_seed_ctx *ctx = ctx_v;
	char *argv[] = { (char *)PKG_UNSQUASHFS_BIN, "-f", "-no-xattrs", "-d",
		          (char *)staging_rootfs, (char *)ctx->toolchain_path, NULL };
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		logstore_write("cixd", "error", "toolchain seed: fork failed: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execve(PKG_UNSQUASHFS_BIN, argv, environ);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid) {
		logstore_write("cixd", "error", "toolchain seed: waitpid failed: %s", strerror(errno));
		return -1;
	}
	if (!WIFEXITED(status)) {
		logstore_write("cixd", "error", "toolchain seed: unsquashfs did not exit normally");
		return -1;
	}
	switch (WEXITSTATUS(status)) {
	case 0:
		return 0;
	case 2:
		logstore_write("cixd", "info",
		                "toolchain seed: unsquashfs reported non-fatal errors (exit 2) -- the "
		                "extraction completed and is being kept; some permissions or "
		                "attributes could not be reproduced on this filesystem");
		return 0;
	case 127:
		logstore_write("cixd", "error",
		                "toolchain seed: could not execute %s (missing binary or bad path?)",
		                PKG_UNSQUASHFS_BIN);
		return -1;
	default:
		logstore_write("cixd", "error",
		                "toolchain seed: unsquashfs failed fatally (exit %d) -- the archive is "
		                "corrupt or unreadable",
		                WEXITSTATUS(status));
		return -1;
	}
}

enum pkg_error pkg_bootstrap_from_toolchain(const char *toolchain_path)
{
	/*
	 * -no-xattrs: the pkgbuild rootfs is build tooling, not something
	 * needing POSIX capabilities/ACLs preserved on its own binaries --
	 * confirmed directly that without this, unsquashfs exits nonzero
	 * (PKG_ERR_SPAWN_FAILED) on a destination filesystem that can't
	 * store xattrs (e.g. tmpfs, boot_init()'s own containers-partition
	 * fallback) even though the actual extraction fully succeeds; the
	 * tool's own diagnostic message names this exact flag as the fix.
	 */
	struct toolchain_seed_ctx seed_ctx;

	/*
	 * Real, on-disk squashfs magic check via squashfs_image_check()
	 * (squashfsimg.h) -- which is where the open()+read() this used to
	 * carry inline now lives, along with the reasoning this comment
	 * used to hold: it is deliberately not stat()+S_ISREG, because a
	 * squashfs image's bytes are equally valid backing a regular file
	 * (the real, intended operator usage -- scp'd onto a real
	 * filesystem path) or a raw block device, which is what
	 * test_boot_update.c's --test-update-image= relies on.
	 *
	 * This comment named do_system_update()'s copy as its precedent,
	 * which is exactly the shape "No Parallel Implementations"
	 * forbids; a third copy in the ISO builder was not this check at
	 * all, and shipped the bug (#481).
	 */
	if (squashfs_image_check(toolchain_path) != 1)
		return PKG_ERR_INVALID_TOOLCHAIN;

	seed_ctx.toolchain_path = toolchain_path;
	if (image_produce_new_version(PKG_BUILD_SANDBOX_IMAGE, toolchain_seed_mutate, &seed_ctx,
	                               toolchain_path) != 0)
		return PKG_ERR_SPAWN_FAILED;

	return PKG_OK;
}

/*
 * Issue #40, one-time migration. Before the sandbox was a real image it
 * lived at a flat <images_dir>/pkgbuild/rootfs, grown by every install.
 * Simply resolving the image instead would silently DISCARD all of that
 * -- turning a structural refactor into an unannounced content change,
 * which is exactly the kind of thing that should never ride along
 * inside one.
 *
 * So the flat directory is folded in as a real version of the image,
 * once, and then set aside rather than deleted: renamed with the
 * version it produced, so the previous state is still on disk if the
 * migration turns out to have been wrong. What the sandbox CONTAINS is
 * unchanged by this commit; only how it is tracked changes.
 *
 * Cleaning up what accreted in there (issue #37) stays a separate,
 * deliberate act -- and is now something an operator can actually do,
 * because the content finally has versions to inspect and roll back to.
 */
static void pkg_migrate_flat_sandbox(const char *images_dir);

/*
 * Set only when a flat sandbox exists that could NOT be migrated. While
 * it is set, builds keep using it exactly as they always did.
 *
 * This exists because the first version of this change did not have it,
 * and the consequence was immediate and total on the real box: the
 * migration failed, builds silently switched to the image's own
 * declared content, and the very next install died on `sed: command not
 * found` -- sed being one of the many packages that had accreted into
 * the flat sandbox and was never in the image's manifest. Which is,
 * precisely, the thing this whole issue is about; it simply bit the
 * migration first.
 *
 * So a failed migration must not change what builds run against. It
 * degrades to the old behaviour and says so, rather than proceeding
 * with content that is missing most of what recipes depend on.
 */
static char g_flat_sandbox_fallback[PATH_MAX];

struct sandbox_migrate_ctx {
	const char *flat_rootfs;
};

static int sandbox_migrate_mutate(const char *staging_rootfs, void *ctx_v)
{
	struct sandbox_migrate_ctx *ctx = ctx_v;

	if (merge_tree(ctx->flat_rootfs, staging_rootfs, "", NULL) != 0) {
		logstore_write("cixd", "error",
		               "build sandbox migration: merging %s into the staging tree failed: %s",
		               ctx->flat_rootfs, strerror(errno));
		return -1;
	}
	return 0;
}

void pkg_migrate_build_sandbox(void)
{
	pkg_migrate_flat_sandbox(g_images_dir);
}

static void pkg_migrate_flat_sandbox(const char *images_dir)
{
	char flat[PATH_MAX];
	char retired[PATH_MAX];
	char version[IMAGE_VERSION_MAX];
	struct sandbox_migrate_ctx ctx;
	struct stat st;

	if (snprintf(flat, sizeof(flat), "%s/pkgbuild/rootfs", images_dir) >= (int)sizeof(flat))
		return;
	if (stat(flat, &st) != 0 || !S_ISDIR(st.st_mode)) {
		/* Already migrated, or never existed -- both fine, and both
		 * worth saying once. A migration nobody can see run is a
		 * migration nobody can trust ran. */
		logstore_write("cixd", "info",
		               "pkg: no flat build sandbox at %s -- nothing to migrate (issue #40)", flat);
		return;
	}

	ctx.flat_rootfs = flat;
	if (image_produce_new_version(PKG_BUILD_SANDBOX_IMAGE, sandbox_migrate_mutate, &ctx,
	                               "migrate:flat-pkgbuild-rootfs") != 0) {
		snprintf(g_flat_sandbox_fallback, sizeof(g_flat_sandbox_fallback), "%s", flat);
		logstore_write("cixd", "error",
		               "pkg: could not migrate the flat build sandbox at %s into %s -- keeping "
		               "it, and builds keep using it, exactly as before. The image's own "
		               "content is NOT a substitute: it lacks everything that only ever "
		               "accreted here (issue #40)",
		               flat, PKG_BUILD_SANDBOX_IMAGE);
		return;
	}
	if (image_current_version(PKG_BUILD_SANDBOX_IMAGE, version, sizeof(version)) != IMAGE_OK)
		return;
	if (snprintf(retired, sizeof(retired), "%s/pkgbuild/rootfs.migrated-%s", images_dir,
	             version) < (int)sizeof(retired))
		rename(flat, retired);
	logstore_write("cixd", "info",
	               "pkg: migrated the flat build sandbox into %s version %s (previous content "
	               "kept at %s, not deleted) (issue #40)",
	               PKG_BUILD_SANDBOX_IMAGE, version, retired);
}

/*
 * Where the build sandbox's content currently lives. Resolved fresh on
 * every call, because "current" genuinely moves: each install produces
 * a new version, and a caller holding yesterday's path would be reading
 * an immutable directory nothing writes to any more.
 *
 * Returns -1 when the sandbox has never been bootstrapped, which is a
 * real state (a box that has never run `pkg bootstrap`) and not an
 * error to paper over with a path that does not exist.
 */
/*
 * ---- Issue #109: a build environment composed from a recipe's own
 * ---- declared build tools, and nothing else (ADR-0199)
 *
 * The environment a package builds in is a property of the recipe, not
 * of this box's install history. A recipe naming its build tools gets a
 * container whose lowerdir contains exactly those packages' own files:
 * no extras to accidentally depend on, and nothing missing that the
 * build will not immediately name.
 *
 * Composed from each declared package's own RECORDED FILE LIST -- the
 * exact paths that package contributed, which is the only honest
 * definition of "what this package provides". Not the image it lives in
 * (that is a union of whatever was installed into it), and not the
 * shared sandbox (which is the problem).
 *
 * Deliberately the manifest and not the build cache, though the cache
 * holds the same bytes: the cache is a cache, prunable by size, so
 * composing from it would mean a build environment could become
 * unbuildable because an entry aged out. The file list is durable state
 * -- it is what an upgrade already unlinks against -- so an environment
 * stays reproducible for as long as the packages are installed.
 *
 * The result is an ordinary image named for the hash of the declared
 * set, so the same set is composed once and reused forever, and two
 * different sets can never collide. That reuse is not an optimisation
 * bolted on: it is what makes "declare your tools" cheap enough to
 * apply everywhere.
 */
#define PKG_BUILDENV_IMAGE_PREFIX "__buildenv-"
#define PKG_BUILDENV_MAX_TOOLS 32

struct buildenv_tool {
	char name[PKG_NAME_MAX];
	char version[PKG_VERSION_MAX];
	/* The installed entry it resolved to -- its file list is what gets
	 * copied, and its image is where those files currently live. */
	struct pkg_entry *entry;
};

struct buildenv_ctx {
	const struct buildenv_tool *tools;
	int tool_count;
};

/*
 * Every declared tool must be a package this box has actually built --
 * we compose from its cached output, so "installed somewhere" is not
 * enough on its own, the cache entry has to be there too.
 *
 * Refused, never substituted: a build environment missing a tool the
 * recipe declared is not a build environment, and quietly falling back
 * to something fuller would reintroduce exactly the fungibility this
 * exists to remove.
 */
/*
 * Adds one package -- a declared build tool, or something a declared
 * build tool needs in order to RUN -- to the environment being
 * resolved, then does the same for whatever it depends on.
 *
 * The recursion is not decoration. A build environment holds exactly
 * what the recipe declares and nothing else, so a tool arriving without
 * the libraries it links against is not a lean environment, it is a
 * broken one. Confirmed on the real box the first time this was
 * exercised: zlib declared `binutils` for `ar`, the environment
 * received binutils' own files alone, and `ar` died with "error while
 * loading shared libraries: libz.so.1". Under the old shared sandbox
 * that library merely happened to be lying around.
 *
 * `via` names whatever pulled this in, so a failure three levels down
 * still reports which declared tool the operator actually wrote.
 */
/*
 * Orders two package version strings the way a person reads them:
 * digit runs compare numerically, everything else lexically. So
 * "2.36-3" is newer than "2.36", and "1.3.2-10" is newer than
 * "1.3.2-9" -- which a plain strcmp() gets exactly backwards.
 *
 * This exists because a build environment has to pick ONE version of a
 * declared tool, and the same package is routinely installed at
 * different versions in different images. Taking whichever the registry
 * happened to list first made the environment depend on install order,
 * which is precisely the non-determinism ADR-0199 exists to remove --
 * and it failed a real build: `libc-dev` resolved to 2.36, which does
 * not stage libm.so, while 2.36-3, which does, sat installed in two
 * other images. Newest wins, and it is the same answer every time.
 */
static int pkg_version_cmp(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			long na = 0, nb = 0;

			while (isdigit((unsigned char)*a))
				na = na * 10 + (*a++ - '0');
			while (isdigit((unsigned char)*b))
				nb = nb * 10 + (*b++ - '0');
			if (na != nb)
				return na < nb ? -1 : 1;
			continue;
		}
		if (*a != *b)
			return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
		a++;
		b++;
	}
	if (*a == *b)
		return 0;
	return *a == '\0' ? -1 : 1;
}

static int buildenv_add_tool(const char *name, const char *via, struct buildenv_tool *out, int max,
                              int *n, char *err, size_t err_size, int depth)
{
	struct pkg_entry *found = NULL;
	const char *unusable = NULL;
	char version[IMAGE_VERSION_MAX];
	char deps_copy[PKG_DEPENDS_MAX];
	char *tok, *save = NULL;
	char bare_name[PKG_NAME_MAX];
	const char *want_version = NULL;
	int i;

	/*
	 * Issue #127: a declared build tool may be version-pinned as
	 * "name@version" (e.g. "tcc@0.9.27-7", to require the revision that
	 * fixes a miscompile). Split it here: bare_name is what matches a
	 * package's own name, want_version (if present and non-empty) is the
	 * exact installed version required. A bare "name" pins nothing and
	 * resolves to the newest usable copy, exactly as before.
	 */
	{
		const char *at = strchr(name, '@');

		if (at != NULL) {
			size_t bl = (size_t)(at - name);

			if (bl >= sizeof(bare_name))
				bl = sizeof(bare_name) - 1;
			memcpy(bare_name, name, bl);
			bare_name[bl] = '\0';
			want_version = (at[1] != '\0') ? at + 1 : NULL;
		} else {
			snprintf(bare_name, sizeof(bare_name), "%s", name);
		}
	}

	for (i = 0; i < *n; i++) {
		if (strcmp(out[i].name, bare_name) == 0)
			return 0; /* already in the environment */
	}
	if (depth > PKG_BUILDENV_MAX_TOOLS) {
		snprintf(err, err_size, "dependency chain under build tool \"%s\" is too deep", via);
		return -1;
	}
	if (*n >= max) {
		snprintf(err, err_size, "more than %d packages in the composed build environment", max);
		return -1;
	}

	/*
	 * Any image will do -- a package's own built files are the same
	 * files whichever image it was installed into -- but only if that
	 * image can still be resolved to a real rootfs to copy them out of.
	 * A stale image left behind by an earlier era of this project
	 * ("cix-builder", on the first real box) still carries INSTALLED
	 * package rows while having no current version at all, and taking
	 * the first name match regardless meant a perfectly healthy tool
	 * installed in three good images resolved to the one broken one
	 * (issue #111). Checked here rather than during the copy so the
	 * failure names the package and the image, not a file path.
	 */
	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		const char *img;

		if (!g_packages[i].in_use)
			continue;
		/*
		 * INSTALLED, or mid-upgrade with a previous install's files
		 * still recorded (issue #166).
		 *
		 * A package that honestly declares ITSELF as a build tool --
		 * tcc, which compiles tcc; gcc, which bootstraps gcc -- could
		 * never be upgraded. start_fetch_for() sets the entry to
		 * FETCHING before composing anything, so the only entry that
		 * matches the name is no longer INSTALLED, and composition
		 * failed with "declared build tool \"tcc\" is not installed
		 * anywhere" while GET /v1/pkg showed it installed the whole
		 * time. The message was accurate about what it looked for and
		 * wrong about the world.
		 *
		 * The files are genuinely there. Image versions are immutable
		 * (ADR-0107/0108), so the target image's CURRENT version still
		 * holds the old package's files, unchanged, until a new
		 * version is produced at the very end -- which is exactly what
		 * start_fetch_for() already documents about why it leaves
		 * e->version/e->files alone through an in-place upgrade. This
		 * check was simply stricter than that invariant requires.
		 *
		 * file_count is the honest test rather than a new flag: it is
		 * "a previous install left files here to copy", which is the
		 * whole of what the composer needs. A first-time install gets
		 * a memset() fresh slot (0 files, correctly refused), and a
		 * retry after a FAILED attempt frees its files first (also 0,
		 * also correctly refused -- that build's files are not there).
		 */
		if (g_packages[i].state != PKG_STATE_INSTALLED &&
		    !((g_packages[i].state == PKG_STATE_FETCHING ||
		       g_packages[i].state == PKG_STATE_BUILDING) &&
		      g_packages[i].file_count > 0))
			continue;
		if (strcmp(g_packages[i].name, bare_name) != 0)
			continue;
		if (want_version != NULL && strcmp(g_packages[i].version, want_version) != 0)
			continue;
		img = normalize_image(g_packages[i].image);
		if (image_current_version(img, version, sizeof(version)) != IMAGE_OK) {
			unusable = img;
			continue;
		}
		/* Newest usable copy wins, wherever it lives -- never
		 * whichever the registry listed first. */
		if (found == NULL || pkg_version_cmp(g_packages[i].version, found->version) > 0)
			found = &g_packages[i];
	}
	if (found == NULL && unusable != NULL) {
		snprintf(err, err_size,
		         "%s \"%s\" is installed only in image \"%s\", which has no current version to "
		         "copy it out of",
		         via == NULL ? "declared build tool" : "runtime dependency", name, unusable);
		return -1;
	}
	if (found == NULL) {
		if (via == NULL)
			snprintf(err, err_size,
			         "declared build tool \"%s\" is not installed anywhere, so there is nothing "
			         "to compose a build environment from -- install it first",
			         name);
		else
			snprintf(err, err_size,
			         "\"%s\" needs \"%s\" to run, but that is not installed anywhere -- the "
			         "build environment would hold a tool that cannot start",
			         via, name);
		return -1;
	}
	if (found->file_count <= 0) {
		snprintf(err, err_size,
		         "%s \"%s@%s\" has no recorded files, so there is nothing to compose from -- "
		         "reinstall it",
		         via == NULL ? "declared build tool" : "runtime dependency", found->name,
		         found->version);
		return -1;
	}

	snprintf(out[*n].name, sizeof(out[*n].name), "%s", found->name);
	snprintf(out[*n].version, sizeof(out[*n].version), "%s", found->version);
	out[*n].entry = found;
	(*n)++;

	/* What this package needs in order to run, as recorded when it was
	 * installed -- not as the current recipe now says, since the
	 * environment is built from the installed copy. */
	snprintf(deps_copy, sizeof(deps_copy), "%s", found->depends);
	for (tok = strtok_r(deps_copy, " \t", &save); tok != NULL; tok = strtok_r(NULL, " \t", &save)) {
		if (buildenv_add_tool(tok, found->name, out, max, n, err, err_size, depth + 1) != 0)
			return -1;
	}
	return 0;
}

static int buildenv_resolve_tools(const char *declared, struct buildenv_tool *out, int max,
                                   char *err, size_t err_size)
{
	char buf[PKG_DEPENDS_MAX];
	char *tok;
	char *save = NULL;
	int n = 0;

	snprintf(buf, sizeof(buf), "%s", declared);
	for (tok = strtok_r(buf, " \t", &save); tok != NULL; tok = strtok_r(NULL, " \t", &save)) {
		if (buildenv_add_tool(tok, NULL, out, max, &n, err, err_size, 0) != 0)
			return -1;
	}
	if (n == 0) {
		snprintf(err, err_size, "pkg_build_depends is set but names no packages");
		return -1;
	}
	/*
	 * The C library, last, so a recipe that pinned its own version
	 * above already occupies the slot -- buildenv_add_tool() dedups by
	 * name and returns success for one already present.
	 *
	 * Checked here rather than assumed: an environment holds exactly
	 * its tools' own manifest files, and no tool's manifest carries
	 * libc, so before this the loader arrived only because
	 * pkg_seed_image_baseline() copied one off the build host. With
	 * that gone and this absent, every build dies at
	 * execve(/usr/bin/bash) with ENOENT -- which reads as a missing
	 * bash and is really a missing loader (#186).
	 */
	if (buildenv_add_tool(PKG_BASE_LIBC, NULL, out, max, &n, err, err_size, 0) != 0)
		return -1;
	return n;
}

static int buildenv_tool_cmp(const void *a, const void *b)
{
	const struct buildenv_tool *ta = a;
	const struct buildenv_tool *tb = b;
	int c = strcmp(ta->name, tb->name);

	return c != 0 ? c : strcmp(ta->version, tb->version);
}

/*
 * Byte-for-byte, because "both files exist" is not the question -- the
 * question is whether the one in the environment is the one the package
 * built. Streamed rather than stat-compared: two different glibc builds
 * can be the same size.
 */
static int buildenv_files_identical(const char *a, const char *b)
{
	FILE *fa, *fb;
	int same = 1;

	fa = fopen(a, "rb");
	if (fa == NULL)
		return 0;
	fb = fopen(b, "rb");
	if (fb == NULL) {
		fclose(fa);
		return 0;
	}
	for (;;) {
		char ba[65536], bb[65536];
		size_t na = fread(ba, 1, sizeof(ba), fa);
		size_t nb = fread(bb, 1, sizeof(bb), fb);

		if (na != nb || memcmp(ba, bb, na) != 0) {
			same = 0;
			break;
		}
		if (na == 0)
			break;
	}
	fclose(fa);
	fclose(fb);
	return same;
}

/*
 * The environment must carry the C library this platform built, and
 * carry it intact.
 *
 * Composition copies each tool's files in turn, so whichever package is
 * copied last wins any path two packages both claim. glibc's own
 * objects share a private, version-locked interface (GLIBC_PRIVATE):
 * mixing halves of two builds produces something that fails at exec
 * with no useful diagnostic. Nothing in the catalogue collides with
 * these paths today -- libc-dev ships headers and link objects, not
 * libc.so.6 -- and this exists so that stays true by being checked
 * rather than by being remembered.
 *
 * This is the same discipline mkbootroot gained after the build host's
 * libm/libpthread/libresolv landed on top of the platform's own and
 * panicked a real machine at boot, twice, while assembly reported
 * success both times. Copying files is not the same as producing an
 * environment that runs.
 */
static int buildenv_verify_libc_intact(const char *staging_rootfs, const struct buildenv_ctx *ctx)
{
	static const char *const critical[] = {
		PKG_IMAGE_LOADER_REL,
		CIX_LIB_DIR_RUNTIME "/libc.so.6",
		/*
		 * A HEADER, not just a library, and it earns its place: this
		 * is the one another package actually did overwrite. libc-dev
		 * stages the build host's whole /usr/include, whose
		 * multiarch sys/cdefs.h is glibc 2.36's, and GCC searches
		 * that directory before /usr/include -- so our 2.44 header
		 * was never read and __COLD went undefined. Ordering now
		 * prevents it; this makes a future reordering fail loudly
		 * instead of silently compiling against the wrong libc's
		 * headers again (#187).
		 */
		"usr/include/sys/cdefs.h",
	};
	const struct pkg_entry *libc = NULL;
	char src_rootfs[PATH_MAX];
	char version[IMAGE_VERSION_MAX];
	size_t k;
	int i;

	for (i = 0; i < ctx->tool_count; i++) {
		if (strcmp(ctx->tools[i].name, PKG_BASE_LIBC) == 0) {
			libc = ctx->tools[i].entry;
			break;
		}
	}
	if (libc == NULL) {
		logstore_write("cixd", "error",
		               "build environment: no %s in the composed tool set -- nothing in it "
		               "could exec", PKG_BASE_LIBC);
		return -1;
	}
	if (image_current_version(normalize_image(libc->image), version, sizeof(version)) != IMAGE_OK)
		return -1;
	image_version_rootfs_path(normalize_image(libc->image), version, src_rootfs,
	                           sizeof(src_rootfs));

	for (k = 0; k < sizeof(critical) / sizeof(critical[0]); k++) {
		char want[PATH_MAX], got[PATH_MAX];

		snprintf(want, sizeof(want), "%s/%s", src_rootfs, critical[k]);
		snprintf(got, sizeof(got), "%s/%s", staging_rootfs, critical[k]);
		if (!buildenv_files_identical(want, got)) {
			logstore_write("cixd", "error",
			               "build environment: %s is not the copy %s@%s installed -- another "
			               "declared tool overwrote it, and an environment holding halves of "
			               "two C libraries cannot exec",
			               critical[k], libc->name, libc->version);
			return -1;
		}
	}
	return 0;
}

static int buildenv_mutate(const char *staging_rootfs, void *ctx_v)
{
	struct buildenv_ctx *ctx = ctx_v;
	int i;
	int pass;

	/* The same baseline every image gets: device nodes and the handful
	 * of files a process needs to start at all. Not a "tool", and not
	 * something a recipe should have to declare. */
	if (pkg_seed_image_baseline(staging_rootfs) != PKG_OK) {
		logstore_write("cixd", "error",
		               "build environment: seeding the image baseline failed");
		return -1;
	}
	/*
	 * Two passes, and the order is the point: everything else first,
	 * the C library LAST (#187).
	 *
	 * Tools are copied in sorted name order, so whichever sorts later
	 * wins any path two packages both claim. That put "libc-dev" after
	 * "glibc", and libc-dev stages the build host's entire
	 * /usr/include -- including a glibc 2.36
	 * /usr/include/x86_64-linux-gnu/sys/cdefs.h. GCC searches the
	 * multiarch include directory BEFORE /usr/include, so our own 2.44
	 * cdefs.h was never read: the 2.36 copy set the include guard
	 * first, __COLD was never defined, and 2.44's stdio.h fell apart on
	 * it. binutils reported that three steps downstream as "cannot run
	 * C compiled programs".
	 *
	 * This is exactly what mkbootroot had to learn: the platform's own
	 * C library must be the final word, or something else quietly
	 * becomes it. There it was libm/libpthread landing on top of ours
	 * and panicking a machine at boot; here it is headers, and the
	 * damage is a build compiled against one libc's headers while
	 * linking another's.
	 *
	 * The environment's identity is the sorted tool SET, not the copy
	 * order, so this changes no hash and reuses every existing
	 * environment unchanged.
	 */
	for (pass = 0; pass < 2; pass++)
	for (i = 0; i < ctx->tool_count; i++) {
		const struct pkg_entry *e = ctx->tools[i].entry;
		char src_rootfs[PATH_MAX];
		char version[IMAGE_VERSION_MAX];
		int f;
		int is_libc = strcmp(ctx->tools[i].name, PKG_BASE_LIBC) == 0;

		if ((pass == 0) == is_libc)
			continue;

		if (image_current_version(normalize_image(e->image), version, sizeof(version)) != IMAGE_OK) {
			logstore_write("cixd", "error",
			               "build environment: declared tool %s@%s lives in image \"%s\", "
			               "which has no current version", e->name, e->version,
			               normalize_image(e->image));
			return -1;
		}
		image_version_rootfs_path(normalize_image(e->image), version, src_rootfs,
		                           sizeof(src_rootfs));
		for (f = 0; f < e->file_count; f++) {
			char src[PATH_MAX];
			char dst[PATH_MAX];

			snprintf(src, sizeof(src), "%s/%s", src_rootfs, e->files[f]);
			snprintf(dst, sizeof(dst), "%s/%s", staging_rootfs, e->files[f]);
			/* One file of one declared package. A path that has gone
			 * missing from the image is a real inconsistency and fails
			 * the composition rather than producing a quietly
			 * incomplete environment. */
			if (copy_one_file_preserving(src, dst) != 0) {
				logstore_write("cixd", "error",
				               "build environment: copying %s from declared tool %s@%s "
				               "(image %s) failed: %s", e->files[f], e->name, e->version,
				               normalize_image(e->image), strerror(errno));
				return -1;
			}
		}
	}
	return buildenv_verify_libc_intact(staging_rootfs, ctx);
}

/*
 * ADR-0221 / issue #112: build environments are reclaimed by LAST USE.
 *
 * The stamp is a marker file's mtime inside the environment's own image
 * directory -- derived from the filesystem rather than kept in a second
 * registry that could disagree with it.
 */
#define PKG_BUILDENV_STAMP ".last-used"

static void buildenv_stamp_path(const char *env_image, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s/%s", g_images_dir, env_image,
	         PKG_BUILDENV_STAMP);
}

/* Called every time a build resolves an environment -- both the reuse
 * path and the compose path, since composing one is itself a use. */
static void buildenv_touch(const char *env_image)
{
	char path[PATH_MAX];
	int fd;

	buildenv_stamp_path(env_image, path, sizeof(path));
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return; /* a missing stamp is handled conservatively -- see buildenv_last_used() */
	close(fd);
}

/*
 * Seconds since this environment was last used, or -1 when that cannot
 * be determined.
 *
 * A MISSING stamp deliberately reports "used now" rather than "never
 * used": an environment composed by an older daemon has no marker, and
 * treating that as ancient would make the first daemon carrying this
 * change delete every pre-existing environment at once. Being
 * conservative costs one retention window.
 */
static long long buildenv_idle_seconds(const char *env_image)
{
	char path[PATH_MAX];
	struct stat st;
	time_t now = time(NULL);

	buildenv_stamp_path(env_image, path, sizeof(path));
	if (stat(path, &st) != 0)
		return 0;
	if (now < st.st_mtime)
		return 0; /* clock moved backwards -- never treat that as age */
	return (long long)(now - st.st_mtime);
}

/*
 * Resolves a recipe's declared build tools to an image containing
 * exactly them, creating it the first time and reusing it after.
 * Returns 0 and fills out_image, or -1 with a message saying which
 * declared tool could not be provided.
 */
/*
 * Resolves the build environment for a declared tool set, composing it
 * if it does not exist yet.
 *
 * Returns 0 when the environment is ready to use, 1 when composition
 * has been FORKED and out_pid/out_pidfd name the child, or -1 on a
 * failure described in err.
 *
 * The fork is #238. Composition walks and copies every file of every
 * declared tool -- for a toolchain that is gcc, glibc and binutils --
 * and it used to run inline on the event loop. Measured on a real host,
 * a burst of publishes that each triggered a rebuild left one client's
 * request unanswered for 247 seconds, with no stall recorded, because
 * the loop kept going round doing seconds of this per pass: alive by
 * the heartbeat and unusable by every other measure.
 *
 * A child is safe here because image state lives on disk, not in
 * memory -- image_current_version() re-reads manifest.json on every
 * call -- so everything the child produces is visible to the parent
 * simply by asking again. That is also why the resume path needs no
 * handover: it calls this function a second time and takes the
 * already-composed fast path above.
 */
static int buildenv_image_for(const char *declared, char *out_image, size_t out_image_size,
                               char *err, size_t err_size, pid_t *out_pid, int *out_pidfd)
{
	struct buildenv_tool tools[PKG_BUILDENV_MAX_TOOLS];
	char canonical[PKG_BUILDENV_MAX_TOOLS * (PKG_NAME_MAX + PKG_VERSION_MAX + 2)];
	char hash[IMAGE_VERSION_MAX];
	size_t off = 0;
	int count;
	int i;

	count = buildenv_resolve_tools(declared, tools, PKG_BUILDENV_MAX_TOOLS, err, err_size);
	if (count < 0)
		return -1;

	/* Sorted, so the identity is the SET and not the order it happened
	 * to be written in -- two recipes naming the same tools differently
	 * ordered share one environment. */
	qsort(tools, (size_t)count, sizeof(tools[0]), buildenv_tool_cmp);
	canonical[0] = '\0';
	for (i = 0; i < count; i++) {
		int n = snprintf(canonical + off, sizeof(canonical) - off, "%s@%s\n", tools[i].name,
		                 tools[i].version);

		if (n < 0 || (size_t)n >= sizeof(canonical) - off) {
			snprintf(err, err_size, "declared build tool set is too long to name");
			return -1;
		}
		off += (size_t)n;
	}
	if (image_hash_manifest_string(canonical, hash, sizeof(hash)) != 0) {
		snprintf(err, err_size, "could not hash the declared build tool set");
		return -1;
	}
	hash[16] = '\0';
	snprintf(out_image, out_image_size, "%s%s", PKG_BUILDENV_IMAGE_PREFIX, hash);

	{
		char existing[IMAGE_VERSION_MAX];

		/*
		 * Already composed: the tool set IS the image's name, so an
		 * existing one is by construction the right one -- but only if
		 * it was ever actually filled. image_create() gives a brand-new
		 * image a current version immediately (the hash of its own empty
		 * manifest), so "has a current version" is not the same question
		 * as "has been composed", and treating them as one is how this
		 * shipped broken: a failed composition left a created-but-empty
		 * image behind, and every later build silently accepted it as
		 * ready. That is exactly what happened on the first real box --
		 * the environment reported composed, then the build died at
		 * execve(/usr/bin/bash) because there was nothing in it at all.
		 *
		 * The empty-manifest hash is the marker for that state, and
		 * skipping it means an image poisoned by an earlier daemon
		 * heals itself on the next build rather than needing a manual
		 * delete.
		 */
		char empty[IMAGE_VERSION_MAX];

		if (image_empty_manifest_version(empty, sizeof(empty)) != 0) {
			snprintf(err, err_size, "could not determine the empty-image version");
			return -1;
		}
		if (image_current_version(out_image, existing, sizeof(existing)) == IMAGE_OK &&
		    existing[0] != '\0' && strcmp(existing, empty) != 0) {
			buildenv_touch(out_image); /* ADR-0221: reuse is a use */
			return 0;
		}
	}
	{
		struct buildenv_ctx ctx;
		pid_t pid;
		int pidfd;

		ctx.tools = tools;
		ctx.tool_count = count;

		logstore_write("cixd", "info",
		               "pkg: composing build environment %s from %d declared tool(s)", out_image,
		               count);

		pid = fork();
		if (pid < 0) {
			snprintf(err, err_size, "could not fork to compose a build environment: %s",
			         strerror(errno));
			return -1;
		}
		if (pid == 0) {
			int fd;

			/*
			 * Every descriptor this daemon holds was inherited by the
			 * fork, and unlike the fetch child this one never execve()s,
			 * so SOCK_CLOEXEC does nothing for it. Leaving them open
			 * would keep live client sockets from reaching EOF for as
			 * long as composition runs -- reintroducing #237's symptom
			 * from the other direction, through the very change meant to
			 * stop blocking clients. Closed bluntly from stderr up: this
			 * child needs the filesystem and nothing else.
			 */
			for (fd = 3; fd < 4096; fd++)
				close(fd);
			_exit(image_produce_new_version(out_image, buildenv_mutate, &ctx, canonical) == 0 ? 0
			                                                                                  : 1);
		}

		pidfd = sys_pidfd_open(pid, 0);
		if (pidfd < 0) {
			snprintf(err, err_size, "could not track the build-environment composer: %s",
			         strerror(errno));
			kill(pid, SIGKILL);
			waitpid(pid, NULL, 0);
			return -1;
		}
		*out_pid = pid;
		*out_pidfd = pidfd;
		return 1;
	}
}

/*
 * Called once a composer child forked above has exited. Success needs
 * no work -- the caller simply asks buildenv_image_for() again and gets
 * the fast path. Failure does: image_produce_new_version() creates the
 * image before it can fail, and a created-but-empty one is
 * indistinguishable from a real environment by name alone, so it is
 * removed here. Removing it is also what stops a failed composition
 * from being retried forever: the "already composed" check deliberately
 * skips an empty image, so a leftover would be re-forked on every
 * attempt.
 */
static void buildenv_compose_failed_cleanup(const char *env_image)
{
	image_delete(env_image);
}

static int pkg_build_sandbox_rootfs(char *out, size_t out_size)
{
	char version[IMAGE_VERSION_MAX];

	/* An un-migrated flat sandbox wins: it is what builds have always
	 * run against, and the image's own content is not equivalent. */
	if (g_flat_sandbox_fallback[0] != '\0') {
		snprintf(out, out_size, "%s", g_flat_sandbox_fallback);
		return 0;
	}
	if (image_current_version(PKG_BUILD_SANDBOX_IMAGE, version, sizeof(version)) != IMAGE_OK)
		return -1;
	image_version_rootfs_path(PKG_BUILD_SANDBOX_IMAGE, version, out, out_size);
	return 0;
}

int pkg_toolchain_has_gcc(void)
{
	char rootfs[PATH_MAX];
	char gcc_path[PATH_MAX];
	struct stat st;

	if (pkg_build_sandbox_rootfs(rootfs, sizeof(rootfs)) != 0)
		return 0;
	snprintf(gcc_path, sizeof(gcc_path), "%s/usr/bin/gcc", rootfs);
	return stat(gcc_path, &st) == 0;
}

enum pkg_error pkg_seed_image_baseline(const char *rootfs_path)
{
	/*
	 * Source paths deliberately have no "/usr" prefix -- they must match
	 * exactly where mkbootroot.c's test_image_fixture_build()/
	 * test_image_fixture_add_lib() calls actually write these files on
	 * the installed control-plane root (lib64/..., lib/x86_64-linux-gnu/...,
	 * no /usr/lib/... form ever exists there). The previous /usr-prefixed
	 * paths only ever resolved by accident on a rich dev sandbox (merged-
	 * /usr symlinks make /lib64/... and /usr/lib/.../... the same file
	 * there) -- confirmed directly they silently never matched anything
	 * on a real install, so image create()'s own runtime seeding
	 * (ADR-0023) was a no-op there: "not present on this host -- skip,
	 * not fatal" swallowed the failure, leaving every freshly created
	 * image with no ld.so/libc.so.6 at all. This form is correct on both:
	 * a real install has these files ONLY at this path; this dev
	 * sandbox's own /lib64 -> /usr/lib symlink chain (confirmed via
	 * readlink -f) resolves it to the identical real file either way.
	 */
	/*
	 * THE GLIBC FLOOR IS CLOSED (#186).
	 *
	 * Four files used to be copied here straight off the build host --
	 * the dynamic loader, libc, libm and the files-NSS backend --
	 * because nothing in this project had ever built glibc, and an
	 * image with no C library cannot run anything at all. ADR-0209
	 * named them plainly as "the only files in any image that this
	 * project did not build", and said closing it meant building glibc
	 * from source on a Cix host.
	 *
	 * That is done. `glibc` is an ordinary recipe now, built on a Cix
	 * host with this platform's own gcc against its own kernel headers,
	 * and its package carries all four of those paths -- both spellings
	 * of the loader included. So an image gets its C library the same
	 * way it gets everything else: by installing a package, recorded in
	 * its manifest, with a version that can be upgraded and an origin
	 * that can be audited. Every composed build environment gets one
	 * implicitly (PKG_BASE_LIBC, see buildenv_resolve_tools()), and an
	 * image that runs containers declares one like any other package.
	 *
	 * A freshly created image therefore has NO runtime, and that is
	 * correct rather than a gap: POST /v1/containers refuses an image
	 * with no loader and names what to install, instead of letting it
	 * surface later as a container exiting 127.
	 *
	 * Nothing in any image is copied off the build host any more. What
	 * this function still does below -- device nodes, directories, a
	 * written nsswitch.conf -- it CREATES; it does not borrow.
	 */
	/*
	 * ADR-0041: the same real, generic gap Phase 23 (iptables' own
	 * /run/xtables.lock) and Phase 24 (bird hard-crashing with no
	 * /dev/null at all) both hit -- no image this platform builds ever
	 * shipped a baseline FHS layout beyond what pkg install itself
	 * produces, and both were fixed by hand, directly on the router
	 * image's own on-disk files, not reproducible from a fresh install.
	 * Standard char device nodes, same table/pattern
	 * test_image_fixture_stage_toolchain()'s own dev_nodes[] (test/
	 * test_image_fixture.c) already proves safe in this exact sandbox
	 * (its own ancestor cgroup's BPF_CGROUP_DEVICE policy permits
	 * exactly this set).
	 */
	static const struct {
		const char *name;
		unsigned int major, minor;
	} dev_nodes[] = {
		{ "null", 1, 3 }, { "zero", 1, 5 }, { "full", 1, 7 }, { "random", 1, 8 },
		{ "urandom", 1, 9 },
	};
	const char *target_rootfs = rootfs_path;
	size_t i;


	/*
	 * Dev nodes and /run have no "host source" that might legitimately
	 * be absent the way a runtime lib does -- a real mknod()/mkdir_p()
	 * failure here is a genuine I/O or permission problem, not a
	 * tolerable gap, so unlike the loop above this is fatal (matching
	 * that loop's own fatal handling of a real copy failure, not its
	 * tolerant handling of a missing source). Only EEXIST on mknod is
	 * tolerated, for idempotent re-runs.
	 */
	{
		char dev_dir[PATH_MAX];

		snprintf(dev_dir, sizeof(dev_dir), "%s/dev", target_rootfs);
		if (persist_mkdir_p(dev_dir) != 0)
			return PKG_ERR_PERSIST_FAILED;
		for (i = 0; i < sizeof(dev_nodes) / sizeof(dev_nodes[0]); i++) {
			char path[PATH_MAX];

			snprintf(path, sizeof(path), "%s/%s", dev_dir, dev_nodes[i].name);
			if (mknod(path, S_IFCHR | 0666, makedev(dev_nodes[i].major, dev_nodes[i].minor)) !=
			        0 &&
			    errno != EEXIST)
				return PKG_ERR_PERSIST_FAILED;
			/*
			 * mknod()'s own requested mode is subject to the calling
			 * process's umask like any other file-creation call (POSIX;
			 * confirmed live -- a real /dev/null created this way ended
			 * up 0644, not the 0666 requested here, since nothing in
			 * this daemon ever calls umask(0)). A standard device node
			 * MUST stay world-writable regardless of whatever umask
			 * this daemon process happens to inherit at startup -- a
			 * non-root process inside a container writing to /dev/null
			 * (e.g. redirecting a subprocess's own stderr, ADR-0144
			 * task #838's own AuthorizedKeysCommand script) is a
			 * completely ordinary, expected operation, not something
			 * that should ever depend on this daemon's own environment.
			 * chmod() explicitly here, unconditionally (even on the
			 * EEXIST/idempotent-rerun path above, to also correct any
			 * node an earlier, umask-affected run already created
			 * wrong) -- deliberately not a process-wide umask(0) call,
			 * which would affect every other file this daemon creates
			 * too, not just these five nodes.
			 */
			if (chmod(path, 0666) != 0)
				return PKG_ERR_PERSIST_FAILED;
		}

		/*
		 * /dev/ptmx as a symlink to pts/ptmx, the real devpts-provided
		 * multiplexor device -- a static mknod()'d node the way the
		 * five above are can't work here: this project's containers
		 * use a plain overlay /dev (no devtmpfs), and pty allocation
		 * needs a genuine devpts filesystem actually mounted, which
		 * only happens fresh on each container start (mountns_pivot(),
		 * src/mountns.c) -- this symlink is the static, one-time part
		 * of that fix; the mount itself can't be seeded here since
		 * mount points don't persist in image content. Same convention
		 * every real distro/container runtime uses (glibc's own
		 * posix_openpt() opens literally "/dev/ptmx"), not invented
		 * here. See mountns_pivot()'s own doc comment for the full
		 * root cause this closes (found investigating a real "PTY
		 * allocation request failed" during an interactive SSH
		 * session).
		 */
		{
			char ptmx_path[PATH_MAX];

			snprintf(ptmx_path, sizeof(ptmx_path), "%s/ptmx", dev_dir);
			if (symlink("pts/ptmx", ptmx_path) != 0 && errno != EEXIST)
				return PKG_ERR_PERSIST_FAILED;
		}
	}

	{
		char run_dir[PATH_MAX];

		snprintf(run_dir, sizeof(run_dir), "%s/run", target_rootfs);
		if (persist_mkdir_p(run_dir) != 0)
			return PKG_ERR_PERSIST_FAILED;
	}

	/*
	 * ADR-0051: stage the platform's own CA trust chain into this
	 * image, so a TLS client running inside any container built on it
	 * (curl, openssl, ...) can verify a Cix-issued cert without
	 * -k/--insecure. No image built by this platform has ever shipped
	 * ANY CA trust (not even public roots) -- a real, closeable gap,
	 * not something this seeding step is regressing. Idempotent (skip
	 * if already staged, same as the runtime-libs loop above) and
	 * tolerant of no CA existing yet (PKI_ERR_NOT_BOOTSTRAPPED is the
	 * common state on a fresh install's very first image) -- only a
	 * real write failure is fatal, matching the dev/run block's own
	 * "real I/O problem, not a tolerable gap" stance.
	 */
	{
		char bundle_dst[PATH_MAX];
		struct stat dst_st;
		enum pki_error perr;

		snprintf(bundle_dst, sizeof(bundle_dst), "%s" PKG_IMAGE_CA_BUNDLE_PATH,
		         target_rootfs);
		if (stat(bundle_dst, &dst_st) != 0) {
			char bundle_dir[PATH_MAX];

			snprintf(bundle_dir, sizeof(bundle_dir), "%s/etc/ssl/certs", target_rootfs);
			if (persist_mkdir_p(bundle_dir) != 0)
				return PKG_ERR_PERSIST_FAILED;

			perr = pki_write_trust_bundle_file(bundle_dst);
			if (perr != PKI_OK && perr != PKI_ERR_NOT_BOOTSTRAPPED)
				return PKG_ERR_PERSIST_FAILED;
		}
	}

	/*
	 * A real /etc/nsswitch.conf, so glibc consults the NSS backends it
	 * has rather than whatever its compiled-in default names -- that
	 * default is a moving target across glibc versions and this
	 * project has no reason to depend on it being correct.
	 *
	 * The backends come from the glibc PACKAGE, not from here:
	 * measured 2026-09-17, glibc 2.44-16 in jumpbox ships 13 libnss
	 * files including libnss_files.so.2 and libnss_dns.so.2. (ADR-0111
	 * once staged libnss_files.so.2 from this function; nothing in the
	 * tree does now -- `grep -rn libnss` over the C sources finds only
	 * these comments. So every image with glibc already has the dns
	 * backend, and #478 was purely that the file never named it.)
	 *
	 * Content comes from nsswitch.c, which is also where the
	 * ldap_client variant comes from, so the two cannot disagree the
	 * way they did in #478.
	 *
	 * WRITTEN WHENEVER THE CONTENT DIFFERS, not merely when the file
	 * is absent, and that is the load-bearing half. ADR-0111 wrote it
	 * only when absent, which was harmless while the content never
	 * changed and became the reason every image already built kept
	 * "hosts: files" -- and would have kept it forever, since nothing
	 * else ever rewrites it. A file whose content the platform
	 * declares is a file the platform has to converge (ADR-0296).
	 *
	 * Reaching an image still depends on ADR-0155: this runs against a
	 * staging rootfs whose new version is discarded if the package
	 * manifest is unchanged, so convergence arrives with the next real
	 * install or rolling rebuild, not with the daemon that carries it.
	 */
	{
		const char *nsswitch_content = nsswitch_baseline_content();
		char nsswitch_dst[PATH_MAX];
		/*
		 * 512 is larger than either declared variant (the longer,
		 * ldap_client, is under 200 bytes), and a file that does not
		 * fit is deliberately rewritten rather than read fully: a
		 * short read fills the buffer, have_len comes back as 512,
		 * and nsswitch_needs_write() sees a length mismatch. That is
		 * the wanted answer -- anything that is not byte-identical to
		 * what the platform declares gets replaced (ADR-0296) -- so
		 * this is not a truncation bug to "fix" with a bigger buffer.
		 */
		char have[512];
		size_t have_len = 0;
		FILE *nf;

		snprintf(nsswitch_dst, sizeof(nsswitch_dst), "%s/etc/nsswitch.conf", target_rootfs);
		nf = fopen(nsswitch_dst, "rb");
		if (nf != NULL) {
			have_len = fread(have, 1, sizeof(have), nf);
			fclose(nf);
		}
		if (nsswitch_needs_write(nf != NULL ? have : NULL, have_len, nsswitch_content)) {
			char etc_dir[PATH_MAX];

			snprintf(etc_dir, sizeof(etc_dir), "%s/etc", target_rootfs);
			if (persist_mkdir_p(etc_dir) != 0)
				return PKG_ERR_PERSIST_FAILED;
			if (persist_atomic_write(nsswitch_dst, nsswitch_content,
			                          strlen(nsswitch_content)) != 0)
				return PKG_ERR_PERSIST_FAILED;
		}
	}

	/*
	 * /etc/os-release, so a container can say what platform it is on.
	 *
	 * Without it every reader falls back to "linux" -- the freedesktop
	 * spec's own documented default when ID is absent -- so a Cix
	 * container reported itself as generic Linux to anything that
	 * asked. Found while packaging fastfetch, which reads exactly this
	 * file and had no way to identify the platform it was running on.
	 *
	 * The content comes from osrelease_render() rather than a literal
	 * here, because mkbootroot stages the same file into the
	 * control-plane root and two copies would drift -- leaving a host
	 * and the containers running on it disagreeing about what they
	 * are.
	 *
	 * BUILD_ID is the DAEMON's build, which is the honest answer: an
	 * image has no version of its own that means anything to a reader
	 * (ADR-0155's image version is a hash of a package manifest), and
	 * what actually produced this rootfs is this daemon. Same
	 * already-present check as nsswitch above: a package that ships its
	 * own os-release wins, and re-seeding never overwrites it.
	 */
	{
		char osr_dst[PATH_MAX];
		struct stat dst_st;

		snprintf(osr_dst, sizeof(osr_dst), "%s/etc/os-release", target_rootfs);
		if (stat(osr_dst, &dst_st) != 0) {
			char osr[OSRELEASE_MAX];
			char etc_dir[PATH_MAX];

			if (osrelease_render(osr, sizeof(osr), CIX_BUILD_VERSION) != 0)
				return PKG_ERR_PERSIST_FAILED;
			snprintf(etc_dir, sizeof(etc_dir), "%s/etc", target_rootfs);
			if (persist_mkdir_p(etc_dir) != 0)
				return PKG_ERR_PERSIST_FAILED;
			if (persist_atomic_write(osr_dst, osr, strlen(osr)) != 0)
				return PKG_ERR_PERSIST_FAILED;
		}
	}

	return PKG_OK;
}

void pkg_write_json_recipes(struct json_writer *w)
{
	DIR *names;
	struct dirent *nde;

	jw_arr_open(w);
	names = opendir(g_recipes_dir);
	if (names != NULL) {
		while ((nde = readdir(names)) != NULL) {
			char name_dir[PATH_MAX];
			DIR *versions;
			struct dirent *vde;

			if (nde->d_name[0] == '.')
				continue;
			snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, nde->d_name);
			versions = opendir(name_dir);
			if (versions == NULL)
				continue;
			while ((vde = readdir(versions)) != NULL) {
				char path[PATH_MAX];
				char version_dir[PATH_MAX];
				struct pkg_recipe r;

				if (vde->d_name[0] == '.')
					continue;
				snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, vde->d_name);
				if (pkg_recipe_file_in(version_dir, path, sizeof(path), NULL, NULL) != 0)
					continue;
				if (parse_recipe(path, &r) != 0)
					continue;
				jw_obj_open(w);
				jw_key(w, "name");
				jw_str(w, r.name);
				jw_key(w, "version");
				jw_str(w, r.version);
				/* ADR-0305: which language this version is written in. Not
				 * something a recipe declares -- it IS the filename, so it
				 * cannot disagree with what will actually run. */
				jw_key(w, "format");
				jw_str(w, recipe_path_is_pbs(path) ? "pbs" : "shell");
				jw_key(w, "depends");
				jw_str(w, r.depends);
				jw_key(w, "changelog");
				if (r.changelog[0] != '\0')
					jw_str(w, r.changelog);
				else
					jw_null(w);
				jw_key(w, "created_at");
				jw_int(w, recipe_created_at(path));
				jw_obj_close(w);
			}
			closedir(versions);
		}
		closedir(names);
	}
	jw_arr_close(w);
}

enum pkg_error pkg_recipe_get(const char *name, const char *version, struct json_writer *w)
{
	char recipe_path[PATH_MAX];
	struct pkg_recipe r;
	char *content;
	size_t content_len;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;

	if (find_recipe_path(name, version, recipe_path, sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &r) != 0 || strcmp(r.name, name) != 0)
		return PKG_ERR_NOT_FOUND;
	if (persist_read_file(recipe_path, &content, &content_len) != 0 || content == NULL)
		return PKG_ERR_PERSIST_FAILED;
	/* #405: this endpoint needs no authentication and serves the
	 * recipe's stored bytes verbatim, so a recipe written before #60
	 * put a live token here. Redacted on the way out, which is what
	 * covers the ones already on disk. */
	redact_repo_token(content, content_len + 1);

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, r.name);
	jw_key(w, "version");
	jw_str(w, r.version);
	jw_key(w, "depends");
	jw_str(w, r.depends);
	jw_key(w, "changelog");
	if (r.changelog[0] != '\0')
		jw_str(w, r.changelog);
	else
		jw_null(w);
	jw_key(w, "created_at");
	jw_int(w, recipe_created_at(recipe_path));
	jw_key(w, "content");
	jw_str(w, content);
	jw_obj_close(w);
	free(content);
	return PKG_OK;
}

static void rebuild_queue_enqueue(const char *image)
{
	int i;

	for (i = 0; i < g_rebuild_queue_count; i++) {
		if (strcmp(g_rebuild_queue[i], image) == 0)
			return; /* already queued */
	}
	if (g_rebuild_queue_count >= PKG_REBUILD_QUEUE_MAX)
		return; /* best-effort, matches this queue's own documented bound */
	snprintf(g_rebuild_queue[g_rebuild_queue_count], PKG_IMAGE_NAME_MAX, "%s", image);
	g_rebuild_queue_count++;
}

/*
 * Drops one image from the rebuild queue, wherever it sits (#243).
 *
 * Distinct from pop_front(): that one means "this image is caught up",
 * this one means "this image cannot be caught up right now".
 */
static void rebuild_queue_remove(const char *image)
{
	int i;

	for (i = 0; i < g_rebuild_queue_count; i++) {
		if (strcmp(g_rebuild_queue[i], image) != 0)
			continue;
		logstore_write("cixd", "info",
		               "pkg: dropped image %s from the rebuild queue -- a package in it failed, "
		               "so retrying now would only repeat it; publish again to re-queue",
		               image);
		rebuild_queue_remove_at(i);
		return;
	}
}

/*
 * ADR-0273: removal by INDEX, because a gate means the drain no longer
 * always acts on the front. Everything that used to pop the front now
 * goes through here, so there is one implementation of "take this image
 * out of the queue" rather than two that can drift.
 */
static void rebuild_queue_remove_at(int idx)
{
	int i;

	if (idx < 0 || idx >= g_rebuild_queue_count)
		return;
	approval_forget(PKG_GATE_ROLL, g_rebuild_queue[idx]);
	for (i = idx + 1; i < g_rebuild_queue_count; i++)
		snprintf(g_rebuild_queue[i - 1], PKG_IMAGE_NAME_MAX, "%s", g_rebuild_queue[i]);
	g_rebuild_queue_count--;
}

static void rebuild_queue_pop_front(void)
{
	rebuild_queue_remove_at(0);
}

/* An image was deleted -- see pkg.h. Silent, unlike
 * rebuild_queue_remove(): nothing failed here, the target simply went
 * away, and rebuild_queue_remove_at() forgets its approval on the way
 * out. Safe to call for an image that was never queued. */
void pkg_image_forgotten(const char *image)
{
	int i;

	if (image == NULL || image[0] == '\0')
		return;
	for (i = 0; i < g_rebuild_queue_count; i++) {
		if (strcmp(g_rebuild_queue[i], image) == 0) {
			rebuild_queue_remove_at(i);
			return;
		}
	}
	approval_forget(PKG_GATE_ROLL, image);
}

/*
 * Rebuild the queue from what is actually behind, at startup (#373).
 *
 * The queue was in-memory only and nothing wrote it down, so a daemon
 * restart between a publish and the drain lost every queued rebuild
 * with no record that they had been queued at all. The images then sat
 * behind their manifests indefinitely, and the only thing that would
 * ever queue them again was another publish of the same package.
 *
 * Deliberately DERIVED rather than persisted, which is what #373 itself
 * argued for and is the better answer: an image that is behind is
 * discoverable, so the queue is recoverable state, not durable state.
 * Persisting it would have written down an intent that can be computed,
 * added a state file to keep in step with the images it describes, and
 * still have lost a queue to a crash rather than a clean restart. This
 * repairs both, and repairs a queue that was lost before this shipped.
 *
 * The "is it behind" test is pkg_entry_drift(), the same primitive
 * GET /pkg/drift and the available-version column already use -- ADR-0031
 * extracted it precisely so a third caller would not carry its own copy,
 * which is exactly what this would otherwise have become.
 *
 * A pinned manifest entry is never queued: pinning means "never move",
 * so being behind the latest recipe is the intended state, not drift to
 * repair. Same rule queue_rolling_rebuilds_for() applies on publish.
 */
void pkg_rebuild_queue_rederive(void)
{
	char image_names[IMAGE_LIST_MAX][PKG_IMAGE_NAME_MAX];
	int image_count = image_list_names(image_names, IMAGE_LIST_MAX);
	int queued_count = 0;
	int i;

	for (i = 0; i < image_count; i++) {
		struct image_manifest_entry entries[IMAGE_MANIFEST_MAX_PACKAGES];
		int entry_count, j;

		if (image_manifest_read(image_names[i], entries, &entry_count,
		                         IMAGE_MANIFEST_MAX_PACKAGES) != IMAGE_OK)
			continue;
		for (j = 0; j < entry_count; j++) {
			const struct pkg_entry *e;

			if (entries[j].mode != IMAGE_PKG_ROLLING)
				continue;
			e = pkg_find(entries[j].package, image_names[i]);
			if (e == NULL || !pkg_entry_drift(e, NULL, 0))
				continue;
			rebuild_queue_enqueue(image_names[i]);
			queued_count++;
			break;
		}
	}
	if (queued_count > 0)
		logstore_write("cixd", "info",
		                "pkg: re-derived %d image(s) behind a rolling package into the rebuild "
		                "queue at startup (#373)",
		                queued_count);
}

/*
 * ADR-0107: the moment pkg_name@pkg_version is published, every image
 * whose own manifest tracks pkg_name as "rolling" with a floor at or
 * below pkg_version needs a rebuild to actually pick it up -- queued
 * here, drained later by pkg_try_start_queued_rebuild() (this daemon's
 * own single-job-in-flight constraint means it usually can't start
 * immediately). A pinned entry never triggers this -- pinning means
 * "never move," by definition unaffected by a newer version existing.
 */
static void queue_rolling_rebuilds_for(const char *pkg_name, const char *pkg_version)
{
	char image_names[IMAGE_LIST_MAX][PKG_IMAGE_NAME_MAX];
	int image_count = image_list_names(image_names, IMAGE_LIST_MAX);
	char queued[512];
	int queued_len = 0;
	int queued_count = 0;
	int i;

	queued[0] = '\0';

	for (i = 0; i < image_count; i++) {
		struct image_manifest_entry entries[IMAGE_MANIFEST_MAX_PACKAGES];
		int entry_count, j;

		if (image_manifest_read(image_names[i], entries, &entry_count,
		                         IMAGE_MANIFEST_MAX_PACKAGES) != IMAGE_OK)
			continue;
		for (j = 0; j < entry_count; j++) {
			if (entries[j].mode == IMAGE_PKG_ROLLING &&
			    strcmp(entries[j].package, pkg_name) == 0 &&
			    pkg_version_compare(pkg_version, entries[j].version) >= 0) {
				rebuild_queue_enqueue(image_names[i]);
				if (queued_len < (int)sizeof(queued) - 1)
					queued_len += snprintf(queued + queued_len,
					                        sizeof(queued) - (size_t)queued_len, "%s%s",
					                        queued_len > 0 ? ", " : "", image_names[i]);
				queued_count++;
				break;
			}
		}
	}

	/*
	 * SAY SO (#236).
	 *
	 * Publishing a recipe is documented and implemented as a metadata
	 * write, and it also commits this host to rebuilding every image
	 * tracking that package `rolling`. Nothing anywhere reported that:
	 * not the request, not the response, not any endpoint -- so a
	 * publish silently started real work, and a handful in sequence
	 * made the box unusable for reasons nothing named.
	 *
	 * The response still carries no body (it is a 204, and changing
	 * that is a contract change worth making separately), so this line
	 * plus GET /v1/pkg/rebuilds are where the consequence becomes
	 * visible. Silence about work you have just started is the failure
	 * mode; naming it costs one log line.
	 */
	if (queued_count > 0)
		logstore_write("cixd", "info",
		               "pkg: publishing %s@%s queued a rebuild of %d image(s): %s", pkg_name,
		               pkg_version, queued_count, queued);
}

/*
 * How many images are waiting for a rolling rebuild (#236).
 *
 * Exists so the event loop can ask "is there anything to do" without
 * paying for the answer. pkg_try_start_queued_rebuild() below is
 * expensive when it has work -- it can start a real install, which
 * composes a build environment -- so calling it speculatively every
 * pass would be far worse than the problem it solves.
 */
int pkg_rebuild_queue_depth(void)
{
	return g_rebuild_queue_count;
}

/*
 * ADR-0270: put an image on the same queue a rolling recipe publish
 * uses, from outside pkg.c.
 *
 * The deployment auto-fork needs exactly what queue_rolling_rebuilds_
 * for() already does per image -- "this image's manifest wants
 * realizing, converge it when a slot frees" -- and building a second
 * path to the same queue would be two mechanisms for one thing. Idempotent
 * (rebuild_queue_enqueue() ignores an image already queued), so a repeated
 * apply of a still-waiting deployment costs nothing.
 */
void pkg_rebuild_queue_add(const char *image)
{
	if (image == NULL || image[0] == '\0')
		return;
	rebuild_queue_enqueue(image);
}

int pkg_try_start_queued_rebuild(pid_t *out_pid, int *out_pidfd, int *out_chain_idx)
{
	int qi = 0;

	if (pkg_any_job_busy())
		return 0;

	/*
	 * ADR-0273: an INDEX walk, not "always the front".
	 *
	 * A hold implemented as "stop at the front" would let one image
	 * awaiting a person freeze every other image's convergence -- a
	 * gate on one thing becoming an outage for everything. A held entry
	 * is skipped and left in place, costing one string compare per
	 * pass rather than a rebuild attempt.
	 */
	while (qi < g_rebuild_queue_count) {
		const char *image = g_rebuild_queue[qi];
		struct image_manifest_entry entries[IMAGE_MANIFEST_MAX_PACKAGES];
		int entry_count, i;
		int started = 0;

		if (gate_holds(PKG_GATE_ROLL, image)) {
			qi++;
			continue;
		}
		/*
		 * #382: this image is already converging -- leave it queued
		 * and move on, exactly as a held one is.
		 *
		 * Before ADR-0273 the drain popped an image the moment it
		 * started a build for it, so it could not be started twice.
		 * ADR-0273 deliberately keeps it queued until it has fully
		 * converged (an image may need several packages installed in
		 * turn, and spending the approval grant on the first would
		 * strand the rest) -- which removed the only thing that had
		 * been preventing a second start. Nothing else covered it:
		 * pkg_any_job_busy() asks whether ANY chain slot is free, not
		 * whether THIS image is already building, and a
		 * PKG_STATE_BUILDING entry never satisfies the manifest check
		 * below. So every pass reached the same image and started the
		 * same package again. On a real host that put ten copies of
		 * one job into all ten slots, after which no build of
		 * anything could start until cixd restarted.
		 */
		if (image_has_job_in_flight(image)) {
			qi++;
			continue;
		}
		if (image_manifest_read(image, entries, &entry_count, IMAGE_MANIFEST_MAX_PACKAGES) !=
		    IMAGE_OK) {
			rebuild_queue_remove_at(qi);
			continue;
		}

		for (i = 0; i < entry_count; i++) {
			struct pkg_entry *e = pkg_find(entries[i].package, image);
			int satisfied;

			if (entries[i].mode == IMAGE_PKG_PINNED) {
				satisfied = (e != NULL && e->state == PKG_STATE_INSTALLED &&
				             strcmp(e->version, entries[i].version) == 0);
			} else {
				/* rolling: satisfied only if installed at the CURRENT
				 * highest available version -- re-derived fresh every
				 * call rather than trusting any cached "target," which
				 * is exactly what lets this queue naturally settle once
				 * every image has caught up (a later, even newer
				 * version publish just re-queues the image again via
				 * queue_rolling_rebuilds_for()).
				 *
				 * Through recipe_latest_version(), not a raw
				 * find_recipe_path() + parse_recipe() pair (#236). The
				 * answer is identical -- that function IS that pair --
				 * but it is memoised against the package directory's
				 * own mtime and size, so a queue walk that finds
				 * everything already satisfied costs a stat per package
				 * rather than a directory scan and a file parse per
				 * package.
				 *
				 * That difference is the whole bug. This loop runs over
				 * every entry of every queued image, and `toolchain`
				 * alone declares 27 packages rolling, so publishing a
				 * recipe walked hundreds of directory scans and parses
				 * while the event loop waited. "Re-derived fresh every
				 * call" is preserved exactly: the cache is invalidated
				 * by the directory changing, which is precisely when a
				 * new version appears. */
				char latest[PKG_VERSION_MAX];

				satisfied = (e != NULL && e->state == PKG_STATE_INSTALLED &&
				             recipe_latest_version(entries[i].package, latest,
				                                    sizeof(latest)) == 0 &&
				             strcmp(latest, e->version) == 0);
			}

			if (!satisfied) {
				const char *want_version =
				    (entries[i].mode == IMAGE_PKG_PINNED) ? entries[i].version : NULL;
				char started_name[PKG_NAME_MAX];

				/* Manifest-driven, automatic install -- like
				 * handle_pkg_update_all()'s own rebuild, never worth
				 * preserving a build container for (ADR-0175). */
				/* ADR-0272: nobody requested this one -- a publish
				 * caused it. pkg_install_start() consumes this before
				 * its own first early return and puts it on the chain,
				 * so a refused start cannot leave it behind and every
				 * dependency the rebuild pulls carries it too. */
				snprintf(g_next_run_trigger, sizeof(g_next_run_trigger), "%s",
				         PKG_RUN_TRIGGER_ROLLING);
				if (pkg_install_start(entries[i].package, image, want_version, e != NULL, 0,
				                       started_name, sizeof(started_name), out_pid,
				                       out_pidfd, out_chain_idx) == PKG_OK) {
					started = 1;
					break;
				}
				/* Couldn't start this particular entry (recipe removed
				 * out from under the manifest, etc.) -- try the rest of
				 * the manifest rather than getting stuck on one bad
				 * entry. */
			}
		}

		if (started) {
			/*
			 * ADR-0273: deliberately NOT consumed here.
			 *
			 * The image stays queued while its rebuild runs, and an
			 * image may need several packages converged in turn. If
			 * the grant were spent at the start of the first one, the
			 * gate would hold this image again on the very next pass
			 * -- with no grant left, so it would be skipped forever:
			 * converged, never popped, and reported pending for good.
			 * The grant covers converging this image, and is spent
			 * when that is done. (Found by reading this loop, not by
			 * observing it -- it would have shipped looking correct.)
			 */
			return 1;
		}

		/* Every manifest entry already satisfied -- this image is
		 * caught up, drop it and try whatever's queued next. */
		approval_consume(PKG_GATE_ROLL, image);
		rebuild_queue_remove_at(qi);
	}
	return 0;
}

/*
 * True when `updated` is `stored` plus a pkg_artifact_sha256= line, and
 * differs in nothing else.
 *
 * This is the one edit a published recipe version may take, and the
 * asymmetry is the point. A recipe's build instructions must be
 * immutable: they are what a source build follows, and letting them
 * change under a version already installed somewhere is the drift
 * ADR-0107's immutability rule exists to prevent. But the artifact
 * checksum is not an instruction -- it is an APPROVAL of the bytes
 * those instructions produced, and it cannot be known until after the
 * version is built and published, which is strictly later than the
 * recipe has to exist.
 *
 * That circularity had a real cost. `pkg.c`'s artifact tier is entered
 * only when a recipe declares a checksum, so a package built here and
 * pushed to the cache was still rebuilt from source on every other
 * host: the artifact existed, was verified, and was never consulted.
 * Of 37 packages installed on the first real box, 12 sat in the cache
 * unapproved -- including grub, python, openssl and tcc, the expensive
 * ones. "Rebuild the box from the cache" could not work.
 *
 * Only absent -> present is accepted. Replacing an existing checksum
 * would let one version name two different byte sequences, which is
 * exactly what immutability protects against, and is the failure #145
 * documents: an edited checksum that silently stops matching, sending
 * every install down the source path with an error that never mentions
 * checksums.
 */
static int recipe_adds_only_artifact_sha256(const char *stored, const char *updated)
{
	const char *key = "pkg_artifact_sha256=";
	const char *sp = stored;
	const char *up = updated;
	int saw_new_approval = 0;

	for (;;) {
		size_t slen, ulen;
		const char *snl = strchr(sp, '\n');
		const char *unl;

		/* A pkg_artifact_sha256= line in the STORED copy means this
		 * version is already approved -- nothing further may change
		 * it. */
		if (strncmp(sp, key, strlen(key)) == 0)
			return 0;

		unl = strchr(up, '\n');
		if (strncmp(up, key, strlen(key)) == 0) {
			/* The added approval: skip it and keep comparing. */
			if (saw_new_approval)
				return 0; /* more than one -- not a simple addition */
			saw_new_approval = 1;
			up = (unl != NULL) ? unl + 1 : up + strlen(up);
			continue;
		}

		if (*sp == '\0' && *up == '\0')
			return saw_new_approval;
		if (*sp == '\0' || *up == '\0')
			return 0;

		slen = (snl != NULL) ? (size_t)(snl - sp) : strlen(sp);
		ulen = (unl != NULL) ? (size_t)(unl - up) : strlen(up);
		if (slen != ulen || memcmp(sp, up, slen) != 0)
			return 0;

		sp = (snl != NULL) ? snl + 1 : sp + slen;
		up = (unl != NULL) ? unl + 1 : up + ulen;
	}
}

enum pkg_error pkg_recipe_add(const char *name, const char *content,
                               enum pkg_recipe_format format, int *out_was_approval)
{
	char *redacted;
	char name_dir[PATH_MAX];
	char version_dir[PATH_MAX];
	char staging_path[PATH_MAX];
	char recipe_path[PATH_MAX];
	char other_path[PATH_MAX];
	char *explain_json = NULL;
	struct pkg_recipe parsed;
	struct stat st;
	const int is_pbs = format == PKG_RECIPE_PBS;

	if (out_was_approval != NULL)
		*out_was_approval = 0;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;

	if (persist_mkdir_p(g_recipes_dir) != 0)
		return PKG_ERR_PERSIST_FAILED;

	/* Staged under g_recipes_dir itself (not yet inside any
	 * name/version subdirectory -- the version isn't known until the
	 * staged content is parsed below), same ".name.recipe.new"
	 * dotfile-hiding convention this always used.
	 *
	 * A PBS recipe's staging file keeps the .cbs extension, and that
	 * is not cosmetic: has_cbs_extension() in cbs refuses any other
	 * path outright, in `explain` as well as `build`, so a staging
	 * file without it cannot be read at all (ADR-0305). */
	if (snprintf(staging_path, sizeof(staging_path), "%s/.%s.recipe.new%s", g_recipes_dir, name,
	             is_pbs ? PKG_RECIPE_PBS_SUFFIX : "") >= (int)sizeof(staging_path))
		return PKG_ERR_INVALID_NAME;
	/* #405: never store a live repo token. Everything below -- the
	 * write, the immutability check and the approval comparison --
	 * works on the redacted copy, so a caller that submits an expanded
	 * URL gets the same recipe a caller that submits the placeholder
	 * does, rather than a second, secret-bearing version of it. */
	redacted = strdup(content);
	if (redacted == NULL)
		return PKG_ERR_PERSIST_FAILED;
	redact_repo_token(redacted, strlen(redacted) + 1);
	content = redacted;
	if (persist_atomic_write(staging_path, content, strlen(content)) != 0) {
		free(redacted);
		return PKG_ERR_PERSIST_FAILED;
	}

	/*
	 * Identity. This is the one place the two formats genuinely
	 * differ, and the PBS side cannot go through parse_recipe():
	 * that reads the derived explain.json beside a build.cbs, and at
	 * this moment there is no such file -- producing it is what this
	 * branch does.
	 */
	if (is_pbs) {
		struct pbs_explain *ex;
		char err[512];
		size_t json_len = 0;

		explain_json = malloc(PKG_EXPLAIN_MAX);
		if (explain_json == NULL) {
			unlink(staging_path);
			free(redacted);
			return PKG_ERR_PERSIST_FAILED;
		}
		if (run_cbs_explain(staging_path, explain_json, PKG_EXPLAIN_MAX, &json_len, err,
		                     sizeof(err)) != 0) {
			logstore_write("cixd", "error", "pkg: recipe %s rejected: %s", name, err);
			unlink(staging_path);
			free(redacted);
			free(explain_json);
			return PKG_ERR_INVALID_RECIPE;
		}
		ex = pbs_explain_parse(explain_json, json_len, err, sizeof(err));
		if (ex == NULL) {
			logstore_write("cixd", "error", "pkg: recipe %s rejected: %s", name, err);
			unlink(staging_path);
			free(redacted);
			free(explain_json);
			return PKG_ERR_INVALID_RECIPE;
		}
		/*
		 * A declared capability this daemon cannot name is refused
		 * rather than dropped (cix-build-system#162). `cbs explain
		 * --json` reports a COUNT, and a count of 1 does not say
		 * whether the recipe asked for CAP_SYS_ADMIN or
		 * CAP_NET_ADMIN -- so granting by count would be worse than
		 * not supporting the field, and dropping it silently is
		 * exactly the defect ADR-0304 was written about. Refusing at
		 * publish means a recipe whose capability would go missing
		 * never becomes a stored file.
		 */
		if (pbs_explain_capability_count(ex) > 0) {
			logstore_write("cixd", "error",
			               "pkg: recipe %s declares %d build capability/ies, and `cbs "
			               "explain --json` reports only a count, not the names "
			               "(cix-build-system#162) -- refusing rather than building "
			               "without them",
			               name, pbs_explain_capability_count(ex));
			pbs_explain_free(ex);
			unlink(staging_path);
			free(redacted);
			free(explain_json);
			return PKG_ERR_INVALID_RECIPE;
		}
		memset(&parsed, 0, sizeof(parsed));
		snprintf(parsed.name, sizeof(parsed.name), "%s", pbs_explain_name(ex));
		if (pbs_explain_version(ex, parsed.version, sizeof(parsed.version)) != 0) {
			pbs_explain_free(ex);
			unlink(staging_path);
			free(redacted);
			free(explain_json);
			return PKG_ERR_INVALID_RECIPE;
		}
		pbs_explain_free(ex);
		if (strcmp(parsed.name, name) != 0) {
			unlink(staging_path);
			free(redacted);
			free(explain_json);
			return PKG_ERR_INVALID_RECIPE;
		}
	} else if (parse_recipe(staging_path, &parsed) != 0 || strcmp(parsed.name, name) != 0) {
		unlink(staging_path);
		free(redacted);
		return PKG_ERR_INVALID_RECIPE;
	}

	/* Immutability (ADR-0107): an already-published (name,version) is a
	 * real error, never a silent overwrite.
	 *
	 * ADR-0305 adds the other half: a version holds one recipe
	 * language, never two. Publishing a build.cbs over a version that
	 * already has a build.sh (or the reverse) is refused here, with
	 * the same 409 a republish gets, because a version that could be
	 * read two ways has no single answer to what its build will do.
	 * pkg_recipe_file_in() would then refuse to resolve it at all, so the
	 * package would become unbuildable rather than ambiguous -- worth
	 * preventing at the one moment it can be. */
	snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, name);
	snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, parsed.version);
	snprintf(recipe_path, sizeof(recipe_path), "%s/%s", version_dir,
	         is_pbs ? PKG_RECIPE_PBS_FILE : PKG_RECIPE_SHELL_FILE);
	snprintf(other_path, sizeof(other_path), "%s/%s", version_dir,
	         is_pbs ? PKG_RECIPE_SHELL_FILE : PKG_RECIPE_PBS_FILE);
	if (stat(other_path, &st) == 0) {
		logstore_write("cixd", "error",
		               "pkg: recipe %s@%s is already published as %s -- a version holds one "
		               "recipe language, never both (ADR-0305)",
		               name, parsed.version, is_pbs ? "a shell recipe" : "a PBS recipe");
		unlink(staging_path);
		free(redacted);
		free(explain_json);
		return PKG_ERR_DUPLICATE;
	}
	if (stat(recipe_path, &st) == 0) {
		char *stored = NULL;
		size_t stored_len = 0;
		int only_approval = 0;

		/*
		 * The one permitted edit: adding the artifact checksum for
		 * bytes this very version produced. See
		 * recipe_adds_only_artifact_sha256() for why this is not a
		 * hole in immutability but the completion of it.
		 *
		 * It does not apply to a PBS recipe, and not because it was
		 * left out: CPDL 0.1 rejects unknown package keys, so there
		 * is no line in a build.cbs to add (cix-build-system#161).
		 * The consequence is real and deliberately not hidden -- a
		 * PBS recipe cannot take a cache hit and rebuilds from
		 * source on every install, which is why the packages flipped
		 * first are the ones that carry no approval today.
		 */
		if (!is_pbs && persist_read_file(recipe_path, &stored, &stored_len) == 0 &&
		    stored != NULL) {
			only_approval = recipe_adds_only_artifact_sha256(stored, content);
			free(stored);
		}
		free(redacted);
		free(explain_json);
		if (!only_approval) {
			unlink(staging_path);
			return PKG_ERR_DUPLICATE;
		}
		if (rename(staging_path, recipe_path) != 0) {
			unlink(staging_path);
			return PKG_ERR_PERSIST_FAILED;
		}
		logstore_write("cixd", "info",
		                "pkg: recipe %s@%s approved its published artifact", name,
		                parsed.version);
		if (out_was_approval != NULL)
			*out_was_approval = 1; /* #404 */
		return PKG_OK;
	}
	free(redacted);

	if (persist_mkdir_p(version_dir) != 0) {
		unlink(staging_path);
		free(explain_json);
		return PKG_ERR_PERSIST_FAILED;
	}
	/*
	 * The derived identity goes down BEFORE the recipe it describes
	 * (ADR-0305), and the order is the whole of its correctness.
	 *
	 * pkg_recipe_file_in() reports a version as published the moment a
	 * build.cbs exists, and parse_recipe() then reads explain.json
	 * beside it. Renaming the recipe in first would make the version
	 * briefly visible with no identity -- a window in which a
	 * concurrent list or dependency resolution reads a recipe that
	 * parses as nothing. Writing the identity first means the
	 * opposite failure instead: an explain.json with no recipe, which
	 * nothing looks for and the next publish overwrites.
	 */
	if (is_pbs) {
		char explain_path[PATH_MAX];

		pbs_explain_path(recipe_path, explain_path, sizeof(explain_path));
		if (persist_atomic_write(explain_path, explain_json, strlen(explain_json)) != 0) {
			unlink(staging_path);
			free(explain_json);
			return PKG_ERR_PERSIST_FAILED;
		}
	}
	free(explain_json);
	if (rename(staging_path, recipe_path) != 0) {
		unlink(staging_path);
		return PKG_ERR_PERSIST_FAILED;
	}
	queue_rolling_rebuilds_for(parsed.name, parsed.version);
	return PKG_OK;
}

/*
 * Re-derives the explain.json beside an already-written build.cbs
 * (ADR-0305).
 *
 * Exists for one caller: a system restore. A PBS recipe's identity is
 * DERIVED state and is deliberately absent from a backup -- it is
 * what one particular cbs made of the document, and restoring a copy
 * taken months ago would resurrect that reading instead of the
 * current one. Re-deriving also proves the restored recipe is still
 * readable by the engine this host actually has, which a copied file
 * would have hidden until the first build.
 *
 * Does NOT re-validate against a package name or refuse a declared
 * capability the way publishing does: this document was already
 * accepted by a publish on the host the backup came from, and a
 * restore's job is to put back what was there rather than to re-open
 * decisions. A document the current cbs cannot read at all still
 * fails, which is the case worth catching.
 */
enum pkg_error pkg_recipe_rederive_identity(const char *recipe_path)
{
	char explain_path[PATH_MAX];
	char err[512];
	char *json;
	size_t json_len = 0;
	enum pkg_error rc = PKG_OK;

	if (recipe_path == NULL || !recipe_path_is_pbs(recipe_path))
		return PKG_ERR_INVALID_RECIPE;

	json = malloc(PKG_EXPLAIN_MAX);
	if (json == NULL)
		return PKG_ERR_PERSIST_FAILED;
	if (run_cbs_explain(recipe_path, json, PKG_EXPLAIN_MAX, &json_len, err, sizeof(err)) != 0) {
		logstore_write("cixd", "error", "pkg: could not re-derive identity for %s: %s",
		               recipe_path, err);
		free(json);
		return PKG_ERR_INVALID_RECIPE;
	}
	pbs_explain_path(recipe_path, explain_path, sizeof(explain_path));
	if (persist_atomic_write(explain_path, json, strlen(json)) != 0)
		rc = PKG_ERR_PERSIST_FAILED;
	free(json);
	return rc;
}

enum pkg_error pkg_recipe_delete(const char *name, const char *version)
{
	char name_dir[PATH_MAX];

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;

	snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, name);

	if (version != NULL && version[0] != '\0') {
		char version_dir[PATH_MAX];
		char recipe_path[PATH_MAX];

		snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, version);
		if (pkg_recipe_file_in(version_dir, recipe_path, sizeof(recipe_path), NULL, NULL) != 0)
			return PKG_ERR_NOT_FOUND;
		/* The derived identity goes with the recipe it describes
		 * (ADR-0305). Left behind it would make the next publish of
		 * this same version inherit a stale explain.json for a few
		 * instructions -- and, worse, rmdir() below would fail on a
		 * non-empty directory, so the delete would report success
		 * having removed nothing that mattered. Unconditional: for a
		 * shell recipe there is no such file and unlink() harmlessly
		 * fails. */
		{
			char explain_path[PATH_MAX];

			pbs_explain_path(recipe_path, explain_path, sizeof(explain_path));
			unlink(explain_path);
		}
		if (unlink(recipe_path) != 0 || rmdir(version_dir) != 0)
			return PKG_ERR_PERSIST_FAILED;
		/* Leave name_dir itself if other versions remain -- rmdir()
		 * on a non-empty directory harmlessly fails and is ignored. */
		rmdir(name_dir);
		return PKG_OK;
	}

	/* No version given -- remove every published version of name. */
	{
		DIR *d = opendir(name_dir);
		struct dirent *de;
		int found = 0;

		if (d == NULL)
			return PKG_ERR_NOT_FOUND;
		while ((de = readdir(d)) != NULL) {
			char version_dir[PATH_MAX];
			char recipe_path[PATH_MAX];

			if (de->d_name[0] == '.')
				continue;
			found = 1;
			snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, de->d_name);
			if (pkg_recipe_file_in(version_dir, recipe_path, sizeof(recipe_path), NULL, NULL) == 0) {
				char explain_path[PATH_MAX];

				pbs_explain_path(recipe_path, explain_path, sizeof(explain_path));
				unlink(explain_path);
				unlink(recipe_path);
			}
			rmdir(version_dir);
		}
		closedir(d);
		if (!found)
			return PKG_ERR_NOT_FOUND;
		if (rmdir(name_dir) != 0)
			return PKG_ERR_PERSIST_FAILED;
		return PKG_OK;
	}
}

/*
 * Recursive DFS, post-order: name's own pkg_depends (each recursed
 * into first) land in queue before name itself. Already-INSTALLED
 * packages are skipped as already-satisfied dependencies -- UNLESS
 * force is set, which only the top-level pkg_install_start() call
 * uses, so an explicit-upgrade target still gets queued even though
 * it's already installed. Cycle detection via "visiting" (the current
 * DFS path); a name reappearing there is a circular dependency, not a
 * legitimate diamond (diamonds are fine and already deduplicated by
 * the "already in queue" check below, independent of the cycle check).
 * All local file I/O -- no async need, every recipe involved is
 * already on disk.
 *
 * version (ADR-0107) is only ever meaningful for the top-level call
 * (pkg_install_start()'s own requested pin) -- name's OWN recipe is
 * looked up at that exact version so its depends= reflects what that
 * specific version actually declared. Every recursive call for a
 * dependency token always passes NULL: a dependency resolves to its
 * own highest available version regardless of what the top-level
 * target is pinned to (pinning applies only to the single package
 * actually being installed, never transitively).
 */
static int resolve_chain(const char *name, const char *version, const char *image, int force,
                          char queue[][PKG_NAME_MAX], int *count, char visiting[][PKG_NAME_MAX],
                          int *visiting_count, char *err_out, size_t err_out_size)
{
	struct pkg_entry *existing;
	struct pkg_recipe recipe;
	char recipe_path[PATH_MAX];
	char deps_copy[PKG_DEPENDS_MAX];
	char *tok, *save = NULL;
	int i;

	if (!pkg_name_is_valid(name)) {
		snprintf(err_out, err_out_size, "invalid dependency name '%s'", name);
		return -1;
	}

	existing = pkg_find(name, image);
	if (!force && existing != NULL && existing->state == PKG_STATE_INSTALLED)
		return 0; /* already satisfied */

	for (i = 0; i < *count; i++) {
		if (strcmp(queue[i], name) == 0)
			return 0; /* already queued via another branch (a diamond, not a cycle) */
	}

	for (i = 0; i < *visiting_count; i++) {
		if (strcmp(visiting[i], name) == 0) {
			snprintf(err_out, err_out_size, "circular dependency involving '%s'", name);
			return -1;
		}
	}

	if (find_recipe_path(name, version, recipe_path, sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0) {
		snprintf(err_out, err_out_size, "unknown dependency '%s' (no recipe)", name);
		return -1;
	}

	if (*visiting_count >= PKG_MAX_DEP_CHAIN) {
		snprintf(err_out, err_out_size, "dependency chain too deep");
		return -1;
	}
	strncpy(visiting[*visiting_count], name, PKG_NAME_MAX - 1);
	visiting[*visiting_count][PKG_NAME_MAX - 1] = '\0';
	(*visiting_count)++;

	snprintf(deps_copy, sizeof(deps_copy), "%s", recipe.depends);
	tok = strtok_r(deps_copy, " \t", &save);
	while (tok != NULL) {
		if (resolve_chain(tok, NULL, image, 0, queue, count, visiting, visiting_count, err_out,
		                   err_out_size) != 0)
			return -1;
		tok = strtok_r(NULL, " \t", &save);
	}

	(*visiting_count)--;

	if (*count >= PKG_MAX_DEP_CHAIN) {
		snprintf(err_out, err_out_size, "dependency chain too large");
		return -1;
	}
	strncpy(queue[*count], name, PKG_NAME_MAX - 1);
	queue[*count][PKG_NAME_MAX - 1] = '\0';
	(*count)++;
	return 0;
}

/*
 * True exactly while the entry currently being fetched/built is the
 * single top-level package the in-flight job actually asked for --
 * g_chains[chain_idx].dep_queue's own post-order construction (resolve_chain()) always
 * places that entry last, and it's the only entry a version pin
 * (g_chains[chain_idx].target_version) ever applies to (ADR-0107).
 */
static int current_fetch_is_target(int chain_idx)
{
	return g_chains[chain_idx].dep_queue_pos == g_chains[chain_idx].dep_queue_count - 1;
}

/*
 * The version to resolve the currently-fetching/-building entry's own
 * recipe at: the job's requested pin, but ONLY for the top-level
 * target entry -- every dependency always resolves to its own highest
 * available version regardless of what the target is pinned to.
 * Returns NULL for "resolve to highest available", the pre-existing
 * behavior for every case that isn't an explicit top-level pin.
 */
static const char *current_fetch_effective_version(int chain_idx)
{
	if (current_fetch_is_target(chain_idx) && g_chains[chain_idx].target_version[0] != '\0')
		return g_chains[chain_idx].target_version;
	return NULL;
}

/* ADR-0122: forward declarations -- defined below (near the end of this
 * file, alongside the rest of the local-cache/artifact-server module)
 * but needed here since start_fetch_for()/pkg_fetch_completed()/
 * pkg_build_completed() are the only call sites and all come first. */
static int pkg_cache_has(const char *name, const char *version);
static void pkg_cache_touch(const char *name, const char *version);
static void pkg_cache_save(const char *name, const char *version, const char *dest_dir);
static void pkg_build_log_dir(char *out, size_t out_size); /* issue #57 */
static void pkg_build_log_open(struct pkg_entry *e, const char *version); /* issue #57 */
static void pkg_build_log_close(struct pkg_entry *e);
static void pkg_cache_save_from_file(const char *name, const char *version, const char *src_path);
static int pkg_cache_extract(const char *name, const char *version, const char *out_dir);
/* Issue #139: defined with pkg_cache_extract() further down; called from
 * the install path's own cache/artifact-hit branch above it. */
static void warn_unexecutable_binaries(const char *root, const char *pkg_name, int depth,
                                        int *reported);
static int pkg_artifact_is_configured(void);
/* Issue #129: defined with the rest of the push machinery further
 * down; called from pkg_build_completed()'s own fresh-build branch. */
static void pkg_artifact_push_enqueue(const char *name, const char *version);
static void pkg_artifact_build_request(const char *name, const char *version, char *out_url,
                                        size_t out_url_size, char *out_header, size_t out_header_size);
static void artifact_sentinel_path(const char *name, const char *version, char *out, size_t out_size);

/*
 * Starts the fetch for a single package already known to have a valid
 * recipe (resolve_chain() already checked). Shared by pkg_install_start()
 * (the first queue entry) and pkg_build_completed() (chaining into the
 * next one). For a genuine in-place upgrade of an already-INSTALLED
 * entry, deliberately does NOT reset it -- e->version/e->files are
 * left exactly as they are until the new build actually succeeds, so
 * a failure here leaves the old, still-physically-present install
 * correctly tracked instead of silently orphaned.
 */
static enum pkg_error start_fetch_for(const char *name, int chain_idx, pid_t *out_pid, int *out_pidfd)
{
	struct pkg_recipe recipe;
	char recipe_path[PATH_MAX];
	struct pkg_entry *e;
	int i, slot = -1;
	int is_upgrade;
	int cache_hit;
	pid_t pid;
	int pidfd;

	if (find_recipe_path(name, current_fetch_effective_version(chain_idx), recipe_path,
	                      sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0)
		return PKG_ERR_INVALID_RECIPE;

	/* ADR-0122/ADR-0289: computed fresh for every queue entry (never
	 * left stale from a prior one). The local cache is consulted for a
	 * hostbuild too. ADR-0122 originally excluded hostbuild here,
	 * reasoning that a hostbuild's own artifact never belongs in a
	 * shared package cache -- true of the OUTPUT (it is harvested to
	 * ARTIFACTS_DIR, never merged into any image, see the harvest
	 * branch in pkg_fetch_completed()), but the exclusion wrongly
	 * extended to this READ. Since #129 a hostbuild publishes its
	 * artifact, and a successful remote artifact fetch saves the
	 * fetched tarball into the local cache (pkg_cache_save_from_file(),
	 * further down), so the local cache legitimately holds hostbuild
	 * packages -- and reusing those bytes lets a re-hostbuild skip a
	 * re-download. A hostbuild that BUILDS still never writes the local
	 * cache (its harvest branch has no pkg_cache_save() -- only the
	 * ordinary-install branch does), so a hit here only ever comes from
	 * a prior fetch, whose bytes are that same version and therefore
	 * the same artifact. Held in a local here (rather than written
	 * straight to e->cache_hit) since e isn't resolved/settled until
	 * just below -- a brand new entry gets a fresh memset() that would
	 * wipe a too-early write right back out; the forked child below
	 * reads this same local via its own fork()-inherited copy, not
	 * e->cache_hit, for the identical reason. */
	cache_hit = pkg_cache_has(recipe.name, recipe.version);

	e = pkg_find(name, g_chains[chain_idx].image);
	is_upgrade = (e != NULL && e->state == PKG_STATE_INSTALLED);

	if (e == NULL || !is_upgrade) {
		if (e == NULL) {
			for (i = 0; i < PKG_MAX_PACKAGES; i++) {
				if (!g_packages[i].in_use) {
					slot = i;
					break;
				}
			}
			if (slot < 0)
				return PKG_ERR_FULL;
			e = &g_packages[slot];
		} else {
			pkg_entry_free_files(e); /* retry after a previous FAILED attempt */
		}
		memset(e, 0, sizeof(*e));
		e->in_use = 1;
		strncpy(e->name, recipe.name, sizeof(e->name) - 1);
		strncpy(e->image, g_chains[chain_idx].image, sizeof(e->image) - 1);
	}
	e->state = PKG_STATE_FETCHING;
	/*
	 * #326: the recipe is parsed and present RIGHT NOW, so what this
	 * job is installing is settled here rather than re-read at
	 * completion, where the revision may since have been withdrawn.
	 */
	snprintf(g_chains[chain_idx].fetch_resolved_version,
	         sizeof(g_chains[chain_idx].fetch_resolved_version), "%s", recipe.version);
	snprintf(g_chains[chain_idx].fetch_resolved_depends,
	         sizeof(g_chains[chain_idx].fetch_resolved_depends), "%s", recipe.depends);
	/*
	 * ADR-0272: a run opens here, at the single place a job begins --
	 * so every atom of a chain gets one, not just the package that was
	 * asked for. What caused it is the CHAIN's, read rather than
	 * consumed: see struct pkg_chain's own run_trigger for the two
	 * ways consuming a module static here got it wrong.
	 */
	e->run_started_at = time(NULL);
	snprintf(e->run_trigger, sizeof(e->run_trigger), "%s", g_chains[chain_idx].run_trigger);
	e->error[0] = '\0';
	e->status = PIPELINE_OK; /* the previous attempt's outcome is not this
	                          * attempt's story; cleared alongside error[]
	                          * rather than left to outlive it */
	e->kept_build_container[0] = '\0'; /* ADR-0175: a fresh attempt starting means any
	                                     * previously-preserved failed build container's
	                                     * name is no longer this entry's current story --
	                                     * the memset() above already clears it for the
	                                     * fresh-slot case, this covers the upgrade-reuse
	                                     * case (e != NULL, not memset()'d) too. */
	e->cache_hit = cache_hit;

	if (persist_mkdir_p(g_sources_dir) != 0) {
		pkg_fail(e, is_upgrade, PIPELINE_FETCH, "could not create sources directory");
		return PKG_ERR_PERSIST_FAILED;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		pkg_fail(e, is_upgrade, PIPELINE_FETCH, "fork failed");
		return PKG_ERR_SPAWN_FAILED;
	}
	if (pid == 0) {
		/* One curl per source entry, sequentially, inside this same
		 * forked child (ADR-0036) -- a grandchild per URL, not a
		 * generated shell command, so a recipe-supplied URL never
		 * passes through shell interpolation. The single pidfd the
		 * caller registers for *this* child already covers the whole
		 * sequence; pkg_fetch_completed()'s existing "one exit status
		 * summarizes the whole fetch" contract needs no change. */
		int j;
		char fetch_err_path[PATH_MAX];

		fetch_error_sidecar_path(recipe.name, fetch_err_path, sizeof(fetch_err_path));
		unlink(fetch_err_path);

		if (cache_hit) {
			/* Local cache already has this exact (name,version) --
			 * nothing to fetch; pkg_fetch_completed() stages straight
			 * from the cache. */
			_exit(0);
		}

		/*
		 * ADR-0122: a recipe that has opted in (pkg_artifact_sha256=
		 * set) gets one best-effort attempt at a precompiled artifact
		 * from the configured plain-HTTP artifact server before
		 * falling through to a real source fetch+compile -- verified
		 * against the recipe's own git-tracked checksum before ever
		 * being trusted (the artifact server itself is never a trust
		 * boundary, same "verify, don't just believe" posture
		 * pkg_source/pkg_sha256 already have). A miss here (no server
		 * configured, this recipe never opted in, a 404, or a
		 * checksum mismatch) is not a fetch failure -- it just means
		 * "build from source like always," so the real per-source
		 * loop below still runs unconditionally in that case.
		 */
		/*
		 * ADR-0279: a checksum in the recipe is no longer the only way
		 * in. A signature published beside the artifact is an approval
		 * too, and it is the one that travels -- the checksum reaches
		 * only the host that built the package, which is #403.
		 *
		 * So the tier is tried when EITHER exists: a recipe that
		 * carries a checksum, or a host that holds a key some
		 * signature could be checked against. Both gates are applied
		 * below and, where both are present, both must pass.
		 */
		if (pkg_artifact_is_configured() &&
		    (recipe.artifact_sha256[0] != '\0' ||
		     releasekey_trust_count(g_trusted_keys_dir) > 0)) {
			char artifact_url[768];
			char artifact_header[320];
			char artifact_path[PATH_MAX];
			/*
			 * Both initialised because the mismatch-note check below
			 * reuses them after a short-circuited && chain: if
			 * waitpid() failed, `status` was never written; if curl
			 * succeeded but pkg_run_capture_sha256() failed, `sha_out`
			 * was never written. Reading either then is stack garbage
			 * -- found by review (Fable's audit of #149), not by a
			 * failure, which is exactly why it gets fixed now rather
			 * than after it writes a garbage "cache served ..." note.
			 */
			char sha_out[128] = "";
			int sha_ok = 0;
			int sig_ok = 0;

			pkg_artifact_build_request(recipe.name, recipe.version, artifact_url,
			                            sizeof(artifact_url), artifact_header,
			                            sizeof(artifact_header));
			artifact_sentinel_path(recipe.name, recipe.version, artifact_path,
			                        sizeof(artifact_path));
			unlink(artifact_path);

			/* No inner fork any more (#410): this whole function
			 * already runs inside start_fetch_for()'s own forked
			 * child, and curlfetch_perform() is a synchronous library
			 * call, not a subprocess needing its own fork+waitpid. */
			{
				struct curlfetch_opts opts;

				memset(&opts, 0, sizeof(opts));
				opts.url = artifact_url;
				opts.path = artifact_path;
				opts.header1 = artifact_header[0] != '\0' ? artifact_header : NULL;
				opts.connect_timeout = PKG_CURL_CONNECT_TIMEOUT_SECS;
				opts.low_speed_limit = PKG_CURL_LOW_SPEED_LIMIT;
				opts.low_speed_time = PKG_CURL_LOW_SPEED_TIME_SECS;
				if (curlfetch_perform(&opts, NULL, NULL, 0) == 0)
					sha_ok = pkg_run_capture_sha256(artifact_path, sha_out, sizeof(sha_out)) == 0;
			}

			/*
			 * The signature gate (ADR-0279).
			 *
			 * Fetched only once the artifact is here and hashed:
			 * there is nothing to verify otherwise, and the digest is
			 * half of what the trusted comment has to agree with.
			 *
			 * The comment is compared against a string built here
			 * rather than parsed, and compared in FULL. A signature
			 * whose comment does not say exactly which package,
			 * revision and bytes it approves is not an approval of
			 * this artifact -- it is an approval of something, and
			 * accepting it on the strength of the key alone is how a
			 * genuine glibc@2.44-16 artifact gets installed as
			 * 2.44-17. minisign's global signature covers the comment,
			 * so agreeing with it means the signer said this.
			 */
			if (sha_ok && releasekey_trust_count(g_trusted_keys_dir) > 0) {
				char sig_path[PATH_MAX];
				char sig_url[832];
				char comment[PKG_NAME_MAX + PKG_VERSION_MAX + 96];
				char want[PKG_NAME_MAX + PKG_VERSION_MAX + 96];
				struct curlfetch_opts sig_opts;

				snprintf(sig_path, sizeof(sig_path), "%s.minisig", artifact_path);
				snprintf(sig_url, sizeof(sig_url), "%s.minisig", artifact_url);
				unlink(sig_path);

				memset(&sig_opts, 0, sizeof(sig_opts));
				sig_opts.url = sig_url;
				sig_opts.path = sig_path;
				sig_opts.header1 = artifact_header[0] != '\0' ? artifact_header : NULL;
				sig_opts.connect_timeout = PKG_CURL_CONNECT_TIMEOUT_SECS;
				sig_opts.low_speed_limit = PKG_CURL_LOW_SPEED_LIMIT;
				sig_opts.low_speed_time = PKG_CURL_LOW_SPEED_TIME_SECS;
				if (curlfetch_perform(&sig_opts, NULL, NULL, 0) == 0 &&
				    releasekey_verify_file(artifact_path, sig_path, g_trusted_keys_dir, comment,
				                            sizeof(comment)) == RELEASEKEY_OK) {
					snprintf(want, sizeof(want), "cix pkg %s@%s sha256=%s", recipe.name,
					          recipe.version, sha_out);
					if (strcmp(comment, want) == 0)
						sig_ok = 1;
				}
				unlink(sig_path);
			}

			/*
			 * Where both approvals exist, both must agree. A recipe
			 * with a checksum keeps exactly the behaviour ADR-0122
			 * gave it -- 624 artifacts are published unsigned and must
			 * stay installable -- and a recipe without one now has a
			 * way in at all, which is the cold-box case #306 and #403
			 * are both about.
			 */
			if (recipe.artifact_sha256[0] != '\0') {
				if (sha_ok && strcasecmp(sha_out, recipe.artifact_sha256) == 0)
					_exit(0); /* verified -- pkg_fetch_completed() stages from this file */
			} else if (sig_ok) {
				_exit(0);
			}
			/*
			 * Issue #149: an artifact that downloaded fine but failed
			 * its checksum is a different situation from one that was
			 * simply absent, and it is worth saying so.
			 *
			 * "Absent" is ordinary -- most packages have no published
			 * artifact. A MISMATCH means the cache is serving
			 * different bytes than the recipe approves, which in
			 * practice means the recipe's pkg_artifact_sha256 was
			 * edited in place on an already-published version and can
			 * never take effect (#145). The install then silently
			 * falls back to source and fails with whatever that path
			 * fails with -- on a host without a working resolver, a
			 * DNS error that says nothing about checksums. That
			 * misdirection cost a real debugging cycle across 14
			 * recipes.
			 *
			 * Written to the fetch note so it surfaces whether or not
			 * the source fallback goes on to succeed: a mismatch is
			 * worth knowing about even when the install works.
			 */
			if (sha_ok) {
				char note_path[PATH_MAX];
				int nfd;

				fetch_note_sidecar_path(recipe.name, note_path, sizeof(note_path));
				nfd = open(note_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
				if (nfd >= 0) {
					char note[640];
					int n;

					/*
					 * Two different disappointments, and they need
					 * different sentences. A checksum mismatch means
					 * the cache is serving bytes this recipe does not
					 * approve. A signature that did not verify means
					 * this host cannot establish that Cix published
					 * them at all -- and for a recipe with no checksum
					 * that is the ONLY gate, so saying "failed
					 * checksum" there would name a field the recipe
					 * does not have (ADR-0279).
					 */
					if (recipe.artifact_sha256[0] != '\0')
						n = snprintf(note, sizeof(note),
						              "artifact for %s@%s downloaded but failed checksum "
						              "(recipe approves %.16s..., cache served %.16s...) -- "
						              "falling back to source; if this recipe version is "
						              "already published, an edited pkg_artifact_sha256 "
						              "cannot take effect, bump pkg_version instead",
						              recipe.name, recipe.version, recipe.artifact_sha256,
						              sha_out);
					else
						n = snprintf(note, sizeof(note),
						              "artifact for %s@%s downloaded but its signature did "
						              "not verify against this host's trusted keys -- "
						              "falling back to source. Either it was published "
						              "unsigned, or it was signed by a key this host does "
						              "not trust: `cixctl pkg sync` adopts the keys "
						              "published in docs/keys/",
						              recipe.name, recipe.version);
					if (n > 0) {
						ssize_t written = write(nfd, note, (size_t)n);

						(void)written;
					}
					close(nfd);
				}
			}
			unlink(artifact_path); /* not found / wrong checksum -- discard, fall through */
		}

		/*
		 * Issue #153: a source still carrying {{REPO_TOKEN}} means no
		 * repo token is configured on this host -- substitution
		 * happens at recipe-parse time and leaves the placeholder
		 * alone when the stored token is empty (#60).
		 *
		 * Caught here rather than handed to curl, because curl reports
		 * it as URL syntax:
		 *
		 *   curl: (3) nested brace in URL position 17:
		 *   https://osakka:{{REPO_TOKEN}}@git.home.arpa/...
		 *
		 * which reads like a malformed recipe and says nothing about
		 * the token. The daemon knows exactly what is wrong and can
		 * say so, which is one `pkg repo-config set --token=` away
		 * from fixed. Hit live on a real host, where the fetch had
		 * been failing this way with nothing naming the cause.
		 */
		for (j = 0; j < recipe.source_count; j++) {
			if (strstr(recipe.source[j], "{{REPO_TOKEN}}") != NULL) {
				static const char msg[] =
				    "pkg_source needs a repo token, but none is configured on this host -- "
				    "set one with `cixctl pkg repo-config set --token=...`";
				int efd = open(fetch_err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

				/*
				 * Written to the error sidecar, NOT to stderr. This
				 * daemon never mirrors a child's stderr into the log
				 * store or into e->error, so an fprintf here would be
				 * invisible to the operator -- which is the whole
				 * failure being fixed. The sidecar is what
				 * pkg_fetch_completed() reads back as the "detail"
				 * half of the reported error.
				 */
				if (efd >= 0) {
					ssize_t written = write(efd, msg, sizeof(msg) - 1);

					(void)written;
					close(efd);
				}
				/* Distinct from curl's own exit codes: nothing was
				 * fetched, so reporting a curl status would be the
				 * same species of misleading error this guard exists
				 * to remove. */
				_exit(PKG_FETCH_EXIT_PRECONDITION);
			}
		}

		for (j = 0; j < recipe.source_count; j++) {
			char src_tarball_path[PATH_MAX];
			struct curlfetch_opts opts;
			char curl_err[256];
			/*
			 * retry_count/retry_all_errors/resume: large sources
			 * (e.g. kernel.recipe's ~150MB tarball) hit real,
			 * reproducible mid-transfer connection resets in
			 * this project's own dev sandbox (ADR-0056) --
			 * confirmed independent of HTTP version (both
			 * default HTTP/2 and --http1.1 reset at different
			 * offsets). A curated retry set alone is not enough:
			 * it covers timeouts and HTTP 5xx/408/429, and does
			 * NOT cover a raw connection reset by default --
			 * confirmed the hard way when a curated-only run
			 * still failed outright on the first reset.
			 * retry_all_errors widens that to every failure.
			 * sha256 verification downstream in
			 * pkg_fetch_completed() still catches any corrupt
			 * resume, so this only ever helps, never masks a bad
			 * download.
			 */
			snprintf(src_tarball_path, sizeof(src_tarball_path), "%s/%s-%s-%d.src",
			         g_sources_dir, recipe.name, recipe.version, j);

			/*
			 * resume only makes sense continuing *this* attempt's
			 * own partial download (recovering from a transient
			 * mid-transfer reset within curlfetch_perform()'s own
			 * retry loop). A stale, already-fully-downloaded file
			 * left over from an earlier, separate fetch attempt at
			 * this same fixed path makes it request a byte range
			 * starting past EOF, which the server correctly
			 * answers with HTTP 416 -- confirmed the hard way
			 * (ADR-0056): every retry hit the identical 416 since
			 * the file never changed. Starting every fresh
			 * top-level attempt from a clean slate avoids this;
			 * ENOENT is expected and fine.
			 */
			unlink(src_tarball_path);

			memset(&opts, 0, sizeof(opts));
			opts.url = recipe.source[j];
			opts.path = src_tarball_path;
			opts.connect_timeout = PKG_CURL_CONNECT_TIMEOUT_SECS;
			opts.low_speed_limit = PKG_CURL_LOW_SPEED_LIMIT;
			opts.low_speed_time = PKG_CURL_LOW_SPEED_TIME_SECS;
			opts.retry_count = 8;
			opts.retry_all_errors = 1;
			opts.retry_delay = 3;
			opts.resume = 1;
			if (curlfetch_perform(&opts, NULL, curl_err, sizeof(curl_err)) != 0) {
				int fd;

				/* recipe.source[j] can carry the real repo token
				 * (gitea's token-in-URL convention) after {{REPO_
				 * TOKEN}} substitution -- redact before this ever
				 * reaches a sidecar file, same discipline as
				 * pkg_sync_start's own child (#405/#410). */
				redact_repo_token(curl_err, sizeof(curl_err));
				fd = open(fetch_err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
				if (fd >= 0) {
					ssize_t written = write(fd, curl_err, strlen(curl_err));

					(void)written;
					close(fd);
				}
				_exit(1);
			}
		}
		_exit(0);
	}

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		perror("pidfd_open");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		pkg_fail(e, is_upgrade, PIPELINE_FETCH, "could not track fetch subprocess");
		return PKG_ERR_SPAWN_FAILED;
	}

	strncpy(g_chains[chain_idx].name, name, sizeof(g_chains[chain_idx].name) - 1);
	g_chains[chain_idx].fetch_pid = pid; /* #239: so a cancel can reach it */
	*out_pid = pid;
	*out_pidfd = pidfd;
	return PKG_OK;
}

enum pkg_error pkg_install_start(const char *name, const char *image, const char *version,
                                  int upgrade, int keep_on_failure, char *out_started_name,
                                  size_t out_started_name_size, pid_t *out_pid, int *out_pidfd,
                                  int *out_chain_idx)
{
	struct pkg_recipe recipe;
	char recipe_path[PATH_MAX];
	struct pkg_entry *e;
	char visiting[PKG_MAX_DEP_CHAIN][PKG_NAME_MAX];
	int visiting_count = 0;
	char err[PKG_ERROR_MAX];
	enum pkg_error perr;
	int chain_idx;
	char trigger[16];

	/*
	 * ADR-0272: consumed HERE, before any early return, so a caller's
	 * "rolling" cannot survive a refused start and mislabel the next
	 * install. Held in a local until there is a chain to put it on.
	 */
	snprintf(trigger, sizeof(trigger), "%s", g_next_run_trigger);
	snprintf(g_next_run_trigger, sizeof(g_next_run_trigger), "%s", PKG_RUN_TRIGGER_REQUEST);

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	if (image != NULL && image[0] != '\0' && !pkg_image_is_valid(image))
		return PKG_ERR_INVALID_NAME;
	if (pkg_any_job_busy())
		return PKG_ERR_BUSY;
	chain_idx = chain_alloc();
	if (chain_idx >= 0)
		snprintf(g_chains[chain_idx].run_trigger, sizeof(g_chains[chain_idx].run_trigger), "%s",
		         trigger);

	if (find_recipe_path(name, version, recipe_path, sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0)
		return PKG_ERR_INVALID_RECIPE;

	snprintf(g_chains[chain_idx].image, sizeof(g_chains[chain_idx].image), "%s", normalize_image(image));
	snprintf(g_chains[chain_idx].target_version, sizeof(g_chains[chain_idx].target_version), "%s",
	         (version != NULL) ? version : "");
	g_chains[chain_idx].is_hostbuild = 0;
	g_chains[chain_idx].keep_on_failure = keep_on_failure;

	e = pkg_find(name, g_chains[chain_idx].image);
	if (e != NULL && e->state == PKG_STATE_INSTALLED &&
	    (!upgrade || strcmp(e->version, recipe.version) == 0))
		return PKG_ERR_DUPLICATE;

	/* Resolve the full install order: name's own dependencies first
	 * (skipped if already satisfied), then name itself -- force=1 so
	 * an explicit upgrade target is queued even though it's already
	 * installed (resolve_chain()'s default "already installed, skip"
	 * rule is for pure dependencies, not the package actually asked for). */
	g_chains[chain_idx].dep_queue_count = 0;
	if (resolve_chain(name, version, g_chains[chain_idx].image, 1, g_chains[chain_idx].dep_queue,
	                   &g_chains[chain_idx].dep_queue_count, visiting, &visiting_count, err,
	                   sizeof(err)) != 0) {
		/* #318: resolve_chain() names exactly what it could not
		 * resolve, and that message used to be discarded here -- so the
		 * caller was told "no such recipe, or it failed to parse" about
		 * the package it asked for, whose recipe is fine. */
		logstore_write("cixd", "error", "pkg install %s@%s: %s", name,
		                g_chains[chain_idx].image, err);
		return PKG_ERR_DEP_UNRESOLVABLE;
	}

	g_chains[chain_idx].dep_queue_pos = 0;
	g_chains[chain_idx].dep_queue_is_upgrade = (e != NULL && e->state == PKG_STATE_INSTALLED);

	perr = start_fetch_for(g_chains[chain_idx].dep_queue[0], chain_idx, out_pid, out_pidfd);
	if (perr != PKG_OK) {
		g_chains[chain_idx].dep_queue_count = 0;
		return perr;
	}
	*out_chain_idx = chain_idx;
	snprintf(out_started_name, out_started_name_size, "%s", g_chains[chain_idx].dep_queue[0]);
	return PKG_OK;
}

enum pkg_error pkg_hostbuild_start(const char *name, const char *version,
                                    int upgrade, const char *extra_config_symbols,
                                    int keep_on_failure, pid_t *out_pid, int *out_pidfd,
                                    int *out_chain_idx)
{
	struct pkg_recipe recipe;
	char recipe_path[PATH_MAX];
	struct pkg_entry *e;
	enum pkg_error perr;
	int chain_idx;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	if (extra_config_symbols != NULL &&
	    strlen(extra_config_symbols) >= PKG_HOSTBUILD_EXTRA_SYMBOLS_MAX)
		return PKG_ERR_INVALID_NAME;
	if (pkg_any_job_busy())
		return PKG_ERR_BUSY;
	chain_idx = chain_alloc();

	if (find_recipe_path(name, version, recipe_path, sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0)
		return PKG_ERR_INVALID_RECIPE;
	/*
	 * pkg_depends is CARRIED here, never resolved (#465).
	 *
	 * This used to refuse any non-empty pkg_depends outright, on the
	 * stated grounds that "every prerequisite must already be baked
	 * into build_image's own rootfs" -- which is true, and is a
	 * statement about pkg_build_depends, a different field. A
	 * hostbuild's build container is rooted on build_image's current
	 * version (the is_hostbuild branch of
	 * pkg_build_container_spec(), which is the first arm of that
	 * if-chain and is why pkg_build_depends is not consulted for a
	 * hostbuild either), so pkg_depends never had any part in
	 * composing this build.
	 *
	 * What pkg_depends describes is what the finished artifact needs
	 * in order to RUN, and that outlives the harvest:
	 * pkg_build_completed() records it onto the entry from the
	 * chain's captured copy BEFORE the hostbuild/install split, so it
	 * reaches GET /v1/pkg and the persisted record either way.
	 * Resolution is skipped because resolving means "install the
	 * closure into an image" and a hostbuild has no image to merge
	 * into -- not because the declaration is meaningless. The one
	 * reader that walks the closure, declared_provided_sonames() for
	 * the #389 undeclared-link gate, sits in the non-hostbuild arm of
	 * pkg_build_completed() and is never reached from here.
	 *
	 * Refusing it made cix's own recipe unsatisfiable in both
	 * directions at once: cixd links libssl, so an ordinary
	 * `pkg install cix` needs pkg_depends="openssl" to pass that same
	 * gate, and declaring it made `pkg hostbuild cix` -- the only way
	 * this platform builds itself -- refuse the recipe as invalid.
	 */

	/*
	 * No build image is resolved, validated or recorded here, because
	 * a hostbuild no longer has one (ADR-0304, issue #482).
	 *
	 * What this function used to do: take a caller's --build-image=,
	 * fall back to the recipe's own pkg_build_image= (#182), refuse a
	 * mismatch between the two (PKG_ERR_WRONG_BUILD_IMAGE), refuse an
	 * absent choice (PKG_ERR_NO_BUILD_IMAGE), and refuse a named image
	 * with no current version (PKG_ERR_NO_SUCH_BUILD_IMAGE, #317). All
	 * five are gone with the field.
	 *
	 * The build container is now composed from pkg_build_depends by
	 * the ADR-0199 arm of pkg_build_container_spec(), the same one
	 * every ordinary install has used since #109 -- so a hostbuild's
	 * environment is what its recipe declares rather than whatever an
	 * operator-curated image happens to contain. That image was the
	 * last place in this platform where a declaration could be
	 * decorative: kernel-builder carried wireless-regdb, the kernel
	 * build embedded it via CONFIG_EXTRA_FIRMWARE, and the recipe
	 * declared it nowhere -- measured by composing the declared set
	 * and watching the build fail on the missing database.
	 */

	/*
	 * The sentinel image is created here if it does not exist, and
	 * that is load-bearing rather than tidiness.
	 *
	 * A cache-hit job takes the #144 no-op arm of
	 * pkg_build_container_spec(), which roots its container on the
	 * chain's TARGET image -- PKG_HOSTBUILD_IMAGE for a hostbuild.
	 * That arm wants a real rootfs path even though the container is a
	 * deliberate no-op, because an empty build_lowerdir fails the
	 * overlay mount. While a hostbuild had a build image, the build
	 * image supplied it and this never came up; without one, the
	 * sentinel has to be a real (empty) image.
	 *
	 * It exists on any host that has ever completed a hostbuild, so
	 * the case this covers is narrow and specifically the one that
	 * matters: a FRESH host deploying cix straight from a
	 * checksum-verified artifact, which is the documented recovery
	 * route for a box whose resolver is broken and therefore cannot
	 * build from source (#138). IMAGE_ERR_DUPLICATE is the
	 * already-exists answer and is not a failure, the same way
	 * image_produce_new_version() treats it. A real failure here is a
	 * disk/state failure, hence PKG_ERR_PERSIST_FAILED.
	 */
	{
		enum image_error ierr = image_create(PKG_HOSTBUILD_IMAGE);

		if (ierr != IMAGE_OK && ierr != IMAGE_ERR_DUPLICATE) {
			logstore_write("cixd", "error",
			               "pkg hostbuild %s: could not create the %s sentinel image", name,
			               PKG_HOSTBUILD_IMAGE);
			return PKG_ERR_PERSIST_FAILED;
		}
	}

	/*
	 * ADR-0094: same "409 with no way out" gap pkg_install_start()
	 * already solved with its own upgrade parameter -- a hostbuild
	 * that already succeeded once for this name used to make every
	 * later hostbuild attempt a bare, permanent PKG_ERR_DUPLICATE,
	 * with no way to rebuild from fresh source under the same recipe
	 * name (e.g. a new commit under cix.recipe's own tracked tag).
	 * Same rule as install: only actually re-run the build if the
	 * recipe's own version genuinely differs from what's already
	 * installed -- upgrade=1 with an unchanged version is still a
	 * no-op duplicate, since nothing would actually be different.
	 */
	e = pkg_find(name, PKG_HOSTBUILD_IMAGE);
	if (e != NULL && e->state == PKG_STATE_INSTALLED &&
	    (!upgrade || strcmp(e->version, recipe.version) == 0))
		return PKG_ERR_DUPLICATE;

	snprintf(g_chains[chain_idx].image, sizeof(g_chains[chain_idx].image), "%s", PKG_HOSTBUILD_IMAGE);
	snprintf(g_chains[chain_idx].target_version, sizeof(g_chains[chain_idx].target_version), "%s",
	         (version != NULL) ? version : "");
	snprintf(g_chains[chain_idx].hostbuild_extra_config_symbols,
	         sizeof(g_chains[chain_idx].hostbuild_extra_config_symbols), "%s",
	         (extra_config_symbols != NULL) ? extra_config_symbols : "");
	g_chains[chain_idx].is_hostbuild = 1;
	g_chains[chain_idx].keep_on_failure = keep_on_failure;

	/* A hostbuild job is always a single, standalone entry: no
	 * resolve_chain(), because a declared pkg_depends is carried and
	 * never resolved here (ADR-0303) -- there is no image to install a
	 * dependency closure into. This comment said "pkg_depends is
	 * required empty above" and was left false by that change; the
	 * requirement it named is gone. */
	g_chains[chain_idx].dep_queue_count = 0;
	strncpy(g_chains[chain_idx].dep_queue[0], name, PKG_NAME_MAX - 1);
	g_chains[chain_idx].dep_queue[0][PKG_NAME_MAX - 1] = '\0';
	g_chains[chain_idx].dep_queue_count = 1;
	g_chains[chain_idx].dep_queue_pos = 0;
	g_chains[chain_idx].dep_queue_is_upgrade = 0;

	perr = start_fetch_for(name, chain_idx, out_pid, out_pidfd);
	if (perr != PKG_OK) {
		g_chains[chain_idx].dep_queue_count = 0;
		g_chains[chain_idx].is_hostbuild = 0;
		return perr;
	}
	*out_chain_idx = chain_idx;
	return PKG_OK;
}

/*
 * ADR-0177/issue #46: the common tail every real spawn of a build
 * container shares -- populate spec_out from e's own build_* fields
 * (already fully computed by whichever caller reached this point,
 * fresh via pkg_fetch_completed() below or reused via
 * pkg_resume_build()), stand up the output-capture pipe, mark the
 * entry BUILDING. Extracted so pkg_resume_build() doesn't duplicate
 * this exact, security/correctness-sensitive sequence (CLONE_* flags,
 * memory/cpu ceilings, the O_CLOEXEC/O_NONBLOCK pipe dance) a second
 * time -- one source of truth for "how a build container's own spec
 * gets built and its output pipe wired up," regardless of how its
 * build_* fields were populated. Always returns 1 (never fails) --
 * matches pkg_fetch_completed()'s own prior inline behavior exactly,
 * a pipe2()/fcntl() failure degrades to "no captured output," it was
 * never a fatal condition for the build itself.
 */
static int start_build_container_spec(int chain_idx, struct pkg_entry *e,
                                       struct container_spec *spec_out, int *out_stdio_write_fd,
                                       const char *build_caps)
{
	memset(spec_out, 0, sizeof(*spec_out));
	spec_out->ns.clone_flags =
	    CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWCGROUP | CLONE_INTO_CGROUP;
	spec_out->ns.hostname = e->build_container_name;
	/*
	 * Issue #85: every build container lives under ONE parent cgroup that
	 * carries the configured budget, and the children carry no ceiling of
	 * their own.
	 *
	 * ADR-0165 originally applied memory_max/cpu_max to each build
	 * container individually. That reads like a budget but is not one:
	 * with max_concurrent_jobs (default 10) the real ceiling was
	 * limit x concurrency. On a real 2-CPU box, four concurrent builds at
	 * a 1.5-CPU each ceiling demanded 6 CPUs, starved cixd off the run
	 * queue, and -- because a shell-less host has no way in except the
	 * daemon's own REST API -- took the machine out of reach until it was
	 * reset. Putting the limit on the shared parent makes the configured
	 * number mean what it says at any concurrency; the kernel then shares
	 * that budget between however many builds are actually running.
	 */
	pkg_build_ensure_parent_cgroup();
	snprintf(e->build_cgroup_path, sizeof(e->build_cgroup_path), "%s/%s",
	         PKG_BUILD_CGROUP_PARENT, e->build_container_name);
	spec_out->cg.name = e->build_cgroup_path;

	/*
	 * Capabilities this recipe asked its build container for (#224).
	 *
	 * Unknown names are refused by container_caps_parse() downstream
	 * rather than silently dropped -- a recipe that asks for something
	 * this platform will not grant should fail loudly, not build in a
	 * weaker environment than it declared and then fail somewhere less
	 * obvious.
	 */
	if (build_caps != NULL && build_caps[0] != '\0') {
		const char *p = build_caps;

		spec_out->cap_add_count = 0;
		while (*p != '\0' && spec_out->cap_add_count < CONTAINER_MAX_CAP_ADD) {
			size_t n = 0;

			while (*p == ' ' || *p == '\t')
				p++;
			while (p[n] != '\0' && p[n] != ' ' && p[n] != '\t')
				n++;
			if (n == 0)
				break;
			if (n >= CONTAINER_CAP_NAME_MAX)
				n = CONTAINER_CAP_NAME_MAX - 1;
			memcpy(spec_out->cap_add[spec_out->cap_add_count], p, n);
			spec_out->cap_add[spec_out->cap_add_count][n] = '\0';
			spec_out->cap_add_count++;
			p += n;
		}
		logstore_write("cixd", "info", "pkg %s@%s: build container requests capabilities: %s",
		               e->name, e->image, build_caps);
	}
	spec_out->ov.lowerdir = e->build_lowerdir;
	spec_out->ov.upperdir = e->build_upperdir;
	spec_out->ov.workdir = e->build_workdir;
	spec_out->ov.merged = e->build_merged;
	spec_out->mnt.put_old_rel = ".old_root";
	spec_out->argv = e->build_argv;
	spec_out->envp = e->build_envp;

	e->build_output_captured_len = 0;
	e->last_output_at = 0;
	e->toolscan_len = 0;            /* issue #302 */
	e->missing_tool[0] = '\0';
	e->missing_tool_count = 0;
	/*
	 * The chain's resolved version, not current_fetch_effective_
	 * version() (#326). That helper returns NULL for anything but an
	 * explicit top-level pin, so the overwhelmingly common unpinned
	 * build passed NULL, fell through to a first install's still-empty
	 * e->version, and landed on the literal "unknown" -- which is the
	 * exact outcome pkg_build_log_open()'s own comment says it exists
	 * to prevent, for "exactly the builds most worth finding again".
	 * The comment was right about the goal and the argument did not
	 * reach it. fetch_resolved_version is set in start_fetch_for() and
	 * is never empty for a job that got this far.
	 */
	pkg_build_log_open(e, g_chains[chain_idx].fetch_resolved_version); /* issue #57 */
	/* ADR-0157 Phase 1: this entry is now the one owning the open
	 * build-output pipe, regardless of whether pipe2()/fcntl() below
	 * actually succeed (both failure branches still explicitly set
	 * build_output_rd to -1, which every reader already treats as
	 * "nothing to read") -- see g_build_output_entry's own doc comment
	 * for why pkg_build_output_close() needs this separate from
	 * g_chains[chain_idx].name/image. */
	g_build_output_entries[chain_idx] = e;
	{
		int output_pipe[2];

		/*
		 * O_CLOEXEC on both ends (dup2() in src/container.c's
		 * pre-exec setup clears it on the child's own fd 1/2
		 * copies before any execve() happens, so the child is
		 * unaffected); O_NONBLOCK only on the read end, set
		 * separately below -- the write end must stay blocking,
		 * since it becomes the build script's own stdout/stderr
		 * and an EAGAIN there would be a real, unhandled error for
		 * most programs.
		 */
		if (pipe2(output_pipe, O_CLOEXEC) == 0) {
			if (fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) == 0) {
				spec_out->capture_output = 1;
				spec_out->stdout_fd = output_pipe[1];
				spec_out->stderr_fd = output_pipe[1];
				e->build_output_rd = output_pipe[0];
				*out_stdio_write_fd = output_pipe[1];
			} else {
				close(output_pipe[0]);
				close(output_pipe[1]);
				e->build_output_rd = -1;
				*out_stdio_write_fd = -1;
			}
		} else {
			e->build_output_rd = -1;
			*out_stdio_write_fd = -1;
		}
	}

	e->state = PKG_STATE_BUILDING;
	/* The clock a stall is measured against before any output arrives.
	 * Without this a build that produces NOTHING -- the worst kind to
	 * be blind to -- would never be reported, because there would be
	 * no "last output" to be old. */
	e->build_started_at = time(NULL);
	e->stall_reported = 0;
	return 1;
}

/*
 * Prepares and starts the build container for an install whose source
 * is already in place.
 *
 * Split out of pkg_fetch_completed() for #238, and the split is the
 * whole point: composing a build environment (ADR-0199) walks and
 * copies every file of every declared build tool, which used to happen
 * inline here, on the event loop. Measured on a real host, a burst of
 * publishes that each triggered a rebuild left one client's request
 * unanswered for 247 seconds -- with no stall recorded, because the
 * loop kept going round doing seconds of this work per pass. Alive by
 * the heartbeat, unusable by any other measure.
 *
 * So composition now runs in a forked child, and this function is
 * re-entered when that child exits. The re-entry is why it takes only
 * re-derivable inputs: the second call recomputes the same recipe and
 * paths and reaches buildenv_image_for()'s "already composed" fast
 * path, which costs nothing. Nothing here is stateful across the two
 * calls, so a resume is indistinguishable from a first call that
 * happened to find the environment ready.
 *
 * Returns 1 to start the build in spec_out, 0 if the install is over
 * (failed, or completed without a container), or 2 if a composition
 * child was forked -- out_compose_pid/out_compose_pidfd then name it,
 * and pkg_buildenv_completed() continues from there.
 */

/*
 * #412: cixd already chooses these symbols (kmod-build --symbol=) and
 * already owns /build/extra (ADR-0036, the directory both call sites
 * below stage every other extra source into) -- writes the file
 * kernel.recipe's own merge_config.sh call reads directly, rather than
 * handing the recipe a space-separated CIX_KMOD_EXTRA_SYMBOLS env var
 * to loop over and re-derive the identical "<symbol>=m" lines from.
 *
 * Empty symbols UNLINKS the file rather than merely skipping the
 * write. A fresh build never sees a stale one (reset_build_container_dir()
 * wipes container_base first), but pkg_resume_build() deliberately
 * preserves /build/extra across a resume -- so a resume that drops the
 * symbols an earlier attempt had must remove what that attempt wrote,
 * or the recipe would merge a file this call never asked for. ENOENT
 * on the unlink is not a failure; there being nothing to remove is the
 * common case.
 *
 * strtok_r splits on " \t\n" (the shell's own IFS default), matching
 * exactly what the retired `for sym in $CIX_KMOD_EXTRA_SYMBOLS` word
 * splitting did -- space alone was narrower than the field it replaced.
 */
static int write_kmod_extra_config(const char *extra_dir, const char *symbols)
{
	char path[PATH_MAX];
	char copy[PKG_HOSTBUILD_EXTRA_SYMBOLS_MAX];
	char *sym, *saveptr;
	FILE *f;

	snprintf(path, sizeof(path), "%s/kmod-extra.config", extra_dir);

	if (symbols == NULL || symbols[0] == '\0') {
		if (unlink(path) != 0 && errno != ENOENT)
			return -1;
		return 0;
	}
	if (persist_mkdir_p(extra_dir) != 0)
		return -1;
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	snprintf(copy, sizeof(copy), "%s", symbols);
	for (sym = strtok_r(copy, " \t\n", &saveptr); sym != NULL; sym = strtok_r(NULL, " \t\n", &saveptr)) {
		if (fprintf(f, "%s=m\n", sym) < 0) {
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	return 0;
}

static int pkg_prepare_build_and_start(int chain_idx, struct pkg_entry *e,
                                        const struct pkg_recipe *recipe_in,
                                        int is_final_upgrade, const char *recipe_path,
                                        struct container_spec *spec_out,
                                        int *out_stdio_write_fd, pid_t *out_compose_pid,
                                        int *out_compose_pidfd)
{
	/* Copied rather than referenced so the extracted body below reads
	 * exactly as it did when it was inline. */
	struct pkg_recipe recipe = *recipe_in;
	char container_base[PATH_MAX];
	char src_dir[PATH_MAX], dest_dir[PATH_MAX], recipe_dst[PATH_MAX], extra_dir[PATH_MAX];
	int i;

	*out_compose_pid = -1;
	*out_compose_pidfd = -1;

	snprintf(container_base, sizeof(container_base), "%s/%s", g_containers_dir,
	         e->build_container_name);
	/*
	 * ADR-0304 (#482): there is no hostbuild arm here any more.
	 *
	 * A hostbuild used to root its build container on
	 * --build-image's own current rootfs, and because that test came
	 * FIRST in this chain, a hostbuild never reached the ADR-0199 arm
	 * below -- so a hostbuild recipe's pkg_build_depends was read by
	 * nothing at all. The kernel recipe's own changelog records the
	 * consequence in as many words, having moved wireless-regdb into
	 * kernel-builder's manifest rather than its own declaration
	 * "because a host build sandbox is the build image rootfs and
	 * build_depends composes nothing there".
	 *
	 * Now a hostbuild falls through exactly like an ordinary install:
	 * a cache hit takes the no-op arm (#144), and anything that
	 * actually builds composes its environment from what the recipe
	 * declares. One answer to "what does a build run inside", which
	 * is what ADR-0199 decided and what this had been quietly
	 * exempt from.
	 */
	if (e->cache_hit) {
		/*
		 * Issue #144: a precompiled package needs NO build environment.
		 *
		 * The container spawned below is a deliberate no-op for a
		 * cache/artifact hit -- its upperdir already IS the finished
		 * package, and it exists only so the merge and
		 * dependency-chaining logic downstream stays one code path.
		 * That was harmless when a no-op container had no
		 * prerequisites. It stopped being harmless when ADR-0199 made
		 * composing a build container require every declared build
		 * tool to be INSTALLED on this box: a no-op suddenly needed a
		 * full toolchain.
		 *
		 * The effect was to invert the artifact tier exactly where it
		 * matters most. On a fresh install, `sed` had a valid,
		 * checksum-verified artifact and still failed with "declared
		 * build tool tcc is not installed anywhere"; `dnsmasq` failed
		 * the same way for want of a build sandbox image. Artifacts
		 * exist so a box does not need a toolchain -- requiring one to
		 * unpack them defeats the entire mechanism.
		 *
		 * So a cache hit is rooted on the TARGET image instead: it
		 * certainly exists (the package is being installed into it),
		 * it needs nothing composed, and the no-op container has
		 * nothing to compile against anyway.
		 */
		char target_version[IMAGE_VERSION_MAX];

		if (image_current_version(g_chains[chain_idx].image, target_version,
		                           sizeof(target_version)) == IMAGE_OK)
			image_version_rootfs_path(g_chains[chain_idx].image, target_version,
			                           e->build_lowerdir, sizeof(e->build_lowerdir));
		else
			e->build_lowerdir[0] = '\0';
	} else if (recipe.build_depends[0] != '\0') {
		/*
		 * Issue #109: this recipe declares its build tools, so its
		 * build container is composed from exactly those and nothing
		 * else -- the environment is a property of the recipe, not of
		 * this box's install history.
		 *
		 * A tool that cannot be provided fails the build here, naming
		 * it. Deliberately no fallback to the shared sandbox: falling
		 * back would mean the build quietly ran against something
		 * fuller than it declared, which is the exact failure mode
		 * this replaces -- and it would succeed, teaching everyone
		 * that the declaration is decorative.
		 */
		char env_image[PKG_IMAGE_NAME_MAX];
		char env_err[256];
		char declared[PKG_DEPENDS_MAX];
		int envr;

		/*
		 * A PBS recipe does not declare the engine that runs it
		 * (ADR-0305), and that is the one place this daemon does not
		 * follow "everything needs declares" literally.
		 *
		 * The distinction is between a package's dependencies and the
		 * driver of its build: without cbs the build cannot start at
		 * all, and the resulting failure (`cbs: not found`) names the
		 * least useful cause there is. It is the same argument
		 * buildenv_resolve_tools() already makes for the C library,
		 * which it appends unconditionally -- and a shell recipe does
		 * not declare bash's interpreter either.
		 *
		 * Appended rather than forced, so a recipe that needs a
		 * specific engine revision can still say `cbs@v0.1.24-4` and
		 * win the slot: buildenv_add_tool() dedups by name and returns
		 * success for one already present, so a recipe's own pin is
		 * seen first and this append is then a no-op.
		 */
		snprintf(declared, sizeof(declared), "%s", recipe.build_depends);
		if (recipe.is_pbs && append_words(declared, sizeof(declared), "cbs") != 0) {
			pkg_fail(e, is_final_upgrade, PIPELINE_BUILD,
			         "the declared build tool list has no room for the cbs engine");
			logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
		}

		envr = buildenv_image_for(declared, env_image, sizeof(env_image), env_err,
		                          sizeof(env_err), out_compose_pid, out_compose_pidfd);
		if (envr < 0) {
			pkg_fail(e, is_final_upgrade, PIPELINE_BUILD, "%s", env_err);
			logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
		}
		if (envr == 1) {
			/*
			 * Composition is running in a child (#238). The chain keeps
			 * its slot and the entry keeps its state; this function is
			 * re-entered by pkg_buildenv_completed() when the child
			 * exits, and takes the fast path on that second call.
			 */
			snprintf(g_chains[chain_idx].buildenv_image,
			         sizeof(g_chains[chain_idx].buildenv_image), "%s", env_image);
			return 2;
		}
		{
			char env_version[IMAGE_VERSION_MAX];

			if (image_current_version(env_image, env_version, sizeof(env_version)) == IMAGE_OK)
				image_version_rootfs_path(env_image, env_version, e->build_lowerdir,
				                           sizeof(e->build_lowerdir));
			else
				e->build_lowerdir[0] = '\0';
		}
		/* Torn down when this build finishes -- see
		 * buildenv_release() at pkg_build_completed(). */
		snprintf(g_chains[chain_idx].buildenv_image, sizeof(g_chains[chain_idx].buildenv_image),
		         "%s", env_image);
	} else {
		/*
		 * Issue #168: there is no fallback. A recipe that does not
		 * declare its build tools cannot be built, and says so.
		 *
		 * Until now this branch resolved to a shared, fungible sandbox
		 * -- which was the `cix-builder` image itself, grown by every
		 * successful install on the box and seeded by `pkg bootstrap`
		 * with the build host's entire /usr. The result was an image
		 * of 88,798 entries, 93% of them belonging to no package in
		 * its manifest: rustup, cargo, chromium, node, go, qemu, sudo,
		 * several gcc trees. A build running against that is not
		 * reproducible and not honest -- it compiles against whatever
		 * this particular box happens to have accumulated, which is a
		 * second source of truth about a recipe's requirements sitting
		 * outside the recipe.
		 *
		 * Refusing is the whole point. A missing declaration now fails
		 * loudly, naming the recipe, rather than succeeding against
		 * something fuller than it asked for -- which is precisely how
		 * a declaration becomes decorative.
		 */
		pkg_fail(e, is_final_upgrade, PIPELINE_BUILD,
		         "recipe declares no pkg_build_depends -- every build environment is composed "
		         "from a recipe's declared tools and nothing else (issue #168); add them to "
		         "%s's recipe",
		         e->name);
		logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}
	snprintf(e->build_upperdir, sizeof(e->build_upperdir), "%s/upper", container_base);
	snprintf(e->build_workdir, sizeof(e->build_workdir), "%s/work", container_base);
	snprintf(e->build_merged, sizeof(e->build_merged), "%s/merged", container_base);

	snprintf(src_dir, sizeof(src_dir), "%s/build/src", e->build_upperdir);
	snprintf(dest_dir, sizeof(dest_dir), "%s/build/pkg-dest", e->build_upperdir);
	/* ADR-0305: cbs refuses any recipe path not ending in .cbs
	 * (has_cbs_extension(), in `build` as well as `explain`), so the
	 * name this is staged under is load-bearing rather than cosmetic --
	 * a build.cbs copied in as recipe.sh could not be built at all. */
	snprintf(recipe_dst, sizeof(recipe_dst), "%s/build/recipe%s", e->build_upperdir,
	         recipe.is_pbs ? PKG_RECIPE_PBS_SUFFIX : ".sh");
	snprintf(extra_dir, sizeof(extra_dir), "%s/build/extra", e->build_upperdir);

	{
		char main_src_path[PATH_MAX];
		char tmp_dir[PATH_MAX];

		snprintf(main_src_path, sizeof(main_src_path), "%s/%s-%s-0.src", g_sources_dir, e->name,
		         recipe.version);
		/*
		 * /tmp -- caught empirically (ADR-0056), same session as the
		 * /bin/sh discovery above: this project's own from-recipe
		 * images have no /tmp at all (CLAUDE.md's own documented
		 * fact), and some real build systems assume it exists
		 * unconditionally regardless of $TMPDIR (GNU Make's own
		 * parallel-job FIFO, mkfifo("/tmp/GMfifoN"), confirmed
		 * directly -- not derived from any env var this project
		 * could instead set). A plain, always-present, empty
		 * directory in the build container's own upperdir, exactly
		 * the same "just make sure it exists" posture build/pkg-dest
		 * and build/src already have -- benefits every future
		 * recipe run against a minimal image, not just this one.
		 */
		snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", e->build_upperdir);

		/*
		 * A sequential if/else-if chain rather than the equivalent
		 * single ||-chain condition -- functionally identical, but
		 * this way pkg_fetch_completed() itself knows (and can log)
		 * exactly which of the six sub-steps failed instead of
		 * bucketing all of them into one opaque message. run_subprocess()
		 * already logs its own subprocess-level detail (exit status/
		 * signal/exec failure) for the two steps that shell out
		 * (reset_build_container_dir(), extract_tarball()); the
		 * errno here covers the two direct-syscall steps
		 * (persist_mkdir_p(), copy_file_simple()), captured
		 * immediately after each one's own failing call so nothing
		 * else can clobber it first.
		 */
		{
			const char *prep_step = NULL;
			int prep_errno = 0;
			/*
			 * ADR-0256: unpacking the source is its OWN stage, not
			 * part of the build. An archive that downloaded intact
			 * and cannot be opened used to report as a build failure,
			 * which sends a reader to a compile log for something
			 * that happened before any compiler ran. Every other step
			 * here really is build-container setup.
			 */
			enum pipeline_stage prep_stage = PIPELINE_BUILD;

			if (reset_build_container_dir(container_base) != 0) {
				prep_step = "reset build container dir";
			} else if (persist_mkdir_p(dest_dir) != 0) {
				prep_step = "create dest dir";
				prep_errno = errno;
			} else if (persist_mkdir_p(tmp_dir) != 0) {
				prep_step = "create tmp dir";
				prep_errno = errno;
			} else if (e->cache_hit) {
				/* ADR-0122: dest_dir is populated straight from the
				 * local cache -- no source to verify, no recipe.sh to
				 * source, no real compile needed. The build container
				 * spawned below runs a pure no-op; its own upperdir
				 * already IS the finished package by the time it
				 * starts, purely to keep pkg_build_completed()'s own
				 * merge/dependency-chaining logic completely unchanged
				 * for this case too. */
				if (pkg_cache_extract(e->name, recipe.version, dest_dir) != 0) {
					prep_step = "extract cached artifact";
					prep_stage = PIPELINE_UNPACK;
				} else {
					/* Issue #139: a checksum proves an artifact is
					 * intact, never that it is usable. */
					int reported = 0;

					warn_unexecutable_binaries(dest_dir, e->name, 0, &reported);
				}
			} else if (persist_mkdir_p(src_dir) != 0) {
				prep_step = "create src dir";
				prep_errno = errno;
			} else if (copy_file_simple(recipe_path, recipe_dst) != 0) {
				prep_step = "copy recipe.sh";
				prep_errno = errno;
			} else if (write_finalize_script(e->build_upperdir) != 0) {
				/* ADR-0251: the policy is not optional, so a build
				 * that could not be given it does not run. */
				prep_step = "write finalize.sh";
				prep_errno = errno;
			} else if (stage_main_source(main_src_path, src_dir, recipe.source[0]) != 0) {
				prep_step = "stage source";
				prep_stage = PIPELINE_UNPACK;
			}

			if (prep_step != NULL) {
				if (prep_errno != 0)
					logstore_write("cixd", "error",
					                "pkg %s@%s: could not prepare build container (%s): %s",
					                e->name, g_chains[chain_idx].image, prep_step,
					                strerror(prep_errno));
				else
					logstore_write("cixd", "error",
					                "pkg %s@%s: could not prepare build container (%s) -- see run_subprocess detail above",
					                e->name, g_chains[chain_idx].image, prep_step);
				if (prep_stage == PIPELINE_UNPACK)
					pkg_fail(e, is_final_upgrade, prep_stage, "%s (%s failed)",
					         pipeline_stage_verb(prep_stage), prep_step);
				else
					pkg_fail(e, is_final_upgrade, prep_stage,
					         "could not prepare the build container (%s failed)", prep_step);
				g_chains[chain_idx].name[0] = '\0';
				g_chains[chain_idx].dep_queue_count = 0;
				return 0;
			}
		}
	}

	/* Sources beyond index 0 are plain files, never extracted -- copied
	 * verbatim into /build/extra/<basename-of-their-own-URL> for
	 * pkg_build()/pkg_install() to reference directly (ADR-0036).
	 * Basename collisions across multiple extra URLs are a stated
	 * recipe-author responsibility, not auto-resolved here. Skipped for
	 * a cache hit -- nothing was fetched at all in that case. */
	if (!e->cache_hit && recipe.source_count > 1) {
		if (persist_mkdir_p(extra_dir) != 0) {
			logstore_write("cixd", "error",
			                "pkg %s@%s: could not prepare build container (create extra dir): %s",
			                e->name, g_chains[chain_idx].image, strerror(errno));
			pkg_fail(e, is_final_upgrade, PIPELINE_BUILD,
			         "could not prepare the build container (create extra dir failed)");
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
		}
		for (i = 1; i < recipe.source_count; i++) {
			char src_path[PATH_MAX], extra_dst[PATH_MAX], extra_basename[PATH_MAX];

			snprintf(src_path, sizeof(src_path), "%s/%s-%s-%d.src", g_sources_dir, e->name,
			         recipe.version, i);
			url_basename(recipe.source[i], extra_basename, sizeof(extra_basename));
			snprintf(extra_dst, sizeof(extra_dst), "%s/%s", extra_dir, extra_basename);
			if (copy_file_simple(src_path, extra_dst) != 0) {
				logstore_write("cixd", "error",
				                "pkg %s@%s: could not prepare build container (copy extra source %d): %s",
				                e->name, g_chains[chain_idx].image, i, strerror(errno));
				pkg_fail(e, is_final_upgrade, PIPELINE_BUILD,
				         "could not prepare the build container (copy extra source %d failed)", i);
				g_chains[chain_idx].name[0] = '\0';
				g_chains[chain_idx].dep_queue_count = 0;
				return 0;
			}
		}
	}

	/*
	 * ADR-0305: hand CBS the sources this daemon has already fetched
	 * and checksum-verified, as cache entries it will accept.
	 *
	 * CBS looks for <cache>/<sha256> and re-verifies the file before
	 * using it (source.c), emitting a source-cache-hit and fetching
	 * nothing. The digest in the recipe IS the filename, so cixd's
	 * verification and CBS's are checks of the same string rather than
	 * two conventions that have to be kept in step -- a recipe whose
	 * digest disagreed with what was fetched fails at the cache lookup
	 * instead of building something unintended.
	 *
	 * Doing it this way is what keeps fetching where it belongs. CBS
	 * fetches over a dlopen()ed libcurl if it has to, and the build
	 * container has no route and no repo token -- and must not: the
	 * {{REPO_TOKEN}} substitution (#405) happens host-side precisely so
	 * a credential never reaches a build.
	 */
	if (!e->cache_hit && recipe.is_pbs) {
		char cbs_cache_dir[PATH_MAX];

		char cbs_ws_dir[PATH_MAX];

		/*
		 * The workspace directory itself, which cbs does NOT create.
		 *
		 * cbs_workspace_prepare() makes root/src, root/build,
		 * root/dest, root/cache and root/tmp -- and not root. So
		 * `--staged /build/cbsws` against a missing /build/cbsws fails
		 * at mkdir(ENOENT) and the whole build is rejected before it
		 * starts, with no diagnostic at all: the rejection happens
		 * before cbs emits its build-begin event, so even --events
		 * reports nothing. Two build cycles were spent on that, and it
		 * is one mkdir.
		 */
		snprintf(cbs_ws_dir, sizeof(cbs_ws_dir), "%s/build/cbsws", e->build_upperdir);
		if (persist_mkdir_p(cbs_ws_dir) != 0) {
			logstore_write("cixd", "error",
			                "pkg %s@%s: could not prepare build container (create cbs workspace): %s",
			                e->name, g_chains[chain_idx].image, strerror(errno));
			pkg_fail(e, is_final_upgrade, PIPELINE_BUILD,
			         "could not prepare the build container (create cbs workspace failed)");
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
		}
		snprintf(cbs_cache_dir, sizeof(cbs_cache_dir), "%s/build/cbscache",
		         e->build_upperdir);
		if (persist_mkdir_p(cbs_cache_dir) != 0) {
			logstore_write("cixd", "error",
			                "pkg %s@%s: could not prepare build container (create cbs cache): %s",
			                e->name, g_chains[chain_idx].image, strerror(errno));
			pkg_fail(e, is_final_upgrade, PIPELINE_BUILD,
			         "could not prepare the build container (create cbs cache failed)");
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
		}
		for (i = 0; i < recipe.source_count; i++) {
			char src_path[PATH_MAX], cache_dst[PATH_MAX];

			snprintf(src_path, sizeof(src_path), "%s/%s-%s-%d.src", g_sources_dir, e->name,
			         recipe.version, i);
			snprintf(cache_dst, sizeof(cache_dst), "%s/%s", cbs_cache_dir, recipe.sha256[i]);
			if (copy_file_simple(src_path, cache_dst) != 0) {
				logstore_write("cixd", "error",
				                "pkg %s@%s: could not prepare build container (stage source %d "
				                "into the cbs cache): %s",
				                e->name, g_chains[chain_idx].image, i, strerror(errno));
				pkg_fail(e, is_final_upgrade, PIPELINE_BUILD,
				         "could not prepare the build container (stage source %d for cbs failed)",
				         i);
				g_chains[chain_idx].name[0] = '\0';
				g_chains[chain_idx].dep_queue_count = 0;
				return 0;
			}
		}
	}

	/* ADR-0122: a cache hit runs a pure no-op -- dest_dir (this
	 * container's own /build/pkg-dest) was already populated straight
	 * from the cache above, nothing left for the container itself to
	 * do. */
	if (e->cache_hit)
		snprintf(e->build_argv_cmd, sizeof(e->build_argv_cmd), ":");
	else if (recipe.is_pbs)
		/* See PKG_CBS_WORKSPACE for why there is no --output and why
		 * the cache is pre-filled. */
		snprintf(e->build_argv_cmd, sizeof(e->build_argv_cmd),
		         "set -e; cbs build /build/recipe.cbs --arch %s --staged %s --cache %s "
		         "--events human; . /build/finalize.sh",
		         pkg_host_arch(), PKG_CBS_WORKSPACE, PKG_CBS_CACHE_DIR);
	else
		/* See PKG_BUILD_CMD for why this is shaped the way it is. */
		snprintf(e->build_argv_cmd, sizeof(e->build_argv_cmd), "%s", PKG_BUILD_CMD);
	/*
	 * /usr/bin/bash, not /bin/sh -- caught empirically (ADR-0056) the
	 * first time a hostbuild job's own build_image was one of this
	 * project's OWN from-recipe images rather than the shared
	 * shared build sandbox: every from-recipe image
	 * follows this project's own no-/bin, usr/bin-only FHS convention
	 * (the exact same reasoning main.c's own retired CONSOLE_DEFAULT_CMD already
	 * documents), so "/bin/sh" -- which happened to work for years
	 * only because the toolchain sandbox is a wholesale host /usr copy
	 * with a real bin -> usr/bin symlink -- fails outright
	 * (execve: No such file or directory) the moment the lowerdir is
	 * a real, minimal Cix-built image instead. bash accepts the
	 * identical "-c <script>" invocation sh does, and every image this
	 * project has ever built (dev, base, router, ...) has it at this
	 * exact path -- confirmed directly, not assumed.
	 */
	e->build_argv[0] = "/usr/bin/bash";
	e->build_argv[1] = "-c";
	e->build_argv[2] = e->build_argv_cmd;
	e->build_argv[3] = NULL;
	if (recipe.is_pbs)
		snprintf(e->build_destdir_env, sizeof(e->build_destdir_env), "PKG_DESTDIR=%s/dest",
		         PKG_CBS_WORKSPACE);
	else
		snprintf(e->build_destdir_env, sizeof(e->build_destdir_env),
		         "PKG_DESTDIR=/build/pkg-dest");
	e->build_envp[0] = e->build_destdir_env;
	e->build_envp[1] = "PATH=/usr/bin:/bin";
	e->build_envp[2] = "HOME=/build";
	/* ADR-0159 Phase B, #412: only kernel.recipe's own pkg_build() ever
	 * looks for /build/extra/kmod-extra.config -- every other recipe
	 * simply never references it. build_envp[3] is retired (kept at
	 * NULL) now that the symbols travel as a file, not an env var. */
	e->build_envp[3] = NULL;
	if (write_kmod_extra_config(extra_dir, g_chains[chain_idx].hostbuild_extra_config_symbols) != 0) {
		logstore_write("cixd", "error",
		                "pkg %s@%s: could not prepare build container (write kmod-extra.config): %s",
		                e->name, g_chains[chain_idx].image, strerror(errno));
		pkg_fail(e, is_final_upgrade, PIPELINE_BUILD,
		         "could not prepare the build container (write kmod-extra.config failed)");
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	return start_build_container_spec(chain_idx, e, spec_out, out_stdio_write_fd,
	                                   recipe.build_caps);
}

int pkg_fetch_completed(int chain_idx, int exit_status, struct container_spec *spec_out,
                         int *out_stdio_write_fd, pid_t *out_compose_pid, int *out_compose_pidfd)
{
	/*
	 * Issue #98: the fetch path has the same chain-slot-reuse exposure
	 * the build path had, minus a container name to key on. The
	 * discriminator here is the entry's own state: an entry that is not
	 * FETCHING cannot be the one whose fetch just exited, so a late
	 * event that resolved onto a reused slot's new job is dropped
	 * instead of driving that job's package into a build it never
	 * asked for.
	 */
	struct pkg_entry *e = pkg_find(g_chains[chain_idx].name, g_chains[chain_idx].image);
	char recipe_path[PATH_MAX];
	struct pkg_recipe recipe;
	char sha_out[128];
	int is_final_upgrade;
	int i;

	*out_compose_pid = -1;
	*out_compose_pidfd = -1;

	/* The fetch child has been reaped by the caller -- nothing left to
	 * kill, and a stale pid must never be signalled (#239). */
	g_chains[chain_idx].fetch_pid = 0;

	if (e == NULL || e->state != PKG_STATE_FETCHING) {
		/* Stale: this slot has moved on (issue #98). Whether it is
		 * still HELD depends on what moved onto it -- see
		 * chain_release_if_job_over() (#254). */
		chain_release_if_job_over(chain_idx, e);
		return 0;
	}
	is_final_upgrade = g_chains[chain_idx].dep_queue_is_upgrade && (g_chains[chain_idx].dep_queue_pos + 1 >= g_chains[chain_idx].dep_queue_count);

	/*
	 * Issue #149: report an artifact checksum mismatch regardless of
	 * how the fetch went on to end. Read before the exit_status branch
	 * below precisely because it matters on SUCCESS too -- the source
	 * fallback may well have worked, and the operator still needs to
	 * know the cache is serving bytes the recipe does not approve.
	 * Left unreported, this surfaces later as whatever the fallback
	 * fails with, which on a resolver-less host is a DNS error that
	 * mentions nothing about checksums.
	 */
	{
		char note_path[PATH_MAX];
		char note[512];
		int nfd;

		fetch_note_sidecar_path(e->name, note_path, sizeof(note_path));
		nfd = open(note_path, O_RDONLY);
		if (nfd >= 0) {
			ssize_t n = read(nfd, note, sizeof(note) - 1);

			close(nfd);
			unlink(note_path);
			if (n > 0) {
				note[n] = '\0';
				logstore_write("cixd", "warn", "pkg %s@%s: %s", e->name, e->image, note);
			}
		}
	}

	if (exit_status != 0) {
		char fetch_err_path[PATH_MAX];
		char detail[512];
		int detail_len = 0;
		int fd;

		detail[0] = '\0';
		fetch_error_sidecar_path(e->name, fetch_err_path, sizeof(fetch_err_path));
		fd = open(fetch_err_path, O_RDONLY);
		if (fd >= 0) {
			ssize_t n = read(fd, detail, sizeof(detail) - 1);

			close(fd);
			unlink(fetch_err_path);
			if (n > 0) {
				detail_len = (int)n;
				/* curl's own error text always ends in its own
				 * newline; strip trailing whitespace so it reads
				 * naturally packed into e->error/the log line
				 * below rather than leaving a dangling blank line. */
				while (detail_len > 0 && (detail[detail_len - 1] == '\n' ||
				                           detail[detail_len - 1] == '\r'))
					detail_len--;
				detail[detail_len] = '\0';
			}
		}

		/*
		 * A cancelled fetch is not a failed one, and must not be
		 * reported as one (#239).
		 *
		 * pkg_cancel() ends a stuck fetch by killing its child, so the
		 * exit status that arrives here is a real nonzero -- which,
		 * checked in the ordinary order, records "fetch failed (curl
		 * exit status -1)" with failure_kind "fetch". An operator then
		 * cannot tell the network breaking from their own cancel
		 * taking effect, and the contract promises "cancelled".
		 *
		 * Checked first, before any of the curl-shaped explanations
		 * below, because the operator's intent is the true cause here
		 * and curl's exit status is only its consequence.
		 */
		if (e->cancel_requested) {
			e->cancel_requested = 0;
			pkg_fail_cancelled(e, is_final_upgrade, PIPELINE_BUILD,
			         "fetch cancelled by operator");
			logstore_write("cixd", "info", "pkg %s@%s: fetch cancelled by operator", e->name,
			               e->image);
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
		}

		if (exit_status == PKG_FETCH_EXIT_PRECONDITION && detail_len > 0)
			/* Never reached curl -- a precondition failed, and the
			 * sidecar says which. */
			pkg_fail(e, is_final_upgrade, PIPELINE_FETCH, "%s", detail);
		else if (detail_len > 0)
			pkg_fail(e, is_final_upgrade, PIPELINE_FETCH,
			         "fetch failed (curl exit status %d): %s", exit_status, detail);
		else
			pkg_fail(e, is_final_upgrade, PIPELINE_FETCH, "fetch failed (curl exit status %d)",
			         exit_status);
		logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	if (find_recipe_path(e->name, current_fetch_effective_version(chain_idx), recipe_path,
	                      sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0) {
		pkg_fail(e, is_final_upgrade, PIPELINE_AUTHOR, "recipe became unreadable mid-install");
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	/*
	 * ADR-0122: start_fetch_for()'s child may have left a checksum-
	 * verified artifact tarball sitting at the sentinel path instead
	 * of ever fetching recipe.source[] at all -- promote it into the
	 * real local cache right here and fold this job into the exact
	 * same cache-hit path a local cache hit already takes (set fresh
	 * by start_fetch_for() itself; this is the only other way it ever
	 * becomes true).
	 */
	if (!e->cache_hit) {
		char artifact_sentinel[PATH_MAX];
		struct stat st;

		artifact_sentinel_path(e->name, recipe.version, artifact_sentinel,
		                        sizeof(artifact_sentinel));
		if (stat(artifact_sentinel, &st) == 0 && S_ISREG(st.st_mode)) {
			pkg_cache_save_from_file(e->name, recipe.version, artifact_sentinel);
			e->cache_hit = 1;
		}
	}

	/*
	 * recipe.version, not e->version -- during an in-place upgrade
	 * e->version is deliberately still the OLD version until this job
	 * actually succeeds; each source on disk was fetched under the
	 * NEW version's name by start_fetch_for(). Every fetched source
	 * (the main one at index 0, and any extras) is verified against
	 * its own sha256 before any of them are touched further -- one
	 * bad entry fails the whole job, matching the single-source
	 * case's own existing all-or-nothing guarantee (ADR-0036). Skipped
	 * entirely for a cache/artifact hit -- nothing was fetched from
	 * recipe.source[] at all in that case.
	 */
	if (!e->cache_hit) {
		for (i = 0; i < recipe.source_count; i++) {
			char src_path[PATH_MAX];
			struct stat sst;
			const char *why = NULL;

			snprintf(src_path, sizeof(src_path), "%s/%s-%s-%d.src", g_sources_dir, e->name,
			         recipe.version, i);
			/*
			 * #332: this reported only that two hashes differed, and
			 * threw away every fact that would say WHY.
			 *
			 * Worse, it collapsed two unrelated causes into one
			 * sentence: pkg_run_capture_sha256() failing (the file is
			 * absent, unreadable, or empty -- nothing was ever
			 * fetched to this path) read as "checksum mismatch",
			 * which sends the reader to look at the recipe's sha256
			 * and at upstream. Both are then found to be correct,
			 * because neither was ever involved. That is exactly the
			 * shape of #380, where a dependency failed the source
			 * checksum while a standalone recipe fetched the SAME url
			 * against the SAME sha and built.
			 *
			 * The three facts that separate the real causes are the
			 * byte count, the computed hash and the path -- a short
			 * file is a truncated transfer, a full-size file with the
			 * wrong hash is a substituted body, and an absent file is
			 * a fetch that never wrote here at all. None survived.
			 *
			 * They go to the log store, which has room for two full
			 * hashes and a path; e->error is 256 bytes and carries a
			 * summary with enough of each hash to tell them apart.
			 */
			if (stat(src_path, &sst) != 0 || !S_ISREG(sst.st_mode)) {
				logstore_write("cixd", "error",
				                "pkg %s@%s: source %d was never written -- %s is absent (%s). "
				                "The recipe's sha256 is not involved in this failure; the "
				                "fetch did not put a file at the path the check reads.",
				                e->name, recipe.version, i, src_path, strerror(errno));
				pkg_fail(e, is_final_upgrade, PIPELINE_FETCH,
				         "source %d was never written to %s -- no bytes arrived, so this is a "
				         "fetch failure and not a checksum one",
				         i, src_path);
				g_chains[chain_idx].name[0] = '\0';
				g_chains[chain_idx].dep_queue_count = 0;
				return 0;
			}
			/* Cleared first: a failed capture leaves sha_out untouched,
			 * and a stale value from the previous source would be
			 * reported as this one's computed hash. */
			sha_out[0] = '\0';
			if (pkg_run_capture_sha256(src_path, sha_out, sizeof(sha_out)) != 0)
				why = "could not be hashed";
			else if (strcasecmp(sha_out, recipe.sha256[i]) != 0)
				why = "hashes to the wrong value";
			if (why != NULL) {
				/* #461: recipe.source[i] carries the token parse_recipe()
				 * substituted for {{REPO_TOKEN}}, so logging it verbatim
				 * writes a live repo credential into the log store
				 * (measured on 192.168.15.95, 2026-09-13, in a cix
				 * fetch-mismatch line). Redact it exactly as the
				 * recipe-serving path does -- the token value becomes the
				 * {{REPO_TOKEN}} placeholder again; the host and path, the
				 * useful diagnostic, stay. */
				char url_redacted[PKG_URL_MAX];

				snprintf(url_redacted, sizeof(url_redacted), "%s",
				         recipe.source[i][0] != '\0' ? recipe.source[i] : "(none)");
				redact_repo_token(url_redacted, sizeof(url_redacted));
				logstore_write("cixd", "error",
				                "pkg %s@%s: source %d %s. path=%s bytes=%lld computed=%s "
				                "declared=%s url=%s",
				                e->name, recipe.version, i, why, src_path,
				                (long long)sst.st_size,
				                sha_out[0] != '\0' ? sha_out : "(none)", recipe.sha256[i],
				                url_redacted);
				pkg_fail(e, is_final_upgrade, PIPELINE_FETCH,
				         "checksum mismatch (source %d): %lld bytes hash to %.12s..., recipe "
				         "declares %.12s... -- full hashes, path and url in the log store",
				         i, (long long)sst.st_size,
				         sha_out[0] != '\0' ? sha_out : "(unreadable)", recipe.sha256[i]);
				g_chains[chain_idx].name[0] = '\0';
				g_chains[chain_idx].dep_queue_count = 0;
				return 0;
			}
		}
	}

	/*
	 * Issue #213: a cancel that arrived before this build had a
	 * container.
	 *
	 * Between the fetch finishing and the container being spawned the
	 * entry is already PKG_STATE_BUILDING -- composing a build
	 * environment (ADR-0199) happens in that window and is not quick --
	 * so pkg_cancel() can legitimately be called with nothing yet to
	 * kill. It sets the flag and returns; this is where the flag has to
	 * be honoured, because otherwise the build would go on to spawn,
	 * succeed, and the cancel would have silently done nothing.
	 *
	 * Checked before the container name is claimed (issue #98's
	 * invariant) so a cancelled build never takes ownership of a slot
	 * name it is not going to use.
	 */
	if (e->cancel_requested) {
		e->cancel_requested = 0;
		pkg_fail_cancelled(e, is_final_upgrade, PIPELINE_BUILD,
		         "build cancelled by operator before it started");
		logstore_write("cixd", "info",
		               "pkg %s@%s: cancelled before its build container was spawned",
		               e->name, e->image);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	pkg_build_container_name(chain_idx, e->build_container_name, sizeof(e->build_container_name));
	/*
	 * Issue #98: exactly one entry may claim a given build container
	 * name at a time. Build container names are per chain SLOT
	 * ("__pkgbuild-0"), and slots are reused constantly, so without
	 * this every package that ever built in slot 0 keeps claiming
	 * "__pkgbuild-0" forever -- and anything resolving an exited
	 * container back to its owner finds the oldest claimant instead of
	 * the real one. Establishing the invariant here, at the moment the
	 * claim is made, is what makes that resolution exact.
	 */
	{
		int oi;

		for (oi = 0; oi < PKG_MAX_PACKAGES; oi++) {
			if (&g_packages[oi] == e || !g_packages[oi].in_use)
				continue;
			if (strcmp(g_packages[oi].build_container_name, e->build_container_name) == 0)
				g_packages[oi].build_container_name[0] = '\0';
		}
	}
	return pkg_prepare_build_and_start(chain_idx, e, &recipe, is_final_upgrade, recipe_path,
	                                    spec_out, out_stdio_write_fd, out_compose_pid,
	                                    out_compose_pidfd);
}

/*
 * Continues an install whose build environment was being composed in a
 * forked child (#238), once that child has exited.
 *
 * Success needs no handover at all: image state lives on disk, so this
 * simply re-enters pkg_prepare_build_and_start(), which asks
 * buildenv_image_for() again and gets the already-composed fast path.
 * The recipe and the upgrade flag are re-derived from the chain rather
 * than carried across, for the same reason -- there is no second copy
 * of them to go stale.
 *
 * Returns what pkg_fetch_completed() returns: 1 to start the build in
 * spec_out, 0 if the install is over, 2 if another composition was
 * forked (which cannot normally happen, since a successful child leaves
 * the environment ready, but is handled rather than assumed away).
 */
int pkg_buildenv_completed(int chain_idx, int exit_status, struct container_spec *spec_out,
                            int *out_stdio_write_fd, pid_t *out_compose_pid,
                            int *out_compose_pidfd)
{
	struct pkg_entry *e = pkg_find(g_chains[chain_idx].name, g_chains[chain_idx].image);
	char recipe_path[PATH_MAX];
	struct pkg_recipe recipe;
	int is_final_upgrade;

	*out_compose_pid = -1;
	*out_compose_pidfd = -1;

	/* Same stale-slot discriminator pkg_fetch_completed() uses (issue
	 * #98): an entry that is no longer FETCHING cannot be the one whose
	 * composer just exited. */
	if (e == NULL || e->state != PKG_STATE_FETCHING) {
		chain_release_if_job_over(chain_idx, e);
		return 0;
	}

	is_final_upgrade = g_chains[chain_idx].dep_queue_is_upgrade &&
	                   (g_chains[chain_idx].dep_queue_pos + 1 >= g_chains[chain_idx].dep_queue_count);

	if (exit_status != 0) {
		/* Remove the created-but-empty image the failed composition
		 * left, or every later attempt re-forks against it forever. */
		if (g_chains[chain_idx].buildenv_image[0] != '\0')
			buildenv_compose_failed_cleanup(g_chains[chain_idx].buildenv_image);
		g_chains[chain_idx].buildenv_image[0] = '\0';
		pkg_fail(e, is_final_upgrade, PIPELINE_BUILD,
		         "could not compose a build environment from the declared tools");
		logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	if (find_recipe_path(e->name, current_fetch_effective_version(chain_idx), recipe_path,
	                      sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0) {
		pkg_fail(e, is_final_upgrade, PIPELINE_BUILD,
		         "the recipe became unreadable while its build environment was composed");
		logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	return pkg_prepare_build_and_start(chain_idx, e, &recipe, is_final_upgrade, recipe_path,
	                                    spec_out, out_stdio_write_fd, out_compose_pid,
	                                    out_compose_pidfd);
}

/*
 * ADR-0177/issue #46: resumes a build container pkg_build_completed()
 * preserved after a failure (ADR-0175's keep_on_failure) instead of
 * paying for a full fetch+extract+build restart -- the real, repeated
 * cost this exists to eliminate (a multi-hour gcc.recipe bootstrap
 * attempt, restarted from zero after every single fixup, is what
 * motivated filing this). Reuses the kept container's own lowerdir/
 * upperdir/workdir/merged paths completely unchanged (still sitting in
 * e->build_lowerdir/build_upperdir/build_workdir/build_merged exactly
 * as pkg_fetch_completed() last computed them -- registry_remove()
 * never touches disk, confirmed directly in registry.c, so nothing
 * here was ever at risk of being wiped); only the recipe.sh staged
 * inside the existing upperdir is replaced with whatever recipe
 * version the caller requests now, and only the previous attempt's own
 * partial install destination (build/pkg-dest) is reset -- build/src
 * (the already-extracted, already-partially-built source tree) is
 * deliberately left completely alone, since re-extracting it from
 * scratch is exactly the cost this feature exists to avoid.
 *
 * Requires (name, image) to resolve to a real PKG_STATE_FAILED entry
 * with a non-empty kept_build_container (PKG_ERR_NOT_FOUND otherwise --
 * nothing to resume). The chain slot reused is whichever index the
 * kept container's own name encodes (pkg_build_container_chain_index()),
 * NEVER a freshly chain_alloc()'d one -- pkg_build_completed() looks up
 * g_chains[chain_idx] by the exit event's own chain_idx, so reusing any
 * other slot would silently corrupt an unrelated chain's bookkeeping.
 * PKG_ERR_BUSY if that specific slot has since been reclaimed by a
 * different job (chain_alloc() scans from 0, and this slot went idle
 * the moment the original build failed -- see pkg_build_completed()'s
 * own "g_chains[chain_idx].name[0] = '\0';" -- so a same-slot reuse by
 * an unrelated job in between is a real, if narrow, possibility, not
 * just theoretical); the caller can retry once whatever currently holds
 * it finishes, same as any other busy condition.
 *
 * Deliberately does NOT touch the registry itself (registry_remove()/
 * registry_create()) -- pkg.c has never linked against registry.h
 * (main.c alone owns every registry_create() call following a
 * pkg_fetch_completed() success, see handle_pkg_fetch_event()), and
 * this keeps that same layering intact rather than adding a new
 * dependency edge just for resume. The caller (main.c's own new REST
 * handler) is responsible for calling registry_remove() on the exact
 * same container name (derived via pkg_build_container_name(chain_idx,
 * ...), guaranteed identical to the original since chain_idx was parsed
 * back out of that same name above) before its own registry_create()
 * call using spec_out -- mirroring handle_pkg_fetch_event()'s existing
 * sequence exactly, with one extra registry_remove() first since this
 * name is a reuse, not a fresh one.
 */
enum pkg_error pkg_resume_build(const char *name, const char *image, const char *version,
                                 const char *extra_config_symbols, int keep_on_failure,
                                 struct container_spec *spec_out, int *out_chain_idx,
                                 int *out_stdio_write_fd)
{
	struct pkg_recipe recipe;
	char recipe_path[PATH_MAX];
	struct pkg_entry *e;
	char dest_dir[PATH_MAX], recipe_dst[PATH_MAX], extra_dir[PATH_MAX];
	int chain_idx;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	if (extra_config_symbols != NULL &&
	    strlen(extra_config_symbols) >= PKG_HOSTBUILD_EXTRA_SYMBOLS_MAX)
		return PKG_ERR_INVALID_NAME;

	e = pkg_find(name, image);
	if (e == NULL || e->state != PKG_STATE_FAILED || e->kept_build_container[0] == '\0')
		return PKG_ERR_NOT_FOUND;

	chain_idx = pkg_build_container_chain_index(e->kept_build_container);
	/* Unreachable in practice -- kept_build_container is only ever
	 * written by this module's own pkg_build_container_name() output
	 * (pkg_build_completed()), which this same parser always accepts. */
	if (chain_idx < 0)
		return PKG_ERR_NOT_FOUND;
	if (g_chains[chain_idx].name[0] != '\0')
		return PKG_ERR_BUSY;

	if (find_recipe_path(name, version, recipe_path, sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0)
		return PKG_ERR_INVALID_RECIPE;

	snprintf(recipe_dst, sizeof(recipe_dst), "%s/build/recipe.sh", e->build_upperdir);
	snprintf(dest_dir, sizeof(dest_dir), "%s/build/pkg-dest", e->build_upperdir);
	snprintf(extra_dir, sizeof(extra_dir), "%s/build/extra", e->build_upperdir);

	if (copy_file_simple(recipe_path, recipe_dst) != 0)
		return PKG_ERR_PERSIST_FAILED;
	/* ADR-0251: staged wherever recipe.sh is, so the two cannot
	 * disagree about whether the policy ran. */
	if (write_finalize_script(e->build_upperdir) != 0)
		return PKG_ERR_PERSIST_FAILED;
	if (cix_btrfs_subvol_delete_or_rmtree(dest_dir) != 0)
		return PKG_ERR_PERSIST_FAILED;
	if (persist_mkdir_p(dest_dir) != 0)
		return PKG_ERR_PERSIST_FAILED;

	e->kept_build_container[0] = '\0';

	/*
	 * #326: this reuses a chain slot by index and never goes through
	 * start_fetch_for(), so the slot's captured version is whatever
	 * the last job to use it left there -- empty, or another
	 * package's. pkg_build_completed() reads it to decide what this
	 * entry installed, so a resume has to set it from its own parsed
	 * recipe, which is the same recipe the resumed build will run.
	 * Without this the fix for #326 would be a worse bug than the one
	 * it replaced: a confidently-written wrong version rather than a
	 * missing one.
	 */
	snprintf(g_chains[chain_idx].fetch_resolved_version,
	         sizeof(g_chains[chain_idx].fetch_resolved_version), "%s", recipe.version);
	snprintf(g_chains[chain_idx].fetch_resolved_depends,
	         sizeof(g_chains[chain_idx].fetch_resolved_depends), "%s", recipe.depends);

	snprintf(e->build_argv_cmd, sizeof(e->build_argv_cmd), "%s", PKG_BUILD_CMD);
	e->build_argv[0] = "/usr/bin/bash";
	e->build_argv[1] = "-c";
	e->build_argv[2] = e->build_argv_cmd;
	e->build_argv[3] = NULL;
	e->build_envp[0] = "PKG_DESTDIR=/build/pkg-dest";
	e->build_envp[1] = "PATH=/usr/bin:/bin";
	e->build_envp[2] = "HOME=/build";
	e->build_envp[3] = NULL;
	if (write_kmod_extra_config(extra_dir, extra_config_symbols) != 0)
		return PKG_ERR_PERSIST_FAILED;

	g_chains[chain_idx].is_hostbuild = (strcmp(e->image, PKG_HOSTBUILD_IMAGE) == 0);
	snprintf(g_chains[chain_idx].target_version, sizeof(g_chains[chain_idx].target_version), "%s",
	         (version != NULL) ? version : "");
	snprintf(g_chains[chain_idx].hostbuild_extra_config_symbols,
	         sizeof(g_chains[chain_idx].hostbuild_extra_config_symbols), "%s",
	         (extra_config_symbols != NULL) ? extra_config_symbols : "");
	g_chains[chain_idx].keep_on_failure = keep_on_failure;
	strncpy(g_chains[chain_idx].dep_queue[0], name, PKG_NAME_MAX - 1);
	g_chains[chain_idx].dep_queue[0][PKG_NAME_MAX - 1] = '\0';
	g_chains[chain_idx].dep_queue_count = 1;
	g_chains[chain_idx].dep_queue_pos = 0;
	g_chains[chain_idx].dep_queue_is_upgrade = 0;
	snprintf(g_chains[chain_idx].image, sizeof(g_chains[chain_idx].image), "%s", e->image);
	/* Set last -- this is what marks the slot busy (chain_alloc()/
	 * pkg_any_job_busy() both key off name[0]), so nothing above can
	 * partially populate a slot another job's chain_alloc() might
	 * concurrently claim (this daemon is single-threaded/event-loop
	 * driven, but keeping the same ordering discipline every other
	 * *_start() function already follows costs nothing and avoids a
	 * silent trap for a future refactor). */
	snprintf(g_chains[chain_idx].name, sizeof(g_chains[chain_idx].name), "%s", name);

	*out_chain_idx = chain_idx;
	/* Resume path (ADR-0177): the recipe is not re-parsed here, and a
	 * resumed build reuses the container it already has, so it needs no
	 * capability request of its own. */
	start_build_container_spec(chain_idx, e, spec_out, out_stdio_write_fd, NULL);
	return PKG_OK;
}

void pkg_build_spawn_failed(int chain_idx)
{
	struct pkg_entry *e = pkg_find(g_chains[chain_idx].name, g_chains[chain_idx].image);

	if (e != NULL) {
		int is_final_upgrade = g_chains[chain_idx].dep_queue_is_upgrade && (g_chains[chain_idx].dep_queue_pos + 1 >= g_chains[chain_idx].dep_queue_count);

		pkg_fail(e, is_final_upgrade, PIPELINE_BUILD, "could not start the build container");
	}
	g_chains[chain_idx].name[0] = '\0';
	g_chains[chain_idx].dep_queue_count = 0;
	/*
	 * registry_create() never spawned a container, so no epoll
	 * registration for its build_output_rd exists anywhere to drive
	 * its own EOF-triggered close (see register_pkg_build_output()/
	 * handle_pkg_build_output_event() in main.c) -- this is the one
	 * path that has to close it directly.
	 */
	pkg_build_output_close(chain_idx);
}


/*
 * Issue #57: the persisted build logs, newest first. Deliberately a
 * directory listing rather than an index file -- the files ARE the
 * state, so nothing can drift out of step with them, and a log that
 * was copied off the box by hand still shows up.
 */
int pkg_build_log_list(struct pkg_build_log_entry *out, int max)
{
	char dir[PATH_MAX];
	struct dirent *ent;
	DIR *d;
	int count = 0, i;

	pkg_build_log_dir(dir, sizeof(dir));
	d = opendir(dir);
	if (d == NULL)
		return 0;
	while ((ent = readdir(d)) != NULL && count < max) {
		char path[PATH_MAX];
		struct stat st;

		if (ent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		snprintf(out[count].file, sizeof(out[count].file), "%s", ent->d_name);
		out[count].size_bytes = (long long)st.st_size;
		out[count].modified_at = (long long)st.st_mtime;
		count++;
	}
	closedir(d);

	/* Newest first: the log anyone wants is nearly always the last one. */
	for (i = 1; i < count; i++) {
		struct pkg_build_log_entry tmp = out[i];
		int k = i;

		while (k > 0 && out[k - 1].modified_at < tmp.modified_at) {
			out[k] = out[k - 1];
			k--;
		}
		out[k] = tmp;
	}
	return count;
}

/*
 * Reads one build log into a caller-provided buffer, TAIL-first when it
 * does not fit: a caller asking for the last 100 KB of a 40 MB build
 * wants the end, where the failure is. Returns the number of bytes
 * placed in buf, or -1 if there is no such log.
 *
 * The filename is validated rather than trusted: it is a path component
 * from an HTTP request, and this function opens a file with it.
 */
long long pkg_build_log_read(const char *file, char *buf, long long cap, long long *out_total)
{
	char dir[PATH_MAX], path[PATH_MAX];
	struct stat st;
	int fd;
	long long total, offset;
	ssize_t n;

	if (file == NULL || file[0] == '\0' || strchr(file, '/') != NULL || strstr(file, "..") != NULL)
		return -1;
	pkg_build_log_dir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/%s", dir, file);
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return -1;
	total = (long long)st.st_size;
	if (out_total != NULL)
		*out_total = total;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	offset = total > cap ? total - cap : 0;
	if (offset > 0 && lseek(fd, offset, SEEK_SET) < 0) {
		close(fd);
		return -1;
	}
	n = read(fd, buf, (size_t)(total - offset > cap ? cap : total - offset));
	close(fd);
	return n < 0 ? -1 : (long long)n;
}

int pkg_build_output_fd(int chain_idx)
{
	struct pkg_entry *e = g_build_output_entries[chain_idx];

	return e != NULL ? e->build_output_rd : -1;
}


/*
 * Issue #57: where every build's complete output is kept, and how many
 * are kept. A bounded directory rather than an unbounded one -- build
 * logs are diagnostic, and a box that fills its own disk with them has
 * traded one failure for a worse one.
 */
#define PKG_BUILD_LOG_KEEP 40

static void pkg_build_log_dir(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/build-logs", g_pkg_dir);
}

/* ---- ADR-0273: gates and approvals -------------------------------- */

static int *gate_flag(const char *gate)
{
	if (gate == NULL)
		return NULL;
	if (strcmp(gate, PKG_GATE_PUBLISH) == 0)
		return &g_gate_publish;
	if (strcmp(gate, PKG_GATE_ROLL) == 0)
		return &g_gate_roll;
	if (strcmp(gate, PKG_GATE_DEPLOY) == 0)
		return &g_gate_deploy;
	return NULL;
}

int pkg_gate_enabled(const char *gate)
{
	const int *f = gate_flag(gate);

	return f != NULL && *f;
}

int pkg_gate_set(const char *gate, int on)
{
	int *f = gate_flag(gate);

	if (f == NULL)
		return -1;
	*f = on ? 1 : 0;
	/*
	 * Persisted here, not merely rendered. pkg_runs_render() has always
	 * written the gates; nothing called this. So a gate turned on stayed
	 * on until the next restart and then silently reverted, releasing
	 * exactly the change someone had decided to stop -- and reporting
	 * nothing. test_stallwatch's restart assertion caught it.
	 */
	pkg_runs_save();
	return 0;
}

static int approval_index(const char *gate, const char *target)
{
	int i;

	for (i = 0; i < g_approval_count; i++) {
		if (strcmp(g_approvals[i].gate, gate) == 0 &&
		    strcmp(g_approvals[i].target, target) == 0)
			return i;
	}
	return -1;
}

static void approval_drop_at(int idx)
{
	int i;

	if (idx < 0 || idx >= g_approval_count)
		return;
	for (i = idx + 1; i < g_approval_count; i++)
		g_approvals[i - 1] = g_approvals[i];
	g_approval_count--;
}

/*
 * True when this gate is on AND nothing has approved this target yet --
 * i.e. the drain must leave it alone. A gate that is off holds nothing,
 * which is what makes "default off" a genuine no-op rather than a
 * cheaper code path.
 */
static int gate_holds(const char *gate, const char *target)
{
	return pkg_gate_enabled(gate) && approval_index(gate, target) < 0;
}

/*
 * Consumes the approval for a target the drain is about to act on.
 * Called at the moment the thing actually goes through, never at the
 * moment it is checked -- a drain may look at the same queue entry many
 * times before a slot frees, and burning the grant on a look would mean
 * an operator approved something that then silently needed approving
 * again.
 */
/*
 * Drops a grant for something that is no longer queued, without an
 * audit line: nothing went through, so there is nothing to attribute.
 * Called from rebuild_queue_remove_at(), so every path that takes an
 * image out of the queue also forgets its approval -- otherwise grants
 * for abandoned work accumulate until PKG_APPROVAL_MAX and start
 * refusing real ones.
 */
static void approval_forget(const char *gate, const char *target)
{
	int i = approval_index(gate, target);

	if (i < 0)
		return;
	approval_drop_at(i);
	pkg_runs_save();
}

static void approval_consume(const char *gate, const char *target)
{
	int i = approval_index(gate, target);

	if (i < 0)
		return;
	logstore_write("audit", "info", "gate %s: %s went through, approved by %s", gate, target,
	                g_approvals[i].who);
	approval_drop_at(i);
	pkg_runs_save();
}

/* ---- ADR-0272: the run store ------------------------------------- */

static void pkg_runs_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/runs.json", g_pkg_dir);
}

/*
 * Trims to the configured retention, oldest-first. Called after every
 * append, so the cap is enforced at the moment it would be exceeded
 * rather than by a sweep nobody scheduled -- the same discipline
 * pkg_build_log_prune() already follows for the logs.
 */
static void pkg_runs_trim(void)
{
	int drop;

	if (g_runs == NULL || g_run_count <= g_run_keep)
		return;
	drop = g_run_count - g_run_keep;
	memmove(&g_runs[0], &g_runs[drop], (size_t)(g_run_count - drop) * sizeof(g_runs[0]));
	g_run_count -= drop;
}

/*
 * The on-disk form is an object, not a bare array, so the retention an
 * operator chose survives a restart alongside the runs it governs. A
 * setting that silently reverts on the next boot is worse than one that
 * cannot be changed at all: nothing reports the reversion, and the
 * store quietly grows or shrinks back to a default nobody asked for.
 */
static void pkg_runs_render(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "retention");
	jw_int(w, g_run_keep);
	/*
	 * ADR-0273: the gates and the approvals live here too, so an item
	 * held for a person survives a restart. A gate that reverted to off
	 * on the next boot would quietly release exactly the change someone
	 * had decided to stop.
	 */
	jw_key(w, "gates");
	jw_obj_open(w);
	jw_key(w, "publish");
	jw_bool(w, g_gate_publish);
	jw_key(w, "roll");
	jw_bool(w, g_gate_roll);
	jw_key(w, "deploy");
	jw_bool(w, g_gate_deploy);
	jw_obj_close(w);
	jw_key(w, "approvals");
	jw_arr_open(w);
	for (i = 0; i < g_approval_count; i++) {
		jw_obj_open(w);
		jw_key(w, "gate");
		jw_str(w, g_approvals[i].gate);
		jw_key(w, "target");
		jw_str(w, g_approvals[i].target);
		jw_key(w, "who");
		jw_str(w, g_approvals[i].who);
		jw_key(w, "at");
		jw_int(w, (long long)g_approvals[i].at);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_key(w, "runs");
	jw_arr_open(w);
	for (i = 0; i < g_run_count; i++) {
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, g_runs[i].name);
		jw_key(w, "image");
		jw_str(w, g_runs[i].image);
		jw_key(w, "version");
		jw_str(w, g_runs[i].version);
		jw_key(w, "trigger");
		jw_str(w, g_runs[i].trigger);
		jw_key(w, "started_at");
		jw_int(w, (long long)g_runs[i].started_at);
		jw_key(w, "ended_at");
		jw_int(w, (long long)g_runs[i].ended_at);
		jw_key(w, "stage");
		jw_str(w, pipeline_stage_name(g_runs[i].stage));
		jw_key(w, "status");
		jw_str(w, pipeline_status_name(g_runs[i].status));
		jw_key(w, "log");
		jw_str(w, g_runs[i].log);
		jw_key(w, "error");
		jw_str(w, g_runs[i].error);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}

static void pkg_runs_save(void)
{
	struct json_writer w;
	char path[PATH_MAX];

	/*
	 * Deliberately NOT guarded on g_runs: a host that has set a
	 * retention and not yet finished a single run still has something
	 * worth writing, and the render loop below is bounded by
	 * g_run_count, which is zero in exactly that case.
	 */
	if (g_pkg_dir[0] == '\0')
		return;
	/* Same defensiveness as pkg_build_log_open()'s own mkdir, and for
	 * the same reason: persist_atomic_write() into a directory that is
	 * not there fails silently, and the symptom would surface much
	 * later as "the retention did not survive a restart" -- pointing at
	 * the load path, which would be innocent. */
	(void)persist_mkdir_p(g_pkg_dir);
	pkg_runs_path(path, sizeof(path));
	jw_init(&w);
	pkg_runs_render(&w);
	if (w.buf != NULL)
		(void)persist_atomic_write(path, w.buf, w.len);
	jw_free(&w);
}

static int pkg_runs_alloc(void)
{
	if (g_runs != NULL)
		return 0;
	g_runs = calloc(PKG_RUN_MAX, sizeof(*g_runs));
	return g_runs != NULL ? 0 : -1;
}

/*
 * Closes the open run on this entry, if it has one.
 *
 * The run_started_at guard is what makes this callable from both the
 * failure funnel and the success tail without a job ever being recorded
 * twice: a provisional success that is then reverted by pkg_fail()
 * reaches pkg_record_outcome() with the run already closed, and the
 * record that survives is the one made by whichever of them ran first
 * -- which is the success tail only when nothing reverted it.
 */
static void pkg_run_close(struct pkg_entry *e)
{
	struct pkg_run *r;
	const char *base;

	if (e == NULL || e->run_started_at == 0)
		return;
	if (pkg_runs_alloc() != 0) {
		e->run_started_at = 0;
		return;
	}
	if (g_run_count >= PKG_RUN_MAX) {
		memmove(&g_runs[0], &g_runs[1], (size_t)(PKG_RUN_MAX - 1) * sizeof(g_runs[0]));
		g_run_count = PKG_RUN_MAX - 1;
	}
	r = &g_runs[g_run_count++];
	memset(r, 0, sizeof(*r));
	snprintf(r->name, sizeof(r->name), "%s", e->name);
	snprintf(r->image, sizeof(r->image), "%s", e->image);
	snprintf(r->version, sizeof(r->version), "%s", e->version);
	snprintf(r->trigger, sizeof(r->trigger), "%s",
	         e->run_trigger[0] != '\0' ? e->run_trigger : PKG_RUN_TRIGGER_REQUEST);
	base = strrchr(e->build_log_path, '/');
	snprintf(r->log, sizeof(r->log), "%s",
	         base != NULL ? base + 1 : e->build_log_path);
	snprintf(r->error, sizeof(r->error), "%s", e->error);
	r->started_at = e->run_started_at;
	r->ended_at = time(NULL);
	r->stage = e->stage;
	/*
	 * A run's status is the entry's, with one substitution: an entry
	 * whose status is PIPELINE_OK but whose state is FAILED cannot
	 * happen through pkg_record_outcome(), and if it ever does the run
	 * should say failed rather than quietly reporting a failure as a
	 * success it can no longer distinguish.
	 */
	r->status = (e->state == PKG_STATE_FAILED && e->status == PIPELINE_OK) ? PIPELINE_FAILED
	                                                                       : e->status;
	e->run_started_at = 0;
	e->run_trigger[0] = '\0';
	pkg_runs_trim();
	pkg_runs_save();
}

/*
 * #375: close every run that is still open, because the daemon is
 * stopping and they will not finish.
 *
 * A build in flight is live state -- ADR-0272 is explicit that the run
 * store holds no run whose outcome is unknown, and that writing a
 * half-record and amending it later is the mutable second copy that ADR
 * exists to prevent. So this is not a half-record: the daemon stopping
 * IS the outcome, and it is written once, at the moment it becomes
 * true, and never touched again. That is the same contract every other
 * run record has.
 *
 * PIPELINE_FAILED with a real reason rather than PIPELINE_CANCELLED,
 * which would say somebody asked for this. Nobody did.
 *
 * This covers a clean stop: a reboot, an update, a SIGTERM -- which is
 * how this daemon almost always goes down, deploys included. It does
 * NOT cover a panic or a power cut, where nothing can be written at the
 * time. Closing that hole needs durable knowledge that a run was open,
 * and the obvious place to put it is the run store, which ADR-0272
 * forbids. Stated here rather than half-solved; see #375.
 */
void pkg_runs_close_open_at_shutdown(void)
{
	int i, closed = 0;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		struct pkg_entry *e = &g_packages[i];

		if (!e->in_use || e->run_started_at == 0)
			continue;
		if (e->error[0] == '\0')
			snprintf(e->error, sizeof(e->error),
			         "the daemon stopped while this was %s",
			         e->state == PKG_STATE_FETCHING ? "fetching" : "building");
		e->state = PKG_STATE_FAILED;
		e->status = PIPELINE_FAILED;
		pkg_run_close(e);
		closed++;
	}
	if (closed > 0)
		logstore_write("cixd", "info",
		                "pkg: closed %d in-flight run(s) as failed -- the daemon is stopping "
		                "and they will not finish (#375)",
		                closed);
}

static void pkg_runs_load(void)
{
	char path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;
	const struct json_value *runs, *keep;
	size_t i;

	if (g_pkg_dir[0] == '\0')
		return;
	pkg_runs_path(path, sizeof(path));
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return;
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		return;
	}
	keep = json_object_get(root, "retention");
	if (keep != NULL && keep->type == JSON_NUMBER) {
		int k = (int)json_as_number(keep);

		if (k >= 1 && k <= PKG_RUN_MAX)
			g_run_keep = k;
	}
	/* ADR-0273. Absent keys mean "off" and "none", which is what an
	 * older file legitimately says. */
	{
		const struct json_value *g = json_object_get(root, "gates");
		const struct json_value *ap = json_object_get(root, "approvals");
		size_t k2;

		if (g != NULL && g->type == JSON_OBJECT) {
			const struct json_value *v2;

			v2 = json_object_get(g, "publish");
			g_gate_publish = v2 != NULL && v2->type == JSON_BOOL && v2->u.boolean;
			v2 = json_object_get(g, "roll");
			g_gate_roll = v2 != NULL && v2->type == JSON_BOOL && v2->u.boolean;
			v2 = json_object_get(g, "deploy");
			g_gate_deploy = v2 != NULL && v2->type == JSON_BOOL && v2->u.boolean;
		}
		g_approval_count = 0;
		if (ap != NULL && ap->type == JSON_ARRAY) {
			for (k2 = 0; k2 < ap->u.array.count && g_approval_count < PKG_APPROVAL_MAX; k2++) {
				const struct json_value *o2 = ap->u.array.items[k2];
				struct pkg_approval *a = &g_approvals[g_approval_count];
				const char *sv2;

				if (o2 == NULL || o2->type != JSON_OBJECT)
					continue;
				memset(a, 0, sizeof(*a));
				sv2 = json_as_string(json_object_get(o2, "gate"));
				if (sv2 == NULL || sv2[0] == '\0')
					continue;
				snprintf(a->gate, sizeof(a->gate), "%s", sv2);
				sv2 = json_as_string(json_object_get(o2, "target"));
				if (sv2 == NULL || sv2[0] == '\0')
					continue;
				snprintf(a->target, sizeof(a->target), "%s", sv2);
				sv2 = json_as_string(json_object_get(o2, "who"));
				snprintf(a->who, sizeof(a->who), "%s", sv2 != NULL ? sv2 : "-");
				a->at = (time_t)json_as_number(json_object_get(o2, "at"));
				g_approval_count++;
			}
		}
	}
	runs = json_object_get(root, "runs");
	if (runs == NULL || runs->type != JSON_ARRAY) {
		json_free(root);
		return;
	}
	if (pkg_runs_alloc() != 0) {
		json_free(root);
		return;
	}
	g_run_count = 0;
	for (i = 0; i < runs->u.array.count && g_run_count < PKG_RUN_MAX; i++) {
		const struct json_value *o = runs->u.array.items[i];
		struct pkg_run *r = &g_runs[g_run_count];
		const char *sv;

		if (o == NULL || o->type != JSON_OBJECT)
			continue;
		memset(r, 0, sizeof(*r));
		sv = json_as_string(json_object_get(o, "name"));
		if (sv == NULL || sv[0] == '\0')
			continue; /* a record with no atom names nothing and is dropped */
		snprintf(r->name, sizeof(r->name), "%s", sv);
		sv = json_as_string(json_object_get(o, "image"));
		snprintf(r->image, sizeof(r->image), "%s", sv != NULL ? sv : "");
		sv = json_as_string(json_object_get(o, "version"));
		snprintf(r->version, sizeof(r->version), "%s", sv != NULL ? sv : "");
		sv = json_as_string(json_object_get(o, "trigger"));
		snprintf(r->trigger, sizeof(r->trigger), "%s",
		         sv != NULL ? sv : PKG_RUN_TRIGGER_REQUEST);
		sv = json_as_string(json_object_get(o, "log"));
		snprintf(r->log, sizeof(r->log), "%s", sv != NULL ? sv : "");
		sv = json_as_string(json_object_get(o, "error"));
		snprintf(r->error, sizeof(r->error), "%s", sv != NULL ? sv : "");
		r->started_at = (time_t)json_as_number(json_object_get(o, "started_at"));
		r->ended_at = (time_t)json_as_number(json_object_get(o, "ended_at"));
		if (pipeline_stage_from_name(json_as_string(json_object_get(o, "stage")), &r->stage) != 0)
			r->stage = PIPELINE_DISCOVER;
		if (pipeline_status_from_name(json_as_string(json_object_get(o, "status")),
		                               &r->status) != 0)
			r->status = PIPELINE_OK;
		g_run_count++;
	}
	json_free(root);
	pkg_runs_trim();
}

int pkg_run_retention_get(void)
{
	return g_run_keep;
}

int pkg_run_retention_set(int keep)
{
	if (keep < 1 || keep > PKG_RUN_MAX)
		return -1;
	g_run_keep = keep;
	pkg_runs_trim();
	pkg_runs_save();
	return 0;
}

void pkg_runs_write_json(struct json_writer *w, const char *name, const char *image, int limit)
{
	char dir[PATH_MAX];
	int i, emitted = 0;

	pkg_build_log_dir(dir, sizeof(dir));
	if (limit <= 0 || limit > PKG_RUN_MAX)
		limit = PKG_RUN_MAX;

	jw_obj_open(w);
	jw_key(w, "runs");
	jw_arr_open(w);
	/* Newest first: the question an operator asks of a history is
	 * almost always about its most recent end. */
	for (i = g_run_count - 1; i >= 0 && emitted < limit; i--) {
		const struct pkg_run *r = &g_runs[i];
		char log_path[PATH_MAX];
		struct stat st;
		int have_log;

		if (name != NULL && name[0] != '\0' && strcmp(r->name, name) != 0)
			continue;
		if (image != NULL && image[0] != '\0' && strcmp(r->image, image) != 0)
			continue;
		emitted++;

		have_log = 0;
		if (r->log[0] != '\0') {
			snprintf(log_path, sizeof(log_path), "%s/%s", dir, r->log);
			have_log = stat(log_path, &st) == 0;
		}

		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, r->name);
		jw_key(w, "image");
		jw_str(w, r->image);
		jw_key(w, "version");
		jw_str(w, r->version);
		jw_key(w, "trigger");
		jw_str(w, r->trigger);
		jw_key(w, "started_at");
		jw_int(w, (long long)r->started_at);
		jw_key(w, "ended_at");
		jw_int(w, (long long)r->ended_at);
		jw_key(w, "duration_seconds");
		jw_int(w, (long long)(r->ended_at > r->started_at ? r->ended_at - r->started_at : 0));
		jw_key(w, "stage");
		jw_str(w, pipeline_stage_name(r->stage));
		jw_key(w, "status");
		jw_str(w, pipeline_status_name(r->status));
		jw_key(w, "error");
		jw_str(w, r->error);
		/*
		 * Both facts, deliberately. `log` is what this run wrote;
		 * `log_available` is whether it is still on disk. Build logs
		 * are capped at PKG_BUILD_LOG_KEEP and runs are kept far
		 * longer, so most historical runs name a pruned file -- and a
		 * caller that can tell "pruned" from "never had one" can say
		 * so, where one given only a filename would offer a link that
		 * 404s.
		 */
		jw_key(w, "log");
		jw_str(w, r->log);
		jw_key(w, "log_available");
		jw_bool(w, have_log);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_key(w, "total");
	jw_int(w, g_run_count);
	jw_key(w, "retention");
	jw_int(w, g_run_keep);
	jw_obj_close(w);
}

/* Deletes oldest-first until at most PKG_BUILD_LOG_KEEP remain. Called
 * before opening a new one, so the cap is enforced at the moment it
 * would otherwise be exceeded rather than by a sweep nobody scheduled. */
static void pkg_build_log_prune(void)
{
	char dir[PATH_MAX];
	struct dirent *ent;
	DIR *d;
	struct {
		char name[NAME_MAX + 1];
		time_t mtime;
	} entries[256];
	int count = 0, i, j;

	pkg_build_log_dir(dir, sizeof(dir));
	d = opendir(dir);
	if (d == NULL)
		return;
	while ((ent = readdir(d)) != NULL && count < (int)(sizeof(entries) / sizeof(entries[0]))) {
		char path[PATH_MAX];
		struct stat st;

		if (ent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		snprintf(entries[count].name, sizeof(entries[count].name), "%s", ent->d_name);
		entries[count].mtime = st.st_mtime;
		count++;
	}
	closedir(d);
	if (count <= PKG_BUILD_LOG_KEEP)
		return;

	/* Insertion sort, oldest first -- count is bounded by the array
	 * above and in practice is the keep-count plus one. */
	for (i = 1; i < count; i++) {
		int k = i;

		while (k > 0 && entries[k - 1].mtime > entries[k].mtime) {
			char tmp_name[NAME_MAX + 1];
			time_t tmp_mtime = entries[k - 1].mtime;

			snprintf(tmp_name, sizeof(tmp_name), "%s", entries[k - 1].name);
			entries[k - 1].mtime = entries[k].mtime;
			snprintf(entries[k - 1].name, sizeof(entries[k - 1].name), "%s", entries[k].name);
			entries[k].mtime = tmp_mtime;
			snprintf(entries[k].name, sizeof(entries[k].name), "%s", tmp_name);
			k--;
		}
	}
	for (j = 0; j < count - PKG_BUILD_LOG_KEEP; j++) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s", dir, entries[j].name);
		unlink(path);
	}
}

/*
 * Opens this build's own log file. Best-effort: a build whose log
 * cannot be opened still builds, it just is not recorded -- diagnostics
 * must never be the reason a build fails.
 *
 * The filename carries name, version and a start timestamp, so repeated
 * attempts at the same version are separate files rather than one
 * overwriting the other. Losing the failed attempt at the moment you
 * retry it is precisely the wrong behaviour for a diagnostic.
 */
static void pkg_build_log_open(struct pkg_entry *e, const char *version)
{
	char dir[PATH_MAX];

	e->build_log_fd = -1;
	e->build_log_path[0] = '\0';
	pkg_build_log_dir(dir, sizeof(dir));
	if (persist_mkdir_p(dir) != 0)
		return;
	pkg_build_log_prune();
	/*
	 * The version has to be passed in rather than read off the entry:
	 * on a first install e->version is only filled from the recipe once
	 * the build COMPLETES, so naming the file from it produced
	 * "<name>-unknown-<time>.log" for exactly the builds most worth
	 * finding again.
	 */
	snprintf(e->build_log_path, sizeof(e->build_log_path), "%s/%s-%s-%lld.log", dir, e->name,
	         version != NULL && version[0] != '\0'
	             ? version
	             : (e->version[0] != '\0' ? e->version : "unknown"),
	         (long long)time(NULL));
	e->build_log_fd = open(e->build_log_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
	if (e->build_log_fd < 0)
		e->build_log_path[0] = '\0';
}

static void pkg_build_log_close(struct pkg_entry *e)
{
	if (e->build_log_fd >= 0) {
		close(e->build_log_fd);
		e->build_log_fd = -1;
	}
}

/*
 * Issue #302: examine one complete line of build output for the
 * shell's own missing-command report, and record the tool it names.
 *
 * bash puts the name immediately before the phrase, behind whatever
 * prefix it happens to carry:
 *
 *   ../libtool: line 6182: find: command not found
 *   /bin/sh: line 1: gzip: command not found
 *   ./configure: line 13423: cmp: command not found
 *
 * so the tool is the token between the last separator before the
 * phrase and the phrase itself. Only this exact wording is matched,
 * because it is the wording actually measured -- dash says "not
 * found" instead, and no image here ships dash. Matching a phrase
 * that has never been observed would be guessing at a second format.
 */
static void pkg_toolscan_line(struct pkg_entry *e, const char *line)
{
	static const char phrase[] = ": command not found";
	const char *hit = strstr(line, phrase);
	const char *start;
	size_t n;

	if (hit == NULL)
		return;

	e->missing_tool_count++;
	if (e->missing_tool[0] != '\0')
		return; /* the first name is what the message needs; the count carries the rest */

	start = hit;
	while (start > line && start[-1] != ' ' && start[-1] != ':' && start[-1] != '\t')
		start--;
	n = (size_t)(hit - start);
	if (n > 0 && n < sizeof(e->missing_tool)) {
		memcpy(e->missing_tool, start, n);
		e->missing_tool[n] = '\0';
	}
}

static void pkg_build_output_append(struct pkg_entry *e, const char *data, int len)
{
	int take = (len > PKG_BUILD_OUTPUT_CAPTURE_MAX) ? PKG_BUILD_OUTPUT_CAPTURE_MAX : len;
	int new_total = e->build_output_captured_len + take;
	int i;

	/*
	 * Issue #302: scanned over every byte, not over the ~4KB tail
	 * below -- the missing-command line that matters is usually
	 * thousands of lines before the end, which is exactly why the
	 * tail never showed it. Lines are assembled across chunk
	 * boundaries here because a read() splits wherever it likes.
	 */
	for (i = 0; i < len; i++) {
		char ch = data[i];

		if (ch == '\n' || ch == '\r') {
			e->toolscan_line[e->toolscan_len] = '\0';
			pkg_toolscan_line(e, e->toolscan_line);
			e->toolscan_len = 0;
			continue;
		}
		if (e->toolscan_len >= PKG_TOOLSCAN_LINE_MAX - 1) {
			/*
			 * Keep the TAIL of an over-long line: the tool name
			 * sits immediately before the phrase, at the end.
			 * Halved rather than shifted by one, so a single
			 * 200KB link command line stays linear.
			 */
			int keep = PKG_TOOLSCAN_LINE_MAX / 2;

			memmove(e->toolscan_line, e->toolscan_line + (e->toolscan_len - keep),
			        (size_t)keep);
			e->toolscan_len = keep;
		}
		e->toolscan_line[e->toolscan_len++] = ch;
	}

	/* Teed here, before the tail truncation below, because the whole
	 * point is to keep what the tail throws away. A short write is
	 * ignored deliberately -- a full disk must not fail the build. */
	if (e->build_log_fd >= 0 && len > 0) {
		ssize_t ignored = write(e->build_log_fd, data, (size_t)len);

		(void)ignored;
	}

	if (new_total > PKG_BUILD_OUTPUT_CAPTURE_MAX) {
		int overflow = new_total - PKG_BUILD_OUTPUT_CAPTURE_MAX;

		memmove(e->build_output_captured, e->build_output_captured + overflow,
		        e->build_output_captured_len - overflow);
		e->build_output_captured_len -= overflow;
	}
	memcpy(e->build_output_captured + e->build_output_captured_len, data + (len - take), take);
	e->build_output_captured_len += take;
	e->last_output_at = time(NULL);
	e->stall_reported = 0;
}

/*
 * Issue #58 gave every in-flight build a last_output_seconds_ago, and
 * nothing ever acted on it. This does.
 *
 * A build that stops producing output has not failed -- nothing exits,
 * no status is reported, and the package sits in "building" looking
 * exactly like one that is working. A real gcc build sat like that for
 * an hour: cc1 deadlocked in __futex_wait on a single conftest, with
 * every other process waiting on it, and the only reason anyone
 * noticed was a human wondering why the box was idle.
 *
 * So this says so, once per stall, and says WHERE: the whole diagnosis
 * that time was one process's wchan. Everything in the build container
 * was at do_wait -- waiting for a child, which tells you nothing --
 * except one cc1 at __futex_wait, which told you everything.
 *
 * It reports and does not kill. A long quiet stretch is not always a
 * stall (a large link, mksquashfs on a big tree), and killing a build
 * that was about to finish is worse than letting an operator decide
 * with the evidence in front of them. Recipes that genuinely want a
 * hard timeout can still have one; what they should not have to do is
 * re-implement the detection, which is what three gcc recipes were
 * each doing separately.
 */
#define PKG_BUILD_STALL_SECONDS_DEFAULT 600

/*
 * How long a build may produce nothing before it is reported as
 * possibly stalled. Overridable through CIX_BUILD_STALL_SECONDS
 * because the honest default is ten minutes -- a legitimately quiet
 * stretch (a large link, mksquashfs over a big tree) can run for
 * several -- and no test should have to sit through that to check that
 * the reporting works at all. Read once, on first use.
 */
static long pkg_build_stall_seconds(void)
{
	static long cached = -1;

	if (cached < 0) {
		const char *env = getenv("CIX_BUILD_STALL_SECONDS");
		long v = env != NULL ? strtol(env, NULL, 10) : 0;

		cached = v > 0 ? v : PKG_BUILD_STALL_SECONDS_DEFAULT;
	}
	return cached;
}

/*
 * How often pkg_check_build_stalls() runs. Derived from the threshold
 * rather than fixed, because a fixed granularity silently bounds what
 * the threshold can mean: with a 60-second tick, asking for a
 * 15-second stall threshold still took up to 75 seconds to report one,
 * so the knob did not do what it said. A quarter of the threshold
 * keeps detection latency proportionate (at most 1.25x the configured
 * value), and the 60-second ceiling keeps the production default (600)
 * ticking exactly as often as it always has.
 */
long pkg_build_stall_check_interval(void)
{
	long interval = pkg_build_stall_seconds() / 4;

	if (interval < 1)
		interval = 1;
	if (interval > 60)
		interval = 60;
	return interval;
}

static void pkg_report_stalled_build(struct pkg_entry *e)
{
	struct hostproc_entry *procs = NULL;
	size_t count = 0;
	size_t i;
	int shown = 0;

	{
		time_t quiet_since = e->last_output_at > 0 ? e->last_output_at : e->build_started_at;

		logstore_write("cixd", "error",
		               "pkg %s@%s: no build output for %ld seconds -- the build may be stalled",
		               e->name, e->image, (long)(time(NULL) - quiet_since));
	}

	if (e->build_container_name[0] == '\0')
		return;
	if (hostproc_snapshot(&procs, &count) != HOSTPROC_OK)
		return;
	for (i = 0; i < count; i++) {
		if (strcmp(procs[i].container, e->build_container_name) != 0)
			continue;
		/* do_wait is every parent in the tree waiting on a child; the
		 * process blocked on anything else is where the build actually
		 * is. Both are printed -- the contrast is the signal. */
		logstore_write("cixd", "error", "  pkg %s: pid %d %s state=%c wchan=%s", e->name,
		               (int)procs[i].pid, procs[i].comm, procs[i].state,
		               procs[i].wchan[0] != '\0' ? procs[i].wchan : "(running)");
		shown++;
		if (shown >= 24)
			break;
	}
	free(procs);
}

void pkg_check_build_stalls(void)
{
	time_t now = time(NULL);
	time_t quiet_since;
	int i;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		struct pkg_entry *e = &g_packages[i];

		if (!e->in_use || e->state != PKG_STATE_BUILDING)
			continue;
		/* Silence is measured from the last output, or from the start
		 * of the build when there has never been any. */
		quiet_since = e->last_output_at > 0 ? e->last_output_at : e->build_started_at;
		if (quiet_since <= 0 || now - quiet_since < pkg_build_stall_seconds())
			continue;
		/* Once per stall, not once per tick -- re-armed by any new
		 * output, so a build that recovers and stalls again is
		 * reported again. */
		if (e->stall_reported)
			continue;
		e->stall_reported = 1;
		pkg_report_stalled_build(e);
	}
}

int pkg_build_output_readable(int chain_idx, char *new_data, int new_data_cap, int *new_data_len)
{
	struct pkg_entry *e = g_build_output_entries[chain_idx];
	char chunk[4096];
	ssize_t n;

	if (new_data_len != NULL)
		*new_data_len = 0;

	if (e == NULL || e->build_output_rd < 0)
		return 1;

	for (;;) {
		n = read(e->build_output_rd, chunk, sizeof(chunk));
		if (n > 0) {
			pkg_build_output_append(e, chunk, (int)n);
			if (new_data != NULL && new_data_len != NULL && *new_data_len < new_data_cap) {
				int room = new_data_cap - *new_data_len;
				int take = ((int)n > room) ? room : (int)n;

				memcpy(new_data + *new_data_len, chunk, take);
				*new_data_len += take;
			}
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0; /* drained everything available right now */
		return 1;         /* EOF (n == 0) or a real read error */
	}
}

/*
 * Copies the current sliding-window tail buffer out for a newly-
 * attaching live-log client (see try_pkg_build_log_upgrade(),
 * daemon/src/main.c) to replay before streaming further live chunks --
 * without this, a client attaching mid-build would see nothing until
 * the next chunk arrives, no matter how much output already happened.
 * Returns the number of bytes copied (<= out_cap).
 */
int pkg_build_output_snapshot(int chain_idx, char *out, int out_cap)
{
	struct pkg_entry *e = g_build_output_entries[chain_idx];
	int take;

	if (e == NULL)
		return 0;
	take = (e->build_output_captured_len > out_cap) ? out_cap : e->build_output_captured_len;

	memcpy(out, e->build_output_captured, take);
	return take;
}

void pkg_build_output_close(int chain_idx)
{
	struct pkg_entry *e = g_build_output_entries[chain_idx];

	if (e != NULL && e->build_output_rd >= 0) {
		close(e->build_output_rd);
		e->build_output_rd = -1;
	}
	/* Issue #57: the log file closes with the pipe, so a build that
	 * was SIGKILLed leaves a complete-up-to-that-point log rather than
	 * nothing -- the killed case is exactly the one worth reading. */
	if (e != NULL)
		pkg_build_log_close(e);
	g_build_output_entries[chain_idx] = NULL;
}

/* image_produce_new_version()'s own mutate() callback for a package
 * install/upgrade -- runs against the freshly copy-forwarded staging
 * rootfs (never the old version's own immutable directory). On an
 * upgrade, the OLD manifest's files are unlinked first (a version that
 * renamed/dropped files shouldn't leave the old ones behind in the
 * new version) and e's own file list is forgotten before merge_tree()
 * repopulates it fresh from dest_dir. pkg_seed_image_baseline() is
 * still called every time (not just on a brand first version) --
 * cheap and idempotent (stat-based skip), and lets an image that
 * missed baseline seeding self-heal on its very next install. */
struct install_mutate_ctx {
	const char *dest_dir;
	struct pkg_entry *e;
	int is_upgrade;
};

/*
 * image_produce_new_version()'s mutate() for folding a just-installed
 * package into the shared build sandbox. Deliberately merge-only: no
 * manifest entry and no baseline seeding, matching exactly what the
 * previous flat merge_tree() call did. The sandbox's manifest still
 * describes only what an operator installed into it directly -- its
 * CONTENT is the wider union, and closing that gap is the declared
 * package-list half of issue #40, not this half.
 */
struct sandbox_merge_ctx {
	const char *dest_dir;
};

/*
 * Issue #389: the undeclared-link gate.
 *
 * Everything below assembles the one input elfcheck_undeclared_links()
 * cannot work out for itself -- which sonames this package's DECLARED
 * dependencies provide. The declaration is the subject: what happens
 * to be installed in the target image is deliberately NOT consulted,
 * because that is precisely the check that would have let
 * cmake@4.4.3-2 through (openssl was installed in cix-builder, and
 * cmake's `depends` was still a lie that the next composed build
 * environment would believe).
 */
#define PKG_DEPCLOSURE_MAX 256

struct depclosure {
	const struct pkg_entry *entries[PKG_DEPCLOSURE_MAX];
	int count;
};

/*
 * Adds one declared dependency and everything it in turn declares.
 *
 * Resolution mirrors buildenv_add_tool(): the recorded `depends` of the
 * INSTALLED copy, not what a recipe now says, because the installed
 * copy is what will actually be present. `name` may carry an `@version`
 * pin, which is stripped -- this asks what a package provides, and
 * every version of it provides the same sonames or it would not be the
 * same package.
 */
static int depclosure_add(const char *name, const char *image, struct depclosure *c,
                           char *err, size_t err_size, int depth)
{
	char bare[PKG_NAME_MAX];
	char deps_copy[PKG_DEPENDS_MAX];
	const struct pkg_entry *found;
	const char *at;
	char *tok, *save = NULL;
	int i;

	at = strchr(name, '@');
	if (at != NULL) {
		size_t bl = (size_t)(at - name);

		if (bl >= sizeof(bare))
			bl = sizeof(bare) - 1;
		memcpy(bare, name, bl);
		bare[bl] = '\0';
	} else {
		snprintf(bare, sizeof(bare), "%s", name);
	}
	if (bare[0] == '\0')
		return 0;

	for (i = 0; i < c->count; i++)
		if (strcmp(c->entries[i]->name, bare) == 0)
			return 0;
	if (depth > PKG_DEPCLOSURE_MAX || c->count >= PKG_DEPCLOSURE_MAX) {
		snprintf(err, err_size, "the declared dependency closure is too large to resolve");
		return -1;
	}

	found = pkg_find(bare, image);
	if (found == NULL || found->state != PKG_STATE_INSTALLED) {
		/*
		 * Not a violation -- an unanswerable question. Dependencies
		 * are installed ahead of the package that declares them, so
		 * this is unexpected; but "I could not find what this
		 * provides" must never be reported as "this links something
		 * undeclared". The caller skips the gate and says why.
		 */
		snprintf(err, err_size, "declared dependency \"%s\" is not installed in image \"%s\"",
		         bare, image);
		return -1;
	}

	c->entries[c->count++] = found;

	snprintf(deps_copy, sizeof(deps_copy), "%s", found->depends);
	for (tok = strtok_r(deps_copy, " \t", &save); tok != NULL; tok = strtok_r(NULL, " \t", &save)) {
		if (depclosure_add(tok, image, c, err, err_size, depth + 1) != 0)
			return -1;
	}
	return 0;
}

/*
 * The sonames provided by a package's declared runtime dependencies,
 * plus the C library -- which is implicit here for the same reason
 * buildenv_resolve_tools() adds it implicitly: no recipe declares it,
 * every binary needs it, and requiring the declaration would be a rule
 * that every package in this platform violates.
 *
 * Returns 0 and fills *out (caller frees) / *out_count, or -1 with a
 * reason in err.
 */
static int declared_provided_sonames(const struct pkg_entry *e, const char *image,
                                      char (**out)[ELFCHECK_SONAME_MAX], int *out_count,
                                      char *err, size_t err_size)
{
	struct depclosure c;
	char deps_copy[PKG_DEPENDS_MAX];
	char (*names)[ELFCHECK_SONAME_MAX];
	char *tok, *save = NULL;
	long total = 0;
	int n = 0;
	int i, f;

	c.count = 0;
	snprintf(deps_copy, sizeof(deps_copy), "%s", e->depends);
	for (tok = strtok_r(deps_copy, " \t", &save); tok != NULL; tok = strtok_r(NULL, " \t", &save)) {
		if (depclosure_add(tok, image, &c, err, err_size, 0) != 0)
			return -1;
	}
	if (depclosure_add(PKG_BASE_LIBC, image, &c, err, err_size, 0) != 0)
		return -1;

	for (i = 0; i < c.count; i++)
		total += c.entries[i]->file_count;
	if (total <= 0) {
		*out = NULL;
		*out_count = 0;
		return 0;
	}
	names = malloc((size_t)total * ELFCHECK_SONAME_MAX);
	if (names == NULL) {
		snprintf(err, err_size, "out of memory assembling the provided-soname list");
		return -1;
	}

	for (i = 0; i < c.count; i++) {
		for (f = 0; f < c.entries[i]->file_count; f++) {
			const char *rel = c.entries[i]->files[f];
			const char *base = strrchr(rel, '/');

			base = base != NULL ? base + 1 : rel;
			if (!elfcheck_is_soname(base))
				continue;
			snprintf(names[n], ELFCHECK_SONAME_MAX, "%s", base);
			n++;
		}
	}
	*out = names;
	*out_count = n;
	return 0;
}

/*
 * Refuses an install whose staged tree links a soname nothing it
 * declares provides. Returns 0 to proceed, -1 to refuse (with the
 * reason in err).
 *
 * A question this cannot answer is not a refusal. That is the whole
 * discipline of a gate that fails an install: it must fire on
 * evidence, and an unresolvable dependency or an unreadable tree is
 * the absence of evidence, not its presence.
 */
static int undeclared_link_gate(const struct pkg_entry *e, const char *image,
                                 const char *staged_dir, char *err, size_t err_size)
{
	char (*provided)[ELFCHECK_SONAME_MAX] = NULL;
	char bad_file[PATH_MAX], bad_soname[ELFCHECK_SONAME_MAX];
	char why[256];
	int provided_count = 0;
	int r;

	if (declared_provided_sonames(e, image, &provided, &provided_count, why, sizeof(why)) != 0) {
		logstore_write("cixd", "warn",
		               "pkg install: %s: skipping the undeclared-link check -- %s (#389)",
		               e->name, why);
		return 0;
	}

	r = elfcheck_undeclared_links(staged_dir, (const char (*)[ELFCHECK_SONAME_MAX])provided,
	                              provided_count, bad_file, sizeof(bad_file), bad_soname,
	                              sizeof(bad_soname));
	free(provided);
	if (r < 0) {
		logstore_write("cixd", "warn",
		               "pkg install: %s: the undeclared-link check could not read the staged "
		               "tree, so it did not run (#389)",
		               e->name);
		return 0;
	}
	if (r == 0)
		return 0;

	snprintf(err, err_size,
	         "%s links \"%s\", which nothing it declares provides -- add the package "
	         "supplying it to pkg_depends= (having it in pkg_build_depends= only puts it in "
	         "the build sandbox, so the link is recorded and the dependency is not)",
	         bad_file, bad_soname);
	logstore_write("cixd", "error", "pkg install: %s: %s (#389)", e->name, err);
	return -1;
}

static int install_mutate(const char *staging_rootfs, void *ctx_v)
{
	struct install_mutate_ctx *ctx = ctx_v;

	if (ctx->is_upgrade) {
		unlink_manifest_files(ctx->e, staging_rootfs);
		pkg_entry_free_files(ctx->e);
	}
	if (pkg_seed_image_baseline(staging_rootfs) != PKG_OK)
		return -1;
	return merge_tree(ctx->dest_dir, staging_rootfs, "", ctx->e);
}

/*
 * Issue #98: the entry that owns a given build container, found by the
 * container's own name rather than by whatever the chain slot currently
 * says.
 *
 * pkg_build_completed() used to resolve its entry with
 * pkg_find(g_chains[idx].name, ...). A chain slot is freed on several
 * failure paths and can then be reallocated to a different job, while
 * the previous job's build container is still alive -- its exit event
 * arrives afterwards, resolves against the NEW job's name, and lands on
 * an entry that never started a build. The visible symptom was a
 * harvest running against "<containers_dir>//upper/build/pkg-dest" (an
 * empty container name in the middle of the path), and the invisible
 * one was completion state being written onto an unrelated package.
 *
 * The container name is the exact key: every build container is named
 * for the chain that started it and recorded on the entry it belongs
 * to, so this cannot resolve to a bystander.
 */
static struct pkg_entry *pkg_find_by_build_container(const char *container_name)
{
	int i;

	if (container_name == NULL || container_name[0] == '\0')
		return NULL;
	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (g_packages[i].in_use && g_packages[i].build_container_name[0] != '\0' &&
		    strcmp(g_packages[i].build_container_name, container_name) == 0)
			return &g_packages[i];
	}
	return NULL;
}

/*
 * Issue #168: destroy a composed build environment once nothing is
 * building against it.
 *
 * A build environment is composed from one recipe's declared tools and
 * exists to serve that build. Leaving it behind is how a box ends up
 * with a drawer of images nobody can account for -- the same accretion
 * that made `cix-builder` an 8 GB pile in the first place, just under
 * prettier names.
 *
 * The environment is named for the SET of tools it holds, so two
 * concurrent builds declaring the same tools legitimately share one
 * (ADR-0157 allows four builds at once). Deleting it while another
 * chain is still using it would pull the lowerdir out from under a
 * running build, so this checks first. That check is why the name is
 * recorded on the chain rather than recomputed: it must answer "is
 * anyone else using THIS image", not "would anyone else compose the
 * same one".
 */
static void buildenv_release(int chain_idx)
{
	char image[PKG_IMAGE_NAME_MAX];
	int i;

	if (g_chains[chain_idx].buildenv_image[0] == '\0')
		return;
	snprintf(image, sizeof(image), "%s", g_chains[chain_idx].buildenv_image);
	g_chains[chain_idx].buildenv_image[0] = '\0';

	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
		if (i == chain_idx)
			continue;
		if (g_chains[i].name[0] != '\0' && strcmp(g_chains[i].buildenv_image, image) == 0)
			return; /* another build is still running against it */
	}

	/* Best-effort: a leftover environment is untidy, never incorrect,
	 * and is certainly not worth failing an otherwise-successful
	 * install over. Reported rather than swallowed so it does not
	 * accumulate silently, which is exactly what went unnoticed
	 * before. */
	if (image_delete(image) != IMAGE_OK)
		fprintf(stderr, "pkg: could not remove the build environment %s\n", image);
}

/*
 * Issue #213: stop an in-flight build.
 *
 * Marks the entry and hands its build container's name back; the CALLER
 * kills it. Deliberately does NOT touch the registry itself
 * (registry_remove()) -- pkg.c has never linked against registry.h,
 * main.c alone owns every registry_* call, and pkg_resume() above
 * already establishes exactly this hand-back shape. Adding a dependency
 * edge just for cancel would trade a real architectural boundary for a
 * few saved lines.
 *
 * The outcome is not recorded here either. Killing the container makes
 * pkg_build_completed() run like any other build death, and that is the
 * single place a failure is written (issue #101). Two writers for one
 * outcome would race over which description survives.
 *
 * The state check is a refusal rather than a no-op on purpose: a cancel
 * arriving just after a build finished must not be able to mark a
 * completed install as cancelled, and "there was nothing to cancel" is
 * more useful to a caller than silence and a success code.
 */
enum pkg_error pkg_cancel(const char *name, const char *image,
                          char *out_container, size_t out_container_size)
{
	struct pkg_entry *e;

	if (out_container == NULL || out_container_size == 0)
		return PKG_ERR_INVALID_NAME;
	out_container[0] = '\0';

	if (name == NULL || name[0] == '\0')
		return PKG_ERR_INVALID_NAME;

	e = pkg_find(name, image);
	if (e == NULL)
		return PKG_ERR_NOT_FOUND;

	if (e->state != PKG_STATE_FETCHING && e->state != PKG_STATE_BUILDING)
		return PKG_ERR_NOT_BUILDING;

	e->cancel_requested = 1;

	if (e->build_container_name[0] != '\0') {
		snprintf(out_container, out_container_size, "%s", e->build_container_name);
		logstore_write("cixd", "info", "pkg %s@%s: cancelling build container %s",
		               e->name, e->image, e->build_container_name);
		return PKG_OK;
	}

	/*
	 * No build container, so this is a fetch -- kill it (#239).
	 *
	 * This used to set the flag and stop, on the reasoning that a fetch
	 * is a subprocess rather than a container and there was "nothing for
	 * the caller to kill", leaving the flag to be honoured by whichever
	 * completion path ran next. If the fetch never completes, no
	 * completion path ever runs: a fetch retrying an unreachable
	 * upstream held its chain slot indefinitely, every later install was
	 * refused 409, and cancel returned 200 having done nothing. Only a
	 * reboot cleared it.
	 *
	 * SIGKILL rather than SIGTERM because the child is curl in a retry
	 * loop and the caller has already said stop. Killing it makes its
	 * pidfd readable, which drives the ordinary fetch-completion path,
	 * which sees cancel_requested and records a cancelled failure --
	 * so the existing machinery finishes the job, and this only has to
	 * end the child.
	 */
	{
		int i;

		for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
			if (g_chains[i].fetch_pid > 0 && strcmp(g_chains[i].name, e->name) == 0 &&
			    strcmp(g_chains[i].image, e->image) == 0) {
				logstore_write("cixd", "info", "pkg %s@%s: cancelling fetch (pid %d)",
				               e->name, e->image, (int)g_chains[i].fetch_pid);
				kill(g_chains[i].fetch_pid, SIGKILL);
				return PKG_OK;
			}
		}
	}

	/*
	 * Genuinely nothing to end, and measured to be a narrow case rather
	 * than the common one it was once thought to be (#339).
	 *
	 * A job past its fetch always has a build_container_name -- it is
	 * assigned in pkg_fetch_completed() BEFORE start_build_container_
	 * spec() runs -- so even a job sitting in a build-environment
	 * composition takes the container branch above, and main.c's own
	 * "already gone from the registry" path ends it. Measured on
	 * 192.168.15.95: cancelling during a real composition logs
	 * "cancelling build container __pkgbuild-0" followed by that path,
	 * and the composition stops with no image left behind.
	 *
	 * So reaching here means a FETCHING entry whose fetch child is
	 * already gone. The flag is recorded and the next completion path
	 * will honour it, but nothing is guaranteed to run one -- hence a
	 * warning rather than an info. Deliberately NOT ending the job
	 * here: that would release a chain slot some child may still own,
	 * which is the #98 hazard.
	 */
	logstore_write("cixd", "warn",
	               "pkg %s@%s: cancel requested with no fetch or build container running -- "
	               "the flag is recorded but nothing was ended (#339)",
	               e->name, e->image);
	return PKG_OK;
}

int pkg_build_completed(const char *container_name, int exit_status, pid_t *out_pid,
                         int *out_pidfd, int *out_chain_idx, char *out_hostbuild_done_name,
                         int *out_kept)
{
	struct pkg_entry *e;
	char container_base[PATH_MAX];
	char dest_dir[PATH_MAX];
	int is_final, is_upgrade;
	int chain_idx;

	out_hostbuild_done_name[0] = '\0';
	*out_kept = 0;

	chain_idx = pkg_build_container_chain_index(container_name);
	if (chain_idx < 0)
		return 0;

	/*
	 * Issue #168: the environment composed for this build goes when the
	 * build is over -- success, failure, or a chained dependency moving
	 * on. A later step in the same chain composes its own from its own
	 * recipe's declared tools; that is the point.
	 *
	 * Except when the build container is being PRESERVED (ADR-0175's
	 * keep_on_failure): that container's whole purpose is to be
	 * inspected and resumed afterwards, and the composed environment is
	 * its overlay lowerdir. Destroying it leaves a container that
	 * cannot be read or resumed -- which is exactly what happened when
	 * this released unconditionally: `pkg resume` failed on a container
	 * whose environment had been deleted out from under it, and the
	 * test suite caught it.
	 *
	 * A resumed build ends in this same function and releases then, so
	 * the resume path does not leak. A kept container the operator
	 * simply deletes does leave its environment behind -- untidy, never
	 * incorrect, and preferable to breaking the feature that asked for
	 * the container to be kept in the first place.
	 */
	if (exit_status == 0 || !g_chains[chain_idx].keep_on_failure)
		buildenv_release(chain_idx);

	/*
	 * Issue #98: keyed on the container that actually exited. Resolving
	 * through the chain slot's current name is wrong whenever that slot
	 * has been freed and reused since this build started -- see
	 * pkg_find_by_build_container()'s own comment.
	 */
	e = pkg_find_by_build_container(container_name);
	if (e == NULL) {
		/* Nothing owns this container: a stale exit for a job that is
		 * already finished and cleaned up. Touch no entry -- the whole
		 * bug this guards against was completion state being written
		 * onto a bystander. The chain slot is only cleared if it is
		 * still the one this container belonged to. */
		struct pkg_entry *chain_entry =
		    pkg_find(g_chains[chain_idx].name, g_chains[chain_idx].image);

		if (chain_entry == NULL ||
		    strcmp(chain_entry->build_container_name, container_name) == 0) {
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
		}
		return 0;
	}

	is_final = (g_chains[chain_idx].dep_queue_pos + 1 >= g_chains[chain_idx].dep_queue_count);
	is_upgrade = is_final && g_chains[chain_idx].dep_queue_is_upgrade;

	/*
	 * Deliberately NOT gated on exit_status. A cancelled build is
	 * cancelled whatever the kill reported.
	 *
	 * The first version of this checked `exit_status != 0` on the
	 * reasoning that a killed process cannot exit cleanly. It can: the
	 * observed status after registry_remove() reaped the container came
	 * back 0, the success path ran, and a build the operator had just
	 * stopped was INSTALLED -- a half-built tree merged into the image
	 * because the kill happened to look tidy.
	 *
	 * So intent wins over exit code. There is no reading of a status
	 * that should turn "the operator stopped this" into "this
	 * succeeded".
	 */
	if (e != NULL && e->cancel_requested) {
		/*
		 * Issue #213: an operator stopped this build. The container
		 * was SIGKILLed, so without this it lands in the
		 * killed-by-signal branch below and is reported as
		 * "killed by signal 9" -- true, useless, and identical to
		 * every other way a build can be killed.
		 *
		 * Recorded through pkg_fail() like every other outcome
		 * (issue #101), with is_upgrade carrying the usual rule: a
		 * cancelled UPGRADE leaves the package installed at the
		 * version it already had, because that version is still
		 * there and still working.
		 */
		e->cancel_requested = 0;
		pkg_fail_cancelled(e, is_upgrade, PIPELINE_BUILD,
		         "build cancelled by operator");
		logstore_write("cixd", "info", "pkg %s@%s: build cancelled by operator",
		               e->name, e->image);

		/*
		 * Return, exactly as the failure branch below does.
		 *
		 * Falling through would reach the success path further down and
		 * merge this build's tree into the image -- which is precisely
		 * what happened before this return existed: a cancelled build
		 * was recorded as cancelled and then INSTALLED a moment later,
		 * ending up "installed" with the operator's cancel silently
		 * overwritten.
		 *
		 * The chain is abandoned for the same reason a failed
		 * dependency abandons it: whatever asked for this cannot
		 * complete now.
		 */
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	} else if (exit_status != 0) {
		/*
		 * Whatever the build container's own stdout/stderr actually
		 * said -- the one piece of information no exit-status decode
		 * below can ever substitute for: the exit code says WHICH
		 * syscall or exec attempt failed, this says WHY in the
		 * recipe script's own words (e.g. a real "make: not found"
		 * from bash itself, indistinguishable from container.c's own
		 * exit-127 fallback by exit status alone).
		 *
		 * e->build_output_captured has already been incrementally
		 * filled by pkg_build_output_readable() (see its own comment
		 * and ADR-0087) as the container ran, via the caller's epoll
		 * registration -- NOT read here in one shot, which is what
		 * used to deadlock any build whose combined output exceeded
		 * the pipe's 64KB kernel buffer. One last non-blocking drain
		 * catches anything written in the brief window between the
		 * container's own final write() and its exit (by the time
		 * waitpid() confirms the exit, the kernel has already torn
		 * down its fd table, so this read either returns real data
		 * still sitting in the pipe or immediate EOF, never blocks).
		 * Keeps only the TAIL -- the last PKG_BUILD_OUTPUT_CAPTURE_MAX
		 * bytes, via a fixed-size sliding window (drop the front,
		 * append at the end) -- since the actual error is almost
		 * always the last thing printed, regardless of total output
		 * length (confirmed live, 2026-08-08: a real lldap build
		 * failure's own captured output was 100% "Compiling ..."
		 * lines under the old head-only capture, the real error long
		 * since scrolled past).
		 */
		char captured[PKG_BUILD_OUTPUT_CAPTURE_MAX + 1];
		int captured_len;

		pkg_build_output_readable(chain_idx, NULL, 0, NULL);
		captured_len = e->build_output_captured_len;
		memcpy(captured, e->build_output_captured, captured_len);
		captured[captured_len] = '\0';

		/*
		 * 110-119 are src/container.c's own distinct pre-exec setup
		 * failure codes (mount/overlay/network/etc., one of ~10 steps
		 * that all ran before the recipe's own script ever got to
		 * execve()), 127 is execve() itself failing -- both far more
		 * specific and useful than the bare "exit status N" every
		 * other value already gets, and container.c (runtime-library
		 * code, no logstore.h dependency of its own) has no other way
		 * to surface which step failed than through this same exit
		 * status pkg.c already receives.
		 */
		/*
		 * The pre-execve exit codes belong to src/container.c and are
		 * named by container_decode_exit_status(), the same function
		 * registry.c already reports ordinary containers through.
		 *
		 * This file used to carry its own copies of those two tables,
		 * and the duplication cost exactly what a second copy always
		 * costs: ADR-0260 added exit 138 (an fd named on cix-init's
		 * argv was not open) to container.c's table and not to this
		 * one, so every package build on the platform failed with
		 * "build killed by signal 10" -- 138 is also 128+10 -- while
		 * the container's own log line next to it said "fcntl(keep_fds,
		 * F_SETFD): Bad file descriptor". One table, read from one
		 * place, cannot drift from itself.
		 *
		 * Captured output is the discriminator, and it is decisive
		 * rather than a heuristic: container.c emits these codes only
		 * BEFORE execve() succeeds, so a single byte of build output
		 * proves the number came from the recipe shell instead --
		 * where 130-136 mean "killed by signal 2..8" and nothing to do
		 * with overlayfs. The old 130-136 branch had no such guard and
		 * would have reported a shell killed by SIGKILL... as an
		 * overlay mkdir failure.
		 */
		if (exit_status >= 110 && exit_status <= 139 && e->build_output_captured_len == 0) {
			char step[192];

			container_decode_exit_status(exit_status, 0, step, sizeof(step));
			logstore_write("cixd", "error", "pkg %s@%s: build container setup failed (%s)",
			                e->name, g_chains[chain_idx].image, step);
			pkg_fail(e, is_upgrade, PIPELINE_BUILD, "build container setup failed (%s)", step);
		} else if (exit_status >= 141 && exit_status <= 255 &&
		           e->build_output_captured_len == 0) {
			/*
			 * A real errno (encoded by src/container.c/src/overlay.c,
			 * 140 + errno, deliberately one single shared range --
			 * see include/container.h's own comment for why that's
			 * safe) from either of the two syscalls in this pipeline
			 * that can fail with a genuinely informative one:
			 * overlay_create()'s own mount(2) (e.g. EINVAL is the
			 * classic symptom of lowerdir's filesystem not returning
			 * real d_type from readdir()), or the final execve() that
			 * actually runs the recipe's build script (ENOENT
			 * genuinely missing, EACCES not executable -- lost +x bit
			 * or a noexec mount, ENOEXEC bad ELF format, ELIBBAD
			 * corrupted/incompatible shared library) -- none of which
			 * a bare exit 127 could ever distinguish, and the two
			 * cases are mutually exclusive within a single run
			 * (execve() is only ever reached once overlay_create()
			 * has already succeeded).
			 */
			int real_errno = exit_status - 140;

			logstore_write("cixd", "error",
			                "pkg %s@%s: build container setup/exec failed: %s", e->name,
			                g_chains[chain_idx].image, strerror(real_errno));
			pkg_fail(e, is_upgrade, PIPELINE_BUILD, "build container setup/exec failed: %s",
			         strerror(real_errno));
		} else if (exit_status >= 128 && exit_status <= 128 + 64) {
			/*
			 * The build ran (it produced output), so this is the
			 * recipe shell's own exit status, not the 140+errno
			 * encoding above -- and 128+N is the shell's convention
			 * for "the command I ran was killed by signal N".
			 *
			 * The two ranges overlap, which produced a genuinely
			 * misleading report: a gcc build that compiled an entire
			 * compiler and then died in its cleanup exited 143, and
			 * 143 is both 128+SIGTERM and 140+ESRCH. It was reported
			 * as "overlay mount or exec failed: No such process" -- a
			 * finished build described as a container that never
			 * started.
			 *
			 * Captured output is what tells them apart, and it is
			 * decisive rather than a heuristic: the 140+errno encoding
			 * is only ever emitted BEFORE execve() succeeds (see
			 * src/container.c), so a single byte of build output
			 * proves the setup path was left behind long ago.
			 */
			int sig = exit_status - 128;
			const char *name = container_signal_name(sig);

			if (name != NULL)
				pkg_fail(e, is_upgrade, PIPELINE_BUILD,
				         "build killed by signal %d (%s)", sig, name);
			else
				pkg_fail(e, is_upgrade, PIPELINE_BUILD, "build killed by signal %d", sig);
			logstore_write("cixd", "error", "pkg %s@%s: build killed by signal %d%s%s%s",
			                e->name, g_chains[chain_idx].image, sig, name != NULL ? " (" : "",
			                name != NULL ? name : "", name != NULL ? ")" : "");
		} else if (exit_status == 127) {
			/*
			 * Genuinely ambiguous by exit status alone: either
			 * container.c's own fallback for an execve() errno too
			 * large even for the shared 1-115 range above, OR a
			 * real, legitimate exit 127 from bash itself (its own
			 * "command not found" convention) once bash was
			 * successfully execve()'d and something INSIDE the
			 * recipe's build script -- make, a compiler, whatever
			 * -- failed to run. The captured output logged below is
			 * how to actually tell the two apart: container.c's own
			 * pre-execve perror() calls never reach the pipe (they
			 * go to the daemon's own stderr, unrelated), so any real
			 * captured text here means bash did launch and this is
			 * its own real exit code, not the fallback.
			 */
			logstore_write("cixd", "error",
			                "pkg %s@%s: build failed with exit 127 (ambiguous: either an "
			                "execve() errno too large to encode, or a real \"command not "
			                "found\" from inside the recipe's own build script -- check the "
			                "build output logged separately)",
			                e->name, g_chains[chain_idx].image);
			pkg_fail(e, is_upgrade, PIPELINE_BUILD,
			         "build failed (exit 127 -- see build output in logs)");
		} else {
			pkg_fail(e, is_upgrade, PIPELINE_BUILD, "build failed (exit status %d)",
			         exit_status);
		}
		if (captured_len > 0)
			logstore_write("cixd", "error", "pkg %s@%s: build output: %s", e->name,
			                g_chains[chain_idx].image, captured);

		/*
		 * ADR-0175/issue #35: only the chain's own final job (the
		 * package actually requested, not an incidental dependency
		 * pulled in along the way -- is_final) ever gets preserved,
		 * and only when the caller actually asked for it
		 * (keep_on_failure). e->build_container_name was set back in
		 * pkg_fetch_completed() when this build container was first
		 * spawned and is still valid here -- copied into
		 * kept_build_container (a distinct field, not reused in
		 * place) so a later, unrelated retry on this same pkg_entry
		 * can freely overwrite build_container_name without silently
		 * invalidating what's reported here. The container itself is
		 * left fully alone -- the caller (main.c's
		 * handle_container_event()) is the one that actually skips
		 * registry_remove() based on *out_kept.
		 */
		if (is_final && g_chains[chain_idx].keep_on_failure) {
			*out_kept = 1;
			snprintf(e->kept_build_container, sizeof(e->kept_build_container), "%s",
			         e->build_container_name);
			snprintf(e->error + strlen(e->error), sizeof(e->error) - strlen(e->error),
			         " (build container preserved for debugging: %s -- see GET .../files or "
			         "DELETE .../containers/%s to clean up)",
			         e->kept_build_container, e->kept_build_container);
		}

		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0; /* abort the rest of the chain -- a failed dependency
		                        * means the top-level install can't complete either */
		return 0;
	}

	/*
	 * Clean success -- the captured output (opened regardless of
	 * outcome in pkg_fetch_completed()) is simply discarded, not
	 * logged, matching the failure branch's own use above. The pipe
	 * fd itself is deliberately left alone here: it is still
	 * registered with the caller's epoll loop (see
	 * pkg_build_output_fd()/register_pkg_build_output() in main.c),
	 * which owns closing it once its own EOF-driven
	 * handle_pkg_build_output_event() fires -- closing it directly
	 * from here would race that still-live epoll registration.
	 */
	/*
	 * Issue #302: exiting 0 is not sufficient. If the shell reported
	 * a missing command anywhere in this build's output, something
	 * the build reached for was not in the environment -- and the
	 * cases that matter most are precisely the ones that did NOT
	 * change the exit status.
	 *
	 * Measured before this was made fatal: of the 41 build logs on
	 * the host at the time, exactly one package's logs contained the
	 * phrase, and that package is the bug this came from. So nothing
	 * that builds correctly today is refused by it.
	 *
	 * gettext is the worked example. find missing meant libtool
	 * absorbed no convenience archives and reported success; cmp
	 * missing meant configure answered two feature probes from a
	 * tool that was not there. Both exited 0 with wrong output.
	 */
	if (e->missing_tool_count > 0) {
		logstore_write("cixd", "error",
		                "pkg %s@%s: build environment is missing '%s'%s -- the shell reported "
		                "%d missing command(s) during a build that exited 0; declare it in "
		                "pkg_build_depends (#302)",
		                e->name, g_chains[chain_idx].image, e->missing_tool,
		                e->missing_tool_count > 1 ? " and others" : "",
		                e->missing_tool_count);
		pkg_fail(e, is_upgrade, PIPELINE_BUILD,
		         "build environment is missing '%s' (%d missing command(s) reported) -- "
		         "declare it in pkg_build_depends",
		         e->missing_tool, e->missing_tool_count);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	e->build_output_captured_len = 0;
	e->last_output_at = 0;

	snprintf(container_base, sizeof(container_base), "%s/%s", g_containers_dir,
	         e->build_container_name);
	snprintf(dest_dir, sizeof(dest_dir), "%s/upper/build/pkg-dest", container_base);

	/*
	 * Move the entry to the version this job installed -- for a fresh
	 * install the first time it is set, for an upgrade the point where
	 * it finally leaves the old version.
	 *
	 * Read from the chain, which captured it in start_fetch_for() when
	 * the recipe was parsed, rather than re-read off disk here (#326).
	 * The old code did the latter and wrote version/depends only `if`
	 * the lookup and parse both succeeded, immediately above an
	 * UNCONDITIONAL `e->state = PKG_STATE_INSTALLED` -- so a revision
	 * withdrawn while its build was in flight left an entry INSTALLED
	 * with no version, which is the wreckage #326 measured. A job's
	 * own version cannot change halfway through; looking it up twice
	 * was the whole defect.
	 */
	if (g_chains[chain_idx].fetch_resolved_version[0] != '\0') {
		snprintf(e->version, sizeof(e->version), "%s",
		         g_chains[chain_idx].fetch_resolved_version);
		snprintf(e->depends, sizeof(e->depends), "%s",
		         g_chains[chain_idx].fetch_resolved_depends);
	}

	/*
	 * Marked INSTALLED here, before either branch below, rather than
	 * only after both succeed -- the non-hostbuild branch's own
	 * image_produce_new_version() call needs e's final post-install
	 * state (name/version/state all correct) to compute the new
	 * image version's own manifest hash (ADR-0108) for THIS package's
	 * contribution. Safe: both branches below unconditionally
	 * overwrite this back to PKG_STATE_FAILED on their own failure
	 * path, so a failed merge never leaves a falsely-INSTALLED entry.
	 */
	e->state = PKG_STATE_INSTALLED;
	e->error[0] = '\0';
	e->status = PIPELINE_OK; /* a success that leaves a failed status set
	                          * reports an installed package as failed to
	                          * anything keying on it */

	if (g_chains[chain_idx].is_hostbuild) {
		/* A hostbuild's output is a standalone host artifact (a
		 * bzImage, a cixd-root squashfs's own components), not
		 * something that belongs inside any container image's
		 * rootfs -- harvested to a plain host directory instead of
		 * merged into an image (ADR-0056). Passing the real e here
		 * (issue #6) populates e->files purely for GET-visibility
		 * ("what did this hostbuild actually produce" -- previously
		 * always empty, ambiguous with "nothing built" even on a
		 * genuine success) -- unlink_manifest_files() is never
		 * reached for a hostbuild entry regardless (both its call
		 * sites go through install_mutate_ctx, which only the
		 * ordinary, non-hostbuild branch below ever constructs), so
		 * there is still no real manifest-delete behavior here, just
		 * real reporting. */
		char artifact_dir[PATH_MAX];

		snprintf(artifact_dir, sizeof(artifact_dir), "%s/%s", g_artifacts_dir, e->name);
		/*
		 * Forget the previous round's list before merge_tree() fills
		 * it in again, exactly as install_mutate() does for an
		 * ordinary install (#185). Without this the list is APPENDED
		 * to on every rebuild: `cix` reported its twelve files five
		 * times over, once per hostbuild since the entry was created,
		 * growing without bound. The paths on disk were always right;
		 * only the record of them was wrong. This branch was added
		 * later, for GET-visibility alone, and never picked up the
		 * reset the ordinary path had always had.
		 */
		pkg_entry_free_files(e);
		/*
		 * ADR-0253: and forget the previous round's FILES, not just the
		 * record of them. The comment above is right that the record
		 * was the bug it was written for, and wrong as a general claim:
		 * the paths on disk were NOT always right. mkbootroot reads
		 * <artifacts>/cix/web, no cix recipe has ever staged it, and
		 * every assembly worked anyway because a web/ left by an older
		 * build was still sitting here. Reinstalling the box swept the
		 * leftovers away and assembly began failing immediately. A
		 * build output tree is created fresh or it is not a build
		 * output tree.
		 */
		if (persist_fresh_output_dir(artifact_dir) != 0 ||
		    merge_tree(dest_dir, artifact_dir, "", e) != 0) {
			pkg_fail(e, 0, PIPELINE_INSTALL, "failed to harvest the built artifact");
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			g_chains[chain_idx].is_hostbuild = 0;
			return 0;
		}
		/*
		 * Publish it, exactly as the ordinary branch below does for a
		 * freshly built package (issue #129).
		 *
		 * This branch never enqueued one, so no hostbuild artifact has
		 * ever auto-published: the cache's newest `cix` was
		 * v2.2.0-rc6, and `kernel` and `isotools` had no entry at all,
		 * while ordinary packages published on every build. Nothing
		 * about a hostbuild makes its output less worth distributing --
		 * it is the most worth distributing, since `cix` and `kernel`
		 * are what other hosts install to update themselves, and
		 * rebuilding one is the most expensive thing this platform
		 * does.
		 *
		 * Only a fresh build, same as below: a cache hit's bytes
		 * already came from the cache.
		 */
		if (!e->cache_hit)
			pkg_artifact_push_enqueue(e->name, e->version);
	} else {
		struct install_mutate_ctx ctx;
		char undeclared[512];

		/*
		 * Issue #389, before a single byte is staged -- same
		 * discipline as the #176 builtin check inside merge_tree(): a
		 * refused build leaves nothing of itself behind.
		 *
		 * Here rather than inside install_mutate() so the refusal
		 * costs no copy-forward of the image, and so the message
		 * names THIS package. A failure raised from inside the mutate
		 * callback surfaces as "image X could not produce a new
		 * version", which is the right message for the failures that
		 * are the image's fault and exactly the wrong one for this,
		 * where the package is at fault and is the thing to fix.
		 */
		if (undeclared_link_gate(e, g_chains[chain_idx].image, dest_dir, undeclared,
		                          sizeof(undeclared)) != 0) {
			pkg_fail(e, 0, PIPELINE_INSTALL, undeclared);
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
		}

		ctx.dest_dir = dest_dir;
		ctx.e = e;
		ctx.is_upgrade = is_upgrade;
		if (image_produce_new_version(g_chains[chain_idx].image, install_mutate, &ctx, NULL) !=
		    0) {
			const char *why = pkg_last_image_produce_failure();
			char msg[320];

			/* Name the image and what it could not do, not this
			 * package -- the package is usually blameless (#180). */
			snprintf(msg, sizeof(msg),
			         "image \"%s\" could not produce a new version%s%s",
			         g_chains[chain_idx].image, why != NULL ? ": " : "",
			         why != NULL ? why : "");
			pkg_fail(e, 0, PIPELINE_INSTALL, msg);
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
		}

		/*
		 * #281: the merge said yes -- check that it meant it.
		 *
		 * Deliberately after the merge and before anything that
		 * treats this install as done: save_state() below is what
		 * makes "installed" survive a restart, and an entry that
		 * survives while its files do not is precisely the split this
		 * issue is about.
		 */
		{
			char first_missing[PKG_NAME_MAX + 128];
			int missing = installed_files_missing(g_chains[chain_idx].image, e, first_missing,
			                                       sizeof(first_missing));

			if (missing > 0) {
				char msg[448];

				snprintf(msg, sizeof(msg),
				         "installed into image \"%s\" but %d of %d file(s) are not in its "
				         "current version, starting with \"%s\" -- an image version is a "
				         "hash of the installed package set (ADR-0108), so re-installing an "
				         "already-present name@version reproduces the same hash and the "
				         "rebuilt tree is discarded (ADR-0155). Bump the package revision, "
				         "or delete and recreate the image",
				         g_chains[chain_idx].image, missing, e->file_count, first_missing);
				logstore_write("cixd", "error", "pkg %s@%s: %s", e->name,
				                g_chains[chain_idx].image, msg);
				pkg_fail(e, 0, PIPELINE_INSTALL, "%s", msg);
				g_chains[chain_idx].name[0] = '\0';
				g_chains[chain_idx].dep_queue_count = 0;
				return 0;
			}
		}

		/*
		 * #289: the package installed headers -- do they resolve?
		 *
		 * Logged and not fatal, unlike the check above it. That one
		 * asks whether the files arrived, which has one answer; this
		 * one reads C without a preprocessor and cannot claim the
		 * same certainty, so it reports rather than refuses. It is
		 * still said at the moment of the install, because the whole
		 * failure this exists for is a broken interface shipping
		 * quietly and surfacing later as someone else's build error.
		 */
		{
			char first_bad[PKG_NAME_MAX + 320];
			int bad = header_includes_unresolved(g_chains[chain_idx].image, e, first_bad,
			                                      sizeof(first_bad));

			if (bad > 0)
				logstore_write("cixd", "error",
				                "pkg %s@%s: installs %d header include(s) that do not "
				                "resolve in this image, starting with \"%s\" -- the header "
				                "is shipped and cannot be included, so anything compiling "
				                "against it fails with an error naming neither this package "
				                "nor the missing file (#289). Check whether the recipe "
				                "builds every directory whose headers it installs",
				                e->name, g_chains[chain_idx].image, bad, first_bad);
		}

		/* ADR-0122: a fresh, real build's own output is saved into the
		 * local cache for next time (best-effort, LRU-evicting older
		 * entries as needed); a cache/artifact hit's own dest_dir was
		 * itself just extracted FROM the cache, so there's nothing new
		 * to save -- just bump its mtime so LRU eviction correctly
		 * treats it as recently used, not stale. */
		if (e->cache_hit) {
			pkg_cache_touch(e->name, e->version);
		} else {
			pkg_cache_save(e->name, e->version, dest_dir);
			/* Issue #129: a fresh build is the only thing worth
			 * publishing -- a cache/artifact hit's bytes already came
			 * from somewhere else. */
			pkg_artifact_push_enqueue(e->name, e->version);
		}

		/*
		 * Issue #168: a successful install no longer folds into any
		 * shared sandbox.
		 *
		 * It used to also merge into PKG_BUILD_SANDBOX_IMAGE -- which
		 * is the `cix-builder` image -- so that a later recipe's build
		 * could see it. That was a real fix for a real gap at the
		 * time (sbsigntools needed bfd.h from binutils-dev, and the
		 * sandbox was a snapshot nothing ever updated), but ADR-0199
		 * solved the same problem properly: a build environment is
		 * composed from the tools a recipe DECLARES, so a build sees
		 * its dependencies because it named them, not because someone
		 * happened to install them here first.
		 *
		 * Keeping both meant every install grew a user-facing image
		 * forever, in a way no manifest described -- 66 versions of an
		 * 8 GB rootfs on the first box to be looked at. Build-time
		 * visibility now comes from declaration alone, which is the
		 * only source of truth a recipe should have.
		 */
	}

	/*
	 * ADR-0272: the run closes HERE and not at the provisional
	 * `e->state = PKG_STATE_INSTALLED` further up, whose own comment
	 * says both branches above may revert it. Every failure between
	 * the two returns early through pkg_fail(), so reaching this line
	 * is what makes the success final.
	 */
	pkg_run_close(e);

	save_state();

	if (g_chains[chain_idx].is_hostbuild)
		snprintf(out_hostbuild_done_name, PKG_NAME_MAX, "%s", e->name);

	if (!is_final) {
		enum pkg_error perr;

		g_chains[chain_idx].dep_queue_pos++;
		perr = start_fetch_for(g_chains[chain_idx].dep_queue[g_chains[chain_idx].dep_queue_pos],
		                        chain_idx, out_pid, out_pidfd);
		if (perr == PKG_OK) {
			*out_chain_idx = chain_idx;
			return 1;
		}
		/* couldn't start the next dependency -- abort the chain */
	}

	g_chains[chain_idx].name[0] = '\0';
	g_chains[chain_idx].dep_queue_count = 0;
	return 0;
}

/*
 * The same packages, as CONFIGURATION rather than as state (ADR-0206).
 *
 * pkg_write_json_list() answers "what is the package manager doing" --
 * every entry with its file manifest, its build error, whether its
 * artifact was cached, how long since its last output. All of that is
 * the right answer for GET /pkg and the wrong one for a configuration
 * document, where the fact is simply: this package, at this version, in
 * this image.
 *
 * The difference is not cosmetic. Measured on a real host with 174
 * packages installed, the full form renders 1806 KB and 1756 KB of that
 * -- 97% -- is files[] manifests. It made the configuration document
 * 1.76 MB, of which 99.7% was one section, which is not a document
 * anybody reads, diffs or pastes into a review. Those are the three
 * things it exists for.
 *
 * state is kept because "installed" versus "failed" changes what the
 * document means; everything transient is dropped.
 */
void pkg_write_json_config(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (!g_packages[i].in_use)
			continue;
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, g_packages[i].name);
		jw_key(w, "image");
		jw_str(w, g_packages[i].image);
		jw_key(w, "version");
		jw_str(w, g_packages[i].version);
		jw_key(w, "state");
		jw_str(w, pkg_state_name(g_packages[i].state));
		jw_obj_close(w);
	}
	jw_arr_close(w);
}

void pkg_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (g_packages[i].in_use)
			write_pkg_json(&g_packages[i], w);
	}
	jw_arr_close(w);
}

/*
 * GET /pkg/drift -- issue #217.
 *
 * Counts every installed package and reports the ones behind their
 * recipe. The count of installed packages is included deliberately:
 * "12 behind" means something very different against 30 packages than
 * against 300, and an operator reading a bare list has no denominator.
 */
/*
 * GET /v1/pkg/verify (#281) -- which installed packages are not
 * actually in their image.
 *
 * The install path refuses this state now, but only for installs made
 * since. An image whose version was deduped before that gate existed,
 * or whose tree was removed by something outside pkg.c, still holds
 * the split -- "installed" in one view, absent from the other, with
 * the operator's first evidence being a missing binary.
 *
 * On demand, deliberately, and never from GET /v1/pkg. That endpoint
 * is polled every two seconds by the dashboard and was already the
 * single largest source of event-loop stalls; adding a stat of every
 * file of every package to it would be a self-inflicted outage. This
 * one is a verb an operator runs.
 */
void pkg_write_verify_json(struct json_writer *w)
{
	int i;
	int checked = 0, bad = 0;

	jw_obj_open(w);
	jw_key(w, "packages");
	jw_arr_open(w);
	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		struct pkg_entry *e = &g_packages[i];
		char first_missing[PKG_NAME_MAX + 128];
		char first_bad_include[PKG_NAME_MAX + 320];
		int missing, unresolved;

		if (!e->in_use || e->state != PKG_STATE_INSTALLED)
			continue;
		if (strcmp(e->image, PKG_HOSTBUILD_IMAGE) == 0)
			continue; /* A hostbuild's output is a host artifact, never
			           * merged into an image rootfs (ADR-0056), so there
			           * is no image tree for it to be missing from. */
		checked++;
		missing = installed_files_missing(e->image, e, first_missing, sizeof(first_missing));
		unresolved = header_includes_unresolved(e->image, e, first_bad_include,
		                                         sizeof(first_bad_include));
		if (missing == 0 && unresolved == 0)
			continue;
		bad++;
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, e->name);
		jw_key(w, "image");
		jw_str(w, e->image);
		jw_key(w, "version");
		jw_str(w, e->version);
		jw_key(w, "missing_count");
		jw_int(w, missing);
		jw_key(w, "file_count");
		jw_int(w, e->file_count);
		jw_key(w, "first_missing");
		jw_str(w, first_missing);
		jw_key(w, "unresolved_includes");
		jw_int(w, unresolved);
		jw_key(w, "first_unresolved_include");
		jw_str(w, first_bad_include);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_key(w, "checked");
	jw_int(w, checked);
	jw_key(w, "incomplete");
	jw_int(w, bad);
	jw_obj_close(w);
}

void pkg_write_drift_json(struct json_writer *w)
{
	int i, installed = 0, behind = 0;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (g_packages[i].in_use && g_packages[i].state == PKG_STATE_INSTALLED)
			installed++;
	}

	jw_obj_open(w);
	jw_key(w, "installed");
	jw_int(w, installed);

	/* Walked twice rather than buffered: the count has to be written
	 * before the array in a streaming writer, and PKG_MAX_PACKAGES is
	 * a fixed, small array. Buffering the drifted set to avoid a second
	 * pass would trade a real allocation for nothing. */
	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (pkg_entry_drift(&g_packages[i], NULL, 0))
			behind++;
	}
	jw_key(w, "behind");
	jw_int(w, behind);

	jw_key(w, "packages");
	jw_arr_open(w);
	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		const struct pkg_entry *e = &g_packages[i];
		char available[PKG_VERSION_MAX];

		if (!pkg_entry_drift(e, available, sizeof(available)))
			continue;
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, e->name);
		jw_key(w, "image");
		jw_str(w, e->image);
		jw_key(w, "installed_version");
		jw_str(w, e->version);
		jw_key(w, "available_version");
		jw_str(w, available);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}

int pkg_find_update_candidate(char *out_name, size_t out_name_size, char *out_image,
                               size_t out_image_size)
{
	int i;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		struct pkg_entry *e = &g_packages[i];

		if (pkg_entry_drift(e, NULL, 0)) {
			snprintf(out_name, out_name_size, "%s", e->name);
			snprintf(out_image, out_image_size, "%s", e->image);
			return 1;
		}
	}
	return 0;
}

int pkg_rename_image(const char *old_image, const char *new_image)
{
	int i, moved = 0;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (!g_packages[i].in_use)
			continue;
		if (strcmp(normalize_image(g_packages[i].image), normalize_image(old_image)) != 0)
			continue;
		snprintf(g_packages[i].image, sizeof(g_packages[i].image), "%s", new_image);
		moved++;
	}
	if (moved > 0 && save_state() != 0)
		return -1;
	return moved;
}

int pkg_image_has_packages(const char *image)
{
	const char *norm_image = normalize_image(image);
	int i;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (g_packages[i].in_use && strcmp(g_packages[i].image, norm_image) == 0)
			return 1;
	}
	return 0;
}

/*
 * Issue #144: is this chain's in-flight job a cache/artifact hit?
 *
 * Lets main.c skip spawning a build container for a package whose
 * content is already on disk -- see the call site for why that
 * container was never harmless.
 */
int pkg_chain_is_cache_hit(int chain_idx)
{
	struct pkg_entry *e;

	if (chain_idx < 0 || chain_idx >= PKG_MAX_CONCURRENT_JOBS)
		return 0;
	if (g_chains[chain_idx].name[0] == '\0')
		return 0;
	e = pkg_find(g_chains[chain_idx].name, g_chains[chain_idx].image);
	return (e != NULL && e->cache_hit) ? 1 : 0;
}

enum pkg_error pkg_get_one(const char *name, const char *image, struct json_writer *w)
{
	struct pkg_entry *e = pkg_find(name, image);

	if (e == NULL)
		return PKG_ERR_NOT_FOUND;
	write_pkg_json(e, w);
	return PKG_OK;
}

/*
 * Issue #129: the installed version of a hostbuild package and the
 * directory its harvested output lives in (ADR-0056's
 * g_artifacts_dir/<name>/), so an exporter can find the bytes without
 * main.c having to learn that layout -- the artifact-directory
 * convention stays owned here, exactly like artifact_path in
 * write_pkg_json() already is.
 *
 * Returns PKG_ERR_NOT_FOUND when there is no such hostbuild package,
 * PKG_ERR_INVALID_STATE when one exists but is not INSTALLED (a
 * failed or in-flight build has no artifact to export), PKG_OK
 * otherwise.
 */
enum pkg_error pkg_hostbuild_artifact_info(const char *name, char *out_version,
                                           size_t out_version_size, char *out_dir,
                                           size_t out_dir_size)
{
	struct pkg_entry *e = pkg_find(name, PKG_HOSTBUILD_IMAGE);

	if (e == NULL)
		return PKG_ERR_NOT_FOUND;
	if (e->state != PKG_STATE_INSTALLED)
		return PKG_ERR_NOT_INSTALLED;
	snprintf(out_version, out_version_size, "%s", e->version);
	snprintf(out_dir, out_dir_size, "%s/%s", g_artifacts_dir, e->name);
	return PKG_OK;
}

/* image_produce_new_version()'s own mutate() callback for pkg_delete()
 * -- unlinks ctx's own saved file list from the copy-forwarded staging
 * rootfs. */
struct delete_mutate_ctx {
	const struct pkg_entry *e;
};

static int delete_mutate(const char *staging_rootfs, void *ctx_v)
{
	struct delete_mutate_ctx *ctx = ctx_v;

	unlink_manifest_files(ctx->e, staging_rootfs);
	return 0;
}

/*
 * Issue #165: a hostbuild's real output is a directory of files, and
 * deleting the package record left every one of them on disk.
 *
 * An ordinary package is removed by producing a new image version with
 * its files unlinked (ADR-0107/0108). A hostbuild has no image to
 * unlink from -- its output lives at <artifacts>/<name>/ -- so the
 * delete cleared the record and nothing else. That is not merely
 * untidy: those paths are consumed BY PATH, not by package.
 * iso_build_start() reads <artifacts>/kernel/bzImage directly, and a
 * deploy passes --kernel=<artifact_path>/bzImage. So an uninstalled
 * hostbuild stayed fully deployable and bootable, with GET /v1/pkg
 * showing nothing at all to explain where the bytes came from.
 *
 * Removing it is safe for an already-deployed slot: a deploy COPIES
 * these bytes into the slot, so the running system holds its own copy
 * and does not read this path again. What disappears is only the
 * ability to deploy this artifact AGAIN -- which is exactly what
 * "uninstalled" should mean, and the property that was missing.
 *
 * Failure to remove is reported, not swallowed. A half-removed
 * artifact directory is the one outcome worse than either extreme:
 * the record says gone, the bytes say deployable, and nothing says
 * which.
 */
static int remove_hostbuild_artifacts(const char *name, char *out_err, size_t err_size)
{
	char artifact_dir[PATH_MAX];
	struct stat st;

	snprintf(artifact_dir, sizeof(artifact_dir), "%s/%s", g_artifacts_dir, name);
	if (stat(artifact_dir, &st) != 0) {
		/* Never built, or already gone -- both are the desired end
		 * state, so neither is an error. */
		return 0;
	}
	if (persist_remove_tree(artifact_dir) != 0) {
		snprintf(out_err, err_size, "%s", strerror(errno));
		return -1;
	}
	return 0;
}

enum pkg_error pkg_delete(const char *name, const char *image)
{
	struct pkg_entry *e = pkg_find(name, image);
	struct pkg_entry saved;
	struct delete_mutate_ctx ctx;

	if (e == NULL || (e->state != PKG_STATE_INSTALLED && e->state != PKG_STATE_FAILED))
		return PKG_ERR_NOT_FOUND;
	{
		int i;

		for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
			if (strcmp(g_chains[i].name, name) == 0 &&
			    strcmp(g_chains[i].image, normalize_image(image)) == 0)
				return PKG_ERR_BUSY;
		}
	}

	/*
	 * A PKG_STATE_FAILED entry never had its files actually committed
	 * into any real, persisted image version: either the failure
	 * happened before pkg_build_completed()'s own merge step ever ran
	 * (e->files still empty -- the common case, a fetch or build
	 * failure), or it happened inside image_produce_new_version()
	 * itself, after install_mutate()'s own merge_tree() had already
	 * populated e->files as a side effect of writing into that
	 * attempt's own scratch staging directory --
	 * image_produce_new_version() always discards that staging
	 * directory on any failure (see its own comment) before ever
	 * renaming it into a real version, so e->files in that case
	 * describes content that was already deleted along with the
	 * staging copy, not anything an operator can actually see in the
	 * image. Either way there is nothing to unlink from a real image
	 * and no new image version to produce for this removal --
	 * image_produce_new_version()/delete_mutate() below is for the
	 * PKG_STATE_INSTALLED case only; a failed entry is cleared
	 * directly. save_state() is harmless-but-not-load-bearing here
	 * (only PKG_STATE_INSTALLED entries are ever persisted), kept for
	 * consistency with the branch below.
	 */
	if (e->state == PKG_STATE_FAILED) {
		char rmerr[128];

		/*
		 * A FAILED hostbuild can still have left a partial artifact
		 * directory behind -- the build got far enough to write
		 * something and then died. Those bytes are consumed by path
		 * like any other, so leaving them is the same bug in its
		 * worst form: output from a build that is on record as having
		 * failed.
		 */
		if (strcmp(normalize_image(image), PKG_HOSTBUILD_IMAGE) == 0 &&
		    remove_hostbuild_artifacts(name, rmerr, sizeof(rmerr)) != 0) {
			logstore_write("cixd", "error",
			               "pkg delete: %s: could not remove the artifact directory: %s -- the "
			               "package record was left in place so the two do not disagree",
			               name, rmerr);
			return PKG_ERR_PERSIST_FAILED;
		}
		pkg_entry_free_files(e);
		memset(e, 0, sizeof(*e));
		return save_state() == 0 ? PKG_OK : PKG_ERR_PERSIST_FAILED;
	}

	/*
	 * Uninstalling a package must, like an install/upgrade, produce a
	 * new immutable image version (ADR-0107/0108) rather than mutating
	 * a version's rootfs that an already-running container might have
	 * open as its own overlay lowerdir right now. e's own registry
	 * slot is cleared BEFORE image_produce_new_version() runs (a
	 * struct copy captures e->files' pointer into saved first, so
	 * nothing is freed yet) so build_image_manifest_string()'s own
	 * fresh g_packages scan -- which only runs after delete_mutate()
	 * has unlinked the files -- correctly excludes this package from
	 * the new version's manifest.
	 */
	/*
	 * Removed BEFORE the record is cleared, deliberately. If this
	 * fails, the package stays installed and still describes the bytes
	 * that are still there -- the two agree. Clearing the record first
	 * and failing here would leave exactly the state this issue is
	 * about: no package, and a fully deployable artifact.
	 */
	if (strcmp(normalize_image(image), PKG_HOSTBUILD_IMAGE) == 0) {
		char rmerr[128];

		if (remove_hostbuild_artifacts(name, rmerr, sizeof(rmerr)) != 0) {
			logstore_write("cixd", "error",
			               "pkg delete: %s: could not remove the artifact directory: %s -- the "
			               "package was left installed so its record still describes the bytes "
			               "on disk",
			               name, rmerr);
			return PKG_ERR_PERSIST_FAILED;
		}
	}

	saved = *e;
	memset(e, 0, sizeof(*e));
	ctx.e = &saved;

	if (image_produce_new_version(normalize_image(image), delete_mutate, &ctx, NULL) != 0) {
		*e = saved; /* the uninstall never actually happened -- restore e exactly */
		return PKG_ERR_PERSIST_FAILED;
	}

	pkg_entry_free_files(&saved);

	if (save_state() != 0)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

/* ---- pkg/ redesign Part 2 (ADR-0121): configurable repo + pkg sync ---- */

static char g_repo_config_path[PATH_MAX];
static char g_repo_url[PKGREPO_URL_MAX];
static char g_repo_kind[PKGREPO_KIND_MAX] = "gitea";
static char g_repo_ref[PKGREPO_REF_MAX] = "master";
static char g_repo_auth_token[PKGREPO_TOKEN_MAX];


static const char *pkg_repo_token(void)
{
	return g_repo_auth_token;
}
static int g_repo_legacy_sync_interval; /* ADR-0257 migration only */

static pid_t g_sync_pid = -1;
enum sync_state { SYNC_NEVER = 0, SYNC_RUNNING, SYNC_SUCCESS, SYNC_FAILED };
static enum sync_state g_sync_last_state = SYNC_NEVER;
static time_t g_sync_last_attempt;
static int g_sync_last_added;
static int g_sync_last_approved; /* #404 */
static int g_sync_last_skipped;
/*
 * Issue #59: one recipe version this sync is allowed to REPLACE rather
 * than skip. Recipe-version immutability is a sound default -- ADR-0107's
 * resolution rules depend on it -- but during active development of a
 * recipe the only workaround was burning a new version number per
 * iteration, and the catalogue really did accumulate five dead kernel
 * pins and four dead gcc pins from a single investigation.
 *
 * Deliberately a single, explicit name@version rather than a blanket
 * "refetch everything" flag: an operator correcting one recipe they are
 * working on is a different act from silently rewriting history for a
 * catalogue other boxes resolve against. One-shot -- cleared as soon as
 * the sync that asked for it completes, so it can never leak into the
 * periodic background sync.
 */
static char g_sync_refetch_name[PKG_NAME_MAX];
static char g_sync_refetch_version[PKG_VERSION_MAX];
static char g_sync_last_error[PKG_ERROR_MAX];

static int repo_kind_is_valid(const char *kind)
{
	return strcmp(kind, "gitea") == 0 || strcmp(kind, "github") == 0 ||
	       strcmp(kind, "gitlab") == 0;
}

static int save_repo_config(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "repo_url");
	jw_str(&w, g_repo_url);
	jw_key(&w, "repo_kind");
	jw_str(&w, g_repo_kind);
	jw_key(&w, "ref");
	jw_str(&w, g_repo_ref);
	jw_key(&w, "auth_token");
	jw_str(&w, g_repo_auth_token);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = persist_atomic_write(g_repo_config_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

/* ADR-0141 Phase 4: path-only repoint -- see pkg_repoint()'s own doc
 * comment for the shared reasoning. */
void pkg_repo_repoint(const char *new_config_path)
{
	snprintf(g_repo_config_path, sizeof(g_repo_config_path), "%s", new_config_path);
}

int pkg_repo_init(const char *config_path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *interval;
	const char *s;

	if (snprintf(g_repo_config_path, sizeof(g_repo_config_path), "%s", config_path) >=
	    (int)sizeof(g_repo_config_path))
		return -1;

	g_repo_url[0] = '\0';
	snprintf(g_repo_kind, sizeof(g_repo_kind), "gitea");
	snprintf(g_repo_ref, sizeof(g_repo_ref), "master");
	g_repo_auth_token[0] = '\0';
	g_repo_legacy_sync_interval = 0;

	if (persist_read_file(config_path, &buf, &len) != 0 || buf == NULL)
		return 0; /* no persisted config yet -- defaults stand */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return 0;

	s = json_as_string(json_object_get(root, "repo_url"));
	if (s != NULL)
		snprintf(g_repo_url, sizeof(g_repo_url), "%s", s);
	s = json_as_string(json_object_get(root, "repo_kind"));
	if (s != NULL && repo_kind_is_valid(s))
		snprintf(g_repo_kind, sizeof(g_repo_kind), "%s", s);
	s = json_as_string(json_object_get(root, "ref"));
	if (s != NULL && s[0] != '\0')
		snprintf(g_repo_ref, sizeof(g_repo_ref), "%s", s);
	s = json_as_string(json_object_get(root, "auth_token"));
	if (s != NULL)
		snprintf(g_repo_auth_token, sizeof(g_repo_auth_token), "%s", s);
	/* An older file still carries this; it is migrated into a schedule
	 * once and then never written again (ADR-0257). */
	interval = json_object_get(root, "sync_interval_seconds");
	if (interval != NULL)
		g_repo_legacy_sync_interval = (int)json_as_number(interval);

	json_free(root);
	return 0;
}

void pkg_repo_write_json_config(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "repo_url");
	jw_str(w, g_repo_url);
	jw_key(w, "repo_kind");
	jw_str(w, g_repo_kind);
	jw_key(w, "ref");
	jw_str(w, g_repo_ref);
	jw_key(w, "auth_token_set");
	jw_bool(w, g_repo_auth_token[0] != '\0');
	jw_obj_close(w);
}

enum pkg_error pkg_repo_set_config(const char *repo_url, const char *repo_kind, const char *ref,
                                    const char *auth_token)
{
	if (repo_kind != NULL && !repo_kind_is_valid(repo_kind))
		return PKG_ERR_INVALID_NAME;

	if (repo_url != NULL)
		snprintf(g_repo_url, sizeof(g_repo_url), "%s", repo_url);
	if (repo_kind != NULL)
		snprintf(g_repo_kind, sizeof(g_repo_kind), "%s", repo_kind);
	if (ref != NULL && ref[0] != '\0')
		snprintf(g_repo_ref, sizeof(g_repo_ref), "%s", ref);
	if (auth_token != NULL)
		snprintf(g_repo_auth_token, sizeof(g_repo_auth_token), "%s", auth_token);
	if (save_repo_config() != 0)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

int pkg_repo_legacy_sync_interval_seconds(void)
{
	return g_repo_legacy_sync_interval;
}

void pkg_repo_clear_legacy_sync_interval(void)
{
	g_repo_legacy_sync_interval = 0;
	save_repo_config();
}

int pkg_repo_is_configured(void)
{
	return g_repo_url[0] != '\0';
}

/*
 * Splits g_repo_url ("<scheme>://<host>/<owner>/<repo>[.git][/...]")
 * into its parts. Only the FIRST TWO path segments are ever owner/repo
 * -- anything after is ignored, not folded into "owner". This is
 * deliberately lenient, not just simple: a real, confirmed bug (found
 * live, not in review) was a naive "split at the LAST slash" version
 * silently mis-parsing a real browser browse URL an operator pasted
 * verbatim (e.g. ".../itdlabs/cix/src/branch/master/recipes/package",
 * exactly what a forge's own address bar shows while browsing a repo
 * -- an entirely natural thing to copy-paste) into a garbage owner
 * ("itdlabs/cix/src/branch/master/pkg") and repo ("recipes"),
 * which then failed at fetch time as an opaque 404/curl-exit-22 with
 * no clue the URL itself was the problem. Taking the first two
 * segments and discarding the rest accepts that exact paste directly,
 * matching what any reasonable operator would expect "give it the
 * repo URL" to mean. A bare "owner/repo" (no nested subgroup) still
 * covers every kind this project actually talks to (gitea/github
 * always; gitlab subgroup paths are a real gap, out of v1 scope --
 * flagged here, not silently mishandled: a subgroup path just ends up
 * with the subgroup's own first segment folded into "owner", which
 * fails cleanly at fetch time rather than doing something wrong
 * quietly).
 */
static int parse_repo_url(char *out_scheme, size_t scheme_sz, char *out_host, size_t host_sz,
                           char *out_owner, size_t owner_sz, char *out_repo, size_t repo_sz)
{
	const char *scheme_end = strstr(g_repo_url, "://");
	const char *host_start, *host_end, *path;
	char path_buf[PKGREPO_URL_MAX];
	char *owner_start, *slash1, *slash2, *repo_start;
	size_t repo_len;

	if (scheme_end == NULL)
		return -1;
	snprintf(out_scheme, scheme_sz, "%.*s", (int)(scheme_end - g_repo_url), g_repo_url);

	host_start = scheme_end + 3;
	host_end = strchr(host_start, '/');
	if (host_end == NULL || host_end == host_start)
		return -1;
	snprintf(out_host, host_sz, "%.*s", (int)(host_end - host_start), host_start);

	path = host_end + 1;
	snprintf(path_buf, sizeof(path_buf), "%s", path);

	owner_start = path_buf;
	slash1 = strchr(owner_start, '/');
	if (slash1 == NULL || slash1 == owner_start)
		return -1;
	*slash1 = '\0';
	snprintf(out_owner, owner_sz, "%s", owner_start);

	repo_start = slash1 + 1;
	slash2 = strchr(repo_start, '/');
	if (slash2 != NULL)
		*slash2 = '\0'; /* everything past the repo segment is ignored, see above */

	repo_len = strlen(repo_start);
	if (repo_len > 4 && strcmp(repo_start + repo_len - 4, ".git") == 0) {
		repo_start[repo_len - 4] = '\0';
		repo_len -= 4;
	}
	if (repo_len == 0)
		return -1;
	snprintf(out_repo, repo_sz, "%s", repo_start);
	return out_repo[0] != '\0' && out_owner[0] != '\0' ? 0 : -1;
}

/*
 * Three small, explicitly separate branches, one per forge -- not one
 * "generic REST archive API" abstraction, because there isn't one:
 * gitea/github/gitlab each have a genuinely different URL shape and
 * auth convention (confirmed directly before choosing this design,
 * see ADR-0121). Only gitea (git.home.arpa) is actually reachable
 * from this project's own dev/test environment; github/gitlab follow
 * the identical documented pattern each forge's own API reference
 * specifies, but are unverified against a real github.com/gitlab.com
 * account here -- flagged honestly, not silently assumed working.
 */
static int build_sync_fetch_request(char *out_url, size_t out_url_size, char *out_header,
                                     size_t out_header_size)
{
	char scheme[16], host[256], owner[256], repo[256];
	const char *ref = g_repo_ref[0] != '\0' ? g_repo_ref : "master";

	out_header[0] = '\0';
	if (!pkg_repo_is_configured())
		return -1;
	if (parse_repo_url(scheme, sizeof(scheme), host, sizeof(host), owner, sizeof(owner), repo,
	                    sizeof(repo)) != 0)
		return -1;

	if (strcmp(g_repo_kind, "gitea") == 0) {
		/* Token-in-URL basic auth, same proven convention ADR-0057's
		 * own cix.recipe pkg_source already relies on. */
		if (g_repo_auth_token[0] != '\0')
			snprintf(out_url, out_url_size, "%s://%s@%s/api/v1/repos/%s/%s/archive/%s.tar.gz",
			         scheme, g_repo_auth_token, host, owner, repo, ref);
		else
			snprintf(out_url, out_url_size, "%s://%s/api/v1/repos/%s/%s/archive/%s.tar.gz",
			         scheme, host, owner, repo, ref);
	} else if (strcmp(g_repo_kind, "github") == 0) {
		/* github.com's own REST API is always at the separate
		 * api.github.com host regardless of what host the repo URL
		 * itself names; GitHub Enterprise Server uses <host>/api/v3/
		 * instead -- both per GitHub's own published API docs. */
		if (strcmp(host, "github.com") == 0)
			snprintf(out_url, out_url_size, "https://api.github.com/repos/%s/%s/tarball/%s",
			         owner, repo, ref);
		else
			snprintf(out_url, out_url_size, "%s://%s/api/v3/repos/%s/%s/tarball/%s", scheme,
			         host, owner, repo, ref);
		if (g_repo_auth_token[0] != '\0')
			snprintf(out_header, out_header_size, "Authorization: Bearer %s", g_repo_auth_token);
	} else if (strcmp(g_repo_kind, "gitlab") == 0) {
		/* Unlike github, gitlab.com and a self-hosted GitLab instance
		 * both use the identical <host>/api/v4/ convention -- no
		 * separate-domain special case needed here. */
		snprintf(out_url, out_url_size,
		         "%s://%s/api/v4/projects/%s%%2F%s/repository/archive.tar.gz?sha=%s", scheme,
		         host, owner, repo, ref);
		if (g_repo_auth_token[0] != '\0')
			snprintf(out_header, out_header_size, "PRIVATE-TOKEN: %s", g_repo_auth_token);
	} else {
		return -1;
	}
	return 0;
}

static void sync_state_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/sync.tar.gz", g_pkg_dir);
}

/*
 * Issue #59: arms a one-shot refetch for the NEXT sync only. Empty
 * name clears it. Returns -1 if the version is missing, since
 * "refetch this whole package" is not a thing that can be asked for --
 * immutability is per version, so the escape hatch is too.
 */
int pkg_sync_set_refetch(const char *name, const char *version)
{
	if (name == NULL || name[0] == '\0') {
		g_sync_refetch_name[0] = '\0';
		g_sync_refetch_version[0] = '\0';
		return 0;
	}
	if (version == NULL || version[0] == '\0')
		return -1;
	snprintf(g_sync_refetch_name, sizeof(g_sync_refetch_name), "%s", name);
	snprintf(g_sync_refetch_version, sizeof(g_sync_refetch_version), "%s", version);
	return 0;
}

/* Sidecar the child writes its curlfetch_perform() error to on failure
 * (#410) -- pkg_sync_fetch_done() otherwise has only an exit code. */
static void sync_fetch_err_path(char *out, size_t out_size)
{
	char tarball_path[PATH_MAX];

	sync_state_path(tarball_path, sizeof(tarball_path));
	snprintf(out, out_size, "%s.err", tarball_path);
}

enum pkg_error pkg_sync_start(pid_t *out_pid, int *out_pidfd)
{
	char url[PKGREPO_URL_MAX + PKGREPO_TOKEN_MAX + 64];
	char header[320];
	char tarball_path[PATH_MAX];
	char err_path[PATH_MAX];
	pid_t pid;
	int pidfd;

	if (g_sync_pid > 0)
		return PKG_ERR_BUSY;
	if (build_sync_fetch_request(url, sizeof(url), header, sizeof(header)) != 0)
		return PKG_ERR_NOT_FOUND;

	sync_state_path(tarball_path, sizeof(tarball_path));
	unlink(tarball_path);
	sync_fetch_err_path(err_path, sizeof(err_path));
	unlink(err_path);

	pid = fork();
	if (pid < 0)
		return PKG_ERR_SPAWN_FAILED;
	if (pid == 0) {
		struct curlfetch_opts opts;
		char curl_err[256];

		memset(&opts, 0, sizeof(opts));
		opts.url = url;
		opts.path = tarball_path;
		opts.header1 = header[0] != '\0' ? header : NULL;
		opts.connect_timeout = PKG_CURL_CONNECT_TIMEOUT_SECS;
		opts.low_speed_limit = PKG_CURL_LOW_SPEED_LIMIT;
		opts.low_speed_time = PKG_CURL_LOW_SPEED_TIME_SECS;
		if (curlfetch_perform(&opts, NULL, curl_err, sizeof(curl_err)) != 0) {
			int efd;

			/* url carries the real repo token in its own userinfo
			 * (gitea's token-in-URL convention, above) -- a libcurl
			 * error string can legitimately echo the URL back
			 * (CURLE_URL_MALFORMAT and friends), so this must be
			 * redacted before it reaches a file at all, the same
			 * "both sides of persistence" discipline #405/#60 already
			 * established for recipe content. */
			redact_repo_token(curl_err, sizeof(curl_err));
			efd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
			if (efd >= 0) {
				ssize_t ignored = write(efd, curl_err, strlen(curl_err));

				(void)ignored;
				close(efd);
			}
			_exit(1);
		}
		_exit(0);
	}

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return PKG_ERR_SPAWN_FAILED;
	}

	g_sync_pid = pid;
	g_sync_last_state = SYNC_RUNNING;
	g_sync_last_attempt = time(NULL);
	g_sync_last_error[0] = '\0';
	*out_pid = pid;
	*out_pidfd = pidfd;
	return PKG_OK;
}

/*
 * Image recipes (ADR-0123) are versioned in git for the same reason
 * package recipes are (a real revision history as the declared
 * package list changes over time, ADR-0149) but the daemon's own
 * image_recipe_add() stores exactly one current manifest per image
 * name -- there is no "pin to an old image recipe version" concept
 * at the API layer, since an image's actual build result is already
 * content-addressed independently (ADR-0108). So sync only ever
 * needs the highest version per name directory, same selection rule
 * find_recipe_path() already uses for an unpinned package install.
 */
static void sync_walk_image_recipes(const char *images_root, int *added, int *skipped,
                                     int *failed)
{
	DIR *names_d;
	struct dirent *name_de;

	names_d = opendir(images_root);
	if (names_d == NULL)
		return;
	while ((name_de = readdir(names_d)) != NULL) {
		char name_dir[PATH_MAX];
		char best_version[PKG_VERSION_MAX];
		char script_path[PATH_MAX];
		char *content;
		size_t content_len;
		DIR *vd;
		struct dirent *vde;
		int have_best = 0;
		enum pkg_error rc;

		if (name_de->d_name[0] == '.')
			continue;
		snprintf(name_dir, sizeof(name_dir), "%s/%s", images_root, name_de->d_name);
		vd = opendir(name_dir);
		if (vd == NULL)
			continue;
		while ((vde = readdir(vd)) != NULL) {
			char candidate[PATH_MAX];
			struct stat st;

			if (vde->d_name[0] == '.')
				continue;
			snprintf(candidate, sizeof(candidate), "%s/%s/build.sh", name_dir, vde->d_name);
			if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			if (!have_best || pkg_version_compare(vde->d_name, best_version) > 0) {
				snprintf(best_version, sizeof(best_version), "%s", vde->d_name);
				have_best = 1;
			}
		}
		closedir(vd);
		if (!have_best)
			continue;

		snprintf(script_path, sizeof(script_path), "%s/%s/build.sh", name_dir, best_version);
		if (persist_read_file(script_path, &content, &content_len) != 0 || content == NULL)
			continue;
		rc = image_recipe_add(name_de->d_name, content);
		free(content);
		/* image_recipe_add() always overwrites (name-keyed, no
		 * version-keying of its own, ADR-0123) -- it never returns
		 * PKG_ERR_DUPLICATE the way pkg_recipe_add()'s immutable
		 * package versions can. Every successful sync of an image
		 * recipe therefore lands in "added", even on a re-sync of
		 * identical content; the skipped branch is kept for the same
		 * counting shape as the package walk below, not because it
		 * can currently trigger. */
		if (rc == PKG_OK)
			(*added)++;
		else if (rc == PKG_ERR_DUPLICATE)
			(*skipped)++;
		else
			(*failed)++;
	}
	closedir(names_d);
}

/*
 * Same shape as sync_walk_image_recipes() above -- highest version per
 * name directory wins, container_recipe_add() always overwrites (no
 * version-keying at the daemon layer, ADR-0151, mirroring ADR-0123's
 * image-recipe design) -- but the leaf filename is container.json,
 * not build.sh, since a container recipe's content is a JSON body,
 * never a shell script.
 */
static void sync_walk_container_recipes(const char *containers_root, int *added, int *skipped,
                                         int *failed)
{
	DIR *names_d;
	struct dirent *name_de;

	names_d = opendir(containers_root);
	if (names_d == NULL)
		return;
	while ((name_de = readdir(names_d)) != NULL) {
		char name_dir[PATH_MAX];
		char best_version[PKG_VERSION_MAX];
		char json_path[PATH_MAX];
		char *content;
		size_t content_len;
		DIR *vd;
		struct dirent *vde;
		int have_best = 0;
		enum pkg_error rc;

		if (name_de->d_name[0] == '.')
			continue;
		snprintf(name_dir, sizeof(name_dir), "%s/%s", containers_root, name_de->d_name);
		vd = opendir(name_dir);
		if (vd == NULL)
			continue;
		while ((vde = readdir(vd)) != NULL) {
			char candidate[PATH_MAX];
			struct stat st;

			if (vde->d_name[0] == '.')
				continue;
			snprintf(candidate, sizeof(candidate), "%s/%s/container.json", name_dir,
			         vde->d_name);
			if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			if (!have_best || pkg_version_compare(vde->d_name, best_version) > 0) {
				snprintf(best_version, sizeof(best_version), "%s", vde->d_name);
				have_best = 1;
			}
		}
		closedir(vd);
		if (!have_best)
			continue;

		snprintf(json_path, sizeof(json_path), "%s/%s/container.json", name_dir, best_version);
		if (persist_read_file(json_path, &content, &content_len) != 0 || content == NULL)
			continue;
		rc = container_recipe_add(name_de->d_name, content);
		free(content);
		if (rc == PKG_OK)
			(*added)++;
		else if (rc == PKG_ERR_DUPLICATE)
			(*skipped)++;
		else
			(*failed)++;
	}
	closedir(names_d);
}

/*
 * ADR-0278 (#367): the forkable half of a sync -- clear the extraction
 * directory and unpack the archive into it.
 *
 * Split out because this is where the five seconds went. The archive is
 * the whole recipe repository and unpacking it ran synchronously on the
 * event loop, every six hours, on a daemon that is pid 1 and whose loop
 * is the only way into the host. Measured stalls of 5062-6116 ms at
 * 03:23, 09:23, 15:23 and 21:23 across four days, against a threshold
 * the daemon itself calls slow at 750 ms.
 *
 * Everything here is filesystem-effecting ONLY, which is what makes it
 * safe to run in a fork: it touches no global the parent will read
 * afterwards. The recipe merge below is the opposite -- it calls
 * pkg_recipe_add() and reads g_sync_refetch_* -- so it stays in the
 * parent, where it is also fast (a few hundred small files).
 *
 * Returns 0 on success. Callable from a helper child or, if the fork
 * fails, inline from the parent: it behaves identically either way.
 */
void pkg_trusted_keys_init(const char *dir)
{
	snprintf(g_trusted_keys_dir, sizeof(g_trusted_keys_dir), "%s", dir);
}

int pkg_sync_extract(void)
{
	char tarball_path[PATH_MAX];
	char extract_dir[PATH_MAX];

	sync_state_path(tarball_path, sizeof(tarball_path));
	snprintf(extract_dir, sizeof(extract_dir), "%s/sync-extract", g_pkg_dir);
	cix_btrfs_subvol_delete_or_rmtree(extract_dir);
	if (persist_mkdir_p(extract_dir) != 0)
		return -1;
	if (extract_tarball(tarball_path, extract_dir) != 0)
		return -2;
	return 0;
}

/*
 * Where the forked half leaves its tallies for the parent.
 *
 * A fork cannot hand back an int triple through memory, and the exit
 * status has room for one small number, not three. A file is the least
 * machinery that carries them, and the extraction directory is already
 * this sync's own scratch space.
 */
static void sync_counts_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/sync-extract.counts", g_pkg_dir);
}

static void sync_counts_write(int added, int approved, int skipped, int failed)
{
	char path[PATH_MAX];
	char buf[80];
	int len = snprintf(buf, sizeof(buf), "%d %d %d %d\n", added, approved, skipped, failed);

	if (len <= 0)
		return;
	sync_counts_path(path, sizeof(path));
	persist_atomic_write(path, buf, (size_t)len);
}

static void sync_counts_read(int *added, int *approved, int *skipped, int *failed)
{
	char path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;

	*added = *approved = *skipped = *failed = 0;
	sync_counts_path(path, sizeof(path));
	if (persist_read_file(path, &buf, &len) == 0 && buf != NULL) {
		sscanf(buf, "%d %d %d %d", added, approved, skipped, failed);
		free(buf);
	}
	unlink(path);
}

/*
 * The fetch finished. Records the outcome and says whether the caller
 * should now run pkg_sync_extract() (1) or whether this sync is already
 * over (0) -- ADR-0278 split the extraction out so it can go into a
 * helper process, and this is the part that must run on the loop
 * because it owns the sync's state.
 */
int pkg_sync_fetch_done(int exit_status)
{
	char tarball_path[PATH_MAX];

	g_sync_pid = -1;
	sync_state_path(tarball_path, sizeof(tarball_path));
	if (exit_status != 0) {
		char err_path[PATH_MAX];
		char curl_err[256] = "";
		int efd;

		sync_fetch_err_path(err_path, sizeof(err_path));
		efd = open(err_path, O_RDONLY);
		if (efd >= 0) {
			ssize_t n = read(efd, curl_err, sizeof(curl_err) - 1);

			curl_err[n > 0 ? n : 0] = '\0';
			close(efd);
			unlink(err_path);
		}
		g_sync_last_state = SYNC_FAILED;
		if (curl_err[0] != '\0')
			snprintf(g_sync_last_error, sizeof(g_sync_last_error), "fetch failed: %s", curl_err);
		else
			snprintf(g_sync_last_error, sizeof(g_sync_last_error), "fetch failed (exit %d)",
			         exit_status);
		unlink(tarball_path);
		return 0;
	}
	return 1;
}

/*
 * The extraction finished (in a helper, or inline when the fork
 * failed). `rc` is pkg_sync_extract()'s own return. Everything from
 * here on touches in-memory state and therefore runs on the loop.
 */
/*
 * ADR-0278 (#367), corrected: the forkable half is the WHOLE of this,
 * not just the extraction.
 *
 * The first attempt moved only `extract_tarball()` into a helper on the
 * assumption that unpacking the archive was where the five seconds
 * went. Measured after deploying it: a real sync still froze the loop
 * for 4991 ms. The merge is the heavy half -- roughly 1300 recipe files
 * read, parsed and written -- and the extraction was never the problem.
 *
 * The one thing that stopped this being forkable is
 * pkg_recipe_add()'s call to queue_rolling_rebuilds_for(), which
 * mutates the in-memory rebuild queue: a child would fill its copy and
 * exit. That is exactly what pkg_rebuild_queue_rederive() (#373)
 * reconstructs, from the images and their drift, so the parent rebuilds
 * the queue after the child finishes rather than the child trying to
 * hand it back. One source of truth, and the function already exists.
 *
 * The tallies come back through a file, because a fork cannot return an
 * int triple and the exit status has room for one small number.
 */
int pkg_sync_merge(void)
{
	char extract_dir[PATH_MAX];
	char recipes_root[PATH_MAX];
	char images_root[PATH_MAX];
	char containers_root[PATH_MAX];
	DIR *names_d;
	struct dirent *name_de;
	int added = 0, approved = 0, skipped = 0, failed = 0;

	snprintf(extract_dir, sizeof(extract_dir), "%s/sync-extract", g_pkg_dir);

	snprintf(recipes_root, sizeof(recipes_root), "%s/recipes/package", extract_dir);
	names_d = opendir(recipes_root);
	if (names_d != NULL) {
		while ((name_de = readdir(names_d)) != NULL) {
			char name_dir[PATH_MAX];
			DIR *vd;
			struct dirent *vde;

			if (name_de->d_name[0] == '.')
				continue;
			snprintf(name_dir, sizeof(name_dir), "%s/%s", recipes_root, name_de->d_name);
			vd = opendir(name_dir);
			if (vd == NULL)
				continue;
			while ((vde = readdir(vd)) != NULL) {
				char script_path[PATH_MAX];
				char version_dir[PATH_MAX];
				char *content;
				size_t content_len;
				enum pkg_error rc;

				if (vde->d_name[0] == '.')
					continue;
				/*
				 * ADR-0305: whichever language the repo holds this
				 * version in. Without this, a build.cbs committed to
				 * git would simply never reach a box -- the sync would
				 * walk straight past it and report nothing, which is
				 * the worst shape of failure this platform has: a
				 * recipe that exists, is correct, and is invisible.
				 */
				snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, vde->d_name);
				if (pkg_recipe_file_in(version_dir, script_path, sizeof(script_path), NULL, NULL) != 0)
					continue;
				if (persist_read_file(script_path, &content, &content_len) != 0 ||
				    content == NULL)
					continue;
				/* Issue #59: the one version this sync was explicitly
				 * asked to re-fetch is deleted first, so the add below
				 * takes the repo's current content instead of being
				 * skipped as a duplicate. Everything else keeps the
				 * immutability that ADR-0107 depends on. */
				if (g_sync_refetch_name[0] != '\0' &&
				    strcmp(g_sync_refetch_name, name_de->d_name) == 0 &&
				    strcmp(g_sync_refetch_version, vde->d_name) == 0)
					pkg_recipe_delete(name_de->d_name, vde->d_name);

				{
					int was_approval = 0;

					rc = pkg_recipe_add(name_de->d_name, content,
					                    recipe_path_is_pbs(script_path) ? PKG_RECIPE_PBS
					                                                    : PKG_RECIPE_SHELL,
					                    &was_approval);
					free(content);
					/* #404: an approval applied to an already-published
					 * version is NOT a new recipe -- count it apart so
					 * `pkg sync` does not report an approval as an add. */
					if (rc == PKG_OK && was_approval)
						approved++;
					else if (rc == PKG_OK)
						added++;
					else if (rc == PKG_ERR_DUPLICATE)
						skipped++;
					else
						failed++;
				}
			}
			closedir(vd);
		}
		closedir(names_d);
	}

	snprintf(images_root, sizeof(images_root), "%s/recipes/image", extract_dir);
	sync_walk_image_recipes(images_root, &added, &skipped, &failed);

	/*
	 * #371: deployments were "container recipes". A daemon still
	 * running the old build looks for recipes/container in the synced
	 * tarball, finds nothing, and adds nothing -- sync_walk_container_
	 * recipes() returns silently on opendir() failure and this sync
	 * never prunes, so the window between the git rename and the
	 * deploy that understands it costs one no-op cycle and cannot lose
	 * a published recipe. Measured before renaming rather than hoped.
	 */
	snprintf(containers_root, sizeof(containers_root), "%s/recipes/deployment", extract_dir);
	sync_walk_container_recipes(containers_root, &added, &skipped, &failed);

	/*
	 * The signing keys travel in the same tarball and were thrown away
	 * with it (ADR-0279). A repository with no docs/keys/ adopts
	 * nothing and is not an error -- an operator running their own
	 * recipe repo need not publish keys at all, and then this host
	 * simply verifies no artifact signature and builds from source,
	 * which is correct rather than degraded.
	 */
	if (g_trusted_keys_dir[0] != '\0') {
		char keys_root[PATH_MAX];
		int adopted;

		snprintf(keys_root, sizeof(keys_root), "%s/docs/keys", extract_dir);
		adopted = releasekey_trust_adopt(keys_root, g_trusted_keys_dir);
		if (adopted < 0)
			logstore_write("cixd", "warn",
			                "pkg sync: could not write the trusted-key store at %s -- "
			                "artifact signatures cannot be verified until it is writable",
			                g_trusted_keys_dir);
		else if (adopted > 0)
			logstore_write("cixd", "info", "pkg sync: %d release signing key%s trusted",
			                adopted, adopted == 1 ? "" : "s");
	}

	cix_btrfs_subvol_delete_or_rmtree(extract_dir);

	/* One-shot: cleared here so the periodic background sync can never
	 * inherit a refetch an operator asked for once. */
	g_sync_refetch_name[0] = '\0';
	g_sync_refetch_version[0] = '\0';

	sync_counts_write(added, approved, skipped, failed);
	return 0;
}

/*
 * The sync's last step, on the loop: adopt the tallies the merge left
 * behind, rebuild the rolling queue the child could not hand back, and
 * record the outcome.
 */
void pkg_sync_completed(int rc)
{
	int added = 0, approved = 0, skipped = 0, failed = 0;
	char tarball_path[PATH_MAX];

	sync_state_path(tarball_path, sizeof(tarball_path));
	unlink(tarball_path);

	if (rc != 0) {
		g_sync_last_state = SYNC_FAILED;
		snprintf(g_sync_last_error, sizeof(g_sync_last_error), "%s",
		         rc == -1 ? "could not create extraction directory"
		                  : "archive extraction or recipe merge failed");
		return;
	}

	sync_counts_read(&added, &approved, &skipped, &failed);

	/* One-shot: cleared here so the periodic background sync can never
	 * inherit a refetch an operator asked for once. The child cleared
	 * its own copy; this is the one that lasts. */
	g_sync_refetch_name[0] = '\0';
	g_sync_refetch_version[0] = '\0';

	/* #373's derivation, used for the reason it was built: the child
	 * queued rebuilds into a copy of the queue that died with it. */
	pkg_rebuild_queue_rederive();

	g_sync_last_state = SYNC_SUCCESS;
	g_sync_last_added = added;
	g_sync_last_approved = approved;
	g_sync_last_skipped = skipped;
	if (failed > 0)
		snprintf(g_sync_last_error, sizeof(g_sync_last_error),
		         "%d recipe(s) failed to add (invalid content)", failed);
	else
		g_sync_last_error[0] = '\0';
}

void pkg_sync_write_json_status(struct json_writer *w)
{
	const char *state_str = g_sync_last_state == SYNC_NEVER     ? "never"
	                         : g_sync_last_state == SYNC_RUNNING ? "running"
	                         : g_sync_last_state == SYNC_SUCCESS ? "success"
	                                                              : "failed";

	jw_obj_open(w);
	jw_key(w, "state");
	jw_str(w, state_str);
	jw_key(w, "last_attempt");
	if (g_sync_last_attempt > 0)
		jw_int(w, (long long)g_sync_last_attempt);
	else
		jw_null(w);
	jw_key(w, "added");
	jw_int(w, g_sync_last_added);
	jw_key(w, "approved"); /* #404 */
	jw_int(w, g_sync_last_approved);
	jw_key(w, "skipped");
	jw_int(w, g_sync_last_skipped);
	jw_key(w, "error");
	if (g_sync_last_error[0] != '\0')
		jw_str(w, g_sync_last_error);
	else
		jw_null(w);
	jw_obj_close(w);
}

int pkg_sync_in_progress(void)
{
	return g_sync_pid > 0;
}

/* ---- pkg/ redesign Part 3 (ADR-0122): local build-artifact cache + LRU eviction ---- */

static char g_cache_dir[PATH_MAX];
static char g_cache_config_path[PATH_MAX];
static long long g_cache_max_bytes = PKG_CACHE_DEFAULT_MAX_BYTES;

static void cache_tarball_path(const char *name, const char *version, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s-%s.tar.gz", g_cache_dir, name, version);
}

/* Beside the artifact it signs, named the way the cache names it. */
static void cache_signature_path(const char *name, const char *version, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s-%s.tar.gz.minisig", g_cache_dir, name, version);
}

static int save_cache_config(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "max_bytes");
	jw_int(&w, g_cache_max_bytes);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = persist_atomic_write(g_cache_config_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

/* ADR-0141 Phase 4: path-only repoint -- see pkg_repoint()'s own doc
 * comment for the shared reasoning. */
void pkg_cache_repoint(const char *new_cache_dir, const char *new_config_path)
{
	snprintf(g_cache_dir, sizeof(g_cache_dir), "%s", new_cache_dir);
	snprintf(g_cache_config_path, sizeof(g_cache_config_path), "%s", new_config_path);
}

int pkg_cache_init(const char *cache_dir, const char *config_path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *mb;

	if (snprintf(g_cache_dir, sizeof(g_cache_dir), "%s", cache_dir) >= (int)sizeof(g_cache_dir))
		return -1;
	if (snprintf(g_cache_config_path, sizeof(g_cache_config_path), "%s", config_path) >=
	    (int)sizeof(g_cache_config_path))
		return -1;
	g_cache_max_bytes = PKG_CACHE_DEFAULT_MAX_BYTES;

	if (persist_read_file(config_path, &buf, &len) != 0 || buf == NULL)
		return 0; /* no persisted config yet -- the default cap stands */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return 0;

	mb = json_object_get(root, "max_bytes");
	if (mb != NULL) {
		double v = json_as_number(mb);

		if (v > 0)
			g_cache_max_bytes = (long long)v;
	}
	json_free(root);
	return 0;
}

long long pkg_cache_get_max_bytes(void)
{
	return g_cache_max_bytes;
}

enum pkg_error pkg_cache_set_max_bytes(long long max_bytes)
{
	if (max_bytes <= 0)
		return PKG_ERR_INVALID_NAME;
	g_cache_max_bytes = max_bytes;
	if (save_cache_config() != 0)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

/* Scanned once per eviction pass -- a static, file-scope buffer (not a
 * stack array): PKG_CACHE_SCAN_MAX entries at this per-entry size is
 * real memory a stack frame shouldn't carry, but is trivial as static
 * data, the same tradeoff struct pkg_entry's own build_output_captured already makes. A
 * real cache holding more than this many distinct (name,version)
 * artifacts at once is not a case this project has ever seen -- a
 * real, generous bound, not an unbounded scan. */
#define PKG_CACHE_SCAN_MAX 4096
struct cache_scan_entry {
	char filename[PKG_NAME_MAX + PKG_VERSION_MAX + 16];
	time_t mtime;
	long long size;
};
static struct cache_scan_entry g_cache_scan_buf[PKG_CACHE_SCAN_MAX];
static int g_cache_scan_count;

static int cache_scan_cmp(const void *a, const void *b)
{
	const struct cache_scan_entry *ea = a, *eb = b;

	if (ea->mtime < eb->mtime)
		return -1;
	if (ea->mtime > eb->mtime)
		return 1;
	return 0;
}

static long long cache_scan(void)
{
	DIR *d;
	struct dirent *de;
	int count = 0;
	long long total = 0;

	d = opendir(g_cache_dir);
	if (d == NULL)
		return 0;
	while ((de = readdir(d)) != NULL && count < PKG_CACHE_SCAN_MAX) {
		char path[PATH_MAX];
		struct stat st;

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", g_cache_dir, de->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		snprintf(g_cache_scan_buf[count].filename, sizeof(g_cache_scan_buf[count].filename), "%s",
		         de->d_name);
		g_cache_scan_buf[count].mtime = st.st_mtime;
		g_cache_scan_buf[count].size = (long long)st.st_size;
		total += (long long)st.st_size;
		count++;
	}
	closedir(d);
	qsort(g_cache_scan_buf, (size_t)count, sizeof(g_cache_scan_buf[0]), cache_scan_cmp);
	g_cache_scan_count = count;
	return total;
}

/* Real LRU: evicts the least-recently-used (oldest mtime) entries
 * first until incoming_size will fit under g_cache_max_bytes -- called
 * before every new cache write (pkg_cache_save()/pkg_cache_save_from_
 * file()). A single artifact bigger than the whole configured cap is
 * simply never cached (the caller checks this itself before calling)
 * rather than evicting everything else to make room for one oversized
 * entry. */
static void cache_evict_lru_until_fits(long long incoming_size)
{
	long long total = cache_scan();
	int i;

	for (i = 0; i < g_cache_scan_count && total + incoming_size > g_cache_max_bytes; i++) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s", g_cache_dir, g_cache_scan_buf[i].filename);
		if (unlink(path) == 0)
			total -= g_cache_scan_buf[i].size;
	}
}

static int pkg_cache_has(const char *name, const char *version)
{
	char path[PATH_MAX];
	struct stat st;

	cache_tarball_path(name, version, path, sizeof(path));
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Marks a cache entry as just-used (bumps its mtime to now) -- called
 * both after a cache-hit install and right after a fresh save, so LRU
 * eviction genuinely reflects recency of use, not just insertion
 * order. Best-effort: a failure here (e.g. the entry vanished under a
 * concurrent evict) is never a reason to fail the install that
 * triggered it. */
static void pkg_cache_touch(const char *name, const char *version)
{
	char path[PATH_MAX];

	cache_tarball_path(name, version, path, sizeof(path));
	utime(path, NULL);
}

/*
 * Tars dest_dir's own content into the cache, keyed name-version,
 * evicting older entries first if needed to fit under the configured
 * cap. Best-effort, deliberately non-fatal: the real, authoritative
 * install (merging dest_dir into the target image) has already
 * succeeded by the time this runs -- the same "best effort, not the
 * primary install" posture pkg_build_completed()'s own g_pkgbuild_
 * rootfs merge already established for a comparable secondary write.
 */
static void pkg_cache_save(const char *name, const char *version, const char *dest_dir)
{
	char final_path[PATH_MAX];
	char tmp_path[PATH_MAX];
	struct stat st;
	long long size;

	if (persist_mkdir_p(g_cache_dir) != 0) {
		logstore_write("cixd", "error", "pkg cache: cannot create %s: %s -- %s@%s not cached",
		                g_cache_dir, strerror(errno), name, version);
		return;
	}
	cache_tarball_path(name, version, final_path, sizeof(final_path));
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-%d", final_path, (int)getpid());
	unlink(tmp_path);

	{
		/*
		 * Issue #129: the normalizing flags make this archive
		 * REPRODUCIBLE -- two independent builds of the same content
		 * produce byte-identical output. Confirmed by direct
		 * experiment, not assumed: without --sort/--mtime/--owner the
		 * per-file mtimes ride in the tar headers, so two builds of
		 * an identical tree differ; with them they hash the same.
		 * (The gzip timestamp is already zero here for an unrelated
		 * reason -- --use-compress-program pipes through gzip's
		 * stdin, which has no filename or mtime to record. That came
		 * from issue #125's PATH fix and is worth knowing before
		 * anyone "simplifies" it back to -z.)
		 *
		 * This matters because these bytes get published to a shared
		 * artifact server whose contract is that one name means one
		 * byte sequence forever. Without reproducibility every host
		 * would produce a different tarball for the same recipe
		 * version, so the first push would win and every later one
		 * would be refused as a conflict -- turning a real integrity
		 * rule into permanent noise. With it, a rejected push means
		 * what it should: two builds genuinely diverged.
		 */
		/*
		 * Issue #164: built through targz_create(), which pipes tar
		 * into gzip itself instead of asking tar to spawn the
		 * compressor. tar's own --use-compress-program goes through
		 * /bin/sh, which this platform's control-plane root does not
		 * have -- so on every real installed host this call failed
		 * and NOTHING was ever cached, silently, because a cache save
		 * is best-effort. The normalizing flags moved into targz.c
		 * with the tar invocation itself; the bytes are unchanged.
		 */
		if (targz_create(dest_dir, tmp_path, NULL) != 0) {
			/* Best-effort stays best-effort, but never silent again:
			 * a host whose cache save fails on EVERY build looked
			 * exactly like a host with nothing to cache, for months
			 * (issue #125) -- run_subprocess's own log line names
			 * tar, not the caller, so this one names the caller. */
			logstore_write("cixd", "error",
			                "pkg cache: tar create failed for %s@%s (dest %s) -- not cached",
			                name, version, dest_dir);
			unlink(tmp_path);
			return;
		}
	}
	if (stat(tmp_path, &st) != 0) {
		logstore_write("cixd", "error", "pkg cache: %s@%s tarball missing after create: %s",
		                name, version, strerror(errno));
		unlink(tmp_path);
		return;
	}
	size = (long long)st.st_size;
	if (size > g_cache_max_bytes) {
		logstore_write("cixd", "info",
		                "pkg cache: %s@%s is %lld bytes, over the whole %lld cache cap -- not cached",
		                name, version, size, (long long)g_cache_max_bytes);
		unlink(tmp_path); /* bigger than the whole cache cap -- not cacheable */
		return;
	}
	cache_evict_lru_until_fits(size);
	unlink(final_path);
	if (rename(tmp_path, final_path) != 0)
		unlink(tmp_path);
}

/*
 * Same cap/eviction discipline as pkg_cache_save(), but for an
 * artifact tarball that already exists on disk as a whole file
 * (pkg_sync_completed()'s own network-fetched pkg/artifacts/ entries)
 * rather than a directory to tar up. Takes ownership of src_path: it
 * either becomes the cache entry (rename) or is removed (copy+unlink
 * fallback across a filesystem boundary, or on any failure) -- never
 * left behind either way.
 */
static void pkg_cache_save_from_file(const char *name, const char *version, const char *src_path)
{
	char final_path[PATH_MAX];
	struct stat st;
	long long size;

	if (stat(src_path, &st) != 0)
		return;
	size = (long long)st.st_size;
	if (persist_mkdir_p(g_cache_dir) != 0 || size > g_cache_max_bytes) {
		unlink(src_path);
		return;
	}
	cache_tarball_path(name, version, final_path, sizeof(final_path));
	cache_evict_lru_until_fits(size);
	unlink(final_path);
	if (rename(src_path, final_path) != 0) {
		if (copy_file_simple(src_path, final_path) == 0)
			unlink(src_path);
		else
			unlink(src_path);
	}
}

/* Extracts a cache hit's own content into out_dir (a fresh, empty
 * directory the caller already created) -- a plain tar extraction, no
 * common-top-dir stripping needed since pkg_cache_save() always tars
 * dest_dir's own contents directly (tar -C dest_dir -czf ... .), never
 * a single wrapping directory. */
/*
 * Issue #139: does this tree contain a binary that cannot be run?
 *
 * A package artifact is verified for INTEGRITY (its sha256 matches the
 * recipe) but never for USABILITY, and those are not the same thing. An
 * artifact built by copying files through an API that reports content
 * but not mode contains perfectly intact, perfectly checksummed,
 * perfectly non-executable binaries -- which install cleanly, start a
 * container successfully, and fail at execve with "Permission denied",
 * an error naming permissions rather than the artifact behind it. That
 * happened, and it cost a full debugging cycle to trace back.
 *
 * The heuristic is deliberately narrow and non-fatal: a regular file
 * directly inside a bin/ or sbin/ directory with no execute bit for
 * anyone. That is what a broken artifact looks like, and a legitimate
 * counter-example is rare enough that saying so out loud is the right
 * response -- refusing an install on a heuristic is not.
 */
static void warn_unexecutable_binaries(const char *root, const char *pkg_name, int depth,
                                        int *reported)
{
	DIR *d;
	struct dirent *de;

	if (depth > 8 || *reported >= 5)
		return;
	d = opendir(root);
	if (d == NULL)
		return;
	while ((de = readdir(d)) != NULL && *reported < 5) {
		char path[PATH_MAX];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
		if (lstat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			warn_unexecutable_binaries(path, pkg_name, depth + 1, reported);
			continue;
		}
		if (!S_ISREG(st.st_mode) || (st.st_mode & 0111) != 0)
			continue;
		{
			const char *slash = strrchr(root, '/');
			const char *dir = (slash != NULL) ? slash + 1 : root;

			if (strcmp(dir, "bin") != 0 && strcmp(dir, "sbin") != 0)
				continue;
		}
		logstore_write("cixd", "error",
		                "pkg %s: \"%s\" sits in a bin/sbin directory but is NOT executable "
		                "(mode 0%o) -- this artifact will install successfully and then fail "
		                "at execve with \"Permission denied\". Its checksum is valid; the "
		                "artifact itself is wrong.",
		                pkg_name, path, (unsigned)(st.st_mode & 07777));
		(*reported)++;
	}
	closedir(d);
}

static int pkg_cache_extract(const char *name, const char *version, const char *out_dir)
{
	char path[PATH_MAX];

	cache_tarball_path(name, version, path, sizeof(path));
	return extract_archive_to(path, out_dir, 0);
}

void pkg_cache_write_json_status(struct json_writer *w)
{
	long long total = cache_scan();

	jw_obj_open(w);
	jw_key(w, "max_bytes");
	jw_int(w, g_cache_max_bytes);
	jw_key(w, "current_bytes");
	jw_int(w, total);
	jw_key(w, "entry_count");
	jw_int(w, g_cache_scan_count);
	jw_obj_close(w);
}

void pkg_cache_clear(void)
{
	DIR *d = opendir(g_cache_dir);
	struct dirent *de;

	if (d == NULL)
		return;
	while ((de = readdir(d)) != NULL) {
		char path[PATH_MAX];

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", g_cache_dir, de->d_name);
		unlink(path);
	}
	closedir(d);
}

/* ---- ADR-0157 Phase 3: operator-configured concurrent-build limit ----
 *
 * Mirrors pkg_cache_get_max_bytes()/pkg_cache_set_max_bytes()'s own
 * shape exactly (one persisted field, load-with-default, save-on-set) --
 * the same precedent ADR-0122/ADR-0124 already established for every
 * other small daemon-wide *-config resource in this codebase.
 */

static char g_build_config_path[PATH_MAX];
static int g_build_max_jobs = PKG_BUILD_MAX_JOBS_DEFAULT;
/*
 * Real resource ceilings for the pkgbuild sandbox (found missing the
 * hard way: a burst of concurrent installs hung cixd entirely on
 * 192.168.15.95, TCP still accepting but no HTTP request ever
 * completing again -- see ADR-0165). 0/NULL means "no limit," matching
 * cgroup_create()'s own existing gate (struct cgroup_limits, already
 * used by every regular container) -- this reuses that exact
 * mechanism rather than inventing a second one, since __pkgbuild-N
 * containers already go through container_create() like any other.
 * Defaults are deliberately NOT 0/NULL (unlimited) -- 2GiB memory, one
 * full CPU's worth of quota (cgroup v2 "quota period" syntax, the same
 * raw pass-through format run --cpu-max= already uses) -- a real
 * ceiling from the very first boot, not an opt-in an operator has to
 * remember to configure after getting burned once.
 */
#define PKG_BUILD_MEMORY_MAX_DEFAULT (2LL * 1024 * 1024 * 1024)
#define PKG_BUILD_CPU_MAX_DEFAULT "100000 100000"
static long long g_build_memory_max = PKG_BUILD_MEMORY_MAX_DEFAULT;
static char g_build_cpu_max[64] = PKG_BUILD_CPU_MAX_DEFAULT;

static int save_build_config(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "max_concurrent_jobs");
	jw_int(&w, g_build_max_jobs);
	jw_key(&w, "memory_max");
	jw_int(&w, g_build_memory_max);
	jw_key(&w, "cpu_max");
	if (g_build_cpu_max[0] != '\0')
		jw_str(&w, g_build_cpu_max);
	else
		jw_null(&w);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = persist_atomic_write(g_build_config_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

/* ADR-0141 Phase 4: path-only repoint -- see pkg_repoint()'s own doc comment. */
void pkg_build_config_repoint(const char *new_config_path)
{
	snprintf(g_build_config_path, sizeof(g_build_config_path), "%s", new_config_path);
}

int pkg_build_config_init(const char *config_path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *mj, *jmem, *jcpu;

	if (snprintf(g_build_config_path, sizeof(g_build_config_path), "%s", config_path) >=
	    (int)sizeof(g_build_config_path))
		return -1;
	g_build_max_jobs = PKG_BUILD_MAX_JOBS_DEFAULT;
	g_build_memory_max = PKG_BUILD_MEMORY_MAX_DEFAULT;
	snprintf(g_build_cpu_max, sizeof(g_build_cpu_max), "%s", PKG_BUILD_CPU_MAX_DEFAULT);

	if (persist_read_file(config_path, &buf, &len) != 0 || buf == NULL)
		return 0; /* no persisted config yet -- the defaults stand */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return 0;

	mj = json_object_get(root, "max_concurrent_jobs");
	if (mj != NULL) {
		double v = json_as_number(mj);

		if (v >= 1 && v <= PKG_MAX_CONCURRENT_JOBS)
			g_build_max_jobs = (int)v;
	}
	jmem = json_object_get(root, "memory_max");
	if (jmem != NULL && jmem->type != JSON_NULL) {
		double v = json_as_number(jmem);

		if (v >= 0)
			g_build_memory_max = (long long)v;
	}
	/*
	 * Unlike memory_max above (save_build_config() always writes it as
	 * a real integer, 0 meaning unlimited, never null), cpu_max is
	 * written as a genuine JSON null when explicitly cleared -- so
	 * "key present but null" and "key absent entirely" are two
	 * different, real states here, not one. Confirmed the hard way
	 * live on 192.168.15.95 (ADR-0166's own investigation): a real
	 * PUT clearing cpu_max to unlimited silently reverted to
	 * PKG_BUILD_CPU_MAX_DEFAULT on the very next daemon restart,
	 * because the old `jcpu->type != JSON_NULL` check treated both
	 * states identically -- neither branch ever cleared
	 * g_build_cpu_max back to "" for the genuinely-null case, so the
	 * line 4758 default (set moments before this whole persisted-
	 * config block even runs) silently won.
	 */
	jcpu = json_object_get(root, "cpu_max");
	if (jcpu != NULL) {
		if (jcpu->type == JSON_NULL) {
			g_build_cpu_max[0] = '\0';
		} else {
			const char *s = json_as_string(jcpu);

			if (s != NULL)
				snprintf(g_build_cpu_max, sizeof(g_build_cpu_max), "%s", s);
		}
	}
	json_free(root);
	return 0;
}

int pkg_build_get_max_jobs(void)
{
	return g_build_max_jobs;
}

enum pkg_error pkg_build_set_max_jobs(int max_jobs)
{
	if (max_jobs < 1 || max_jobs > PKG_MAX_CONCURRENT_JOBS)
		return PKG_ERR_INVALID_NAME;
	g_build_max_jobs = max_jobs;
	if (save_build_config() != 0)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

long long pkg_build_get_memory_max(void)
{
	return g_build_memory_max;
}

/* 0 means unlimited (cgroup_create()'s own existing "memory_max > 0"
 * gate) -- a deliberate, explicit opt-out, not a validation failure,
 * matching a real operator choice ("I have plenty of RAM, don't
 * bother"). Negative is rejected -- there's no such thing as negative
 * memory. */
enum pkg_error pkg_build_set_memory_max(long long memory_max)
{
	if (memory_max < 0)
		return PKG_ERR_INVALID_NAME;
	g_build_memory_max = memory_max;
	if (save_build_config() != 0)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

const char *pkg_build_get_cpu_max(void)
{
	return g_build_cpu_max[0] != '\0' ? g_build_cpu_max : NULL;
}

/* NULL/empty means unlimited (cgroup_create()'s own "cpu_max != NULL"
 * gate) -- same deliberate-opt-out reasoning as memory_max above. No
 * syntax validation beyond length here -- cgroup v2's own cpu.max file
 * is the real authority on whether "QUOTA PERIOD" parses, the same
 * "pass it straight through, let the kernel be the judge" posture
 * struct cgroup_limits.cpu_max already has for every regular
 * container's own run --cpu-max=. */
enum pkg_error pkg_build_set_cpu_max(const char *cpu_max)
{
	if (cpu_max == NULL || cpu_max[0] == '\0') {
		g_build_cpu_max[0] = '\0';
	} else {
		if (strlen(cpu_max) >= sizeof(g_build_cpu_max))
			return PKG_ERR_INVALID_NAME;
		snprintf(g_build_cpu_max, sizeof(g_build_cpu_max), "%s", cpu_max);
	}
	if (save_build_config() != 0)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

/* ---- pkg/ redesign Part 3b (ADR-0122): plain-HTTP precompiled-artifact server config ----
 *
 * Deliberately NOT the same forge-aware repo config Part 2 built for
 * recipes (ADR-0121). Confirmed with the user directly: recipes are
 * small, versioned text that belongs in git; a compiled package
 * artifact does not, and stuffing binaries into a git-forge's own
 * archive/raw-content endpoints (the mechanism Part 2 already has)
 * would mean an operator has to check binaries into that same git
 * tree to make them fetchable -- exactly the "source contaminated
 * with compiled output" problem this design explicitly avoids. A
 * precompiled artifact is instead served from any plain HTTP
 * location (a generic file server, a forge's own release-assets/
 * package-registry feature reachable over plain HTTP, a peer
 * cixd) at a fixed, predictable path this daemon computes itself:
 * <base_url>/<name>-<version>.tar.gz -- the exact same naming
 * convention the local cache already uses. Trust never comes from
 * the server (see struct pkg_recipe's own artifact_sha256 comment):
 * the artifact is only ever accepted after verifying it against the
 * recipe's own git-tracked checksum.
 */

static char g_artifact_base_url[PKGARTIFACT_URL_MAX];
static char g_artifact_token[PKGARTIFACT_TOKEN_MAX];
static char g_artifact_config_path[PATH_MAX];
/* Issue #129: whether a fresh local build publishes its own result to
 * the configured artifact server. Off by default -- publishing is an
 * outward-facing action, so it is opted into deliberately, never
 * inherited from merely having a base_url set for pulling. */
static int g_artifact_push_enabled;

static int save_artifact_config(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "base_url");
	jw_str(&w, g_artifact_base_url);
	jw_key(&w, "auth_token");
	jw_str(&w, g_artifact_token);
	jw_key(&w, "push_enabled");
	jw_bool(&w, g_artifact_push_enabled);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = persist_atomic_write(g_artifact_config_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

/* ADR-0141 Phase 4: path-only repoint -- see pkg_repoint()'s own doc
 * comment for the shared reasoning. */
void pkg_artifact_repoint(const char *new_config_path)
{
	snprintf(g_artifact_config_path, sizeof(g_artifact_config_path), "%s", new_config_path);
}

int pkg_artifact_init(const char *config_path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const char *s;

	if (snprintf(g_artifact_config_path, sizeof(g_artifact_config_path), "%s", config_path) >=
	    (int)sizeof(g_artifact_config_path))
		return -1;
	g_artifact_base_url[0] = '\0';
	g_artifact_token[0] = '\0';
	g_artifact_push_enabled = 0;

	if (persist_read_file(config_path, &buf, &len) != 0 || buf == NULL)
		return 0; /* no persisted config yet -- defaults stand */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return 0;

	s = json_as_string(json_object_get(root, "base_url"));
	if (s != NULL)
		snprintf(g_artifact_base_url, sizeof(g_artifact_base_url), "%s", s);
	s = json_as_string(json_object_get(root, "auth_token"));
	if (s != NULL)
		snprintf(g_artifact_token, sizeof(g_artifact_token), "%s", s);
	{
		const struct json_value *pe = json_object_get(root, "push_enabled");

		if (pe != NULL && pe->type == JSON_BOOL)
			g_artifact_push_enabled = pe->u.boolean ? 1 : 0;
	}

	json_free(root);
	return 0;
}

/* {"base_url","auth_token_set"} -- the token itself is never echoed
 * back, same posture pkg_repo_write_json_config() already has. */
void pkg_artifact_write_json_config(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "base_url");
	jw_str(w, g_artifact_base_url);
	jw_key(w, "auth_token_set");
	jw_bool(w, g_artifact_token[0] != '\0');
	jw_key(w, "push_enabled");
	jw_bool(w, g_artifact_push_enabled);
	jw_obj_close(w);
}

/* NULL leaves that field unchanged (partial PUT, same contract
 * pkg_repo_set_config() already has); "" for auth_token explicitly
 * clears it. */
enum pkg_error pkg_artifact_set_config(const char *base_url, const char *auth_token,
                                       const int *push_enabled)
{
	if (base_url != NULL)
		snprintf(g_artifact_base_url, sizeof(g_artifact_base_url), "%s", base_url);
	if (auth_token != NULL)
		snprintf(g_artifact_token, sizeof(g_artifact_token), "%s", auth_token);
	if (push_enabled != NULL)
		g_artifact_push_enabled = *push_enabled ? 1 : 0;

	if (save_artifact_config() != 0)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

static int pkg_artifact_is_configured(void)
{
	return g_artifact_base_url[0] != '\0';
}

/*
 * ============================ artifact push (issue #129) ============
 *
 * A fresh local build's own result, published to the configured
 * artifact server so the next host pulls it instead of rebuilding it.
 * Before this, an artifact's default fate was to be forgotten: it
 * lived in one box's local cache and nothing could ever retrieve it,
 * which for a multi-hour toolchain build is a real loss, not an
 * inconvenience.
 *
 * Three properties this deliberately keeps:
 *
 * - It never publishes what it did not build. A cache or artifact HIT
 *   already came from somewhere; re-uploading it would be pure noise,
 *   so only the fresh-build branch enqueues.
 * - It is not a trust boundary, and does not become one. The bytes are
 *   still verified by the consumer against the recipe's own git-tracked
 *   pkg_artifact_sha256 (ADR-0122); the X-Cix-Sha256 header this sends
 *   is a corruption check at the door, not a claim the receiver takes
 *   on faith.
 * - It never blocks the event loop. The upload runs in a forked child
 *   that also computes the digest, so a multi-hundred-megabyte push
 *   costs the daemon one fork, not a stalled control plane (ADR-0180).
 *
 * Pushes are serialized through a small queue rather than run
 *  concurrently: parallel builds (ADR-0157) can finish together, and
 * dropping the overflow would recreate exactly the forgotten-artifact
 * problem this exists to fix.
 */
#define PKG_PUSH_QUEUE_MAX 32
/* Distinct from any curl exit status, so the reaper can tell a failed
 * digest apart from a failed upload. */
#define PKG_PUSH_EXIT_SHA_FAILED 90
#define PKG_PUSH_EXIT_SIGN_FAILED 91

/*
 * A signature is pushed as an ordinary artifact push (ADR-0279).
 *
 * The alternative was a second upload path beside this one, which would
 * have been a parallel implementation of fork-hash-PUT-reap for an
 * object that is a file in the same cache directory going to the same
 * server. One kind field reuses the queue, the approval gate, the
 * stall guard, the status file and the reaper exactly as they are.
 */
enum pkg_push_kind {
	PKG_PUSH_ARTIFACT = 0,
	PKG_PUSH_SIGNATURE
};

struct pkg_push_job {
	char name[PKG_NAME_MAX];
	char version[PKG_VERSION_MAX];
	enum pkg_push_kind kind;
};

static struct pkg_push_job g_push_queue[PKG_PUSH_QUEUE_MAX];
static int g_push_queue_count;

/*
 * Is this target actually held right now? Answered from the live queues
 * rather than from anything stored, which is what lets "what is
 * pending" have no persistent state at all.
 *
 * `deploy` is the exception and the asymmetry is deliberate (ADR-0273):
 * publish and roll hold items that sit in a queue, so being held is a
 * readable fact; an update is one synchronous request with no queue
 * behind it, so any target is accepted while the gate is on.
 */
int pkg_target_is_queued(const char *gate, const char *target)
{
	int i;

	if (gate == NULL || target == NULL)
		return 0;
	if (strcmp(gate, PKG_GATE_DEPLOY) == 0)
		return 1;
	if (strcmp(gate, PKG_GATE_ROLL) == 0) {
		for (i = 0; i < g_rebuild_queue_count; i++) {
			if (strcmp(g_rebuild_queue[i], target) == 0)
				return 1;
		}
		return 0;
	}
	if (strcmp(gate, PKG_GATE_PUBLISH) == 0) {
		for (i = 0; i < g_push_queue_count; i++) {
			char t[PKG_APPROVAL_TARGET_MAX];

			snprintf(t, sizeof(t), "%s@%s", g_push_queue[i].name, g_push_queue[i].version);
			if (strcmp(t, target) == 0)
				return 1;
		}
		return 0;
	}
	return 0;
}

/*
 * ADR-0273: what is waiting for a person, and what a person has already
 * allowed. The first list is computed here and stored nowhere; the
 * second is the stored one.
 */
void pkg_approvals_write_json(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "pending");
	jw_arr_open(w);
	if (g_gate_roll) {
		for (i = 0; i < g_rebuild_queue_count; i++) {
			if (approval_index(PKG_GATE_ROLL, g_rebuild_queue[i]) >= 0)
				continue;
			jw_obj_open(w);
			jw_key(w, "gate");
			jw_str(w, PKG_GATE_ROLL);
			jw_key(w, "target");
			jw_str(w, g_rebuild_queue[i]);
			jw_obj_close(w);
		}
	}
	if (g_gate_publish) {
		for (i = 0; i < g_push_queue_count; i++) {
			char t[PKG_APPROVAL_TARGET_MAX];

			snprintf(t, sizeof(t), "%s@%s", g_push_queue[i].name, g_push_queue[i].version);
			if (approval_index(PKG_GATE_PUBLISH, t) >= 0)
				continue;
			jw_obj_open(w);
			jw_key(w, "gate");
			jw_str(w, PKG_GATE_PUBLISH);
			jw_key(w, "target");
			jw_str(w, t);
			jw_obj_close(w);
		}
	}
	jw_arr_close(w);
	jw_key(w, "granted");
	jw_arr_open(w);
	for (i = 0; i < g_approval_count; i++) {
		jw_obj_open(w);
		jw_key(w, "gate");
		jw_str(w, g_approvals[i].gate);
		jw_key(w, "target");
		jw_str(w, g_approvals[i].target);
		jw_key(w, "who");
		jw_str(w, g_approvals[i].who);
		jw_key(w, "at");
		jw_int(w, (long long)g_approvals[i].at);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}

/*
 * ADR-0273: consumes the deploy grant for this exact target, if there
 * is one. Deploy is the gate with no queue behind it, so unlike roll
 * and publish -- whose grants are spent by their own drains -- this one
 * is spent by the request it lets through.
 */
int pkg_approval_take_deploy(const char *target)
{
	int i;

	if (target == NULL || target[0] == '\0')
		return 0;
	i = approval_index(PKG_GATE_DEPLOY, target);
	if (i < 0)
		return 0;
	logstore_write("audit", "info", "gate deploy: %s went through, approved by %s", target,
	                g_approvals[i].who);
	approval_drop_at(i);
	pkg_runs_save();
	return 1;
}

int pkg_approval_grant(const char *gate, const char *target, const char *who)
{
	if (gate_flag(gate) == NULL)
		return PKG_APPROVE_UNKNOWN_GATE;
	if (target == NULL || target[0] == '\0')
		return PKG_APPROVE_UNKNOWN_GATE;
	if (!pkg_gate_enabled(gate))
		return PKG_APPROVE_NOT_HELD; /* nothing is being held by an off gate */
	if (approval_index(gate, target) >= 0)
		return PKG_APPROVE_ALREADY;
	if (!pkg_target_is_queued(gate, target))
		return PKG_APPROVE_NOT_HELD;
	if (g_approval_count >= PKG_APPROVAL_MAX)
		return PKG_APPROVE_FULL;
	memset(&g_approvals[g_approval_count], 0, sizeof(g_approvals[0]));
	snprintf(g_approvals[g_approval_count].gate, sizeof(g_approvals[0].gate), "%s", gate);
	snprintf(g_approvals[g_approval_count].target, sizeof(g_approvals[0].target), "%s", target);
	snprintf(g_approvals[g_approval_count].who, sizeof(g_approvals[0].who), "%s",
	         who != NULL && who[0] != '\0' ? who : "-");
	g_approvals[g_approval_count].at = time(NULL);
	g_approval_count++;
	pkg_runs_save();
	return PKG_APPROVE_OK;
}

static int g_push_in_flight;
static struct pkg_push_job g_push_current;

/* Where the push child records curl's own HTTP status, so the reaper
 * can say "published" / "already present with different bytes" /
 * "rejected" instead of a bare exit code. One push runs at a time, so
 * one fixed path is enough. */
static void push_status_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/.artifact-push.status", g_sources_dir);
}

/*
 * Queues a freshly built package for publication. Best-effort by
 * design -- a push that never happens must never fail the build that
 * produced it -- but never silent: every reason to skip is logged,
 * because a host that silently publishes nothing looks exactly like a
 * host with nothing to publish (the same trap issue #125 set).
 */
static void pkg_artifact_push_enqueue_kind(const char *name, const char *version,
                                            enum pkg_push_kind kind)
{
	if (!g_artifact_push_enabled) {
		/*
		 * Issue #171: the one refusal path here that used to say
		 * nothing at all, while every other one below logs.
		 *
		 * A host may legitimately not publish, so this is not an
		 * error. But a build that produces a distributable artifact
		 * and silently discards its only copy is the same class of
		 * quiet loss as diagnostics that only ever reached stderr
		 * (#132). It really cost: nine packages were built on
		 * 192.168.15.95 with push_enabled false -- gawk, mtools,
		 * xorriso, diffutils, bison, python, grub and others, hours of
		 * real compute -- and none reached the cache. Nothing said so,
		 * and a host's local cache does not survive a reinstall.
		 *
		 * Says what to do about it, because the answer is not obvious
		 * from the fact alone: the artifact is still here and can be
		 * published without rebuilding.
		 */
		logstore_write("cixd", "info",
		                "artifact push: %s@%s was built here but not published -- push is "
		                "disabled. The artifact is in this host's cache and can still be "
		                "published with POST /v1/pkg/%s/artifact/publish; it will be lost "
		                "if this host is reinstalled first.",
		                name, version, name);
		return;
	}
	if (!pkg_artifact_is_configured()) {
		logstore_write("cixd", "info",
		                "artifact push: %s@%s not published -- push is enabled but no "
		                "base_url is configured",
		                name, version);
		return;
	}
	if (g_artifact_token[0] == '\0') {
		logstore_write("cixd", "info",
		                "artifact push: %s@%s not published -- push is enabled but no "
		                "auth_token is configured",
		                name, version);
		return;
	}
	if (g_push_queue_count >= PKG_PUSH_QUEUE_MAX) {
		logstore_write("cixd", "error",
		                "artifact push: queue full (%d) -- %s@%s will NOT be published",
		                PKG_PUSH_QUEUE_MAX, name, version);
		return;
	}
	snprintf(g_push_queue[g_push_queue_count].name, PKG_NAME_MAX, "%s", name);
	snprintf(g_push_queue[g_push_queue_count].version, PKG_VERSION_MAX, "%s", version);
	g_push_queue[g_push_queue_count].kind = kind;
	g_push_queue_count++;
}

static void pkg_artifact_push_enqueue(const char *name, const char *version)
{
	pkg_artifact_push_enqueue_kind(name, version, PKG_PUSH_ARTIFACT);
}

/*
 * Where a push will look for this package's tarball, and whether it is
 * there yet. A hostbuild's artifact is a DIRECTORY -- it is assembled on
 * this host rather than fetched, so nothing ever tarred it into the
 * cache, and pkg_artifact_push_try_start() found nothing to send. That
 * is why "cix", "kernel" and "isotools" could never be published while
 * ordinary source-built packages published themselves automatically:
 * the difference was never the request, it was that half the packages
 * on a box had no tarball for the pusher to find. The caller builds one
 * from the installed tree before enqueuing.
 */
void pkg_artifact_cache_path(const char *name, const char *version, char *out, size_t out_size)
{
	cache_tarball_path(name, version, out, out_size);
}

/*
 * Stages the package seed an installer ISO carries (#135).
 *
 * The bootstrap cycle this breaks: a fresh install has no recipes;
 * recipes come from a git forge addressed by hostname; a hostname needs
 * DNS; this platform's DNS is a container built from a recipe. Bringing
 * a box up therefore meant hand-feeding it 300-odd recipes from a
 * developer machine over literal IPs, which is not something an
 * operator can be asked to do.
 *
 * Both halves of the delivery already existed and were never used:
 * mkinstalleriso stages <seed>/recipes and <seed>/artifacts onto the
 * media, and cix-install copies them into the installed box. What was
 * missing was anything producing a seed -- the daemon passed "" with a
 * comment saying that staging one changes what the media DOES and is a
 * product decision. This is that decision.
 *
 * WHAT GOES IN, and why it is a short explicit list rather than a rule:
 *
 * Recipes are all of them; they are small text and having the full set
 * is what makes the box able to build anything at all afterwards.
 *
 * Artifacts are only what a fresh box needs to reach NAME RESOLUTION,
 * because everything else follows from that: once dns-1/dns-2 run, the
 * forge resolves, `pkg sync` works, and the artifact cache is reachable.
 * That is the acceptance test #135 states, and it costs ~57 MB (glibc
 * is nearly all of it). Staging everything the local cache holds would
 * instead put gcc, the kernel and every toolchain package on the media
 * for no bootstrap benefit.
 *
 * A list rather than a derivation on purpose: deriving it (from the dns
 * image's manifest, or from registered DNS servers) would silently
 * change what the media carries whenever an unrelated image changed,
 * and would produce an empty artifact set on a host that happens not to
 * run DNS itself -- shipping media that claims a seed and cannot
 * bootstrap. A list is visible in a diff, so it cannot change by
 * accident.
 */
static const char *const g_seed_packages[] = { PKG_BASE_LIBC, "zlib", "dnsmasq" };

/*
 * One recipe version per package onto the media, not the whole store.
 *
 * The seed exists so a fresh box can reach name resolution before it
 * has a forge (ADR-0229). A superseded revision cannot serve that: the
 * only artifacts on the media are the three staged below, so an older
 * recipe has nothing to install from, and the moment the box does have
 * a forge it syncs the real history anyway. It was 11.9 MiB of 12.8 --
 * 94% of the recipe tree, 302 revisions of `cix` alone -- on media
 * that a fresh box boots once.
 *
 * The exception is load-bearing: a seeded ARTIFACT's own version is
 * copied even when it is not the latest. Ship only the newest recipe
 * beside an older artifact and a fresh box resolves glibc to a version
 * the media does not carry, then tries to build a C library on a
 * machine that has no compiler yet.
 */
static int seed_copy_one_recipe(const char *recipes_dst, const char *name, const char *version)
{
	char src[PATH_MAX];
	char dst_dir[PATH_MAX];
	char dst[PATH_MAX];
	struct stat st;

	if (snprintf(src, sizeof(src), "%s/%s/%s", g_recipes_dir, name, version) >=
	        (int)sizeof(src) ||
	    snprintf(dst_dir, sizeof(dst_dir), "%s/%s", recipes_dst, name) >=
	        (int)sizeof(dst_dir) ||
	    snprintf(dst, sizeof(dst), "%s/%s", dst_dir, version) >= (int)sizeof(dst))
		return -1;
	if (stat(src, &st) != 0 || !S_ISDIR(st.st_mode))
		return 0; /* nothing there to copy is not a failure */
	if (stat(dst, &st) == 0)
		return 0; /* already staged -- latest and artifact agree */
	if (persist_mkdir_p(dst_dir) != 0)
		return -1;
	return treecopy_recursive(src, dst);
}

static int seed_stage_recipes(const char *recipes_dst)
{
	DIR *d;
	struct dirent *de;
	int rc = 0;

	if (persist_mkdir_p(recipes_dst) != 0)
		return -1;
	d = opendir(g_recipes_dir);
	if (d == NULL)
		return -1;
	while ((de = readdir(d)) != NULL) {
		char latest[PKG_VERSION_MAX];

		if (de->d_name[0] == '.')
			continue;
		if (!pkg_name_is_valid(de->d_name))
			continue;
		if (recipe_latest_version(de->d_name, latest, sizeof(latest)) != 0)
			continue;
		if (seed_copy_one_recipe(recipes_dst, de->d_name, latest) != 0) {
			rc = -1;
			break;
		}
	}
	closedir(d);
	return rc;
}

enum pkg_error pkg_seed_stage(const char *dest_dir, char *err, size_t err_size)
{
	char recipes_dst[PATH_MAX];
	char artifacts_dst[PATH_MAX];
	size_t i;

	if (dest_dir == NULL || dest_dir[0] == '\0')
		return PKG_ERR_INVALID_NAME;

	if (snprintf(recipes_dst, sizeof(recipes_dst), "%s/recipes", dest_dir) >=
	        (int)sizeof(recipes_dst) ||
	    snprintf(artifacts_dst, sizeof(artifacts_dst), "%s/artifacts", dest_dir) >=
	        (int)sizeof(artifacts_dst)) {
		snprintf(err, err_size, "seed staging path too long");
		return PKG_ERR_INVALID_NAME;
	}

	if (persist_mkdir_p(artifacts_dst) != 0) {
		snprintf(err, err_size, "could not create %s: %s", artifacts_dst, strerror(errno));
		return PKG_ERR_PERSIST_FAILED;
	}

	if (seed_stage_recipes(recipes_dst) != 0) {
		snprintf(err, err_size, "could not stage recipes from %s: %s", g_recipes_dir,
		         treecopy_last_error());
		return PKG_ERR_PERSIST_FAILED;
	}

	/*
	 * Every named artifact must be present, and a missing one fails the
	 * whole ISO rather than quietly shipping media that cannot do what
	 * carrying a seed implies. cix-install already refuses a seed whose
	 * directories are missing entirely; this is the same refusal one
	 * level up, where the reason is still known.
	 */
	for (i = 0; i < sizeof(g_seed_packages) / sizeof(g_seed_packages[0]); i++) {
		const struct pkg_entry *e = NULL;
		char src[PATH_MAX];
		char dst[PATH_MAX];
		const char *base;
		int j;

		for (j = 0; j < PKG_MAX_PACKAGES; j++) {
			if (g_packages[j].in_use && g_packages[j].state == PKG_STATE_INSTALLED &&
			    strcmp(g_packages[j].name, g_seed_packages[i]) == 0) {
				e = &g_packages[j];
				break;
			}
		}
		if (e == NULL) {
			snprintf(err, err_size,
			         "the installer seed needs %s and this host has none installed -- an ISO "
			         "built now could not bring up DNS on a fresh box",
			         g_seed_packages[i]);
			return PKG_ERR_NOT_FOUND;
		}
		/*
		 * This artifact's OWN recipe version, even if a newer one is
		 * what seed_stage_recipes() already staged. Otherwise a fresh
		 * box resolves this package to a recipe the media carries no
		 * artifact for, and tries to build a C library on a machine
		 * with no compiler.
		 */
		if (seed_copy_one_recipe(recipes_dst, e->name, e->version) != 0) {
			snprintf(err, err_size, "could not stage the %s@%s recipe the seeded artifact "
			                        "needs: %s",
			         e->name, e->version, treecopy_last_error());
			return PKG_ERR_PERSIST_FAILED;
		}

		cache_tarball_path(e->name, e->version, src, sizeof(src));
		if (!pkg_artifact_cache_has(e->name, e->version)) {
			snprintf(err, err_size,
			         "the installer seed needs %s@%s and its artifact is not in this host's "
			         "cache -- publish or rebuild it first",
			         e->name, e->version);
			return PKG_ERR_NOT_FOUND;
		}
		base = strrchr(src, '/');
		base = (base != NULL) ? base + 1 : src;
		if (snprintf(dst, sizeof(dst), "%s/%s", artifacts_dst, base) >= (int)sizeof(dst)) {
			snprintf(err, err_size, "seed artifact path too long for %s", e->name);
			return PKG_ERR_INVALID_NAME;
		}
		if (copy_file_simple(src, dst) != 0) {
			snprintf(err, err_size, "could not stage the %s artifact: %s", e->name,
			         strerror(errno));
			return PKG_ERR_PERSIST_FAILED;
		}
		logstore_write("cixd", "info", "iso seed: staged %s@%s", e->name, e->version);
	}

	return PKG_OK;
}

int pkg_artifact_cache_has(const char *name, const char *version)
{
	char path[PATH_MAX];
	struct stat st;

	cache_tarball_path(name, version, path, sizeof(path));
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/*
 * Resolves what pkg_artifact_publish() would publish, without enqueuing
 * anything -- same validation, same version selection, so the two can
 * never disagree about which version a publish means.
 */
enum pkg_error pkg_artifact_publish_resolve(const char *name, char *out_version,
                                             size_t out_version_size, int *out_is_hostbuild)
{
	int i;

	if (!g_artifact_push_enabled || g_artifact_base_url[0] == '\0')
		return PKG_ERR_INVALID_RECIPE;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (!g_packages[i].in_use || g_packages[i].state != PKG_STATE_INSTALLED)
			continue;
		if (strcmp(g_packages[i].name, name) != 0)
			continue;
		snprintf(out_version, out_version_size, "%s", g_packages[i].version);
		/* Derived from the image, never a stored flag -- the same
		 * One Source of Truth rule the listing follows (ADR-0056). */
		if (out_is_hostbuild != NULL)
			*out_is_hostbuild = strcmp(g_packages[i].image, PKG_HOSTBUILD_IMAGE) == 0;
		return PKG_OK;
	}
	return PKG_ERR_NOT_FOUND;
}

/*
 * See pkg.h for why this exists. Everything here is a refusal except
 * the last two lines -- the point is to start an ordinary install, and
 * to do it only when that install cannot turn into a build.
 */
int pkg_default_image_libc_failed(char *out_error, size_t out_error_size)
{
	int i;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (!g_packages[i].in_use)
			continue;
		if (strcmp(g_packages[i].name, PKG_BASE_LIBC) != 0)
			continue;
		if (strcmp(normalize_image(g_packages[i].image), PKG_DEFAULT_IMAGE) != 0)
			continue;
		if (g_packages[i].state != PKG_STATE_FAILED)
			return 0;
		snprintf(out_error, out_error_size, "%s", g_packages[i].error);
		return 1;
	}
	return 0;
}

enum pkg_error pkg_seed_default_image_libc(pid_t *out_pid, int *out_pidfd, int *out_chain_idx)
{
	char version[IMAGE_VERSION_MAX];
	char rootfs[PATH_MAX];
	char recipe_path[PATH_MAX];
	struct pkg_recipe recipe;
	char started[PKG_NAME_MAX];

	if (image_current_version(PKG_DEFAULT_IMAGE, version, sizeof(version)) != IMAGE_OK)
		return PKG_ERR_NOT_FOUND;
	image_version_rootfs_path(PKG_DEFAULT_IMAGE, version, rootfs, sizeof(rootfs));

	/*
	 * Already has a real C library PACKAGE -- the ordinary case on every
	 * boot after the first, and the reason this is safe to call
	 * unconditionally.
	 *
	 * This used to ask whether the dynamic loader FILE existed, and that
	 * is a different question with the same shape (#241).
	 * pkg_seed_image_baseline() stages a loader and a libc into every
	 * image, so the file is always there and this always concluded
	 * "already runnable" -- meaning the default image never received the
	 * glibc package at all. It then carried a baseline libc for the rest
	 * of its life, and the first binary installed into it that needed a
	 * newer one failed at runtime:
	 *
	 *   /usr/bin/bash: /lib/x86_64-linux-gnu/libc.so.6:
	 *   version `GLIBC_2.38' not found (required by /usr/bin/bash)
	 *
	 * Confirmed on a real host: `base` had 46 packages and no glibc
	 * among them, and every container made from it exited 1 instantly
	 * while POST /v1/containers still answered 201 "running" (which only
	 * ever proved the child execve()'d).
	 *
	 * "Runnable" inferred from a file being present rather than from
	 * anything actually running is the same mistake this project has
	 * made before, and the fix is the same: ask the question you mean.
	 * Installing the package is idempotent -- once it is in the
	 * manifest this returns here, and until then the guards below
	 * refuse cleanly when there is no recipe or no cached artifact.
	 */
	if (pkg_find(PKG_BASE_LIBC, PKG_DEFAULT_IMAGE) != NULL)
		return PKG_ERR_NOT_FOUND;

	/*
	 * Past this point the default image genuinely has no runtime, so
	 * every refusal below is a state an operator may need to act on --
	 * and each says which one it is. Returning a bare "not found" for
	 * four different reasons is how a box ends up unable to run a
	 * container with nothing anywhere saying why.
	 */

	/* A recipe, so we know which version we would be installing... */
	if (find_recipe_path(PKG_BASE_LIBC, NULL, recipe_path, sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, PKG_BASE_LIBC) != 0) {
		logstore_write("cixd", "info",
		               "the default image \"%s\" has no C library and there is no %s recipe to "
		               "install one from -- containers created from it will be refused until "
		               "a package source is configured",
		               PKG_DEFAULT_IMAGE, PKG_BASE_LIBC);
		printf("default image: no C library and no %s recipe to install one from\n",
		       PKG_BASE_LIBC);
		fflush(stdout);
		return PKG_ERR_NOT_FOUND;
	}

	/* ...and its artifact already here, so the install is a cache hit
	 * needing no build environment. Without this a fresh box would try
	 * to BUILD glibc with no toolchain -- a long, loud failure in place
	 * of a box that simply says what to install. */
	if (!pkg_artifact_cache_has(PKG_BASE_LIBC, recipe.version)) {
		logstore_write("cixd", "info",
		               "the default image \"%s\" has no C library and %s@%s is not in the local "
		               "artifact cache -- not building it here, since a box with no toolchain "
		               "would fail slowly instead of saying what it needs",
		               PKG_DEFAULT_IMAGE, PKG_BASE_LIBC, recipe.version);
		printf("default image: no C library and %s@%s is not in the artifact cache\n",
		       PKG_BASE_LIBC, recipe.version);
		fflush(stdout);
		return PKG_ERR_NOT_FOUND;
	}

	/*
	 * And it must be the bytes the recipe approves.
	 *
	 * A cache hit deliberately skips fetching AND verifying -- the
	 * fetch child exits immediately and the install stages straight
	 * from the cache. That is sound only because of an invariant:
	 * everything in that cache was verified on the way IN, either a
	 * download checked against pkg_artifact_sha256 or an artifact this
	 * host built itself.
	 *
	 * This path is the first that can be handed a cached artifact from
	 * somewhere else -- an installer seed written before the daemon
	 * ever ran (#189). Verifying here keeps the invariant true rather
	 * than widening the daemon's trust to whatever wrote its cache
	 * directory: the artifact server was never a trust boundary, and
	 * neither is install media.
	 */
	if (recipe.artifact_sha256[0] != '\0') {
		char cached[PATH_MAX];
		char got[80];

		pkg_artifact_cache_path(PKG_BASE_LIBC, recipe.version, cached, sizeof(cached));
		if (pkg_run_capture_sha256(cached, got, sizeof(got)) != 0 ||
		    strcmp(got, recipe.artifact_sha256) != 0) {
			logstore_write("cixd", "error",
			               "not seeding the default image: the cached %s@%s artifact is not "
			               "the bytes its own recipe approves -- refusing to install it",
			               PKG_BASE_LIBC, recipe.version);
			return PKG_ERR_NOT_FOUND;
		}
	}

	return pkg_install_start(PKG_BASE_LIBC, PKG_DEFAULT_IMAGE, NULL, 0, 0, started,
	                          sizeof(started), out_pid, out_pidfd, out_chain_idx);
}

enum pkg_error pkg_artifact_publish(const char *name)
{
	int i;

	if (!g_artifact_push_enabled || g_artifact_base_url[0] == '\0')
		return PKG_ERR_INVALID_RECIPE;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (!g_packages[i].in_use || g_packages[i].state != PKG_STATE_INSTALLED)
			continue;
		if (strcmp(g_packages[i].name, name) != 0)
			continue;
		pkg_artifact_push_enqueue(g_packages[i].name, g_packages[i].version);
		return PKG_OK;
	}
	return PKG_ERR_NOT_FOUND;
}

/*
 * Starts the next queued push, if any and if none is already running.
 * Returns 1 with *out_pid/*out_pidfd owned by the caller (main.c
 * registers the pidfd and calls pkg_artifact_push_completed() when it
 * fires), 0 when there is nothing to do.
 */
int pkg_artifact_push_try_start(pid_t *out_pid, int *out_pidfd, char *out_desc, size_t desc_size)
{
	char tarball[PATH_MAX];
	char artifact[PATH_MAX];
	char status_path[PATH_MAX];
	char url[PKGARTIFACT_URL_MAX];
	char auth_header[PKGARTIFACT_TOKEN_MAX + 32];
	struct stat st;
	pid_t pid;
	int pidfd;
	int i;

	if (g_push_in_flight)
		return 0;

	/*
	 * Skipping an entry must not strand the rest of the queue: nothing
	 * schedules another pump until a push actually completes, so a bare
	 * "return 0" here would leave everything behind it unpublished
	 * until the next unrelated build happened to finish. Keep taking
	 * entries until one is genuinely startable.
	 */
	for (;;) {
		int pick = -1;

		if (g_push_queue_count == 0)
			return 0;

		/*
		 * ADR-0273: take the first entry NOT held for approval. This
		 * queue is destructive-take (the entry is removed as it is
		 * picked), so a held one cannot be skipped by continuing --
		 * it has to be stepped over while choosing. Everything held
		 * stays queued, in order, waiting for a person.
		 */
		for (i = 0; i < g_push_queue_count; i++) {
			char t[PKG_APPROVAL_TARGET_MAX];

			/* A signature is only ever queued after its artifact's
			 * own push succeeded, so the person who approved that
			 * publish has already approved this. Asking again would
			 * hold the signature behind a second approval for a
			 * decision nobody made twice -- and an artifact whose
			 * signature never arrives is worse than one that was
			 * never published, because it looks complete. */
			if (g_push_queue[i].kind == PKG_PUSH_SIGNATURE) {
				pick = i;
				break;
			}
			snprintf(t, sizeof(t), "%s@%s", g_push_queue[i].name, g_push_queue[i].version);
			if (!gate_holds(PKG_GATE_PUBLISH, t)) {
				pick = i;
				break;
			}
		}
		if (pick < 0)
			return 0; /* everything queued is waiting for a person */

		g_push_current = g_push_queue[pick];
		for (i = pick + 1; i < g_push_queue_count; i++)
			g_push_queue[i - 1] = g_push_queue[i];
		g_push_queue_count--;
		if (g_push_current.kind != PKG_PUSH_SIGNATURE) {
			char t[PKG_APPROVAL_TARGET_MAX];

			snprintf(t, sizeof(t), "%s@%s", g_push_current.name, g_push_current.version);
			approval_consume(PKG_GATE_PUBLISH, t);
		}

		/*
		 * Both kinds are gated on the ARTIFACT being present, because
		 * both are made from it: the signature does not exist yet and
		 * is produced in the child below, over these same bytes.
		 */
		cache_tarball_path(g_push_current.name, g_push_current.version, artifact,
		                   sizeof(artifact));
		if (g_push_current.kind == PKG_PUSH_SIGNATURE)
			cache_signature_path(g_push_current.name, g_push_current.version, tarball,
			                      sizeof(tarball));
		else
			snprintf(tarball, sizeof(tarball), "%s", artifact);
		if (stat(artifact, &st) == 0)
			break;
		/*
		 * Two different causes, and the message used to assert the
		 * wrong one. "No longer in the local cache" claims eviction --
		 * and the comment here said so outright -- but for a hostbuild
		 * the tarball was NEVER there: its output is a directory, and
		 * nothing built a tarball from it before this enqueue. Anyone
		 * reading the old line went looking for an LRU-sizing problem
		 * that did not exist, which is how `cix` sat five releases
		 * behind on the artifact cache without being noticed (#200).
		 *
		 * A message confidently wrong about its own cause is worse
		 * than a vague one. This one now says only what it knows: the
		 * tarball is not there. #200's fix means a hostbuild builds
		 * one before this point, so reaching here really is eviction
		 * -- but the message no longer bets on that.
		 */
		logstore_write("cixd", "info",
		                "artifact push: %s@%s has no tarball in the local cache -- not published",
		                g_push_current.name, g_push_current.version);
	}
	pkg_artifact_build_request(g_push_current.name, g_push_current.version, url, sizeof(url),
	                           auth_header, sizeof(auth_header));
	if (g_push_current.kind == PKG_PUSH_SIGNATURE) {
		size_t ulen = strlen(url);

		/* The sibling name the cache expects: the artifact's own name
		 * with .minisig on it, so stem, release and architecture parse
		 * identically on both (cix-cache#12). */
		if (ulen + 8 >= sizeof(url)) {
			logstore_write("cixd", "error",
			                "artifact push: %s@%s signature URL too long -- not published",
			                g_push_current.name, g_push_current.version);
			return 0;
		}
		snprintf(url + ulen, sizeof(url) - ulen, ".minisig");
	}
	push_status_path(status_path, sizeof(status_path));
	unlink(status_path);

	pid = fork();
	if (pid < 0) {
		logstore_write("cixd", "error", "artifact push: fork failed: %s -- %s@%s not published",
		                strerror(errno), g_push_current.name, g_push_current.version);
		return 0;
	}
	if (pid == 0) {
		char sha[65];
		char sha_header[96];

		/*
		 * Signing happens HERE, in the child, for the same reason the
		 * digest does: Ed25519 is PureEdDSA, so openssl reads the
		 * whole artifact, and a hundred-megabyte tarball would stall
		 * the reactor for as long as that takes (ADR-0247).
		 *
		 * The trusted comment is what makes this an approval of a
		 * particular build rather than of some bytes: it names the
		 * package, the exact revision and the artifact's digest, and
		 * minisign's global signature covers it, so none of the three
		 * can be edited afterwards. Without it a signature over
		 * glibc@2.44-17 verifies perfectly as 2.44-16 (ADR-0279).
		 */
		if (g_push_current.kind == PKG_PUSH_SIGNATURE) {
			char art_sha[65];
			char comment[PKG_NAME_MAX + PKG_VERSION_MAX + 96];

			if (pkg_run_capture_sha256(artifact, art_sha, sizeof(art_sha)) != 0)
				_exit(PKG_PUSH_EXIT_SHA_FAILED);
			snprintf(comment, sizeof(comment), "cix pkg %s@%s sha256=%s",
			          g_push_current.name, g_push_current.version, art_sha);
			if (releasekey_sign_file(artifact, tarball, comment) != RELEASEKEY_OK)
				_exit(PKG_PUSH_EXIT_SIGN_FAILED);
		}

		/* Digest in the child, deliberately: hashing a large tarball
		 * on the daemon's own thread would stall the event loop for
		 * exactly as long as the file is big. */
		if (pkg_run_capture_sha256(tarball, sha, sizeof(sha)) != 0)
			_exit(PKG_PUSH_EXIT_SHA_FAILED);
		snprintf(sha_header, sizeof(sha_header), "X-Cix-Sha256: %s", sha);
		{
			/* A real PUT with Content-Length set from the file
			 * itself (CURLOPT_INFILESIZE_LARGE inside curlfetch_
			 * perform()), which is what the receiver requires (it
			 * refuses chunked encoding) -- #410. The HTTP status is
			 * written to the status file below regardless of what
			 * libcurl itself calls success, so the reaper can report
			 * what the server said rather than a bare exit code, the
			 * same contract --write-out "%{http_code}" gave it. */
			struct curlfetch_opts opts;
			long http_status = 0;
			int upload_rc;
			int sfd;

			memset(&opts, 0, sizeof(opts));
			opts.url = url;
			opts.path = tarball;
			opts.upload = 1;
			opts.header1 = auth_header;
			opts.header2 = sha_header;
			opts.connect_timeout = PKG_CURL_CONNECT_TIMEOUT_SECS;
			opts.low_speed_limit = PKG_CURL_LOW_SPEED_LIMIT;
			opts.low_speed_time = PKG_CURL_LOW_SPEED_TIME_SECS;
			upload_rc = curlfetch_perform(&opts, &http_status, NULL, 0);

			sfd = open(status_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
			if (sfd >= 0) {
				char buf[16];
				int n = snprintf(buf, sizeof(buf), "%ld", http_status);
				ssize_t ignored = write(sfd, buf, (size_t)n);

				(void)ignored;
				close(sfd);
			}
			_exit(upload_rc == 0 ? 0 : 1);
		}
	}
	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		logstore_write("cixd", "error", "artifact push: pidfd_open failed: %s", strerror(errno));
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return 0;
	}
	g_push_in_flight = 1;
	*out_pid = pid;
	*out_pidfd = pidfd;
	snprintf(out_desc, desc_size, "%s@%s", g_push_current.name, g_push_current.version);
	/*
	 * st is the ARTIFACT's stat, which is the right size for an
	 * artifact push and would be a false one for a signature -- the
	 * signature does not exist yet when this line is written, because
	 * the child that makes it has only just been forked.
	 */
	if (g_push_current.kind == PKG_PUSH_SIGNATURE)
		logstore_write("cixd", "info",
		                "artifact push: signing %s@%s (%lld bytes) and uploading to %s",
		                g_push_current.name, g_push_current.version, (long long)st.st_size, url);
	else
		logstore_write("cixd", "info", "artifact push: uploading %s@%s (%lld bytes) to %s",
		                g_push_current.name, g_push_current.version, (long long)st.st_size, url);
	return 1;
}

/*
 * Reaps a finished push and reports what the server actually said.
 * The HTTP status matters more than the exit code here: a 409 is not
 * a failure of this host -- it means that name already holds different
 * bytes, which is the artifact server's immutability rule doing its
 * job and a genuine signal that two builds diverged.
 */
/*
 * ADR-0251, and the gap #306 turned out to be: a published artifact
 * approves itself in its own recipe.
 *
 * A recipe with no pkg_artifact_sha256 has its artifact tier skipped
 * entirely (ADR-0122), so that package can only ever be BUILT. On a host
 * that already has a toolchain this costs nothing visible, which is why
 * it survived indefinitely. On a cold host it is fatal and circular:
 * bash cannot be built without bash. A freshly installed box was left
 * unable to obtain a compiler for exactly this reason -- gcc names
 * linux-headers as a runtime dependency, dependencies always resolve to
 * their highest revision, and that revision had no approved artifact, so
 * it had to be built with the gcc that was waiting on it.
 *
 * The missing step was never automated: build, publish, then go back and
 * add the checksum by hand. Nobody could carry the checksum forward on a
 * revision bump either, because different bytes -- so it had to be redone
 * every time, and it was done for some revisions and not others with no
 * rule behind which. Counting bash's revisions: + + - + -.
 *
 * A gate would only report what a human then had to fix. This does the
 * step instead, at the one moment the bytes are known to be both built
 * here and accepted by the cache.
 *
 * Deliberately routed through recipe_adds_only_artifact_sha256(), the
 * same predicate that guards an operator's own POST /pkg/recipes: there
 * is one definition of "this edit is permitted to an immutable recipe",
 * and this path cannot drift from it.
 */
static void approve_published_artifact(const char *name, const char *version)
{
	char tarball[PATH_MAX], recipe_path[PATH_MAX], tmp_path[PATH_MAX];
	char sha[65];
	char *stored = NULL, *updated = NULL;
	size_t stored_len = 0, head = 0, need;
	const char *anchor, *nl;
	int fd;
	ssize_t w;

	cache_tarball_path(name, version, tarball, sizeof(tarball));
	if (pkg_run_capture_sha256(tarball, sha, sizeof(sha)) != 0)
		return; /* the tarball is gone from the cache -- nothing to approve */

	if ((size_t)snprintf(recipe_path, sizeof(recipe_path), "%s/%s/%s/build.sh", g_recipes_dir,
	                      name, version) >= sizeof(recipe_path))
		return;
	if (persist_read_file(recipe_path, &stored, &stored_len) != 0 || stored == NULL) {
		/*
		 * A PBS recipe (ADR-0305) reaches here and there is nothing to
		 * do: CPDL 0.1 rejects unknown package keys, so a build.cbs has
		 * nowhere to carry an artifact checksum
		 * (cix-build-system#161). Safe by construction -- this function
		 * only ever opens build.sh, so it cannot splice a line into a
		 * CPDL document -- but silence here has a real, confusing
		 * consequence: the package rebuilds from source on every
		 * install, forever, with nothing saying why. So it is said
		 * once, at the moment the approval would have happened.
		 */
		char pbs_path[PATH_MAX];
		struct stat st;

		if ((size_t)snprintf(pbs_path, sizeof(pbs_path), "%s/%s/%s/%s", g_recipes_dir, name,
		                      version, PKG_RECIPE_PBS_FILE) < sizeof(pbs_path) &&
		    stat(pbs_path, &st) == 0)
			logstore_write("cixd", "info",
			               "pkg: %s@%s is a PBS recipe, so its published artifact cannot be "
			               "approved and it will rebuild from source on every install -- CPDL "
			               "0.1 has nowhere to carry a checksum (cix-build-system#161)",
			               name, version);
		return;
	}

	/* Already approved, by an operator or by a previous publish. */
	if (strncmp(stored, "pkg_artifact_sha256=", 20) == 0 ||
	    strstr(stored, "\npkg_artifact_sha256=") != NULL) {
		free(stored);
		return;
	}

	/*
	 * Placed immediately after pkg_sha256=, which is where every recipe
	 * in this set already carries it, so a human reading the file finds
	 * it where they expect. Without that anchor there is nowhere
	 * unambiguous to put it and the recipe is left alone.
	 */
	anchor = strstr(stored, "\npkg_sha256=\"");
	if (anchor == NULL) {
		logstore_write("cixd", "warn",
		                "artifact push: %s@%s published but its recipe has no pkg_sha256= line to "
		                "anchor an approval after -- add pkg_artifact_sha256 by hand or the "
		                "artifact tier stays skipped",
		                name, version);
		free(stored);
		return;
	}
	nl = strchr(anchor + 1, '\n');
	if (nl == NULL) {
		free(stored);
		return;
	}
	head = (size_t)(nl - stored) + 1;

	need = stored_len + strlen("pkg_artifact_sha256=\"\"\n") + strlen(sha) + 1;
	updated = malloc(need);
	if (updated == NULL) {
		free(stored);
		return;
	}
	memcpy(updated, stored, head);
	w = snprintf(updated + head, need - head, "pkg_artifact_sha256=\"%s\"\n", sha);
	memcpy(updated + head + (size_t)w, stored + head, stored_len - head);
	updated[head + (size_t)w + (stored_len - head)] = '\0';

	/* The self-check: exactly the rule an operator's POST is held to. */
	if (!recipe_adds_only_artifact_sha256(stored, updated)) {
		logstore_write("cixd", "warn",
		                "artifact push: %s@%s published but writing its approval would have "
		                "changed more than one line -- recipe left untouched",
		                name, version);
		free(stored);
		free(updated);
		return;
	}

	if ((size_t)snprintf(tmp_path, sizeof(tmp_path), "%s.approve", recipe_path) >= sizeof(tmp_path)) {
		free(stored);
		free(updated);
		return;
	}
	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		size_t total = strlen(updated);

		w = write(fd, updated, total);
		if (close(fd) == 0 && w >= 0 && (size_t)w == total && rename(tmp_path, recipe_path) == 0)
			logstore_write("cixd", "info",
			                "artifact push: %s@%s approved its own published artifact (%.16s...)",
			                name, version, sha);
		else
			unlink(tmp_path);
	}
	free(stored);
	free(updated);
}

void pkg_artifact_push_completed(int exit_status)
{
	char status_path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;
	long code = 0;

	g_push_in_flight = 0;
	push_status_path(status_path, sizeof(status_path));
	if (persist_read_file(status_path, &buf, &len) == 0 && buf != NULL) {
		code = strtol(buf, NULL, 10);
		free(buf);
	}
	unlink(status_path);

	if (WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == PKG_PUSH_EXIT_SHA_FAILED) {
		logstore_write("cixd", "error",
		                "artifact push: %s@%s -- could not compute sha256, not published",
		                g_push_current.name, g_push_current.version);
		return;
	}
	if (WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == PKG_PUSH_EXIT_SIGN_FAILED) {
		/* The artifact is published and unsigned. Said plainly,
		 * because a host that cannot verify it will build from source
		 * instead and never explain why. */
		logstore_write("cixd", "error",
		                "artifact push: %s@%s is published but could NOT be signed -- other "
		                "hosts will rebuild it from source rather than trust it. Check the "
		                "release key with GET /v1/system/release-key.",
		                g_push_current.name, g_push_current.version);
		return;
	}
	if (code == 201 || code == 200 || code == 204) {
		if (g_push_current.kind == PKG_PUSH_SIGNATURE) {
			logstore_write("cixd", "info",
			                "artifact push: %s@%s signature published (HTTP %ld) -- any host "
			                "trusting this key can now install it without rebuilding",
			                g_push_current.name, g_push_current.version, code);
			return;
		}
		logstore_write("cixd", "info", "artifact push: %s@%s published (HTTP %ld)",
		                g_push_current.name, g_push_current.version, code);
		/* The bytes are now both built here and accepted by the cache,
		 * which is the only moment an approval is honestly earnable. */
		approve_published_artifact(g_push_current.name, g_push_current.version);
		/*
		 * And the same moment, recorded where it travels (ADR-0279).
		 * The line above reaches only this host's own recipe store;
		 * the signature reaches every host that can fetch the
		 * artifact, which is the whole of #403.
		 *
		 * Artifact first, then signature -- the reverse of the ISO
		 * rule, and deliberate: an ISO may not be published unsigned,
		 * a package may, so there is nothing to refuse here and the
		 * signature can only be made once the bytes are accepted.
		 */
		if (releasekey_is_set())
			pkg_artifact_push_enqueue_kind(g_push_current.name, g_push_current.version,
			                                PKG_PUSH_SIGNATURE);
		else
			logstore_write("cixd", "info",
			                "artifact push: %s@%s published unsigned -- this host holds no "
			                "release key, so other hosts will rebuild it from source rather "
			                "than trust it",
			                g_push_current.name, g_push_current.version);
		return;
	}
	if (code == 409) {
		logstore_write("cixd", "error",
		                "artifact push: %s@%s REJECTED (HTTP 409) -- that name already holds "
		                "different bytes on the artifact server. Two builds of the same "
		                "recipe version produced different output; the server's immutability "
		                "rule refused to overwrite it.",
		                g_push_current.name, g_push_current.version);
		return;
	}
	if (code == 401 || code == 403) {
		logstore_write("cixd", "error",
		                "artifact push: %s@%s rejected (HTTP %ld) -- the configured auth_token "
		                "was not accepted",
		                g_push_current.name, g_push_current.version, code);
		return;
	}
	logstore_write("cixd", "error",
	                "artifact push: %s@%s failed (HTTP %ld, curl exit status 0x%x)",
	                g_push_current.name, g_push_current.version, code, (unsigned)exit_status);
}

/* out_url: <base_url>/<name>-<version>.tar.gz (a trailing slash on
 * base_url, if present, is not doubled). out_header: an optional
 * "Authorization: Bearer <token>" when a token is configured, empty
 * string otherwise -- plain bearer auth, not a forge-specific
 * convention, since this is deliberately not talking to a git forge's
 * own API. */
/*
 * This host's architecture, as the artifact cache names it (#183).
 *
 * uname(2)'s machine field, asked once. It is the same string the cache
 * uses ("x86_64"), which is not a coincidence to rely on silently: if a
 * future port disagrees with the cache's vocabulary, this is the single
 * place that has to learn a mapping.
 *
 * Falling back to "unknown" rather than to a bare name is deliberate.
 * An artifact published as <name>-<version>-unknown.tar.gz is visibly
 * wrong and can be deleted; one published bare is indistinguishable
 * from a correct single-architecture artifact and will be served to a
 * machine that cannot run it.
 */
static const char *pkg_host_arch(void)
{
	static char arch[32];
	struct utsname u;

	if (arch[0] != '\0')
		return arch;
	if (uname(&u) == 0 && u.machine[0] != '\0')
		snprintf(arch, sizeof(arch), "%s", u.machine);
	else
		snprintf(arch, sizeof(arch), "unknown");
	return arch;
}

static void pkg_artifact_build_request(const char *name, const char *version, char *out_url,
                                        size_t out_url_size, char *out_header, size_t out_header_size)
{
	size_t len = strlen(g_artifact_base_url);

	if (len > 0 && g_artifact_base_url[len - 1] == '/')
		len--;
	/*
	 * Architecture is part of an artifact's identity (#183, #179).
	 *
	 * This is the single choke point for BOTH directions -- the fetch
	 * path and the push worker both arrive here -- which is why one
	 * edit fixes publishing and consuming together.
	 *
	 * Before this, every artifact this host published arrived at the
	 * cache unlabelled and was stamped by hand afterwards; the cache
	 * has carried architecture since its own ADR-0008 and deliberately
	 * never infers one, because "everything here was built for this" is
	 * something an operator knows and a parser must not guess.
	 *
	 * Safe to start asking for a stamped name before every artifact has
	 * one: the cache resolves a bare name for GET/HEAD while exactly
	 * one architecture is published, so an older bare artifact still
	 * fetches. Once two architectures exist it returns 409 and names
	 * the problem rather than picking one -- which is the whole point,
	 * because silently serving the wrong architecture to a machine that
	 * will boot it is the worst failure this system could have.
	 */
	snprintf(out_url, out_url_size, "%.*s/%s-%s-%s.tar.gz", (int)len, g_artifact_base_url, name,
	         version, pkg_host_arch());
	if (g_artifact_token[0] != '\0')
		snprintf(out_header, out_header_size, "Authorization: Bearer %s", g_artifact_token);
	else
		out_header[0] = '\0';
}

enum pkg_error pkg_artifact_push_request(const char *remote_name, char *out_url, size_t url_size,
                                          char *out_auth_header, size_t hdr_size)
{
	size_t len;

	if (remote_name == NULL || remote_name[0] == '\0')
		return PKG_ERR_INVALID_NAME;
	if (g_artifact_base_url[0] == '\0')
		return PKG_ERR_NOT_FOUND;

	len = strlen(g_artifact_base_url);
	if (len > 0 && g_artifact_base_url[len - 1] == '/')
		len--;
	snprintf(out_url, url_size, "%.*s/%s", (int)len, g_artifact_base_url, remote_name);
	if (g_artifact_token[0] != '\0')
		snprintf(out_auth_header, hdr_size, "Authorization: Bearer %s", g_artifact_token);
	else
		out_auth_header[0] = '\0';
	return PKG_OK;
}

/*
 * ADR-0221: how long an unused build environment is kept.
 *
 * Fixed rather than configurable, deliberately. The penalty for
 * reclaiming too eagerly is one recomposition, so there is no operator
 * decision here worth a knob -- and a knob would imply the number
 * matters more than it does.
 */
#define PKG_BUILDENV_RETENTION_SECONDS (14LL * 24 * 60 * 60)

static int buildenv_in_use(const char *name)
{
	int i;

	/* The one hard rule (ADR-0221): never touch an environment a live
	 * build holds. This is a question about a running process, not a
	 * prediction about future use. */
	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
		if (g_chains[i].name[0] != '\0' && strcmp(g_chains[i].buildenv_image, name) == 0)
			return 1;
	}
	return 0;
}

void pkg_buildenv_write_json(struct json_writer *w)
{
	char names[IMAGE_LIST_MAX][PKG_IMAGE_NAME_MAX];
	int n, i;

	jw_obj_open(w);
	jw_key(w, "buildenvs");
	jw_arr_open(w);
	n = image_list_names(names, IMAGE_LIST_MAX);
	for (i = 0; i < n; i++) {
		if (strncmp(names[i], PKG_BUILDENV_IMAGE_PREFIX, strlen(PKG_BUILDENV_IMAGE_PREFIX)) != 0)
			continue;
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, names[i]);
		jw_key(w, "idle_seconds");
		jw_int(w, buildenv_idle_seconds(names[i]));
		jw_key(w, "in_use");
		jw_bool(w, buildenv_in_use(names[i]));
		jw_key(w, "retention_seconds");
		jw_int(w, PKG_BUILDENV_RETENTION_SECONDS);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}

enum pkg_error pkg_buildenv_delete(const char *name)
{
	if (name == NULL ||
	    strncmp(name, PKG_BUILDENV_IMAGE_PREFIX, strlen(PKG_BUILDENV_IMAGE_PREFIX)) != 0)
		return PKG_ERR_INVALID_NAME;
	{
		char cur[IMAGE_VERSION_MAX];

		if (image_current_version(name, cur, sizeof(cur)) != IMAGE_OK)
			return PKG_ERR_NOT_FOUND;
	}
	if (buildenv_in_use(name))
		return PKG_ERR_BUSY;
	if (image_delete(name) != IMAGE_OK)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

int pkg_buildenv_reclaim(void)
{
	char names[IMAGE_LIST_MAX][PKG_IMAGE_NAME_MAX];
	int n, i, removed = 0;

	n = image_list_names(names, IMAGE_LIST_MAX);
	for (i = 0; i < n; i++) {
		if (strncmp(names[i], PKG_BUILDENV_IMAGE_PREFIX, strlen(PKG_BUILDENV_IMAGE_PREFIX)) != 0)
			continue;
		if (buildenv_in_use(names[i]))
			continue;
		if (buildenv_idle_seconds(names[i]) < PKG_BUILDENV_RETENTION_SECONDS)
			continue;
		if (image_delete(names[i]) == IMAGE_OK) {
			removed++;
			logstore_write("cixd", "info",
			               "build environment %s reclaimed after %lld days idle -- it will be "
			               "recomposed automatically if a build wants it again (ADR-0221)",
			               names[i], buildenv_idle_seconds(names[i]) / (24 * 60 * 60));
		}
	}
	return removed;
}

int pkg_artifact_push_is_enabled(void)
{
	return g_artifact_push_enabled;
}

/* Where start_fetch_for()'s child stages a checksum-verified artifact
 * fetch before pkg_fetch_completed() promotes it into the real local
 * cache -- deliberately under g_sources_dir (not g_cache_dir): an
 * unverified download never touches the cache directory at all, only
 * a file that has already passed the recipe's own sha256 check does. */
static void artifact_sentinel_path(const char *name, const char *version, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/.artifact-%s-%s.tar.gz", g_sources_dir, name, version);
}

/* ---- pkg/ redesign Part 4 (ADR-0123): image recipes + image-artifact fetch ---- */

#define IMAGE_RECIPE_TOKEN_MAX (PKG_NAME_MAX + PKG_VERSION_MAX + 16)

struct image_recipe {
	struct image_manifest_entry entries[IMAGE_MANIFEST_MAX_PACKAGES];
	int entry_count;
};

static char g_image_recipes_dir[PATH_MAX];

/*
 * ADR-0209: applying an image recipe is synchronous, always. The
 * async half of this -- fetch a whole-rootfs artifact, verify it,
 * extract it as the image's new version -- is retired along with the
 * rest of ADR-0123's fast path, so there is no in-flight job to track
 * and no separate status to poll. The last-apply state fields, the
 * IMAGE_APPLY_* enum and the busy flag all went with it.
 */
void image_recipe_init(const char *pkg_dir)
{
	snprintf(g_image_recipes_dir, sizeof(g_image_recipes_dir), "%s/image-recipes", pkg_dir);
}

/* ADR-0141 Phase 4: path-only repoint, called from pkg_repoint() --
 * unlike image_recipe_init(), never resets the last-apply-status
 * fields (a live migration mid-apply-status has nothing to do with
 * where the recipe files themselves live). */
void image_recipe_repoint(const char *pkg_dir)
{
	snprintf(g_image_recipes_dir, sizeof(g_image_recipes_dir), "%s/image-recipes", pkg_dir);
}

static void image_recipe_path(const char *name, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s.recipe", g_image_recipes_dir, name);
}

/* buf is modified in place by extract_line_value()/tokenize_into(),
 * same convention parse_recipe() already has for its own raw_source/
 * raw_sha256 buffers. */
static int parse_image_recipe_buf(char *buf, struct image_recipe *out)
{
	char raw_packages[IMAGE_MANIFEST_MAX_PACKAGES * IMAGE_RECIPE_TOKEN_MAX];
	char tokens[IMAGE_MANIFEST_MAX_PACKAGES][IMAGE_RECIPE_TOKEN_MAX];
	int n, i;

	memset(out, 0, sizeof(*out));
	if (extract_line_value(buf, "image_packages=", raw_packages, sizeof(raw_packages)) != 0)
		return -1;
	n = tokenize_into(raw_packages, (char *)tokens, IMAGE_RECIPE_TOKEN_MAX,
	                   IMAGE_MANIFEST_MAX_PACKAGES);
	if (n <= 0)
		return -1;
	for (i = 0; i < n; i++) {
		char *pkg_tok, *mode_tok, *ver_tok, *save;

		pkg_tok = strtok_r(tokens[i], ":", &save);
		mode_tok = strtok_r(NULL, ":", &save);
		ver_tok = strtok_r(NULL, ":", &save);
		if (pkg_tok == NULL || mode_tok == NULL || ver_tok == NULL)
			return -1;
		if (!pkg_name_is_valid(pkg_tok) || strlen(ver_tok) >= PKG_VERSION_MAX)
			return -1;
		if (strcmp(mode_tok, "pinned") == 0)
			out->entries[i].mode = IMAGE_PKG_PINNED;
		else if (strcmp(mode_tok, "rolling") == 0)
			out->entries[i].mode = IMAGE_PKG_ROLLING;
		else
			return -1;
		snprintf(out->entries[i].package, sizeof(out->entries[i].package), "%s", pkg_tok);
		snprintf(out->entries[i].version, sizeof(out->entries[i].version), "%s", ver_tok);
	}
	out->entry_count = n;
	return 0;
}

enum pkg_error image_recipe_add(const char *name, const char *content)
{
	struct image_recipe parsed;
	char *buf_copy;
	char staging_path[PATH_MAX], path[PATH_MAX];
	int fd;
	size_t len;
	ssize_t written;

	if (!pkg_image_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	if (persist_mkdir_p(g_image_recipes_dir) != 0)
		return PKG_ERR_PERSIST_FAILED;

	buf_copy = strdup(content);
	if (buf_copy == NULL)
		return PKG_ERR_PERSIST_FAILED;
	if (parse_image_recipe_buf(buf_copy, &parsed) != 0) {
		free(buf_copy);
		return PKG_ERR_INVALID_RECIPE;
	}
	free(buf_copy);

	/* Staged then renamed into place -- same crash-safety convention
	 * pkg_recipe_add() already established (never a half-written file
	 * visible under the real name). */
	snprintf(staging_path, sizeof(staging_path), "%s/.%s.recipe.new", g_image_recipes_dir, name);
	fd = open(staging_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return PKG_ERR_PERSIST_FAILED;
	len = strlen(content);
	written = write(fd, content, len);
	close(fd);
	if (written < 0 || (size_t)written != len) {
		unlink(staging_path);
		return PKG_ERR_PERSIST_FAILED;
	}
	image_recipe_path(name, path, sizeof(path));
	if (rename(staging_path, path) != 0) {
		unlink(staging_path);
		return PKG_ERR_PERSIST_FAILED;
	}
	return PKG_OK;
}

enum pkg_error image_recipe_get(const char *name, char **out_content, size_t *out_len)
{
	char path[PATH_MAX];

	if (!pkg_image_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	image_recipe_path(name, path, sizeof(path));
	if (persist_read_file(path, out_content, out_len) != 0 || *out_content == NULL)
		return PKG_ERR_NOT_FOUND;
	return PKG_OK;
}

enum pkg_error image_recipe_rm(const char *name)
{
	char path[PATH_MAX];

	if (!pkg_image_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	image_recipe_path(name, path, sizeof(path));
	if (unlink(path) != 0)
		return (errno == ENOENT) ? PKG_ERR_NOT_FOUND : PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

void image_recipe_write_json_list(struct json_writer *w)
{
	DIR *d = opendir(g_image_recipes_dir);
	struct dirent *de;

	jw_arr_open(w);
	if (d != NULL) {
		while ((de = readdir(d)) != NULL) {
			size_t nlen = strlen(de->d_name);
			char namebuf[PKG_IMAGE_NAME_MAX];
			size_t copy_len;

			if (de->d_name[0] == '.' || nlen <= 7 ||
			    strcmp(de->d_name + nlen - 7, ".recipe") != 0)
				continue;
			copy_len = nlen - 7;
			if (copy_len >= sizeof(namebuf))
				copy_len = sizeof(namebuf) - 1;
			memcpy(namebuf, de->d_name, copy_len);
			namebuf[copy_len] = '\0';
			jw_obj_open(w);
			jw_key(w, "name");
			jw_str(w, namebuf);
			jw_obj_close(w);
		}
		closedir(d);
	}
	jw_arr_close(w);
}

/*
 * Container recipes (ADR-0151): a git-syncable, reproducible template
 * for a real POST /v1/containers body -- cmd/files/network/restart-
 * policy/sysctls, everything an image recipe deliberately does NOT
 * cover (ADR-0123's own image_packages= is package-manifest-only by
 * design; a container recipe is the missing other half). Unlike an
 * image recipe, content is stored and applied AS-IS: it must already
 * be the exact JSON body create_container_from_body() (main.c) would
 * accept, including its own "name" field, which must match the name
 * this recipe is published under (same "uploaded name must match the
 * content's own declared name" contract pkg_recipe_add() already has
 * for pkg_name=, and image recipes deliberately don't need since they
 * carry no internal name field of their own). A recipe MAY embed
 * {{SECRET:KEY}} tokens inside any string value (typically staged
 * file content) -- container_recipe_apply_start() substitutes them
 * from the apply request's own secrets map before ever calling
 * create_container_from_body(), so a real credential never has to be
 * committed to git (the same class of gap cix.recipe's own
 * REPLACE_WITH_REAL_TOKEN placeholder already established a
 * convention for, generalized here into a real substitution
 * mechanism instead of a manual find-and-replace).
 */
static char g_container_recipes_dir[PATH_MAX];

void container_recipe_init(const char *pkg_dir)
{
	snprintf(g_container_recipes_dir, sizeof(g_container_recipes_dir), "%s/container-recipes",
	         pkg_dir);
}

/* ADR-0141 Phase 4 style path-only repoint, mirroring image_recipe_
 * repoint()'s own doc comment exactly -- see pkg_repoint(). */
void container_recipe_repoint(const char *pkg_dir)
{
	snprintf(g_container_recipes_dir, sizeof(g_container_recipes_dir), "%s/container-recipes",
	         pkg_dir);
}

static void container_recipe_path(const char *name, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s.recipe", g_container_recipes_dir, name);
}

enum pkg_error container_recipe_add(const char *name, const char *content)
{
	struct json_value *root;
	const struct json_value *jname;
	const char *body_name;
	char staging_path[PATH_MAX], path[PATH_MAX];
	int fd;
	size_t len;
	ssize_t written;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;

	/* Must parse as real JSON (placeholder tokens are plain text INSIDE
	 * an already-open string value, e.g. "content":"...{{SECRET:X}}...",
	 * so a well-formed recipe with placeholders is still well-formed
	 * JSON -- this check catches a genuinely malformed recipe, not
	 * anything placeholder-related), and its own "name" field must
	 * match name, the same contract pkg_recipe_add() already has for
	 * pkg_name= -- a mismatch is rejected outright rather than silently
	 * publishing a recipe that would create a differently-named
	 * container than its own catalog entry implies. */
	root = json_parse(content, strlen(content));
	if (root == NULL)
		return PKG_ERR_INVALID_RECIPE;
	jname = json_object_get(root, "name");
	body_name = json_as_string(jname);
	if (body_name == NULL || strcmp(body_name, name) != 0) {
		json_free(root);
		return PKG_ERR_INVALID_RECIPE;
	}
	json_free(root);

	if (persist_mkdir_p(g_container_recipes_dir) != 0)
		return PKG_ERR_PERSIST_FAILED;

	/* Staged then renamed into place -- same crash-safety convention
	 * image_recipe_add()/pkg_recipe_add() already established. */
	snprintf(staging_path, sizeof(staging_path), "%s/.%s.recipe.new", g_container_recipes_dir,
	         name);
	fd = open(staging_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return PKG_ERR_PERSIST_FAILED;
	len = strlen(content);
	written = write(fd, content, len);
	close(fd);
	if (written < 0 || (size_t)written != len) {
		unlink(staging_path);
		return PKG_ERR_PERSIST_FAILED;
	}
	container_recipe_path(name, path, sizeof(path));
	if (rename(staging_path, path) != 0) {
		unlink(staging_path);
		return PKG_ERR_PERSIST_FAILED;
	}
	return PKG_OK;
}

enum pkg_error container_recipe_get(const char *name, char **out_content, size_t *out_len)
{
	char path[PATH_MAX];

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	container_recipe_path(name, path, sizeof(path));
	if (persist_read_file(path, out_content, out_len) != 0 || *out_content == NULL)
		return PKG_ERR_NOT_FOUND;
	return PKG_OK;
}

enum pkg_error container_recipe_rm(const char *name)
{
	char path[PATH_MAX];

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	container_recipe_path(name, path, sizeof(path));
	if (unlink(path) != 0)
		return (errno == ENOENT) ? PKG_ERR_NOT_FOUND : PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

void container_recipe_write_json_list(struct json_writer *w)
{
	DIR *d = opendir(g_container_recipes_dir);
	struct dirent *de;

	jw_arr_open(w);
	if (d != NULL) {
		while ((de = readdir(d)) != NULL) {
			size_t nlen = strlen(de->d_name);
			char namebuf[PKG_NAME_MAX];
			size_t copy_len;

			if (de->d_name[0] == '.' || nlen <= 7 ||
			    strcmp(de->d_name + nlen - 7, ".recipe") != 0)
				continue;
			copy_len = nlen - 7;
			if (copy_len >= sizeof(namebuf))
				copy_len = sizeof(namebuf) - 1;
			memcpy(namebuf, de->d_name, copy_len);
			namebuf[copy_len] = '\0';
			jw_obj_open(w);
			jw_key(w, "name");
			jw_str(w, namebuf);
			jw_obj_close(w);
		}
		closedir(d);
	}
	jw_arr_close(w);
}

/*
 * Substitutes every {{SECRET:KEY}} token in content with secrets[KEY]
 * (from the apply request's own "secrets" object), JSON-escaping each
 * substituted value via jw_raw_escaped_content() since it's spliced
 * into the middle of an already-open JSON string literal -- content
 * itself is copied through verbatim (jw_raw_text()) everywhere else.
 * An unmatched token (no such key in secrets) is left completely
 * untouched, on purpose: a missing secret should surface as whatever
 * error create_container_from_body() gives for the resulting
 * malformed value (e.g. a literal "{{SECRET:X}}" landing in a config
 * file), not silently swallowed here, and a recipe author previewing
 * raw content via container_recipe_get() should always see the real,
 * unsubstituted token, never a guess. Returns a newly allocated
 * string the caller frees, or NULL on allocation failure.
 */
static char *container_recipe_substitute_secrets(const char *content,
                                                   const struct json_value *secrets)
{
	struct json_writer w;
	const char *p = content;
	char *out;

	jw_init(&w);
	while (*p != '\0') {
		/* Issue #66: two token families share one scan -- the original
		 * {{SECRET:KEY}} (values from the apply request) and
		 * {{LDAP:FIELD}} (values from the daemon's own LDAP client
		 * config, ldap_config_get() -- URI / BASE_DN / BIND_DN /
		 * BIND_PASSWORD), so recipes needing LDAP login carry no
		 * copied values and no per-apply secret at all. Same
		 * unmatched-token-left-verbatim posture for both. */
		const char *stok = strstr(p, "{{SECRET:");
		const char *ltok = strstr(p, "{{LDAP:");
		const char *tok;
		int is_ldap;
		size_t prefix_len;
		const char *key_end;
		char key[128];
		size_t key_len;
		const struct json_value *jval;
		const char *val;
		char uri[600];

		if (stok == NULL && ltok == NULL) {
			jw_raw_text(&w, p, strlen(p));
			break;
		}
		if (stok != NULL && (ltok == NULL || stok < ltok)) {
			tok = stok;
			is_ldap = 0;
			prefix_len = 9;
		} else {
			tok = ltok;
			is_ldap = 1;
			prefix_len = 7;
		}
		jw_raw_text(&w, p, (size_t)(tok - p));

		key_end = strstr(tok + prefix_len, "}}");
		if (key_end == NULL) {
			/* No closing "}}" anywhere -- not a real token, copy the
			 * rest verbatim rather than loop forever. */
			jw_raw_text(&w, tok, strlen(tok));
			break;
		}
		key_len = (size_t)(key_end - (tok + prefix_len));
		if (key_len == 0 || key_len >= sizeof(key)) {
			jw_raw_text(&w, tok, (size_t)(key_end + 2 - tok));
			p = key_end + 2;
			continue;
		}
		memcpy(key, tok + prefix_len, key_len);
		key[key_len] = '\0';

		if (is_ldap) {
			const struct ldap_config *lc = ldap_config_get();

			val = NULL;
			if (strcmp(key, "URI") == 0) {
				/*
				 * Derived, not read straight off client_uri (#327).
				 * ldap_effective_client_uri() is the one place that
				 * answers "which LDAP servers should a client talk
				 * to": an explicitly configured list when there is
				 * one, otherwise the live IPs of the registered
				 * servers, health-filtered either way (#81/#84).
				 *
				 * Reading the raw field meant this token resolved
				 * only when an operator had also hand-typed a copy
				 * of the server list the daemon already tracks --
				 * the exact duplication #66 was filed to remove,
				 * and which that issue's own spec says this token
				 * should be "rendered from the registered-servers
				 * list". The daemon's own nslcd.conf rendering has
				 * always used the derived value, so one container
				 * create produced two files disagreeing about the
				 * same servers, with only the token left verbatim.
				 */
				if (ldap_effective_client_uri(uri, sizeof(uri)))
					val = uri;
			} else if (strcmp(key, "BASE_DN") == 0 && lc->base_dn[0] != '\0')
				val = lc->base_dn;
			else if (strcmp(key, "BIND_DN") == 0 && lc->bind_dn[0] != '\0')
				val = lc->bind_dn;
			else if (strcmp(key, "BIND_PASSWORD") == 0 && lc->bind_password[0] != '\0')
				val = lc->bind_password;
		} else {
			jval = secrets != NULL ? json_object_get(secrets, key) : NULL;
			val = json_as_string(jval);
		}
		if (val != NULL)
			jw_raw_escaped_content(&w, val);
		else
			jw_raw_text(&w, tok, (size_t)(key_end + 2 - tok));
		p = key_end + 2;
	}

	out = malloc(w.len + 1);
	if (out != NULL) {
		memcpy(out, w.buf, w.len);
		out[w.len] = '\0';
	}
	jw_free(&w);
	return out;
}

char *container_recipe_render(const char *name, const struct json_value *secrets,
                               enum pkg_error *out_err)
{
	char *content;
	size_t content_len;
	char *rendered;
	enum pkg_error rc;

	rc = container_recipe_get(name, &content, &content_len);
	if (rc != PKG_OK) {
		*out_err = rc;
		return NULL;
	}

	rendered = container_recipe_substitute_secrets(content, secrets);
	free(content);
	if (rendered == NULL) {
		*out_err = PKG_ERR_PERSIST_FAILED;
		return NULL;
	}
	/*
	 * Issue #153's sibling case. An unmatched {{SECRET:KEY}} is left
	 * untouched deliberately (see substitute_secrets above) -- a
	 * preview must show the real token, not a guess. The reasoning
	 * there assumes the leftover surfaces as an error downstream, and
	 * for most fields it does.
	 *
	 * It does NOT for a files[] entry: a config file containing a
	 * literal "{{SECRET:X}}" is perfectly valid JSON and creates a
	 * perfectly valid container, which then misbehaves at runtime for
	 * reasons nothing connects back to a missing secret. That is the
	 * same shape as the dnsmasq pidfile and the non-executable
	 * artifact -- valid by every check the platform makes, wrong in
	 * the one way that matters.
	 *
	 * Warned rather than refused, because the deliberate leave-alone
	 * behaviour is still correct and a caller may genuinely intend a
	 * literal. The point is that it stops being silent.
	 */
	if (strstr(rendered, "{{SECRET:") != NULL)
		logstore_write("cixd", "warn",
		                "container recipe %s: rendered with an unsubstituted {{SECRET:...}} "
		                "token -- the container will be created with the placeholder as "
		                "literal text, which is valid but almost certainly not intended",
		                name);
	*out_err = PKG_OK;
	return rendered;
}

struct image_recipe_ref {
	const char *name;
	const char *version;
};

static int image_recipe_ref_cmp(const void *a, const void *b)
{
	return strcmp(((const struct image_recipe_ref *)a)->name,
	              ((const struct image_recipe_ref *)b)->name);
}

/*
 * ADR-0209: apply is bulk-declare, and only bulk-declare.
 *
 * ADR-0123 added a second path here: a recipe that was entirely
 * "pinned" and carried an image_artifact_sha256 could be applied by
 * fetching one whole-rootfs tarball, verifying it, and extracting it
 * as the image's new version -- skipping every per-package build. That
 * is exactly the mechanism that took one contaminated capture and
 * shipped it to every host (#168), and it is a second representation
 * of something already fully described by a package set plus a
 * manifest. It is gone: package artifacts are the only published
 * binaries now, each checksummed and traceable to a recipe and the
 * Cix host that built it.
 *
 * What remains is what the common case always did -- declare every
 * entry into the image's manifest, exactly what N manual
 * PUT /v1/images/{name}/manifest calls would do. Realizing a declared
 * entry into a real rootfs still needs an explicit pkg install, the
 * same as any other manifest edit.
 */
enum pkg_error pkg_image_recipe_apply_start(const char *image)
{
	char path[PATH_MAX];
	char *buf;
	size_t len;
	struct image_recipe recipe;
	int i;

	if (!pkg_image_is_valid(image))
		return PKG_ERR_INVALID_NAME;
	/*
	 * ADR-0270 removed a pkg_any_job_busy() check here.
	 *
	 * It was a leftover from the whole-rootfs artifact fetch ADR-0209
	 * deleted: back then apply really did fetch and extract, which
	 * needed a job slot. What remains is bulk-declare -- N
	 * image_manifest_set() calls -- and the manual path for exactly
	 * that, handle_image_manifest_set() in api_image.c, has never
	 * checked busy at all. Two callers, the same work, one of them
	 * refusing whenever any unrelated package build happened to be
	 * running.
	 */

	image_recipe_path(image, path, sizeof(path));
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return PKG_ERR_NOT_FOUND;
	if (parse_image_recipe_buf(buf, &recipe) != 0) {
		free(buf);
		return PKG_ERR_INVALID_RECIPE;
	}
	free(buf);

	for (i = 0; i < recipe.entry_count; i++) {
		enum image_error ierr =
		    image_manifest_set(image, recipe.entries[i].package, recipe.entries[i].mode,
		                        recipe.entries[i].version);

		if (ierr == IMAGE_ERR_NOT_FOUND)
			return PKG_ERR_TARGET_IMAGE_NOT_FOUND;
		if (ierr != IMAGE_OK)
			return PKG_ERR_INVALID_RECIPE;
	}
	return PKG_OK;
}

/*
 * Name-only enumerations, for reconciling what is DECLARED against what
 * is actually INSTALLED (issue #97).
 *
 * The write_json_list() functions above already walk exactly these
 * directories, but they emit JSON rather than returning names, and the
 * reconciliation needs the names to compare. Kept next to the writers
 * so the two can never disagree about what counts as a recipe.
 */
static int recipe_dir_names(const char *dir, char names[][PKG_IMAGE_NAME_MAX], int max)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	int count = 0;

	if (d == NULL)
		return 0;
	while ((de = readdir(d)) != NULL && count < max) {
		size_t nlen = strlen(de->d_name);
		size_t copy_len;

		if (de->d_name[0] == '.' || nlen <= 7 || strcmp(de->d_name + nlen - 7, ".recipe") != 0)
			continue;
		copy_len = nlen - 7;
		if (copy_len >= PKG_IMAGE_NAME_MAX)
			copy_len = PKG_IMAGE_NAME_MAX - 1;
		memcpy(names[count], de->d_name, copy_len);
		names[count][copy_len] = '\0';
		count++;
	}
	closedir(d);
	return count;
}

int image_recipe_list_names(char names[][PKG_IMAGE_NAME_MAX], int max)
{
	return recipe_dir_names(g_image_recipes_dir, names, max);
}

int container_recipe_list_names(char names[][PKG_IMAGE_NAME_MAX], int max)
{
	return recipe_dir_names(g_container_recipes_dir, names, max);
}

/*
 * Package recipes are stored per-name-per-version (ADR-0107), so the
 * directory walk is one level shallower than the version dirs: each
 * entry under the recipes root IS a package name.
 */
int pkg_recipe_list_names(char names[][PKG_IMAGE_NAME_MAX], int max)
{
	DIR *d = opendir(g_recipes_dir);
	struct dirent *de;
	int count = 0;

	if (d == NULL)
		return 0;
	while ((de = readdir(d)) != NULL && count < max) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(names[count], PKG_IMAGE_NAME_MAX, "%s", de->d_name);
		count++;
	}
	closedir(d);
	return count;
}

int pkg_positions(const char *name, struct pkg_position *out, int max)
{
	int i, n = 0;

	if (name == NULL || out == NULL)
		return 0;
	for (i = 0; i < PKG_MAX_PACKAGES && n < max; i++) {
		const struct pkg_entry *e = &g_packages[i];

		if (!e->in_use || strcmp(e->name, name) != 0)
			continue;
		memset(&out[n], 0, sizeof(out[n]));
		snprintf(out[n].image, sizeof(out[n].image), "%s", e->image);
		snprintf(out[n].version, sizeof(out[n].version), "%s", e->version);
		snprintf(out[n].error, sizeof(out[n].error), "%s", e->error);
		out[n].state = e->state;
		out[n].stage = e->stage;
		out[n].status = e->status;
		/*
		 * An in-flight job has no recorded outcome yet, and reporting
		 * it as PIPELINE_OK would say the opposite of what is true. It
		 * stands AT the stage it is currently doing, blocked in the
		 * sense that nothing downstream can proceed until it finishes.
		 */
		if (e->status == PIPELINE_OK) {
			if (e->state == PKG_STATE_FETCHING) {
				out[n].stage = PIPELINE_FETCH;
				out[n].status = PIPELINE_BLOCKED;
			} else if (e->state == PKG_STATE_BUILDING) {
				out[n].stage = PIPELINE_BUILD;
				out[n].status = PIPELINE_BLOCKED;
			} else {
				out[n].stage = PIPELINE_INSTALL;
			}
		}
		n++;
	}
	return n;
}

int pkg_recipe_list_versions(const char *name, char versions[][PKG_VERSION_MAX], int max)
{
	char name_dir[PATH_MAX];
	DIR *d;
	struct dirent *de;
	int count = 0;

	if (!pkg_name_is_valid(name))
		return 0;
	snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, name);
	d = opendir(name_dir);
	if (d == NULL)
		return 0;
	while ((de = readdir(d)) != NULL && count < max) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(versions[count], PKG_VERSION_MAX, "%s", de->d_name);
		count++;
	}
	closedir(d);
	return count;
}

int pkg_recipe_latest_version(const char *name, char *out, size_t out_size)
{
	char latest[PKG_VERSION_MAX];

	if (name == NULL || out == NULL || out_size == 0)
		return -1;
	if (!pkg_name_is_valid(name))
		return -1;
	if (recipe_latest_version(name, latest, sizeof(latest)) != 0)
		return -1;
	snprintf(out, out_size, "%s", latest);
	return 0;
}

/*
 * Installed package names, de-duplicated: the same package installed
 * into three images is one piece of software, not three.
 */
int pkg_installed_list_names(char names[][PKG_IMAGE_NAME_MAX], int max)
{
	int count = 0;
	int i, k;

	for (i = 0; i < PKG_MAX_PACKAGES && count < max; i++) {
		if (!g_packages[i].in_use || g_packages[i].state != PKG_STATE_INSTALLED)
			continue;
		for (k = 0; k < count; k++) {
			if (strcmp(names[k], g_packages[i].name) == 0)
				break;
		}
		if (k < count)
			continue;
		snprintf(names[count], PKG_IMAGE_NAME_MAX, "%s", g_packages[i].name);
		count++;
	}
	return count;
}
