#include "pkg.h"
#include "targz.h"
#include "pkgpolicy.h"
#include "hostproc.h"
#include "image.h"
#include "btrfs.h"
#include "ldap.h"
#include "linux_compat.h"
#include "logstore.h"
#include "namecheck.h"
#include "persist.h"
#include "pki.h"
#include "test_image_fixture.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

extern char **environ;

/* Exit code the fetch child uses when it never got as far as running
 * curl, so the reported error does not claim a curl status that never
 * happened. Chosen outside curl's own range of documented codes. */
#define PKG_FETCH_EXIT_PRECONDITION 91

/* PKG_CURL_BIN now lives in pkg.h -- shared with main.c's own
 * bootstrap-fetch mechanism (ADR-0065), one real definition. */
#define PKG_TAR_BIN TARGZ_TAR_BIN
/*
 * Issue #125: tar's own -z shells out to a BARE "gzip", resolved
 * through PATH -- and this daemon runs as PID 1 from the kernel, whose
 * environment carries no PATH at all, so that lookup fails and tar
 * exits 2. Every cache save on a real installed host failed this way,
 * silently, for months. Naming the compressor absolutely removes the
 * lookup; mkbootroot stages this exact path into the control-plane
 * image (its own host_tool_bins list, from gzip.recipe).
 */
#define PKG_GZIP_BIN TARGZ_GZIP_BIN
#define PKG_SHA256SUM_BIN "/usr/bin/sha256sum"
#define PKG_RM_BIN "/bin/rm"
#define PKG_UNSQUASHFS_BIN "/usr/bin/unsquashfs"

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

struct pkg_entry {
	char name[PKG_NAME_MAX];
	char image[PKG_IMAGE_NAME_MAX];
	char version[PKG_VERSION_MAX];
	char depends[PKG_DEPENDS_MAX];
	enum pkg_state state;
	char error[PKG_ERROR_MAX];
	/* Issue #101: what kind of failure `error` describes. Set by
	 * pkg_fail(), which is the only way a package becomes FAILED. */
	enum pkg_failure_kind failure_kind;
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
	char *build_envp[5];
	/* Backing storage for build_envp's own optional 4th entry (ADR-0159
	 * Phase B) -- "CIX_KMOD_EXTRA_SYMBOLS=<value>", built once
	 * g_chains[chain_idx].hostbuild_extra_config_symbols is known, in
	 * pkg_fetch_completed(). */
	char build_envp_extra[PKG_HOSTBUILD_EXTRA_SYMBOLS_MAX + 32];
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
	/* Issue #58: wall-clock time of the last byte drained from this
	 * entry's build-output pipe -- the daemon already owns that pipe,
	 * so "is the build actually producing output?" is answerable
	 * first-class instead of by load-average guesswork (which caused
	 * one real mistaken kill of a healthy 71-minute build, and one
	 * real wedge going unnoticed). 0 until the first byte. */
	time_t last_output_at;
	time_t build_started_at;
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
};

struct pkg_recipe {
	char name[PKG_NAME_MAX];
	char version[PKG_VERSION_MAX];
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
static char g_recipes_dir[PATH_MAX];
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
 * ADR-0157 Phase 2: pkg_image_recipe_apply_start() (a third, unrelated
 * job kind: a bulk artifact fetch with no pkg_entry of its own at all)
 * used to double-use a chain slot's name/image fields directly as its
 * own busy marker -- that stopped being correct once a free chain slot
 * no longer means "nothing running" (another chain could be mid-job),
 * so it now has its own dedicated g_image_apply_busy/g_image_apply_image
 * pair instead (see pkg_any_job_busy()).
 */

struct pkg_chain {
	/* Was g_chains[chain_idx].name -- "" means this chain slot is idle. */
	char name[PKG_NAME_MAX];
	/* The target image the in-flight job (name above, plus every
	 * dependency it pulls in) merges into -- always normalized (never
	 * empty; see normalize_image()), valid exactly when name is
	 * non-empty. For a hostbuild job this is always
	 * PKG_HOSTBUILD_IMAGE (where the resulting pkg_entry is filed, not
	 * where the build container's own lowerdir comes from -- see
	 * build_image below). */
	char image[PKG_IMAGE_NAME_MAX];
	/* True exactly while this chain's job is a hostbuild (ADR-0056) --
	 * set explicitly at the start of every job (pkg_install_start()
	 * clears it, pkg_hostbuild_start() sets it), never left stale from
	 * a prior job, so it's safe to read any time name is non-empty. */
	int is_hostbuild;
	/* Only meaningful while is_hostbuild is true: the real image whose
	 * rootfs supplies the build container's own lowerdir (e.g.
	 * "cix-builder"), as opposed to the always-shared
	 * shared build-sandbox image every ordinary install uses
	 * (PKG_BUILD_SANDBOX_IMAGE). */
	char build_image[PKG_IMAGE_NAME_MAX];
	/*
	 * Issue #168: the composed build environment this chain is using
	 * (a "__buildenv-<hash>" image), empty for a hostbuild or a
	 * cache hit, which need none. Recorded so it can be torn down when
	 * the build finishes: a build environment exists for one build and
	 * should not outlive it. Kept here rather than derived again later
	 * because the recipe it was composed from may have been superseded
	 * by then.
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
	/* ADR-0175/issue #35: this chain's own copy of pkg_install_start()'s/
	 * pkg_hostbuild_start()'s keep_on_failure argument -- read by
	 * pkg_build_completed() at the moment a failure is decided, before
	 * this chain slot is cleared for reuse. */
	int keep_on_failure;
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
static int g_image_apply_busy;
static char g_image_apply_image[PKG_IMAGE_NAME_MAX];

/* Finds a free chain slot (name[0] == '\0'). Returns its index, or -1
 * if every slot is already in use. */
static int chain_alloc(void)
{
	int i;
	int max_jobs = pkg_build_get_max_jobs();

	for (i = 0; i < max_jobs; i++) {
		if (g_chains[i].name[0] == '\0')
			return i;
	}
	return -1;
}

/* True if there is no room for a new top-level job right now -- every
 * chain slot in use, or an image-recipe-apply already in flight (a
 * third, unrelated job kind sharing the same one-daemon-process
 * "how much can genuinely run at once" ceiling). */
static int pkg_any_job_busy(void)
{
	return g_image_apply_busy || chain_alloc() < 0;
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

	for (i = 0; i < PKG_MAX_CONCURRENT_JOBS; i++) {
		if (g_chains[i].name[0] != '\0' &&
		    strcmp(g_chains[i].name, name) == 0 &&
		    strcmp(g_chains[i].image, norm_image) == 0)
			return i;
	}
	return -1;
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
int pkg_active_chain_indices(int *out_indices)
{
	int i, count = 0;

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


const char *pkg_failure_kind_name(enum pkg_failure_kind kind)
{
	switch (kind) {
	case PKG_FAILURE_RECIPE:
		return "recipe";
	case PKG_FAILURE_FETCH:
		return "fetch";
	case PKG_FAILURE_BUILD:
		return "build";
	case PKG_FAILURE_INSTALL:
		return "install";
	case PKG_FAILURE_NONE:
	default:
		return "none";
	}
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
static void pkg_fail(struct pkg_entry *e, int keep_installed, enum pkg_failure_kind kind,
                     const char *fmt, ...)
{
	va_list ap;

	if (e == NULL)
		return;
	e->state = keep_installed ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
	e->failure_kind = kind;
	va_start(ap, fmt);
	vsnprintf(e->error, sizeof(e->error), fmt, ap);
	va_end(ap);
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
static void unlink_manifest_files(const struct pkg_entry *e, const char *rootfs)
{
	int i;

	for (i = 0; i < e->file_count; i++) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s", rootfs, e->files[i]);
		unlink(path);
	}
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

static int parse_recipe(const char *path, struct pkg_recipe *out)
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
	extract_line_value(buf, "pkg_artifact_sha256=", out->artifact_sha256,
	                    sizeof(out->artifact_sha256));
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
	{
		const char *tok = pkg_repo_token();
		int i;

		if (tok != NULL && tok[0] != '\0') {
			for (i = 0; i < out->source_count; i++)
				str_replace_all(out->source[i], sizeof(out->source[i]),
				                "{{REPO_TOKEN}}", tok);
		}
	}

	if (rc != 0 || !pkg_name_is_valid(out->name))
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
 * Resolves name (and optional specific version) to the path of its
 * build.sh under ADR-0107's version-keyed layout:
 * <g_recipes_dir>/<name>/<version>/build.sh. version NULL or ""
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

	snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, name);

	if (version != NULL && version[0] != '\0') {
		struct stat st;

		snprintf(out_path, out_path_size, "%s/%s/build.sh", name_dir, version);
		if (stat(out_path, &st) != 0 || !S_ISREG(st.st_mode))
			return -1;
		return 0;
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
			struct stat st;

			/* A pin is a HOLD: if the pinned version is not published,
			 * that is an error, not a reason to drift to another one.
			 * Silently resolving elsewhere is the exact behaviour a pin
			 * exists to prevent. */
			snprintf(out_path, out_path_size, "%s/%s/build.sh", name_dir, pinned);
			if (stat(out_path, &st) != 0 || !S_ISREG(st.st_mode))
				return -1;
			return 0;
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
				struct stat st;
				int wins;

				if (de->d_name[0] == '.')
					continue;
				snprintf(candidate, sizeof(candidate), "%s/%s/build.sh", name_dir, de->d_name);
				if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode))
					continue;
				if (policy == PKG_POLICY_NEWEST) {
					/* A recipe version's file is written exactly once
					 * and never touched again (ADR-0107 immutability),
					 * so its mtime really is "first published" -- see
					 * recipe_created_at()'s own comment. Ties fall back
					 * to the version order, so the answer is stable
					 * rather than dependent on readdir() order. */
					wins = !have_best || st.st_mtime > best_created ||
					       (st.st_mtime == best_created &&
					        pkg_version_compare(de->d_name, best) > 0);
				} else {
					wins = !have_best || pkg_version_compare(de->d_name, best) > 0;
				}
				if (wins) {
					snprintf(best, sizeof(best), "%s", de->d_name);
					best_created = (long)st.st_mtime;
					have_best = 1;
				}
			}
			closedir(d);
			if (!have_best)
				return -1;
			snprintf(out_path, out_path_size, "%s/%s/build.sh", name_dir, best);
			return 0;
		}
	}
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
 * Captures up to a bounded listing (64KB -- comfortably thousands of
 * short path entries) via `tar -tf`; a mismatch found anywhere within
 * that capture is conclusive (git-archive-without-prefix's own
 * top-level entries diverge immediately -- Makefile, daemon/, docs/,
 * ... -- never needs the full listing to detect). A capture that's
 * still fully consistent when it ends (whether by EOF or by filling
 * the buffer) is treated as "has a common top dir" -- the buffer is
 * sized generously enough that truncation without ever having seen a
 * mismatch is itself strong evidence, not a real gap in practice.
 */
static int tarball_has_common_top_dir(const char *tarball_path)
{
	int pipefd[2];
	pid_t pid;
	int status;
	char buf[65536];
	size_t total = 0;
	ssize_t n;
	char top[PATH_MAX] = { 0 };
	size_t top_len = 0;
	size_t line_start = 0;
	size_t i;

	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return 0;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return 0;
	}
	if (pid == 0) {
		char *argv[] = { (char *)PKG_TAR_BIN, "-tf", (char *)tarball_path, NULL };

		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execve(PKG_TAR_BIN, argv, environ);
		_exit(127);
	}
	close(pipefd[1]);
	while (total + 1 < sizeof(buf)) {
		n = read(pipefd[0], buf + total, sizeof(buf) - total - 1);
		if (n <= 0)
			break;
		total += (size_t)n;
	}
	buf[total] = '\0';
	/*
	 * Drain and reap even if the buffer filled before EOF -- otherwise
	 * a large listing leaves tar blocked writing to a full pipe,
	 * leaking a zombie child. A real, previously-undiscovered bug this
	 * pass found and fixed alongside the truncation trim below: this
	 * drain used to reuse `buf` itself (starting back at index 0) as
	 * its own scratch space, silently overwriting the very capture the
	 * mismatch scan below still needed to read -- invisible for any
	 * tarball whose listing fits inside 64KB (nothing left to drain,
	 * this loop never touches `buf` at all), but real and reproducible
	 * for one that doesn't (confirmed directly on coreutils-9.11.tar.xz:
	 * the drain's own reads landed arbitrary tail fragments like
	 * "thanks-gen" at buf[0], clobbering the real captured head). A
	 * small, separate discard buffer fixes it -- the drained bytes are
	 * never needed for anything, only their being read off the pipe is.
	 */
	{
		char discard[4096];

		while ((n = read(pipefd[0], discard, sizeof(discard))) > 0)
			;
	}
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return 0;

