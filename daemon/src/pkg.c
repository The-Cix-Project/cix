#include "libdirs.h"
#include "pkg.h"
#include "squashfsimg.h"
#include "nsswitch.h"
#include "curlfetch.h"
#include "osrelease.h"
#include "version.h"
#include "pkgpolicy.h"
#include "imagepolicy.h"
#include "hostproc.h"
#include "image.h"
#include "btrfs.h"
#include "ldap.h"
#include "linux_compat.h"
#include "logstore.h"
#include "elfcheck.h"
#include "namecheck.h"
#include "cbsrecipe.h"
#include "forgecommit.h"
#include "catalogue.h"
#include "srcgitea.h"
#include "srcrecord.h"
#include "pgpverify.h"
#include "upstreamkeys.h"
#include "srcresolve.h"
#include "srcupstream.h"
#include "srctrust.h"
#include "pkgrepo.h"
#include "pkgsource.h"
#include "json.h"
#include "jsondiff.h"
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
#include <sys/statvfs.h>
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
	 * fixed path. A CBS recipe stages wherever CBS's workspace puts
	 * it: `cbs build --staged W` creates W/src, W/build, W/dest and
	 * W/cache, and an install phase's ${dest} is W/dest. Pointing the
	 * variable at that subdirectory is one assignment; the
	 * alternative was moving the tree afterwards in the build
	 * command, which would have made every CBS recipe depend on mv
	 * and rmdir being present -- tools a recipe declares, or does
	 * not.
	 */
	char build_destdir_env[PATH_MAX + 16];
	/*
	 * The same destination as build_destdir_env above, without the
	 * "PKG_DESTDIR=/" prefix, so the post-build harvest can find the
	 * tree the build was TOLD to write to rather than re-deriving it.
	 *
	 * pkg_build_completed() has no recipe in scope -- it is handed a
	 * container name and an exit status -- so re-deriving would mean
	 * looking the recipe up again and trusting the answer to match.
	 * It did not match once already: the first CBS build harvested
	 * build/pkg-dest while the build staged into the cbs workspace,
	 * and published a package with zero files and a state of
	 * "installed". Remembering beats re-deriving.
	 */
	char build_dest_rel[64];
	/*
	 * The artifact format this build will produce (ADR-0307),
	 * remembered here for the same reason build_dest_rel above is:
	 * pkg_build_completed() is handed a container name and an exit
	 * status and has no recipe in scope, so the alternative is
	 * looking the recipe up again and trusting the answer to match.
	 * It did not match once already -- the first CBS build harvested
	 * the wrong directory and published a package with zero files
	 * and a state of "installed". Remembering beats re-deriving, and
	 * a format read at prepare time is the one the build was
	 * actually driven with.
	 */
	char artifact_format[PKG_ARTIFACT_FORMAT_MAX];
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
	/* #424: when the install stage began -- a successful build's output
	 * starting into the image. 0 until then. */
	time_t install_started_at;
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
	 * The two halves `version` above is the join of (ADR-0305):
	 * CPDL's own `version` and `release`, carried separately because
	 * they cannot be recovered from the join.
	 *
	 * `0.2.57-361` splits into 0.2.57 and 361; `v2.2.0-rc1` does not
	 * split at all, its last hyphen introducing a pre-release tag.
	 * Nothing in the string distinguishes the two cases. The cache
	 * guesses -- trailing digits are the release, absent means 1 --
	 * which is good enough to name a file and not good enough to
	 * write into an artifact's own metadata, which is what
	 * `cbs package --version X --release N` does (cix#528).
	 *
	 * A shell recipe has no release concept, so it keeps
	 * release 1 -- the same value the cache already infers for it.
	 */
	char bare_version[PKG_VERSION_MAX];
	long long release;
	/*
	 * Which artifact format this version publishes, as the recipe
	 * DECLARED it in the explain document (ADR-0307). Read rather
	 * than assumed: CPDL accepts `format "tar.gz"`, which `cbs build`
	 * then refuses to execute ("standalone builds require cixpkg"), so
	 * the daemon sees that declaration and refuses it at publish
	 * rather than failing at build time. Always cixpkg on a recipe
	 * that was published.
	 */
	char artifact_format[PKG_ARTIFACT_FORMAT_MAX];
	/* source[0]/sha256[0] is the main source and the rest are extras;
	 * cixd fetches and verifies every one host-side and hands each to
	 * cbs as a source-cache entry named by its sha256 (ADR-0305), which
	 * is the only place a build reads it from (cix#569). */
	char source[PKG_MAX_SOURCES][PKG_URL_MAX];
	char sha256[PKG_MAX_SOURCES][PKG_SHA256_MAX];
	int source_count;
	/*
	 * Fallback urls for the sources above (#507). source[i] is a
	 * source's FIRST url; a CPDL recipe may declare more, which are
	 * ordered mirrors for that same source identity sharing its one
	 * checksum. mirror_source[k] says which source index mirror_url[k]
	 * belongs to, and entries for one source appear in the order the
	 * recipe gave them -- which is the order the fetch tries them in,
	 * after that source's own source[i] fails.
	 *
	 * A shell recipe has no syntax for a second url and always leaves
	 * mirror_count at 0, which is exactly the behaviour every recipe
	 * had before this field existed.
	 */
	char mirror_url[PKG_MAX_MIRRORS][PKG_URL_MAX];
	int mirror_source[PKG_MAX_MIRRORS];
	int mirror_count;
	char depends[PKG_DEPENDS_MAX];
	/* cix#553: packages whose files this one may take over (CPDL
	 * `replaces`, cbs v0.1.99). Space-separated; "" for none. */
	char replaces[PKG_DEPENDS_MAX];
	/* cix#558: the aggregate memory this recipe's build declares it needs
	 * (CPDL `resources { memory }`, cbs v0.1.100), in bytes; 0 for none.
	 * Never part of what is built -- only of what the build is given. */
	long long build_memory;
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
	 * ADR-0323: the upstream block's parameters (cbs v0.1.102). tag is
	 * the template a release's tag is spelled by ("v{version}"); source
	 * the template of a release's main source url, which cbs checks
	 * expands to this recipe's own url, kept as written -- placeholders
	 * and all, because the next revision's url is written from it -- and
	 * substituted only where a copy reaches curl; verify the method.
	 * "" when absent.
	 */
	char upstream_tag[PKG_NAME_MAX];
	char upstream_source[PKG_URL_MAX];
	char upstream_verify[32];
	/* the signed rungs (ADR-0323): verify.format, its url template, and the
	 * pinned fingerprint of the key that signs it (ADR-0318) */
	char upstream_verify_format[32];
	char upstream_verify_url[PKG_URL_MAX];
	char upstream_verify_key[64];
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
 * " -- and <dir> has N MiB free, which is very likely the cause", or
 * "" when space is not short.
 *
 * #503: a full /var/lib/cix reported itself as three different
 * misleading messages and one silence. Two of the three are the
 * failures this decorates:
 *
 *   fetch failed (curl exit status 1)
 *   build failed (exit status 3)
 *
 * The first is a ~150 MB download into a partition with no room and
 * reads exactly like the documented flaky-mirror class -- a wrong
 * diagnosis was filed and committed on the strength of it before
 * anyone checked free space. The second came with a 65-byte log
 * holding one line written before the disk filled.
 *
 * Neither errno is reachable here: curl and cbs are child processes
 * and all that survives is an exit status. So rather than thread a
 * cause that does not exist, this ASKS THE FILESYSTEM at the moment
 * of failure. That is a correlation and is deliberately worded as
 * one -- "very likely the cause", never "the cause" -- because a
 * build can fail on its own merits on a host that also happens to
 * be low. A wrong certainty here would be the same error the
 * mirror diagnosis was.
 *
 * Silent when space is fine, so an ordinary failure message is
 * unchanged.
 */
#define PKG_DISK_PRESSURE_MIB 256

static void disk_pressure_note(char *out, size_t out_size)
{
	struct statvfs st;
	unsigned long long free_mib;

	out[0] = '\0';
	if (g_pkg_dir[0] == '\0' || statvfs(g_pkg_dir, &st) != 0)
		return;
	free_mib = ((unsigned long long)st.f_bavail * (unsigned long long)st.f_frsize) / (1024ULL * 1024ULL);
	if (free_mib >= PKG_DISK_PRESSURE_MIB)
		return;
	snprintf(out, out_size,
	         " -- and %s has %llu MiB free, which is very likely the cause", g_pkg_dir,
	         free_mib);
}

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
	/* #424: when the build and install stages began, 0 for a stage this
	 * run did not have; stages_known is 0 for a record loaded from before
	 * these were kept. */
	time_t build_started_at;
	time_t install_started_at;
	int stages_known;
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
	lim.memory_max = pkg_build_effective_memory_max(); /* cix#558: memory_max, or a raise */
	lim.cpu_max = pkg_build_get_cpu_max();
	/* Re-applied on every build so a runtime change to the configured
	 * budget takes effect rather than leaving a stale ceiling behind. */
	cgroup_create_parent(&lim);
}

/* cix#558: defined with the build config they read, further down. */
static int memory_budget_at_least(long long a, long long b);
static int build_memory_admit(long long need, char *msg, size_t msg_size);
static void pkg_build_memory_note_start(int chain_idx, long long need);
static void pkg_build_memory_note_end(int chain_idx);

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
	/*
	 * ADR-0307 clause 3: a cached .cixpkg has already been unpacked
	 * for this job, so pkg_prepare_build_and_start() takes the fast
	 * path when the unpack child's completion re-enters it.
	 *
	 * The same shape as buildenv_image above, which is what tells
	 * that function a composition it forked has finished. Without a
	 * flag the re-entry would simply start a second unpack, forever:
	 * the state that says "done" is the extracted tree, and asking
	 * the filesystem whether a directory looks populated is an
	 * inference, not a fact about what this job did.
	 *
	 * Cleared with the rest of the slot, so it cannot survive into
	 * the next job in the chain.
	 */
	int unpacked;
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
	char fetch_resolved_replaces[PKG_DEPENDS_MAX]; /* cix#553: the recipe's `replaces` */
	/*
	 * cix#558: the memory this job's build declares it needs (0 for
	 * none), and whether its build container is running right now.
	 * Together they are what pkg_build_effective_memory_max() raises
	 * the shared budget for, and when it may lower it again.
	 */
	long long fetch_resolved_build_memory;
	int build_running;
	/* #380: the recipe FILE start_fetch_for() resolved, read again by
	 * every stage after it -- see fetched_recipe_path(). The file, not
	 * a version to look up: a store directory is not always named by
	 * the recipe's own version string (a fixture dropped into the
	 * store before the daemon starts is not), so a lookup by that
	 * string can miss the very recipe the fetch read. */
	char fetch_resolved_recipe_path[PATH_MAX];
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
			 * ADR-0307 clause 3, same reasoning as fetch_pid: a
			 * slot that came back round would otherwise report the
			 * previous job's unpack as this one's, and skip the
			 * extraction this job needs.
			 *
			 * NOT THE ONLY CLEAR, and reading it as one cost #531:
			 * unpacked is per-PACKAGE, and a chain installs several
			 * in the same slot, so pkg_build_completed() clears it
			 * again on every chain advance. The sentence above is
			 * true of a reused slot and was quietly assumed to be
			 * the whole lifecycle; it is half of it.
			 *
			 * The other three fields here do not need that, and the
			 * distinction is the thing to carry away: fetch_pid and
			 * fetch_resolved_version/_depends are all set by
			 * start_fetch_for(), which every chain advance calls, so
			 * they refresh per package on their own. Only a field
			 * set by a COMPLETION and cleared on handout has this
			 * shape. If a future field does, clear it in both places.
			 */
			g_chains[i].unpacked = 0;
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
			g_chains[i].fetch_resolved_replaces[0] = '\0';
			g_chains[i].fetch_resolved_build_memory = 0;
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
 * True when a chain slot is already building or installing `package`,
 * for any image.
 *
 * The rolling drain starts one queued image per pass while any slot is
 * free (ADR-0157), and the only thing it held back was a second job for
 * the same IMAGE (#382). So publishing glibc@2.44-20, which queued six
 * images tracking it, started six identical glibc builds at once on
 * 192.168.15.95 (2026-09-30), each over an hour in the shared one-CPU
 * build cgroup. One build caches and publishes its artifact, after which
 * every other image installs it in seconds (#555). So an image whose next
 * package is already in flight elsewhere waits for it.
 */
static int package_job_in_flight(const char *package)
{
	int i;

	chain_reap_stale();
	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
		if (g_chains[i].name[0] != '\0' && strcmp(g_chains[i].name, package) == 0)
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
static void fetch_error_sidecar_path(int chain_idx, const char *name, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/.fetcherr-c%d-%s", g_sources_dir, chain_idx, name);
}

/*
 * Sibling of the error sidecar for something worth saying even when the
 * fetch SUCCEEDS -- currently an artifact that downloaded cleanly but
 * failed its checksum (#149). The error sidecar is only ever read on
 * failure, so a note about a fallback that then worked would be lost
 * there.
 */
static void fetch_note_sidecar_path(int chain_idx, const char *name, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/.fetchnote-c%d-%s", g_sources_dir, chain_idx, name);
}

/*
 * #501: EVERY file a fetch writes under g_sources_dir is keyed by the
 * build slot (chain_idx), not only by the package.
 *
 * These paths used to be name-version-index (sources, the artifact
 * download) or just the name (the two sidecars). Publishing a recipe
 * queues a rebuild of every image tracking it, so two builds of one
 * name@version run in different slots at once -- and each fetch starts
 * by unlink()ing its path and downloading into it. The second build's
 * unlink detached the first build's half-written file, so the first
 * then checksummed the second's partial download. Observed on
 * 192.168.15.95, 2026-09-21: kmod@34.2-7 for cix-kmod failed with 323584
 * bytes hashing wrong while cix-builder fetched the same url into the
 * same path in the same window. Downloading to a temporary name and
 * renaming would not have been enough: a later rename could still
 * replace bytes one build had verified with bytes it had not, between
 * its check and its staging. A slot runs one package at a time, so the
 * slot is the unit that makes every one of these files private.
 *
 * The slot goes FIRST ("c3-..."), so sources_clear_slot() can match a
 * slot by prefix -- "c1-" cannot match "c10-".
 */
static void source_download_path(int chain_idx, const char *name, const char *version, int index,
                                 char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/c%d-%s-%s-%d.src", g_sources_dir, chain_idx, name, version,
	         index);
}

/* "c<digits>-", the slot marker every name above starts with (after the
 * sidecar or artifact prefix, where there is one). */
static int slot_marked(const char *s, int *out_slot)
{
	int slot = 0;
	const char *p = s;

	if (*p++ != 'c' || *p < '0' || *p > '9')
		return 0;
	while (*p >= '0' && *p <= '9')
		slot = slot * 10 + (*p++ - '0');
	if (*p != '-')
		return 0;
	*out_slot = slot;
	return 1;
}

/*
 * Removes what a slot's previous fetch left in g_sources_dir, and any
 * file still named the pre-#501 way. Called by each fetch child before
 * it writes anything.
 *
 * A slot runs its packages one after another, and by the time it starts
 * the next fetch the previous package has been staged into its build
 * container (pkg_prepare_build_and_start()); a resume reuses that kept
 * container and never reads a source file. So nothing of this slot's is
 * still needed, and the directory stays bounded at one package per slot
 * -- where before #501 nothing ever removed a source at all, and every
 * name@version fetched stayed on disk.
 *
 * The pre-#501 names ("<name>-<version>-<i>.src", ".artifact-<name>-...",
 * ".fetcherr-<name>", ".fetchnote-<name>") are read by nothing once this
 * build runs, so they go too. ".artifact-push.status" is the push
 * worker's and is kept.
 */
static void sources_clear_slot(int chain_idx)
{
	static const char *const prefixes[] = { ".artifact-", ".fetcherr-", ".fetchnote-" };
	DIR *dir = opendir(g_sources_dir);
	struct dirent *de;

	if (dir == NULL)
		return;
	while ((de = readdir(dir)) != NULL) {
		const char *rest = de->d_name;
		size_t i, n = strlen(de->d_name);
		int slot;
		int ours = 0;
		char path[PATH_MAX];

		if (strcmp(de->d_name, ".artifact-push.status") == 0)
			continue;
		for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
			if (strncmp(de->d_name, prefixes[i], strlen(prefixes[i])) == 0) {
				rest = de->d_name + strlen(prefixes[i]);
				ours = 1;
				break;
			}
		}
		if (!ours && !(n > 4 && strcmp(de->d_name + n - 4, ".src") == 0))
			continue;
		/* Another slot's file, current naming: not this fetch's to touch. */
		if (slot_marked(rest, &slot) && slot != chain_idx)
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", g_sources_dir, de->d_name) >=
		    (int)sizeof(path))
			continue;
		unlink(path);
	}
	closedir(dir);
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

/*
 * ---- #553: a path in an image belongs to exactly one installed package
 *
 * Until 2026-09-30 two packages could install the same path, and the
 * LAST one installed was what was on disk, with nothing recording
 * whose bytes those were. Two consumers then read the path as if it
 * were the other package's:
 *
 *   - Build-environment composition copies each declared tool's
 *     recorded paths out of its image. coreutils 9.11-8 shipped
 *     usr/lib/libcap.so.2 as a link to its own libcap.so.2.66, the
 *     libcap package ships the same path as a link to .2.78, and in
 *     jumpbox the path held coreutils' link. An environment composed
 *     from libcap got that link without its target, and every build
 *     declaring coreutils failed on `libcap.so.2: cannot open shared
 *     object file` (measured on 192.168.15.95, 2026-09-30; #510).
 *   - Uninstall (#175, below) keeps a path another package still
 *     claims, but not the files that path's CONTENT depends on.
 *
 * So an install whose files include a path another installed package
 * in the same image owns is refused, and the error names both packages
 * and the path (path_owner_gate()). It is Debian's rule. A package's
 * own previous version is not "another package", so an ordinary
 * upgrade is unaffected. Moving a path between packages needs the
 * installing package to declare that it replaces the other one: CPDL's
 * `replaces { package "NAME" }` (cbs v0.1.99, cix-build-system#275). The
 * gate then lets exactly those packages' paths through, and once the
 * install is verified transfer_replaced_paths() takes them out of the
 * previous owner's manifest, so the path is still owned once.
 *
 * Scoped to one image because that is the unit a manifest describes:
 * the same package installed into two images owns two independent sets
 * of files, and one image's contents say nothing about another's.
 */

/* One path an installed package's manifest claims, and that package. */
struct path_claim {
	const char *rel;
	const struct pkg_entry *owner;
};

struct path_claim_index {
	struct path_claim *claims;
	size_t count;
};

static int path_claim_cmp(const void *a, const void *b)
{
	return strcmp(((const struct path_claim *)a)->rel, ((const struct path_claim *)b)->rel);
}

/*
 * Every path claimed by an INSTALLED package in `image` other than
 * `self`, sorted for bsearch(). Sorted once rather than scanned per
 * file, because a check is one lookup per staged file: gcc staged into
 * cix-builder would otherwise be thousands of files against every
 * file of every package there, on the daemon's one event loop.
 *
 * The strings are the entries' own; the index is only valid until the
 * package table next changes, so it is built and freed within one
 * call. Returns 0, or -1 with errno set on allocation failure.
 */
static int path_claim_index_build(const struct pkg_entry *self, const char *image,
                                  struct path_claim_index *out)
{
	size_t total = 0, n = 0;
	int i, f;

	out->claims = NULL;
	out->count = 0;
	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		const struct pkg_entry *o = &g_packages[i];

		if (o == self || !o->in_use || o->state != PKG_STATE_INSTALLED)
			continue;
		if (strcmp(normalize_image(o->image), image) != 0)
			continue;
		total += (size_t)o->file_count;
	}
	if (total == 0)
		return 0;
	out->claims = calloc(total, sizeof(*out->claims));
	if (out->claims == NULL)
		return -1;
	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		const struct pkg_entry *o = &g_packages[i];

		if (o == self || !o->in_use || o->state != PKG_STATE_INSTALLED)
			continue;
		if (strcmp(normalize_image(o->image), image) != 0)
			continue;
		for (f = 0; f < o->file_count && n < total; f++) {
			out->claims[n].rel = o->files[f];
			out->claims[n].owner = o;
			n++;
		}
	}
	out->count = n;
	qsort(out->claims, out->count, sizeof(*out->claims), path_claim_cmp);
	return 0;
}

static const struct pkg_entry *path_claim_owner(const struct path_claim_index *idx,
                                                const char *rel)
{
	struct path_claim key;
	const struct path_claim *hit;

	if (idx->count == 0)
		return NULL;
	key.rel = rel;
	key.owner = NULL;
	hit = bsearch(&key, idx->claims, idx->count, sizeof(*idx->claims), path_claim_cmp);
	return hit != NULL ? hit->owner : NULL;
}

static void path_claim_index_free(struct path_claim_index *idx)
{
	free(idx->claims);
	idx->claims = NULL;
	idx->count = 0;
}

/* Is name one of the space-separated names in list? "" matches nothing. */
static int name_in_list(const char *list, const char *name)
{
	size_t nlen = strlen(name);
	const char *p = list;

	while (p != NULL && *p != '\0') {
		const char *end = strchr(p, ' ');
		size_t len = end != NULL ? (size_t)(end - p) : strlen(p);

		if (len == nlen && strncmp(p, name, nlen) == 0)
			return 1;
		p = end != NULL ? end + 1 : NULL;
	}
	return 0;
}

/*
 * Walks a staged install tree and stops at the first regular file or
 * symlink another package owns -- the two kinds merge_tree() records in
 * a manifest, so the two kinds that can be owned. Returns 1 with the
 * refusal in err, 0 when nothing collides, -1 with err set when the
 * tree could not be read.
 */
static int path_owner_walk(const struct pkg_entry *self, const char *image,
                           const struct path_claim_index *idx, const char *root,
                           const char *relpath, const char *replaces, char *err,
                           size_t err_size)
{
	char dir[PATH_MAX];
	DIR *d;
	struct dirent *de;
	int r = 0;

	snprintf(dir, sizeof(dir), "%s%s%s", root, relpath[0] ? "/" : "", relpath);
	d = opendir(dir);
	if (d == NULL) {
		/* No tree at all is a package that installs nothing, which
		 * merge_tree() accepts too. */
		if (relpath[0] == '\0' && errno == ENOENT)
			return 0;
		snprintf(err, err_size, "could not read the staged tree at \"%s\" to check who owns "
		         "its paths: %s", relpath[0] ? relpath : "/", strerror(errno));
		return -1;
	}
	while (r == 0 && (de = readdir(d)) != NULL) {
		char rel[PATH_MAX];
		char path[PATH_MAX];
		struct stat st;
		const struct pkg_entry *owner;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(rel, sizeof(rel), "%s%s%s", relpath, relpath[0] ? "/" : "", de->d_name);
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (lstat(path, &st) != 0) {
			snprintf(err, err_size, "could not read \"%s\" in the staged tree to check who "
			         "owns it: %s", rel, strerror(errno));
			r = -1;
		} else if (S_ISDIR(st.st_mode)) {
			r = path_owner_walk(self, image, idx, root, rel, replaces, err, err_size);
		} else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
			owner = path_claim_owner(idx, rel);
			/* A package the recipe declares it replaces (cbs#275) may
			 * hand over its paths; nothing else may. */
			if (owner != NULL && !name_in_list(replaces, owner->name)) {
				snprintf(err, err_size,
				         "%s@%s installs \"%s\", which %s@%s already owns in image \"%s\" "
				         "-- a path belongs to one package (#553): drop it from one of the "
				         "two recipes, or, if the file is moving, declare "
				         "replaces { package \"%s\" } in this one",
				         self->name, self->version, rel, owner->name,
				         owner->version, image, owner->name);
				r = 1;
			}
		}
	}
	closedir(d);
	return r;
}

/*
 * #553's refusal, run before a single byte is staged, beside the
 * #389 undeclared-link gate and for the same reason: the message names
 * this package, and a refused install leaves nothing behind. Returns 0
 * to proceed, nonzero with the refusal in err.
 */
static int path_owner_gate(const struct pkg_entry *self, const char *image, const char *dest_dir,
                           const char *replaces, char *err, size_t err_size)
{
	struct path_claim_index idx;
	int r;

	if (path_claim_index_build(self, normalize_image(image), &idx) != 0) {
		snprintf(err, err_size, "could not index the paths image \"%s\" already owns: %s",
		         normalize_image(image), strerror(errno));
		return -1;
	}
	r = path_owner_walk(self, normalize_image(image), &idx, dest_dir, "", replaces, err,
	                    err_size);
	path_claim_index_free(&idx);
	return r;
}


/*
 * Removes the files a package's manifest claims -- except any that
 * another installed package in the same image also claims, which are
 * left alone and reported (issue #175).
 *
 * Since #553 no install creates such a path, but images installed
 * before that rule still hold some, and deleting one breaks a package
 * nobody touched. That really happened: uninstalling libc-dev silently
 * gutted linux-headers' share of usr/include, and it surfaced later,
 * elsewhere, as
 *
 *   build environment: copying usr/include/asm-generic/resource.h from
 *   declared tool linux-headers@6.18.40-4 (image cix-builder) failed:
 *   No such file or directory
 *
 * If the index cannot be built nothing is removed, for the same
 * reason: keeping a file costs disk, deleting a shared one breaks a
 * package somewhere else.
 *
 * It does not touch e->files: the caller decides when to forget the
 * list, e.g. only once a replacement build has succeeded for an
 * in-place upgrade. Shared by pkg_delete() and the upgrade path, both
 * through image_produce_new_version()'s mutate() callback, so rootfs is
 * always a scratch copy-forward staging directory (ADR-0107/0108),
 * never a version's own immutable rootfs.
 */
static void unlink_manifest_files(const struct pkg_entry *e, const char *rootfs)
{
	struct path_claim_index idx;
	int i, kept = 0;

	if (path_claim_index_build(e, normalize_image(e->image), &idx) != 0) {
		logstore_write("cixd", "error",
		               "pkg %s@%s: could not index the paths other packages in this image "
		               "own (%s), so none of its %d file(s) were removed",
		               e->name, normalize_image(e->image), strerror(errno), e->file_count);
		return;
	}
	for (i = 0; i < e->file_count; i++) {
		char path[PATH_MAX];

		if (path_claim_owner(&idx, e->files[i]) != NULL) {
			kept++;
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", rootfs, e->files[i]);
		unlink(path);
	}
	path_claim_index_free(&idx);
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
 *
 * WHAT THIS DOES NOT DO, because the paragraph above reads as though
 * it does and #502 was filed from believing it. "The recipes that
 * already exist stop being served" is true only of recipes carrying
 * THIS token. One carrying `osakka:<password>@` basic auth holds a
 * different secret, matches nothing here, and was served in full.
 * Shape-based redaction is redact_url_userinfo() below, and the
 * serving and logging paths run both.
 */
static void redact_repo_token(char *buf, size_t cap)
{
	int i;

	if (buf == NULL)
		return;
	/* ADR-0324: every source's token, not one -- a recipe or an error
	 * message may carry any of them. */
	for (i = 0; i < pkgsource_count(); i++) {
		const char *tok = pkgsource_at(i)->token;

		if (tok[0] != '\0')
			str_replace_all(buf, cap, tok, "{{REPO_TOKEN}}");
	}
}

/*
 * Every URL in `buf` that carries userinfo -- a user and a secret
 * before the `@` of a host -- loses it.
 *
 * (Described rather than spelled, because test_secrets scans this
 * tree for exactly that shape and cannot tell an example in a
 * comment from a live credential. A scanner that tried to would be
 * one that misses real ones, so the comment adapts to the gate
 * rather than the other way round -- the same wording test_secrets
 * itself uses for the same reason.)
 *
 * THE DIFFERENCE FROM redact_repo_token() IS THE WHOLE POINT, and
 * #502 is what happens without it. That function substitutes exactly
 * the tokens of this host's sources -- so it protects recipes carrying
 * one of THOSE and nothing else. A recipe carrying
 * `https://osakka:<password>@git.home.arpa/...` uses basic auth, a
 * different secret from the configured API token, so there was
 * nothing to match and `pkg recipe show` served the credential in
 * full. It then reached git, in a repository whose whole point is
 * being readable.
 *
 * A credential is recognisable by SHAPE, not by equality with a
 * string the daemon happens to know. Redacting the userinfo
 * component covers every case including ones this host has never
 * held -- which is the property that makes it a guarantee rather
 * than a list.
 *
 * USERINFO THAT IS ALREADY A PLACEHOLDER IS LEFT ALONE. When the
 * serving path runs redact_repo_token() first, a known token becomes
 * `{{REPO_TOKEN}}`, which is not a secret, is exactly what the git
 * corpus carries, and is more useful to a reader than `REDACTED`.
 * Blanking it too would make the endpoint's output differ from the
 * recipe's canonical form for no gain.
 *
 * Not used on the WRITE path, where redact_repo_token()'s
 * substitution is functional rather than protective: it normalises a
 * live token to the placeholder that substitute_repo_token() expands
 * again at fetch time. Replacing an arbitrary credential with
 * `REDACTED` there would store a URL that cannot fetch, turning a
 * disclosure into a broken package.
 */
static void redact_url_userinfo(char *buf, size_t cap)
{
	static const char marker[] = "REDACTED";
	const size_t marker_len = sizeof(marker) - 1;
	char *p;

	if (buf == NULL || cap == 0)
		return;
	p = buf;
	while ((p = strstr(p, "://")) != NULL) {
		char *authority = p + 3;
		char *at = NULL;
		char *q;
		size_t span;
		size_t len;

		/* The authority ends at the first delimiter; an '@' after
		 * one belongs to a path or query and is not userinfo. */
		for (q = authority; *q != '\0'; q++) {
			if (*q == '/' || *q == '?' || *q == '#' || *q == ' ' || *q == '\t' ||
			    *q == '\n' || *q == '"' || *q == '\'')
				break;
			if (*q == '@')
				at = q;
		}
		if (at == NULL) {
			p = authority;
			continue;
		}
		span = (size_t)(at - authority);
		/* Already a placeholder -- see the comment above. */
		if (memmem(authority, span, "{{", 2) != NULL) {
			p = at;
			continue;
		}
		len = strlen(buf);
		if (len - span + marker_len + 1 > cap) {
			/* Cannot fit the marker. Leave this one rather than
			 * truncate the document, and keep scanning: a buffer
			 * this tight is not a reason to stop protecting the
			 * rest of it. */
			p = at;
			continue;
		}
		memmove(authority + marker_len, at, len - (size_t)(at - buf) + 1);
		memcpy(authority, marker, marker_len);
		p = authority + marker_len;
	}
}

/* The token of the source that owns `name` (ADR-0324), or NULL. */
static const char *owning_token(const char *name)
{
	char item[PKG_SOURCE_ITEM_MAX];
	char owner[PKG_SOURCES_MAX * PKG_SOURCE_NAME_MAX];
	const struct pkg_source *s;

	snprintf(item, sizeof(item), "package:%s", name);
	if (pkgsource_owner_of(item, owner, sizeof(owner)) != PKGSOURCE_OWNER_ONE)
		return NULL;
	s = pkgsource_find(owner);
	return s != NULL && s->token[0] != '\0' ? s->token : NULL;
}

/*
 * Issue #60/#405: substitutes the token of the source that owns this
 * package (ADR-0324) for the {{REPO_TOKEN}} placeholder in every source
 * URL. The owner's, never another source's: a recipe from one source
 * must not be able to spend a token this host holds for a different
 * forge.
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
 * A recipe with no placeholder, no owning source, or an owner with no
 * token, is left byte-for-byte unchanged.
 */
static void substitute_repo_token(struct pkg_recipe *out)
{
	const char *tok = owning_token(out->name);
	int i;

	if (tok == NULL)
		return;
	for (i = 0; i < out->source_count; i++)
		str_replace_all(out->source[i], sizeof(out->source[i]), "{{REPO_TOKEN}}", tok);
	/* Mirrors are urls like any other and reach curl the same way, so
	 * they get the same substitution -- a placeholder left in a mirror
	 * would fail only on the fallback path, which is the one nobody
	 * exercises until the day it matters (#507). */
	for (i = 0; i < out->mirror_count; i++)
		str_replace_all(out->mirror_url[i], sizeof(out->mirror_url[i]), "{{REPO_TOKEN}}",
		                tok);
	/* The upstream source template is NOT substituted here: it is what
	 * the author stage writes the next revision's url from, and that
	 * url keeps the placeholder (ADR-0323). Where it reaches curl, the
	 * caller substitutes a copy with owning_token(). */
}

/*
 * Runs one host-side cbs verb -- `explain --json`, or `revise` (ADR-0323)
 * -- and returns its stdout (ADR-0305). argv is the whole vector, argv[0]
 * included; verb names it in errors.
 *
 * `explain` is the ONLY place this daemon learns what a CBS recipe
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
static int run_cbs(char *const argv[], const char *verb, char *out, size_t out_size,
                   size_t *out_len, char *err, size_t err_size)
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
			snprintf(err, err_size, "cbs %s produced more than %zu bytes of output", verb,
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
				snprintf(err, err_size, "cbs %s was killed by a signal", verb);
			else if (WEXITSTATUS(status) == 127)
				snprintf(err, err_size, "cbs %s could not be executed (%s)", verb,
				         PKG_CBS_BIN);
			else if (dtotal > 0)
				snprintf(err, err_size, "%s", diag);
			else
				snprintf(err, err_size, "cbs %s exited %d with no diagnostic", verb,
				         WEXITSTATUS(status));
			out[0] = '\0';
			return -1;
		}
	}

	if (out_len != NULL)
		*out_len = total;
	return 0;
}

/* `cbs explain RECIPE --json`: what the recipe declares (ADR-0305). */
static int run_cbs_explain(const char *recipe_path, char *out, size_t out_size, size_t *out_len,
                           char *err, size_t err_size)
{
	char *argv[] = { (char *)"cbs", (char *)"explain", (char *)recipe_path, (char *)"--json",
		         NULL };

	return run_cbs(argv, "explain", out, out_size, out_len, err, err_size);
}

/*
 * Is this path a CBS recipe (ADR-0305)?
 *
 * Not a heuristic: the filename IS the format, so this reads the one
 * source of truth rather than guessing from content. `.cbs` is CBS's
 * own requirement and not our preference -- has_cbs_extension()
 * refuses any other path, in both `cbs explain` and `cbs build`.
 */
static int recipe_path_is_cbs(const char *path)
{
	size_t len;

	if (path == NULL)
		return 0;
	len = strlen(path);
	if (len < sizeof(PKG_RECIPE_CBS_SUFFIX) - 1)
		return 0;
	return strcmp(path + len - (sizeof(PKG_RECIPE_CBS_SUFFIX) - 1), PKG_RECIPE_CBS_SUFFIX) == 0;
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
static void cbs_explain_path(const char *recipe_path, char *out, size_t out_size)
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
 * Maps a CBS recipe's declarations onto this daemon's own fields, out
 * of the explain.json written at publish (ADR-0305).
 *
 * Every field a shell recipe can declare, a CBS recipe now can too.
 * Three of them could not until cix-build-system#161 and #162 closed,
 * and this comment said so; it is corrected rather than deleted because
 * the reason they were empty is worth keeping:
 *
 *   build_caps       came back as a COUNT, and a count of 1 does not
 *                    say whether the recipe asked for CAP_SYS_ADMIN or
 *                    CAP_NET_ADMIN, so a non-zero count was refused at
 *                    publish rather than dropped here. Now read by
 *                    name. An older cbs still answering with a count
 *                    is still refused -- see the publish path.
 *   artifact_sha256  had nowhere to live, because CPDL rejects unknown
 *                    package keys, so a converted recipe rebuilt from
 *                    source on every install on every host. Now in the
 *                    opaque `metadata { }` block.
 *   changelog        same gap, same block.
 *
 * Verified rather than assumed: cbs 0.1.25-6 is built from
 * cix-build-system main at 170dc744d, and its own cli-contract-test.sh
 * -- which asserts both `"capabilities":["CAP_ONE","CAP_TWO"]` and
 * `"metadata":{"artifact_sha256":...}` out of explain --json -- passes
 * in a Cix build container on 192.168.15.95.
 *
 * `bootstrap` tools are folded into build_depends alongside `build`
 * ones because CBS itself treats them as build-time: its
 * role_selected() applies both roles to every phase except install.
 * A name appearing in both roles is harmless -- buildenv_add_tool()
 * dedups by name, and a recipe's own pin still wins the slot.
 */
static int parse_cbs_recipe(const char *path, struct pkg_recipe *out)
{
	char explain_path[PATH_MAX];
	char err[256];
	char *buf = NULL;
	size_t len = 0;
	struct cbs_explain *ex;
	int count;
	int i;

	cbs_explain_path(path, explain_path, sizeof(explain_path));
	if (persist_read_file(explain_path, &buf, &len) != 0 || buf == NULL) {
		logstore_write("cixd", "error",
		               "pkg: %s has no derived identity beside it (%s) -- it was not "
		               "published by this daemon",
		               path, explain_path);
		return -1;
	}
	ex = cbs_explain_parse(buf, len, err, sizeof(err));
	free(buf);
	if (ex == NULL) {
		logstore_write("cixd", "error", "pkg: %s: %s", explain_path, err);
		return -1;
	}

	snprintf(out->name, sizeof(out->name), "%s", cbs_explain_name(ex));
	if (cbs_explain_version(ex, out->version, sizeof(out->version)) != 0) {
		logstore_write("cixd", "error", "pkg: %s: version and release do not fit a version string",
		               explain_path);
		cbs_explain_free(ex);
		return -1;
	}
	/* The same two fields unjoined, for anything that must hand them
	 * over separately (cix#528). Read from the document, never split
	 * back out of out->version -- see struct pkg_recipe. */
	if (cbs_explain_version_parts(ex, out->bare_version, sizeof(out->bare_version),
	                               &out->release) != 0) {
		logstore_write("cixd", "error", "pkg: %s: version does not fit on its own",
		               explain_path);
		cbs_explain_free(ex);
		return -1;
	}

	count = cbs_explain_source_count(ex);
	if (count <= 0 || count > PKG_MAX_SOURCES) {
		logstore_write("cixd", "error", "pkg: %s: %d sources, but 1..%d is supported",
		               explain_path, count, PKG_MAX_SOURCES);
		cbs_explain_free(ex);
		return -1;
	}
	out->mirror_count = 0;
	for (i = 0; i < count; i++) {
		int urls;
		int u;

		if (cbs_explain_source(ex, i, out->source[i], PKG_URL_MAX, out->sha256[i],
		                        PKG_SHA256_MAX) != 0) {
			logstore_write("cixd", "error", "pkg: %s: source %d has no usable url/sha256 pair",
			               explain_path, i);
			cbs_explain_free(ex);
			return -1;
		}
		/*
		 * Every url past the first is a mirror for this same source
		 * and this same checksum (#507). Recorded here rather than
		 * discarded, so the fetch can fall back through them; a
		 * recipe that declares one url adds nothing.
		 *
		 * Refused rather than truncated when the pool is full: a
		 * silently dropped mirror is the exact failure this change
		 * exists to remove, and a recipe author who declared one has
		 * to be able to believe it is there.
		 */
		urls = cbs_explain_source_url_count(ex, i);
		for (u = 1; u < urls; u++) {
			if (out->mirror_count >= PKG_MAX_MIRRORS) {
				logstore_write("cixd", "error",
				               "pkg: %s: more than %d mirror urls across all sources",
				               explain_path, PKG_MAX_MIRRORS);
				cbs_explain_free(ex);
				return -1;
			}
			if (cbs_explain_source_url(ex, i, u, out->mirror_url[out->mirror_count],
			                            PKG_URL_MAX) != 0) {
				logstore_write("cixd", "error",
				               "pkg: %s: source %d url %d is not a usable string",
				               explain_path, i, u);
				cbs_explain_free(ex);
				return -1;
			}
			out->mirror_source[out->mirror_count] = i;
			out->mirror_count++;
		}
	}
	out->source_count = count;

	if (cbs_explain_requires(ex, "runtime", "package", out->depends, sizeof(out->depends)) != 0) {
		logstore_write("cixd", "error", "pkg: %s: the runtime package list does not fit",
		               explain_path);
		cbs_explain_free(ex);
		return -1;
	}
	if (cbs_explain_replaces(ex, out->replaces, sizeof(out->replaces)) != 0) {
		logstore_write("cixd", "error", "pkg: %s: the replaces list does not fit",
		               explain_path);
		cbs_explain_free(ex);
		return -1;
	}
	if (cbs_explain_resources_memory(ex, &out->build_memory) != 0) {
		logstore_write("cixd", "error",
		               "pkg: %s: resources.memory is not a positive whole number of bytes",
		               explain_path);
		cbs_explain_free(ex);
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

				if (cbs_explain_requires(ex, roles[ri], kinds[ki], one, sizeof(one)) != 0 ||
				    append_words(out->build_depends, sizeof(out->build_depends), one) != 0) {
					logstore_write("cixd", "error",
					               "pkg: %s: the %s/%s list does not fit the build "
					               "declaration",
					               explain_path, roles[ri], kinds[ki]);
					cbs_explain_free(ex);
					return -1;
				}
			}
		}
	}

	snprintf(out->upstream, sizeof(out->upstream), "%s", cbs_explain_upstream(ex));
	snprintf(out->upstream_tag, sizeof(out->upstream_tag), "%s",
	         cbs_explain_upstream_param(ex, "tag"));
	snprintf(out->upstream_source, sizeof(out->upstream_source), "%s",
	         cbs_explain_upstream_param(ex, "source"));
	snprintf(out->upstream_verify, sizeof(out->upstream_verify), "%s",
	         cbs_explain_upstream_verify(ex, "method"));
	snprintf(out->upstream_verify_format, sizeof(out->upstream_verify_format), "%s",
	         cbs_explain_upstream_verify(ex, "format"));
	snprintf(out->upstream_verify_url, sizeof(out->upstream_verify_url), "%s",
	         cbs_explain_upstream_verify(ex, "url"));
	snprintf(out->upstream_verify_key, sizeof(out->upstream_verify_key), "%s",
	         cbs_explain_upstream_verify(ex, "key"));

	/*
	 * Capabilities by name. A return of 1 is the installed cbs still
	 * answering with a count: refused here as well as at publish,
	 * because a recipe published against a newer cbs can be BUILT
	 * against an older one -- cixd execs whichever is on the host, and
	 * a downgrade must not quietly drop what the recipe declares.
	 */
	{
		int caps = cbs_explain_capabilities(ex, out->build_caps, sizeof(out->build_caps));

		if (caps != 0) {
			logstore_write("cixd", "error",
			               "pkg: %s: %s -- refusing rather than building without the "
			               "capabilities this recipe declares (cix-build-system#162)",
			               explain_path,
			               caps == 1 ? "the installed cbs reports a capability COUNT "
			                            "rather than names; upgrade cbs"
			                          : "the declared capabilities do not fit");
			cbs_explain_free(ex);
			return -1;
		}
	}

	/*
	 * The declared artifact format (ADR-0307 clause 1). Read, never
	 * assumed: CPDL accepts `format "tar.gz"` and `cbs build` refuses
	 * to execute it, so the daemon has to be able to see the
	 * declaration in order to refuse it at publish.
	 *
	 * An empty value is an engine older than v0.1.26, whose explain
	 * has no `format` key at all. Clause 7's startup sweep re-derives
	 * exactly those documents, so reaching here with "" means the
	 * sweep could not run -- refused rather than defaulted, because a
	 * default here is the assumption clause 1 exists to prevent.
	 */
	{
		const char *fmt = cbs_explain_format(ex);

		if (fmt[0] == '\0') {
			logstore_write("cixd", "error",
			               "pkg: %s declares no artifact format -- it was derived by a cbs "
			               "older than v0.1.26, and re-deriving it is what ADR-0307 clause 7 "
			               "does at startup; this daemon could not",
			               explain_path);
			cbs_explain_free(ex);
			return -1;
		}
		/* cixpkg and nothing else: a recipe declaring another format is
		 * refused at publish, and no other is installed (cix#569). */
		if (strcmp(fmt, PKG_ARTIFACT_FORMAT_CIXPKG) != 0) {
			logstore_write("cixd", "error",
			               "pkg: %s declares artifact format \"%s\"; only cixpkg is built "
			               "or installed (cix#569)",
			               explain_path, fmt);
			cbs_explain_free(ex);
			return -1;
		}
		snprintf(out->artifact_format, sizeof(out->artifact_format), "%s", fmt);
	}

	if (cbs_explain_metadata(ex, "artifact_sha256", out->artifact_sha256,
	                          sizeof(out->artifact_sha256)) != 0 ||
	    cbs_explain_metadata(ex, "changelog", out->changelog, sizeof(out->changelog)) != 0) {
		/* The same limits as the publish path's own message (#493).
		 * Unreachable in practice, since publish refuses such a
		 * recipe before it is ever stored -- but this reads a
		 * document off disk, and "cannot happen" is a claim about
		 * what put it there. */
		logstore_write("cixd", "error",
		               "pkg: %s: a metadata value does not fit (artifact_sha256 max %d "
		               "bytes, changelog max %d)",
		               explain_path, PKG_SHA256_MAX - 1, PKG_CHANGELOG_MAX - 1);
		cbs_explain_free(ex);
		return -1;
	}

	cbs_explain_free(ex);
	return 0;
}

/*
 * One recipe: a CPDL file, read through `cbs explain` (ADR-0305).
 *
 * A path that is not a .cbs file is not a recipe. Shell recipes must
 * not exist (the owner, 2026-10-03, cix#569): there is no second
 * reader, so nothing in this file can resolve, build, install or
 * publish one, and every caller that resolves a path gets -1 for it.
 */
static int parse_recipe(const char *path, struct pkg_recipe *out)
{
	if (!recipe_path_is_cbs(path))
		return -1;
	memset(out, 0, sizeof(*out));
	if (parse_cbs_recipe(path, out) != 0)
		return -1;
	substitute_repo_token(out);
	if (!pkg_name_is_valid(out->name))
		return -1;
	return 0;
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
 * The recipe file inside one version directory: its build.cbs
 * (ADR-0305). Fills out_path and, when out_created is non-NULL, the
 * file's mtime -- which ADR-0107 immutability makes a real "first
 * published" timestamp (see recipe_created_at()).
 *
 * Returns -1 when the directory holds no build.cbs. A build.sh is not a
 * recipe: shell recipes do not exist (cix#569), and a version
 * directory that holds only one is left over from before that and is
 * removed at startup (retire_shell_recipes()).
 */
int pkg_recipe_file_in(const char *version_dir, char *out_path, size_t out_path_size,
                        long *out_created, const char **out_filename)
{
	char cbs_path[PATH_MAX];
	struct stat cbs_st;

	snprintf(cbs_path, sizeof(cbs_path), "%s/%s", version_dir, PKG_RECIPE_CBS_FILE);
	if (stat(cbs_path, &cbs_st) != 0 || !S_ISREG(cbs_st.st_mode))
		return -1;
	snprintf(out_path, out_path_size, "%s", cbs_path);
	if (out_created != NULL)
		*out_created = (long)cbs_st.st_mtime;
	if (out_filename != NULL)
		*out_filename = PKG_RECIPE_CBS_FILE;
	return 0;
}

/*
 * Resolves name (and optional specific version) to the path of its
 * recipe under ADR-0107's version-keyed layout:
 * <g_recipes_dir>/<name>/<version>/build.cbs (ADR-0305 --
 * pkg_recipe_file_in() above is the one place that reads it).
 * version NULL or ""
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

/* ---- ADR-0323: refreshing the gitea-tags kind's listings ----
 *
 * One package at a time, each from its own repository: the newest
 * recipe's upstream source template (srcgitea.h) names the repository,
 * its owning source's token reaches the listing the same way it reaches
 * every url (substitute_repo_token()), and what is found -- or why
 * nothing could be -- is written to that package's cache file, which
 * the catalogue reads. Runs in a helper process (ADR-0278): it is
 * network I/O, one bounded request per package.
 */
static int g_upstream_refresh_running;

/* Defined with the recipe-version cache, further down. */
static int recipe_latest_version(const char *name, char *out_version, size_t out_size);

int pkg_upstream_refresh_begin(char *err, size_t err_size)
{
	if (g_upstream_refresh_running) {
		snprintf(err, err_size, "a discovery run is already in progress");
		return -1;
	}
	g_upstream_refresh_running = 1;
	return 0;
}

/* `url` with the owning source's token in place of {{REPO_TOKEN}}: a copy
 * for curl, never stored. 0, or -1 when a placeholder has no token. */
static int url_with_token(const char *name, const char *url, char *out, size_t out_size)
{
	const char *tok = owning_token(name);

	snprintf(out, out_size, "%s", url);
	if (tok != NULL)
		str_replace_all(out, out_size, "{{REPO_TOKEN}}", tok);
	return strstr(out, "{{REPO_TOKEN}}") != NULL ? -1 : 0;
}

static void store_note(const char *name, const char *version, const char *stage,
                       const char *status, const char *reason)
{
	struct srcrecord_note note;

	memset(&note, 0, sizeof(note));
	snprintf(note.version, sizeof(note.version), "%s", version);
	snprintf(note.stage, sizeof(note.stage), "%s", stage);
	snprintf(note.status, sizeof(note.status), "%s", status);
	snprintf(note.reason, sizeof(note.reason), "%s", reason);
	srcrecord_store_note(name, &note);
}

/* Reads one package's listing; 0, or -1 with the cache's error written. */
static int refresh_one_gitea(const char *name, const struct pkg_recipe *recipe, long now)
{
	char tags[PKG_URL_MAX], url[PKG_URL_MAX], tmp[PATH_MAX], err[PKG_ERROR_MAX];
	struct curlfetch_opts opts;
	char *buf = NULL;
	size_t len = 0;
	int rc;

	if (recipe->upstream_source[0] == '\0') {
		srcgitea_store_error(name,
		                     "the recipe declares no upstream source template, so its "
		                     "repository is not known -- declare `source` as the release "
		                     "archive url with {version} in it",
		                     now);
		return -1;
	}
	if (srcgitea_tags_url(recipe->upstream_source, tags, sizeof(tags)) != 0) {
		srcgitea_store_error(name,
		                     "its upstream source is not a Gitea API archive url "
		                     "(<base>/api/v1/repos/<owner>/<repo>/archive/<tag>.tar.gz)",
		                     now);
		return -1;
	}
	if (url_with_token(name, tags, url, sizeof(url)) != 0) {
		srcgitea_store_error(name,
		                     "its source asks for {{REPO_TOKEN}} and the source that owns "
		                     "this package has no token",
		                     now);
		return -1;
	}
	snprintf(tmp, sizeof(tmp), "%s/upstream/.fetch-%s.json", g_pkg_dir, name);
	memset(&opts, 0, sizeof(opts));
	opts.url = url;
	opts.path = tmp;
	opts.connect_timeout = 10;
	opts.max_time = 30;
	if (curlfetch_perform(&opts, NULL, err, sizeof(err)) != 0) {
		char why[PKG_ERROR_MAX + 64];

		unlink(tmp);
		redact_repo_token(err, sizeof(err));
		snprintf(why, sizeof(why), "the tags listing could not be fetched: %s", err);
		srcgitea_store_error(name, why, now);
		return -1;
	}
	if (persist_read_file(tmp, &buf, &len) != 0 || buf == NULL) {
		unlink(tmp);
		srcgitea_store_error(name, "the tags listing was fetched but could not be read", now);
		return -1;
	}
	unlink(tmp);
	rc = srcgitea_store_listing(name, buf, len,
	                            recipe->upstream_tag[0] != '\0' ? recipe->upstream_tag
	                                                            : "{version}",
	                            now);
	free(buf);
	if (rc != 0) {
		srcgitea_store_error(name, "the tags listing was not a Gitea tags array", now);
		return -1;
	}
	return 0;
}

/* Fetches `url` to `path` with the bounds every discovery fetch uses.
 * 0, or -1 with curl's reason in err (any token redacted). */
static int discover_fetch(const char *url, const char *path, char *err, size_t err_size)
{
	struct curlfetch_opts opts;

	memset(&opts, 0, sizeof(opts));
	opts.url = url;
	opts.path = path;
	opts.connect_timeout = 10;
	opts.max_time = 600;
	opts.low_speed_limit = 1024;
	opts.low_speed_time = 60;
	if (curlfetch_perform(&opts, NULL, err, err_size) == 0)
		return 0;
	unlink(path);
	redact_repo_token(err, err_size);
	return -1;
}

static void utc_stamp(long now, char *out, size_t out_size)
{
	time_t t = (time_t)now;
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/*
 * Rung 4, origin trust: the release archive fetched over TLS from an
 * origin the operator trusts, its sha256 recorded at discovery.
 */
static void authenticate_by_origin(const char *name, const struct pkg_recipe *recipe,
                                   const char *version, long now)
{
	char origin[SRCTRUST_ORIGIN_MAX], raw[PKG_URL_MAX], url[PKG_URL_MAX];
	char tmp[PATH_MAX], err[PKG_ERROR_MAX], why[512], stamp[32];
	struct srcrecord_candidate c;

	if (srctrust_origin_of(recipe->upstream_source, origin, sizeof(origin)) != 0 ||
	    !srctrust_trusts(recipe->upstream_source)) {
		snprintf(why, sizeof(why),
		         "origin %s is not trusted on this host -- an operator adds it with `cixctl "
		         "pkg trusted-origins add` (ADR-0323)",
		         origin[0] != '\0' ? origin : "(none)");
		store_note(name, version, "authenticate", "blocked", why);
		return;
	}
	if (srcupstream_expand(recipe->upstream_source, version, raw, sizeof(raw)) != 0 ||
	    url_with_token(name, raw, url, sizeof(url)) != 0) {
		store_note(name, version, "authenticate", "failed",
		           "the release url could not be formed from the source template");
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s/upstream/.archive-%s", g_pkg_dir, name);
	memset(&c, 0, sizeof(c));
	if (discover_fetch(url, tmp, err, sizeof(err)) != 0 ||
	    pkg_run_capture_sha256(tmp, c.sha256, sizeof(c.sha256)) != 0) {
		unlink(tmp);
		snprintf(why, sizeof(why), "the release archive could not be fetched from %s: %s",
		         origin, err);
		store_note(name, version, "authenticate", "failed", why);
		return;
	}
	unlink(tmp);
	utc_stamp(now, stamp, sizeof(stamp));
	snprintf(c.version, sizeof(c.version), "%s", version);
	snprintf(c.url, sizeof(c.url), "%s", raw);
	snprintf(c.verification, sizeof(c.verification),
	         "origin trust: the release archive fetched from trusted origin %s at %s", origin,
	         stamp);
	srcrecord_store_candidate(name, &c);
	snprintf(why, sizeof(why), "sha256 %s, by origin trust from %s; waiting for the author stage",
	         c.sha256, origin);
	store_note(name, version, "authenticate", "ok", why);
}

/*
 * Rung 2, a signed checksum list (kernel.org's sha256sums.asc): the
 * list is fetched, verified against the key the recipe names -- which
 * must be installed for THIS package (ADR-0318) -- and the sha256 of
 * the release archive is read out of the verified text. The archive
 * itself is not fetched here: the build fetches it and refuses bytes
 * that do not hash to what the signed list says.
 */
static void authenticate_by_checksums(const char *name, const struct pkg_recipe *recipe,
                                      const char *version, long now)
{
	char fpr[41], raw[PKG_URL_MAX], url[PKG_URL_MAX], src[PKG_URL_MAX], file[256];
	char tmp[PATH_MAX], err[PKG_ERROR_MAX], why[768], stamp[32];
	const char *key, *leaf;
	char *doc = NULL, *text = NULL;
	size_t key_len = 0, doc_len = 0, file_len;
	enum pgp_verify_result vr;
	struct srcrecord_candidate c;

	if (strcmp(recipe->upstream_verify_format, "openpgp-clearsigned") != 0) {
		snprintf(why, sizeof(why),
		         "the checksum list is declared as \"%s\"; this platform verifies "
		         "\"openpgp-clearsigned\" lists",
		         recipe->upstream_verify_format);
		store_note(name, version, "authenticate", "blocked", why);
		return;
	}
	if (upstreamkeys_normalise(recipe->upstream_verify_key, fpr) != 0) {
		snprintf(why, sizeof(why),
		         "the recipe's verify key \"%s\" is not a 40-digit fingerprint",
		         recipe->upstream_verify_key);
		store_note(name, version, "authenticate", "blocked", why);
		return;
	}
	key = upstreamkeys_find(name, fpr, &key_len);
	if (key == NULL) {
		snprintf(why, sizeof(why),
		         "no upstream key %s is installed for %s -- an operator installs it with "
		         "`cixctl pkg upstream-keys add %s --fingerprint=%s --file=KEY.asc` (ADR-0318)",
		         fpr, name, name, fpr);
		store_note(name, version, "authenticate", "blocked", why);
		return;
	}
	if (srcupstream_expand(recipe->upstream_verify_url, version, raw, sizeof(raw)) != 0 ||
	    url_with_token(name, raw, url, sizeof(url)) != 0 ||
	    srcupstream_expand(recipe->upstream_source, version, src, sizeof(src)) != 0) {
		store_note(name, version, "authenticate", "failed",
		           "the checksum list or release url could not be formed from its template");
		return;
	}
	/* The archive's name in the list is the last path element of its url. */
	leaf = strrchr(src, '/');
	leaf = leaf != NULL ? leaf + 1 : src;
	file_len = strcspn(leaf, "?#");
	if (file_len == 0 || file_len >= sizeof(file)) {
		store_note(name, version, "authenticate", "failed",
		           "the release url names no file to look up in the checksum list");
		return;
	}
	memcpy(file, leaf, file_len);
	file[file_len] = '\0';

	snprintf(tmp, sizeof(tmp), "%s/upstream/.checksums-%s", g_pkg_dir, name);
	if (discover_fetch(url, tmp, err, sizeof(err)) != 0 ||
	    persist_read_file(tmp, &doc, &doc_len) != 0 || doc == NULL) {
		unlink(tmp);
		snprintf(why, sizeof(why), "the checksum list could not be fetched from %s: %s", raw,
		         err);
		store_note(name, version, "authenticate", "failed", why);
		return;
	}
	unlink(tmp);
	vr = pgp_clearsign_verify(doc, doc_len, key, key_len, fpr, &text, NULL, err, sizeof(err));
	free(doc);
	if (vr != PGP_VERIFY_OK) {
		snprintf(why, sizeof(why), "the checksum list from %s did not verify under key %s: %s (%s)",
		         raw, fpr, pgp_verify_result_name(vr), err);
		store_note(name, version, "authenticate", "failed", why);
		return;
	}
	memset(&c, 0, sizeof(c));
	if (pgp_checksum_lookup(text, file, c.sha256, sizeof(c.sha256)) != 0) {
		free(text);
		snprintf(why, sizeof(why), "the signed checksum list from %s does not name %s", raw,
		         file);
		store_note(name, version, "authenticate", "failed", why);
		return;
	}
	free(text);
	utc_stamp(now, stamp, sizeof(stamp));
	snprintf(c.version, sizeof(c.version), "%s", version);
	snprintf(c.url, sizeof(c.url), "%s", src);
	snprintf(c.verification, sizeof(c.verification),
	         "signed checksums: %s in %s, verified under key %s at %s", file, raw, fpr, stamp);
	srcrecord_store_candidate(name, &c);
	snprintf(why, sizeof(why),
	         "sha256 %s, from a checksum list signed by %s; waiting for the author stage",
	         c.sha256, fpr);
	store_note(name, version, "authenticate", "ok", why);
}

/*
 * The authenticate stage for one package: a release newer than any
 * recipe is authenticated the way the recipe says, or the row says why
 * not. A package that is current, or whose discovery stopped, is left.
 */
static void authenticate_one(const char *name, const struct pkg_recipe *recipe, long now)
{
	struct srcresolve_entry e;
	char why[512];

	/*
	 * A release with no recipe reads author/blocked -- or, once this
	 * stage has tried it, whatever this stage last recorded
	 * (authenticate/blocked, failed or ok), because the catalogue
	 * applies the record. Both are this stage's to (re)try: the cause
	 * of a block -- an untrusted origin, a missing key -- is fixed by
	 * an operator between runs. Measured on 0.2.57-457's selftest:
	 * acting on author/blocked alone left both the origin and the
	 * checksum case stuck on their first run's note. Anything else is
	 * current, held, marked bad, being authored, or stopped earlier.
	 */
	srcresolve_package(name, recipe->upstream, &e);
	if (!(e.stage == PIPELINE_AUTHOR && e.status == PIPELINE_BLOCKED) &&
	    e.stage != PIPELINE_AUTHENTICATE)
		return;

	if (strcmp(recipe->upstream_verify, "origin") == 0) {
		authenticate_by_origin(name, recipe, e.resolved_version, now);
		return;
	}
	if (strcmp(recipe->upstream_verify, "checksums") == 0) {
		authenticate_by_checksums(name, recipe, e.resolved_version, now);
		return;
	}
	if (recipe->upstream_verify[0] == '\0')
		snprintf(why, sizeof(why),
		         "the recipe's upstream block declares no verification, so no release of it "
		         "can be authenticated (ADR-0323)");
	else
		snprintf(why, sizeof(why),
		         "the recipe verifies by \"%s\", which this platform does not implement yet "
		         "-- origin trust (rung 4) and a signed checksum list (rung 2) are",
		         recipe->upstream_verify);
	store_note(name, e.resolved_version, "authenticate", "blocked", why);
}

/* The helper's work: refresh every gitea-tags package of the run's
 * kind, then authenticate every package with an upstream of that kind.
 * Exits with how many listings failed, capped, so done() can say so
 * without a result file. */
int pkg_upstream_refresh_work(void *params_arg)
{
	static char names[1024][PKG_IMAGE_NAME_MAX];
	const struct pkg_discover_params *params = params_arg;
	const char *kind = params != NULL ? params->kind : "";
	int refresh = params == NULL || params->refresh;
	char recipe_path[PATH_MAX];
	struct pkg_recipe recipe;
	long now = (long)time(NULL);
	int n, i, failed = 0;

	n = pkg_recipe_list_names(names, 1024);
	for (i = 0; i < n; i++) {
		if (find_recipe_path(names[i], NULL, recipe_path, sizeof(recipe_path)) != 0 ||
		    parse_recipe(recipe_path, &recipe) != 0 || recipe.upstream[0] == '\0' ||
		    (kind[0] != '\0' && strcmp(recipe.upstream, kind) != 0))
			continue;
		if (refresh && strcmp(recipe.upstream, "gitea-tags") == 0 &&
		    refresh_one_gitea(names[i], &recipe, now) != 0) {
			failed++;
			continue;
		}
		authenticate_one(names[i], &recipe, now);
	}
	return failed > 100 ? 100 : failed;
}

/*
 * ADR-0323's author stage, driven by discovery: the first authenticated
 * candidate is written, committed and published through
 * pkg_recipe_revise_start(). One per discovery run, because a recipe
 * commit is one at a time; the rest keep their candidate and are
 * authored on a later run. Returns 1 when a commit was staged (the
 * caller then runs its helper), 0 when nothing was.
 */
int pkg_discover_author_next(void)
{
	static char names[1024][PKG_IMAGE_NAME_MAX];
	char err[PKG_ERROR_MAX], newest[PKG_VERSION_MAX], candidate_version[PKG_VERSION_MAX + 4];
	struct srcrecord_candidate c;
	int n, i;

	n = pkg_recipe_list_names(names, 1024);
	for (i = 0; i < n; i++) {
		if (srcrecord_candidate(names[i], &c) != 0)
			continue;
		/* #565: a hold placed after the candidate was found still holds. */
		{
			struct srcpolicy held;

			srcpolicy_get(names[i], &held);
			if (held.pinned) {
				srcrecord_store_candidate(names[i], NULL);
				continue;
			}
		}
		/* A candidate a recipe already builds was authored already. */
		snprintf(candidate_version, sizeof(candidate_version), "%s-1", c.version);
		if (recipe_latest_version(names[i], newest, sizeof(newest)) == 0 &&
		    pkg_version_compare(candidate_version, newest) <= 0) {
			srcrecord_store_candidate(names[i], NULL);
			continue;
		}
		if (pkg_recipe_revise_start(names[i], c.version, c.url, c.sha256, c.verification, NULL,
		                            err, sizeof(err)) == PKG_OK) {
			srcrecord_store_candidate(names[i], NULL);
			store_note(names[i], c.version, "author", "ok",
			           "the next revision was written and is being committed before it is "
			           "published");
			return 1;
		}
		/* Refused: it stays a candidate, and the row says why. */
		store_note(names[i], c.version, "author", "failed", err);
	}
	return 0;
}

void pkg_upstream_refresh_done(int exit_status, void *unused)
{
	(void)unused;
	g_upstream_refresh_running = 0;
	if (exit_status == 0)
		logstore_write("cixd", "info", "pkg: discovery run finished (ADR-0323)");
	else if (exit_status > 0)
		logstore_write("cixd", "warn",
		               "pkg: discovery finished; %d gitea-tags listing(s) could not be "
		               "refreshed -- each says why "
		               "in GET /v1/pkg/source-catalogue (ADR-0323)",
		               exit_status);
	else
		logstore_write("cixd", "error", "pkg: the discovery helper did not finish");
}

/* Called when the helper could not be started, so done() never runs. */
void pkg_upstream_refresh_abort(void)
{
	g_upstream_refresh_running = 0;
}

int pkg_upstream_refresh_running(void)
{
	return g_upstream_refresh_running;
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
 * Written into the build container beside recipe.sh and RUN after the
 * install phase -- by the build command for a shell recipe, and by
 * `cbs build --finalize-command` for a CBS one (ADR-0307 clause 2).
 * It used to be sourced, reading $PKG_DESTDIR out of the environment;
 * CBS runs an embedder's finalizer as `execlp(cmd, cmd, staged_root,
 * NULL)`, so the only way one policy file can serve both languages is
 * as a program taking the staged root as its argument.
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

	/*
	 * 0755, because it is execve()d now rather than sourced (ADR-0307
	 * clause 2).
	 *
	 * By construction rather than by observation, and said that way
	 * on purpose: cix-build-system src/main.c:514-522 forks and calls
	 * execlp(command, command, staged_root, NULL), then _exit(127) on
	 * any failure to exec -- so a 0644 file's EACCES arrives as 127,
	 * indistinguishable from a missing interpreter or a missing file.
	 * The policy has never actually been staged 0644 through this
	 * path; the point is that if it were, the error would name
	 * nothing useful.
	 */
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0)
		return -1;
	n = write(fd, PKG_FINALIZE_SH, len);
	if (close(fd) != 0 || n < 0 || (size_t)n != len)
		return -1;
	return 0;
}

/*
 * A CBS recipe's build (ADR-0305).
 *
 * `cbs build --staged W` treats W as a WORKSPACE, not as the staged
 * tree: it creates W/src, W/build, W/dest and W/cache under it, and an
 * install phase's ${dest} is W/dest. That is why PKG_DESTDIR is set
 * per-entry rather than being the fixed /build/pkg-dest a shell recipe
 * uses -- pointing the variable at the subdirectory costs one
 * assignment, where moving the tree afterwards would have made every
 * CBS recipe depend on mv and rmdir being among the tools it declared.
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
 * chosen: without it a failing phase prints NOTHING. The first CBS
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
 * finalize.sh is CBS's own --finalize-command rather than a shell step
 * after it (ADR-0307 clause 2). Measured at tag v0.1.29 rather than
 * assumed, because the ordering is the whole point:
 * cbs_build_standalone_with_events_policy() runs the finalizer at
 * src/package.c:374 and packages at :389, so the policy is applied
 * BEFORE any artifact is written -- which is what makes it possible
 * for CBS to write that artifact at all (clause 2's other half). It
 * also runs when no --output is given, so this is correct today,
 * while cixd still packages the tree itself.
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

#define PKG_CBS_WORKSPACE_REL "build/cbsws"
#define PKG_CBS_WORKSPACE "/" PKG_CBS_WORKSPACE_REL
#define PKG_CBS_CACHE_REL "build/cbscache"
#define PKG_CBS_CACHE_DIR "/" PKG_CBS_CACHE_REL

/*
 * Where CBS writes the artifact it packages (ADR-0307 clause 2).
 *
 * Beside recipe.cbs and finalize.sh in /build, and deliberately NOT
 * inside the workspace: `--staged W` owns W entirely -- src, build,
 * dest, cache and tmp are its subdirectories -- and an artifact
 * dropped in there would be a file CBS did not put there, inside a
 * tree it manages.
 *
 * One definition with both roots, the same shape as PKG_DEST_REL_CBS
 * below and for the same reason: the path the container is told to
 * write and the path cixd harvests are one string, so they cannot
 * disagree. They already did once -- the first CBS build harvested
 * the shell destination while the build staged into the workspace,
 * and published a package with zero files and a state of
 * "installed".
 */
#define PKG_CBS_ARTIFACT_REL "build/artifact.cixpkg"
#define PKG_CBS_ARTIFACT "/" PKG_CBS_ARTIFACT_REL

/*
 * A kmod-build's extra symbols reach a CPDL recipe as a CBS input
 * (#517): `cbs build --input kmod_extra=<this path>`, which the kernel
 * recipe consumes with `args input "kmod_extra"` -- a CPDL recipe
 * cannot name /build/extra itself, which is outside the CBS roots
 * (cix-build-system#231, in cbs v0.1.54). The file is the one
 * write_kmod_extra_config() writes, and the option is passed only when
 * that wrote one, because an absent optional input is how a recipe
 * learns there are no extra symbols.
 */
#define PKG_CBS_KMOD_EXTRA_INPUT "kmod_extra=/build/extra/kmod-extra.config"

/*
 * Where a recipe's install lands, relative to both the container's root
 * and the build container's upperdir -- which are the same path with a
 * different prefix.
 *
 * ONE definition, because two once disagreed and the failure was
 * silent: the first CBS build set the container's PKG_DESTDIR to the
 * workspace's dest and left cixd harvesting the shell recipes'
 * build/pkg-dest, so the package was published with ZERO files and a
 * state of "installed". Shell recipes are gone (cix#569); the rule that
 * there is one definition stays.
 *
 * So the destination is computed here and nowhere else, and the
 * environment variable the recipe reads and the directory cixd
 * harvests are the same string with different roots.
 */
#define PKG_DEST_REL_CBS PKG_CBS_WORKSPACE_REL "/dest"

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

	/*
	 * Every exit from this function logs (#504). A caller reporting
	 * "the step's own error is logged immediately above" depends on
	 * it, and four exits here used to return silently.
	 */
	a = archive_read_new();
	if (a == NULL) {
		logstore_write("cixd", "error", "extract %s: archive_read_new failed (out of memory)",
		               archive_path);
		return -1;
	}
	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);

	ext = archive_write_disk_new();
	if (ext == NULL) {
		logstore_write("cixd", "error",
		               "extract %s: archive_write_disk_new failed (out of memory)", archive_path);
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
		char hfull[PATH_MAX];
		const char *hardlink;
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

		if ((size_t)snprintf(full, sizeof(full), "%s/%s", dest_dir, name) >= sizeof(full)) {
			logstore_write("cixd", "error",
			               "extract %s: member \"%s\" is too long to place under %s",
			               archive_path, name, dest_dir);
			goto out;
		}
		archive_entry_set_pathname(entry, full);

		/*
		 * #506: a HARD LINK's target is an archive-internal path and
		 * needs the identical transformation the pathname just got.
		 * Rewriting one and not the other is the whole bug: the
		 * member lands at <dest_dir>/<stripped> while its link target
		 * still reads "<archive-prefix>/<path>", which libarchive
		 * resolves against the current directory, does not find, and
		 * refuses --
		 *
		 *   write header for ".../build/src/snap/hooks/install"
		 *   failed: Hard-link target
		 *   'keepalived-2.3.4/snap/hooks/post-refresh' does not exist
		 *
		 * The message names the LINK and the TARGET and nothing about
		 * path rewriting, so it reads as a malformed archive. It is
		 * not: keepalived ships snap/hooks/install and post-refresh
		 * as one inode, which is ordinary, and every such package was
		 * unbuildable.
		 *
		 * A SYMLINK target is deliberately NOT rewritten. It is
		 * interpreted relative to the link's own directory at
		 * resolution time, so it stays correct under both the strip
		 * and the dest_dir prefix -- rewriting it would break it. The
		 * two look alike and are opposite, which is worth the
		 * paragraph.
		 *
		 * An absolute target is refused for the same reason an
		 * absolute member path is: it escapes dest_dir, and
		 * ARCHIVE_EXTRACT_SECURE_NODOTDOT does not cover a link
		 * target.
		 */
		hardlink = archive_entry_hardlink(entry);
		if (hardlink != NULL) {
			const char *h = hardlink;

			if (h[0] == '/') {
				logstore_write("cixd", "error",
				                "extract %s: refusing absolute hard-link target \"%s\"",
				                archive_path, h);
				goto out;
			}
			if (strip_first_component) {
				const char *hslash = strchr(h, '/');

				if (hslash == NULL) {
					logstore_write("cixd", "error",
					                "extract %s: hard-link target \"%s\" has no component to strip",
					                archive_path, h);
					goto out;
				}
				h = hslash + 1;
			}
			if ((size_t)snprintf(hfull, sizeof(hfull), "%s/%s", dest_dir, h) >= sizeof(hfull)) {
				logstore_write("cixd", "error",
				               "extract %s: hard-link target \"%s\" is too long to place under %s",
				               archive_path, h, dest_dir);
				goto out;
			}
			archive_entry_set_hardlink(entry, hfull);
		}

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

/*
 * keep_privileged (#552): whether a copied file keeps its setuid, setgid
 * and sticky bits. Nonzero only for a tree cbs produced -- a CPDL build
 * or an extracted .cixpkg -- because cbs refuses any setuid or setgid
 * entry the recipe did not declare with `privileged file`, so what
 * arrives here is what the recipe asked for. Everything else is masked
 * to 0777, as every install was until 2026-09-30: that mask is how
 * openssh's declared 04711 ssh-keysign reached jumpbox as 0711
 * (probe-setuid@1-1, 192.168.15.95).
 */
static int merge_tree(const char *src_root, const char *dst_root, const char *relpath,
                       struct pkg_entry *e, int keep_privileged)
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
			if (merge_tree(src_root, dst_root, child_rel, e, keep_privileged) != 0) {
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
			chmod(dst_path, st.st_mode & (keep_privileged ? 07777 : 0777));
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
 * How many of e's recorded files are absent under rootfs, and the first
 * one. lstat, not stat: a dangling symlink is a file the package really
 * installed. A path too long to build cannot be checked, so it is not
 * counted against the tree. Shared by the post-install check (#281) and
 * the dedup check below (#551), which ask the same question of two trees.
 */
static int files_missing_in(const char *rootfs, const struct pkg_entry *e, char *first_missing,
                            size_t first_missing_size)
{
	char path[PATH_MAX];
	struct stat st;
	int missing = 0;
	int i;

	if (first_missing != NULL && first_missing_size > 0)
		first_missing[0] = '\0';
	if (e == NULL)
		return 0;
	for (i = 0; i < e->file_count; i++) {
		const char *rel = e->files[i];

		while (*rel == '/')
			rel++;
		if (snprintf(path, sizeof(path), "%s/%s", rootfs, rel) >= (int)sizeof(path))
			continue;
		if (lstat(path, &st) != 0) {
			if (missing == 0 && first_missing != NULL && first_missing_size > 0)
				snprintf(first_missing, first_missing_size, "%s", rel);
			missing++;
		}
	}
	return missing;
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

	if (first_missing != NULL && first_missing_size > 0)
		first_missing[0] = '\0';
	if (e == NULL || e->file_count == 0)
		return 0;
	if (image_current_version(image, version, sizeof(version)) != IMAGE_OK)
		return 0; /* Not this check's failure to report -- the caller
		           * already treats a missing current version as its own
		           * error, and guessing here would report the wrong one. */
	image_version_rootfs_path(image, version, rootfs, sizeof(rootfs));
	return files_missing_in(rootfs, e, first_missing, first_missing_size);
}

/*
 * #551: is the stored tree for a version this image is about to reuse
 * missing files its installed packages record?
 *
 * An image version is named by its package set (ADR-0108), so a set that
 * recurs reuses the tree first stored under that name (ADR-0155). A tree
 * stored wrong -- chains before #531 was fixed merged one package's
 * leftovers into another's install, measured on 192.168.15.95 as
 * jumpbox's login@2.42.2-5 holding linux-pam's files and no /bin/login --
 * came back every time its set recurred, and the correct tree just built
 * was thrown away in its favour. ADR-0320 made recurring sets routine:
 * downgrades and re-applies revisit them.
 *
 * Checks the same entries build_image_manifest_string() hashes, so the
 * answer is about exactly the set the version name stands for. Returns 1
 * with the first finding in why, else 0.
 */
static int stored_version_is_wrong(const char *image, const char *rootfs, char *why,
                                   size_t why_size)
{
	int i;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		const struct pkg_entry *e = &g_packages[i];
		char first[PATH_MAX];
		int missing;

		if (!e->in_use || e->state != PKG_STATE_INSTALLED || strcmp(e->image, image) != 0)
			continue;
		missing = files_missing_in(rootfs, e, first, sizeof(first));
		if (missing > 0) {
			snprintf(why, why_size, "%s@%s: %d of %d file(s) missing, starting with %s",
			         e->name, e->version, missing, e->file_count, first);
			return 1;
		}
	}
	return 0;
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
	char why[PATH_MAX + 160];

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
	if (stat(new_rootfs, &st) == 0 &&
	    stored_version_is_wrong(image, new_rootfs, why, sizeof(why))) {
		/*
		 * #551: the stored tree for this package set is wrong, so the
		 * tree just built wins. The old one is renamed aside rather than
		 * deleted: a running container mounted it as its lower layer, and
		 * an overlay holds the directory it resolved at mount time, not
		 * the path. image_sweep_replaced_trees() deletes it at the next
		 * start, before any container can mount anything.
		 */
		char aside[PATH_MAX];

		if (snprintf(aside, sizeof(aside), "%s.replaced-%ld", new_rootfs, (long)time(NULL)) >=
		    (int)sizeof(aside))
			return produce_fail(image, staging, "naming the wrong stored tree aside", 0);
		if (rename(new_rootfs, aside) != 0)
			return produce_fail(image, staging, "setting the wrong stored tree aside", 1);
		if (rename(staging, new_rootfs) != 0) {
			rename(aside, new_rootfs);
			return produce_fail(image, staging, "renaming the rebuilt tree over the wrong one",
			                     1);
		}
		logstore_write("cixd", "error",
		               "image %s: the stored tree for version %s was wrong (%s) -- replaced by "
		               "the tree just built, which has them. The old tree is at %s until the "
		               "next start, for any container still using it (#551)",
		               image, new_version, why, aside);
	} else if (stat(new_rootfs, &st) == 0) {
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
 * answers 409 to a republish). So a recipe cannot change under a
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

int pkg_host_state(const char *name, struct pkg_host_state *out)
{
	const struct pkg_entry *e = pkg_find(name, PKG_HOSTBUILD_IMAGE);

	memset(out, 0, sizeof(*out));
	if (e == NULL)
		return -1;
	out->building = e->state == PKG_STATE_FETCHING || e->state == PKG_STATE_BUILDING;
	out->failed = e->state == PKG_STATE_FAILED;
	if (e->state == PKG_STATE_INSTALLED)
		snprintf(out->installed, sizeof(out->installed), "%s", e->version);
	if (out->failed)
		snprintf(out->error, sizeof(out->error), "%s", e->error);
	pkg_entry_drift(e, out->available, sizeof(out->available));
	return 0;
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
	/*
	 * #424: the stage now in flight, when it began, and how long it has
	 * been going -- stage_seconds server-computed for the same no-skew
	 * reason as run_seconds below. The current stage is the latest one
	 * whose clock has started: install, else build, else fetch.
	 */
	{
		/* A cache hit's no-op build container builds nothing, and its
		 * run record leaves the build stage out, so the live view does
		 * too: it is still fetching until the install begins. */
		int built = e->build_started_at > 0 && !e->cache_hit;
		enum pipeline_stage now = e->install_started_at > 0 ? PIPELINE_INSTALL
		                          : built                   ? PIPELINE_BUILD
		                                                    : PIPELINE_FETCH;
		time_t stage_at = e->install_started_at > 0 ? e->install_started_at
		                  : built                   ? e->build_started_at
		                                            : e->run_started_at;

		jw_key(w, "current_stage");
		if (e->run_started_at > 0)
			jw_str(w, pipeline_stage_name(now));
		else
			jw_null(w);

		jw_key(w, "install_started_at");
		if (e->run_started_at > 0 && e->install_started_at > 0)
			jw_int(w, (long long)e->install_started_at);
		else
			jw_null(w);
		jw_key(w, "stage_started_at");
		if (e->run_started_at > 0)
			jw_int(w, (long long)stage_at);
		else
			jw_null(w);
		jw_key(w, "stage_seconds");
		if (e->run_started_at > 0)
			jw_int(w, (long long)(time(NULL) - stage_at));
		else
			jw_null(w);
	}
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
	{
		char upstream_dir[PATH_MAX];

		snprintf(upstream_dir, sizeof(upstream_dir), "%s/upstream", pkg_dir);
		srcgitea_init(upstream_dir);
		snprintf(upstream_dir, sizeof(upstream_dir), "%s/discovery", pkg_dir);
		srcrecord_init(upstream_dir);
	}
}

/*
 * ADR-0307 clause 7: rebuild every derived identity when the engine
 * that derives them has changed.
 *
 * `explain.json` is a cache of `cbs explain --json` over an immutable
 * recipe (ADR-0107), so re-deriving one cannot produce a different
 * answer about the same text -- only a more complete one from a newer
 * engine. What made that necessary rather than tidy: `format` only
 * exists from cbs v0.1.26 (cix-build-system#173), and clause 1 reads
 * the declared artifact format out of this document, so every
 * document written by an older engine is missing the field the daemon
 * now needs. Refusing those would make the older half of the CBS
 * corpus unbuildable; assuming a value is what clause 1 exists to
 * prevent.
 *
 * Once at startup, and that is COMPLETE rather than merely cheap: the
 * host's cbs is /usr/bin/cbs inside the control-plane root, which
 * mkbootroot stages from cix-hosttools at assembly time, and that root
 * is a read-only squashfs replaced only by POST /system/update and a
 * reboot. The engine therefore cannot change while this daemon runs,
 * and every way it can change passes through a restart. A recipe
 * published while the daemon is up is derived by that same engine at
 * publish. So there is no window a startup sweep misses, and no need
 * for a second, lazy path on the read side -- which would be the far
 * worse shape anyway: see cbs_explain_path()'s own comment for why a
 * re-derive per read is forbidden (~1400 recipe files, and #236
 * measured the event loop blocked for 10981 ms on a cheaper walk).
 *
 * Staleness is the engine's version string, not a probe for one key.
 * A key-absence test cannot tell "this engine does not emit it" from
 * "this recipe did not declare it", and it would have to be written
 * again for the next key CBS adds.
 */
static void cbs_engine_version(char *out, size_t out_size)
{
	int fds[2];
	pid_t pid;
	int status = 0;
	size_t total = 0;

	out[0] = '\0';
	if (access(PKG_CBS_BIN, X_OK) != 0)
		return;
	if (pipe2(fds, O_CLOEXEC) != 0)
		return;
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (pid == 0) {
		char *argv[] = { (char *)PKG_CBS_BIN, (char *)"--version", NULL };
		int devnull = open("/dev/null", O_RDWR);

		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDERR_FILENO);
		}
		dup2(fds[1], STDOUT_FILENO);
		execve(PKG_CBS_BIN, argv, environ);
		_exit(127);
	}
	close(fds[1]);
	for (;;) {
		ssize_t n = read(fds[0], out + total, out_size - 1 - total);

		if (n <= 0)
			break;
		total += (size_t)n;
		if (total >= out_size - 1)
			break;
	}
	out[total] = '\0';
	close(fds[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		out[0] = '\0';
		return;
	}
	out[strcspn(out, "\r\n")] = '\0';
}

/*
 * What the sweep did, held for pkg_log_explain_sweep() to report.
 *
 * pkg_init() runs ~100 lines BEFORE logstore_init() in main(), so a
 * logstore_write() from here is dropped by write_entry()'s own
 * !g_initialized guard and the sweep says nothing anywhere. That was
 * measured, not guessed: the first cut of this logged directly, the
 * documents on 192.168.15.95 were demonstrably re-derived, and
 * `GET /system/logs?regex=CPDL engine` returned `[]`.
 *
 * This is the fourth time this project has learned that a diagnostic
 * written before there is somewhere to write it goes nowhere --
 * log_degraded_placements() (#256), pkg_migrate_build_sandbox()
 * (#40) and iso_recover_state() (#205) each carry the same note. The
 * shape they settled on is this one: do the work where it has to
 * happen, record what happened, and say it once the log store exists.
 */
static struct {
	int ran;
	int no_engine;
	int rederived;
	int failed;
	/* Wide enough for "<version> <64-hex digest>" plus slack, since
	 * the key gained the digest (#496) and a truncated one would
	 * compare unequal forever, re-deriving on every boot. */
	char from[256];
	char to[256];
	/* Which recipes failed, for the same reason as the counts: named
	 * at sweep time, said once the log store exists. Bounded; names
	 * that do not fit are counted in failed_unnamed. */
	char failed_names[1024];
	int failed_unnamed;
} g_explain_sweep;

void pkg_log_explain_sweep(void)
{
	if (!g_explain_sweep.ran)
		return;
	if (g_explain_sweep.no_engine) {
		logstore_write("cixd", "info",
		               "pkg: no CPDL engine at %s, so no derived identity was checked "
		               "(ADR-0307 clause 7)",
		               PKG_CBS_BIN);
		return;
	}
	logstore_write("cixd", g_explain_sweep.failed > 0 ? "warn" : "info",
	               "pkg: CPDL engine changed (%s -> %s): re-derived %d identit%s, %d failed "
	               "(ADR-0307 clause 7)",
	               g_explain_sweep.from[0] != '\0' ? g_explain_sweep.from : "none recorded",
	               g_explain_sweep.to, g_explain_sweep.rederived,
	               g_explain_sweep.rederived == 1 ? "y" : "ies", g_explain_sweep.failed);
	if (g_explain_sweep.failed_names[0] != '\0')
		logstore_write("cixd", "warn",
		               "pkg: engine sweep could not re-derive %s%s -- those recipes keep "
		               "their old identity until they are re-derived",
		               g_explain_sweep.failed_names,
		               g_explain_sweep.failed_unnamed > 0 ? " (and more not listed)" : "");
}

/*
 * cix#569: shell recipes do not exist (the owner, 2026-10-03), and
 * neither does the .tar.gz artifact they published. Nothing in this
 * daemon reads either, so what a store still holds of them is removed
 * at startup, once, and idempotently: a host with none left does
 * nothing.
 *
 * - A version directory's build.sh is deleted, and the directory with
 *   it when that leaves it empty. A directory that also holds a
 *   build.cbs keeps it. Nothing is deleted that is not exactly a
 *   regular file named build.sh, and rmdir() refuses a directory that
 *   still holds anything, so no recipe this daemon reads is touched.
 * - The local artifact cache's .tar.gz files, and their .minisig, are
 *   deleted by retire_targz_artifacts() once pkg_cache_init() has set
 *   the cache directory.
 *
 * Counted here, logged by pkg_log_shell_retirement() once the log store
 * is up -- the same split the explain sweep uses.
 */
#define SHELL_RECIPE_FILE "build.sh"

static struct {
	int recipes;   /* build.sh files removed */
	int versions;  /* version directories left empty and removed */
	int artifacts; /* .tar.gz cache files, and signatures, removed */
	int failed;    /* removals that did not happen */
} g_shell_retire;

static void retire_shell_recipes(void)
{
	DIR *names = opendir(g_recipes_dir);
	struct dirent *nde;

	if (names == NULL)
		return;
	while ((nde = readdir(names)) != NULL) {
		char name_dir[PATH_MAX];
		DIR *versions;
		struct dirent *vde;

		if (nde->d_name[0] == '.')
			continue;
		if (snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, nde->d_name) >=
		    (int)sizeof(name_dir))
			continue;
		versions = opendir(name_dir);
		if (versions == NULL)
			continue;
		while ((vde = readdir(versions)) != NULL) {
			char version_dir[PATH_MAX];
			char shell_file[PATH_MAX];
			struct stat st;

			if (vde->d_name[0] == '.')
				continue;
			if (snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir,
			             vde->d_name) >= (int)sizeof(version_dir) ||
			    snprintf(shell_file, sizeof(shell_file), "%s/" SHELL_RECIPE_FILE,
			             version_dir) >= (int)sizeof(shell_file))
				continue;
			if (lstat(shell_file, &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			if (unlink(shell_file) != 0) {
				g_shell_retire.failed++;
				continue;
			}
			g_shell_retire.recipes++;
			if (rmdir(version_dir) == 0)
				g_shell_retire.versions++;
		}
		closedir(versions);
		/* Empty now only if every version was a shell one. */
		(void)rmdir(name_dir);
	}
	closedir(names);
}

void pkg_log_shell_retirement(void)
{
	if (g_shell_retire.recipes == 0 && g_shell_retire.artifacts == 0 &&
	    g_shell_retire.failed == 0)
		return;
	logstore_write("cixd", g_shell_retire.failed > 0 ? "warn" : "info",
	               "pkg: removed %d stored shell recipe%s (%d version director%s left empty) "
	               "and %d .tar.gz cache file%s; %d removal%s failed -- shell recipes and "
	               ".tar.gz artifacts no longer exist (cix#569)",
	               g_shell_retire.recipes, g_shell_retire.recipes == 1 ? "" : "s",
	               g_shell_retire.versions, g_shell_retire.versions == 1 ? "y" : "ies",
	               g_shell_retire.artifacts, g_shell_retire.artifacts == 1 ? "" : "s",
	               g_shell_retire.failed, g_shell_retire.failed == 1 ? "" : "s");
}

static void explain_sweep_if_engine_changed(void)
{
	char version[128];
	char digest[65];
	char key[256];
	char recorded[256];
	char state_path[PATH_MAX];
	char *stored = NULL;
	size_t stored_len = 0;
	DIR *names;
	struct dirent *nde;

	/*
	 * The key is the engine's own BYTES, not the version string it
	 * prints, and that distinction is not hypothetical (#496).
	 *
	 * Measured on 2026-09-21: cix-build-system's `main` carries
	 * commit f22e6aa -- a real change to manifest.c -- while its
	 * VERSION file still reads 0.1.29, the same as the tag. Upstream
	 * ships fixes between tags, and its own test suite asserts that
	 * `cbs --version` equals VERSION, so a newer engine can and does
	 * report an older number. Keyed on that string, this sweep would
	 * decide nothing had changed and leave every derived document
	 * stale -- silently, which is the failure mode clause 7 exists to
	 * end rather than to reproduce in a new place.
	 *
	 * A digest cannot say that. Any change to the binary changes it,
	 * including one upstream did not think worth a version.
	 *
	 * The version string is still read, because it is what a human
	 * reads in the log line -- "0.1.29 -> 0.1.29" alongside a changed
	 * digest is a more useful thing to see than two hashes, and it
	 * says out loud that the engine moved without renaming itself.
	 */
	cbs_engine_version(version, sizeof(version));
	if (version[0] == '\0') {
		/* No engine on this host. Not an error in itself -- a box
		 * that installs no CBS recipe never needs one -- and saying
		 * so once at startup beats a refusal per recipe later. */
		g_explain_sweep.ran = 1;
		g_explain_sweep.no_engine = 1;
		return;
	}

	if (pkg_run_capture_sha256(PKG_CBS_BIN, digest, sizeof(digest)) != 0) {
		/* An engine that runs but cannot be hashed is a state worth
		 * saying out loud rather than treating as unchanged: the
		 * alternative is deciding "no sweep needed" from a failure. */
		g_explain_sweep.ran = 1;
		g_explain_sweep.failed++;
		snprintf(g_explain_sweep.to, sizeof(g_explain_sweep.to), "%s (unhashable)", version);
		return;
	}
	snprintf(key, sizeof(key), "%s %s", version, digest);

	snprintf(state_path, sizeof(state_path), "%s/cbs-engine", g_pkg_dir);
	recorded[0] = '\0';
	if (persist_read_file(state_path, &stored, &stored_len) == 0 && stored != NULL) {
		snprintf(recorded, sizeof(recorded), "%.*s", (int)stored_len, stored);
		recorded[strcspn(recorded, "\r\n")] = '\0';
		free(stored);
	}
	if (strcmp(recorded, key) == 0)
		return;

	g_explain_sweep.ran = 1;
	snprintf(g_explain_sweep.from, sizeof(g_explain_sweep.from), "%s", recorded);
	snprintf(g_explain_sweep.to, sizeof(g_explain_sweep.to), "%s", key);

	names = opendir(g_recipes_dir);
	if (names == NULL) {
		/* No recipes yet is the state of every fresh install, and
		 * recording the engine is still right: there is nothing
		 * stale, which is exactly what the record will then say. */
		if (persist_atomic_write(state_path, key, strlen(key)) != 0)
			g_explain_sweep.failed++;
		return;
	}
	while ((nde = readdir(names)) != NULL) {
		char name_dir[PATH_MAX];
		DIR *versions;
		struct dirent *vde;

		if (nde->d_name[0] == '.' || !pkg_name_is_valid(nde->d_name))
			continue;
		snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, nde->d_name);
		versions = opendir(name_dir);
		if (versions == NULL)
			continue;
		while ((vde = readdir(versions)) != NULL) {
			char version_dir[PATH_MAX];
			char recipe_path[PATH_MAX];

			if (vde->d_name[0] == '.')
				continue;
			snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, vde->d_name);
			if (pkg_recipe_file_in(version_dir, recipe_path, sizeof(recipe_path), NULL,
			                        NULL) != 0)
				continue;
			/*
			 * The same primitive a backup restore uses, not a
			 * second one: re-deriving a document is one operation
			 * and it already existed. Best-effort per recipe --
			 * one unreadable recipe must not stop the sweep,
			 * because the documents it has not reached yet are the
			 * ones a running daemon needs.
			 */
			if (pkg_recipe_rederive_identity(recipe_path) != PKG_OK) {
				/*
				 * Named, not only counted: on 2026-09-23 the
				 * cbs 0.1.34 -> 0.1.52 sweep reported "1 failed"
				 * and nothing anywhere said which recipe. The
				 * first fix logged the name right here, which is
				 * before logstore_init() -- so it was dropped too,
				 * and every sweep from 0.1.71 to 0.1.97 on
				 * 192.168.15.95 reported "2 failed" with no name
				 * anywhere in 1.5 MB of cixd log (2026-09-29).
				 * Recorded here, logged by pkg_log_explain_sweep().
				 */
				size_t used = strlen(g_explain_sweep.failed_names);
				int n = snprintf(g_explain_sweep.failed_names + used,
				                 sizeof(g_explain_sweep.failed_names) - used, "%s%s@%s",
				                 used > 0 ? ", " : "", nde->d_name, vde->d_name);

				if (n < 0 || (size_t)n >= sizeof(g_explain_sweep.failed_names) - used) {
					g_explain_sweep.failed_names[used] = '\0';
					g_explain_sweep.failed_unnamed++;
				}
				g_explain_sweep.failed++;
				continue;
			}
			g_explain_sweep.rederived++;
		}
		closedir(versions);
	}
	closedir(names);

	/*
	 * Recorded only when every document was rebuilt. A partial sweep
	 * that recorded the new engine would never be retried, leaving
	 * whichever recipes failed permanently stale -- and stale is the
	 * state this exists to end.
	 */
	if (g_explain_sweep.failed == 0 && persist_atomic_write(state_path, key, strlen(key)) != 0)
		g_explain_sweep.failed++;
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
	{
		char upstream_dir[PATH_MAX];

		/* ADR-0323: the gitea-tags kind's per-package listings. */
		snprintf(upstream_dir, sizeof(upstream_dir), "%s/upstream", pkg_dir);
		if (srcgitea_init(upstream_dir) != 0)
			return -1;
		/* ADR-0323: what authenticate and author found, for every kind. */
		snprintf(upstream_dir, sizeof(upstream_dir), "%s/discovery", pkg_dir);
		if (srcrecord_init(upstream_dir) != 0)
			return -1;
	}

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
	/* cix#569: before anything resolves a recipe, so nothing meets a
	 * build.sh in the store. */
	retire_shell_recipes();
	/*
	 * Before load_state(), because a derived identity that is about to
	 * be rebuilt should be rebuilt before anything reads it -- and
	 * load_state() is the first thing that does (ADR-0307 clause 7).
	 */
	explain_sweep_if_engine_changed();
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

	if (merge_tree(ctx->flat_rootfs, staging_rootfs, "", NULL, 0) != 0) {
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
/*
 * How many packages a composed environment may hold, counted over the
 * declared tools, their runtime dependencies, and the implicit glibc
 * and cbs.
 *
 * 64 since 2026-09-25, from 32, because 32 had become a real ceiling
 * rather than a generous one: the full cix-tests suite needs a set of
 * exactly 32 and so cannot declare one more, which is why
 * `test_installer` and `test_targz` (since removed) failed for want of `bzip2` -- a
 * package that exists, is built, and simply cannot be added (#485).
 * The recipe carrying that set has a comment apologising for dropping
 * `minisign` to make room, which is the shape of a limit that has
 * stopped being generous.
 *
 * The cost is bounded and small: struct buildenv_tool is 136 bytes, so
 * the array in buildenv_image_for() grows from 4.3 KB of stack to 8.7
 * KB, and the canonical-name buffer beside it from 4.2 KB to 8.4 KB.
 * Composing a larger environment copies more files, which is
 * proportional work rather than a cliff -- and a set nobody declares
 * costs nothing, since the hash of the declared set is what names the
 * image.
 */
#define PKG_BUILDENV_MAX_TOOLS 64
/*
 * How deep a dependency chain under one declared tool may go before
 * it is refused as a cycle or a mistake. A SEPARATE limit, which
 * shared PKG_BUILDENV_MAX_TOOLS until 2026-09-25 -- so raising the
 * breadth of an environment silently raised the depth it would chase,
 * and the two have nothing to do with each other. Their errors always
 * said so ("too deep" against "more than N packages"); only the
 * numbers were entangled. 32 is already far past anything real: the
 * deepest chain in this corpus is a handful of links.
 */
#define PKG_BUILDENV_MAX_DEPTH 32

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
	if (depth > PKG_BUILDENV_MAX_DEPTH) {
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
		snprintf(err, err_size, "the recipe's build requirements name no packages");
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

int pkg_recipe_seed_source_offers(const char *source)
{
	DIR *names = opendir(g_recipes_dir);
	struct dirent *nde;
	char **items = NULL;
	int count = 0, cap = 0, rc, i;

	if (names == NULL)
		return pkgsource_offers_write(source, NULL, 0);
	while ((nde = readdir(names)) != NULL) {
		char item[PKG_SOURCE_ITEM_MAX];

		if (nde->d_name[0] == '.' ||
		    snprintf(item, sizeof(item), "package:%s", nde->d_name) >= (int)sizeof(item))
			continue;
		if (count == cap) {
			int ncap = cap == 0 ? 256 : cap * 2;
			char **n = realloc(items, (size_t)ncap * sizeof(*n));

			if (n == NULL)
				break;
			items = n;
			cap = ncap;
		}
		items[count] = strdup(item);
		if (items[count] == NULL)
			break;
		count++;
	}
	closedir(names);
	rc = pkgsource_offers_write(source, (const char *const *)items, count);
	for (i = 0; i < count; i++)
		free(items[i]);
	free(items);
	return rc == 0 ? pkgsource_offers_load() : rc;
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
				/* ADR-0307: what this version PUBLISHES, which the recipe
				 * declares and the language does not imply. */
				jw_key(w, "artifact_format");
				jw_str(w, r.artifact_format);
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
	/* #502: and any OTHER credential, by shape. The line above only
	 * covers the configured token. */
	redact_url_userinfo(content, content_len + 1);

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
 * A pinned manifest entry is queued only when the image's policy is
 * `apply: converge` and the installed version differs from the pin
 * (ADR-0320): otherwise pinning means "never move",
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

			e = pkg_find(entries[j].package, image_names[i]);
			if (entries[j].mode == IMAGE_PKG_PINNED) {
				/* ADR-0320: under `apply: converge` the manifest is what
				 * the image should hold, so a pin the installed set does
				 * not match is drift to repair -- including one a
				 * `recipe: follow` apply wrote in the sync child, whose
				 * own enqueue died with it. */
				struct image_policy policy;

				imagepolicy_get(image_names[i], &policy);
				if (policy.apply != IMAGE_POLICY_APPLY_CONVERGE ||
				    (e != NULL && e->state == PKG_STATE_INSTALLED &&
				     strcmp(e->version, entries[j].version) == 0))
					continue;
				rebuild_queue_enqueue(image_names[i]);
				queued_count++;
				break;
			}
			if (entries[j].mode != IMAGE_PKG_ROLLING)
				continue;
			if (e == NULL || !pkg_entry_drift(e, NULL, 0))
				continue;
			rebuild_queue_enqueue(image_names[i]);
			queued_count++;
			break;
		}
	}
	if (queued_count > 0)
		logstore_write("cixd", "info",
		                "pkg: re-derived %d image(s) behind their manifest into the rebuild "
		                "queue (#373, ADR-0320)",
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
 * ADR-0330 (#571): under recipe=follow and apply=converge, an image
 * holds its recipe's runtime closure and nothing else. Called by the
 * rebuild drain once every manifest entry is satisfied; a no-op for any
 * other policy.
 *
 * The closure is walked from the manifest through the `depends` each
 * installed entry recorded at fetch time (ADR-0302) -- what that build
 * actually needed to run -- rather than re-read from whichever recipe
 * revision is highest now. Everything in the image outside it is
 * uninstalled, one pkg_delete() at a time, each logged by name.
 *
 * Measured before this existed, 192.168.15.95 on 2026-10-05: four
 * followed images held 46 packages their recipes did not declare,
 * cix-hosttools alone 21 -- the ISO toolchain among them, installed by
 * hand into an image whose recipe never mentioned it.
 */
static void converge_uninstall_strays(const char *image_arg,
                                      const struct image_manifest_entry *entries, int entry_count)
{
	static unsigned char keep[PKG_MAX_PACKAGES];
	char image[PKG_IMAGE_NAME_MAX];
	struct image_policy policy;
	int i, j, changed;

	snprintf(image, sizeof(image), "%s", normalize_image(image_arg));
	imagepolicy_get(image, &policy);
	/*
	 * An empty manifest is not "keep nothing": it is a recipe with no
	 * entries or a manifest not written yet, and trimming against it would
	 * empty the image. No followed image has one; this keeps it that way.
	 */
	if (entry_count == 0)
		return;
	if (policy.recipe != IMAGE_POLICY_RECIPE_FOLLOW || policy.apply != IMAGE_POLICY_APPLY_CONVERGE)
		return;

	memset(keep, 0, sizeof(keep));
	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (!g_packages[i].in_use || strcmp(normalize_image(g_packages[i].image), image) != 0)
			continue;
		for (j = 0; j < entry_count; j++) {
			if (strcmp(g_packages[i].name, entries[j].package) == 0) {
				keep[i] = 1;
				break;
			}
		}
	}
	do {
		changed = 0;
		for (i = 0; i < PKG_MAX_PACKAGES; i++) {
			char deps[PKG_DEPENDS_MAX];
			char *tok, *save;

			if (!keep[i])
				continue;
			snprintf(deps, sizeof(deps), "%s", g_packages[i].depends);
			for (tok = strtok_r(deps, " \t", &save); tok != NULL;
			     tok = strtok_r(NULL, " \t", &save)) {
				for (j = 0; j < PKG_MAX_PACKAGES; j++) {
					if (keep[j] || !g_packages[j].in_use ||
					    strcmp(g_packages[j].name, tok) != 0 ||
					    strcmp(normalize_image(g_packages[j].image), image) != 0)
						continue;
					keep[j] = 1;
					changed = 1;
				}
			}
		}
	} while (changed);

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		char name[PKG_NAME_MAX];
		enum pkg_error perr;

		if (keep[i] || !g_packages[i].in_use ||
		    strcmp(normalize_image(g_packages[i].image), image) != 0 ||
		    (g_packages[i].state != PKG_STATE_INSTALLED && g_packages[i].state != PKG_STATE_FAILED))
			continue;
		snprintf(name, sizeof(name), "%s", g_packages[i].name);
		perr = pkg_delete(name, image);
		if (perr == PKG_OK) {
			logstore_write("cixd", "info",
			               "image %s: uninstalled %s -- outside the runtime closure of the "
			               "recipe it follows (ADR-0330)",
			               image, name);
		} else {
			const char *phrase;
			char msg[512];

			pkg_error_describe(perr, &phrase, msg, sizeof(msg));
			logstore_write("cixd", "warn",
			               "image %s: could not uninstall %s, which its recipe does not cover: %s "
			               "(ADR-0330)",
			               image, name, msg);
		}
	}
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
		int deferred = 0; /* an entry waits on the same package elsewhere */
		int refused = 0;  /* entries whose install could not start */
		enum pkg_error perr;

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
				/* Built once, installed everywhere: while another
				 * image is already building this package, leave this
				 * entry for a later pass, when that build's artifact
				 * is cached. */
				if (package_job_in_flight(entries[i].package)) {
					deferred = 1;
					continue;
				}
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
				perr = pkg_install_start(entries[i].package, image, want_version, e != NULL, 0,
				                         started_name, sizeof(started_name), out_pid,
				                         out_pidfd, out_chain_idx);
				if (perr == PKG_OK) {
					started = 1;
					break;
				}
				/*
				 * Couldn't start this entry (recipe removed out from
				 * under the manifest, etc.) -- try the rest of the
				 * manifest rather than getting stuck on one bad entry,
				 * and say so. This was silent, so a converge that
				 * started nothing read exactly like one with nothing to
				 * do: iso-builder's first converge on 192.168.15.95
				 * installed none of its 40 entries and logged nothing
				 * (2026-10-05, ADR-0330).
				 */
				refused++;
				{
					const char *phrase;
					char msg[512];

					pkg_error_describe(perr, &phrase, msg, sizeof(msg));
					logstore_write("cixd", "warn",
					               "image %s: converge could not start %s: %s (ADR-0330)", image,
					               entries[i].package, msg);
				}
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

		if (deferred) {
			/* Not caught up: waiting on a package another image is
			 * building. Stays queued; a later pass installs it. */
			qi++;
			continue;
		}

		/*
		 * Nothing started and nothing waits, so this pass is done with
		 * the image: either every entry is satisfied, or the ones that
		 * are not could not start and each said why above. This comment
		 * used to call it "caught up" in both cases, which is how a
		 * converge that installed nothing read as one with nothing to
		 * do (ADR-0330).
		 *
		 * Only a fully satisfied image has its strays uninstalled: one
		 * that never reached its manifest may still need what a refused
		 * entry would have brought.
		 */
		if (refused == 0)
			converge_uninstall_strays(image, entries, entry_count);
		else
			logstore_write("cixd", "warn",
			               "image %s: converge ended with %d entr%s not installed -- each is "
			               "named above (ADR-0330)",
			               image, refused, refused == 1 ? "y" : "ies");
		approval_consume(PKG_GATE_ROLL, image);
		rebuild_queue_remove_at(qi);
	}
	return 0;
}

/*
 * The artifact approval a PUBLISHED version declares, read out of the
 * store (#525). Returns 0 and fills out on success.
 *
 * Read rather than parsed, deliberately. parse_recipe() on a CPDL
 * revision means forking `cbs explain`, and the caller below asks this
 * of every sibling version of one package -- 33 for `cbs` today --
 * on a path `recipe-sync` walks for ~400 files every six hours.
 * Two small file reads per sibling is the difference between a gate
 * that can exist and one that cannot.
 *
 * Read from the file that IS its authority (ADR-0305): explain.json,
 * which is what parse_cbs_recipe() reads -- never the .cbs.
 *
 * KEYED ON THE EXACT KEY, never on "a 64-hex run". explain.json also
 * carries the sources' own "sha256", which is the same shape as an
 * approval. A looser scan would report a SOURCE checksum collision,
 * which is not only legitimate but expected the moment two revisions
 * build the same source.
 */
static int stored_approval(const char *version_dir, char *out, size_t out_size)
{
	/* One 64-hex value, so one length, named once. */
	static const size_t SHA_LEN = PKG_SHA256_MAX - 1;
	static const struct {
		const char *file;
		const char *key;
	} WHERE[] = {
		{ "explain.json", "\"artifact_sha256\"" },
	};
	size_t w;

	for (w = 0; w < sizeof(WHERE) / sizeof(WHERE[0]); w++) {
		char path[PATH_MAX];
		char *buf = NULL;
		size_t len = 0;
		const char *p;
		int found = 0;

		snprintf(path, sizeof(path), "%s/%s", version_dir, WHERE[w].file);
		if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
			continue;
		p = strstr(buf, WHERE[w].key);
		if (p != NULL) {
			size_t i;

			p += strlen(WHERE[w].key);
			/* Past the separator and any quoting: `"k": "v"` and
			 * `pkg_artifact_sha256="v"` and a bare `=v` all land
			 * on the first hex digit. */
			while (*p == ':' || *p == ' ' || *p == '\t' || *p == '"' || *p == '\'')
				p++;
			for (i = 0; i < SHA_LEN; i++) {
				char c = p[i];

				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
					break;
			}
			if (i == SHA_LEN && out_size > SHA_LEN) {
				memcpy(out, p, SHA_LEN);
				out[SHA_LEN] = '\0';
				found = 1;
			}
		}
		free(buf);
		if (found)
			return 0;
	}
	return -1;
}

/*
 * Does another version of this package already declare this exact
 * approval? (#525)
 *
 * An approval names ONE byte sequence, produced by ONE build, and the
 * Build Provenance Mandate says it is never carried forward. It was,
 * repeatedly, and always the same way: a revision was derived from the
 * one before it by copying the file and editing the version, which
 * takes the previous revision's approval with it.
 *
 * Measured on 192.168.15.95 and in the corpus, 2026-09-27, before this
 * existed. `cbs` had TEN revisions -- v0.1.30-1 through v0.1.45-1 --
 * all declaring 4973529c7b3d..., which is v0.1.30-1's real artifact.
 * Of the other nine, three have a published artifact that says
 * otherwise (the artifact server's own X-Cix-Sha256 for v0.1.31-1,
 * v0.1.32-1 and v0.1.34-1 is b656c146..., 647478df... and
 * 3db7affb..., at 61650, 65701 and 69192 bytes against v0.1.30-1's
 * 60860), and six have no artifact at all, so their approval would
 * refuse whatever they built. `linux-headers` had the same shape at
 * three revisions, and that one was found the expensive way: through
 * an ADR-0209 floor failure a fortnight later that read as a fetch
 * problem.
 *
 * This is one string comparison per sibling at publish, and it would
 * have caught every one of them on the day it was written.
 *
 * WHAT IT COSTS. Two revisions CAN legitimately produce identical
 * bytes -- a recipe edit that changes only a comment, over the same
 * source, would -- and this refuses the second. That is the right
 * trade: the operator deletes the line and lets the build earn its
 * own approval, which is ADR-0307 clause 5's path and costs one
 * publish, while the failure this prevents is silent and surfaces
 * weeks later somewhere else.
 */
static int approval_is_taken(const char *name_dir, const char *version, const char *sha,
                             char *out_other, size_t out_other_size)
{
	DIR *d;
	struct dirent *e;
	int taken = 0;

	if (sha == NULL || sha[0] == '\0')
		return 0;
	d = opendir(name_dir);
	if (d == NULL)
		return 0;
	while ((e = readdir(d)) != NULL) {
		char version_dir[PATH_MAX];
		char have[PKG_SHA256_MAX];

		if (e->d_name[0] == '.' || strcmp(e->d_name, version) == 0)
			continue;
		snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, e->d_name);
		if (stored_approval(version_dir, have, sizeof(have)) != 0)
			continue;
		if (strcmp(have, sha) == 0) {
			snprintf(out_other, out_other_size, "%s", e->d_name);
			taken = 1;
			break;
		}
	}
	closedir(d);
	return taken;
}

/* See pkg_recipe_add_last_error()'s declaration for why this exists.
 * Cleared at the top of every publish, so it can never describe an
 * older refusal than the one the caller is reporting. */
static char g_recipe_add_err[256];

const char *pkg_recipe_add_last_error(void)
{
	return g_recipe_add_err;
}

/*
 * #503: PKG_ERR_PERSIST_FAILED, with the errno recorded so the
 * client is told WHY.
 *
 * Every one of these sites used to `return PKG_ERR_PERSIST_FAILED`
 * bare, and respond_pkg_recipe_error() turns that into
 * `500 "recipe operation failed"` -- no cause, no errno, nothing.
 *
 * On 2026-09-21 that was the entire diagnosis available while
 * /var/lib/cix sat at 0.0 GiB free: every `POST /v1/pkg/recipes`
 * answered a bare 500, for any package. A write failing for want of
 * space is distinguishable from every other failure and is the
 * single most actionable thing the daemon could say; it said the
 * least actionable thing instead, and the disk being full was found
 * by looking, an hour later.
 *
 * Reuses the existing last-error channel rather than inventing one,
 * and captures errno AT the failure rather than trusting it to
 * survive the unwinding.
 */
static enum pkg_error recipe_persist_failed(const char *what)
{
	int e = errno;

	snprintf(g_recipe_add_err, sizeof(g_recipe_add_err), "could not %s: %s", what,
	         e != 0 ? strerror(e) : "no error reported");
	logstore_write("cixd", "error", "pkg recipe: %s", g_recipe_add_err);
	return PKG_ERR_PERSIST_FAILED;
}

/*
 * ADR-0324: how a CBS recipe arriving for a version already published
 * relates to the stored one. Asked of the TEXT, because the text is the
 * recipe: `cbs explain --json` does not carry a phase's operations, so
 * two recipes that install different files explain identically
 * (measured: test_pkg on 192.168.15.95, 0.2.57-434, a changed `mkdir`
 * path was called the same recipe).
 *
 * The approval #492 writes in after a build is set aside on both sides,
 * because the daemon places that line where a hand-written copy does
 * not (measured: cbs@v0.1.100-1, store against corpus, 2026-10-02);
 * its value is reported, and two approvals naming different bytes are
 * a divergence. Lines whose first non-blank byte is `#`, and blank
 * lines, change nothing that is built: a text differing only in those
 * is COMMENTS, which git -- authoritative for recipes (the owner,
 * 2026-10-02) -- may refresh without a rebuild. 112 versions on
 * 192.168.15.95 differed from git exactly so, almost all from the
 * 2026-09-26 rename sweep run over the corpus after they were
 * published. Both sides are redacted alike first: a stored recipe older
 * than {{REPO_TOKEN}} still carries the token (#405).
 */
enum cbs_relation { CBS_SAME, CBS_COMMENTS, CBS_DIVERGENT };

/* Sets *hex to the value of an approval line. */
static int cbs_approval_line(const char *line, size_t len, const char **hex, size_t *hex_len)
{
	static const char key[] = "\"artifact_sha256\"";
	const char *p = line, *end = line + len, *q;

	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	if ((size_t)(end - p) < sizeof(key) - 1 || memcmp(p, key, sizeof(key) - 1) != 0)
		return 0;
	p += sizeof(key) - 1;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	if (p == end || *p != '"')
		return 0;
	q = memchr(p + 1, '"', (size_t)(end - p - 1));
	if (q == NULL)
		return 0;
	*hex = p + 1;
	*hex_len = (size_t)(q - p - 1);
	return 1;
}

static int cbs_line_is_cosmetic(const char *line, size_t len)
{
	size_t i = 0;

	while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
		i++;
	return i == len || line[i] == '#';
}

/*
 * One pass over both texts, approval lines (and, when `loose`, comment
 * and blank lines) set aside; the first approval value on each side is
 * copied out. 1 when what remains is identical.
 */
static int cbs_lines_equal(const char *a, const char *b, int loose, char *a_sha, char *b_sha)
{
	for (;;) {
		const char *nl, *hex;
		size_t len, hex_len;
		const char **side[2] = { &a, &b };
		char *sha[2] = { a_sha, b_sha };
		const char *anl, *bnl;
		size_t alen, blen;
		int k;

		for (k = 0; k < 2; k++)
			while (**side[k] != '\0') {
				nl = strchr(*side[k], '\n');
				len = nl != NULL ? (size_t)(nl - *side[k]) : strlen(*side[k]);
				if (cbs_approval_line(*side[k], len, &hex, &hex_len)) {
					if (sha[k][0] == '\0' && hex_len < PKG_SHA256_MAX)
						snprintf(sha[k], PKG_SHA256_MAX, "%.*s", (int)hex_len, hex);
				} else if (!loose || !cbs_line_is_cosmetic(*side[k], len)) {
					break;
				}
				*side[k] = nl != NULL ? nl + 1 : *side[k] + len;
			}
		if (*a == '\0' || *b == '\0')
			return *a == '\0' && *b == '\0';
		anl = strchr(a, '\n');
		bnl = strchr(b, '\n');
		alen = anl != NULL ? (size_t)(anl - a) : strlen(a);
		blen = bnl != NULL ? (size_t)(bnl - b) : strlen(b);
		if (alen != blen || memcmp(a, b, alen) != 0)
			return 0;
		a = anl != NULL ? anl + 1 : a + alen;
		b = bnl != NULL ? bnl + 1 : b + blen;
	}
}

/*
 * The relation of `incoming` to the build.cbs at recipe_path, and each
 * side's approval ("" when it has none). DIVERGENT when the stored copy
 * cannot be read.
 */
static enum cbs_relation recipe_cbs_relation(const char *recipe_path, const char *incoming,
                                             char *stored_sha, char *incoming_sha)
{
	char *stored = NULL;
	size_t stored_len = 0;
	enum cbs_relation rel = CBS_DIVERGENT;

	stored_sha[0] = '\0';
	incoming_sha[0] = '\0';
	if (incoming == NULL || persist_read_file(recipe_path, &stored, &stored_len) != 0 ||
	    stored == NULL)
		return CBS_DIVERGENT;
	redact_repo_token(stored, stored_len + 1);
	if (cbs_lines_equal(stored, incoming, 0, stored_sha, incoming_sha)) {
		rel = CBS_SAME;
	} else {
		stored_sha[0] = '\0';
		incoming_sha[0] = '\0';
		if (cbs_lines_equal(stored, incoming, 1, stored_sha, incoming_sha))
			rel = CBS_COMMENTS;
	}
	if (rel != CBS_DIVERGENT && stored_sha[0] != '\0' && incoming_sha[0] != '\0' &&
	    strcmp(stored_sha, incoming_sha) != 0)
		rel = CBS_DIVERGENT;
	free(stored);
	return rel;
}

/*
 * What a dry run reports: the identity cbs derived for the recipe, and
 * its changelog, which is the reason a commit message can give.
 */
struct recipe_check {
	char version[PKG_VERSION_MAX];
	char changelog[PKG_CHANGELOG_MAX];
};

static int cbs_metadata_insert_point(const char *text, size_t *out_off);
static void approve_cbs_artifact(const char *recipe_path, const char *name, const char *version,
                                 const char *sha);

/* What a sync learned from publishing one recipe it carries (ADR-0324). */
struct sync_outcome {
	int refreshed; /* the stored text became git's */
	int writeback; /* this host holds an approval git's copy lacks */
	char version[PKG_VERSION_MAX];
};

/*
 * ADR-0324: git is authoritative for recipes, so a sync whose copy of a
 * version this host already holds differs only in comments, or carries
 * an approval this host lacks, replaces the stored text with git's --
 * the staged copy and its explain, written as a new version's are --
 * and queues nothing: no operation changed, so nothing rebuilds. An
 * approval this host holds and git's copy lacks is put back with
 * approve_cbs_artifact(), the function that wrote it in the first
 * place; git learns it by write-back, not by losing it here. A copy
 * with no `metadata {` block to carry it is not refreshed at all.
 */
static enum pkg_error recipe_cbs_refresh(const char *name, const char *version,
                                         const char *recipe_path, const char *staging_path,
                                         const char *explain_json, const char *incoming,
                                         const char *stored_sha, const char *incoming_sha)
{
	char explain_path[PATH_MAX];
	size_t off;
	int keep = stored_sha[0] != '\0' && incoming_sha[0] == '\0';

	if (keep && cbs_metadata_insert_point(incoming, &off) != 0) {
		unlink(staging_path);
		logstore_write("cixd", "warn",
		               "pkg: recipe %s@%s differs from git only in comments, but git's copy has "
		               "no `metadata {` line to carry this host's approval %.12s, so it was not "
		               "refreshed (ADR-0324)",
		               name, version, stored_sha);
		return PKG_ERR_DIVERGENT;
	}
	cbs_explain_path(recipe_path, explain_path, sizeof(explain_path));
	if (persist_atomic_write(explain_path, explain_json, strlen(explain_json)) != 0) {
		unlink(staging_path);
		return recipe_persist_failed("write the refreshed recipe's explain");
	}
	if (rename(staging_path, recipe_path) != 0) {
		unlink(staging_path);
		return recipe_persist_failed("move the refreshed recipe into place");
	}
	if (keep)
		approve_cbs_artifact(recipe_path, name, version, stored_sha);
	logstore_write("cixd", "info", "pkg: recipe %s@%s refreshed from git (%s); nothing rebuilds",
	               name, version,
	               incoming_sha[0] != '\0' && stored_sha[0] == '\0'
	                   ? "git carries an approval this host did not have"
	                   : "only comments differed");
	return PKG_OK;
}

/*
 * Publishing, or -- with `check` -- every test publishing applies and
 * nothing else (ADR-0323). A recipe is committed to git before it is
 * published, so it has to be known publishable first, and the only
 * honest answer to that is this function's own checks: a second copy
 * of them would drift from the first. A dry run stores nothing.
 */
static enum pkg_error recipe_publish(const char *name, const char *content,
                                     struct recipe_check *check, struct sync_outcome *sync)
{
	char *redacted;
	char name_dir[PATH_MAX];
	char version_dir[PATH_MAX];
	char staging_path[PATH_MAX];
	char recipe_path[PATH_MAX];
	char *explain_json = NULL;
	struct pkg_recipe parsed;
	struct stat st;

	g_recipe_add_err[0] = '\0';

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;

	if (persist_mkdir_p(g_recipes_dir) != 0)
		return recipe_persist_failed("create the recipe directory");

	/* Staged under g_recipes_dir itself (not yet inside any
	 * name/version subdirectory -- the version isn't known until the
	 * staged content is parsed below), same ".name.recipe.new"
	 * dotfile-hiding convention this always used.
	 *
	 * A CBS recipe's staging file keeps the .cbs extension, and that
	 * is not cosmetic: has_cbs_extension() in cbs refuses any other
	 * path outright, in `explain` as well as `build`, so a staging
	 * file without it cannot be read at all (ADR-0305). */
	if (snprintf(staging_path, sizeof(staging_path), "%s/.%s.recipe.new%s", g_recipes_dir, name,
	             PKG_RECIPE_CBS_SUFFIX) >= (int)sizeof(staging_path))
		return PKG_ERR_INVALID_NAME;
	/* #405: never store a live repo token. Everything below -- the
	 * write, the immutability check and the approval comparison --
	 * works on the redacted copy, so a caller that submits an expanded
	 * URL gets the same recipe a caller that submits the placeholder
	 * does, rather than a second, secret-bearing version of it. */
	redacted = strdup(content);
	if (redacted == NULL)
		return recipe_persist_failed("copy the recipe text");
	redact_repo_token(redacted, strlen(redacted) + 1);
	content = redacted;
	if (persist_atomic_write(staging_path, content, strlen(content)) != 0) {
		free(redacted);
		return recipe_persist_failed("write the staged recipe");
	}

	/*
	 * Identity, from `cbs explain` of the staged file. It cannot go
	 * through parse_recipe(), which reads the derived explain.json
	 * beside a build.cbs: at this moment there is no such file, and
	 * producing it is what this block does.
	 */
	{
		struct cbs_explain *ex;
		char err[512];
		size_t json_len = 0;

		explain_json = malloc(PKG_EXPLAIN_MAX);
		if (explain_json == NULL) {
			unlink(staging_path);
			free(redacted);
			return recipe_persist_failed("allocate the explain buffer");
		}
		if (run_cbs_explain(staging_path, explain_json, PKG_EXPLAIN_MAX, &json_len, err,
		                     sizeof(err)) != 0) {
			logstore_write("cixd", "error", "pkg: recipe %s rejected: %s", name, err);
			snprintf(g_recipe_add_err, sizeof(g_recipe_add_err), "%s", err);
			unlink(staging_path);
			free(redacted);
			free(explain_json);
			return PKG_ERR_INVALID_RECIPE;
		}
		ex = cbs_explain_parse(explain_json, json_len, err, sizeof(err));
		if (ex == NULL) {
			logstore_write("cixd", "error", "pkg: recipe %s rejected: %s", name, err);
			snprintf(g_recipe_add_err, sizeof(g_recipe_add_err), "%s", err);
			unlink(staging_path);
			free(redacted);
			free(explain_json);
			return PKG_ERR_INVALID_RECIPE;
		}
		memset(&parsed, 0, sizeof(parsed));
		snprintf(parsed.name, sizeof(parsed.name), "%s", cbs_explain_name(ex));
		if (cbs_explain_version(ex, parsed.version, sizeof(parsed.version)) != 0) {
			cbs_explain_free(ex);
			unlink(staging_path);
			free(redacted);
			free(explain_json);
			return PKG_ERR_INVALID_RECIPE;
		}
		/*
		 * Capabilities by NAME (cix-build-system#162, closed).
		 *
		 * This block used to refuse any recipe declaring one, because
		 * `cbs explain --json` reported only a count and a count of 1
		 * does not say whether the recipe asked for CAP_SYS_ADMIN or
		 * CAP_NET_ADMIN -- granting by count would have been worse
		 * than not supporting the field, and dropping it silently is
		 * the defect ADR-0304 was written about.
		 *
		 * A return of 1 means the installed cbs still answers with a
		 * count, so the refusal stays for exactly that case: cixd
		 * execs whichever cbs is on the host, and an older one turns
		 * a declared capability into none at all with nothing said.
		 */
		{
			int caps = cbs_explain_capabilities(ex, parsed.build_caps,
			                                     sizeof(parsed.build_caps));

			if (caps != 0) {
				logstore_write("cixd", "error",
				               "pkg: recipe %s: %s (cix-build-system#162) -- refusing "
				               "rather than building without the capabilities it "
				               "declares",
				               name,
				               caps == 1 ? "the installed cbs reports a capability "
				                            "COUNT rather than names; upgrade cbs"
				                          : "its declared capabilities do not fit");
				cbs_explain_free(ex);
				unlink(staging_path);
				free(redacted);
				free(explain_json);
				return PKG_ERR_INVALID_RECIPE;
			}
		}
		/*
		 * The declared artifact format, refused at the boundary when
		 * the engine cannot execute it (ADR-0307 clause 1).
		 *
		 * `cbs build` fails any recipe not declaring cixpkg with
		 * "standalone builds require cixpkg" (src/package.c:305), and
		 * the check is not conditional on an output path -- so
		 * `format "tar.gz"` is legal CPDL that this platform's only
		 * way of running a CBS recipe cannot execute. Storing such a
		 * recipe means an immutable version that fails at build time,
		 * every time; refusing it here costs the author one edit
		 * before anything is published.
		 *
		 * The value is READ rather than assumed, which is the whole
		 * reason the daemon needs it: assuming cixpkg would accept
		 * that recipe happily, and would bake in a restriction that
		 * is CBS's to lift rather than ours to encode.
		 */
		{
			const char *fmt = cbs_explain_format(ex);

			if (fmt[0] == '\0' || strcmp(fmt, PKG_ARTIFACT_FORMAT_CIXPKG) != 0) {
				snprintf(g_recipe_add_err, sizeof(g_recipe_add_err), "%s",
				         fmt[0] == '\0'
				             ? "the installed cbs reports no artifact format; it is older "
				               "than v0.1.26 (cix-build-system#173) and this daemon cannot "
				               "tell what the recipe declared"
				             : "it declares format \"tar.gz\", which `cbs build` refuses to "
				               "execute (\"standalone builds require cixpkg\") -- a CBS "
				               "recipe publishes a .cixpkg (ADR-0307)");
				logstore_write("cixd", "error", "pkg: recipe %s rejected: %s", name,
				               g_recipe_add_err);
				cbs_explain_free(ex);
				unlink(staging_path);
				free(redacted);
				free(explain_json);
				return PKG_ERR_INVALID_RECIPE;
			}
			snprintf(parsed.artifact_format, sizeof(parsed.artifact_format), "%s", fmt);
		}
		/*
		 * The two embedder-owned facts, out of the opaque metadata
		 * block (cix-build-system#161, closed). Both optional, and read
		 * here, at publish, so a refusal reaches the author before
		 * anything is stored.
		 *
		 * artifact_sha256 is what lets a host install a package from
		 * the cache instead of building it. Its shape is not validated
		 * here; parse_cbs_recipe() reads the same field from the same
		 * explain document at build time, and one rule in one place is
		 * what keeps the two from differing.
		 */
		if (cbs_explain_metadata(ex, "artifact_sha256", parsed.artifact_sha256,
		                          sizeof(parsed.artifact_sha256)) != 0 ||
		    cbs_explain_metadata(ex, "changelog", parsed.changelog,
		                          sizeof(parsed.changelog)) != 0) {
			/*
			 * #493: the caller gets this, not the generic parse
			 * sentence. Both things that sentence names are FALSE
			 * here -- the recipe parsed (cbs explain already ran on
			 * it and this code is reading the result) and the name
			 * matched -- and both are expensive to go and check, so
			 * it sends the author hunting for a CPDL syntax error
			 * that does not exist. It really happened, twice in one
			 * afternoon, converting xz and zlib with a changelog a
			 * few bytes over.
			 *
			 * The limits travel with the message because they are
			 * the actionable part and the author cannot see them
			 * from the recipe.
			 */
			snprintf(g_recipe_add_err, sizeof(g_recipe_add_err),
			         "a metadata value is too long (artifact_sha256 max %d bytes, changelog "
			         "max %d)",
			         PKG_SHA256_MAX - 1, PKG_CHANGELOG_MAX - 1);
			logstore_write("cixd", "error", "pkg: recipe %s: %s", name, g_recipe_add_err);
			cbs_explain_free(ex);
			unlink(staging_path);
			free(redacted);
			free(explain_json);
			return PKG_ERR_INVALID_RECIPE;
		}
		cbs_explain_free(ex);
		if (strcmp(parsed.name, name) != 0) {
			unlink(staging_path);
			free(redacted);
			free(explain_json);
			return PKG_ERR_INVALID_RECIPE;
		}
	}

	/* Immutability (ADR-0107): an already-published (name,version) is a
	 * real error, never a silent overwrite. */
	snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, name);
	snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, parsed.version);
	snprintf(recipe_path, sizeof(recipe_path), "%s/%s", version_dir, PKG_RECIPE_CBS_FILE);
	if (stat(recipe_path, &st) == 0) {
		char stored_sha[PKG_SHA256_MAX];
		char incoming_sha[PKG_SHA256_MAX];
		enum cbs_relation rel =
		    recipe_cbs_relation(recipe_path, redacted, stored_sha, incoming_sha);

		/*
		 * ADR-0324: the same version with a different meaning is not
		 * a duplicate, it is a conflict -- two places disagree about
		 * what one immutable version is. It is refused like a
		 * duplicate, and named differently, so a sync can report it
		 * rather than count it as a quiet skip.
		 *
		 * An approval reaches a published CPDL recipe through
		 * approve_cbs_artifact() after a build (#492), or through a
		 * sync refresh below -- never as a republish.
		 */
		if (sync != NULL) {
			/* An approval this host holds and git lacks goes back (ADR-0324). */
			snprintf(sync->version, sizeof(sync->version), "%s", parsed.version);
			sync->writeback = rel != CBS_DIVERGENT && stored_sha[0] != '\0' &&
			                  incoming_sha[0] == '\0';
		}
		/* Only a sync refreshes: git is the authority, and a publish
		 * on this host is not git (ADR-0324). */
		if (sync != NULL && check == NULL && rel != CBS_DIVERGENT &&
		    (rel == CBS_COMMENTS || (incoming_sha[0] != '\0' && stored_sha[0] == '\0'))) {
			enum pkg_error rerr =
			    recipe_cbs_refresh(name, parsed.version, recipe_path, staging_path,
			                       explain_json, redacted, stored_sha, incoming_sha);

			free(redacted);
			free(explain_json);
			if (rerr == PKG_OK)
				sync->refreshed = 1;
			return rerr;
		}
		free(redacted);
		free(explain_json);
		unlink(staging_path);
		if (rel != CBS_SAME) {
			logstore_write("cixd", "warn",
			               "pkg: recipe %s@%s refused -- that version is already "
			               "published with different content, and a version is one "
			               "recipe forever (ADR-0107, ADR-0324)",
			               name, parsed.version);
			return PKG_ERR_DIVERGENT;
		}
		return PKG_ERR_DUPLICATE;
	}
	free(redacted);

	/*
	 * #525: refuse an approval another revision of this package
	 * already declares.
	 *
	 * POSITION IS THE WHOLE OF THIS CHECK'S CORRECTNESS, measured:
	 *
	 * - AFTER the immutability block, so re-offering a recipe already
	 *   in the store still returns PKG_ERR_DUPLICATE. `recipe-sync`
	 *   calls this for every file in the corpus every six hours and
	 *   counted `added=28 skipped=379` on its last run. **86 of those
	 *   files carry a duplicated approval today** -- 73
	 *   probe-cix-testreport revisions, 10 cbs, 3 linux-headers --
	 *   so refusing before the immutability test would turn 86 quiet
	 *   skips into 86 errors, every window, forever. This is not a
	 *   hypothetical ordering risk; it is the state of the corpus
	 *   right now.
	 * - BEFORE the version directory is created, so a refusal leaves
	 *   nothing behind.
	 *
	 * It does NOT reach approve_cbs_artifact(), and it should not:
	 * that writes the sha a build just produced, at the one moment
	 * the bytes are known. A duplicate arriving that way would mean
	 * two revisions really did build identical bytes, which is not
	 * the defect this is for.
	 */
	{
		char other_version[PKG_VERSION_MAX];

		other_version[0] = '\0';
		if (approval_is_taken(name_dir, parsed.version, parsed.artifact_sha256, other_version,
		                       sizeof(other_version))) {
			snprintf(g_recipe_add_err, sizeof(g_recipe_add_err),
			         "this artifact_sha256 is already %s@%s's approval -- an approval names "
			         "one byte sequence from one build and is never carried forward. Drop "
			         "the line and let the build earn its own (ADR-0307 clause 5)",
			         name, other_version);
			logstore_write("cixd", "error",
			               "pkg: recipe %s@%s refused -- its artifact_sha256 %s is already "
			               "%s@%s's approval, so it describes bytes this version did not "
			               "produce (#525)",
			               name, parsed.version, parsed.artifact_sha256, name, other_version);
			unlink(staging_path);
			free(explain_json);
			return PKG_ERR_INVALID_RECIPE;
		}
	}

	/* A dry run ends here: every refusal above has been tested, and
	 * nothing below is a test, only the store. */
	if (check != NULL) {
		snprintf(check->version, sizeof(check->version), "%s", parsed.version);
		snprintf(check->changelog, sizeof(check->changelog), "%s", parsed.changelog);
		unlink(staging_path);
		free(explain_json);
		return PKG_OK;
	}

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
	{
		char explain_path[PATH_MAX];

		cbs_explain_path(recipe_path, explain_path, sizeof(explain_path));
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

enum pkg_error pkg_recipe_add(const char *name, const char *content)
{
	return recipe_publish(name, content, NULL, NULL);
}

/*
 * Re-derives the explain.json beside an already-written build.cbs
 * (ADR-0305).
 *
 * Exists for one caller: a system restore. A CBS recipe's identity is
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

	if (recipe_path == NULL || !recipe_path_is_cbs(recipe_path))
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
	cbs_explain_path(recipe_path, explain_path, sizeof(explain_path));
	if (persist_atomic_write(explain_path, json, strlen(json)) != 0)
		rc = PKG_ERR_PERSIST_FAILED;
	free(json);
	return rc;
}

enum pkg_error pkg_recipe_delete(const char *name, const char *version)
{
	char name_dir[PATH_MAX];
	/* #503: shared with pkg_recipe_add() via respond_pkg_recipe_error(),
	 * so it must not report that function's last message. */
	g_recipe_add_err[0] = '\0';

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

			cbs_explain_path(recipe_path, explain_path, sizeof(explain_path));
			unlink(explain_path);
		}
		if (unlink(recipe_path) != 0 || rmdir(version_dir) != 0)
			return recipe_persist_failed("remove the recipe version");
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

				cbs_explain_path(recipe_path, explain_path, sizeof(explain_path));
				unlink(explain_path);
				unlink(recipe_path);
			}
			rmdir(version_dir);
		}
		closedir(d);
		if (!found)
			return PKG_ERR_NOT_FOUND;
		if (rmdir(name_dir) != 0)
			return recipe_persist_failed("remove the recipe directory");
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

/*
 * #380: the recipe the CURRENT fetch resolved, for every stage after it.
 *
 * current_fetch_effective_version() answers "which version should this
 * fetch resolve", and for anything installed without a version that is
 * NULL -- "whatever is highest right now". start_fetch_for() resolved
 * it once. The stages after it -- fetch completion (the checksum),
 * build-environment completion and unpack completion -- each resolved
 * it AGAIN, so if a newer revision was published while a download ran
 * (the six-hourly recipe-sync does exactly that), they worked from a
 * different recipe than the fetch: a checksum failure that survives
 * any change of source url, or, when the new revision kept the source,
 * the new revision's steps built on the old revision's download while
 * pkg_build_completed() and the build log named the old one.
 * Reproduced on 192.168.15.95, 2026-09-29, with probe-slowfetch: 1-6
 * published while 1-5 was fetching sent staging looking for 1-6's
 * source file.
 *
 * So every stage after the fetch reads the FILE start_fetch_for()
 * resolved (fetch_resolved_recipe_path) -- the same principle #326
 * applied to the version string. Falls back to resolving only for a
 * slot that has not recorded one, which no stage after a fetch is.
 */
static int fetched_recipe_path(int chain_idx, const char *name, char *out, size_t out_size)
{
	const char *path = g_chains[chain_idx].fetch_resolved_recipe_path;

	if (path[0] != '\0') {
		if (snprintf(out, out_size, "%s", path) >= (int)out_size)
			return -1;
		return 0;
	}
	return find_recipe_path(name, current_fetch_effective_version(chain_idx), out, out_size);
}

/* ADR-0122: forward declarations -- defined below (near the end of this
 * file, alongside the rest of the local-cache/artifact-server module)
 * but needed here since start_fetch_for()/pkg_fetch_completed()/
 * pkg_build_completed() are the only call sites and all come first. */
static int pkg_cache_has(const char *name, const char *version);
static void pkg_cache_touch(const char *name, const char *version);
static void pkg_build_log_dir(char *out, size_t out_size); /* issue #57 */
static void pkg_build_log_open(struct pkg_entry *e, const char *version); /* issue #57 */
static void pkg_build_log_close(struct pkg_entry *e);
static void pkg_cache_save_from_file(const char *name, const char *version, const char *format,
                                      const char *src_path);
/* ADR-0307: defined with the rest of the cache-naming helpers further
 * down; called from pkg_prepare_build_and_start()'s cache-hit branch,
 * some five thousand lines above them, to find a cached artifact. */
static int cache_artifact_path_existing(const char *name, const char *version, char *out,
                                         size_t out_size);
/* Issue #139: defined further down; called from the install path's
 * unpack completion above it. */
static void warn_unexecutable_binaries(const char *root, const char *pkg_name, int depth,
                                        int *reported);
static int pkg_artifact_is_configured(void);
/* Issue #129: defined with the rest of the push machinery further
 * down; called from pkg_build_completed()'s own fresh-build branch. */
static void pkg_artifact_push_enqueue(const char *name, const char *version);
static void pkg_artifact_build_request(const struct pkg_repository *r, const char *name,
                                       const char *version, const char *format,
                                       char *out_url, size_t out_url_size, char *out_header,
                                       size_t out_header_size);
static void artifact_sentinel_path(int chain_idx, const char *name, const char *version,
                                   const char *format, char *out, size_t out_size);

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
	 * cache (its harvest branch writes nothing to it -- only the
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
	snprintf(g_chains[chain_idx].fetch_resolved_recipe_path,
	         sizeof(g_chains[chain_idx].fetch_resolved_recipe_path), "%s", recipe_path);
	snprintf(g_chains[chain_idx].fetch_resolved_depends,
	         sizeof(g_chains[chain_idx].fetch_resolved_depends), "%s", recipe.depends);
	snprintf(g_chains[chain_idx].fetch_resolved_replaces,
	         sizeof(g_chains[chain_idx].fetch_resolved_replaces), "%s", recipe.replaces);
	g_chains[chain_idx].fetch_resolved_build_memory = recipe.build_memory;
	/*
	 * ADR-0272: a run opens here, at the single place a job begins --
	 * so every atom of a chain gets one, not just the package that was
	 * asked for. What caused it is the CHAIN's, read rather than
	 * consumed: see struct pkg_chain's own run_trigger for the two
	 * ways consuming a module static here got it wrong.
	 */
	e->run_started_at = time(NULL);
	/*
	 * #424: this run's stage clocks start empty. An entry is reused across
	 * runs (an upgrade of an installed package is not memset), and a
	 * build_started_at left from the last run read as this run having
	 * started building while it was still fetching -- #423's own field
	 * included.
	 */
	e->build_started_at = 0;
	e->install_started_at = 0;
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

		/* #501: nothing this slot fetched before is needed any more,
		 * and nothing another slot is fetching is touched. */
		sources_clear_slot(chain_idx);
		fetch_error_sidecar_path(chain_idx, recipe.name, fetch_err_path, sizeof(fetch_err_path));
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
			char note_path[PATH_MAX];
			int ri;

			/*
			 * ADR-0324: every package repository, in order, each copy
			 * verified on its own. Order decides which copy arrives
			 * first and nothing else -- a copy that fails the checksum
			 * or the signature is refused and noted naming the
			 * repository, and the next one is tried. The note file
			 * collects one line per refusal, so a mirror serving wrong
			 * bytes is reported even when the next mirror succeeds.
			 */
			fetch_note_sidecar_path(chain_idx, recipe.name, note_path, sizeof(note_path));
			unlink(note_path);
			for (ri = 0; ri < pkgrepo_count(); ri++) {
				const struct pkg_repository *repo = pkgrepo_at(ri);
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

				pkg_artifact_build_request(repo, recipe.name, recipe.version, recipe.artifact_format,
				                            artifact_url, sizeof(artifact_url), artifact_header,
				                            sizeof(artifact_header));
				artifact_sentinel_path(chain_idx, recipe.name, recipe.version, recipe.artifact_format,
				                        artifact_path, sizeof(artifact_path));
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
					int nfd;

					nfd = open(note_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
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
							              "repository %s served %s@%s with the wrong checksum "
							              "(recipe approves %.16s..., it served %.16s...) -- "
							              "not used; the next repository, then the source, is "
							              "tried. If this recipe version is already "
							              "published, an edited pkg_artifact_sha256 cannot "
							              "take effect: bump pkg_version instead\n",
							              repo->name, recipe.name, recipe.version,
							              recipe.artifact_sha256, sha_out);
						else
							n = snprintf(note, sizeof(note),
							              "repository %s served %s@%s with a signature that "
							              "did not verify against this host's trusted keys -- "
							              "not used; the next repository, then the source, is "
							              "tried. Either it was published unsigned, or by a "
							              "key this host does not trust: `cixctl pkg sync` "
							              "adopts the keys a trusted source publishes in "
							              "docs/keys/\n",
							              repo->name, recipe.name, recipe.version);
						if (n > 0) {
							ssize_t written = write(nfd, note, (size_t)n);

							(void)written;
						}
						close(nfd);
					}
				}
				unlink(artifact_path); /* not found / wrong checksum -- discard, fall through */
			}
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
		 * say so, which is one `pkg source set NAME --token=` away
		 * from fixed. Hit live on a real host, where the fetch had
		 * been failing this way with nothing naming the cause.
		 */
		for (j = 0; j < recipe.source_count + recipe.mirror_count; j++) {
			/* Mirrors are checked alongside the sources, not after
			 * them: a placeholder left in a mirror would surface
			 * only once the primary failed, i.e. on the path nobody
			 * exercises until it matters (#507). */
			const char *url_to_check = j < recipe.source_count
			                               ? recipe.source[j]
			                               : recipe.mirror_url[j - recipe.source_count];

			if (strstr(url_to_check, "{{REPO_TOKEN}}") != NULL) {
				static const char msg[] =
				    "pkg_source needs a repo token, and the source that owns this package has "
				    "none (or no source owns it) -- set one with `cixctl pkg source set NAME --token=...`";
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
			char url_shown[PKG_URL_MAX];
			/*
			 * Every url tried for this source, each with its own
			 * failure (#507).
			 *
			 * 512 is not a guess: pkg_fetch_completed() reads this
			 * sidecar back into a `char detail[512]`, so a larger
			 * buffer here would write bytes that are silently
			 * dropped on the way to the operator -- the tail, which
			 * is where the last url's failure is. Matching the
			 * reader means nothing is written that cannot be read.
			 * Beyond four or five mirrors the tail does truncate;
			 * the full text is in the build log either way.
			 */
			char fetch_err[512];
			size_t err_len;
			int attempt;
			int fetched;
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
			source_download_path(chain_idx, recipe.name, recipe.version, j, src_tarball_path,
			                     sizeof(src_tarball_path));

			/*
			 * source[j] first, then every mirror declared for it,
			 * in the order the recipe gave them (#507). CPDL calls
			 * these ordered mirrors for one source identity sharing
			 * one checksum, and that shared checksum is what makes
			 * trying another host safe: pkg_fetch_completed()
			 * verifies whatever arrives against the recipe's own
			 * sha256 regardless of which url produced it, so a
			 * mirror cannot substitute different bytes without
			 * failing a gate that already exists.
			 *
			 * Before this, only source[j] was ever tried and the
			 * remaining urls were parsed and dropped, so a declared
			 * mirror list did nothing at all -- freetype@2.13.3-7
			 * declared three and failed naming the first.
			 */
			attempt = 0;
			fetched = 0;
			fetch_err[0] = '\0';
			for (;;) {
				const char *url;
				int k;
				int seen = 0;

				if (attempt == 0) {
					url = recipe.source[j];
				} else {
					url = NULL;
					for (k = 0; k < recipe.mirror_count; k++) {
						if (recipe.mirror_source[k] != j)
							continue;
						if (++seen == attempt) {
							url = recipe.mirror_url[k];
							break;
						}
					}
					if (url == NULL)
						break;
				}
				attempt++;

				/*
				 * resume only makes sense continuing *this*
				 * attempt's own partial download (recovering from
				 * a transient mid-transfer reset within
				 * curlfetch_perform()'s own retry loop). A stale
				 * file left at this same fixed path -- by an
				 * earlier fetch attempt, or by the url this loop
				 * just tried -- makes it request a byte range
				 * starting past EOF, which the server correctly
				 * answers with HTTP 416; confirmed the hard way
				 * (ADR-0056), every retry hit the identical 416
				 * since the file never changed. Every attempt
				 * starts from a clean slate; ENOENT is expected
				 * and fine.
				 */
				unlink(src_tarball_path);

				memset(&opts, 0, sizeof(opts));
				opts.url = url;
				opts.path = src_tarball_path;
				opts.connect_timeout = PKG_CURL_CONNECT_TIMEOUT_SECS;
				opts.low_speed_limit = PKG_CURL_LOW_SPEED_LIMIT;
				opts.low_speed_time = PKG_CURL_LOW_SPEED_TIME_SECS;
				opts.retry_count = 8;
				opts.retry_all_errors = 1;
				opts.retry_delay = 3;
				opts.resume = 1;
				if (curlfetch_perform(&opts, NULL, curl_err, sizeof(curl_err)) == 0) {
					fetched = 1;
					break;
				}
				/* A url can carry the real repo token (gitea's
				 * token-in-URL convention) after {{REPO_TOKEN}}
				 * substitution -- redact both the error text and
				 * the url before either reaches a sidecar file,
				 * same discipline as pkg_sync_start's own child
				 * (#405/#410). */
				redact_repo_token(curl_err, sizeof(curl_err));
				redact_url_userinfo(curl_err, sizeof(curl_err));
				snprintf(url_shown, sizeof(url_shown), "%s", url);
				redact_repo_token(url_shown, sizeof(url_shown));
				redact_url_userinfo(url_shown, sizeof(url_shown));
				/*
				 * Every failed url is named, not just the last.
				 * A fallback list whose failures collapse into one
				 * message is how a mirror that is merely
				 * misspelled reads exactly like one that is down.
				 * Bounded by the buffer: a truncated tail is
				 * acceptable, an unbounded sidecar is not.
				 */
				err_len = strlen(fetch_err);
				snprintf(fetch_err + err_len, sizeof(fetch_err) - err_len,
				         "%s%s: %s", err_len > 0 ? "; " : "", url_shown, curl_err);
			}
			if (!fetched) {
				int fd;

				fd = open(fetch_err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
				if (fd >= 0) {
					ssize_t written = write(fd, fetch_err, strlen(fetch_err));

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
	/*
	 * NO CHECK THAT THE IMAGE EXISTS HERE, and that is deliberate
	 * (#500). The refusal of an unknown image lives in
	 * handle_pkg_install(), the one place a person names an image:
	 * every other caller of this function -- dependencies, hostbuild,
	 * build environments, image-recipe apply, the rolling drain --
	 * passes an image the daemon chose, not one a person could have
	 * misspelled.
	 *
	 * The history, because the first attempt got it wrong. A check was
	 * written HERE on 2026-09-27, on the strength of #500's stated
	 * cause -- that `--image=jump` for `jumpbox` failed at "could not
	 * unpack (extract cached artifact failed)" because the image was
	 * absent. That cause is false: dest_dir is built under the BUILD
	 * CONTAINER's own upperdir, not under the image, and on
	 * 192.168.15.95 on 2026-09-27 `pkg install --name=ncurses
	 * --image=nosuchimage-probe` reached `installed` and `image ls`
	 * then listed the new image. So an absent image was never a
	 * failure; it was an implicit create, and the check failed every
	 * test that installed into an image it had not created (imgA-imgD
	 * in test_pkg_cache; router, vnew, vold and libcollide in
	 * test_pkg). It was reverted. On 2026-09-30 the owner decided the
	 * implicit create should go -- one way to make an image,
	 * POST /v1/images -- so those tests now create their images first
	 * and the request is refused at the API.
	 */
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
	/* cix#558: counted as running before the budget is applied, so a
	 * declared need is in the budget this container starts under. */
	pkg_build_memory_note_start(chain_idx,
	                            e->cache_hit ? 0 : g_chains[chain_idx].fetch_resolved_build_memory);
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
 * already owns /build/extra in the build container -- this file is
 * the only thing put there (cix#569) -- writes the file
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

/*
 * Starts unpacking a cached CIXPKG into dest_dir, in a forked child
 * (ADR-0307 clause 3).
 *
 * Forked rather than run here, and that is the whole design rather
 * than a detail. cixd never links CBS (ADR-0305) and never parses its
 * formats, so reading a .cixpkg means running `cbs extract` -- and
 * this function is reached from the install pipeline, which runs on
 * the single epoll loop that serves every request. A wait here would
 * stop the control plane for as long as it takes to write out a
 * package, which for glibc is not a moment. So the child is tracked
 * by pidfd like every other child this daemon starts, and
 * pkg_unpack_completed() resumes the install when it exits.
 *
 * The tarball path is deliberately NOT changed to match: libarchive
 * is a library, so that extraction has no child to wait on and
 * nothing to make asynchronous. One dispatch point, two mechanisms,
 * because the two formats differ in exactly that way.
 *
 * dest_dir is REMOVED before the fork, not after: cbs_cixpkg_extract()
 * refuses a destination that already exists, and the caller has just
 * created it. rmdir() only succeeds on an empty directory, so this
 * cannot quietly discard a populated tree -- if anything is in there,
 * the unpack does not start and says so.
 */
static int cixpkg_unpack_start(const char *artifact, const char *dest_dir, pid_t *out_pid,
                                int *out_pidfd)
{
	struct stat st;
	char parent[PATH_MAX];
	char *slash;
	pid_t pid;
	int pidfd;
	int dest_existed;
	int rmdir_errno = 0;
	int parent_is_dir;

	*out_pid = -1;
	*out_pidfd = -1;

	/*
	 * Each refusal below says which one it was. They all returned a
	 * bare -1, which the caller renders as "start the cixpkg unpack"
	 * -- one message for a missing engine, a destination that would
	 * not go away, and a fork that failed, which are three different
	 * things to fix.
	 */
	if (access(PKG_CBS_BIN, X_OK) != 0) {
		logstore_write("cixd", "error",
		                "cixpkg unpack: %s is not executable, and nothing else reads this "
		                "format (ADR-0307 clause 6)",
		                PKG_CBS_BIN);
		return -1;
	}

	dest_existed = stat(dest_dir, &st) == 0;
	if (rmdir(dest_dir) != 0) {
		rmdir_errno = errno;
		if (rmdir_errno != ENOENT) {
			logstore_write("cixd", "error",
			                "cixpkg unpack: rmdir(\"%s\") failed: %s -- cbs refuses a "
			                "destination that already exists, so it is not started",
			                dest_dir, strerror(rmdir_errno));
			return -1;
		}
	}

	/*
	 * #485, and cix-build-system#233 is why this is worth a line of
	 * its own: `cbs extract` answers a DESTINATION fault with
	 * "<artifact>: error[CIXPKG-E4001]: artifact extraction failed",
	 * naming the artifact. Measured on 192.168.15.95, 2026-09-25
	 * (probe-cixpkg-extract@4): a destination whose parent is missing
	 * and a destination that already exists produce that same line and
	 * the same exit 4, and so, presumably, does an artifact that
	 * genuinely cannot be read. The daemon keeps the child's stderr and
	 * nothing else, so a reader gets a message about bytes that are
	 * usually fine -- three probes went into the artifact before the
	 * destination was suspected at all.
	 *
	 * So the state cbs is about to see is recorded here, where it is
	 * still knowable, rather than inferred afterwards from a message
	 * that points the wrong way. It stays once cbs#233 is fixed: the
	 * fork loses this context either way.
	 */
	snprintf(parent, sizeof(parent), "%s", dest_dir);
	slash = strrchr(parent, '/');
	if (slash != NULL && slash != parent)
		*slash = '\0';
	parent_is_dir = stat(parent, &st) == 0 && S_ISDIR(st.st_mode);
	logstore_write("cixd", "info",
	                "cixpkg unpack: %s -> %s: artifact %s, destination %s, rmdir %s, parent "
	                "\"%s\" %s",
	                artifact, dest_dir, access(artifact, R_OK) == 0 ? "readable" : "NOT READABLE",
	                dest_existed ? "existed" : "was already absent",
	                rmdir_errno == 0 ? "removed it" : strerror(rmdir_errno), parent,
	                parent_is_dir ? "is a directory" : "IS NOT A DIRECTORY");

	pid = fork();
	if (pid < 0) {
		logstore_write("cixd", "error", "cixpkg unpack: fork failed: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		char *argv[] = { (char *)PKG_CBS_BIN, (char *)"extract", (char *)artifact,
		                 (char *)"--into", (char *)dest_dir, NULL };

		execve(PKG_CBS_BIN, argv, environ);
		_exit(127);
	}
	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		logstore_write("cixd", "error",
		                "cixpkg unpack: pidfd_open on the cbs child failed: %s -- killed it "
		                "rather than leave it untracked",
		                strerror(errno));
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return -1;
	}
	*out_pid = pid;
	*out_pidfd = pidfd;
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
	char dest_dir[PATH_MAX], recipe_dst[PATH_MAX], extra_dir[PATH_MAX];
	int i;

	*out_compose_pid = -1;
	*out_compose_pidfd = -1;

	/*
	 * NO SHELL-RECIPE REFUSAL HERE, deliberately, and it was tried.
	 *
	 * A refusal at the top of this function reads as the obvious place
	 * -- it is the one point every build passes through. It is wrong,
	 * and the existing refusal further down says why: it sits in the
	 * `else` of the CACHE-HIT arm, so a shell package whose artifact
	 * is already cached still installs. That path never needed a build
	 * command at all (it gets `:`), and refusing it up here would
	 * strand every host holding a cached shell artifact -- which,
	 * measured on 192.168.15.95, is most of the toolchain: gcc,
	 * glibc, kernel, python, binutils and elfutils are all installed
	 * from a shell revision today.
	 *
	 * ADR-0309 clause 3 retires BUILDING from a shell recipe, not
	 * installing what one already built.
	 */

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

	/*
	 * cix#558: a build that declares more memory than the operator's
	 * ceiling allows is refused before anything is composed for it,
	 * naming both numbers and the command that changes the ceiling. A
	 * cache hit builds nothing, so what it would have needed does not
	 * matter.
	 */
	if (!e->cache_hit) {
		char refused[PKG_ERROR_MAX];

		if (build_memory_admit(recipe.build_memory, refused, sizeof(refused)) != 0) {
			pkg_fail(e, is_final_upgrade, PIPELINE_BUILD, "%s", refused);
			logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
		}
	}

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
		 * A CBS recipe does not declare the engine that runs it
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
		if (append_words(declared, sizeof(declared), "cbs") != 0) {
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
		         "recipe declares no build requirements (requires { build { ... } }) -- every build environment is composed "
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

	snprintf(dest_dir, sizeof(dest_dir), "%s/%s", e->build_upperdir, PKG_DEST_REL_CBS);
	/* ADR-0305: cbs refuses any recipe path not ending in .cbs
	 * (has_cbs_extension(), in `build` as well as `explain`), so the
	 * name this is staged under is load-bearing rather than cosmetic --
	 * a build.cbs staged under any other name could not be built. */
	snprintf(recipe_dst, sizeof(recipe_dst), "%s/build/recipe" PKG_RECIPE_CBS_SUFFIX,
	         e->build_upperdir);
	snprintf(extra_dir, sizeof(extra_dir), "%s/build/extra", e->build_upperdir);

	{
		char tmp_dir[PATH_MAX];

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
		 * the same "just make sure it exists" posture the destination
		 * directory already has -- benefits every future
		 * recipe run against a minimal image, not just this one.
		 */
		snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", e->build_upperdir);

		/*
		 * A sequential if/else-if chain rather than the equivalent
		 * single ||-chain condition -- functionally identical, but
		 * this way pkg_fetch_completed() itself knows (and can log)
		 * exactly which sub-step failed instead of
		 * bucketing all of them into one opaque message. Every step
		 * is in-process, and the errno each sets is captured
		 * immediately after each one's own failing call so nothing
		 * else can clobber it first.
		 */
		{
			const char *prep_step = NULL;
			int prep_errno = 0;
			/*
			 * ADR-0256: unpacking is its OWN stage, not
			 * part of the build. A cached .cixpkg that downloaded intact
			 * and cannot be opened used to report as a build failure,
			 * which sends a reader to a compile log for something
			 * that happened before any compiler ran. Every other step
			 * here really is build-container setup.
			 */
			enum pipeline_stage prep_stage = PIPELINE_BUILD;

			if (g_chains[chain_idx].unpacked) {
				/*
				 * Re-entered after the cixpkg unpack child exited
				 * (ADR-0307 clause 3), and every step below has
				 * already run: the container directory was reset,
				 * dest_dir and tmp_dir were created, the child
				 * filled dest_dir, and pkg_unpack_completed() ran
				 * the #139 usability check over the result.
				 *
				 * FIRST in the chain, and that placement is the
				 * point rather than tidiness:
				 * reset_build_container_dir() is what the old
				 * first branch does, and on a re-entry it would
				 * delete the tree the child just extracted --
				 * leaving an empty package that installs cleanly.
				 */
			} else if (reset_build_container_dir(container_base) != 0) {
				prep_step = "reset build container dir";
				/* A filesystem operation, so errno names the cause --
				 * it was discarded here, and the message then pointed
				 * at a subprocess this step never ran (#504). */
				prep_errno = errno;
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
				char cached[PATH_MAX];

				(void)cache_artifact_path_existing(e->name, recipe.version, cached,
				                                    sizeof(cached));
				/*
				 * ADR-0307 clause 3: the point where bytes become a
				 * tree. The cached artifact is a .cixpkg, the only
				 * format looked for (cix#569), and cbs reads it.
				 */
				if (access(PKG_CBS_BIN, X_OK) != 0) {
					/*
					 * ADR-0307 clause 6's other half: refused by
					 * name, never by falling through to an
					 * extractor that cannot be correct.
					 *
					 * Its own failure rather than a prep_step,
					 * because the generic template would render
					 * this as "unpacking (... failed)" and bury
					 * the one thing an operator needs -- which
					 * install fixes it. Clause 6 makes this
					 * unreachable on an assembled root, since
					 * mkbootroot now refuses to seal one without
					 * cbs; it stays because "unreachable" is a
					 * claim about today's assembly path and this
					 * is a claim about what the daemon does.
					 */
					logstore_write("cixd", "error",
					                "pkg %s@%s: %s is a .cixpkg and this host has no CPDL "
					                "engine at %s -- nothing else reads that format. Fix "
					                "it with: pkg install --image=cix-hosttools cbs, then "
					                "reassemble and reboot the control plane (ADR-0307 "
					                "clause 6)",
					                e->name, g_chains[chain_idx].image, cached, PKG_CBS_BIN);
					pkg_fail(e, is_final_upgrade, PIPELINE_UNPACK,
					         "the cached artifact is a .cixpkg and this host has no %s to "
					         "read it with",
					         PKG_CBS_BIN);
					g_chains[chain_idx].name[0] = '\0';
					g_chains[chain_idx].dep_queue_count = 0;
					return 0;
				} else {
					if (cixpkg_unpack_start(cached, dest_dir, out_compose_pid,
					                         out_compose_pidfd) != 0) {
						prep_step = "start the cixpkg unpack";
						prep_stage = PIPELINE_UNPACK;
					} else {
						/*
						 * 3, not 2: the same out-parameters carry
						 * the child either way, and the caller
						 * tells them apart by this value so it can
						 * register the right completion. The
						 * install resumes in
						 * pkg_unpack_completed().
						 */
						return 3;
					}
				}
			} else if (copy_file_simple(recipe_path, recipe_dst) != 0) {
				prep_step = "copy recipe";
				prep_errno = errno;
			} else if (write_finalize_script(e->build_upperdir) != 0) {
				/* ADR-0251: the policy is not optional, so a build
				 * that could not be given it does not run. */
				prep_step = "write finalize.sh";
				prep_errno = errno;
			}

			if (prep_step != NULL) {
				/*
				 * The destination, named (#500). "extract cached
				 * artifact failed" points at the artifact and at
				 * the unpacker, and every one of these steps has
				 * two operands -- the thing being read and the
				 * place it is going -- while the message named
				 * only the first. That is the same shape as
				 * CIXPKG-E4001 and cbs's `copy`, both of which
				 * report a failure against the operand that is
				 * fine; each cost a round of investigation
				 * against the wrong subsystem.
				 */
				if (prep_errno != 0)
					logstore_write("cixd", "error",
					                "pkg %s@%s: could not prepare build container (%s, into %s): %s",
					                e->name, g_chains[chain_idx].image, prep_step, dest_dir,
					                strerror(prep_errno));
				else
					/*
					 * #504: this used to say "see run_subprocess
					 * detail above", and for eight days that sent the
					 * reader to a line that was never written. NONE of
					 * the steps that reach here is a subprocess any
					 * more -- extraction, staging and the container
					 * reset all became in-process in the #352/#410/
					 * #411 shell-out audits, and the message was not
					 * updated with them. The real reason was logged
					 * the whole time, one line up, by
					 * extract_archive_to():
					 *
					 *   extract .../glibc-2.44-16.tar.gz: write header
					 *   for ".../POSIX_V6_LP64_OFF64" failed:
					 *   Hard-link target './usr/bin/getconf' does not
					 *   exist.
					 *
					 * which is #506's bug, fixed three hours after
					 * #504 was filed, under a different number, with
					 * nobody connecting the two.
					 *
					 * So this names no mechanism. Every step that
					 * lands here now logs its own reason before
					 * returning (cixpkg_unpack_start());
					 * the ones whose failure is
					 * a plain syscall set prep_errno instead and take
					 * the branch above.
					 */
					logstore_write("cixd", "error",
					                "pkg %s@%s: could not prepare build container (%s, into %s) -- "
					                "the step's own error is logged immediately above",
					                e->name, g_chains[chain_idx].image, prep_step, dest_dir);
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
	if (!e->cache_hit) {
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
		snprintf(cbs_ws_dir, sizeof(cbs_ws_dir), "%s/%s", e->build_upperdir,
		         PKG_CBS_WORKSPACE_REL);
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
		snprintf(cbs_cache_dir, sizeof(cbs_cache_dir), "%s/%s", e->build_upperdir,
		         PKG_CBS_CACHE_REL);
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

			source_download_path(chain_idx, e->name, recipe.version, i, src_path, sizeof(src_path));
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
	 * container's own PKG_DEST_REL_CBS) was already populated straight
	 * from the cache above, nothing left for the container itself to
	 * do. */
	if (e->cache_hit)
		snprintf(e->build_argv_cmd, sizeof(e->build_argv_cmd), ":");
	else
		/* See PKG_CBS_WORKSPACE for why the cache is pre-filled and
		 * why the finalize policy is CBS's own --finalize-command,
		 * and PKG_CBS_ARTIFACT for where --output writes. */
		snprintf(e->build_argv_cmd, sizeof(e->build_argv_cmd),
		         "set -e; cbs build /build/recipe.cbs --arch %s --staged %s --cache %s "
		         "--output %s --finalize-command /build/finalize.sh --events human%s",
		         pkg_host_arch(), PKG_CBS_WORKSPACE, PKG_CBS_CACHE_DIR, PKG_CBS_ARTIFACT,
		         g_chains[chain_idx].hostbuild_extra_config_symbols[0] != '\0'
		                 ? " --input " PKG_CBS_KMOD_EXTRA_INPUT
		                 : "");
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
	snprintf(e->build_dest_rel, sizeof(e->build_dest_rel), "%s", PKG_DEST_REL_CBS);
	snprintf(e->artifact_format, sizeof(e->artifact_format), "%s", recipe.artifact_format);
	snprintf(e->build_destdir_env, sizeof(e->build_destdir_env), "PKG_DESTDIR=/%s",
	         e->build_dest_rel);
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
		char note[4096]; /* ADR-0324: one line per repository that served a bad copy */
		int nfd;

		fetch_note_sidecar_path(chain_idx, e->name, note_path, sizeof(note_path));
		nfd = open(note_path, O_RDONLY);
		if (nfd >= 0) {
			ssize_t n = read(nfd, note, sizeof(note) - 1);

			close(nfd);
			unlink(note_path);
			if (n > 0) {
				char *line, *save = NULL;

				note[n] = '\0';
				for (line = strtok_r(note, "\n", &save); line != NULL;
				     line = strtok_r(NULL, "\n", &save))
					logstore_write("cixd", "warn", "pkg %s@%s: %s", e->name, e->image, line);
			}
		}
	}

	if (exit_status != 0) {
		char fetch_err_path[PATH_MAX];
		char detail[512];
		int detail_len = 0;
		int fd;

		detail[0] = '\0';
		fetch_error_sidecar_path(chain_idx, e->name, fetch_err_path, sizeof(fetch_err_path));
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
		else {
			/* #503: a fetch into a partition with no room fails as a
			 * bare curl exit 1, which reads exactly like the
			 * documented flaky-mirror class -- and was filed as one. */
			char disk[256];

			disk_pressure_note(disk, sizeof(disk));
			if (detail_len > 0)
				pkg_fail(e, is_final_upgrade, PIPELINE_FETCH,
				         "fetch failed (curl exit status %d): %s%s", exit_status, detail, disk);
			else
				pkg_fail(e, is_final_upgrade, PIPELINE_FETCH,
				         "fetch failed (curl exit status %d)%s", exit_status, disk);
		}
		logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	if (fetched_recipe_path(chain_idx, e->name, recipe_path, sizeof(recipe_path)) != 0 ||
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

		artifact_sentinel_path(chain_idx, e->name, recipe.version, recipe.artifact_format,
		                        artifact_sentinel, sizeof(artifact_sentinel));
		if (stat(artifact_sentinel, &st) == 0 && S_ISREG(st.st_mode)) {
			pkg_cache_save_from_file(e->name, recipe.version, recipe.artifact_format,
			                          artifact_sentinel);
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

			source_download_path(chain_idx, e->name, recipe.version, i, src_path, sizeof(src_path));
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
				redact_url_userinfo(url_redacted, sizeof(url_redacted));
				logstore_write("cixd", "error",
				                "pkg %s@%s: source %d %s. path=%s bytes=%lld computed=%s "
				                "declared=%s url=%s",
				                e->name, recipe.version, i, why, src_path,
				                (long long)sst.st_size,
				                sha_out[0] != '\0' ? sha_out : "(none)", recipe.sha256[i],
				                url_redacted);
				/*
				 * #513: the FULL computed hash, in the message. It is
				 * the one value needed to re-pin a recipe, and a
				 * 12-character prefix of it cannot be written into one.
				 * It leads because a run record keeps 192 bytes of
				 * this and the tail is what gets cut; the declared
				 * hash is already in the recipe, so a prefix of it is
				 * enough to recognise, and path and url are in the
				 * log line above, whose address is named exactly.
				 */
				pkg_fail(e, is_final_upgrade, PIPELINE_FETCH,
				         "checksum mismatch (source %d): %lld bytes hash to %s, recipe declares "
				         "%.12s... -- path and url: GET /v1/system/logs?source=cixd",
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

	if (fetched_recipe_path(chain_idx, e->name, recipe_path, sizeof(recipe_path)) != 0 ||
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
 * Reaps the cixpkg unpack child and resumes the install it suspended
 * (ADR-0307 clause 3).
 *
 * The same shape as pkg_buildenv_completed() above, because it is the
 * same kind of thing: a step that had to be a child so the reactor
 * would not stop, and whose completion continues a chain. The one
 * difference is what is checked afterwards -- a composition produces
 * an image, this produces a tree, and a tree gets the #139 usability
 * check that a checksum cannot give.
 */
int pkg_unpack_completed(int chain_idx, int exit_status, struct container_spec *spec_out,
                          int *out_stdio_write_fd, pid_t *out_compose_pid,
                          int *out_compose_pidfd)
{
	struct pkg_entry *e = pkg_find(g_chains[chain_idx].name, g_chains[chain_idx].image);
	char recipe_path[PATH_MAX];
	struct pkg_recipe recipe;
	int is_final_upgrade;

	*out_compose_pid = -1;
	*out_compose_pidfd = -1;

	/* The same stale-slot discriminator its two siblings use (#98):
	 * an entry that is no longer FETCHING cannot be the one whose
	 * unpack just exited. */
	if (e == NULL || e->state != PKG_STATE_FETCHING) {
		chain_release_if_job_over(chain_idx, e);
		return 0;
	}

	is_final_upgrade = g_chains[chain_idx].dep_queue_is_upgrade &&
	                   (g_chains[chain_idx].dep_queue_pos + 1 >= g_chains[chain_idx].dep_queue_count);

	if (exit_status != 0) {
		/*
		 * PIPELINE_UNPACK, not PIPELINE_BUILD (ADR-0256): an
		 * artifact that arrived intact and could not be opened is
		 * not a build failure, and reporting it as one sends a
		 * reader to a compile log for something that happened
		 * before any compiler ran.
		 *
		 * 127 is worth distinguishing because it is the one cause
		 * an operator can act on directly: cbs is what reads this
		 * format, and a host without it cannot install a CBS
		 * package at all (ADR-0307 clause 6).
		 */
		if (exit_status == 127)
			pkg_fail(e, is_final_upgrade, PIPELINE_UNPACK,
			         "could not run %s to unpack the cached artifact -- this host has no CPDL "
			         "engine, and nothing else reads a .cixpkg",
			         PKG_CBS_BIN);
		else
			pkg_fail(e, is_final_upgrade, PIPELINE_UNPACK,
			         "unpacking the cached artifact failed (cbs extract exited %d)",
			         exit_status);
		logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	if (fetched_recipe_path(chain_idx, e->name, recipe_path, sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0) {
		pkg_fail(e, is_final_upgrade, PIPELINE_BUILD,
		         "the recipe became unreadable while its artifact was unpacked");
		logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	/*
	 * Issue #139: a checksum proves an artifact is intact, never that
	 * it is usable. Run here rather than left to the caller because
	 * this is where the tree first exists -- the same point the
	 * tarball path runs it, just in a different function.
	 *
	 * A CIXPKG checks a digest per file and refuses setuid modes and
	 * non-root ownership on its way out (ADR-0307 clause 4), which is
	 * strictly more than a tarball offers -- and none of that answers
	 * "is this binary executable", which is what #139 was about.
	 */
	{
		char dest_dir[PATH_MAX];
		int reported = 0;

		snprintf(dest_dir, sizeof(dest_dir), "%s/%s", e->build_upperdir,
		         e->build_dest_rel[0] != '\0' ? e->build_dest_rel : PKG_DEST_REL_CBS);
		warn_unexecutable_binaries(dest_dir, e->name, 0, &reported);
	}

	g_chains[chain_idx].unpacked = 1;
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

	/*
	 * cix#558: the same admission an ordinary build gets in
	 * pkg_prepare_build_and_start(), which a resume does not pass
	 * through. The ceiling may have been lowered since the build that
	 * left this container behind was admitted.
	 */
	{
		char refused[PKG_ERROR_MAX];

		if (build_memory_admit(recipe.build_memory, refused, sizeof(refused)) != 0) {
			logstore_write("cixd", "error", "pkg resume %s@%s: %s", name, recipe.version,
			               refused);
			return PKG_ERR_BUILD_MEMORY_OVER_CEILING;
		}
	}

	snprintf(recipe_dst, sizeof(recipe_dst), "%s/build/recipe%s", e->build_upperdir,
	         PKG_RECIPE_CBS_SUFFIX);
	snprintf(dest_dir, sizeof(dest_dir), "%s/%s", e->build_upperdir, PKG_DEST_REL_CBS);
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
	snprintf(g_chains[chain_idx].fetch_resolved_recipe_path,
	         sizeof(g_chains[chain_idx].fetch_resolved_recipe_path), "%s", recipe_path);
	snprintf(g_chains[chain_idx].fetch_resolved_depends,
	         sizeof(g_chains[chain_idx].fetch_resolved_depends), "%s", recipe.depends);
	snprintf(g_chains[chain_idx].fetch_resolved_replaces,
	         sizeof(g_chains[chain_idx].fetch_resolved_replaces), "%s", recipe.replaces);
	g_chains[chain_idx].fetch_resolved_build_memory = recipe.build_memory;

	/*
	 * The build command depends on the recipe's LANGUAGE, exactly as
	 * it does on the install path -- this branch was missing, and
	 * resuming a CPDL build ran `/build/recipe.sh` and died with
	 * "/usr/bin/bash: line 1: /build/recipe.sh: No such file or
	 * directory" (measured on 192.168.15.95, 2026-09-26, log
	 * resumeme-1.1-1-1790405232).
	 *
	 * It was invisible for as long as it existed because every
	 * fixture that exercises resume was written in shell; converting
	 * them (cix#516) is what surfaced it.
	 *
	 * No cache-hit arm, unlike the install path: a resume exists
	 * precisely because a build failed, so there is nothing cached to
	 * short-circuit to.
	 */
	/* Unconditional: a recipe is CPDL, and a path that is not one
	 * failed parse_recipe() above, before any of the
	 * staging this function does. It used to be refused HERE instead,
	 * which is after copy_file_simple(), write_finalize_script() and a
	 * delete-and-recreate of dest_dir -- so a refused resume had
	 * already destroyed the kept container's output tree on its way to
	 * saying no. A refusal should leave what it refused alone. */
	snprintf(e->build_argv_cmd, sizeof(e->build_argv_cmd),
	         "set -e; cbs build /build/recipe.cbs --arch %s --staged %s --cache %s "
	         "--output %s --finalize-command /build/finalize.sh --events human%s",
	         pkg_host_arch(), PKG_CBS_WORKSPACE, PKG_CBS_CACHE_DIR, PKG_CBS_ARTIFACT,
	         g_chains[chain_idx].hostbuild_extra_config_symbols[0] != '\0'
	                 ? " --input " PKG_CBS_KMOD_EXTRA_INPUT
	                 : "");
	e->build_argv[0] = "/usr/bin/bash";
	e->build_argv[1] = "-c";
	e->build_argv[2] = e->build_argv_cmd;
	e->build_argv[3] = NULL;
	snprintf(e->build_dest_rel, sizeof(e->build_dest_rel), "%s", PKG_DEST_REL_CBS);
	snprintf(e->artifact_format, sizeof(e->artifact_format), "%s", recipe.artifact_format);
	snprintf(e->build_destdir_env, sizeof(e->build_destdir_env), "PKG_DESTDIR=/%s",
	         e->build_dest_rel);
	e->build_envp[0] = e->build_destdir_env;
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

	pkg_build_memory_note_end(chain_idx); /* cix#558: it never ran */

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
/*
 * #550: fills in whose log `le` is, from what was recorded when it was
 * written, never from the filename.
 *
 * A build in flight is looked up first: its log is open and no run
 * names it yet. Its version is the one its chain resolved at fetch, not
 * the entry's own -- during an upgrade the entry still holds the version
 * being replaced (#326's same distinction). Otherwise the newest run
 * whose log is this file names it; a run stores the basename it wrote,
 * so the join is exact. A file neither accounts for stays unnamed.
 */
static void build_log_attribute(struct pkg_build_log_entry *le)
{
	int i;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		const struct pkg_entry *e = &g_packages[i];
		const char *base;
		int c;

		if (!e->in_use || e->run_started_at == 0 || e->build_log_path[0] == '\0')
			continue;
		base = strrchr(e->build_log_path, '/');
		if (strcmp(base != NULL ? base + 1 : e->build_log_path, le->file) != 0)
			continue;
		snprintf(le->name, sizeof(le->name), "%s", e->name);
		snprintf(le->image, sizeof(le->image), "%s", e->image);
		snprintf(le->version, sizeof(le->version), "%s", e->version);
		for (c = 0; c < PKG_MAX_CONCURRENT_JOBS; c++) {
			if (strcmp(g_chains[c].name, e->name) == 0 &&
			    strcmp(g_chains[c].image, e->image) == 0 &&
			    g_chains[c].fetch_resolved_version[0] != '\0') {
				snprintf(le->version, sizeof(le->version), "%s",
				         g_chains[c].fetch_resolved_version);
				break;
			}
		}
		le->in_flight = 1;
		return;
	}
	for (i = g_run_count - 1; i >= 0; i--) {
		if (strcmp(g_runs[i].log, le->file) != 0)
			continue;
		snprintf(le->name, sizeof(le->name), "%s", g_runs[i].name);
		snprintf(le->image, sizeof(le->image), "%s", g_runs[i].image);
		snprintf(le->version, sizeof(le->version), "%s", g_runs[i].version);
		return;
	}
}

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
		memset(&out[count], 0, sizeof(out[count]));
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
	for (i = 0; i < count; i++)
		build_log_attribute(&out[i]);
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
		if (g_runs[i].stages_known) {
			jw_key(w, "build_started_at");
			jw_int(w, (long long)g_runs[i].build_started_at);
			jw_key(w, "install_started_at");
			jw_int(w, (long long)g_runs[i].install_started_at);
		}
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
	/* #424: when each later stage began. A cache hit spawns a no-op build
	 * container to keep one code path, but builds nothing, so it records no
	 * build stage. */
	r->build_started_at = e->cache_hit ? 0 : e->build_started_at;
	r->install_started_at = e->install_started_at;
	r->stages_known = 1;
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
		/* #424: absent from a record written before stage times were kept,
		 * which then reports "stages": null rather than inventing any. */
		{
			const struct json_value *jb = json_object_get(o, "build_started_at");
			const struct json_value *ji = json_object_get(o, "install_started_at");

			r->stages_known = jb != NULL;
			r->build_started_at = jb != NULL ? (time_t)json_as_number(jb) : 0;
			r->install_started_at = ji != NULL ? (time_t)json_as_number(ji) : 0;
		}
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

/*
 * #424: a run's stages as {stage, started_at, seconds}, in the order it
 * went through them. Each stage ends where the next began, and the last
 * where the run ended. A stage the run never reached, or skipped -- a
 * cache hit composes no build -- has no entry rather than a zero:
 * "took no time" and "did not happen" are different answers, the same
 * distinction ADR-0275 draws with `not-implemented` for phases.
 */
static void run_stages_write_json(struct json_writer *w, time_t started, time_t build_at,
                                  time_t install_at, time_t ended)
{
	struct {
		enum pipeline_stage stage;
		time_t at;
	} s[3];
	int n = 0, k;

	s[n].stage = PIPELINE_FETCH;
	s[n++].at = started;
	if (build_at > 0 && build_at >= started) {
		s[n].stage = PIPELINE_BUILD;
		s[n++].at = build_at;
	}
	if (install_at > 0 && install_at >= started) {
		s[n].stage = PIPELINE_INSTALL;
		s[n++].at = install_at;
	}
	jw_arr_open(w);
	for (k = 0; k < n; k++) {
		time_t end = k + 1 < n ? s[k + 1].at : ended;

		jw_obj_open(w);
		jw_key(w, "stage");
		jw_str(w, pipeline_stage_name(s[k].stage));
		jw_key(w, "started_at");
		jw_int(w, (long long)s[k].at);
		jw_key(w, "seconds");
		jw_int(w, (long long)(end > s[k].at ? end - s[k].at : 0));
		jw_obj_close(w);
	}
	jw_arr_close(w);
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
		jw_key(w, "stages");
		if (r->stages_known)
			run_stages_write_json(w, r->started_at, r->build_started_at, r->install_started_at,
			                      r->ended_at);
		else
			jw_null(w); /* a run recorded before #424 kept no stage times */
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
 *
 * The timestamp is whole seconds, so it alone does not make a name
 * unique: since #555 a second image installs the artifact the first
 * one's build just published, typically within the same second, and
 * the O_TRUNC open this used to do replaced the real build's log with
 * the cache hit's empty one (test_pkg's dupbuild case, 0.2.57-423 on
 * 192.168.15.95). So the open is O_EXCL, and a name already taken gets
 * a -2, -3, ... suffix.
 */
static void pkg_build_log_open(struct pkg_entry *e, const char *version)
{
	char dir[PATH_MAX];
	const char *ver;
	long long now;
	int attempt;

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
	ver = version != NULL && version[0] != '\0'
	          ? version
	          : (e->version[0] != '\0' ? e->version : "unknown");
	now = (long long)time(NULL);
	for (attempt = 1; attempt <= 64; attempt++) {
		if (attempt == 1)
			snprintf(e->build_log_path, sizeof(e->build_log_path), "%s/%s-%s-%lld.log", dir,
			         e->name, ver, now);
		else
			snprintf(e->build_log_path, sizeof(e->build_log_path), "%s/%s-%s-%lld-%d.log",
			         dir, e->name, ver, now, attempt);
		e->build_log_fd = open(e->build_log_path,
		                       O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
		if (e->build_log_fd >= 0 || errno != EEXIST)
			break;
	}
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
	/* #552: keep declared setuid/setgid bits -- see merge_tree(). */
	int keep_privileged;
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
/*
 * Issue #486: does the staged tree contain anything at all?
 *
 * Returns 1 for a tree holding at least one non-directory entry, 0 for
 * one holding nothing but directories (or nothing), -1 if it cannot be
 * read.
 *
 * Directories do not count, deliberately. A build whose install phase
 * ran `mkdir -p` and then staged nothing into it leaves a tree that is
 * not empty by any syscall's reckoning and is empty by the only
 * definition that matters: the artifact would ship no content.
 *
 * Measured on 192.168.15.95, 2026-09-18, which is what makes this a
 * gate rather than a theory. TWO packages reached the fleet this way:
 *
 *   probe-pbs 1-1 -- an 87-byte artifact, published to the SHARED
 *   cache, reported `installed`. Every later install took it as a
 *   cache hit and never built, so one build that staged nothing
 *   became every host's copy of that package.
 *
 *   probe-minisign 2 -- zero files, `installed`, while its recipe
 *   visibly stages usr/share/doc/probe-minisign/README. The finalize
 *   phase deleted usr/share/doc (ADR-0251 clause 4, withdrawn by
 *   ADR-0306) and emptied the package on its way out of the build.
 *
 * The second is the argument for putting the check HERE rather than in
 * the build: the tree was correct when the build finished and empty by
 * the time it was packaged. Only the thing about to publish it can
 * tell.
 */
static int staged_tree_has_content(const char *root)
{
	DIR *d = opendir(root);
	struct dirent *ent;
	int found = 0;

	if (d == NULL)
		return -1;
	while (found == 0 && (ent = readdir(d)) != NULL) {
		char child[PATH_MAX];
		struct stat st;

		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		if ((size_t)snprintf(child, sizeof(child), "%s/%s", root, ent->d_name) >=
		    sizeof(child))
			continue;
		if (lstat(child, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			found = staged_tree_has_content(child) == 1 ? 1 : 0;
		else
			found = 1;
	}
	closedir(d);
	return found;
}

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
	         "supplying it to requires { runtime } (having it in requires { build } only puts it in "
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
	return merge_tree(ctx->dest_dir, staging_rootfs, "", ctx->e, ctx->keep_privileged);
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

static int cstr_ptr_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * cix#553: after a verified install whose recipe declares `replaces`,
 * each named package still installed in the image gives up the paths
 * the new package now holds, so every path is owned exactly once again.
 * Done after the install rather than at the gate, so a failed install
 * leaves the previous owner's manifest untouched -- and uninstalling
 * that package later no longer deletes files that are not its own.
 */
static void transfer_replaced_paths(const char *image, const struct pkg_entry *self,
                                    const char *replaces)
{
	const char **mine;
	int i, f;

	if (replaces == NULL || replaces[0] == '\0' || self->file_count <= 0)
		return;
	mine = malloc((size_t)self->file_count * sizeof(*mine));
	if (mine == NULL)
		return;
	for (f = 0; f < self->file_count; f++)
		mine[f] = self->files[f];
	qsort(mine, (size_t)self->file_count, sizeof(*mine), cstr_ptr_cmp);

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		struct pkg_entry *o = &g_packages[i];
		int kept = 0, taken = 0;

		if (o == self || !o->in_use || o->state != PKG_STATE_INSTALLED ||
		    strcmp(normalize_image(o->image), image) != 0 || !name_in_list(replaces, o->name))
			continue;
		for (f = 0; f < o->file_count; f++) {
			const char *key = o->files[f];

			if (bsearch(&key, mine, (size_t)self->file_count, sizeof(*mine), cstr_ptr_cmp) !=
			    NULL) {
				free(o->files[f]);
				taken++;
			} else {
				o->files[kept++] = o->files[f];
			}
		}
		o->file_count = kept;
		if (taken > 0)
			logstore_write("cixd", "info",
			               "image %s: %s@%s took over %d path(s) from %s@%s, which it "
			               "declares it replaces (cix#553)",
			               image, self->name, self->version, taken, o->name, o->version);
	}
	free(mine);
}

/*
 * #570: an install-stage failure that never touched the image leaves the
 * package installed at the version it already had, and the entry says so.
 *
 * `pkg_fail(e, keep_installed, ...)` already carries that rule and every
 * BUILD-stage failure in pkg_build_completed() passes `is_upgrade` to it.
 * Every INSTALL-stage failure passed a literal 0, in the same function,
 * with the same variable in scope -- so a refused upgrade reported the
 * package as FAILED at a version that was never installed. Measured on
 * 192.168.15.95, 2026-10-04: the #389 gate refused `gcc@16.2.0-18` into
 * cix-builder, `pkg ls` then read `gcc cix-builder 16.2.0-18
 * failed:install/failed`, and the image still held and pinned a working
 * 16.2.0-13 -- so the package list named a version the image did not
 * contain, as the image's gcc, in a failed state.
 *
 * Two fields have to move back together, because the entry is also where
 * the version moves to the one being installed (above, at the harvest).
 * That write happens before these gates, so keeping the state alone would
 * trade one wrong answer for a worse one: INSTALLED at the refused
 * version.
 *
 * Called AFTER pkg_fail() deliberately: pkg_run_close() reads e->version
 * inside it (ADR-0272), so the run keeps the version that failed while
 * the entry goes back to the one that works. The two questions have
 * different answers and each surface asks a different one.
 */
static void install_keep_prev_version(struct pkg_entry *e, int is_upgrade,
                                      const char *prev_version, const char *prev_depends)
{
	if (e == NULL || !is_upgrade || prev_version == NULL || prev_version[0] == '\0')
		return;
	snprintf(e->version, sizeof(e->version), "%s", prev_version);
	snprintf(e->depends, sizeof(e->depends), "%s", prev_depends != NULL ? prev_depends : "");
}

/*
 * ADR-0320: an explicit install moves the image's pin for that package
 * to the version it installed, so the manifest and the installed set do
 * not disagree. They used to, and only ever by accident: #535 measured
 * 29 pins left at an old revision while the new one was installed, and
 * once the image was queued the rolling drain would put the pinned
 * version back. A rolling entry is left alone -- its floor already
 * allows the newer version -- and so is a package the manifest does not
 * name, since installing is not declaring.
 */
static void pin_follows_install(const char *image, const char *package, const char *version)
{
	struct image_manifest_entry entries[IMAGE_MANIFEST_MAX_PACKAGES];
	int count = 0, i;

	if (image_manifest_read(image, entries, &count, IMAGE_MANIFEST_MAX_PACKAGES) != IMAGE_OK)
		return;
	for (i = 0; i < count; i++) {
		if (strcmp(entries[i].package, package) != 0 || entries[i].mode != IMAGE_PKG_PINNED)
			continue;
		if (strcmp(entries[i].version, version) == 0)
			return;
		if (image_manifest_set(image, package, IMAGE_PKG_PINNED, version) == IMAGE_OK)
			logstore_write("cixd", "info",
			               "image %s: pin %s moved %s -> %s with an explicit install "
			               "(ADR-0320)",
			               image, package, entries[i].version, version);
		return;
	}
}

int pkg_build_completed(const char *container_name, int exit_status, pid_t *out_pid,
                         int *out_pidfd, int *out_chain_idx, char *out_hostbuild_done_name,
                         int *out_kept)
{
	struct pkg_entry *e;
	char container_base[PATH_MAX];
	char dest_dir[PATH_MAX];
	int is_final, is_upgrade;
	/* #570: what is installed now, kept so an install-stage refusal can
	 * put the entry back on it. */
	char prev_version[PKG_VERSION_MAX], prev_depends[PKG_DEPENDS_MAX];
	int chain_idx;

	out_hostbuild_done_name[0] = '\0';
	prev_version[0] = '\0';
	prev_depends[0] = '\0';
	*out_kept = 0;

	chain_idx = pkg_build_container_chain_index(container_name);
	if (chain_idx < 0)
		return 0;
	pkg_build_memory_note_end(chain_idx); /* cix#558 */

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
			/* #503: the build that started this was exit 3 with a
			 * 65-byte log -- one line, written before the disk
			 * filled. Nothing in either said so. */
			char disk[256];

			disk_pressure_note(disk, sizeof(disk));
			pkg_fail(e, is_upgrade, PIPELINE_BUILD, "build failed (exit status %d)%s",
			         exit_status, disk);
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
		                "the recipe's requires { build } (#302)",
		                e->name, g_chains[chain_idx].image, e->missing_tool,
		                e->missing_tool_count > 1 ? " and others" : "",
		                e->missing_tool_count);
		pkg_fail(e, is_upgrade, PIPELINE_BUILD,
		         "build environment is missing '%s' (%d missing command(s) reported) -- "
		         "declare it in the recipe's requires { build }",
		         e->missing_tool, e->missing_tool_count);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	e->build_output_captured_len = 0;
	e->last_output_at = 0;

	snprintf(container_base, sizeof(container_base), "%s/%s", g_containers_dir,
	         e->build_container_name);
	/* What the build was told, not a second guess at it -- see
	 * build_dest_rel's own comment. A cache hit never entered a
	 * container and may not have set it; its tree is at the same
	 * PKG_DEST_REL_CBS, which the unpack populated directly. */
	snprintf(dest_dir, sizeof(dest_dir), "%s/upper/%s", container_base,
	         e->build_dest_rel[0] != '\0' ? e->build_dest_rel : PKG_DEST_REL_CBS);

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
	/* #570: the version this entry is leaving, for an install-stage
	 * refusal that never reaches the image. */
	snprintf(prev_version, sizeof(prev_version), "%s", e->version);
	snprintf(prev_depends, sizeof(prev_depends), "%s", e->depends);
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
	/* #424: the install stage begins here, where a successful build's
	 * output starts going into the image. */
	e->install_started_at = time(NULL);
	e->state = PKG_STATE_INSTALLED;
	e->error[0] = '\0';
	e->status = PIPELINE_OK; /* a success that leaves a failed status set
	                          * reports an installed package as failed to
	                          * anything keying on it */

	/*
	 * Issue #486: nothing staged is a failed build, not a package.
	 *
	 * Before the branch, because it is a property of what the build
	 * produced rather than of how it is about to be installed, and all
	 * three arms below would otherwise publish it: an ordinary install
	 * merges an empty tree into the image, a hostbuild harvests one
	 * into the artifact directory that mkbootroot then reads, and a
	 * cache hit re-publishes the empty artifact it was handed.
	 *
	 * A tree that cannot be READ is not treated as empty -- that is a
	 * different failure, and the branches below report it with their
	 * own messages rather than having this one guess.
	 */
	if (staged_tree_has_content(dest_dir) == 0) {
		char msg[320];

		snprintf(msg, sizeof(msg),
		         "the build staged no files -- %s holds nothing but directories, so this "
		         "would publish an empty package. Check the install phase, and check "
		         "whether the finalize phase removed everything it staged (#486)",
		         e->build_dest_rel[0] != '\0' ? e->build_dest_rel : PKG_DEST_REL_CBS);
		logstore_write("cixd", "error", "pkg install: %s@%s: %s", e->name, e->version, msg);
		pkg_fail(e, is_upgrade, PIPELINE_INSTALL, msg);
		install_keep_prev_version(e, is_upgrade, prev_version, prev_depends);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		g_chains[chain_idx].is_hostbuild = 0;
		return 0;
	}

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
		    merge_tree(dest_dir, artifact_dir, "", e, 1) != 0) {
			pkg_fail(e, is_upgrade, PIPELINE_INSTALL, "failed to harvest the built artifact");
			install_keep_prev_version(e, is_upgrade, prev_version, prev_depends);
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
		char refused[512];

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
		if (undeclared_link_gate(e, g_chains[chain_idx].image, dest_dir, refused,
		                          sizeof(refused)) != 0) {
			pkg_fail(e, is_upgrade, PIPELINE_INSTALL, refused);
			install_keep_prev_version(e, is_upgrade, prev_version, prev_depends);
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
		}

		/* #553, at the same point and for the same reasons: nothing is
		 * staged, and the message names both packages and the path. */
		if (path_owner_gate(e, g_chains[chain_idx].image, dest_dir,
		                    g_chains[chain_idx].fetch_resolved_replaces, refused,
		                    sizeof(refused)) != 0) {
			logstore_write("cixd", "error", "pkg install: %s: %s", e->name, refused);
			pkg_fail(e, is_upgrade, PIPELINE_INSTALL, refused);
			install_keep_prev_version(e, is_upgrade, prev_version, prev_depends);
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
		}

		ctx.dest_dir = dest_dir;
		ctx.e = e;
		ctx.is_upgrade = is_upgrade;
		ctx.keep_privileged = 1;
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
			/* NOT kept installed (#570): install_mutate() frees this entry's
			 * file list before merging, so a failure part-way leaves it
			 * describing neither version. */
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
				/* NOT kept installed (#570): the merge disturbed this entry's own
				 * file list, and the files it names are not in the image's
				 * current version. An entry claiming INSTALLED here is the
				 * split this check exists to catch. */
				pkg_fail(e, 0, PIPELINE_INSTALL, "%s", msg);
				g_chains[chain_idx].name[0] = '\0';
				g_chains[chain_idx].dep_queue_count = 0;
				return 0;
			}
		}

		/* ADR-0320: an operator's own install moves the pin with it; a
		 * rolling rebuild installs exactly the pin and needs nothing. */
		if (strcmp(g_chains[chain_idx].run_trigger, PKG_RUN_TRIGGER_REQUEST) == 0)
			pin_follows_install(g_chains[chain_idx].image, e->name, e->version);
		/* cix#553: paths this package declared it takes over change owner. */
		transfer_replaced_paths(normalize_image(g_chains[chain_idx].image), e,
		                        g_chains[chain_idx].fetch_resolved_replaces);

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
			/*
			 * A CBS build leaves a finished .cixpkg -- CBS wrote it,
			 * inside the container, after the finalize policy ran
			 * (src/package.c:374 then :389) -- so cixd takes the file,
			 * by rename. Nothing here tars a tree any more (#499,
			 * cix#569).
			 *
			 * Read from the entry: the recipe declared the format, and
			 * a CPDL recipe declaring anything but cixpkg is refused at
			 * publish and by `cbs build` itself.
			 */
			if (strcmp(e->artifact_format, PKG_ARTIFACT_FORMAT_CIXPKG) == 0) {
				char produced[PATH_MAX];
				struct stat pst;

				snprintf(produced, sizeof(produced), "%s/upper/%s", container_base,
				         PKG_CBS_ARTIFACT_REL);
				/*
				 * Said out loud, because pkg_cache_save_from_file()
				 * returns silently on a missing source and the next
				 * thing an operator would see is the push worker
				 * reporting "no artifact in the local cache" -- a
				 * true statement about a cause two steps away. A
				 * build that succeeded and produced no artifact
				 * means cbs did not honour --output, which is worth
				 * naming where it happened.
				 */
				if (stat(produced, &pst) != 0)
					logstore_write("cixd", "error",
					                "pkg %s@%s: the build succeeded but wrote no artifact at "
					                "%s -- nothing to cache or publish (ADR-0307)",
					                e->name, e->version, PKG_CBS_ARTIFACT);
				else
					pkg_cache_save_from_file(e->name, e->version, e->artifact_format,
					                          produced);
			} else {
				/*
				 * Unreachable, and said so rather than served. A
				 * package artifact is a .cixpkg and never a tar.gz
				 * (One Build System Mandate, ADR-0307): shell
				 * recipes do not exist (cix#569), and nothing
				 * here reads one; a CPDL recipe
				 * declaring tar.gz is refused at publish and by
				 * `cbs build` itself. This branch used to tar the
				 * tree into the cache instead, a route back to the
				 * legacy format; it now caches and publishes nothing
				 * (#499).
				 */
				logstore_write("cixd", "error",
				               "pkg %s@%s: built with artifact format \"%s\", which is not "
				               ".cixpkg -- nothing cached or published",
				               e->name, e->version, e->artifact_format);
			}
			/* Issue #129: a fresh build is the only thing worth
			 * publishing -- a cache/artifact hit's bytes already came
			 * from somewhere else. */
			if (strcmp(e->artifact_format, PKG_ARTIFACT_FORMAT_CIXPKG) == 0)
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

		/*
		 * #531: the unpack flag belongs to the PACKAGE, not to the
		 * chain slot, and it was only ever cleared when a slot was
		 * handed out.
		 *
		 * The invariant is already written where it is cleared: "a
		 * slot that came back round would otherwise report the
		 * previous job's unpack as this one's, and skip the
		 * extraction this job needs." That is exactly right and it
		 * was applied one level too coarsely -- the next DEPENDENCY
		 * in a chain is a different package needing its own
		 * extraction, just as surely as the next job is.
		 *
		 * Left set, pkg_prepare_build_and_start()'s re-entry guard
		 * takes its "already unpacked" branch for every dependency
		 * after the first: no reset_build_container_dir(), no
		 * dest_dir, and no unpack. Each one then merges whatever the
		 * PREVIOUS package left in the shared container's dest, and
		 * installs cleanly with the wrong contents.
		 *
		 * Measured on 192.168.15.95, 2026-09-26, installing binutils
		 * into a fresh image: binutils declares zlib and flex at
		 * runtime, so the chain is zlib, m4, flex, binutils. ONE
		 * unpack was logged (zlib's), all four reported installed in
		 * the same second, and all four recorded zlib's seven files.
		 * The image really did receive only zlib, which is why every
		 * later build in it failed finalize with "this package
		 * produced ELF output but the build image has no strip" --
		 * binutils was installed, and binutils was not there.
		 *
		 * Why it stayed latent is worth recording, because it is
		 * not rarity: 163 of the corpus's 473 CPDL recipes declare
		 * runtime dependencies. It is that resolve_chain() returns
		 * early for anything already installed ("already
		 * satisfied"), so a queue longer than ONE entry only forms
		 * when the dependencies are genuinely absent -- a fresh
		 * image, or a dependency a recipe has only just declared.
		 * Every install into an established image has a
		 * single-entry queue, never reaches this advance, and is
		 * correct. That is also why the reproduction needs a new
		 * image and cannot be seen on a working one.
		 */
		g_chains[chain_idx].unpacked = 0;

		g_chains[chain_idx].dep_queue_pos++;
		perr = start_fetch_for(g_chains[chain_idx].dep_queue[g_chains[chain_idx].dep_queue_pos],
		                        chain_idx, out_pid, out_pidfd);
		if (perr == PKG_OK) {
			*out_chain_idx = chain_idx;
			return 1;
		}
		/*
		 * Couldn't start the next dependency: the chain is abandoned,
		 * and it is said here because nothing else will say it. The
		 * step that failed never got an entry, so GET /pkg shows the
		 * dependencies that did install and then simply stops -- the
		 * requested package has no row and no error. Measured on
		 * 192.168.15.95, 2026-10-02: node into a fresh image stopped
		 * after its fifth dependency with nothing logged at all,
		 * because the package table was full.
		 */
		logstore_write("cixd", "error",
		               "pkg %s@%s: install abandoned -- dependency %s could not start: %s (%d)",
		               g_chains[chain_idx].dep_queue[g_chains[chain_idx].dep_queue_count - 1],
		               g_chains[chain_idx].image,
		               g_chains[chain_idx].dep_queue[g_chains[chain_idx].dep_queue_pos],
		               perr == PKG_ERR_FULL ? "the package table is full" : "pkg_error", (int)perr);
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

/* ---- pkg sync (ADR-0121), over every recipe source (ADR-0324, pkgsource.c) ---- */

static pid_t g_sync_pid = -1;
enum sync_state { SYNC_NEVER = 0, SYNC_RUNNING, SYNC_SUCCESS, SYNC_FAILED };
static enum sync_state g_sync_last_state = SYNC_NEVER;
static time_t g_sync_last_attempt;
static int g_sync_last_added;
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

/*
 * Splits a source url ("<scheme>://<host>/<owner>/<repo>[.git][/...]")
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
static int parse_repo_url(const char *url, char *out_scheme, size_t scheme_sz, char *out_host,
                          size_t host_sz, char *out_owner, size_t owner_sz, char *out_repo,
                          size_t repo_sz)
{
	const char *scheme_end = strstr(url, "://");
	const char *host_start, *host_end, *path;
	char path_buf[PKG_SOURCE_URL_MAX];
	char *owner_start, *slash1, *slash2, *repo_start;
	size_t repo_len;

	if (scheme_end == NULL)
		return -1;
	snprintf(out_scheme, scheme_sz, "%.*s", (int)(scheme_end - url), url);

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

/* ---- ADR-0323: commit a recipe to git, then publish it ----
 *
 * The author stage's second half, and the path a person uses to move a
 * pinned package on through the same pipeline: a recipe goes into the
 * recipe repository as one commit FIRST, and is published here only
 * once that commit exists. Git therefore holds every revision a host
 * builds -- which it did not for 0.2.57-428 and -429, published and
 * never committed -- and the commit is the record of what was written.
 *
 * One job at a time, like a sync: the commit is network I/O and runs in
 * a helper process (ADR-0278); its outcome reaches the parent through a
 * result file, and the publish happens in the parent, which is the only
 * place a recipe store change counts.
 */
enum recipe_commit_state { RECIPE_COMMIT_NEVER = 0, RECIPE_COMMIT_RUNNING, RECIPE_COMMIT_DONE,
	                   RECIPE_COMMIT_FAILED };

static struct {
	enum recipe_commit_state state;
	char name[PKG_NAME_MAX];
	char version[PKG_VERSION_MAX];
	int choose;                       /* record the source as the operator's choice when done */
	char source[PKG_SOURCE_NAME_MAX]; /* ADR-0324: the writable source it goes to */
	char repo_path[PATH_MAX];
	char message[PKG_CHANGELOG_MAX + 256];
	char *content; /* the redacted recipe: what is committed is what is published */
	char commit_sha[FORGE_COMMIT_SHA_MAX];
	char error[PKG_ERROR_MAX];
	int published;
	time_t started_at;
	time_t finished_at;
} g_recipe_commit;

static void recipe_commit_result_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/recipe-commit.result", g_pkg_dir);
}

/*
 * ADR-0324: the source a recipe for `name` is committed to, resolved by
 * pkgsource_resolve() exactly as a plain publish is, then required to be
 * writable here -- a host never writes to a source it only reads,
 * because that source is authored somewhere else -- and to hold a token.
 */
static int resolve_commit_target(const char *name, const char *requested, char *out,
                                 size_t out_size, int *choose, char *err, size_t err_size)
{
	char item[PKG_SOURCE_ITEM_MAX];
	const struct pkg_source *s;

	snprintf(item, sizeof(item), "package:%s", name);
	if (pkgsource_resolve(item, requested, 1, out, out_size, choose, err, err_size) != 0)
		return -1;
	s = pkgsource_find(out);
	if (s == NULL || !s->write) {
		snprintf(err, err_size,
		         "source %s is read-only on this host: a source is written where it is "
		         "authored (ADR-0324)",
		         out);
		return -1;
	}
	if (s->token[0] == '\0') {
		snprintf(err, err_size, "source %s has no token, so nothing can be committed to it",
		         s->name);
		return -1;
	}
	return 0;
}

enum pkg_error pkg_recipe_commit_start(const char *name, const char *content, const char *source,
                                       char *err, size_t err_size)
{
	struct recipe_check check;
	char target[PKG_SOURCE_NAME_MAX];
	char host[256];
	int choose = 0;
	char *redacted;
	enum pkg_error perr;

	err[0] = '\0';
	if (g_recipe_commit.state == RECIPE_COMMIT_RUNNING) {
		snprintf(err, err_size, "%s@%s is still being committed", g_recipe_commit.name,
		         g_recipe_commit.version);
		return PKG_ERR_BUSY;
	}
	if (resolve_commit_target(name, source, target, sizeof(target), &choose, err, err_size) != 0)
		return PKG_ERR_NOT_FOUND;

	/* Every test a publish applies, before anything reaches git. */
	perr = recipe_publish(name, content, &check, NULL);
	if (perr != PKG_OK) {
		if (perr == PKG_ERR_DUPLICATE)
			snprintf(err, err_size, "%s is already published at that version", name);
		else if (perr == PKG_ERR_DIVERGENT)
			snprintf(err, err_size, "%s is already published at that version with different "
			                        "content -- publish a new release (ADR-0324)",
			         name);
		else if (pkg_recipe_add_last_error()[0] != '\0')
			snprintf(err, err_size, "%s", pkg_recipe_add_last_error());
		else
			snprintf(err, err_size, "the recipe does not publish: it failed to parse, or "
			                        "its package name is not %s",
			         name);
		return perr;
	}

	redacted = strdup(content);
	if (redacted == NULL) {
		snprintf(err, err_size, "out of memory");
		return PKG_ERR_PERSIST_FAILED;
	}
	redact_repo_token(redacted, strlen(redacted) + 1);

	free(g_recipe_commit.content);
	memset(&g_recipe_commit, 0, sizeof(g_recipe_commit));
	g_recipe_commit.content = redacted;
	snprintf(g_recipe_commit.name, sizeof(g_recipe_commit.name), "%s", name);
	snprintf(g_recipe_commit.version, sizeof(g_recipe_commit.version), "%s", check.version);
	snprintf(g_recipe_commit.source, sizeof(g_recipe_commit.source), "%s", target);
	g_recipe_commit.choose = choose;
	/* ADR-0308: one flat file per revision. */
	snprintf(g_recipe_commit.repo_path, sizeof(g_recipe_commit.repo_path),
	         "recipes/package/%s@%s%s", name, check.version, PKG_RECIPE_CBS_SUFFIX);
	if (gethostname(host, sizeof(host)) != 0)
		snprintf(host, sizeof(host), "%s", "unknown host");
	host[sizeof(host) - 1] = '\0';
	snprintf(g_recipe_commit.message, sizeof(g_recipe_commit.message),
	         "%s@%s: committed by cixd on %s before publishing it (ADR-0323)%s%s", name,
	         check.version, host, check.changelog[0] != '\0' ? "\n\n" : "", check.changelog);
	g_recipe_commit.state = RECIPE_COMMIT_RUNNING;
	g_recipe_commit.started_at = time(NULL);
	return PKG_OK;
}

/*
 * ADR-0323's author stage: the next revision of a package's recipe,
 * written by cixd and committed git first.
 *
 * From the newest published revision, `cbs revise` changes exactly what
 * ADR-0318 section 3 lists -- version, release 1, the main source's url
 * and sha256, the artifact approval removed (an approval of one byte
 * sequence is never carried to another: the Build Provenance Mandate),
 * and a changelog naming the release, its digest and how it was
 * verified -- and preserves every other byte, comments included. cixd
 * does not edit CPDL text itself: cbs owns the grammar, and revise
 * validates what it writes (cix-build-system b1626a5, v0.1.101).
 *
 * The result goes through pkg_recipe_commit_start(), so the revision
 * meets every publish test, goes only to a writable source with a
 * token, and is committed before it is published. Callers check that
 * the package has a recipe first: the author stage never writes a
 * package's first recipe (ADR-0318).
 */
#define PKG_REVISE_MAX 262144

/*
 * The revision says what it was asked to: every field cbs revise was
 * told to set, read back from `cbs explain` of the revised text. revise
 * validates the grammar, not the intent, and the two have differed:
 * measured on 192.168.15.95, 2026-10-02, v0.1.102 wrote a changelog over
 * its own metadata KEY (cix-build-system#279), and the result validated
 * and published as hibr@0.99.4-1. A revision that fails this is refused,
 * naming the field, rather than committed. 0, or -1 with why in err.
 */
static int revision_says(const char *text, size_t len, const char *version, const char *url,
                         const char *sha256, const char *changelog, char *err, size_t err_size)
{
	char path[PATH_MAX], why[PKG_ERROR_MAX];
	char got_version[PKG_VERSION_MAX], got_url[PKG_URL_MAX], got_sha[PKG_SHA256_MAX];
	char got_changelog[PKG_CHANGELOG_MAX], got_approval[PKG_SHA256_MAX];
	char *json;
	size_t json_len = 0;
	long long release = 0;
	struct cbs_explain *ex;
	const char *wrong = NULL;

	/* cbs requires the .cbs suffix on any path it reads. */
	snprintf(path, sizeof(path), "%s/revise-check%s", g_pkg_dir, PKG_RECIPE_CBS_SUFFIX);
	json = malloc(PKG_EXPLAIN_MAX);
	if (json == NULL || persist_atomic_write(path, text, len) != 0) {
		free(json);
		snprintf(err, err_size, "could not stage the revision to check it");
		return -1;
	}
	if (run_cbs_explain(path, json, PKG_EXPLAIN_MAX, &json_len, why, sizeof(why)) != 0) {
		unlink(path);
		free(json);
		snprintf(err, err_size, "cbs explain refused the revision cbs revise wrote: %s", why);
		return -1;
	}
	unlink(path);
	ex = cbs_explain_parse(json, json_len, why, sizeof(why));
	free(json);
	if (ex == NULL) {
		snprintf(err, err_size, "the revision's explain did not parse: %s", why);
		return -1;
	}
	if (cbs_explain_version_parts(ex, got_version, sizeof(got_version), &release) != 0 ||
	    strcmp(got_version, version) != 0)
		wrong = "version";
	else if (release != 1)
		wrong = "release";
	else if (cbs_explain_source(ex, 0, got_url, sizeof(got_url), got_sha, sizeof(got_sha)) != 0 ||
	         strcmp(got_url, url) != 0)
		wrong = "the main source's url";
	else if (strcmp(got_sha, sha256) != 0)
		wrong = "the main source's sha256";
	else if (cbs_explain_metadata(ex, "changelog", got_changelog, sizeof(got_changelog)) != 0 ||
	         strcmp(got_changelog, changelog) != 0)
		wrong = "metadata changelog";
	else if (cbs_explain_metadata(ex, "artifact_sha256", got_approval, sizeof(got_approval)) != 0 ||
	         got_approval[0] != '\0')
		wrong = "metadata artifact_sha256 (it must be gone)";
	cbs_explain_free(ex);
	if (wrong != NULL) {
		snprintf(err, err_size,
		         "cbs revise wrote a revision whose %s is not what was asked, so it is not "
		         "committed",
		         wrong);
		return -1;
	}
	return 0;
}

/*
 * The revision does not still name the release it was revised from
 * (cix#567). cbs revise changes the fields it is told to and nothing
 * else, so a recipe whose body spells its own release out stays on the
 * old one. Measured on 192.168.15.95, 2026-10-03: kernel@7.2.9-1 was
 * revised from 7.2.3-19, carried `linux-7.2.3` and `lib/modules/7.2.3`
 * in 13 places, and was committed and published although it could not
 * build. Such a recipe cannot roll until a person writes those places as
 * ${version}, so it is refused here, where the catalogue row says which
 * line to change, rather than failing a build at 03:00.
 *
 * Not counted: comments (a `#` outside a string starts one), and the
 * values cixd itself just wrote -- the main url and the changelog, which
 * names the revision it was written from on purpose. A match is the old
 * version standing alone, not part of a longer number on either side
 * ("7.2.3" is not in "17.2.3" or in "7.2.31"). An old version under
 * three characters is not checked, because "1" or "12" cannot be told
 * from an ordinary number in a build command. 0, or -1 with the first
 * offending line in err.
 */
static int is_version_digit_edge(const char *s, size_t len, size_t at, int forward)
{
	if (forward) {
		if (at >= len)
			return 0;
		if (s[at] >= '0' && s[at] <= '9')
			return 1;
		return s[at] == '.' && at + 1 < len && s[at + 1] >= '0' && s[at + 1] <= '9';
	}
	if (at == 0)
		return 0;
	if (s[at - 1] >= '0' && s[at - 1] <= '9')
		return 1;
	return s[at - 1] == '.' && at >= 2 && s[at - 2] >= '0' && s[at - 2] <= '9';
}

/* Overwrites every occurrence of `value` in buf with spaces. */
static void blank_value(char *buf, const char *value)
{
	size_t vlen = strlen(value);
	char *p = buf;

	if (vlen == 0)
		return;
	while ((p = strstr(p, value)) != NULL) {
		memset(p, ' ', vlen);
		p += vlen;
	}
}

static int revision_drops_old_version(const char *text, size_t len, const char *old_upstream,
                                      const char *url, const char *changelog, char *err,
                                      size_t err_size)
{
	size_t olen = strlen(old_upstream), start = 0, line_no = 0;
	char *buf;

	if (olen < 3)
		return 0;
	buf = malloc(len + 1);
	if (buf == NULL) {
		snprintf(err, err_size, "out of memory");
		return -1;
	}
	memcpy(buf, text, len);
	buf[len] = '\0';
	blank_value(buf, url);
	blank_value(buf, changelog);

	while (start < len) {
		size_t end = start, code_end, i;
		int in_string = 0;

		while (end < len && buf[end] != '\n')
			end++;
		line_no++;

		/* Where the code ends: the first `#` outside a string. */
		code_end = end;
		for (i = start; i < end; i++) {
			if (in_string && buf[i] == '\\') {
				i++;
				continue;
			}
			if (buf[i] == '"')
				in_string = !in_string;
			else if (!in_string && buf[i] == '#') {
				code_end = i;
				break;
			}
		}

		for (i = start; i + olen <= code_end; i++) {
			if (memcmp(buf + i, old_upstream, olen) != 0 ||
			    is_version_digit_edge(buf, code_end, i, 0) ||
			    is_version_digit_edge(buf, code_end, i + olen, 1))
				continue;
			free(buf);
			snprintf(err, err_size,
			         "line %zu still names the old release %s, so this revision would "
			         "build the old one: write it as ${version} in the recipe, and the "
			         "next discovery run authors this release (cix#567)",
			         line_no, old_upstream);
			return -1;
		}
		start = end + 1;
	}
	free(buf);
	return 0;
}


enum pkg_error pkg_recipe_revise_start(const char *name, const char *version, const char *url,
                                       const char *sha256, const char *verification,
                                       const char *source, char *err, size_t err_size)
{
	char prev[PKG_VERSION_MAX], path[PATH_MAX], candidate[PKG_VERSION_MAX + 4];
	char set_version[PKG_VERSION_MAX + 16], set_url[PKG_URL_MAX + 32];
	char set_sha[PKG_SHA256_MAX + 32], set_changelog[PKG_CHANGELOG_MAX + 32];
	char changelog[PKG_CHANGELOG_MAX], old_upstream[PKG_VERSION_MAX];
	char *argv[16];
	struct pkg_recipe recipe;
	char *revised;
	size_t revised_len = 0, i;
	int n = 0;
	enum pkg_error perr;

	err[0] = '\0';
	if (name == NULL || !pkg_name_is_valid(name) ||
	    recipe_latest_version(name, prev, sizeof(prev)) != 0 ||
	    find_recipe_path(name, prev, path, sizeof(path)) != 0) {
		snprintf(err, err_size, "%s has no recipe to revise: the author stage writes a next "
		                        "revision, never a first one (ADR-0318)",
		         name != NULL ? name : "");
		return PKG_ERR_NOT_FOUND;
	}
	if (!recipe_path_is_cbs(path)) {
		snprintf(err, err_size, "%s@%s is not a CPDL recipe, so cbs cannot revise it", name,
		         prev);
		return PKG_ERR_INVALID_RECIPE;
	}
	if (version == NULL || version[0] == '\0' || strlen(version) + 3 > PKG_VERSION_MAX ||
	    strpbrk(version, " \t\r\n\"/") != NULL) {
		snprintf(err, err_size, "version must be a non-empty upstream version with no "
		                        "spaces, quotes or slashes");
		return PKG_ERR_INVALID_RECIPE;
	}
	if (url == NULL || (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) ||
	    strlen(url) >= PKG_URL_MAX || strpbrk(url, " \t\r\n\"") != NULL) {
		snprintf(err, err_size, "url must be an http(s) URL of at most %d bytes with no "
		                        "spaces or quotes",
		         PKG_URL_MAX - 1);
		return PKG_ERR_INVALID_RECIPE;
	}
	if (sha256 == NULL || strlen(sha256) != 64 || strspn(sha256, "0123456789abcdef") != 64) {
		snprintf(err, err_size, "sha256 must be 64 lowercase hex digits");
		return PKG_ERR_INVALID_RECIPE;
	}
	if (verification == NULL || verification[0] == '\0' ||
	    strpbrk(verification, "\r\n\"") != NULL) {
		snprintf(err, err_size, "verification is required: how this digest was established, "
		                        "on one line with no quotes -- the commit is the record of it "
		                        "(ADR-0323)");
		return PKG_ERR_INVALID_RECIPE;
	}

	/* Never a downgrade, never the same release twice. */
	snprintf(candidate, sizeof(candidate), "%s-1", version);
	if (pkg_version_compare(candidate, prev) <= 0) {
		snprintf(err, err_size, "%s@%s is not newer than the newest revision, %s@%s", name,
		         candidate, name, prev);
		return PKG_ERR_DUPLICATE;
	}

	if ((size_t)snprintf(changelog, sizeof(changelog),
	                     "%s: %s %s, sha256 %s, verified by %s. Written by cixd from %s "
	                     "(ADR-0323).",
	                     candidate, name, version, sha256, verification, prev) >=
	    sizeof(changelog)) {
		snprintf(err, err_size, "the changelog this revision needs is longer than %d bytes: "
		                        "shorten the verification",
		         PKG_CHANGELOG_MAX - 1);
		return PKG_ERR_INVALID_RECIPE;
	}

	snprintf(set_version, sizeof(set_version), "version=%s", version);
	snprintf(set_url, sizeof(set_url), "source.main.url=%s", url);
	snprintf(set_sha, sizeof(set_sha), "source.main.sha256=%s", sha256);
	snprintf(set_changelog, sizeof(set_changelog), "metadata.changelog=%s", changelog);
	argv[n++] = (char *)"cbs";
	argv[n++] = (char *)"revise";
	argv[n++] = path;
	argv[n++] = (char *)"--set";
	argv[n++] = set_version;
	argv[n++] = (char *)"--set";
	argv[n++] = (char *)"release=1";
	argv[n++] = (char *)"--set";
	argv[n++] = set_url;
	argv[n++] = (char *)"--set";
	argv[n++] = set_sha;
	argv[n++] = (char *)"--set";
	argv[n++] = set_changelog;
	/* revise refuses to unset what is absent, so only when there is one. */
	if (parse_recipe(path, &recipe) == 0 && recipe.artifact_sha256[0] != '\0') {
		argv[n++] = (char *)"--unset";
		argv[n++] = (char *)"metadata.artifact_sha256";
	}
	argv[n] = NULL;

	revised = malloc(PKG_REVISE_MAX);
	if (revised == NULL) {
		snprintf(err, err_size, "out of memory");
		return PKG_ERR_PERSIST_FAILED;
	}
	if (run_cbs(argv, "revise", revised, PKG_REVISE_MAX, &revised_len, err, err_size) != 0) {
		free(revised);
		return PKG_ERR_INVALID_RECIPE;
	}
	for (i = 0; i < revised_len; i++)
		if (revised[i] == '\0') {
			free(revised);
			snprintf(err, err_size, "cbs revise wrote a NUL byte into the recipe");
			return PKG_ERR_INVALID_RECIPE;
		}

	if (revision_says(revised, revised_len, version, url, sha256, changelog, err, err_size) != 0) {
		free(revised);
		return PKG_ERR_INVALID_RECIPE;
	}
	srcresolve_upstream_of(prev, old_upstream, sizeof(old_upstream));
	if (revision_drops_old_version(revised, revised_len, old_upstream, url, changelog, err,
	                               err_size) != 0) {
		free(revised);
		return PKG_ERR_INVALID_RECIPE;
	}
	perr = pkg_recipe_commit_start(name, revised, source, err, err_size);
	free(revised);
	if (perr == PKG_OK)
		logstore_write("cixd", "info",
		               "pkg: wrote %s@%s from %s@%s (sha256 %s, verified by %s) -- committing "
		               "it before publishing (ADR-0323)",
		               name, candidate, name, prev, sha256, verification);
	return perr;
}

/* Called when the helper could not be started, so `done` never runs. */
void pkg_recipe_commit_abort(const char *why)
{
	g_recipe_commit.state = RECIPE_COMMIT_FAILED;
	g_recipe_commit.finished_at = time(NULL);
	snprintf(g_recipe_commit.error, sizeof(g_recipe_commit.error), "%s", why);
}

/* The helper's work: the forge call, and nothing that changes memory
 * the parent reads (ADR-0278). */
/* A source as a forge to write to, with the buffers its fields point into. */
struct source_forge {
	char scheme[16], host[256], owner[256], repo[256];
	struct forge_target t;
};

static int source_forge_target(const char *source, struct source_forge *f, char *err,
                               size_t err_size)
{
	const struct pkg_source *s = pkgsource_find(source);

	if (s == NULL || parse_repo_url(s->url, f->scheme, sizeof(f->scheme), f->host,
	                                sizeof(f->host), f->owner, sizeof(f->owner), f->repo,
	                                sizeof(f->repo)) != 0) {
		snprintf(err, err_size, "source %s is gone, or its URL does not name an owner and repo",
		         source);
		return -1;
	}
	memset(&f->t, 0, sizeof(f->t));
	f->t.kind = s->kind;
	f->t.scheme = f->scheme;
	f->t.host = f->host;
	f->t.owner = f->owner;
	f->t.repo = f->repo;
	f->t.token = s->token;
	f->t.branch = s->ref[0] != '\0' ? s->ref : PKG_SOURCE_DEFAULT_REF;
	return 0;
}

int pkg_recipe_commit_work(void *unused)
{
	char result_path[PATH_MAX];
	char sha[FORGE_COMMIT_SHA_MAX];
	char err[PKG_ERROR_MAX];
	struct source_forge f;
	long status = 0;
	int rc;

	(void)unused;
	recipe_commit_result_path(result_path, sizeof(result_path));
	if (source_forge_target(g_recipe_commit.source, &f, err, sizeof(err)) != 0) {
		persist_atomic_write(result_path, err, strlen(err));
		return 1;
	}
	rc = forge_create_file(&f.t, g_recipe_commit.repo_path, g_recipe_commit.content,
	                       strlen(g_recipe_commit.content), g_recipe_commit.message, g_pkg_dir,
	                       sha, sizeof(sha), &status, err, sizeof(err));
	if (rc != 0) {
		redact_repo_token(err, sizeof(err));
		persist_atomic_write(result_path, err, strlen(err));
		return 1;
	}
	persist_atomic_write(result_path, sha, strlen(sha));
	return 0;
}

/* The parent, once the helper has exited: record, then publish. */
void pkg_recipe_commit_done(int exit_status, void *unused)
{
	char result_path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;
	enum pkg_error perr;

	(void)unused;
	recipe_commit_result_path(result_path, sizeof(result_path));
	if (persist_read_file(result_path, &buf, &len) != 0 || buf == NULL)
		len = 0;
	unlink(result_path);
	g_recipe_commit.finished_at = time(NULL);

	if (exit_status != 0) {
		if (len > 0)
			snprintf(g_recipe_commit.error, sizeof(g_recipe_commit.error), "%.*s", (int)len,
			         buf);
		else
			snprintf(g_recipe_commit.error, sizeof(g_recipe_commit.error), "%s",
			         "the commit helper failed and said nothing");
		g_recipe_commit.state = RECIPE_COMMIT_FAILED;
		logstore_write("cixd", "error", "pkg recipe commit %s@%s: %s -- nothing published",
		               g_recipe_commit.name, g_recipe_commit.version, g_recipe_commit.error);
		free(buf);
		return;
	}
	snprintf(g_recipe_commit.commit_sha, sizeof(g_recipe_commit.commit_sha), "%.*s", (int)len,
	         len > 0 ? buf : "");
	free(buf);
	logstore_write("cixd", "info", "pkg recipe commit %s@%s: committed %s as %s",
	               g_recipe_commit.name, g_recipe_commit.version, g_recipe_commit.repo_path,
	               g_recipe_commit.commit_sha);

	/* The commit makes this source offer the package now; the next sync
	 * would say so too, but the publish below may queue a build first,
	 * and that build expands {{REPO_TOKEN}} with the owner's token. */
	{
		char item[PKG_SOURCE_ITEM_MAX];

		snprintf(item, sizeof(item), "package:%s", g_recipe_commit.name);
		pkgsource_offers_add(g_recipe_commit.source, item);
		if (g_recipe_commit.choose) {
			char cerr[256];

			pkgsource_choose(item, g_recipe_commit.source, cerr, sizeof(cerr));
		}
	}

	/* Git first, then here. A publish that fails now is not a lost
	 * revision: the commit exists, and the next recipe sync adds it. */
	perr = pkg_recipe_add(g_recipe_commit.name, g_recipe_commit.content);
	if (perr != PKG_OK) {
		snprintf(g_recipe_commit.error, sizeof(g_recipe_commit.error),
		         "committed as %s but not published here (%s) -- the next recipe sync "
		         "adds it",
		         g_recipe_commit.commit_sha,
		         pkg_recipe_add_last_error()[0] != '\0' ? pkg_recipe_add_last_error()
		                                                : "publish refused");
		g_recipe_commit.state = RECIPE_COMMIT_FAILED;
		logstore_write("cixd", "error", "pkg recipe commit %s@%s: %s", g_recipe_commit.name,
		               g_recipe_commit.version, g_recipe_commit.error);
		return;
	}
	g_recipe_commit.published = 1;
	g_recipe_commit.state = RECIPE_COMMIT_DONE;
}

void pkg_recipe_commit_write_json(struct json_writer *w)
{
	static const char *const names[] = { "never", "running", "done", "failed" };

	jw_obj_open(w);
	jw_key(w, "state");
	jw_str(w, names[g_recipe_commit.state]);
	jw_key(w, "name");
	jw_str(w, g_recipe_commit.name);
	jw_key(w, "version");
	jw_str(w, g_recipe_commit.version);
	jw_key(w, "source");
	jw_str(w, g_recipe_commit.source);
	jw_key(w, "path");
	jw_str(w, g_recipe_commit.repo_path);
	jw_key(w, "commit");
	jw_str(w, g_recipe_commit.commit_sha);
	jw_key(w, "published");
	jw_bool(w, g_recipe_commit.published);
	jw_key(w, "error");
	jw_str(w, g_recipe_commit.error);
	jw_key(w, "started_at");
	jw_int(w, (long long)g_recipe_commit.started_at);
	jw_key(w, "finished_at");
	jw_int(w, (long long)g_recipe_commit.finished_at);
	jw_obj_close(w);
}

/*
 * ---- approval write-back (ADR-0324) ----
 *
 * Git is authoritative for recipes, and a change made on a host writes
 * back -- including the artifact approval #492 writes into a recipe
 * after a build (the owner, 2026-10-02). Without it the approval stays
 * on this host: no other host takes the cache hit, and the stored text
 * quietly differs from git on every built package.
 *
 * Two ways in. After a build, approve_published_artifact() queues the
 * version under the path `recipe commit` itself writes. And every sync
 * queues each version whose stored copy has an approval git's lacks,
 * under the filename the sync actually read -- the sync is the
 * reconciler, so a guess that missed, a forge that was down, or a
 * write that lost a race is retried there, with no state of its own.
 * Only for a source this host may write with a token: a host reading
 * someone else's catalogue keeps its approvals to itself.
 *
 * One helper at a time, one commit per batch of one source: every
 * commit to the owner's forge is mirrored to the public catalogue. The
 * guard runs in the helper against git's bytes at write time: the store
 * text (redacted -- 24 stored recipes predate {{REPO_TOKEN}}, #405) is
 * written only when git's copy is the same recipe without an approval,
 * and the update names the blob it read, so a file changed meanwhile is
 * refused by the forge rather than overwritten.
 */
struct writeback_item {
	char name[PKG_NAME_MAX];
	char version[PKG_VERSION_MAX];
	char source[PKG_SOURCE_NAME_MAX];
	char path[PATH_MAX]; /* in the repository */
};

#define WRITEBACK_BATCH_MAX 256

static struct writeback_item *g_wb_queue;
static int g_wb_count;
static int g_wb_cap;
static struct {
	int running;
	int batch; /* queue items the running helper took */
	int written;
	int left;  /* not written: git already had it, or holds something else */
	int failed;
	char last_commit[FORGE_COMMIT_SHA_MAX];
	char error[PKG_ERROR_MAX];
	time_t last_at;
} g_wb;

static void writeback_result_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/approval-writeback.result", g_pkg_dir);
}

static void writeback_enqueue(const char *name, const char *version, const char *source,
                              const char *path)
{
	int i;

	for (i = 0; i < g_wb_count; i++)
		if (strcmp(g_wb_queue[i].name, name) == 0 &&
		    strcmp(g_wb_queue[i].version, version) == 0)
			return;
	if (g_wb_count == g_wb_cap) {
		int cap = g_wb_cap == 0 ? 64 : g_wb_cap * 2;
		struct writeback_item *n = realloc(g_wb_queue, (size_t)cap * sizeof(*n));

		if (n == NULL)
			return;
		g_wb_queue = n;
		g_wb_cap = cap;
	}
	snprintf(g_wb_queue[g_wb_count].name, sizeof(g_wb_queue[0].name), "%s", name);
	snprintf(g_wb_queue[g_wb_count].version, sizeof(g_wb_queue[0].version), "%s", version);
	snprintf(g_wb_queue[g_wb_count].source, sizeof(g_wb_queue[0].source), "%s", source);
	snprintf(g_wb_queue[g_wb_count].path, sizeof(g_wb_queue[0].path), "%s", path);
	g_wb_count++;
}

/* The source a host may write this package's approval back to, or NULL. */
static const struct pkg_source *writeback_source_for(const char *name)
{
	char item[PKG_SOURCE_ITEM_MAX];
	char owner[PKG_SOURCES_MAX * PKG_SOURCE_NAME_MAX];
	const struct pkg_source *s;

	snprintf(item, sizeof(item), "package:%s", name);
	if (pkgsource_owner_of(item, owner, sizeof(owner)) != PKGSOURCE_OWNER_ONE)
		return NULL;
	s = pkgsource_find(owner);
	return s != NULL && s->write && s->token[0] != '\0' ? s : NULL;
}

/* After a build approved its artifact: the path `recipe commit` writes. */
static void writeback_after_approval(const char *name, const char *version)
{
	const struct pkg_source *s = writeback_source_for(name);
	char path[PATH_MAX];

	if (s == NULL)
		return;
	snprintf(path, sizeof(path), "recipes/package/%s@%s%s", name, version,
	         PKG_RECIPE_CBS_SUFFIX);
	writeback_enqueue(name, version, s->name, path);
}

/*
 * The sync's half. The merge runs in a helper, so what it finds travels
 * to the parent in a file, as its counts do: one line per version,
 * "<name> <version> <source> <path in the repository>".
 */
static void writeback_list_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/sync-writeback.list", g_pkg_dir);
}

static void writeback_record(const char *name, const char *version, const char *source,
                             const char *filename)
{
	const struct pkg_source *s = pkgsource_find(source);
	char path[PATH_MAX];
	FILE *fp;

	if (s == NULL || !s->write || s->token[0] == '\0')
		return;
	writeback_list_path(path, sizeof(path));
	fp = fopen(path, "a");
	if (fp == NULL)
		return;
	fprintf(fp, "%s %s %s recipes/package/%s\n", name, version, source, filename);
	fclose(fp);
}

/* In the parent, once the sync has finished: queue what it found. */
static void writeback_take_sync_list(void)
{
	char path[PATH_MAX];
	char *buf = NULL, *line, *save = NULL;
	size_t len = 0;

	writeback_list_path(path, sizeof(path));
	if (persist_read_file(path, &buf, &len) == 0 && buf != NULL) {
		for (line = strtok_r(buf, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
			char name[PKG_NAME_MAX], version[PKG_VERSION_MAX], source[PKG_SOURCE_NAME_MAX];
			char rpath[PATH_MAX];

			if (sscanf(line, "%63s %63s %63s %4095s", name, version, source, rpath) == 4)
				writeback_enqueue(name, version, source, rpath);
		}
		free(buf);
	}
	unlink(path);
}

int pkg_approval_writeback_ready(void)
{
	int i;

	if (g_wb.running || g_wb_count == 0 || g_recipe_commit.state == RECIPE_COMMIT_RUNNING)
		return 0;
	for (i = 1; i < g_wb_count && i < WRITEBACK_BATCH_MAX; i++)
		if (strcmp(g_wb_queue[i].source, g_wb_queue[0].source) != 0)
			break;
	g_wb.batch = i;
	g_wb.running = 1;
	return 1;
}

void pkg_approval_writeback_abort(const char *why)
{
	g_wb.running = 0;
	g_wb.batch = 0;
	g_wb.last_at = time(NULL);
	snprintf(g_wb.error, sizeof(g_wb.error), "%s", why);
	logstore_write("cixd", "error", "pkg: approval write-back: %s", why);
}

/*
 * The helper. Its result file: "commit <sha>" or "error <why>" on the
 * first line, then one "<outcome> <name>@<version>" line per item,
 * outcome one of written, unchanged, missing, differs.
 */
int pkg_approval_writeback_work(void *unused)
{
	typedef char item_line[PKG_NAME_MAX + PKG_VERSION_MAX + 16];
	struct forge_file_update *updates;
	char **texts;
	item_line *lines;
	char result_path[PATH_MAX];
	char err[PKG_ERROR_MAX];
	char sha[FORGE_COMMIT_SHA_MAX];
	char host[256];
	struct source_forge f;
	char *buf;
	size_t cap, off = 0;
	int i, done = 0, n = 0, rc = 0;
	long status = 0;

	(void)unused;
	writeback_result_path(result_path, sizeof(result_path));
	err[0] = '\0';
	updates = calloc((size_t)g_wb.batch, sizeof(*updates));
	texts = calloc((size_t)g_wb.batch, sizeof(*texts));
	lines = calloc((size_t)g_wb.batch, sizeof(*lines));
	if (updates == NULL || texts == NULL || lines == NULL) {
		snprintf(err, sizeof(err), "out of memory");
		rc = 1;
	} else if (source_forge_target(g_wb_queue[0].source, &f, err, sizeof(err)) != 0) {
		rc = 1;
	}
	for (i = 0; rc == 0 && i < g_wb.batch; i++) {
		const struct writeback_item *it = &g_wb_queue[i];
		char recipe_path[PATH_MAX], blob[FORGE_COMMIT_SHA_MAX];
		char git_sha[PKG_SHA256_MAX], store_sha[PKG_SHA256_MAX];
		char *store = NULL, *git = NULL;
		size_t store_len = 0, git_len = 0;
		const char *outcome = "differs";
		int g;

		snprintf(recipe_path, sizeof(recipe_path), "%s/%s/%s/%s", g_recipes_dir, it->name,
		         it->version, PKG_RECIPE_CBS_FILE);
		if (persist_read_file(recipe_path, &store, &store_len) != 0 || store == NULL) {
			outcome = "missing";
		} else {
			redact_repo_token(store, store_len + 1);
			g = forge_get_file(&f.t, it->path, g_pkg_dir, blob, sizeof(blob), &git, &git_len,
			                   &status, err, sizeof(err));
			if (g < 0) {
				free(store);
				rc = 1;
				break;
			}
			if (g == 1) {
				outcome = "missing";
			} else {
				redact_repo_token(git, git_len + 1);
				git_sha[0] = store_sha[0] = '\0';
				if (cbs_lines_equal(git, store, 0, git_sha, store_sha)) {
					if (git_sha[0] == '\0' && store_sha[0] != '\0') {
						updates[n].path = it->path;
						updates[n].content = store;
						updates[n].content_len = strlen(store);
						updates[n].blob_sha = strdup(blob);
						texts[n++] = store;
						store = NULL;
						outcome = "written";
					} else if (strcmp(git_sha, store_sha) == 0) {
						outcome = "unchanged";
					}
				}
			}
		}
		free(store);
		free(git);
		snprintf(lines[done++], sizeof(lines[0]), "%s %s@%s", outcome, it->name, it->version);
	}
	for (i = 0; rc == 0 && i < n; i++)
		if (updates[i].blob_sha == NULL) {
			snprintf(err, sizeof(err), "out of memory");
			rc = 1;
		}
	if (rc == 0 && n > 0) {
		char *message;
		size_t mlen = 256 + (size_t)n * sizeof(lines[0]), moff;

		if (gethostname(host, sizeof(host)) != 0)
			snprintf(host, sizeof(host), "%s", "unknown host");
		host[sizeof(host) - 1] = '\0';
		message = malloc(mlen);
		if (message == NULL) {
			snprintf(err, sizeof(err), "out of memory");
			rc = 1;
		} else {
			moff = (size_t)snprintf(message, mlen,
			                        "%d artifact approval(s) written back by cixd on %s "
			                        "(ADR-0324)\n",
			                        n, host);
			for (i = 0; i < done; i++)
				if (strncmp(lines[i], "written ", 8) == 0)
					moff += (size_t)snprintf(message + moff, mlen - moff, "\n%s",
					                         lines[i] + 8);
			if (forge_update_files(&f.t, updates, n, message, g_pkg_dir, sha, sizeof(sha),
			                       &status, err, sizeof(err)) != 0)
				rc = 1;
			free(message);
		}
	}

	/* The result: commit or error, then one line per item examined. */
	cap = 64 + sizeof(err) + (size_t)done * (sizeof(lines[0]) + 1);
	buf = malloc(cap);
	if (buf != NULL) {
		if (rc != 0) {
			redact_repo_token(err, sizeof(err));
			off = (size_t)snprintf(buf, cap, "error %s\n", err[0] != '\0' ? err : "unknown");
		} else {
			off = (size_t)snprintf(buf, cap, "commit %s\n", n > 0 ? sha : "-");
		}
		for (i = 0; rc == 0 && i < done; i++)
			off += (size_t)snprintf(buf + off, cap - off, "%s\n", lines[i]);
		persist_atomic_write(result_path, buf, off);
		free(buf);
	}
	for (i = 0; i < n; i++) {
		free(texts[i]);
		free((char *)updates[i].blob_sha);
	}
	free(texts);
	free(updates);
	free(lines);
	return rc;
}

void pkg_approval_writeback_done(int exit_status, void *unused)
{
	char result_path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;
	char *line, *save = NULL;
	int first = 1, k;

	(void)unused;
	(void)exit_status;
	writeback_result_path(result_path, sizeof(result_path));
	g_wb.last_at = time(NULL);
	if (persist_read_file(result_path, &buf, &len) != 0 || buf == NULL) {
		snprintf(g_wb.error, sizeof(g_wb.error), "the write-back helper left no result");
		g_wb.failed += g_wb.batch;
	} else {
		for (line = strtok_r(buf, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
			if (first) {
				first = 0;
				if (strncmp(line, "commit ", 7) == 0) {
					g_wb.error[0] = '\0';
					if (strcmp(line + 7, "-") != 0) {
						snprintf(g_wb.last_commit, sizeof(g_wb.last_commit), "%s", line + 7);
						logstore_write("cixd", "info", "pkg: approval write-back to %s: commit %s",
						               g_wb_queue[0].source, g_wb.last_commit);
					}
				} else {
					snprintf(g_wb.error, sizeof(g_wb.error), "%s",
					         strncmp(line, "error ", 6) == 0 ? line + 6 : line);
					g_wb.failed += g_wb.batch;
					logstore_write("cixd", "error",
					               "pkg: approval write-back of %d version(s) to %s failed: %s",
					               g_wb.batch, g_wb_queue[0].source, g_wb.error);
					break;
				}
				continue;
			}
			if (strncmp(line, "written ", 8) == 0)
				g_wb.written++;
			else if (strncmp(line, "unchanged ", 10) == 0)
				continue; /* git already holds the same approval */
			else {
				int missing = strncmp(line, "missing ", 8) == 0;

				g_wb.left++;
				logstore_write("cixd", "warn",
				               "pkg: approval write-back of %s to %s: not written -- %s",
				               strchr(line, ' ') + 1, g_wb_queue[0].source,
				               missing ? "no file under that path; the next sync names the "
				                         "real one"
				                       : "git's copy is not this version's text without an "
				                         "approval, a real divergence for an operator");
			}
		}
		free(buf);
	}
	unlink(result_path);
	/* Done either way: whatever was not written, the next sync finds again. */
	for (k = g_wb.batch; k < g_wb_count; k++)
		g_wb_queue[k - g_wb.batch] = g_wb_queue[k];
	g_wb_count -= g_wb.batch;
	g_wb.batch = 0;
	g_wb.running = 0;
}

void pkg_approval_writeback_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "pending");
	jw_int(w, g_wb_count);
	jw_key(w, "running");
	jw_bool(w, g_wb.running);
	jw_key(w, "written");
	jw_int(w, g_wb.written);
	jw_key(w, "not_written");
	jw_int(w, g_wb.left);
	jw_key(w, "failed");
	jw_int(w, g_wb.failed);
	jw_key(w, "last_commit");
	if (g_wb.last_commit[0] != '\0')
		jw_str(w, g_wb.last_commit);
	else
		jw_null(w);
	jw_key(w, "error");
	if (g_wb.error[0] != '\0')
		jw_str(w, g_wb.error);
	else
		jw_null(w);
	jw_obj_close(w);
}

/*
 * Three small, explicitly separate branches, one per forge -- not one
 * "generic REST archive API" abstraction, because there isn't one:
 * gitea/github/gitlab each have a genuinely different URL shape and
 * auth convention (confirmed directly before choosing this design,
 * see ADR-0121). gitea is exercised by every sync on 192.168.15.95
 * (git.home.arpa). github.com was measured there on 2026-09-29: repo
 * github.com/The-Cix-Project/cix-recipes, ref main, NO token --
 * POST /v1/pkg/sync ended state=success, added=28 skipped=522. That is
 * the ADR-0315 default, so the anonymous public path is the one
 * proven. gitlab follows GitLab's documented API and has not been run
 * against a real instance.
 */
static int build_sync_fetch_request(const struct pkg_source *s, char *out_url,
                                     size_t out_url_size, char *out_header, size_t out_header_size)
{
	char scheme[16], host[256], owner[256], repo[256];
	const char *ref = s->ref[0] != '\0' ? s->ref : PKG_SOURCE_DEFAULT_REF;

	out_header[0] = '\0';
	if (parse_repo_url(s->url, scheme, sizeof(scheme), host, sizeof(host), owner, sizeof(owner),
	                   repo, sizeof(repo)) != 0)
		return -1;

	if (strcmp(s->kind, "gitea") == 0) {
		/* Token-in-URL basic auth, same proven convention ADR-0057's
		 * own cix.recipe pkg_source already relies on. */
		if (s->token[0] != '\0')
			snprintf(out_url, out_url_size, "%s://%s@%s/api/v1/repos/%s/%s/archive/%s.tar.gz",
			         scheme, s->token, host, owner, repo, ref);
		else
			snprintf(out_url, out_url_size, "%s://%s/api/v1/repos/%s/%s/archive/%s.tar.gz",
			         scheme, host, owner, repo, ref);
	} else if (strcmp(s->kind, "github") == 0) {
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
		if (s->token[0] != '\0')
			snprintf(out_header, out_header_size, "Authorization: Bearer %s", s->token);
	} else if (strcmp(s->kind, "gitlab") == 0) {
		/* Unlike github, gitlab.com and a self-hosted GitLab instance
		 * both use the identical <host>/api/v4/ convention -- no
		 * separate-domain special case needed here. */
		snprintf(out_url, out_url_size,
		         "%s://%s/api/v4/projects/%s%%2F%s/repository/archive.tar.gz?sha=%s", scheme,
		         host, owner, repo, ref);
		if (s->token[0] != '\0')
			snprintf(out_header, out_header_size, "PRIVATE-TOKEN: %s", s->token);
	} else {
		return -1;
	}
	return 0;
}

/*
 * ADR-0324: one archive, one error sidecar and one extraction directory
 * per source, by its position in the list for this sync. Positions, not
 * names, because a name is the operator's text and a path is not the
 * place to find out what characters it can hold.
 */
static void sync_state_path(int i, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/sync-%d.tar.gz", g_pkg_dir, i);
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

/* Sidecar the child writes a source's curlfetch_perform() error to
 * (#410) -- the parent otherwise has only an exit code. */
static void sync_fetch_err_path(int i, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/sync-%d.err", g_pkg_dir, i);
}

/*
 * ADR-0324: every source is fetched, in list order, by one child. A
 * source that fails is recorded in its sidecar and the next one is
 * fetched anyway: one forge being down is no reason to skip the rest,
 * and a failed source keeps what it offered last time, so a network
 * failure never moves who owns a package.
 */
enum pkg_error pkg_sync_start(pid_t *out_pid, int *out_pidfd)
{
	int count = pkgsource_count();
	pid_t pid;
	int pidfd;
	int i;

	if (g_sync_pid > 0)
		return PKG_ERR_BUSY;
	if (count == 0)
		return PKG_ERR_NOT_FOUND;

	for (i = 0; i < PKG_SOURCES_MAX; i++) {
		char path[PATH_MAX];

		sync_state_path(i, path, sizeof(path));
		unlink(path);
		sync_fetch_err_path(i, path, sizeof(path));
		unlink(path);
	}

	pid = fork();
	if (pid < 0)
		return PKG_ERR_SPAWN_FAILED;
	if (pid == 0) {
		for (i = 0; i < count; i++) {
			const struct pkg_source *s = pkgsource_at(i);
			char url[PKG_SOURCE_URL_MAX + PKG_SOURCE_TOKEN_MAX + 64];
			char header[320];
			char tarball_path[PATH_MAX], err_path[PATH_MAX];
			char curl_err[256];
			struct curlfetch_opts opts;
			int failed;

			sync_state_path(i, tarball_path, sizeof(tarball_path));
			sync_fetch_err_path(i, err_path, sizeof(err_path));
			if (build_sync_fetch_request(s, url, sizeof(url), header, sizeof(header)) != 0) {
				snprintf(curl_err, sizeof(curl_err), "the source url does not name an owner "
				                                     "and repository");
				failed = 1;
			} else {
				memset(&opts, 0, sizeof(opts));
				opts.url = url;
				opts.path = tarball_path;
				opts.header1 = header[0] != '\0' ? header : NULL;
				opts.connect_timeout = PKG_CURL_CONNECT_TIMEOUT_SECS;
				opts.low_speed_limit = PKG_CURL_LOW_SPEED_LIMIT;
				opts.low_speed_time = PKG_CURL_LOW_SPEED_TIME_SECS;
				failed = curlfetch_perform(&opts, NULL, curl_err, sizeof(curl_err)) != 0;
			}
			if (failed) {
				int efd;

				unlink(tarball_path);
				redact_repo_token(curl_err, sizeof(curl_err));
				redact_url_userinfo(curl_err, sizeof(curl_err));
				efd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
				if (efd >= 0) {
					ssize_t ignored = write(efd, curl_err, strlen(curl_err));

					(void)ignored;
					close(efd);
				}
			}
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
 * ADR-0324: whether `source` owns `kind:name` and may supply it. A name
 * several sources offer, with no operator choice, is owned by none of
 * them: it is merged from nowhere and counted as held, so the sync says
 * so instead of letting list order pick.
 */
static int sync_source_owns(const char *kind, const char *name, const char *source, int *held)
{
	char item[PKG_SOURCE_ITEM_MAX];
	char owner[PKG_SOURCES_MAX * PKG_SOURCE_NAME_MAX];

	snprintf(item, sizeof(item), "%s:%s", kind, name);
	switch (pkgsource_owner_of(item, owner, sizeof(owner))) {
	case PKGSOURCE_OWNER_ONE:
		return strcmp(owner, source) == 0;
	case PKGSOURCE_OWNER_CONFLICT:
		(*held)++;
		return 0;
	default:
		return 0;
	}
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
/*
 * #505: the recipe repository is FLAT, and a recipe is one file named
 * "<name>@<version>.<ext>" -- sh for a shell recipe, cbs for a CBS one
 * (ADR-0305), json for a deployment definition.
 *
 * It was "<name>/<version>/build.<ext>" until 2026-09-21: two
 * directory levels carrying two fields, under a leaf filename that was
 * the same two strings 1577 times over. Every editor tab, grep hit and
 * diff header said "build.cbs" and the path was the only thing that
 * said which recipe it was. Measured before the change: every one of
 * those 1577 directories held exactly one file and nothing else, so
 * the directories carried no information the filename could not.
 *
 * Splits at the FIRST '@'. That is unambiguous because no package,
 * image or deployment name contains one -- checked across all 155
 * names -- and because '@' is already this platform's name/version
 * separator wherever it prints one (glibc@2.44-14, kmod@cix-builder).
 * The version may contain dots (xorriso@1.5.8.pl02-8.cbs), so the
 * extension is taken from the LAST dot, never the first.
 *
 * Returns 0 and fills name, version and ext on a match; -1 otherwise,
 * which is how a README or any other stray file in the directory is
 * skipped rather than mistaken for a recipe.
 */
static int recipe_file_split(const char *fname, char *name, size_t name_size,
                             char *version, size_t version_size, const char **ext)
{
	const char *at, *dot;
	size_t nlen, vlen;

	if (fname == NULL || fname[0] == '.')
		return -1;
	at = strchr(fname, '@');
	if (at == NULL || at == fname)
		return -1;
	dot = strrchr(at, '.');
	if (dot == NULL || dot == at + 1)
		return -1;
	nlen = (size_t)(at - fname);
	vlen = (size_t)(dot - (at + 1));
	if (nlen == 0 || nlen >= name_size)
		return -1;
	if (vlen == 0 || vlen >= version_size)
		return -1;
	memcpy(name, fname, nlen);
	name[nlen] = '\0';
	memcpy(version, at + 1, vlen);
	version[vlen] = '\0';
	if (ext != NULL)
		*ext = dot + 1;
	return 0;
}

/* ADR-0324 step C: defined with the rest of the catalogue code, below. */
static int sync_vouched(const struct catalogue *cat, const char *rel_dir, const char *name,
                        const char *path, const char *source);

static void sync_walk_image_recipes(const char *images_root, int *added, int *skipped,
                                     int *failed, const char *source, int *held,
                                     const struct catalogue *cat)
{
	DIR *names_d;
	struct dirent *name_de;

	/*
	 * #505: one flat directory. The old shape opened a directory per
	 * name and scanned its versions; now every "<name>@<version>.json"
	 * sits side by side. An image recipe is name-keyed at the daemon
	 * layer (ADR-0123), so exactly one version per name may win, and
	 * the rule is the same as before: the highest.
	 *
	 * Expressed as "this entry wins unless a higher version of the
	 * same name exists", which needs no bookkeeping across the outer
	 * loop and cannot pick two winners. It rescans the directory per
	 * entry, which is quadratic and deliberately so -- there are 58
	 * image recipe versions in the whole corpus, and a clear rule
	 * beats a clever one at that size.
	 *
	 * JSON since cix#569. ADR-0311 made image recipes JSON on 2026-09-25
	 * and gave the daemon a JSON parser, but this walk kept matching .sh,
	 * so no .json image recipe ever reached a host: 192.168.15.95 held
	 * cix-builder at 6.1.0, its last .sh, while git had 6.1.4 (measured
	 * 2026-10-03). Its four follow-and-converge images already had the
	 * latest JSON pins installed by other routes, so the switch installs
	 * nothing there.
	 */
	names_d = opendir(images_root);
	if (names_d == NULL)
		return;
	while ((name_de = readdir(names_d)) != NULL) {
		char name[PKG_NAME_MAX], version[PKG_VERSION_MAX];
		char recipe_file[PATH_MAX];
		char *content;
		size_t content_len;
		struct dirent *vde;
		DIR *vd;
		int outranked = 0;
		const char *ext;
		enum pkg_error rc;

		if (recipe_file_split(name_de->d_name, name, sizeof(name), version,
		                      sizeof(version), &ext) != 0)
			continue;
		if (strcmp(ext, "json") != 0)
			continue;
		if (!sync_source_owns("image", name, source, held))
			continue;

		vd = opendir(images_root);
		if (vd == NULL)
			continue;
		while ((vde = readdir(vd)) != NULL) {
			char n2[PKG_NAME_MAX], v2[PKG_VERSION_MAX];
			const char *e2;

			if (recipe_file_split(vde->d_name, n2, sizeof(n2), v2, sizeof(v2), &e2) != 0)
				continue;
			if (strcmp(e2, "json") != 0 || strcmp(n2, name) != 0)
				continue;
			if (pkg_version_compare(v2, version) > 0) {
				outranked = 1;
				break;
			}
		}
		closedir(vd);
		if (outranked)
			continue;

		snprintf(recipe_file, sizeof(recipe_file), "%s/%s", images_root, name_de->d_name);
		if (!sync_vouched(cat, "recipes/image", name_de->d_name, recipe_file, source)) {
			(*failed)++;
			continue;
		}
		if (persist_read_file(recipe_file, &content, &content_len) != 0 || content == NULL)
			continue;
		/* Held already, byte for byte: skipped, not rewritten. The add
		 * overwrites by name and never answers DUPLICATE, so without
		 * this every unchanged recipe was counted "added" on every sync
		 * (28 a sync on 192.168.15.95, 2026-10-02, while the store did
		 * not change). */
		{
			char *have = NULL;
			size_t have_len = 0;
			int same = image_recipe_get(name, &have, &have_len) == PKG_OK && have != NULL &&
			           strcmp(have, content) == 0;

			free(have);
			if (same) {
				free(content);
				(*skipped)++;
				continue;
			}
		}
		rc = image_recipe_add(name, content);
		free(content);
		/* image_recipe_add() overwrites by name (ADR-0123) and never answers
		 * PKG_ERR_DUPLICATE; identical content was skipped above, so what
		 * reaches it is a real change. */
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
                                         int *failed, const char *source, int *held,
                                         const struct catalogue *cat)
{
	DIR *names_d;
	struct dirent *name_de;

	/* #505: flat, and the same "wins unless outranked" rule as the
	 * image walk above. The extension is json, not sh, because a
	 * container recipe's content is a JSON body. */
	names_d = opendir(containers_root);
	if (names_d == NULL)
		return;
	while ((name_de = readdir(names_d)) != NULL) {
		char name[PKG_NAME_MAX], version[PKG_VERSION_MAX];
		char json_path[PATH_MAX];
		char *content;
		size_t content_len;
		struct dirent *vde;
		DIR *vd;
		int outranked = 0;
		const char *ext;
		enum pkg_error rc;

		if (recipe_file_split(name_de->d_name, name, sizeof(name), version,
		                      sizeof(version), &ext) != 0)
			continue;
		if (strcmp(ext, "json") != 0)
			continue;
		if (!sync_source_owns("deployment", name, source, held))
			continue;

		vd = opendir(containers_root);
		if (vd == NULL)
			continue;
		while ((vde = readdir(vd)) != NULL) {
			char n2[PKG_NAME_MAX], v2[PKG_VERSION_MAX];
			const char *e2;

			if (recipe_file_split(vde->d_name, n2, sizeof(n2), v2, sizeof(v2), &e2) != 0)
				continue;
			if (strcmp(e2, "json") != 0 || strcmp(n2, name) != 0)
				continue;
			if (pkg_version_compare(v2, version) > 0) {
				outranked = 1;
				break;
			}
		}
		closedir(vd);
		if (outranked)
			continue;

		snprintf(json_path, sizeof(json_path), "%s/%s", containers_root, name_de->d_name);
		if (!sync_vouched(cat, "recipes/deployment", name_de->d_name, json_path, source)) {
			(*failed)++;
			continue;
		}
		if (persist_read_file(json_path, &content, &content_len) != 0 || content == NULL)
			continue;
		/* Held already, byte for byte: skipped, not rewritten. The add
		 * overwrites by name and never answers DUPLICATE, so without
		 * this every unchanged recipe was counted "added" on every sync
		 * (28 a sync on 192.168.15.95, 2026-10-02, while the store did
		 * not change). */
		{
			char *have = NULL;
			size_t have_len = 0;
			int same = container_recipe_get(name, &have, &have_len) == PKG_OK && have != NULL &&
			           strcmp(have, content) == 0;

			free(have);
			if (same) {
				free(content);
				(*skipped)++;
				continue;
			}
		}
		rc = container_recipe_add(name, content);
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

void pkg_trusted_keys_init(const char *dir)
{
	snprintf(g_trusted_keys_dir, sizeof(g_trusted_keys_dir), "%s", dir);
}

/*
 * ADR-0324: what a sync did, per source. Filled in the parent from what
 * the fetch child left on disk (pkg_sync_fetch_done()) and what the
 * merge helper left on disk (pkg_sync_completed()) -- never from the
 * helper's memory, which is a copy that dies with it (ADR-0278).
 */
struct sync_source_result {
	char name[PKG_SOURCE_NAME_MAX];
	int fetched;
	char error[256];
	int added, skipped, divergent, failed, held;
	int refreshed; /* git refreshed a comment-only or approval-only difference */
};
static struct sync_source_result g_sync_results[PKG_SOURCES_MAX];
static int g_sync_result_count;
static int g_sync_last_divergent;
static int g_sync_last_refreshed;
static int g_sync_last_held;

static void sync_extract_dir(int i, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/sync-extract/%d", g_pkg_dir, i);
}

static void sync_results_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/sync-extract.results", g_pkg_dir);
}

/*
 * ADR-0278 (#367) and ADR-0324: the forkable first half of a sync --
 * unpack every source's archive into its own directory.
 *
 * Forked because this is where the five seconds went: unpacking the
 * whole recipe repository synchronously on a pid-1 daemon's only loop
 * stalled it 5062-6116 ms every six hours (measured, four days running).
 *
 * It runs in a HELPER CHILD, and so does the merge after it -- both in
 * pkg_sync_extract_work() (main.c). That is safe because neither leaves
 * an answer in memory: the store is files, rolling rebuilds are
 * re-derived from them in the parent (pkg_rebuild_queue_rederive()),
 * and what each source offered and did is written to disk for
 * pkg_sync_completed() to read. Returns 0, or -1 if the extraction
 * root cannot be made. A source whose archive will not unpack is
 * recorded and skipped; the others still sync.
 */
int pkg_sync_extract(void)
{
	char root[PATH_MAX];
	int i;

	snprintf(root, sizeof(root), "%s/sync-extract", g_pkg_dir);
	cix_btrfs_subvol_delete_or_rmtree(root);
	if (persist_mkdir_p(root) != 0)
		return -1;
	for (i = 0; i < pkgsource_count(); i++) {
		char tarball_path[PATH_MAX], dir[PATH_MAX];

		sync_state_path(i, tarball_path, sizeof(tarball_path));
		if (access(tarball_path, F_OK) != 0)
			continue;
		sync_extract_dir(i, dir, sizeof(dir));
		if (persist_mkdir_p(dir) != 0 || extract_tarball(tarball_path, dir) != 0) {
			char err_path[PATH_MAX];
			static const char why[] = "the archive did not unpack";

			cix_btrfs_subvol_delete_or_rmtree(dir);
			sync_fetch_err_path(i, err_path, sizeof(err_path));
			persist_atomic_write(err_path, why, sizeof(why) - 1);
		}
	}
	return 0;
}

/* Every "<kind>:<name>" one directory of recipes offers, deduplicated. */
static int sync_collect_offers(const char *dir, const char *kind, const char *ext_a,
                               const char *ext_b, char ***items, int *count, int *cap)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	if (d == NULL)
		return 0;
	while ((de = readdir(d)) != NULL) {
		char name[PKG_NAME_MAX], version[PKG_VERSION_MAX];
		char item[PKG_SOURCE_ITEM_MAX];
		const char *ext;
		int j, seen = 0;

		if (recipe_file_split(de->d_name, name, sizeof(name), version, sizeof(version), &ext) !=
		    0)
			continue;
		if (strcmp(ext, ext_a) != 0 && (ext_b == NULL || strcmp(ext, ext_b) != 0))
			continue;
		snprintf(item, sizeof(item), "%s:%s", kind, name);
		for (j = 0; j < *count && !seen; j++)
			seen = strcmp((*items)[j], item) == 0;
		if (seen)
			continue;
		if (*count == *cap) {
			int n = *cap == 0 ? 256 : *cap * 2;
			char **grown = realloc(*items, (size_t)n * sizeof(**items));

			if (grown == NULL) {
				closedir(d);
				return -1;
			}
			*items = grown;
			*cap = n;
		}
		(*items)[*count] = strdup(item);
		if ((*items)[*count] == NULL) {
			closedir(d);
			return -1;
		}
		(*count)++;
	}
	closedir(d);
	return 0;
}

/*
 * ---- the signed catalogue (ADR-0324 step C) ----
 *
 * Runs in the sync helper, against the fetched tree -- the bytes a host
 * is about to trust -- never against the local store.
 */

/* Whether the signed index vouches for one file a sync is about to take. */
static int sync_vouched(const struct catalogue *cat, const char *rel_dir, const char *name,
                        const char *path, const char *source)
{
	char rel[PATH_MAX], sha[CATALOGUE_SHA256_HEX];

	if (cat == NULL)
		return 1; /* the source is unsigned by the operator's choice */
	snprintf(rel, sizeof(rel), "%s/%s", rel_dir, name);
	if (pkg_run_capture_sha256(path, sha, sizeof(sha)) == 0 && catalogue_vouches(cat, rel, sha))
		return 1;
	logstore_write("cixd", "error",
	               "pkg sync: %s from source %s is not what its signed index lists -- refused "
	               "(ADR-0324)",
	               rel, source);
	return 0;
}

/* A one-key trust directory holding `pub`, for releasekey_verify_file(). */
static int catalogue_trust_dir(const char *name, const char *pub, char *out, size_t out_size)
{
	char path[PATH_MAX];

	snprintf(out, out_size, "%s/catalogue-trust/%s", g_pkg_dir, name);
	cix_btrfs_subvol_delete_or_rmtree(out);
	if (persist_mkdir_p(out) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/key.pub", out);
	return persist_atomic_write(path, pub, strlen(pub));
}

/*
 * The index in `dir` verified against `pub`: its signature, its trusted
 * comment naming exactly these bytes, and its time. 0 with *out_t, or -1
 * with why.
 */
static int catalogue_check(const char *name, const char *dir, const char *pub, long long *out_t,
                           char *why, size_t why_size)
{
	char index[PATH_MAX], sig[PATH_MAX], trust[PATH_MAX];
	char comment[256], sha[CATALOGUE_SHA256_HEX];
	enum releasekey_error rc;

	snprintf(index, sizeof(index), "%s/%s", dir, CATALOGUE_INDEX_PATH);
	snprintf(sig, sizeof(sig), "%s/%s", dir, CATALOGUE_SIG_PATH);
	if (access(index, F_OK) != 0 || access(sig, F_OK) != 0) {
		snprintf(why, why_size, "the tree carries no signed index (%s)", CATALOGUE_INDEX_PATH);
		return -1;
	}
	if (catalogue_trust_dir(name, pub, trust, sizeof(trust)) != 0) {
		snprintf(why, why_size, "could not stage the catalogue key");
		return -1;
	}
	rc = releasekey_verify_file(index, sig, trust, comment, sizeof(comment));
	if (rc != RELEASEKEY_OK) {
		snprintf(why, why_size, "its index signature does not verify: %s",
		         releasekey_strerror(rc));
		return -1;
	}
	if (pkg_run_capture_sha256(index, sha, sizeof(sha)) != 0 ||
	    catalogue_comment_parse(comment, sha, out_t) != 0) {
		snprintf(why, why_size, "its index signature names other bytes than the index");
		return -1;
	}
	return 0;
}

/*
 * The writing host's half (decision 1: .95 signs). When this host may
 * write the source and holds the catalogue key, and the fetched tree's
 * index is not a valid signature of exactly what the tree holds, it
 * writes a new index and signature into the tree and commits both, so
 * every reader takes the change. A tree already correctly signed is
 * left alone -- the index excludes itself, so committing it does not
 * change it, and this does not fire again on the next sync.
 */
static void catalogue_sign_if_writer(const struct pkg_source *s, const char *dir)
{
	char index[PATH_MAX], sig[PATH_MAX], pub[256], why[256], comment[256];
	char sha[CATALOGUE_SHA256_HEX], commit[FORGE_COMMIT_SHA_MAX], err[PKG_ERROR_MAX];
	char blobs[2][FORGE_COMMIT_SHA_MAX], host[256], message[512];
	struct forge_file_update files[2];
	struct source_forge f;
	char *built, *have = NULL, *texts[2] = { NULL, NULL };
	size_t built_len = 0, have_len = 0, text_len[2] = { 0, 0 };
	long long t, last;
	long status = 0;
	int k;

	if (!s->write || s->token[0] == '\0' || !releasekey_is_set_of(RELEASEKEY_CATALOGUE))
		return;
	built = catalogue_build(dir, pkg_run_capture_sha256, &built_len);
	if (built == NULL) {
		logstore_write("cixd", "error", "catalogue: could not index source %s", s->name);
		return;
	}
	snprintf(index, sizeof(index), "%s/%s", dir, CATALOGUE_INDEX_PATH);
	snprintf(sig, sizeof(sig), "%s/%s", dir, CATALOGUE_SIG_PATH);
	if (persist_read_file(index, &have, &have_len) == 0 && have != NULL &&
	    have_len == built_len && memcmp(have, built, built_len) == 0 &&
	    releasekey_public_of(RELEASEKEY_CATALOGUE, pub, sizeof(pub)) == RELEASEKEY_OK &&
	    catalogue_check(s->name, dir, pub, &t, why, sizeof(why)) == 0) {
		free(have);
		free(built);
		return;
	}
	free(have);

	/* Newer than anything this host has accepted, even under clock skew. */
	t = (long long)time(NULL);
	last = pkgsource_catalogue_time(s->name);
	if (t <= last)
		t = last + 1;
	if (persist_atomic_write(index, built, built_len) != 0 ||
	    pkg_run_capture_sha256(index, sha, sizeof(sha)) != 0 ||
	    catalogue_comment(sha, t, comment, sizeof(comment)) != 0 ||
	    releasekey_sign_file_of(RELEASEKEY_CATALOGUE, index, sig, comment) != RELEASEKEY_OK) {
		free(built);
		logstore_write("cixd", "error", "catalogue: could not sign the index of source %s",
		               s->name);
		return;
	}
	free(built);

	/* Git first: the tree here is signed; readers see it once it is committed. */
	if (source_forge_target(s->name, &f, err, sizeof(err)) != 0) {
		logstore_write("cixd", "error", "catalogue: %s", err);
		return;
	}
	memset(files, 0, sizeof(files));
	for (k = 0; k < 2; k++) {
		const char *rel = k == 0 ? CATALOGUE_INDEX_PATH : CATALOGUE_SIG_PATH;
		char local[PATH_MAX];
		char *old = NULL;
		size_t old_len = 0;
		int g;

		snprintf(local, sizeof(local), "%s/%s", dir, rel);
		if (persist_read_file(local, &texts[k], &text_len[k]) != 0 || texts[k] == NULL) {
			snprintf(err, sizeof(err), "could not read back %s", rel);
			goto fail;
		}
		g = forge_get_file(&f.t, rel, g_pkg_dir, blobs[k], sizeof(blobs[k]), &old, &old_len,
		                   &status, err, sizeof(err));
		free(old);
		if (g < 0)
			goto fail;
		files[k].path = rel;
		files[k].content = texts[k];
		files[k].content_len = text_len[k];
		files[k].blob_sha = g == 0 ? blobs[k] : NULL;
		files[k].operation = g == 0 ? "update" : "create";
	}
	if (gethostname(host, sizeof(host)) != 0)
		snprintf(host, sizeof(host), "%s", "unknown host");
	host[sizeof(host) - 1] = '\0';
	snprintf(message, sizeof(message),
	         "catalogue index signed by cixd on %s (ADR-0324)\n\n%s", host, comment);
	if (forge_update_files(&f.t, files, 2, message, g_pkg_dir, commit, sizeof(commit), &status,
	                       err, sizeof(err)) != 0)
		goto fail;
	logstore_write("cixd", "info", "catalogue: signed the index of source %s, commit %s",
	               s->name, commit);
	free(texts[0]);
	free(texts[1]);
	return;
fail:
	redact_repo_token(err, sizeof(err));
	logstore_write("cixd", "error",
	               "catalogue: signed the index of source %s here but could not commit it: %s "
	               "-- readers keep the previous index until the next sync",
	               s->name, err);
	free(texts[0]);
	free(texts[1]);
}

/*
 * The reading half. A source with a catalogue key yields its verified
 * index, or NULL with `refused` set and why; one without yields NULL,
 * unsigned, as every source was before (decision 4).
 */
static struct catalogue *catalogue_verify(const struct pkg_source *s, const char *dir,
                                          int *refused, char *why, size_t why_size)
{
	char index[PATH_MAX];
	char *text = NULL;
	size_t len = 0;
	long long t = 0, last;
	struct catalogue *cat;

	*refused = 0;
	if (s->catalogue_key[0] == '\0')
		return NULL;
	*refused = 1;
	if (catalogue_check(s->name, dir, s->catalogue_key, &t, why, why_size) != 0)
		return NULL;
	last = pkgsource_catalogue_time(s->name);
	if (t < last) {
		snprintf(why, why_size,
		         "its index was signed at %lld, older than the newest this host accepted (%lld) "
		         "-- an old tree replayed",
		         t, last);
		return NULL;
	}
	snprintf(index, sizeof(index), "%s/%s", dir, CATALOGUE_INDEX_PATH);
	if (persist_read_file(index, &text, &len) != 0 || text == NULL ||
	    (cat = catalogue_parse(text, len)) == NULL) {
		free(text);
		snprintf(why, why_size, "its index could not be read");
		return NULL;
	}
	free(text);
	pkgsource_catalogue_time_set(s->name, t);
	*refused = 0;
	return cat;
}

/* Merges one source's package recipes, only those it owns. */
static void sync_merge_packages(const char *recipes_root, const char *source,
                                const struct catalogue *cat, struct sync_source_result *res)
{
	DIR *names_d = opendir(recipes_root);
	struct dirent *name_de;

	if (names_d == NULL)
		return;
	while ((name_de = readdir(names_d)) != NULL) {
		char name[PKG_NAME_MAX], version[PKG_VERSION_MAX];
		char recipe_file[PATH_MAX];
		char *content;
		size_t content_len;
		const char *ext;
		enum pkg_error rc;
		struct sync_outcome outcome;

		if (recipe_file_split(name_de->d_name, name, sizeof(name), version, sizeof(version),
		                      &ext) != 0)
			continue;
		/* CPDL only: a .sh file in a source is not a recipe (cix#569). */
		if (strcmp(ext, "cbs") != 0)
			continue;
		if (!sync_source_owns("package", name, source, &res->held))
			continue;

		snprintf(recipe_file, sizeof(recipe_file), "%s/%s", recipes_root, name_de->d_name);
		if (!sync_vouched(cat, "recipes/package", name_de->d_name, recipe_file, source)) {
			res->failed++;
			continue;
		}
		if (persist_read_file(recipe_file, &content, &content_len) != 0 || content == NULL)
			continue;
		if (g_sync_refetch_name[0] != '\0' && strcmp(g_sync_refetch_name, name) == 0 &&
		    strcmp(g_sync_refetch_version, version) == 0)
			pkg_recipe_delete(name, version);

		memset(&outcome, 0, sizeof(outcome));
		rc = recipe_publish(name, content, NULL, &outcome);
		free(content);
		if (rc == PKG_OK && outcome.refreshed)
			res->refreshed++;
		else if (rc == PKG_OK)
			res->added++;
		else if (rc == PKG_ERR_DUPLICATE)
			res->skipped++;
		else if (rc == PKG_ERR_DIVERGENT)
			res->divergent++;
		else {
			res->failed++;
			/* #561: a count names nothing an operator can act on. */
			logstore_write("cixd", "error", "pkg sync: %s from source %s was not added: %s",
			               name_de->d_name, source,
			               pkg_recipe_add_last_error()[0] != '\0'
			                   ? pkg_recipe_add_last_error()
			                   : "it failed to parse, or its declared name or version does "
			                     "not match its filename");
		}
		if (outcome.writeback && (rc == PKG_ERR_DUPLICATE || (rc == PKG_OK && outcome.refreshed)))
			writeback_record(name, outcome.version, source, name_de->d_name);
	}
	closedir(names_d);
}

/*
 * ADR-0326: the upstream keys a source carries, as
 * recipes/keys/<package>@<FINGERPRINT>.asc. Taken only from a source
 * the host trusts for keys, only for packages that source owns (a
 * source cannot vouch for another source's package), and from a signed
 * source only what its index lists. The source's previous keys are
 * replaced, so a key removed from git leaves; a source not trusted for
 * keys supplies none, which removes what it supplied before.
 */
static void sync_upstream_keys(const struct pkg_source *s, const char *dir,
                               const struct catalogue *cat)
{
	struct upstreamkeys_offer offers[UPSTREAMKEYS_MAX];
	char *bufs[UPSTREAMKEYS_MAX];
	char names[UPSTREAMKEYS_MAX][PKG_NAME_MAX], fprs[UPSTREAMKEYS_MAX][48];
	char keys_root[PATH_MAX], why[512];
	DIR *kd = NULL;
	struct dirent *kde;
	int n = 0, adopted, refused = 0, i;

	snprintf(keys_root, sizeof(keys_root), "%s/recipes/keys", dir);
	if (s->trust_keys)
		kd = opendir(keys_root);
	while (kd != NULL && (kde = readdir(kd)) != NULL && n < UPSTREAMKEYS_MAX) {
		char from[PATH_MAX], item[PKG_SOURCE_ITEM_MAX];
		char owner[PKG_SOURCES_MAX * PKG_SOURCE_NAME_MAX];
		const char *at = strchr(kde->d_name, '@');
		size_t len = strlen(kde->d_name), name_len;

		if (kde->d_name[0] == '.' || at == NULL || len < 5 ||
		    strcmp(kde->d_name + len - 4, ".asc") != 0)
			continue;
		name_len = (size_t)(at - kde->d_name);
		if (name_len == 0 || name_len >= sizeof(names[n]) ||
		    (size_t)(len - 4 - name_len - 1) >= sizeof(fprs[n]))
			continue;
		memcpy(names[n], kde->d_name, name_len);
		names[n][name_len] = '\0';
		memcpy(fprs[n], at + 1, len - 4 - name_len - 1);
		fprs[n][len - 4 - name_len - 1] = '\0';
		snprintf(item, sizeof(item), "package:%s", names[n]);
		if (pkgsource_owner_of(item, owner, sizeof(owner)) != PKGSOURCE_OWNER_ONE ||
		    strcmp(owner, s->name) != 0) {
			logstore_write("cixd", "warn",
			               "pkg sync: source %s carries an upstream key for %s, which it does "
			               "not own -- not adopted (ADR-0326)",
			               s->name, names[n]);
			continue;
		}
		snprintf(from, sizeof(from), "%s/%s", keys_root, kde->d_name);
		if (cat != NULL && !sync_vouched(cat, "recipes/keys", kde->d_name, from, s->name))
			continue;
		bufs[n] = NULL;
		if (persist_read_file(from, &bufs[n], &offers[n].len) != 0 || bufs[n] == NULL)
			continue;
		offers[n].package = names[n];
		offers[n].fingerprint = fprs[n];
		offers[n].armored = bufs[n];
		n++;
	}
	if (kd != NULL)
		closedir(kd);

	adopted = upstreamkeys_sync_source(s->name, offers, n, (long long)time(NULL), &refused, why,
	                                   sizeof(why));
	for (i = 0; i < n; i++)
		free(bufs[i]);
	if (adopted < 0)
		logstore_write("cixd", "warn", "pkg sync: the upstream key store could not be saved");
	else if (adopted > 0)
		logstore_write("cixd", "info", "pkg sync: %d upstream key%s from source %s (ADR-0326)",
		               adopted, adopted == 1 ? "" : "s", s->name);
	if (refused > 0)
		logstore_write("cixd", "warn",
		               "pkg sync: %d upstream key%s from source %s refused -- %s (ADR-0326)",
		               refused, refused == 1 ? "" : "s", s->name, why);
}

/*
 * The second half, in the same helper: record what every source
 * offers, resolve who owns what from that, then merge from each source
 * only what it owns. Offers are written for every source that unpacked
 * BEFORE any merge, because ownership is a property of all of them
 * together: a name two sources offer is held, whichever comes first.
 */
int pkg_sync_merge(void)
{
	struct sync_source_result res[PKG_SOURCES_MAX];
	struct catalogue *cats[PKG_SOURCES_MAX];
	int refused[PKG_SOURCES_MAX];
	int count = pkgsource_count();
	char path[PATH_MAX];
	struct json_writer w;
	int i;

	memset(res, 0, sizeof(res));
	memset(cats, 0, sizeof(cats));
	memset(refused, 0, sizeof(refused));
	writeback_list_path(path, sizeof(path));
	unlink(path); /* this sync's findings only */

	/*
	 * ADR-0324 step C, before anything a tree says is believed: the
	 * writing host signs, then every host verifies -- in that order, so
	 * the host that signs never refuses its own tree. A refused source
	 * is reported as not synced, through the same error file a failed
	 * fetch uses, so it keeps what it offered before: an unverified
	 * tree moves no ownership.
	 */
	for (i = 0; i < count; i++) {
		const struct pkg_source *s = pkgsource_at(i);
		char dir[PATH_MAX], why[256];

		sync_extract_dir(i, dir, sizeof(dir));
		if (access(dir, F_OK) != 0)
			continue;
		catalogue_sign_if_writer(s, dir);
		cats[i] = catalogue_verify(s, dir, &refused[i], why, sizeof(why));
		if (refused[i]) {
			char err_path[PATH_MAX], msg[320];
			int n = snprintf(msg, sizeof(msg), "catalogue: %s", why);

			sync_fetch_err_path(i, err_path, sizeof(err_path));
			persist_atomic_write(err_path, msg, (size_t)n);
			logstore_write("cixd", "error", "pkg sync: source %s refused -- %s (ADR-0324)",
			               s->name, why);
		}
	}

	for (i = 0; i < count; i++) {
		char dir[PATH_MAX], sub[PATH_MAX];
		char **items = NULL;
		int n = 0, cap = 0, j;

		sync_extract_dir(i, dir, sizeof(dir));
		if (access(dir, F_OK) != 0 || refused[i])
			continue;
		snprintf(sub, sizeof(sub), "%s/recipes/package", dir);
		sync_collect_offers(sub, "package", "cbs", NULL, &items, &n, &cap);
		snprintf(sub, sizeof(sub), "%s/recipes/image", dir);
		sync_collect_offers(sub, "image", "json", NULL, &items, &n, &cap);
		snprintf(sub, sizeof(sub), "%s/recipes/deployment", dir);
		sync_collect_offers(sub, "deployment", "json", NULL, &items, &n, &cap);
		pkgsource_offers_write(pkgsource_at(i)->name, (const char *const *)items, n);
		for (j = 0; j < n; j++)
			free(items[j]);
		free(items);
	}
	pkgsource_offers_load();

	for (i = 0; i < count; i++) {
		const struct pkg_source *s = pkgsource_at(i);
		char dir[PATH_MAX], sub[PATH_MAX];

		sync_extract_dir(i, dir, sizeof(dir));
		if (access(dir, F_OK) != 0 || refused[i])
			continue;
		snprintf(sub, sizeof(sub), "%s/recipes/package", dir);
		sync_merge_packages(sub, s->name, cats[i], &res[i]);
		snprintf(sub, sizeof(sub), "%s/recipes/image", dir);
		sync_walk_image_recipes(sub, &res[i].added, &res[i].skipped, &res[i].failed, s->name,
		                        &res[i].held, cats[i]);
		snprintf(sub, sizeof(sub), "%s/recipes/deployment", dir);
		sync_walk_container_recipes(sub, &res[i].added, &res[i].skipped, &res[i].failed,
		                            s->name, &res[i].held, cats[i]);

		/* ADR-0324: keys only from a source trusted to vouch for
		 * packages. Any other source supplies recipes and nothing
		 * more. And from a signed source, only the keys its index
		 * lists: a key grants artifact trust, so it is the last thing
		 * to take on a tree's word. */
		if (s->trust_keys && g_trusted_keys_dir[0] != '\0') {
			char keys_root[PATH_MAX];
			int adopted;

			snprintf(keys_root, sizeof(keys_root), "%s/docs/keys", dir);
			if (cats[i] != NULL) {
				char vetted[PATH_MAX];
				DIR *kd = opendir(keys_root);
				struct dirent *kde;

				snprintf(vetted, sizeof(vetted), "%s/catalogue-keys-%d", g_pkg_dir, i);
				cix_btrfs_subvol_delete_or_rmtree(vetted);
				persist_mkdir_p(vetted);
				while (kd != NULL && (kde = readdir(kd)) != NULL) {
					char from[PATH_MAX], to[PATH_MAX];

					if (kde->d_name[0] == '.')
						continue;
					snprintf(from, sizeof(from), "%s/%s", keys_root, kde->d_name);
					snprintf(to, sizeof(to), "%s/%s", vetted, kde->d_name);
					if (sync_vouched(cats[i], "docs/keys", kde->d_name, from, s->name))
						copy_file_simple(from, to);
				}
				if (kd != NULL)
					closedir(kd);
				snprintf(keys_root, sizeof(keys_root), "%s", vetted);
			}
			adopted = releasekey_trust_adopt(keys_root, g_trusted_keys_dir);
			if (adopted < 0)
				logstore_write("cixd", "warn",
				                "pkg sync: could not write the trusted-key store at %s -- "
				                "artifact signatures cannot be verified until it is writable",
				                g_trusted_keys_dir);
			else if (adopted > 0)
				logstore_write("cixd", "info",
				                "pkg sync: %d release signing key%s trusted, from source %s",
				                adopted, adopted == 1 ? "" : "s", s->name);
		}
		sync_upstream_keys(s, dir, cats[i]);
	}
	/* ADR-0326: a source an operator removed takes its keys with it. */
	{
		const char *source_names[PKG_SOURCES_MAX];

		for (i = 0; i < count && i < PKG_SOURCES_MAX; i++)
			source_names[i] = pkgsource_at(i)->name;
		upstreamkeys_retain_sources(source_names, i);
	}
	for (i = 0; i < count; i++)
		catalogue_free(cats[i]);

	snprintf(path, sizeof(path), "%s/sync-extract", g_pkg_dir);
	cix_btrfs_subvol_delete_or_rmtree(path);

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < count; i++) {
		jw_arr_open(&w);
		jw_int(&w, res[i].added);
		jw_int(&w, res[i].skipped);
		jw_int(&w, res[i].divergent);
		jw_int(&w, res[i].failed);
		jw_int(&w, res[i].held);
		jw_int(&w, res[i].refreshed);
		jw_arr_close(&w);
	}
	jw_arr_close(&w);
	sync_results_path(path, sizeof(path));
	persist_atomic_write(path, w.buf, w.len);
	jw_free(&w);
	return 0;
}

/*
 * The fetch child has exited. Each source either left an archive or an
 * error sidecar. 0 when there is nothing to merge (every source failed),
 * 1 to go on to the extraction and merge.
 */
int pkg_sync_fetch_done(int exit_status)
{
	int count = pkgsource_count();
	int i, fetched = 0;

	g_sync_pid = -1;
	g_sync_result_count = count;
	for (i = 0; i < count; i++) {
		struct sync_source_result *r = &g_sync_results[i];
		char tarball_path[PATH_MAX], err_path[PATH_MAX];
		char *buf = NULL;
		size_t len = 0;

		memset(r, 0, sizeof(*r));
		snprintf(r->name, sizeof(r->name), "%s", pkgsource_at(i)->name);
		sync_state_path(i, tarball_path, sizeof(tarball_path));
		sync_fetch_err_path(i, err_path, sizeof(err_path));
		if (access(tarball_path, F_OK) == 0) {
			r->fetched = 1;
			fetched++;
		} else if (persist_read_file(err_path, &buf, &len) == 0 && buf != NULL) {
			snprintf(r->error, sizeof(r->error), "fetch failed: %.*s", (int)len, buf);
			free(buf);
		} else {
			snprintf(r->error, sizeof(r->error), "fetch failed (exit %d)", exit_status);
		}
		unlink(err_path);
	}
	if (fetched == 0) {
		g_sync_last_state = SYNC_FAILED;
		snprintf(g_sync_last_error, sizeof(g_sync_last_error), "%s",
		         count > 0 ? g_sync_results[0].error : "no source is configured");
		return 0;
	}
	return 1;
}

void pkg_sync_completed(int rc)
{
	char path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;
	int i, failed = 0, unfetched = 0;

	for (i = 0; i < PKG_SOURCES_MAX; i++) {
		sync_state_path(i, path, sizeof(path));
		unlink(path);
		sync_fetch_err_path(i, path, sizeof(path));
		if (i < g_sync_result_count && g_sync_results[i].fetched && access(path, F_OK) == 0 &&
		    persist_read_file(path, &buf, &len) == 0 && buf != NULL) {
			g_sync_results[i].fetched = 0;
			snprintf(g_sync_results[i].error, sizeof(g_sync_results[i].error), "%.*s",
			         (int)len, buf);
			free(buf);
			buf = NULL;
		}
		unlink(path);
	}

	/* What the merge found to write back (ADR-0324); kept even if it failed later. */
	writeback_take_sync_list();

	if (rc != 0) {
		g_sync_last_state = SYNC_FAILED;
		snprintf(g_sync_last_error, sizeof(g_sync_last_error), "%s",
		         rc == -1 ? "could not create extraction directory"
		                  : "archive extraction or recipe merge failed");
		return;
	}

	/* What the helper wrote: offers per source, and a row of counts per
	 * source in list order. */
	pkgsource_offers_load();
	sync_results_path(path, sizeof(path));
	if (persist_read_file(path, &buf, &len) == 0 && buf != NULL) {
		struct json_value *root = json_parse(buf, len);

		if (root != NULL && root->type == JSON_ARRAY) {
			for (i = 0; i < (int)root->u.array.count && i < g_sync_result_count; i++) {
				const struct json_value *row = root->u.array.items[i];
				struct sync_source_result *r = &g_sync_results[i];

				if (row->type != JSON_ARRAY || row->u.array.count != 6)
					continue;
				r->added = (int)json_as_number(row->u.array.items[0]);
				r->skipped = (int)json_as_number(row->u.array.items[1]);
				r->divergent = (int)json_as_number(row->u.array.items[2]);
				r->failed = (int)json_as_number(row->u.array.items[3]);
				r->held = (int)json_as_number(row->u.array.items[4]);
				r->refreshed = (int)json_as_number(row->u.array.items[5]);
			}
		}
		json_free(root);
		free(buf);
	}
	unlink(path);

	g_sync_refetch_name[0] = '\0';
	g_sync_refetch_version[0] = '\0';

	pkg_rebuild_queue_rederive();

	g_sync_last_added = g_sync_last_skipped = 0;
	g_sync_last_divergent = g_sync_last_held = g_sync_last_refreshed = 0;
	for (i = 0; i < g_sync_result_count; i++) {
		g_sync_last_added += g_sync_results[i].added;
		g_sync_last_skipped += g_sync_results[i].skipped;
		g_sync_last_divergent += g_sync_results[i].divergent;
		g_sync_last_held += g_sync_results[i].held;
		g_sync_last_refreshed += g_sync_results[i].refreshed;
		failed += g_sync_results[i].failed;
		if (!g_sync_results[i].fetched)
			unfetched++;
		if (g_sync_results[i].divergent > 0)
			logstore_write("cixd", "warn",
			               "pkg sync: source %s offered %d recipe version(s) this host already "
			               "holds with different content -- refused, kept as they were "
			               "(ADR-0324)",
			               g_sync_results[i].name, g_sync_results[i].divergent);
	}
	g_sync_last_state = SYNC_SUCCESS;
	g_sync_last_error[0] = '\0';
	if (unfetched > 0)
		snprintf(g_sync_last_error, sizeof(g_sync_last_error),
		         "%d source(s) could not be synced -- see sources[].error", unfetched);
	else if (failed > 0)
		snprintf(g_sync_last_error, sizeof(g_sync_last_error),
		         "%d recipe(s) failed to add (invalid content)", failed);
}

void pkg_sync_write_json_status(struct json_writer *w)
{
	const char *state_str = g_sync_last_state == SYNC_NEVER     ? "never"
	                         : g_sync_last_state == SYNC_RUNNING ? "running"
	                         : g_sync_last_state == SYNC_SUCCESS ? "success"
	                                                              : "failed";
	int i;

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
	jw_key(w, "skipped");
	jw_int(w, g_sync_last_skipped);
	jw_key(w, "divergent"); /* ADR-0324 */
	jw_int(w, g_sync_last_divergent);
	jw_key(w, "held");
	jw_int(w, g_sync_last_held);
	jw_key(w, "refreshed");
	jw_int(w, g_sync_last_refreshed);
	jw_key(w, "error");
	if (g_sync_last_error[0] != '\0')
		jw_str(w, g_sync_last_error);
	else
		jw_null(w);
	jw_key(w, "sources");
	jw_arr_open(w);
	for (i = 0; i < g_sync_result_count; i++) {
		const struct sync_source_result *r = &g_sync_results[i];

		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, r->name);
		jw_key(w, "fetched");
		jw_bool(w, r->fetched);
		jw_key(w, "added");
		jw_int(w, r->added);
		jw_key(w, "skipped");
		jw_int(w, r->skipped);
		jw_key(w, "divergent");
		jw_int(w, r->divergent);
		jw_key(w, "held");
		jw_int(w, r->held);
		jw_key(w, "refreshed");
		jw_int(w, r->refreshed);
		jw_key(w, "failed");
		jw_int(w, r->failed);
		jw_key(w, "error");
		if (r->error[0] != '\0')
			jw_str(w, r->error);
		else
			jw_null(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_key(w, "writeback"); /* ADR-0324 */
	pkg_approval_writeback_write_json(w);
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

/*
 * The one place a package artifact's file extension is spelled
 * (ADR-0307): ".cixpkg", for the one format there is.
 *
 * NULL for any other format, never a default. A recipe declaring
 * another format is refused at publish and by parse_cbs_recipe(), and
 * shell recipes, which published ".tar.gz", no longer exist (cix#569)
 * -- so a NULL here is a caller handing in a format nothing produced,
 * and each caller refuses rather than naming a file in it.
 */
static const char *artifact_suffix(const char *format)
{
	if (format != NULL && strcmp(format, PKG_ARTIFACT_FORMAT_CIXPKG) == 0)
		return ".cixpkg";
	return NULL;
}

/*
 * Where this version's artifact goes in the local cache. 0, or -1 with
 * out empty when format is not one this platform names (see
 * artifact_suffix()).
 */
static int cache_artifact_path(const char *name, const char *version, const char *format,
                                char *out, size_t out_size)
{
	const char *suffix = artifact_suffix(format);

	out[0] = '\0';
	if (suffix == NULL)
		return -1;
	snprintf(out, out_size, "%s/%s-%s%s", g_cache_dir, name, version, suffix);
	return 0;
}

/*
 * Where this version's artifact ALREADY is. Returns 1 and fills out
 * when it exists, 0 otherwise -- and out still holds the .cixpkg path,
 * so a caller reporting "not found" names the file it looked for.
 *
 * Only .cixpkg is looked for. A .tar.gz left in the cache from before
 * cix#569 is not an artifact of anything this host installs, and
 * retire_targz_artifacts() removes it at startup.
 */
static int cache_artifact_path_existing(const char *name, const char *version, char *out,
                                         size_t out_size)
{
	struct stat st;

	(void)cache_artifact_path(name, version, PKG_ARTIFACT_FORMAT_CIXPKG, out, out_size);
	return stat(out, &st) == 0 && S_ISREG(st.st_mode);
}

/*
 * The signature beside this version's artifact, `<artifact>.minisig`
 * (ADR-0279).
 */
static void cache_signature_path_existing(const char *name, const char *version, char *out,
                                           size_t out_size)
{
	char artifact[PATH_MAX];

	(void)cache_artifact_path_existing(name, version, artifact, sizeof(artifact));
	snprintf(out, out_size, "%s.minisig", artifact);
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

/*
 * cix#569: the cache's .tar.gz files, and their .minisig, which shell
 * recipes published and nothing installs any more. Only names ending
 * exactly in ".tar.gz" or ".tar.gz.minisig" -- every .cixpkg, its
 * signature, and anything else in the directory is left alone. Counted
 * into g_shell_retire with the stored recipes retire_shell_recipes()
 * removes.
 */
static void retire_targz_artifacts(void)
{
	static const char *const SUFFIXES[] = { ".tar.gz", ".tar.gz.minisig" };
	DIR *d = opendir(g_cache_dir);
	struct dirent *de;

	if (d == NULL)
		return;
	while ((de = readdir(d)) != NULL) {
		size_t len = strlen(de->d_name);
		size_t i;

		for (i = 0; i < sizeof(SUFFIXES) / sizeof(SUFFIXES[0]); i++) {
			size_t slen = strlen(SUFFIXES[i]);
			char path[PATH_MAX];
			struct stat st;

			if (len <= slen || strcmp(de->d_name + len - slen, SUFFIXES[i]) != 0)
				continue;
			if (snprintf(path, sizeof(path), "%s/%s", g_cache_dir, de->d_name) >=
			        (int)sizeof(path) ||
			    lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
				break;
			if (unlink(path) == 0)
				g_shell_retire.artifacts++;
			else
				g_shell_retire.failed++;
			break;
		}
	}
	closedir(d);
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
	retire_targz_artifacts(); /* cix#569 */

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
 * before every new cache write (pkg_cache_save_from_file()). A single
 * artifact bigger than the whole configured cap is
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

	return cache_artifact_path_existing(name, version, path, sizeof(path));
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

	if (cache_artifact_path_existing(name, version, path, sizeof(path)))
		utime(path, NULL);
}

/*
 * Caches, under the configured cap and with LRU eviction, an
 * artifact tarball that already exists on disk as a whole file
 * (pkg_sync_completed()'s own network-fetched pkg/artifacts/ entries)
 * rather than a directory to tar up. Takes ownership of src_path: it
 * either becomes the cache entry (rename) or is removed (copy+unlink
 * fallback across a filesystem boundary, or on any failure) -- never
 * left behind either way.
 */
static void pkg_cache_save_from_file(const char *name, const char *version, const char *format,
                                      const char *src_path)
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
	if (cache_artifact_path(name, version, format, final_path, sizeof(final_path)) != 0) {
		logstore_write("cixd", "error",
		               "pkg %s@%s: not caching an artifact of format \"%s\"; only cixpkg is "
		               "cached (cix#569)",
		               name, version, format != NULL ? format : "");
		unlink(src_path);
		return;
	}
	cache_evict_lru_until_fits(size);
	unlink(final_path);
	if (rename(src_path, final_path) != 0) {
		if (copy_file_simple(src_path, final_path) == 0)
			unlink(src_path);
		else
			unlink(src_path);
	}
}

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
/*
 * cix#558: the most a recipe's declared resources { memory } may raise the
 * shared build budget to. Never below memory_max; equal to it -- no
 * raise at all -- until an operator sets it higher; 0 means no ceiling.
 */
static long long g_build_memory_max_ceiling = PKG_BUILD_MEMORY_MAX_DEFAULT;

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
	jw_key(&w, "memory_max_ceiling");
	jw_int(&w, g_build_memory_max_ceiling);
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
	const struct json_value *mj, *jmem, *jcpu, *jceil;

	if (snprintf(g_build_config_path, sizeof(g_build_config_path), "%s", config_path) >=
	    (int)sizeof(g_build_config_path))
		return -1;
	g_build_max_jobs = PKG_BUILD_MAX_JOBS_DEFAULT;
	g_build_memory_max = PKG_BUILD_MEMORY_MAX_DEFAULT;
	g_build_memory_max_ceiling = PKG_BUILD_MEMORY_MAX_DEFAULT;
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
	 * cix#558: absent from every config saved before the ceiling
	 * existed, and then it equals memory_max -- so nothing a recipe
	 * declares can raise the budget until an operator says it may.
	 * A value that breaks the ceiling >= memory_max rule (only a
	 * hand-edited file can hold one) is treated the same way.
	 */
	g_build_memory_max_ceiling = g_build_memory_max;
	jceil = json_object_get(root, "memory_max_ceiling");
	if (jceil != NULL && jceil->type != JSON_NULL) {
		double v = json_as_number(jceil);

		if (v >= 0 && memory_budget_at_least((long long)v, g_build_memory_max))
			g_build_memory_max_ceiling = (long long)v;
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

/*
 * True when budget a is at least budget b, where 0 means unlimited and
 * so is larger than any number of bytes.
 */
static int memory_budget_at_least(long long a, long long b)
{
	if (a == 0)
		return 1;
	if (b == 0)
		return 0;
	return a >= b;
}

long long pkg_build_get_memory_max_ceiling(void)
{
	return g_build_memory_max_ceiling;
}

/*
 * cix#558: memory_max and its ceiling are set together, because the one
 * rule between them -- the ceiling is never below memory_max, 0 meaning
 * unlimited for both -- is a property of the pair. Setting them one at
 * a time would make the outcome of a request that raises both depend on
 * the order the fields were applied in.
 *
 * Negative is rejected for either; there is no such thing as negative
 * memory. 0 for memory_max is the deliberate "no limit" opt-out it has
 * always been (cgroup_create()'s "memory_max > 0" gate), and it forces
 * the ceiling to 0 too: nothing can be raised above unlimited.
 */
enum pkg_error pkg_build_set_memory(long long memory_max, long long ceiling, char *err,
                                    size_t err_size)
{
	if (memory_max < 0 || ceiling < 0) {
		snprintf(err, err_size, "memory_max and memory_max_ceiling must be >= 0");
		return PKG_ERR_INVALID_NAME;
	}
	if (!memory_budget_at_least(ceiling, memory_max)) {
		if (memory_max == 0)
			snprintf(err, err_size,
			         "memory_max is 0 (unlimited), so memory_max_ceiling must be 0 too");
		else
			snprintf(err, err_size,
			         "memory_max_ceiling (%lld) is below memory_max (%lld); raise the "
			         "ceiling in the same request",
			         ceiling, memory_max);
		return PKG_ERR_INVALID_NAME;
	}
	g_build_memory_max = memory_max;
	g_build_memory_max_ceiling = ceiling;
	if (save_build_config() != 0) {
		snprintf(err, err_size, "could not save the build config");
		return PKG_ERR_PERSIST_FAILED;
	}
	return PKG_OK;
}

/*
 * cix#558: the budget a recipe raised the shared parent to, or 0 when
 * none is raised. It only ever grows while builds are in flight and
 * returns to 0 when the last one ends -- see pkg_build_memory_note_end()
 * for why it is not lowered sooner.
 */
static long long g_build_memory_raised;

long long pkg_build_effective_memory_max(void)
{
	if (g_build_memory_max == 0)
		return 0;
	return g_build_memory_raised > g_build_memory_max ? g_build_memory_raised
	                                                  : g_build_memory_max;
}

static int build_memory_admit(long long need, char *msg, size_t msg_size)
{
	if (need <= 0 || g_build_memory_max == 0 || g_build_memory_max_ceiling == 0 ||
	    need <= g_build_memory_max_ceiling)
		return 0;
	snprintf(msg, msg_size,
	         "this build declares resources { memory } of %lld bytes (%lld MiB), above "
	         "memory_max_ceiling of %lld bytes (%lld MiB) -- raise it with "
	         "`cixctl pkg-build-config set --memory-max-ceiling=%lld` and install again",
	         need, need / (1024 * 1024), g_build_memory_max_ceiling,
	         g_build_memory_max_ceiling / (1024 * 1024), need);
	return -1;
}

static void pkg_build_memory_note_start(int chain_idx, long long need)
{
	g_chains[chain_idx].build_running = 1;
	if (need > g_build_memory_raised) {
		g_build_memory_raised = need;
		if (need > g_build_memory_max && g_build_memory_max != 0)
			logstore_write("cixd", "info",
			               "pkg %s: build budget raised to %lld bytes for its declared "
			               "resources { memory } (memory_max %lld, ceiling %lld)",
			               g_chains[chain_idx].name, need, g_build_memory_max,
			               g_build_memory_max_ceiling);
	}
}

/*
 * Lowered only when NO build is left running. Dropping memory.max under
 * builds that are still going makes the kernel reclaim from them, and
 * OOM-kill one if it cannot -- and a build that started while the
 * budget was raised may well be using what the raise allowed. Holding
 * the raise until the parent is idle costs nothing a running build
 * needs; it only delays the return to memory_max.
 */
static void pkg_build_memory_note_end(int chain_idx)
{
	long long raised;
	int i;

	if (!g_chains[chain_idx].build_running)
		return;
	g_chains[chain_idx].build_running = 0;
	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++)
		if (g_chains[i].build_running)
			return;
	raised = g_build_memory_raised;
	g_build_memory_raised = 0;
	/* A need at or under memory_max raised nothing, so there is
	 * nothing to put back. */
	if (g_build_memory_max == 0 || raised <= g_build_memory_max)
		return;
	pkg_build_ensure_parent_cgroup();
	logstore_write("cixd", "info", "pkg: no build running; build budget back to memory_max %lld",
	               g_build_memory_max);
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

/* ---- package repositories (ADR-0122, ADR-0324 step B) ----
 *
 * A precompiled artifact is served from any plain HTTP location at a
 * path this daemon computes itself -- <url>/<published name> -- and is
 * accepted only after verifying it against the recipe's own git-tracked
 * checksum and, where keys are trusted, its signature: trust never
 * comes from the server. ADR-0122 kept one such server; ADR-0324 keeps
 * a list (pkgrepo.c), tried in order, each copy verified, the next
 * tried when one fails, and pushes to every repository marked push.
 */

static int pkg_artifact_is_configured(void)
{
	return pkgrepo_count() > 0;
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
	char repo[PKG_SOURCE_NAME_MAX]; /* ADR-0324: the repository it is pushed to */
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

/*
 * #381: the way back from pkg_approval_grant(). Deliberately not gated
 * on the gate being on: a grant survives the gate being switched off and
 * on again, so it must be revocable in either state, or turning a gate
 * off would strand a grant nothing could reach.
 */
int pkg_approval_revoke(const char *gate, const char *target, char *granted_by,
                        size_t granted_by_size)
{
	int i;

	if (gate_flag(gate) == NULL || target == NULL || target[0] == '\0')
		return PKG_APPROVE_UNKNOWN_GATE;
	i = approval_index(gate, target);
	if (i < 0)
		return PKG_APPROVE_NOT_GRANTED;
	if (granted_by != NULL && granted_by_size > 0)
		snprintf(granted_by, granted_by_size, "%s", g_approvals[i].who);
	approval_drop_at(i);
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

/* One push of one artifact (or its signature) to one repository. -1 when the queue is full. */
static int push_enqueue_one(const char *name, const char *version, enum pkg_push_kind kind,
                            const char *repo)
{
	if (g_push_queue_count >= PKG_PUSH_QUEUE_MAX) {
		logstore_write("cixd", "error",
		                "artifact push: queue full (%d) -- %s@%s will NOT be published to %s",
		                PKG_PUSH_QUEUE_MAX, name, version, repo);
		return -1;
	}
	snprintf(g_push_queue[g_push_queue_count].name, PKG_NAME_MAX, "%s", name);
	snprintf(g_push_queue[g_push_queue_count].version, PKG_VERSION_MAX, "%s", version);
	snprintf(g_push_queue[g_push_queue_count].repo, PKG_SOURCE_NAME_MAX, "%s", repo);
	g_push_queue[g_push_queue_count].kind = kind;
	g_push_queue_count++;
	return 0;
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
	int i;

	if (pkgrepo_push_count() == 0) {
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
		                "artifact push: %s@%s was built here but not published -- no package "
		                "repository is marked push. The artifact is in this host's cache and "
		                "can still be published with POST /v1/pkg/%s/artifact/publish; it will "
		                "be lost if this host is reinstalled first.",
		                name, version, name);
		return;
	}
	/* ADR-0324: every repository marked push gets its own copy, and
	 * its own retry -- one repository being down never holds another. */
	for (i = 0; i < pkgrepo_count(); i++) {
		const struct pkg_repository *r = pkgrepo_at(i);

		if (!r->push)
			continue;
		if (r->token[0] == '\0') {
			logstore_write("cixd", "info",
			                "artifact push: %s@%s not published to %s -- it is marked push "
			                "but has no token",
			                name, version, r->name);
			continue;
		}
		if (push_enqueue_one(name, version, kind, r->name) != 0)
			return;
	}
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
	(void)cache_artifact_path_existing(name, version, out, out_size);
}

/*
 * The cache path a package of THIS format would occupy, for a caller
 * that is about to create it.
 *
 * pkg_artifact_cache_path() above answers "where is it", by looking
 * for a file that exists and falling back to `.tar.gz` when none
 * does. That fallback is right for a reader and silently wrong for a
 * writer: the hostbuild publisher calls it precisely when nothing
 * exists yet -- it returns early when pkg_artifact_cache_has() is
 * true -- so the fallback was the only branch it ever took, and every
 * hostbuild published a tarball no matter what its recipe declared
 * (cix#528). A writer must say which format it is writing, and out is
 * left empty for one that is not cixpkg (cix#569).
 */
void pkg_artifact_cache_path_for(const char *name, const char *version, const char *format,
                                  char *out, size_t out_size)
{
	(void)cache_artifact_path(name, version, format, out, out_size);
}

/* The filename suffix a format is written with, exported so the
 * hostbuild exporter in main.c names its output the same way this
 * file does rather than mapping format to suffix a second time. NULL
 * for a format that is not cixpkg (cix#569). */
const char *pkg_artifact_suffix(const char *format)
{
	return artifact_suffix(format);
}

const char *pkg_artifact_arch(void)
{
	return pkg_host_arch();
}

/*
 * The version/release split THE CACHE performs on an artifact name:
 * a trailing `-<digits>` is the release, and a name without one is
 * release 1 (measured on 192.168.15.31, 2026-09-18, #494 --
 * `wget@1.25.0` and `wget@1.25.0-1` resolved to the same
 * stored artifact).
 *
 * Used only when no recipe is readable. It is deliberately the same
 * rule the reader applies, which is what makes it safe here and not
 * a guess: whatever this produces, the cache will independently
 * derive from the filename, so the metadata written INTO the
 * artifact and the name it is stored under agree by construction.
 * That is exactly the property a made-up split would not have, and
 * why the recipe is still preferred when there is one -- it is
 * authoritative for the cases the convention gets wrong
 * (`v2.2.0-rc1` has a trailing `1` that is not a release, and both
 * this and the cache read it as release 1 with the version intact,
 * which is consistent even where it is not what CPDL declared).
 */
static void split_version_by_cache_convention(const char *fused, char *out_version,
                                               size_t out_version_size, long long *out_release)
{
	const char *dash = strrchr(fused, '-');
	const char *p;

	snprintf(out_version, out_version_size, "%s", fused);
	*out_release = 1;
	if (dash == NULL || dash[1] == '\0')
		return;
	for (p = dash + 1; *p != '\0'; p++)
		if (*p < '0' || *p > '9')
			return; /* not all digits -- not a release */
	*out_release = atoll(dash + 1);
	if ((size_t)(dash - fused) < out_version_size)
		out_version[dash - fused] = '\0';
}

enum pkg_error pkg_hostbuild_package_info(const char *name, const char *version,
                                           char *out_bare_version, size_t out_bare_version_size,
                                           long long *out_release)
{
	struct pkg_recipe recipe;
	char recipe_path[PATH_MAX];
	char bare[PKG_VERSION_MAX];
	long long release = 1;

	if (name == NULL || version == NULL)
		return PKG_ERR_NOT_FOUND;

	memset(&recipe, 0, sizeof(recipe));
	if (find_recipe_path(name, version, recipe_path, sizeof(recipe_path)) == 0 &&
	    parse_recipe(recipe_path, &recipe) == 0) {
		snprintf(bare, sizeof(bare), "%s", recipe.bare_version);
		release = recipe.release;
	} else {
		/*
		 * A hostbuild whose recipe is no longer in the store. Real,
		 * not hypothetical: 610 cix recipes were removed on
		 * 2026-09-26 (ADR-0313) while their packages stayed
		 * installed, and a test fixture can seed an installed entry
		 * with no recipe at all.
		 *
		 * The split then follows the cache's own convention so the
		 * artifact's metadata matches the name it will be stored
		 * under.
		 */
		split_version_by_cache_convention(version, bare, sizeof(bare), &release);
	}

	if (out_bare_version != NULL)
		snprintf(out_bare_version, out_bare_version_size, "%s", bare);
	if (out_release != NULL)
		*out_release = release;
	return PKG_OK;
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

/*
 * Which version of a seed package the media should carry, and how its
 * artifact can be obtained (#495).
 *
 * This used to be "the first INSTALLED entry in g_packages with this
 * name", which is not a rule -- it is whichever image the table
 * happened to hold first. Measured on 192.168.15.95, 2026-09-19: ten
 * images carry glibc, five of them at 2.44-16, and the pick was
 * cix-builder's 2.44-14 because that entry sorts first. For zlib the
 * arbitrary pick was actively worse: cix-builder's 1.3.2-10 has no
 * approved checksum and no cached artifact, while jumpbox's 1.3.2-14
 * has both.
 *
 * So: the NEWEST installed version across every image, restricted to
 * versions this host can actually produce bytes for -- one already in
 * the local cache, or one whose recipe carries a pkg_artifact_sha256
 * approving a specific byte sequence the configured artifact cache can
 * be asked for. A version with neither is not a candidate, because
 * seeding it means shipping media that names an artifact nothing can
 * supply.
 *
 * pkg_version_compare() is the one comparator, the same one
 * recipe_latest_version() and the drift report use. A second ordering
 * here would be a second answer to "which is newer".
 */
struct seed_choice {
	char version[PKG_VERSION_MAX];
	char sha256[PKG_SHA256_MAX];
	/* What a FETCH of this version would land, since the file does
	 * not exist locally yet and so cannot be asked (ADR-0307). When
	 * cached is 1 the on-disk file answers instead and this is not
	 * consulted -- what the cache actually holds beats what the
	 * recipe now declares, for a version whose recipe was converted
	 * after the artifact was built. */
	char format[PKG_ARTIFACT_FORMAT_MAX];
	int cached;
};

static enum pkg_error seed_choose(const char *name, struct seed_choice *out, char *err,
                                   size_t err_size)
{
	int i;
	int seen_any = 0;
	int have = 0;
	char newest_rejected[PKG_VERSION_MAX];

	memset(out, 0, sizeof(*out));
	newest_rejected[0] = '\0';

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		struct pkg_recipe recipe;
		char recipe_path[PATH_MAX];
		const char *version;
		int cached;
		int approved;

		if (!g_packages[i].in_use || g_packages[i].state != PKG_STATE_INSTALLED)
			continue;
		if (strcmp(g_packages[i].name, name) != 0)
			continue;
		version = g_packages[i].version;
		seen_any = 1;
		/* The same version in several images is one candidate, which
		 * <= 0 already covers -- equal is not newer. */
		if (have && pkg_version_compare(version, out->version) <= 0)
			continue;

		cached = pkg_artifact_cache_has(name, version);
		memset(&recipe, 0, sizeof(recipe));
		approved = (find_recipe_path(name, version, recipe_path, sizeof(recipe_path)) == 0 &&
		            parse_recipe(recipe_path, &recipe) == 0 && recipe.artifact_sha256[0] != '\0');
		if (!cached && !approved) {
			/*
			 * The NEWEST unusable version is what a refusal names,
			 * not a list of every one: an older version becoming
			 * usable would not change the answer, since this rule
			 * takes the newest, so the newest is the only one worth
			 * acting on. It also sidesteps listing the same version
			 * once per image that carries it.
			 */
			if (newest_rejected[0] == '\0' ||
			    pkg_version_compare(version, newest_rejected) > 0)
				snprintf(newest_rejected, sizeof(newest_rejected), "%s", version);
			continue;
		}

		snprintf(out->version, sizeof(out->version), "%s", version);
		snprintf(out->sha256, sizeof(out->sha256), "%s", approved ? recipe.artifact_sha256 : "");
		snprintf(out->format, sizeof(out->format), "%s", recipe.artifact_format);
		out->cached = cached;
		have = 1;
	}

	if (!seen_any) {
		snprintf(err, err_size,
		         "the installer seed needs %s and this host has none installed -- an ISO "
		         "built now could not bring up DNS on a fresh box",
		         name);
		return PKG_ERR_NOT_FOUND;
	}
	if (!have) {
		snprintf(err, err_size,
		         "the installer seed needs %s, and no installed version of it can be put on "
		         "the media -- the newest, %s, has no cached artifact and no approved "
		         "pkg_artifact_sha256 to fetch one with",
		         name, newest_rejected[0] != '\0' ? newest_rejected : "(none)");
		return PKG_ERR_NOT_FOUND;
	}
	return PKG_OK;
}

/*
 * Answers, without doing any of the work, whether staging a seed can
 * succeed -- so POST /v1/system/iso can still refuse synchronously the
 * cases nothing will fix (#495).
 *
 * The division is deliberate: what is cheap and decisive (a table scan,
 * a stat, a recipe read) stays in the request handler and answers 400;
 * what costs time (a 12.8 MB recipe tree copy, artifact copies, a
 * fetch) moves into the forked build child and fails the build in
 * GET /system/iso instead. Doing the expensive half here is what had
 * cixd copying the whole recipe tree inside its own epoll loop.
 */
enum pkg_error pkg_seed_preflight(char *err, size_t err_size)
{
	size_t i;

	for (i = 0; i < sizeof(g_seed_packages) / sizeof(g_seed_packages[0]); i++) {
		struct seed_choice c;
		enum pkg_error rc = seed_choose(g_seed_packages[i], &c, err, err_size);

		if (rc != PKG_OK)
			return rc;
		if (!c.cached && !pkg_artifact_is_configured()) {
			snprintf(err, err_size,
			         "the installer seed needs %s@%s, its artifact is not in this host's "
			         "cache, and no artifact cache is configured to fetch it from",
			         g_seed_packages[i], c.version);
			return PKG_ERR_NOT_FOUND;
		}
	}
	return PKG_OK;
}

/*
 * Puts one seed artifact at dst, fetching it when the local cache has
 * no copy (#495).
 *
 * Fetched straight into the seed directory rather than into the shared
 * local cache: the seed is rebuilt on every ISO build, and a build
 * child writing the cache would run cache_evict_lru_until_fits()
 * alongside the install pipeline doing the same in the same directory.
 * A re-download per ISO build costs a few seconds and owns nothing.
 *
 * The approved digest is the whole of the trust here. seed_choose()
 * only ever selects an uncached version that has one, so there is no
 * branch where unverified bytes reach the media.
 */
static enum pkg_error seed_place_artifact(const char *name, const struct seed_choice *c,
                                           const char *dst, char *err, size_t err_size)
{
	char url[768];
	char header[320];
	char sha_out[128] = "";
	struct curlfetch_opts opts;
	char curl_err[256];
	int ri;

	if (c->cached) {
		char src[PATH_MAX];

		(void)cache_artifact_path_existing(name, c->version, src, sizeof(src));
		if (copy_file_simple(src, dst) != 0) {
			snprintf(err, err_size, "could not stage the %s artifact: %s", name,
			         strerror(errno));
			return PKG_ERR_PERSIST_FAILED;
		}
		return PKG_OK;
	}

	/* ADR-0324: every repository in order; a copy that is not the
	 * approved bytes is refused and the next one tried. */
	err[0] = '\0';
	for (ri = 0; ri < pkgrepo_count(); ri++) {
		const struct pkg_repository *repo = pkgrepo_at(ri);

		pkg_artifact_build_request(repo, name, c->version, c->format, url, sizeof(url), header,
		                           sizeof(header));
		memset(&opts, 0, sizeof(opts));
		opts.url = url;
		opts.path = dst;
		opts.header1 = header[0] != '\0' ? header : NULL;
		opts.connect_timeout = PKG_CURL_CONNECT_TIMEOUT_SECS;
		opts.low_speed_limit = PKG_CURL_LOW_SPEED_LIMIT;
		opts.low_speed_time = PKG_CURL_LOW_SPEED_TIME_SECS;
		curl_err[0] = '\0';
		unlink(dst);
		if (curlfetch_perform(&opts, NULL, curl_err, sizeof(curl_err)) != 0) {
			snprintf(err, err_size, "could not fetch the %s@%s artifact from %s: %s", name,
			         c->version, repo->name, curl_err[0] != '\0' ? curl_err : "fetch failed");
			continue;
		}
		if (pkg_run_capture_sha256(dst, sha_out, sizeof(sha_out)) != 0) {
			snprintf(err, err_size, "could not hash the %s@%s artifact from %s", name,
			         c->version, repo->name);
			continue;
		}
		if (strcmp(sha_out, c->sha256) != 0) {
			snprintf(err, err_size,
			         "repository %s served %s@%s that is not the approved artifact: the "
			         "recipe approves %s, it served %s",
			         repo->name, name, c->version, c->sha256, sha_out);
			logstore_write("cixd", "warn", "pkg: %s", err);
			continue;
		}
		return PKG_OK;
	}
	unlink(dst);
	if (err[0] == '\0')
		snprintf(err, err_size, "no package repository is configured to fetch %s@%s from",
		         name, c->version);
	return PKG_ERR_NOT_FOUND;
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
		const char *name = g_seed_packages[i];
		struct seed_choice c;
		char src[PATH_MAX];
		char dst[PATH_MAX];
		const char *base;
		enum pkg_error rc;

		rc = seed_choose(name, &c, err, err_size);
		if (rc != PKG_OK)
			return rc;
		/*
		 * This artifact's OWN recipe version, even if a newer one is
		 * what seed_stage_recipes() already staged. Otherwise a fresh
		 * box resolves this package to a recipe the media carries no
		 * artifact for, and tries to build a C library on a machine
		 * with no compiler.
		 */
		if (seed_copy_one_recipe(recipes_dst, name, c.version) != 0) {
			snprintf(err, err_size, "could not stage the %s@%s recipe the seeded artifact "
			                        "needs: %s",
			         name, c.version, treecopy_last_error());
			return PKG_ERR_PERSIST_FAILED;
		}

		/*
		 * The name the media carries: whatever the cache actually
		 * holds when it holds something, and otherwise what a fetch
		 * of the declared format will land. cix-install copies the
		 * directory verbatim and the installed box's own cache reads
		 * these names back, so an artifact whose extension does not
		 * match its bytes would be a cache entry nothing can open.
		 */
		if (!cache_artifact_path_existing(name, c.version, src, sizeof(src)) &&
		    cache_artifact_path(name, c.version, c.format, src, sizeof(src)) != 0) {
			snprintf(err, err_size, "%s@%s declares artifact format \"%s\"; only cixpkg is "
			         "seeded (cix#569)", name, c.version, c.format);
			return PKG_ERR_INVALID_RECIPE;
		}
		base = strrchr(src, '/');
		base = (base != NULL) ? base + 1 : src;
		if (snprintf(dst, sizeof(dst), "%s/%s", artifacts_dst, base) >= (int)sizeof(dst)) {
			snprintf(err, err_size, "seed artifact path too long for %s", name);
			return PKG_ERR_INVALID_NAME;
		}
		rc = seed_place_artifact(name, &c, dst, err, err_size);
		if (rc != PKG_OK)
			return rc;
		/*
		 * stdout, not the log store: this runs in the ISO build's
		 * forked child, whose output the reaper already captures and
		 * reports (#495). The store is a file the PARENT holds open
		 * (logstore.c's g_current_fp) and whose rotation it accounts
		 * for in-process (g_current_size); a forked child writing
		 * through that inherited descriptor appends bytes the parent
		 * does not count, with two independent stdio buffers on one
		 * offset. So the child says what it did on a channel built
		 * for it instead.
		 */
		printf("iso seed: staged %s@%s (%s)\n", name, c.version,
		       c.cached ? "from the local cache" : "fetched");
		fflush(stdout);
	}

	return PKG_OK;
}

int pkg_artifact_cache_has(const char *name, const char *version)
{
	char path[PATH_MAX];

	return cache_artifact_path_existing(name, version, path, sizeof(path));
}

/*
 * The one search behind every publish entry point below: the installed
 * entry for NAME, in IMAGE when one is named. Returns its index, or -1.
 *
 * A package name is not unique -- it names one entry per image it is
 * installed in -- and nothing sorts g_packages, so this used to answer
 * with whatever the array happened to hold first. `kernel` is
 * installed both in `base`, from a cached artifact, and as a
 * `__hostbuild`, in that array order, so every publish of it meant the
 * `base` entry (#520):
 *
 *   - publish_hostbuild_artifact() read is_hostbuild = 0 and returned
 *     before building the tarball, so the push it had already queued
 *     found nothing. No kernel this host built has ever reached the
 *     artifact cache; the newest one there is 7.2.3-3, from two
 *     releases of the shell recipe ago. `cix` was unaffected only
 *     because its one entry IS the hostbuild, and #200's own note
 *     records `isotools`, the same shape, in the same state.
 *   - POST /v1/pkg/{name}/artifact/publish either re-published the
 *     cached `base` version or, when that was not cached, refused with
 *     409 "cannot be rebuilt from the installed tree" -- for a package
 *     whose hostbuild artifact was sitting on disk.
 *
 * With no IMAGE named, a hostbuild entry therefore WINS over the rest
 * rather than the array deciding. That is the only reading under which
 * publishing such a name works at all: a hostbuild's artifact is the
 * one this host produced, the other entry's came from the cache and is
 * published there already, and rebuilding a tarball from an installed
 * tree is something only the hostbuild branch knows how to do. A
 * caller that means one specific image says so.
 */
static int artifact_publish_entry(const char *name, const char *image)
{
	int fallback = -1;
	int i;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		if (!g_packages[i].in_use || g_packages[i].state != PKG_STATE_INSTALLED)
			continue;
		if (strcmp(g_packages[i].name, name) != 0)
			continue;
		if (image != NULL) {
			if (strcmp(g_packages[i].image, image) != 0)
				continue;
			return i;
		}
		if (strcmp(g_packages[i].image, PKG_HOSTBUILD_IMAGE) == 0)
			return i;
		if (fallback < 0)
			fallback = i;
	}
	return fallback;
}

/*
 * Resolves what pkg_artifact_publish_in() would publish, without
 * enqueuing anything -- same validation, same entry, so the two can
 * never disagree about which version a publish means.
 */
enum pkg_error pkg_artifact_publish_resolve_in(const char *name, const char *image,
                                                char *out_version, size_t out_version_size,
                                                int *out_is_hostbuild)
{
	int i;

	if (pkgrepo_push_count() == 0)
		return PKG_ERR_INVALID_RECIPE;

	i = artifact_publish_entry(name, image);
	if (i < 0)
		return PKG_ERR_NOT_FOUND;
	snprintf(out_version, out_version_size, "%s", g_packages[i].version);
	/* Derived from the image, never a stored flag -- the same
	 * One Source of Truth rule the listing follows (ADR-0056). */
	if (out_is_hostbuild != NULL)
		*out_is_hostbuild = strcmp(g_packages[i].image, PKG_HOSTBUILD_IMAGE) == 0;
	return PKG_OK;
}

enum pkg_error pkg_artifact_publish_resolve(const char *name, char *out_version,
                                             size_t out_version_size, int *out_is_hostbuild)
{
	return pkg_artifact_publish_resolve_in(name, NULL, out_version, out_version_size,
	                                        out_is_hostbuild);
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

enum pkg_error pkg_artifact_publish_in(const char *name, const char *image)
{
	int i;

	if (pkgrepo_push_count() == 0)
		return PKG_ERR_INVALID_RECIPE;

	i = artifact_publish_entry(name, image);
	if (i < 0)
		return PKG_ERR_NOT_FOUND;
	pkg_artifact_push_enqueue(g_packages[i].name, g_packages[i].version);
	return PKG_OK;
}

enum pkg_error pkg_artifact_publish(const char *name)
{
	return pkg_artifact_publish_in(name, NULL);
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
	char url[PKG_REPOSITORY_URL_MAX + 256];
	char auth_header[PKG_REPOSITORY_TOKEN_MAX + 32];
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
			int more = 0;

			/* One approval publishes the artifact to every repository
			 * marked push (ADR-0324): it is spent with the last copy. */
			for (i = 0; i < g_push_queue_count; i++)
				if (g_push_queue[i].kind != PKG_PUSH_SIGNATURE &&
				    strcmp(g_push_queue[i].name, g_push_current.name) == 0 &&
				    strcmp(g_push_queue[i].version, g_push_current.version) == 0)
					more = 1;
			snprintf(t, sizeof(t), "%s@%s", g_push_current.name, g_push_current.version);
			if (!more)
				approval_consume(PKG_GATE_PUBLISH, t);
		}
		if (pkgrepo_find(g_push_current.repo) == NULL) {
			logstore_write("cixd", "info",
			                "artifact push: %s@%s not pushed to %s -- that repository is no "
			                "longer configured",
			                g_push_current.name, g_push_current.version, g_push_current.repo);
			continue;
		}

		/*
		 * Both kinds are gated on the ARTIFACT being present, because
		 * both are made from it: the signature does not exist yet and
		 * is produced in the child below, over these same bytes.
		 */
		(void)cache_artifact_path_existing(g_push_current.name, g_push_current.version,
		                                    artifact, sizeof(artifact));
		if (g_push_current.kind == PKG_PUSH_SIGNATURE)
			cache_signature_path_existing(g_push_current.name, g_push_current.version, tarball,
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
	/* The bytes being pushed were found by cache_artifact_path_existing(),
	 * which looks only for a .cixpkg (cix#569). */
	pkg_artifact_build_request(pkgrepo_find(g_push_current.repo), g_push_current.name,
	                           g_push_current.version, PKG_ARTIFACT_FORMAT_CIXPKG, url,
	                           sizeof(url), auth_header, sizeof(auth_header));
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
 * A recipe with no artifact_sha256 approval has its artifact tier skipped
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
 */
/*
 * Writing an artifact approval into a CBS recipe (#492).
 *
 * The approval goes into the recipe in git and the store, and three
 * things about how are each the point rather than an implementation
 * detail.
 *
 * FIRST: it writes TWO files. parse_cbs_recipe() reads the derived
 * explain.json beside the recipe, never the recipe itself (ADR-0305),
 * so an approval written only into the .cbs would be invisible to
 * every build -- the recipe would say approved and the pipeline would
 * rebuild from source forever, which is the bug this fixes wearing a
 * different hat. The recipe and its derived identity move together or
 * not at all.
 *
 * SECOND: the guard is semantic, not textual.
 * "Only the approval changed" is asked of the two EXPLAIN documents --
 * jsondiff_equal_ignoring(..., "artifact_sha256") -- which is one
 * existing implementation (ADR-0292's diff) and is strictly stronger
 * than a text diff: it catches a byte that changes what the recipe
 * MEANS and ignores one that does not. The approval is a key inside a
 * block, not a line, so a line diff would be the wrong rule.
 *
 * THIRD: it refuses rather than guesses. A `metadata { }` block must
 * already exist, on its own line, and the approval is inserted into
 * it. Creating a block would mean choosing a position in a document
 * this daemon deliberately does not parse -- CPDL fixes its
 * declaration order (CPDL-E3003), so the position is real and getting
 * it wrong produces an invalid recipe. Refusing names the three lines
 * to add, once, at the moment the approval would have happened.
 *
 * NOT a sidecar file, and the reason is stronger than the one given
 * when this was asked upstream (cix-build-system#161). `pkg_sync_merge()`
 * pulls recipes from git; a file under g_recipes_dir is daemon-local
 * state no sync carries, so approvals would be per-host and the entire
 * benefit -- that OTHER hosts take the cache hit -- would not happen.
 * The approval travels with the recipe because the recipe is what
 * travels.
 */
static int cbs_metadata_insert_point(const char *text, size_t *out_off)
{
	const char *p = text;

	/*
	 * A line whose only content is `metadata {`. Matched that
	 * narrowly on purpose: upstream's own test fixtures use a
	 * single-line `metadata { "k" "v" }`, and inserting a line after
	 * one of those would put the key OUTSIDE the block -- valid text,
	 * invalid recipe, caught by cbs below but only after a wasted
	 * write. A one-line block is refused with the same message as an
	 * absent one.
	 */
	for (;;) {
		const char *line = p;
		const char *nl = strchr(line, '\n');
		size_t len = (nl != NULL) ? (size_t)(nl - line) : strlen(line);
		const char *t = line;
		size_t tlen = len;

		while (tlen > 0 && (*t == ' ' || *t == '\t')) {
			t++;
			tlen--;
		}
		while (tlen > 0 && (t[tlen - 1] == ' ' || t[tlen - 1] == '\t' || t[tlen - 1] == '\r'))
			tlen--;
		if (tlen == strlen("metadata {") && memcmp(t, "metadata {", tlen) == 0) {
			if (nl == NULL)
				return -1; /* last line of the file: nothing follows to insert before */
			*out_off = (size_t)(nl - text) + 1;
			return 0;
		}
		if (nl == NULL)
			return -1;
		p = nl + 1;
	}
}

static void approve_cbs_artifact(const char *recipe_path, const char *name, const char *version,
                                  const char *sha)
{
	char explain_path[PATH_MAX];
	/*
	 * "" until the candidate is actually written: the cleanup below
	 * unlinks it, and several refusals above reach `done` before there
	 * is anything to unlink. An uninitialised buffer there would ask
	 * the kernel to delete whatever the stack happened to hold.
	 */
	char candidate_path[PATH_MAX] = {0};
	char err[512];
	char *stored = NULL, *updated = NULL, *old_json = NULL, *new_json = NULL;
	size_t stored_len = 0, old_len = 0, new_len = 0, insert_off = 0;
	struct json_value *old_root = NULL, *new_root = NULL;
	char line[PKG_SHA256_MAX + 64];
	size_t line_len;
	int ok = 0;

	if (persist_read_file(recipe_path, &stored, &stored_len) != 0 || stored == NULL)
		return;

	/*
	 * Already approved is asked of the derived identity, never of the
	 * recipe TEXT, and that distinction was a real bug here rather
	 * than a nicety. The first version of this function tested
	 * strstr(stored, "\"artifact_sha256\"") and returned silently on a
	 * hit -- which the very probe written to gate it tripped, because
	 * its header comment quotes the key while explaining what the
	 * approval looks like. A comment mentioning a declaration is not a
	 * declaration. explain.json is the parsed answer and is what the
	 * build path reads anyway, so it is the only thing worth asking.
	 */
	cbs_explain_path(recipe_path, explain_path, sizeof(explain_path));
	if (persist_read_file(explain_path, &old_json, &old_len) != 0 || old_json == NULL)
		goto done;
	old_root = json_parse(old_json, old_len);
	if (old_root == NULL)
		goto done;
	if (json_as_string(json_object_get(json_object_get(old_root, "metadata"),
	                                    "artifact_sha256")) != NULL)
		goto done; /* approved already -- never a replacement (ADR-0107) */

	if (cbs_metadata_insert_point(stored, &insert_off) != 0) {
		logstore_write("cixd", "info",
		               "pkg: %s@%s published, but its recipe has no `metadata {` block on a "
		               "line of its own, so this daemon will not write the approval and the "
		               "package rebuilds from source on every install. Add to the recipe, "
		               "after `requires`:  metadata {  \"artifact_sha256\" \"%s\"  }  (#492)",
		               name, version, sha);
		goto done;
	}

	line_len = (size_t)snprintf(line, sizeof(line), "        \"artifact_sha256\" \"%s\"\n", sha);
	updated = malloc(stored_len + line_len + 1);
	if (updated == NULL)
		goto done;
	memcpy(updated, stored, insert_off);
	memcpy(updated + insert_off, line, line_len);
	memcpy(updated + insert_off + line_len, stored + insert_off, stored_len - insert_off);
	updated[stored_len + line_len] = '\0';

	/*
	 * The candidate must end in .cbs: cbs refuses any other extension,
	 * in `explain` as well as `build` (ADR-0305). Named beside the
	 * recipe so it lands on the same filesystem and the rename below
	 * is atomic.
	 */
	if ((size_t)snprintf(candidate_path, sizeof(candidate_path), "%s.approve.cbs", recipe_path) >=
	    sizeof(candidate_path))
		goto done;
	if (persist_atomic_write(candidate_path, updated, strlen(updated)) != 0)
		goto done;

	new_json = malloc(PKG_EXPLAIN_MAX);
	if (new_json == NULL)
		goto done;
	/*
	 * cbs is the syntax authority: an explain that fails is a recipe
	 * this edit broke, and the recipe is left exactly as it was.
	 */
	if (run_cbs_explain(candidate_path, new_json, PKG_EXPLAIN_MAX, &new_len, err, sizeof(err)) !=
	    0) {
		logstore_write("cixd", "warn",
		               "pkg: %s@%s: writing its approval produced a recipe cbs cannot read "
		               "(%s) -- recipe left untouched (#492)",
		               name, version, err);
		goto done;
	}

	new_root = json_parse(new_json, new_len);
	if (new_root == NULL)
		goto done;

	/*
	 * Absent -> present only, the same asymmetry the shell guard
	 * enforces: replacing an existing approval would let one version
	 * name two different byte sequences, which is what immutability
	 * protects against.
	 */
	if (json_as_string(json_object_get(json_object_get(new_root, "metadata"),
	                                    "artifact_sha256")) == NULL) {
		logstore_write("cixd", "warn",
		               "pkg: %s@%s: the approval did not reach the recipe's metadata block "
		               "-- recipe left untouched (#492)",
		               name, version);
		goto done;
	}

	if (jsondiff_equal_ignoring(old_root, new_root, "artifact_sha256") != 1) {
		logstore_write("cixd", "warn",
		               "pkg: %s@%s: writing its approval changed more than the approval "
		               "-- recipe left untouched (#492)",
		               name, version);
		goto done;
	}

	/*
	 * Both files, recipe first. A crash between the two leaves a
	 * recipe declaring an approval whose explain.json does not carry
	 * it, which reads as unapproved -- the pre-fix behaviour, and the
	 * safe direction to fail in. The reverse order would leave an
	 * explain.json claiming an approval the recipe does not declare,
	 * so a re-derive would silently revoke it.
	 */
	if (rename(candidate_path, recipe_path) != 0)
		goto done;
	if (persist_atomic_write(explain_path, new_json, new_len) != 0) {
		logstore_write("cixd", "error",
		               "pkg: %s@%s: approval written to the recipe but its explain.json could "
		               "not be refreshed -- the package will keep rebuilding from source until "
		               "the identity is re-derived (#492)",
		               name, version);
		goto done;
	}
	ok = 1;
	logstore_write("cixd", "info",
	               "pkg: %s@%s approved its own published artifact (%.16s...) in its metadata "
	               "block",
	               name, version, sha);

done:
	if (!ok && candidate_path[0] != '\0')
		unlink(candidate_path);
	if (old_root != NULL)
		json_free(old_root);
	if (new_root != NULL)
		json_free(new_root);
	free(old_json);
	free(new_json);
	free(updated);
	free(stored);
}

static void approve_published_artifact(const char *name, const char *version)
{
	char artifact[PATH_MAX], cbs_path[PATH_MAX];
	char sha[65];
	struct stat st;

	(void)cache_artifact_path_existing(name, version, artifact, sizeof(artifact));
	if (pkg_run_capture_sha256(artifact, sha, sizeof(sha)) != 0)
		return; /* the artifact is gone from the cache -- nothing to approve */
	/* The recipe is CPDL; there is no other kind (cix#569). */
	if ((size_t)snprintf(cbs_path, sizeof(cbs_path), "%s/%s/%s/%s", g_recipes_dir, name, version,
	                     PKG_RECIPE_CBS_FILE) >= sizeof(cbs_path) ||
	    stat(cbs_path, &st) != 0)
		return;
	approve_cbs_artifact(cbs_path, name, version, sha);
	writeback_after_approval(name, version); /* ADR-0324: git learns it too */
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
		logstore_write("cixd", "info", "artifact push: %s@%s published to %s (HTTP %ld)",
		                g_push_current.name, g_push_current.version, g_push_current.repo, code);
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
			push_enqueue_one(g_push_current.name, g_push_current.version, PKG_PUSH_SIGNATURE,
			                 g_push_current.repo); /* the repository that took the artifact */
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

/*
 * THE name a published artifact has: `<name>-<version>-<arch><suffix>`.
 *
 * `version` is the complete release identity and already carries the
 * release -- `0.2.57-359`, not `0.2.57` (ADR-0312). Nothing here adds
 * a release component, because there is nothing left to add.
 *
 * ONE FUNCTION, because there used to be two and they diverged. The
 * installer ISO built its own name with its own snprintf in main.c,
 * and when #434 changed what version string it was handed -- from the
 * running daemon's tag to the cix ARTIFACT version, which ends in a
 * release -- its hardcoded trailing `-1` became a SECOND release
 * number. The cache's listing preserves the moment it broke:
 *
 *   version=2.57.224     release=1   correct
 *   version=2.57.339-1   release=1   doubled, first bad one
 *   version=0.2.57-359   release=1   doubled
 *
 * Twenty releases shipped malformed. No test caught it, and no test
 * reasonably could have: the two names were assembled in different
 * files from different inputs, so there was no single thing to
 * assert. That is what makes this a parallel implementation rather
 * than a typo, and why the fix is to delete one of them rather than
 * to correct it.
 *
 * `suffix` is passed rather than derived from a package format,
 * because an ISO and its detached signature are published through
 * this same convention and are not package formats -- see
 * artifact_suffix() for the package side.
 */
void pkg_artifact_published_name(const char *name, const char *version, const char *suffix,
                                  char *out, size_t out_size)
{
	snprintf(out, out_size, "%s-%s-%s%s", name != NULL ? name : "",
	         version != NULL ? version : "", pkg_host_arch(), suffix != NULL ? suffix : "");
}

static void pkg_artifact_build_request(const struct pkg_repository *r, const char *name,
                                       const char *version, const char *format,
                                       char *out_url, size_t out_url_size, char *out_header,
                                       size_t out_header_size)
{
	size_t len = strlen(r->url);

	if (len > 0 && r->url[len - 1] == '/')
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
	/*
	 * ADR-0307: the extension comes from the format the recipe
	 * declared, not from a constant. cix-cache already recognises
	 * `.cixpkg` and `.cixpkg.minisig` in its own suffix table and
	 * files them in the package tier, so this is a
	 * name change and not a protocol one -- read from that server's
	 * src/store.c rather than assumed, because four bogus bug
	 * reports have been filed against it from assumptions.
	 */
	{
		/* Through pkg_artifact_published_name(), never composed here:
		 * that function is the one definition of what a published
		 * artifact is called, and the ISO publisher calls it too. */
		char basename[PKG_NAME_MAX + PKG_VERSION_MAX + 64];
		const char *suffix = artifact_suffix(format);

		/* A format nothing produces names no file (cix#569): the URL
		 * is left empty, so the fetch or push that uses it fails. */
		if (suffix == NULL) {
			out_url[0] = '\0';
			out_header[0] = '\0';
			return;
		}
		pkg_artifact_published_name(name, version, suffix, basename, sizeof(basename));
		snprintf(out_url, out_url_size, "%.*s/%s", (int)len, r->url, basename);
	}
	if (r->token[0] != '\0')
		snprintf(out_header, out_header_size, "Authorization: Bearer %s", r->token);
	else
		out_header[0] = '\0';
}

enum pkg_error pkg_artifact_push_request(const struct pkg_repository *r, const char *remote_name,
                                          char *out_url, size_t url_size,
                                          char *out_auth_header, size_t hdr_size)
{
	size_t len;

	if (remote_name == NULL || remote_name[0] == '\0')
		return PKG_ERR_INVALID_NAME;
	if (r->url[0] == '\0')
		return PKG_ERR_NOT_FOUND;

	len = strlen(r->url);
	if (len > 0 && r->url[len - 1] == '/')
		len--;
	snprintf(out_url, url_size, "%.*s/%s", (int)len, r->url, remote_name);
	if (r->token[0] != '\0')
		snprintf(out_auth_header, hdr_size, "Authorization: Bearer %s", r->token);
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
	return pkgrepo_push_count() > 0;
}

/* Where start_fetch_for()'s child stages a checksum-verified artifact
 * fetch before pkg_fetch_completed() promotes it into the real local
 * cache -- deliberately under g_sources_dir (not g_cache_dir): an
 * unverified download never touches the cache directory at all, only
 * a file that has already passed the recipe's own sha256 check does. */
static void artifact_sentinel_path(int chain_idx, const char *name, const char *version,
                                   const char *format, char *out, size_t out_size)
{
	const char *suffix = artifact_suffix(format);

	/* Empty for a format nothing produces (cix#569): nothing is fetched
	 * to it, because pkg_artifact_build_request() names no URL either. */
	out[0] = '\0';
	if (suffix != NULL)
		snprintf(out, out_size, "%s/.artifact-c%d-%s-%s%s", g_sources_dir, chain_idx, name,
		         version, suffix);
}

/* ---- pkg/ redesign Part 4 (ADR-0123): image recipes + image-artifact fetch ---- */


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

/*
 * ADR-0311: an image recipe is a JSON document.
 *
 * Checked on the CONTENT, not on the filename, and that is forced
 * rather than chosen: the daemon's own store is
 * "<image-recipes-dir>/<name>.recipe" (image_recipe_path() above) --
 * one file per image, with no version and no extension in it. ADR-0305's
 * "the filename is the format" governs the corpus, where a recipe is
 * <name>@<version>.<ext>; by the time a recipe reaches this function it
 * has been copied into a store that never carried an extension to
 * dispatch on. So the content is checked: it must open with '{'.
 */
static int parse_image_recipe_json(const char *buf, struct image_recipe *out)
{
	struct json_value *root;
	const struct json_value *packages;
	size_t i;

	root = json_parse(buf, strlen(buf));
	if (root == NULL)
		return -1;
	packages = json_object_get(root, "packages");
	if (packages == NULL || packages->type != JSON_ARRAY ||
	    packages->u.array.count > IMAGE_MANIFEST_MAX_PACKAGES) {
		json_free(root);
		return -1;
	}
	memset(out, 0, sizeof(*out));
	for (i = 0; i < packages->u.array.count; i++) {
		const struct json_value *e = packages->u.array.items[i];
		const char *pkg, *mode, *ver;

		if (e == NULL || e->type != JSON_OBJECT) {
			json_free(root);
			return -1;
		}
		/* package/mode/version, the words POST /v1/images/{name}/manifest
		 * already uses (api_image.c) -- one vocabulary for one concept. */
		pkg = json_as_string(json_object_get(e, "package"));
		mode = json_as_string(json_object_get(e, "mode"));
		ver = json_as_string(json_object_get(e, "version"));
		if (pkg == NULL || mode == NULL || ver == NULL || !pkg_name_is_valid(pkg) ||
		    strlen(ver) >= PKG_VERSION_MAX) {
			json_free(root);
			return -1;
		}
		if (strcmp(mode, "pinned") == 0)
			out->entries[i].mode = IMAGE_PKG_PINNED;
		else if (strcmp(mode, "rolling") == 0)
			out->entries[i].mode = IMAGE_PKG_ROLLING;
		else {
			json_free(root);
			return -1;
		}
		snprintf(out->entries[i].package, sizeof(out->entries[i].package), "%s", pkg);
		snprintf(out->entries[i].version, sizeof(out->entries[i].version), "%s", ver);
	}
	out->entry_count = (int)packages->u.array.count;
	json_free(root);
	if (out->entry_count <= 0)
		return -1;
	return 0;
}

/*
 * An image recipe is a JSON object (ADR-0311), and nothing else: the
 * legacy `image_packages=` assignment, a shell-shaped line in a .sh
 * file, is gone with every other shell recipe (cix#569). A store that
 * still holds one fails here and says so rather than being read.
 */
static int parse_image_recipe_buf(char *buf, struct image_recipe *out)
{
	const char *p = buf;

	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p != '{')
		return -1;
	return parse_image_recipe_json(buf, out);
}

enum pkg_error image_recipe_add(const char *name, const char *content)
{
	struct image_recipe parsed;
	char *buf_copy;
	char staging_path[PATH_MAX], path[PATH_MAX];
	int fd;
	size_t len;
	ssize_t written;
	char *previous = NULL;
	size_t previous_len = 0;
	int changed;

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

	/* ADR-0320: whether this is a new declaration, for recipe: follow. */
	changed = image_recipe_get(name, &previous, &previous_len) != PKG_OK || previous == NULL ||
	          strcmp(previous, content) != 0;
	free(previous);

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
	/*
	 * ADR-0320: under `recipe: follow` the recipe drives the image, so
	 * a changed one is applied as it arrives -- by the 6-hourly sync or
	 * by a publish, which both come through here. The image's own
	 * policy still decides whether that apply converges and whether it
	 * may downgrade; a refusal is logged with every entry it names,
	 * since nobody is waiting on a response.
	 */
	if (changed) {
		struct image_policy policy;

		imagepolicy_get(name, &policy);
		if (policy.recipe == IMAGE_POLICY_RECIPE_FOLLOW) {
			struct pkg_image_apply_result *res = calloc(1, sizeof(*res));
			enum pkg_error aerr = pkg_image_recipe_apply(name, 0, res);

			if (aerr == PKG_ERR_DOWNGRADE_REFUSED && res != NULL) {
				int i;

				for (i = 0; i < res->downgrade_count; i++)
					logstore_write("cixd", "error",
					               "image %s: follows its recipe, and the new one pins %s %s, "
					               "older than %s -- not applied (downgrade: refuse, "
					               "ADR-0320)",
					               name, res->downgrades[i].package, res->downgrades[i].to,
					               res->downgrades[i].from);
			} else if (aerr != PKG_OK) {
				logstore_write("cixd", "error",
				               "image %s: follows its recipe, and applying the new one "
				               "failed (%d) -- not applied (ADR-0320)",
				               name, (int)aerr);
			}
			free(res);
		}
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

_Static_assert(PKG_IMAGE_APPLY_MAX == IMAGE_MANIFEST_MAX_PACKAGES,
               "pkg_image_apply_result holds one downgrade per manifest entry");

/*
 * ADR-0320: the version an image has for package -- installed, else its
 * current pin -- which a recipe pin is compared against to decide
 * whether an apply would move it backwards. "" when it has neither.
 */
static void image_current_package_version(const char *image, const char *package,
                                          const struct image_manifest_entry *manifest,
                                          int manifest_count, char *out, size_t out_size)
{
	const struct pkg_entry *e = pkg_find(package, image);
	int i;

	out[0] = '\0';
	if (e != NULL && e->state == PKG_STATE_INSTALLED && e->version[0] != '\0') {
		snprintf(out, out_size, "%s", e->version);
		return;
	}
	for (i = 0; i < manifest_count; i++) {
		if (strcmp(manifest[i].package, package) == 0 && manifest[i].mode == IMAGE_PKG_PINNED) {
			snprintf(out, out_size, "%s", manifest[i].version);
			return;
		}
	}
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
 * entry into a real rootfs needs an explicit pkg install, the
 * same as any other manifest edit -- unless the image's policy is
 * `apply: converge` (ADR-0320), which queues exactly that install.
 */
enum pkg_error pkg_image_recipe_apply(const char *image, int allow_downgrade,
                                      struct pkg_image_apply_result *out)
{
	char path[PATH_MAX];
	char *buf;
	size_t len;
	struct image_recipe recipe;
	struct image_manifest_entry current[IMAGE_MANIFEST_MAX_PACKAGES];
	int current_count = 0;
	struct image_policy policy;
	int downgrades = 0;
	int i;

	if (out != NULL)
		memset(out, 0, sizeof(*out));
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
	if (image_manifest_read(image, current, &current_count, IMAGE_MANIFEST_MAX_PACKAGES) !=
	    IMAGE_OK)
		return PKG_ERR_TARGET_IMAGE_NOT_FOUND;
	imagepolicy_get(image, &policy);

	/*
	 * ADR-0320: a downgrade is possible, never a side effect. A stale
	 * recipe applied to cix-builder would have walked seven packages
	 * back one to five revisions each (#535), and nothing asked. So
	 * every pinned entry older than what the image has is listed, and
	 * none of the recipe is applied unless the request or the policy
	 * allows it. Rolling entries are floors, and a lower floor moves
	 * nothing installed.
	 */
	for (i = 0; i < recipe.entry_count; i++) {
		char have[PKG_VERSION_MAX];

		if (recipe.entries[i].mode != IMAGE_PKG_PINNED)
			continue;
		image_current_package_version(image, recipe.entries[i].package, current, current_count,
		                              have, sizeof(have));
		if (have[0] == '\0' || pkg_version_compare(recipe.entries[i].version, have) >= 0)
			continue;
		if (out != NULL) {
			struct pkg_image_downgrade *d = &out->downgrades[downgrades];

			snprintf(d->package, sizeof(d->package), "%s", recipe.entries[i].package);
			snprintf(d->from, sizeof(d->from), "%s", have);
			snprintf(d->to, sizeof(d->to), "%s", recipe.entries[i].version);
			out->downgrade_count = downgrades + 1;
		}
		downgrades++;
	}
	if (downgrades > 0 && !allow_downgrade && policy.downgrade != IMAGE_POLICY_DOWNGRADE_ALLOW)
		return PKG_ERR_DOWNGRADE_REFUSED;

	for (i = 0; i < recipe.entry_count; i++) {
		enum image_error ierr =
		    image_manifest_set(image, recipe.entries[i].package, recipe.entries[i].mode,
		                        recipe.entries[i].version);

		if (ierr == IMAGE_ERR_NOT_FOUND)
			return PKG_ERR_TARGET_IMAGE_NOT_FOUND;
		if (ierr != IMAGE_OK)
			return PKG_ERR_INVALID_RECIPE;
		if (out != NULL)
			out->declared++;
	}
	/*
	 * ADR-0330 (#571): under recipe=follow the manifest IS the recipe,
	 * so an entry the recipe no longer lists leaves it. Before this, a
	 * package dropped from an image recipe in git stayed declared
	 * forever -- cix-hosttools 2.4.1 dropped tar, gzip and bzip2 and
	 * the next converge reinstalled them (192.168.15.95, 2026-10-05).
	 * Under recipe=manual the manifest is the operator's, and an apply
	 * only adds and moves entries, as before.
	 */
	if (policy.recipe == IMAGE_POLICY_RECIPE_FOLLOW) {
		int j, k;

		for (j = 0; j < current_count; j++) {
			int listed = 0;

			for (k = 0; k < recipe.entry_count && !listed; k++)
				listed = strcmp(current[j].package, recipe.entries[k].package) == 0;
			if (listed)
				continue;
			if (image_manifest_unset(image, current[j].package) != IMAGE_OK)
				return PKG_ERR_INVALID_RECIPE;
			logstore_write("cixd", "info",
			               "image %s: %s left the manifest -- the recipe it follows no longer "
			               "lists it (ADR-0330)",
			               image, current[j].package);
			if (out != NULL)
				out->removed++;
		}
	}
	/* ADR-0320: converge is the rolling drain's own job -- it installs,
	 * upgrades or downgrades every entry that disagrees with the
	 * manifest, one job at a time -- so it is queued, not re-built. */
	if (policy.apply == IMAGE_POLICY_APPLY_CONVERGE) {
		rebuild_queue_enqueue(image);
		if (out != NULL)
			out->converging = 1;
	}
	logstore_write("cixd", "info",
	               "image %s: applied its recipe -- %d entr%s declared%s%s (ADR-0320)", image,
	               recipe.entry_count, recipe.entry_count == 1 ? "y" : "ies",
	               downgrades > 0 ? ", including downgrades that were allowed" : "",
	               policy.apply == IMAGE_POLICY_APPLY_CONVERGE ? ", converging" : "");
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
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		if (count < max)
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

/*
 * What a pkg_error means, once: its HTTP status and phrase, and the
 * sentence an operator reads. respond_pkg_error() answers a request with
 * it; the host roll (ADR-0327) and the rolling drain (ADR-0330), which
 * have no request, log the sentence. Here rather than in main.c since
 * ADR-0330, so the package code that raises these can describe them too.
 */
int pkg_error_describe(enum pkg_error err, const char **phrase, char *msg, size_t msg_size)
{
	switch (err) {
	case PKG_ERR_DEP_UNRESOLVABLE:
		*phrase = "Bad Request";
		snprintf(msg, msg_size,
		         "a dependency of this package could not be resolved -- the package and its own "
		         "recipe are fine; the daemon log names the dependency and why");
		return 400;
	case PKG_ERR_INVALID_NAME:
		*phrase = "Bad Request";
		snprintf(msg, msg_size, "invalid package name");
		return 400;
	case PKG_ERR_NOT_FOUND:
		*phrase = "Not Found";
		snprintf(msg, msg_size, "no such package");
		return 404;
	case PKG_ERR_INVALID_RECIPE:
		*phrase = "Bad Request";
		snprintf(msg, msg_size, "no such recipe, or it failed to parse");
		return 400;
	case PKG_ERR_DUPLICATE:
		*phrase = "Conflict";
		snprintf(msg, msg_size, "package is already installed");
		return 409;
	case PKG_ERR_BUSY: {
		/*
		 * Name what is holding the slots (#246). "Another install is in
		 * progress" is true and useless: it is the same message whether
		 * a real build is running or a leaked chain slot is holding the
		 * budget with nothing behind it, and telling those apart used to
		 * require reading an unrelated endpoint's error text.
		 */
		char busy[512];
		int n = pkg_active_chain_names(busy, sizeof(busy));

		*phrase = "Conflict";
		if (n > 0)
			snprintf(msg, msg_size,
			         "all %d package job slots are in use (%s) -- wait for one to finish, or "
			         "cancel it with POST /v1/pkg/cancel",
			         n, busy);
		else
			snprintf(msg, msg_size, "another package install is already in progress");
		return 409;
	}
	case PKG_ERR_FULL:
		*phrase = "Internal Server Error";
		snprintf(msg, msg_size, "package table full");
		return 500;
	case PKG_ERR_NOT_BUILDING:
		/*
		 * Issue #213: the entry exists and has no build in flight. 409
		 * rather than 404 -- the package is plainly there, and saying
		 * "no such package" would send the caller looking for the wrong
		 * problem.
		 */
		*phrase = "Conflict";
		snprintf(msg, msg_size, "no build is in flight for that package and image");
		return 409;
	case PKG_ERR_INVALID_TOOLCHAIN:
		*phrase = "Bad Request";
		snprintf(msg, msg_size, "toolchain_path missing, unreadable, or not a regular file");
		return 400;
	case PKG_ERR_BUILD_MEMORY_OVER_CEILING:
		*phrase = "Conflict";
		snprintf(msg, msg_size,
		         "the recipe declares more build memory (resources { memory }) than "
		         "memory_max_ceiling allows -- the cixd log names both; raise it with PUT "
		         "/v1/system/pkg-build-config");
		return 409;
	case PKG_ERR_SPAWN_FAILED:
	case PKG_ERR_PERSIST_FAILED:
	default:
		*phrase = "Internal Server Error";
		snprintf(msg, msg_size, "package operation failed");
		return 500;
	}
}