	/*
	 * A real, previously-undiscovered gap: when the buffer fills before
	 * EOF (a large listing -- coreutils-9.11.tar.xz's own `tar -tf`
	 * output is 137KB, well over this 64KB cap), the trailing captured
	 * bytes are an arbitrary mid-line cut, not a real, complete entry.
	 * Confirmed exactly: a real capture ending "...coreutils-9.11/lib/
	 * stdio-read.c\ncoreuti" -- that dangling "coreuti" fragment has no
	 * '/' within it, so the slash-scan below treats it as a short,
	 * mismatched top-level component against the already-established
	 * "coreutils-9.11", false-positiving the whole tarball as having no
	 * common top dir and silently breaking every subsequent pkg_build()
	 * (extract lands one directory level too deep). Only a genuinely
	 * complete, newline-terminated line is real signal -- trim the
	 * dangling fragment off entirely before scanning when truncated.
	 */
	if (total + 1 >= sizeof(buf)) {
		size_t trimmed = total;

		while (trimmed > 0 && buf[trimmed - 1] != '\n')
			trimmed--;
		total = trimmed;
	}

	for (i = 0; i <= total; i++) {
		if (i == total || buf[i] == '\n') {
			size_t line_len = i - line_start;
			size_t slash;

			if (line_len > 0) {
				for (slash = 0; slash < line_len && buf[line_start + slash] != '/'; slash++)
					;
				if (slash == 0 || slash >= sizeof(top))
					return 0; /* an entry with no top-level component at all */
				if (top_len == 0) {
					memcpy(top, buf + line_start, slash);
					top[slash] = '\0';
					top_len = slash;
				} else if (slash != top_len || memcmp(top, buf + line_start, slash) != 0) {
					return 0; /* real mismatch -- no common top dir */
				}
			}
			line_start = i + 1;
		}
	}
	return top_len > 0;
}

static int extract_tarball(const char *tarball_path, const char *dest_dir)
{
	if (tarball_has_common_top_dir(tarball_path)) {
		char *argv[] = { (char *)PKG_TAR_BIN, "-C",           (char *)dest_dir,
			          "--strip-components=1", "-xf", (char *)tarball_path, NULL };

		return run_subprocess(PKG_TAR_BIN, argv);
	}
	{
		char *argv[] = { (char *)PKG_TAR_BIN, "-C", (char *)dest_dir, "-xf",
			          (char *)tarball_path, NULL };

		return run_subprocess(PKG_TAR_BIN, argv);
	}
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
	char *argv[] = { (char *)PKG_RM_BIN, "-rf", (char *)container_base, NULL };

	return run_subprocess(PKG_RM_BIN, argv);
}

int pkg_run_capture_sha256(const char *path, char *out, size_t out_size)
{
	int pipefd[2];
	pid_t pid;
	int status;
	char buf[256] = { 0 };
	size_t total = 0;
	ssize_t n;

	if (out_size < 65)
		return -1;
	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		char *argv[] = { (char *)PKG_SHA256SUM_BIN, (char *)path, NULL };

		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execve(PKG_SHA256SUM_BIN, argv, environ);
		_exit(127);
	}
	close(pipefd[1]);
	while (total + 1 < sizeof(buf)) {
		n = read(pipefd[0], buf + total, sizeof(buf) - total - 1);
		if (n <= 0)
			break;
		total += (size_t)n;
	}
	buf[total] = '\0';
	close(pipefd[0]);
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;

	if (total < 64)
		return -1;
	memcpy(out, buf, 64);
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
static int produce_fail(const char *image, const char *staging, const char *step, int use_errno)
{
	if (use_errno)
		logstore_write("cixd", "error",
		               "image %s: could not produce a new version -- %s failed: %s", image,
		               step, strerror(errno));
	else
		logstore_write("cixd", "error",
		               "image %s: could not produce a new version -- %s failed", image, step);
	if (staging != NULL)
		cix_btrfs_subvol_delete_or_rmtree(staging);
	return -1;
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
	return 0;
}

static void write_pkg_json(const struct pkg_entry *e, struct json_writer *w)
{
	int i;
	const char *state_str;
	char available_version[PKG_VERSION_MAX];
	int has_available = 0;

	switch (e->state) {
	case PKG_STATE_FETCHING:
		state_str = "fetching";
		break;
	case PKG_STATE_BUILDING:
		state_str = "building";
		break;
	case PKG_STATE_INSTALLED:
		state_str = "installed";
		break;
	case PKG_STATE_FAILED:
	default:
		state_str = "failed";
		break;
	}

	/* Re-read fresh from disk every call -- One Source of Truth, no
	 * cached comparison to keep in sync. Only meaningful once installed
	 * (an in-flight fetch/build is already "the latest recipe", by
	 * definition -- it was just resolved from it). */
	if (e->state == PKG_STATE_INSTALLED) {
		char recipe_path[PATH_MAX];
		struct pkg_recipe recipe;

		if (find_recipe_path(e->name, NULL, recipe_path, sizeof(recipe_path)) == 0 &&
		    parse_recipe(recipe_path, &recipe) == 0 && strcmp(recipe.version, e->version) != 0) {
			snprintf(available_version, sizeof(available_version), "%s", recipe.version);
			has_available = 1;
		}
	}

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, e->name);
	jw_key(w, "image");
	jw_str(w, e->image);
	jw_key(w, "version");
	jw_str(w, e->version);
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
	jw_key(w, "error");
	if (e->error[0] != '\0')
		jw_str(w, e->error);
	else
		jw_null(w);
	/* Issue #101: what kind of failure `error` describes, so a caller
	 * deciding whether to retry does not have to read prose. Null when
	 * there is nothing wrong, rather than the string "none" -- absence
	 * of a failure is not a kind of failure. */
	jw_key(w, "failure_kind");
	if (e->failure_kind != PKG_FAILURE_NONE)
		jw_str(w, pkg_failure_kind_name(e->failure_kind));
	else
		jw_null(w);
	/* Issue #58: the first-class hang-vs-slow signal -- null unless a
	 * build is in flight and has produced at least one byte. */
	jw_key(w, "last_output_seconds_ago");
	if (e->state == PKG_STATE_BUILDING && e->last_output_at > 0)
		jw_int(w, (long long)(time(NULL) - e->last_output_at));
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

			/* Issue #101: the phase it died in IS the kind -- a job
			 * killed mid-fetch is a fetch failure to anyone deciding
			 * what to do about it, and the same for a build. */
			pkg_fail(&g_packages[count], 0,
			         g_packages[count].state == PKG_STATE_FETCHING ? PKG_FAILURE_FETCH
			                                                        : PKG_FAILURE_BUILD,
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
	g_image_apply_busy = 0;
	image_recipe_init(pkg_dir);
	container_recipe_init(pkg_dir);
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
	int src;
	unsigned char magic[4];
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
	 * Real, on-disk squashfs magic check ("hsqs", the little-endian
	 * bytes of 0x73717368) via a plain open()+read() -- the same
	 * precedent do_system_update()'s own image_path validation
	 * already established, deliberately not stat()+S_ISREG: a squashfs
	 * image's own bytes are equally valid whether backing a regular
	 * file (the real, intended operator usage -- scp'd onto a real
	 * filesystem path) or a raw block device (unsquashfs itself
	 * neither knows nor cares), so this also naturally supports the
	 * same "write to a raw scratch partition, point cixd at the
	 * device path" self-test technique test_boot_update.c's own
	 * --test-update-image= already uses for do_system_update().
	 */
	if (toolchain_path == NULL || toolchain_path[0] == '\0')
		return PKG_ERR_INVALID_TOOLCHAIN;
	src = open(toolchain_path, O_RDONLY);
	if (src < 0)
		return PKG_ERR_INVALID_TOOLCHAIN;
	if (read(src, magic, 4) != 4 || memcmp(magic, "hsqs", 4) != 0) {
		close(src);
		return PKG_ERR_INVALID_TOOLCHAIN;
	}
	close(src);

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

		if (!g_packages[i].in_use || g_packages[i].state != PKG_STATE_INSTALLED)
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
	return n;
}

static int buildenv_tool_cmp(const void *a, const void *b)
{
	const struct buildenv_tool *ta = a;
	const struct buildenv_tool *tb = b;
	int c = strcmp(ta->name, tb->name);

	return c != 0 ? c : strcmp(ta->version, tb->version);
}

static int buildenv_mutate(const char *staging_rootfs, void *ctx_v)
{
	struct buildenv_ctx *ctx = ctx_v;
	int i;

	/* The same baseline every image gets: device nodes and the handful
	 * of files a process needs to start at all. Not a "tool", and not
	 * something a recipe should have to declare. */
	if (pkg_seed_image_baseline(staging_rootfs) != PKG_OK) {
		logstore_write("cixd", "error",
		               "build environment: seeding the image baseline failed");
		return -1;
	}
	for (i = 0; i < ctx->tool_count; i++) {
		const struct pkg_entry *e = ctx->tools[i].entry;
		char src_rootfs[PATH_MAX];
		char version[IMAGE_VERSION_MAX];
		int f;

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
	return 0;
}

/*
 * Resolves a recipe's declared build tools to an image containing
 * exactly them, creating it the first time and reusing it after.
 * Returns 0 and fills out_image, or -1 with a message saying which
 * declared tool could not be provided.
 */
static int buildenv_image_for(const char *declared, char *out_image, size_t out_image_size,
                               char *err, size_t err_size)
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
		    existing[0] != '\0' && strcmp(existing, empty) != 0)
			return 0;
	}
	{
		struct buildenv_ctx ctx;

		ctx.tools = tools;
		ctx.tool_count = count;
		if (image_produce_new_version(out_image, buildenv_mutate, &ctx, canonical) != 0) {
			/*
			 * Leave nothing half-built behind. image_produce_new_version()
			 * creates the image before it can fail, and a created-but-
			 * empty one is indistinguishable from a real environment by
			 * name alone -- see the guard above for what that cost.
			 */
			image_delete(out_image);
			snprintf(err, err_size, "could not compose a build environment from the declared "
			                        "tools");
			return -1;
		}
	}
	logstore_write("cixd", "info", "pkg: composed build environment %s from %d declared tool(s)",
	               out_image, count);
	return 0;
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
	static const struct {
		const char *src;
		const char *rel_dst;
	} runtime_libs[] = {
		{ "/lib64/ld-linux-x86-64.so.2", "lib64/ld-linux-x86-64.so.2" },
		/*
		 * Second copy of the same file, ADR-0057: glibc >= 2.34's own
		 * libc.so.6 carries a DT_NEEDED entry on "ld-linux-x86-64.so.2"
		 * itself (confirmed via tinycc's own tccelf.c load_dll() walking
		 * libc.so.6's DT_NEEDED list and calling tcc_add_dll() on each
		 * name found) -- TCC resolves that lookup through its ordinary,
		 * general library-search-path list, which does NOT include
		 * /lib64 (confirmed via `tcc -vv`; only its separate, single-path
		 * ELF-interpreter default does). A hostbuild inside a container
		 * whose only libc.so.6 is this staged one therefore needs this
		 * exact file reachable from a path TCC's general search actually
		 * covers too, not just /lib64 (GCC never hit this because it
		 * never needs to resolve its own runtime linker as a DT_NEEDED
		 * lookup at build time the way TCC's loader does).
		 */
		{ "/lib64/ld-linux-x86-64.so.2", "lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" },
		{ "/lib/x86_64-linux-gnu/libc.so.6", "lib/x86_64-linux-gnu/libc.so.6" },
		{ "/lib/x86_64-linux-gnu/libtinfo.so.6", "lib/x86_64-linux-gnu/libtinfo.so.6" },
		{ "/lib/x86_64-linux-gnu/libgcc_s.so.1", "lib/x86_64-linux-gnu/libgcc_s.so.1" },
		{ "/lib/x86_64-linux-gnu/libm.so.6", "lib/x86_64-linux-gnu/libm.so.6" },
		/*
		 * Task #731: glibc's NSS modules (getpwnam()/getgrnam(), needed
		 * by anything doing real Unix account lookups -- sshd's pubkey
		 * auth being the case that surfaced this) are dlopen()'d at
		 * runtime based on /etc/nsswitch.conf, never a direct ELF
		 * NEEDED dependency -- so no ldd-based shared-lib-closure
		 * staging (this table, or any individual recipe's own lib
		 * copying) has ever caught this gap; nothing built by this
		 * platform needed real user/group resolution before task #730's
		 * jump box. Confirmed the hard way: sshd silently rejected
		 * every pubkey auth attempt with no diagnostic pointing at the
		 * real cause until this was staged (getpwnam() just returns
		 * NULL, indistinguishable from "no such user" without directly
		 * nsenter-ing the container to test account lookup in
		 * isolation from SSH-specific causes).
		 */
		{ "/lib/x86_64-linux-gnu/libnss_files.so.2", "lib/x86_64-linux-gnu/libnss_files.so.2" },
	};
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

	for (i = 0; i < sizeof(runtime_libs) / sizeof(runtime_libs[0]); i++) {
		char dst[PATH_MAX], dst_parent[PATH_MAX], *slash;
		struct stat src_st, dst_st;

		snprintf(dst, sizeof(dst), "%s/%s", target_rootfs, runtime_libs[i].rel_dst);
		if (stat(dst, &dst_st) == 0)
			continue; /* already staged -- idempotent */
		if (stat(runtime_libs[i].src, &src_st) != 0)
			continue; /* not present on this host -- skip, not fatal, same
			           * precedent pkg_bootstrap_build_image() already sets */

		snprintf(dst_parent, sizeof(dst_parent), "%s", dst);
		slash = strrchr(dst_parent, '/');
		if (slash != NULL)
			*slash = '\0';

		if (persist_mkdir_p(dst_parent) != 0 || copy_file_simple(runtime_libs[i].src, dst) != 0)
			return PKG_ERR_PERSIST_FAILED;
	}

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

		snprintf(bundle_dst, sizeof(bundle_dst), "%s/etc/ssl/certs/cix-ca-bundle.pem",
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
	 * A minimal, real /etc/nsswitch.conf so the libnss_files.so.2
	 * staged above actually gets consulted -- glibc's own compiled-in
	 * default database list is used when this file is missing, but
	 * that default is a moving target across glibc versions and this
	 * project has no reason to depend on it being correct. "files"
	 * only for every database this platform's own containers could
	 * plausibly need (no "ldap"/"dns" backend entries -- this project
	 * always pushes rendered files into a container's own filesystem
	 * rather than having the container's own NSS talk to a remote
	 * service directly, the same "Cix owns the durable record,
	 * renders into the consumer's own format" posture dns.c/ldap.c
	 * already established for DNS/LDAP).
	 */
	{
		static const char nsswitch_content[] =
		    "passwd: files\ngroup: files\nshadow: files\nhosts: files\n";
		char nsswitch_dst[PATH_MAX];
		struct stat dst_st;

		snprintf(nsswitch_dst, sizeof(nsswitch_dst), "%s/etc/nsswitch.conf", target_rootfs);
		if (stat(nsswitch_dst, &dst_st) != 0) {
			char etc_dir[PATH_MAX];

			snprintf(etc_dir, sizeof(etc_dir), "%s/etc", target_rootfs);
			if (persist_mkdir_p(etc_dir) != 0)
				return PKG_ERR_PERSIST_FAILED;
			if (persist_atomic_write(nsswitch_dst, nsswitch_content,
			                          sizeof(nsswitch_content) - 1) != 0)
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
				struct pkg_recipe r;

				if (vde->d_name[0] == '.')
					continue;
				snprintf(path, sizeof(path), "%s/%s/build.sh", name_dir, vde->d_name);
				if (parse_recipe(path, &r) != 0)
					continue;
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

static void rebuild_queue_pop_front(void)
{
	int i;

	if (g_rebuild_queue_count == 0)
		return;
	for (i = 1; i < g_rebuild_queue_count; i++)
		snprintf(g_rebuild_queue[i - 1], PKG_IMAGE_NAME_MAX, "%s", g_rebuild_queue[i]);
	g_rebuild_queue_count--;
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
	int i;

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
				break;
			}
		}
	}
}

int pkg_try_start_queued_rebuild(pid_t *out_pid, int *out_pidfd, int *out_chain_idx)
{
	if (pkg_any_job_busy())
		return 0;

	while (g_rebuild_queue_count > 0) {
		const char *image = g_rebuild_queue[0];
		struct image_manifest_entry entries[IMAGE_MANIFEST_MAX_PACKAGES];
		int entry_count, i;
		int started = 0;

		if (image_manifest_read(image, entries, &entry_count, IMAGE_MANIFEST_MAX_PACKAGES) !=
		    IMAGE_OK) {
			rebuild_queue_pop_front();
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
				 * queue_rolling_rebuilds_for()). */
				char recipe_path[PATH_MAX];
				struct pkg_recipe recipe;

				satisfied = (e != NULL && e->state == PKG_STATE_INSTALLED &&
				             find_recipe_path(entries[i].package, NULL, recipe_path,
				                               sizeof(recipe_path)) == 0 &&
				             parse_recipe(recipe_path, &recipe) == 0 &&
				             strcmp(recipe.version, e->version) == 0);
			}

			if (!satisfied) {
				const char *want_version =
				    (entries[i].mode == IMAGE_PKG_PINNED) ? entries[i].version : NULL;
				char started_name[PKG_NAME_MAX];

				/* Manifest-driven, automatic install -- like
				 * handle_pkg_update_all()'s own rebuild, never worth
				 * preserving a build container for (ADR-0175). */
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

		if (started)
			return 1;

		/* Every manifest entry already satisfied -- this image is
		 * caught up, drop it and try whatever's queued next. */
		rebuild_queue_pop_front();
	}
	return 0;
}

enum pkg_error pkg_recipe_add(const char *name, const char *content)
{
	char name_dir[PATH_MAX];
	char version_dir[PATH_MAX];
	char staging_path[PATH_MAX];
	char recipe_path[PATH_MAX];
	struct pkg_recipe parsed;
	struct stat st;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;

	if (persist_mkdir_p(g_recipes_dir) != 0)
		return PKG_ERR_PERSIST_FAILED;

	/* Staged under g_recipes_dir itself (not yet inside any
	 * name/version subdirectory -- the version isn't known until the
	 * staged content is parsed below), same ".name.recipe.new"
	 * dotfile-hiding convention this always used. */
	if (snprintf(staging_path, sizeof(staging_path), "%s/.%s.recipe.new", g_recipes_dir, name) >=
	    (int)sizeof(staging_path))
		return PKG_ERR_INVALID_NAME;
	if (persist_atomic_write(staging_path, content, strlen(content)) != 0)
		return PKG_ERR_PERSIST_FAILED;

	if (parse_recipe(staging_path, &parsed) != 0 || strcmp(parsed.name, name) != 0) {
		unlink(staging_path);
		return PKG_ERR_INVALID_RECIPE;
	}

	/* Immutability (ADR-0107): an already-published (name,version) is a
	 * real error, never a silent overwrite. */
	snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, name);
	snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, parsed.version);
	snprintf(recipe_path, sizeof(recipe_path), "%s/build.sh", version_dir);
	if (stat(recipe_path, &st) == 0) {
		unlink(staging_path);
		return PKG_ERR_DUPLICATE;
	}

	if (persist_mkdir_p(version_dir) != 0) {
		unlink(staging_path);
		return PKG_ERR_PERSIST_FAILED;
	}
	if (rename(staging_path, recipe_path) != 0) {
		unlink(staging_path);
		return PKG_ERR_PERSIST_FAILED;
	}
	queue_rolling_rebuilds_for(parsed.name, parsed.version);
	return PKG_OK;
}

enum pkg_error pkg_recipe_delete(const char *name, const char *version)
{
	char name_dir[PATH_MAX];
	struct stat st;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;

	snprintf(name_dir, sizeof(name_dir), "%s/%s", g_recipes_dir, name);

	if (version != NULL && version[0] != '\0') {
		char version_dir[PATH_MAX];
		char recipe_path[PATH_MAX];

		snprintf(version_dir, sizeof(version_dir), "%s/%s", name_dir, version);
		snprintf(recipe_path, sizeof(recipe_path), "%s/build.sh", version_dir);
		if (stat(recipe_path, &st) != 0)
			return PKG_ERR_NOT_FOUND;
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
			snprintf(recipe_path, sizeof(recipe_path), "%s/build.sh", version_dir);
			unlink(recipe_path);
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
	char tarball_path[PATH_MAX];
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

	/* ADR-0122: computed fresh for every queue entry (never left stale
	 * from a prior one) -- a hostbuild job's own artifact never belongs
	 * in the shared package cache (a one-shot host harvest, not
	 * something merged into any image, see g_chains[chain_idx].is_hostbuild's
	 * own doc), so it's never a cache candidate. Held in a local here
	 * (rather than written straight to e->cache_hit) since e isn't
	 * resolved/settled until just below -- a brand new entry gets a
	 * fresh memset() that would wipe a too-early write right back out;
	 * the forked child below reads this same local via its own
	 * fork()-inherited copy, not e->cache_hit, for the identical
	 * reason. */
	cache_hit = !g_chains[chain_idx].is_hostbuild && pkg_cache_has(recipe.name, recipe.version);

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
	e->error[0] = '\0';
	e->kept_build_container[0] = '\0'; /* ADR-0175: a fresh attempt starting means any
	                                     * previously-preserved failed build container's
	                                     * name is no longer this entry's current story --
	                                     * the memset() above already clears it for the
	                                     * fresh-slot case, this covers the upgrade-reuse
	                                     * case (e != NULL, not memset()'d) too. */
	e->cache_hit = cache_hit;

	if (persist_mkdir_p(g_sources_dir) != 0) {
		pkg_fail(e, is_upgrade, PKG_FAILURE_FETCH, "could not create sources directory");
		return PKG_ERR_PERSIST_FAILED;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		pkg_fail(e, is_upgrade, PKG_FAILURE_FETCH, "fork failed");
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
		if (recipe.artifact_sha256[0] != '\0' && pkg_artifact_is_configured()) {
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
			pid_t sub;
			int status = -1;
			int sha_ok = 0;

			pkg_artifact_build_request(recipe.name, recipe.version, artifact_url,
			                            sizeof(artifact_url), artifact_header,
			                            sizeof(artifact_header));
			artifact_sentinel_path(recipe.name, recipe.version, artifact_path,
			                        sizeof(artifact_path));
			unlink(artifact_path);

			sub = fork();
			if (sub == 0) {
				if (artifact_header[0] != '\0') {
					char *argv[] = { (char *)PKG_CURL_BIN, "-fsSL", "-H", artifact_header,
						          "-o",                  artifact_path, artifact_url, NULL };

					execve(PKG_CURL_BIN, argv, environ);
				} else {
					char *argv[] = { (char *)PKG_CURL_BIN, "-fsSL", "-o", artifact_path,
						          artifact_url, NULL };

					execve(PKG_CURL_BIN, argv, environ);
				}
				_exit(127);
			}
			if (sub > 0 && waitpid(sub, &status, 0) == sub && WIFEXITED(status) &&
			    WEXITSTATUS(status) == 0)
				sha_ok = pkg_run_capture_sha256(artifact_path, sha_out, sizeof(sha_out)) == 0;
			if (sha_ok && strcasecmp(sha_out, recipe.artifact_sha256) == 0) {
				_exit(0); /* verified -- pkg_fetch_completed() stages straight from this file */
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
					char note[512];
					int n = snprintf(note, sizeof(note),
					                  "artifact for %s@%s downloaded but failed checksum "
					                  "(recipe approves %.16s..., cache served %.16s...) -- "
					                  "falling back to source; if this recipe version is "
					                  "already published, an edited pkg_artifact_sha256 "
					                  "cannot take effect, bump pkg_version instead",
					                  recipe.name, recipe.version, recipe.artifact_sha256,
					                  sha_out);
					ssize_t written = write(nfd, note, (size_t)n);

					(void)written;
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
			pid_t sub;
			int status;
			int errpipe[2];
			/*
			 * --retry/--retry-all-errors/-C -: large sources
			 * (e.g. kernel.recipe's ~150MB tarball) hit real,
			 * reproducible mid-transfer connection resets in
			 * this project's own dev sandbox (ADR-0056) --
			 * confirmed independent of HTTP version (both
			 * default HTTP/2 and --http1.1 reset at different
			 * offsets). Plain --retry alone is not enough: curl
			 * only auto-retries a curated list of transient
			 * conditions (timeouts, HTTP 5xx/408/429) and does
			 * NOT cover a raw connection reset (curl exit 56)
			 * by default -- confirmed the hard way when a
			 * --retry-only run still failed outright on the
			 * first reset. --retry-all-errors (curl >= 7.71)
			 * widens that to every failure. sha256 verification
			 * downstream in pkg_fetch_completed() still catches
			 * any corrupt resume, so this only ever helps, never
			 * masks a bad download.
			 */
			char *argv[] = { (char *)PKG_CURL_BIN, "-fsSL", "--retry", "8",
				          "--retry-all-errors", "--retry-delay", "3", "-C", "-",
				          "-o", src_tarball_path, recipe.source[j], NULL };

			snprintf(src_tarball_path, sizeof(src_tarball_path), "%s/%s-%s-%d.src",
			         g_sources_dir, recipe.name, recipe.version, j);

			/*
			 * -C - only makes sense resuming *this* attempt's
			 * own partial download (recovering from a transient
			 * mid-transfer reset within curl's own retry loop
			 * above). A stale, already-fully-downloaded file left
			 * over from an earlier, separate fetch attempt at
			 * this same fixed path makes curl request a byte
			 * range starting past EOF, which the server correctly
			 * answers with HTTP 416 -- confirmed the hard way
			 * (ADR-0056): every retry hit the identical 416 since
			 * the file never changed. Starting every fresh
			 * top-level attempt from a clean slate avoids this;
			 * ENOENT is expected and fine.
			 */
			unlink(src_tarball_path);

			/*
			 * curl's own -S text (whatever real reason it failed
			 * for -- unsupported protocol, TLS, DNS, a bad status
			 * code) is the only thing that can tell those apart;
			 * see fetch_error_sidecar_path()'s own comment. Best
			 * effort only: if pipe2() itself fails, fall back to
			 * the old behavior (curl inherits this process's own
			 * stderr) rather than aborting the fetch over a
			 * diagnostics-only setup failure.
			 */
			if (pipe2(errpipe, O_CLOEXEC) != 0) {
				errpipe[0] = -1;
				errpipe[1] = -1;
			}

			sub = fork();
			if (sub < 0) {
				if (errpipe[0] >= 0)
					close(errpipe[0]);
				if (errpipe[1] >= 0)
					close(errpipe[1]);
				_exit(1);
			}
			if (sub == 0) {
				if (errpipe[1] >= 0)
					dup2(errpipe[1], STDERR_FILENO);
				execve(PKG_CURL_BIN, argv, environ);
				_exit(127);
			}
			if (errpipe[1] >= 0)
				close(errpipe[1]);
			if (waitpid(sub, &status, 0) != sub || !WIFEXITED(status) ||
			    WEXITSTATUS(status) != 0) {
				if (errpipe[0] >= 0) {
					char errbuf[512];
					ssize_t n = read(errpipe[0], errbuf, sizeof(errbuf) - 1);
					int fd;

					close(errpipe[0]);
					if (n > 0) {
						errbuf[n] = '\0';
						fd = open(fetch_err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
						if (fd >= 0) {
							ssize_t written = write(fd, errbuf, (size_t)n);
							(void)written;
							close(fd);
						}
					}
				}
				_exit(1);
			}
			if (errpipe[0] >= 0)
				close(errpipe[0]);
		}
		_exit(0);
	}

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		perror("pidfd_open");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		pkg_fail(e, is_upgrade, PKG_FAILURE_FETCH, "could not track fetch subprocess");
		return PKG_ERR_SPAWN_FAILED;
	}

	strncpy(g_chains[chain_idx].name, name, sizeof(g_chains[chain_idx].name) - 1);
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

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	if (image != NULL && image[0] != '\0' && !pkg_image_is_valid(image))
		return PKG_ERR_INVALID_NAME;
	if (pkg_any_job_busy())
		return PKG_ERR_BUSY;
	chain_idx = chain_alloc();

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
	                   sizeof(err)) != 0)
		return PKG_ERR_INVALID_RECIPE;

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

enum pkg_error pkg_hostbuild_start(const char *name, const char *build_image, const char *version,
                                    int upgrade, const char *extra_config_symbols,
                                    int keep_on_failure, pid_t *out_pid, int *out_pidfd,
                                    int *out_chain_idx)
{
	struct pkg_recipe recipe;
	char recipe_path[PATH_MAX];
	struct pkg_entry *e;
	char build_image_version[IMAGE_VERSION_MAX];
	enum pkg_error perr;
	int chain_idx;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	if (build_image == NULL || build_image[0] == '\0' || !pkg_image_is_valid(build_image))
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
	/* Dependency resolution targets "merge into an image" -- meaningless
	 * for a one-shot artifact harvest. Every prerequisite must already
	 * be baked into build_image's own rootfs (built up via ordinary
	 * `pkg install` first). */
	if (recipe.depends[0] != '\0')
		return PKG_ERR_INVALID_RECIPE;

	/* build_image must already exist -- there is no sane default the
	 * way PKG_DEFAULT_IMAGE is for an ordinary install. Resolved via
	 * its own current version (ADR-0107/0108) -- a hostbuild's own
	 * hermetic environment is always build_image's latest built state,
	 * matching pkg_fetch_completed()'s own resolution below. */
	if (image_current_version(build_image, build_image_version, sizeof(build_image_version)) !=
	    IMAGE_OK)
		return PKG_ERR_NOT_FOUND;

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
	snprintf(g_chains[chain_idx].build_image, sizeof(g_chains[chain_idx].build_image), "%s", build_image);
	snprintf(g_chains[chain_idx].target_version, sizeof(g_chains[chain_idx].target_version), "%s",
	         (version != NULL) ? version : "");
	snprintf(g_chains[chain_idx].hostbuild_extra_config_symbols,
	         sizeof(g_chains[chain_idx].hostbuild_extra_config_symbols), "%s",
	         (extra_config_symbols != NULL) ? extra_config_symbols : "");
	g_chains[chain_idx].is_hostbuild = 1;
	g_chains[chain_idx].keep_on_failure = keep_on_failure;

	/* A hostbuild job is always a single, standalone entry -- no
	 * resolve_chain(), pkg_depends is required empty above. */
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
                                       struct container_spec *spec_out, int *out_stdio_write_fd)
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
	spec_out->ov.lowerdir = e->build_lowerdir;
	spec_out->ov.upperdir = e->build_upperdir;
	spec_out->ov.workdir = e->build_workdir;
	spec_out->ov.merged = e->build_merged;
	spec_out->mnt.put_old_rel = ".old_root";
	spec_out->argv = e->build_argv;
	spec_out->envp = e->build_envp;

	e->build_output_captured_len = 0;
	e->last_output_at = 0;
	pkg_build_log_open(e, current_fetch_effective_version(chain_idx)); /* issue #57 */
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

int pkg_fetch_completed(int chain_idx, int exit_status, struct container_spec *spec_out,
                         int *out_stdio_write_fd)
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
	char container_base[PATH_MAX];
	char src_dir[PATH_MAX], dest_dir[PATH_MAX], recipe_dst[PATH_MAX], extra_dir[PATH_MAX];
	int is_final_upgrade;
	int i;

	if (e != NULL && e->state != PKG_STATE_FETCHING)
		return 0; /* stale: this slot has moved on (issue #98) */
	if (e == NULL) {
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
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

		if (exit_status == PKG_FETCH_EXIT_PRECONDITION && detail_len > 0)
			/* Never reached curl -- a precondition failed, and the
			 * sidecar says which. */
			pkg_fail(e, is_final_upgrade, PKG_FAILURE_FETCH, "%s", detail);
		else if (detail_len > 0)
			pkg_fail(e, is_final_upgrade, PKG_FAILURE_FETCH,
			         "fetch failed (curl exit status %d): %s", exit_status, detail);
		else
			pkg_fail(e, is_final_upgrade, PKG_FAILURE_FETCH, "fetch failed (curl exit status %d)",
			         exit_status);
		logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
		g_chains[chain_idx].name[0] = '\0';
		g_chains[chain_idx].dep_queue_count = 0;
		return 0;
	}

	if (find_recipe_path(e->name, current_fetch_effective_version(chain_idx), recipe_path,
	                      sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0) {
		pkg_fail(e, is_final_upgrade, PKG_FAILURE_RECIPE, "recipe became unreadable mid-install");
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

			snprintf(src_path, sizeof(src_path), "%s/%s-%s-%d.src", g_sources_dir, e->name,
			         recipe.version, i);
			if (pkg_run_capture_sha256(src_path, sha_out, sizeof(sha_out)) != 0 ||
			    strcasecmp(sha_out, recipe.sha256[i]) != 0) {
				pkg_fail(e, is_final_upgrade, PKG_FAILURE_FETCH, "checksum mismatch (source %d)",
				         i);
				g_chains[chain_idx].name[0] = '\0';
				g_chains[chain_idx].dep_queue_count = 0;
				return 0;
			}
		}
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
	snprintf(container_base, sizeof(container_base), "%s/%s", g_containers_dir,
	         e->build_container_name);
	/* A hostbuild job's own build container is rooted on build_image's
	 * own current version's rootfs (ADR-0107/0108, built up via
	 * ordinary `pkg install` beforehand), never the shared toolchain
	 * sandbox every regular install uses (ADR-0056). */
	if (g_chains[chain_idx].is_hostbuild) {
		char build_image_version[IMAGE_VERSION_MAX];

		/* pkg_hostbuild_start() already validated build_image has a
		 * current version before this job was ever queued -- a
		 * failure here is unreachable in practice; an empty lowerdir
		 * fails the subsequent overlay mount cleanly instead of
		 * silently reusing a stale path. */
		if (image_current_version(g_chains[chain_idx].build_image, build_image_version,
		                           sizeof(build_image_version)) == IMAGE_OK)
			image_version_rootfs_path(g_chains[chain_idx].build_image, build_image_version,
			                           e->build_lowerdir, sizeof(e->build_lowerdir));
		else
			e->build_lowerdir[0] = '\0';
	} else if (e->cache_hit) {
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

		if (buildenv_image_for(recipe.build_depends, env_image, sizeof(env_image), env_err,
		                        sizeof(env_err)) != 0) {
			pkg_fail(e, is_final_upgrade, PKG_FAILURE_BUILD, "%s", env_err);
			logstore_write("cixd", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
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
		pkg_fail(e, is_final_upgrade, PKG_FAILURE_BUILD,
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
	snprintf(recipe_dst, sizeof(recipe_dst), "%s/build/recipe.sh", e->build_upperdir);
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
			} else if (extract_tarball(main_src_path, src_dir) != 0) {
				prep_step = "extract source tarball";
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
				pkg_fail(e, is_final_upgrade, PKG_FAILURE_BUILD,
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
			pkg_fail(e, is_final_upgrade, PKG_FAILURE_BUILD,
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
				pkg_fail(e, is_final_upgrade, PKG_FAILURE_BUILD,
				         "could not prepare the build container (copy extra source %d failed)", i);
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
	else
		/*
		 * `set -e`, and semicolons rather than `&&`, both deliberately.
		 *
		 * Without set -e a command that fails partway through
		 * pkg_build() or pkg_install() does not fail the package: the
		 * function keeps going and returns the status of whatever ran
		 * last. libcap 2.78-3 shipped that way -- `make install` died
		 * with Error 2, the `rm -rf` after it succeeded, and the
		 * package was recorded as installed with four of its binaries
		 * missing. A package that quietly contains less than it should
		 * is worse than one that fails, because nothing downstream can
		 * tell.
		 *
		 * The separators matter as much as the flag. POSIX suspends
		 * set -e for any command that is part of an && list except the
		 * last, and that suspension applies inside a function called
		 * from there too -- so `pkg_build && pkg_install` would leave
		 * every failure inside pkg_build() ignored, which is precisely
		 * the case that needs catching.
		 */
		snprintf(e->build_argv_cmd, sizeof(e->build_argv_cmd),
		         "set -e; . /build/recipe.sh; cd /build/src; pkg_build; pkg_install");
	/*
	 * /usr/bin/bash, not /bin/sh -- caught empirically (ADR-0056) the
	 * first time a hostbuild job's own build_image was one of this
	 * project's OWN from-recipe images rather than the shared
	 * shared build sandbox: every from-recipe image
	 * follows this project's own no-/bin, usr/bin-only FHS convention
	 * (the exact same reasoning CONSOLE_DEFAULT_CMD in main.c already
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
	e->build_envp[0] = "PKG_DESTDIR=/build/pkg-dest";
	e->build_envp[1] = "PATH=/usr/bin:/bin";
	e->build_envp[2] = "HOME=/build";
	/* ADR-0159 Phase B: only kernel.recipe's own pkg_build() actually
	 * reads this -- every other recipe simply never references it.
	 * Omitted entirely (not just empty) when g_chains[chain_idx] carried nothing,
	 * matching this project's own "no env var an ordinary recipe would
	 * ever need to guard against seeing" posture. */
	if (g_chains[chain_idx].hostbuild_extra_config_symbols[0] != '\0') {
		snprintf(e->build_envp_extra, sizeof(e->build_envp_extra),
		         "CIX_KMOD_EXTRA_SYMBOLS=%s", g_chains[chain_idx].hostbuild_extra_config_symbols);
		e->build_envp[3] = e->build_envp_extra;
		e->build_envp[4] = NULL;
	} else {
		e->build_envp[3] = NULL;
	}

	return start_build_container_spec(chain_idx, e, spec_out, out_stdio_write_fd);
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
	char dest_dir[PATH_MAX], recipe_dst[PATH_MAX];
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
	if (g_image_apply_busy || g_chains[chain_idx].name[0] != '\0')
		return PKG_ERR_BUSY;

	if (find_recipe_path(name, version, recipe_path, sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0)
		return PKG_ERR_INVALID_RECIPE;

	snprintf(recipe_dst, sizeof(recipe_dst), "%s/build/recipe.sh", e->build_upperdir);
	snprintf(dest_dir, sizeof(dest_dir), "%s/build/pkg-dest", e->build_upperdir);

	if (copy_file_simple(recipe_path, recipe_dst) != 0)
		return PKG_ERR_PERSIST_FAILED;
	{
		char *rmargv[] = { (char *)PKG_RM_BIN, "-rf", dest_dir, NULL };

		if (run_subprocess(PKG_RM_BIN, rmargv) != 0)
			return PKG_ERR_PERSIST_FAILED;
	}
	if (persist_mkdir_p(dest_dir) != 0)
		return PKG_ERR_PERSIST_FAILED;

	e->kept_build_container[0] = '\0';

	snprintf(e->build_argv_cmd, sizeof(e->build_argv_cmd), "%s",
	         "set -e; . /build/recipe.sh; cd /build/src; pkg_build; pkg_install");
	e->build_argv[0] = "/usr/bin/bash";
	e->build_argv[1] = "-c";
	e->build_argv[2] = e->build_argv_cmd;
	e->build_argv[3] = NULL;
	e->build_envp[0] = "PKG_DESTDIR=/build/pkg-dest";
	e->build_envp[1] = "PATH=/usr/bin:/bin";
	e->build_envp[2] = "HOME=/build";
	/* See pkg_fetch_completed()'s own identical block -- omitted
	 * entirely (not just empty) when no extra symbols are given. */
	if (extra_config_symbols != NULL && extra_config_symbols[0] != '\0') {
		snprintf(e->build_envp_extra, sizeof(e->build_envp_extra),
		         "CIX_KMOD_EXTRA_SYMBOLS=%s", extra_config_symbols);
		e->build_envp[3] = e->build_envp_extra;
		e->build_envp[4] = NULL;
	} else {
		e->build_envp[3] = NULL;
	}

	g_chains[chain_idx].is_hostbuild = (strcmp(e->image, PKG_HOSTBUILD_IMAGE) == 0);
	g_chains[chain_idx].build_image[0] = '\0'; /* only meaningful while is_hostbuild; unused
	                                              * again here -- build_lowerdir is already
	                                              * final from the original attempt. */
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
	start_build_container_spec(chain_idx, e, spec_out, out_stdio_write_fd);
	return PKG_OK;
}

void pkg_build_spawn_failed(int chain_idx)
{
	struct pkg_entry *e = pkg_find(g_chains[chain_idx].name, g_chains[chain_idx].image);

	if (e != NULL) {
		int is_final_upgrade = g_chains[chain_idx].dep_queue_is_upgrade && (g_chains[chain_idx].dep_queue_pos + 1 >= g_chains[chain_idx].dep_queue_count);

		pkg_fail(e, is_final_upgrade, PKG_FAILURE_BUILD, "could not start the build container");
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

static void pkg_build_output_append(struct pkg_entry *e, const char *data, int len)
{
	int take = (len > PKG_BUILD_OUTPUT_CAPTURE_MAX) ? PKG_BUILD_OUTPUT_CAPTURE_MAX : len;
	int new_total = e->build_output_captured_len + take;

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

static int sandbox_merge_mutate(const char *staging_rootfs, void *ctx_v)
{
	struct sandbox_merge_ctx *ctx = ctx_v;

	return merge_tree(ctx->dest_dir, staging_rootfs, "", NULL);
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

	/* Issue #168: this build is over either way -- success, failure, or
	 * a chained dependency moving on -- so the environment composed for
	 * it goes now. A later step in the same chain composes its own from
	 * its own recipe's declared tools; that is the point. */
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

	if (exit_status != 0) {
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
		static const char *const setup_step_names[] = {
			"mountns_make_private", "overlay_create", "mountns_pivot", "container_dev_mknod",
			"sethostname",          "net_configure",   "net_install_routes",
			"net_enable_ip_forward", "net_apply_sysctl", "prctl(PDEATHSIG)",
		};
		/* overlay_create()'s own six named sub-steps (include/container.h's
		 * enum overlay_error, translated by src/container.c into exit
		 * codes 130-136 -- see that translation's own comment). */
		static const char *const overlay_step_names[] = {
			"overlay_create: stat(lowerdir)", "overlay_create: mkdir(upperdir)",
			"overlay_create: quota tagging",  "overlay_create: mkdir(workdir)",
			"overlay_create: mkdir(merged)",  "overlay_create: options string too long",
			"overlay_create: mount(overlay)",
		};

		if (exit_status >= 110 && exit_status <= 119) {
			const char *step = setup_step_names[exit_status - 110];

			logstore_write("cixd", "error", "pkg %s@%s: build container setup failed (%s)",
			                e->name, g_chains[chain_idx].image, step);
			pkg_fail(e, is_upgrade, PKG_FAILURE_BUILD, "build container setup failed (%s)", step);
		} else if (exit_status >= 130 && exit_status <= 136) {
			const char *step = overlay_step_names[exit_status - 130];

			logstore_write("cixd", "error", "pkg %s@%s: build container setup failed (%s)",
			                e->name, g_chains[chain_idx].image, step);
			pkg_fail(e, is_upgrade, PKG_FAILURE_BUILD, "build container setup failed (%s)", step);
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
			pkg_fail(e, is_upgrade, PKG_FAILURE_BUILD, "build container setup/exec failed: %s",
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
				pkg_fail(e, is_upgrade, PKG_FAILURE_BUILD,
				         "build killed by signal %d (%s)", sig, name);
			else
				pkg_fail(e, is_upgrade, PKG_FAILURE_BUILD, "build killed by signal %d", sig);
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
			pkg_fail(e, is_upgrade, PKG_FAILURE_BUILD,
			         "build failed (exit 127 -- see build output in logs)");
		} else {
			pkg_fail(e, is_upgrade, PKG_FAILURE_BUILD, "build failed (exit status %d)",
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
	e->build_output_captured_len = 0;
	e->last_output_at = 0;

	snprintf(container_base, sizeof(container_base), "%s/%s", g_containers_dir,
	         e->build_container_name);
	snprintf(dest_dir, sizeof(dest_dir), "%s/upper/build/pkg-dest", container_base);

	/* Refresh version from the recipe -- for a fresh install this is
	 * the first time it's set; for an upgrade this is where the entry
	 * finally moves from the old version to the new one. */
	{
		char recipe_path[PATH_MAX];
		struct pkg_recipe recipe;

		if (find_recipe_path(e->name, current_fetch_effective_version(chain_idx), recipe_path,
		                      sizeof(recipe_path)) == 0 &&
		    parse_recipe(recipe_path, &recipe) == 0) {
			strncpy(e->version, recipe.version, sizeof(e->version) - 1);
			strncpy(e->depends, recipe.depends, sizeof(e->depends) - 1);
		}
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
		if (persist_mkdir_p(artifact_dir) != 0 || merge_tree(dest_dir, artifact_dir, "", e) != 0) {
			pkg_fail(e, 0, PKG_FAILURE_INSTALL, "failed to harvest the built artifact");
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			g_chains[chain_idx].is_hostbuild = 0;
			return 0;
		}
	} else {
		struct install_mutate_ctx ctx;

		ctx.dest_dir = dest_dir;
		ctx.e = e;
		ctx.is_upgrade = is_upgrade;
		if (image_produce_new_version(g_chains[chain_idx].image, install_mutate, &ctx, NULL) !=
		    0) {
			pkg_fail(e, 0, PKG_FAILURE_INSTALL,
			         "failed to merge installed files into the target image");
			g_chains[chain_idx].name[0] = '\0';
			g_chains[chain_idx].dep_queue_count = 0;
			return 0;
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

int pkg_find_update_candidate(char *out_name, size_t out_name_size, char *out_image,
                               size_t out_image_size)
{
	int i;

	for (i = 0; i < PKG_MAX_PACKAGES; i++) {
		struct pkg_entry *e = &g_packages[i];
		char recipe_path[PATH_MAX];
		struct pkg_recipe recipe;

		if (!e->in_use || e->state != PKG_STATE_INSTALLED)
			continue;

		/* Same fresh-from-disk drift check write_pkg_json() performs
		 * per-entry for available_version -- One Source of Truth, no
		 * second comparison rule to keep in sync. */
		if (find_recipe_path(e->name, NULL, recipe_path, sizeof(recipe_path)) == 0 &&
		    parse_recipe(recipe_path, &recipe) == 0 && strcmp(recipe.version, e->version) != 0) {
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
static int g_repo_sync_interval_seconds;

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
	jw_key(&w, "sync_interval_seconds");
	jw_int(&w, g_repo_sync_interval_seconds);
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
	g_repo_sync_interval_seconds = 0;

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
	interval = json_object_get(root, "sync_interval_seconds");
	if (interval != NULL)
		g_repo_sync_interval_seconds = (int)json_as_number(interval);

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
	jw_key(w, "sync_interval_seconds");
	jw_int(w, g_repo_sync_interval_seconds);
	jw_obj_close(w);
}

enum pkg_error pkg_repo_set_config(const char *repo_url, const char *repo_kind, const char *ref,
                                    const char *auth_token, int sync_interval_seconds)
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
	if (sync_interval_seconds >= 0)
		g_repo_sync_interval_seconds = sync_interval_seconds;

	if (save_repo_config() != 0)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

int pkg_repo_get_sync_interval_seconds(void)
{
	return g_repo_sync_interval_seconds;
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

enum pkg_error pkg_sync_start(pid_t *out_pid, int *out_pidfd)
{
	char url[PKGREPO_URL_MAX + PKGREPO_TOKEN_MAX + 64];
	char header[320];
	char tarball_path[PATH_MAX];
	pid_t pid;
	int pidfd;

	if (g_sync_pid > 0)
		return PKG_ERR_BUSY;
	if (build_sync_fetch_request(url, sizeof(url), header, sizeof(header)) != 0)
		return PKG_ERR_NOT_FOUND;

	sync_state_path(tarball_path, sizeof(tarball_path));
	unlink(tarball_path);

	pid = fork();
	if (pid < 0)
		return PKG_ERR_SPAWN_FAILED;
	if (pid == 0) {
		if (header[0] != '\0') {
			char *argv[] = { (char *)PKG_CURL_BIN, "-fsSL", "-H",  header,
				          "-o",                  tarball_path, url, NULL };
			execve(PKG_CURL_BIN, argv, environ);
		} else {
			char *argv[] = { (char *)PKG_CURL_BIN, "-fsSL", "-o", tarball_path, url, NULL };
			execve(PKG_CURL_BIN, argv, environ);
		}
		_exit(127);
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

void pkg_sync_completed(int exit_status)
{
	char tarball_path[PATH_MAX];
	char extract_dir[PATH_MAX];
	char recipes_root[PATH_MAX];
	char images_root[PATH_MAX];
	char containers_root[PATH_MAX];
	DIR *names_d;
	struct dirent *name_de;
	int added = 0, skipped = 0, failed = 0;

	g_sync_pid = -1;
	sync_state_path(tarball_path, sizeof(tarball_path));

	if (exit_status != 0) {
		g_sync_last_state = SYNC_FAILED;
		snprintf(g_sync_last_error, sizeof(g_sync_last_error), "fetch failed (curl exit %d)",
		         exit_status);
		unlink(tarball_path);
		return;
	}

	snprintf(extract_dir, sizeof(extract_dir), "%s/sync-extract", g_pkg_dir);
	{
		char *rm_argv[] = { (char *)PKG_RM_BIN, "-rf", extract_dir, NULL };

		run_subprocess(PKG_RM_BIN, rm_argv);
	}
	if (persist_mkdir_p(extract_dir) != 0) {
		g_sync_last_state = SYNC_FAILED;
		snprintf(g_sync_last_error, sizeof(g_sync_last_error),
		         "could not create extraction directory");
		unlink(tarball_path);
		return;
	}
	if (extract_tarball(tarball_path, extract_dir) != 0) {
		g_sync_last_state = SYNC_FAILED;
		snprintf(g_sync_last_error, sizeof(g_sync_last_error), "archive extraction failed");
		unlink(tarball_path);
		return;
	}
	unlink(tarball_path);

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
				char *content;
				size_t content_len;
				struct stat st;
				enum pkg_error rc;

				if (vde->d_name[0] == '.')
					continue;
				snprintf(script_path, sizeof(script_path), "%s/%s/build.sh", name_dir,
				         vde->d_name);
				if (stat(script_path, &st) != 0 || !S_ISREG(st.st_mode))
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

				rc = pkg_recipe_add(name_de->d_name, content);
				free(content);
				if (rc == PKG_OK)
					added++;
				else if (rc == PKG_ERR_DUPLICATE)
					skipped++;
				else
					failed++;
			}
			closedir(vd);
		}
		closedir(names_d);
	}

	snprintf(images_root, sizeof(images_root), "%s/recipes/image", extract_dir);
	sync_walk_image_recipes(images_root, &added, &skipped, &failed);

	snprintf(containers_root, sizeof(containers_root), "%s/recipes/container", extract_dir);
	sync_walk_container_recipes(containers_root, &added, &skipped, &failed);

	{
		char *rm_argv[] = { (char *)PKG_RM_BIN, "-rf", extract_dir, NULL };

		run_subprocess(PKG_RM_BIN, rm_argv);
	}

	/* One-shot: cleared here so the periodic background sync can never
	 * inherit a refetch an operator asked for once. */
	g_sync_refetch_name[0] = '\0';
	g_sync_refetch_version[0] = '\0';

	g_sync_last_state = SYNC_SUCCESS;
	g_sync_last_added = added;
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
	char *argv[6];

	cache_tarball_path(name, version, path, sizeof(path));
	argv[0] = (char *)PKG_TAR_BIN;
	argv[1] = "-C";
	argv[2] = (char *)out_dir;
	argv[3] = "-xzf";
	argv[4] = path;
	argv[5] = NULL;
	return run_subprocess(PKG_TAR_BIN, argv);
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

struct pkg_push_job {
	char name[PKG_NAME_MAX];
	char version[PKG_VERSION_MAX];
};

static struct pkg_push_job g_push_queue[PKG_PUSH_QUEUE_MAX];
static int g_push_queue_count;
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
static void pkg_artifact_push_enqueue(const char *name, const char *version)
{
	if (!g_artifact_push_enabled)
		return;
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
	g_push_queue_count++;
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
		if (g_push_queue_count == 0)
			return 0;

		g_push_current = g_push_queue[0];
		for (i = 1; i < g_push_queue_count; i++)
			g_push_queue[i - 1] = g_push_queue[i];
		g_push_queue_count--;

		cache_tarball_path(g_push_current.name, g_push_current.version, tarball,
		                   sizeof(tarball));
		if (stat(tarball, &st) == 0)
			break;
		/* Evicted by the cache's own LRU between build and push --
		 * real, and worth saying out loud rather than failing mutely. */
		logstore_write("cixd", "info",
		                "artifact push: %s@%s is no longer in the local cache -- not published",
		                g_push_current.name, g_push_current.version);
	}
	pkg_artifact_build_request(g_push_current.name, g_push_current.version, url, sizeof(url),
	                           auth_header, sizeof(auth_header));
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

		/* Digest in the child, deliberately: hashing a large tarball
		 * on the daemon's own thread would stall the event loop for
		 * exactly as long as the file is big. */
		if (pkg_run_capture_sha256(tarball, sha, sizeof(sha)) != 0)
			_exit(PKG_PUSH_EXIT_SHA_FAILED);
		snprintf(sha_header, sizeof(sha_header), "X-Cix-Sha256: %s", sha);
		{
			/* --upload-file is a real PUT and sets Content-Length
			 * from the file itself, which is what the receiver
			 * requires (it refuses chunked encoding). The HTTP status
			 * is written to stdout, redirected to the status file
			 * below, so the reaper can report what the server said
			 * rather than a bare exit code. */
			char *argv[] = { (char *)PKG_CURL_BIN,
				         (char *)"-s",
				         (char *)"--upload-file",
				         tarball,
				         (char *)"-H",
				         auth_header,
				         (char *)"-H",
				         sha_header,
				         (char *)"--output",
				         (char *)"/dev/null",
				         (char *)"--write-out",
				         (char *)"%{http_code}",
				         url,
				         NULL };
			int sfd = open(status_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

			if (sfd >= 0) {
				dup2(sfd, STDOUT_FILENO);
				close(sfd);
			}
			execve(PKG_CURL_BIN, argv, environ);
		}
		_exit(127);
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
	if (code == 201 || code == 200 || code == 204) {
		logstore_write("cixd", "info", "artifact push: %s@%s published (HTTP %ld)",
		                g_push_current.name, g_push_current.version, code);
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
static void pkg_artifact_build_request(const char *name, const char *version, char *out_url,
                                        size_t out_url_size, char *out_header, size_t out_header_size)
{
	size_t len = strlen(g_artifact_base_url);

	if (len > 0 && g_artifact_base_url[len - 1] == '/')
		len--;
	snprintf(out_url, out_url_size, "%.*s/%s-%s.tar.gz", (int)len, g_artifact_base_url, name,
	         version);
	if (g_artifact_token[0] != '\0')
		snprintf(out_header, out_header_size, "Authorization: Bearer %s", g_artifact_token);
	else
		out_header[0] = '\0';
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
	char artifact_sha256[PKG_SHA256_MAX];
};

static char g_image_recipes_dir[PATH_MAX];

/* Own single-job tracker (distinct from g_sync_last_state above --
 * this job mutates shared g_packages[]/image rootfs state, g_current_
 * job_name is the real mutual-exclusion guard; this is status-
 * reporting only, mirroring pkg_sync's own state/last_attempt/error
 * shape for GET /v1/images/recipe-apply-status). */
enum image_apply_state { IMAGE_APPLY_NEVER = 0, IMAGE_APPLY_RUNNING, IMAGE_APPLY_SUCCESS,
                          IMAGE_APPLY_FAILED };
static enum image_apply_state g_image_apply_last_state;
static char g_image_apply_last_image[PKG_IMAGE_NAME_MAX];
static time_t g_image_apply_last_attempt;
static char g_image_apply_last_error[PKG_ERROR_MAX];
static char g_image_apply_target_version[IMAGE_VERSION_MAX];
static char g_image_apply_tarball_path[PATH_MAX];

void image_recipe_init(const char *pkg_dir)
{
	snprintf(g_image_recipes_dir, sizeof(g_image_recipes_dir), "%s/image-recipes", pkg_dir);
	g_image_apply_last_state = IMAGE_APPLY_NEVER;
	g_image_apply_last_image[0] = '\0';
	g_image_apply_last_attempt = 0;
	g_image_apply_last_error[0] = '\0';
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
	/* Optional -- empty means "never opts into the artifact fast path,
	 * apply always bulk-declares the manifest and nothing more." */
	extract_line_value(buf, "image_artifact_sha256=", out->artifact_sha256,
	                    sizeof(out->artifact_sha256));

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
			if (strcmp(key, "URI") == 0 && lc->client_uri[0] != '\0')
				val = lc->client_uri;
			else if (strcmp(key, "BASE_DN") == 0 && lc->base_dn[0] != '\0')
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

/* The canonical, sorted "name@version,..." string for r's own declared
 * entries -- the exact shape image_hash_manifest_string() expects,
 * matching build_image_manifest_string()'s own convention but sourced
 * from a not-yet-applied recipe rather than g_packages[] (ADR-0108). */
static void image_recipe_manifest_string(const struct image_recipe *r, char *out, size_t out_size)
{
	struct image_recipe_ref refs[IMAGE_MANIFEST_MAX_PACKAGES];
	int i;
	size_t pos = 0;

	for (i = 0; i < r->entry_count; i++) {
		refs[i].name = r->entries[i].package;
		refs[i].version = r->entries[i].version;
	}
	qsort(refs, (size_t)r->entry_count, sizeof(refs[0]), image_recipe_ref_cmp);

	out[0] = '\0';
	for (i = 0; i < r->entry_count; i++) {
		int n = snprintf(out + pos, out_size - pos, "%s%s@%s", (i > 0) ? "," : "", refs[i].name,
		                  refs[i].version);

		if (n < 0 || (size_t)n >= out_size - pos)
			break;
		pos += (size_t)n;
	}
}

/* out_url: <pkg_artifact base_url>/images/<name>-<hash>.tar.gz --
 * deliberately under an "images/" prefix so package artifacts
 * (pkg_artifact_build_request()) and whole-image-rootfs artifacts
 * never collide in the same server namespace even though they share
 * one base_url/token config (Part 3's pkg_artifact_*, reused as-is --
 * a second, near-identical config surface would violate One Source of
 * Truth for what is genuinely the same "plain HTTP + bearer token"
 * server). */
static void image_artifact_build_request(const char *name, const char *hash, char *out_url,
                                          size_t out_url_size, char *out_header,
                                          size_t out_header_size)
{
	size_t len = strlen(g_artifact_base_url);

	if (len > 0 && g_artifact_base_url[len - 1] == '/')
		len--;
	snprintf(out_url, out_url_size, "%.*s/images/%s-%s.tar.gz", (int)len, g_artifact_base_url,
	         name, hash);
	if (g_artifact_token[0] != '\0')
		snprintf(out_header, out_header_size, "Authorization: Bearer %s", g_artifact_token);
	else
		out_header[0] = '\0';
}

enum pkg_error pkg_image_recipe_apply_start(const char *image, int *out_async, pid_t *out_pid,
                                             int *out_pidfd)
{
	char path[PATH_MAX];
	char *buf;
	size_t len;
	struct image_recipe recipe;
	char manifest_str[PKG_MANIFEST_STRING_MAX];
	int all_pinned, i;
	pid_t pid;
	int pidfd;
	char url[768], header[320];

	if (!pkg_image_is_valid(image))
		return PKG_ERR_INVALID_NAME;
	if (pkg_any_job_busy())
		return PKG_ERR_BUSY;

	image_recipe_path(image, path, sizeof(path));
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return PKG_ERR_NOT_FOUND;
	if (parse_image_recipe_buf(buf, &recipe) != 0) {
		free(buf);
		return PKG_ERR_INVALID_RECIPE;
	}
	free(buf);

	all_pinned = 1;
	for (i = 0; i < recipe.entry_count; i++) {
		if (recipe.entries[i].mode != IMAGE_PKG_PINNED) {
			all_pinned = 0;
			break;
		}
	}

	if (recipe.artifact_sha256[0] == '\0' || !all_pinned || !pkg_artifact_is_configured()) {
		/* v1 scope (deliberate, not a stop-gap -- see pkg.h's own doc
		 * comment): bulk-declare only. A "rolling" entry has no single
		 * deterministic content an artifact checksum could ever
		 * validly describe; realizing any declared entry into a real
		 * built rootfs still needs an explicit pkg install, exactly
		 * like any other manifest edit already requires today. */
		for (i = 0; i < recipe.entry_count; i++) {
			enum image_error ierr =
			    image_manifest_set(image, recipe.entries[i].package, recipe.entries[i].mode,
			                        recipe.entries[i].version);

			if (ierr == IMAGE_ERR_NOT_FOUND)
				return PKG_ERR_TARGET_IMAGE_NOT_FOUND;
			if (ierr != IMAGE_OK)
				return PKG_ERR_INVALID_RECIPE;
		}
		*out_async = 0;
		return PKG_OK;
	}

	image_recipe_manifest_string(&recipe, manifest_str, sizeof(manifest_str));
	if (image_hash_manifest_string(manifest_str, g_image_apply_target_version,
	                                sizeof(g_image_apply_target_version)) != 0)
		return PKG_ERR_PERSIST_FAILED;

	image_artifact_build_request(image, g_image_apply_target_version, url, sizeof(url), header,
	                              sizeof(header));
	snprintf(g_image_apply_tarball_path, sizeof(g_image_apply_tarball_path),
	         "%s/.image-artifact-%s.tar.gz", g_sources_dir, image);
	if (persist_mkdir_p(g_sources_dir) != 0)
		return PKG_ERR_PERSIST_FAILED;
	unlink(g_image_apply_tarball_path);

	pid = fork();
	if (pid < 0)
		return PKG_ERR_SPAWN_FAILED;
	if (pid == 0) {
		if (header[0] != '\0') {
			char *argv[] = { (char *)PKG_CURL_BIN, "-fsSL", "-H",  header,
				          "-o",                  g_image_apply_tarball_path, url, NULL };

			execve(PKG_CURL_BIN, argv, environ);
		} else {
			char *argv[] = { (char *)PKG_CURL_BIN, "-fsSL", "-o", g_image_apply_tarball_path,
				          url, NULL };

			execve(PKG_CURL_BIN, argv, environ);
		}
		_exit(127);
	}

	pidfd = sys_pidfd_open(pid, 0);
	if (pidfd < 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return PKG_ERR_SPAWN_FAILED;
	}

	g_image_apply_busy = 1;
	snprintf(g_image_apply_image, sizeof(g_image_apply_image), "%s", image);
	g_image_apply_last_state = IMAGE_APPLY_RUNNING;
	snprintf(g_image_apply_last_image, sizeof(g_image_apply_last_image), "%s", image);
	g_image_apply_last_attempt = time(NULL);
	g_image_apply_last_error[0] = '\0';
	*out_async = 1;
	*out_pid = pid;
	*out_pidfd = pidfd;
	return PKG_OK;
}

static void image_apply_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_image_apply_last_error, sizeof(g_image_apply_last_error), fmt, ap);
	va_end(ap);
	g_image_apply_last_state = IMAGE_APPLY_FAILED;
	g_image_apply_busy = 0;
	unlink(g_image_apply_tarball_path);
}

void pkg_image_recipe_apply_completed(int exit_status)
{
	char image[PKG_IMAGE_NAME_MAX];
	char path[PATH_MAX];
	char *buf;
	size_t len;
	struct image_recipe recipe;
	char sha_out[128];
	char rootfs_dir[PATH_MAX], version_dir[PATH_MAX], *slash;
	int i, slot;
	struct pkg_entry *e;

	snprintf(image, sizeof(image), "%s", g_image_apply_image);

	if (exit_status != 0) {
		image_apply_fail("image artifact fetch failed (curl exit status %d)", exit_status);
		return;
	}

	image_recipe_path(image, path, sizeof(path));
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL) {
		image_apply_fail("image recipe for '%s' vanished mid-apply", image);
		return;
	}
	if (parse_image_recipe_buf(buf, &recipe) != 0) {
		free(buf);
		image_apply_fail("image recipe for '%s' became invalid mid-apply", image);
		return;
	}
	free(buf);

	if (pkg_run_capture_sha256(g_image_apply_tarball_path, sha_out, sizeof(sha_out)) != 0 ||
	    strcasecmp(sha_out, recipe.artifact_sha256) != 0) {
		image_apply_fail("image artifact checksum mismatch for '%s'", image);
		return;
	}

	image_version_rootfs_path(image, g_image_apply_target_version, rootfs_dir, sizeof(rootfs_dir));
	snprintf(version_dir, sizeof(version_dir), "%s", rootfs_dir);
	slash = strrchr(version_dir, '/'); /* drop trailing "/rootfs" */
	if (slash != NULL)
		*slash = '\0';
	if (persist_mkdir_p(rootfs_dir) != 0) {
		image_apply_fail("could not create rootfs directory for '%s'", image);
		return;
	}
	{
		char *argv[] = { (char *)PKG_TAR_BIN, "-C", rootfs_dir, "-xzf", g_image_apply_tarball_path,
			          NULL };

		if (run_subprocess(PKG_TAR_BIN, argv) != 0) {
			persist_remove_tree(version_dir);
			image_apply_fail("extracting image artifact for '%s' failed", image);
			return;
		}
	}

	if (image_record_version(image, g_image_apply_target_version) != IMAGE_OK) {
		persist_remove_tree(version_dir);
		image_apply_fail("recording new version for '%s' failed", image);
		return;
	}

	for (i = 0; i < recipe.entry_count; i++) {
		image_manifest_set(image, recipe.entries[i].package, recipe.entries[i].mode,
		                    recipe.entries[i].version);

		/* Mirrors g_packages[] against the rootfs this job just wrote,
		 * so GET /v1/pkg matches reality -- the same slot-reuse-or-
		 * first-free-slot convention start_fetch_for() already uses.
		 * files[] is deliberately left empty: a whole-rootfs artifact
		 * carries no per-package attribution, the same accepted,
		 * documented gap hostbuild packages already have. */
		e = pkg_find(recipe.entries[i].package, image);
		if (e == NULL) {
			slot = -1;
			for (int j = 0; j < PKG_MAX_PACKAGES; j++) {
				if (!g_packages[j].in_use) {
					slot = j;
					break;
				}
			}
			if (slot < 0)
				continue; /* table full -- manifest is still correct, just not mirrored here */
			e = &g_packages[slot];
			memset(e, 0, sizeof(*e));
			e->in_use = 1;
			snprintf(e->name, sizeof(e->name), "%s", recipe.entries[i].package);
			snprintf(e->image, sizeof(e->image), "%s", image);
		} else {
			pkg_entry_free_files(e);
		}
		e->state = PKG_STATE_INSTALLED;
		e->error[0] = '\0';
		snprintf(e->version, sizeof(e->version), "%s", recipe.entries[i].version);
	}
	save_state();

	unlink(g_image_apply_tarball_path);
	g_image_apply_last_state = IMAGE_APPLY_SUCCESS;
	g_image_apply_busy = 0;
}

void pkg_image_recipe_apply_write_json_status(struct json_writer *w)
{
	const char *state_str = g_image_apply_last_state == IMAGE_APPLY_NEVER     ? "never"
	                         : g_image_apply_last_state == IMAGE_APPLY_RUNNING ? "running"
	                         : g_image_apply_last_state == IMAGE_APPLY_SUCCESS ? "success"
	                                                                            : "failed";

	jw_obj_open(w);
	jw_key(w, "state");
	jw_str(w, state_str);
	jw_key(w, "image");
	if (g_image_apply_last_image[0] != '\0')
		jw_str(w, g_image_apply_last_image);
	else
		jw_null(w);
	jw_key(w, "last_attempt");
	if (g_image_apply_last_attempt > 0)
		jw_int(w, (long long)g_image_apply_last_attempt);
	else
		jw_null(w);
	jw_key(w, "error");
	if (g_image_apply_last_error[0] != '\0')
		jw_str(w, g_image_apply_last_error);
	else
		jw_null(w);
	jw_obj_close(w);
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
