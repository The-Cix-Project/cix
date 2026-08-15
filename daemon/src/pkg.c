#include "pkg.h"
#include "image.h"
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

/* PKG_CURL_BIN now lives in pkg.h -- shared with main.c's own
 * bootstrap-fetch mechanism (ADR-0065), one real definition. */
#define PKG_TAR_BIN "/usr/bin/tar"
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
	char **files;
	int file_count;
	int files_cap;
	int in_use;

	/*
	 * ADR-0157 Phase 1: build-transient fields, meaningful only while
	 * state is FETCHING or BUILDING -- moved here from bare module
	 * statics (this is the one specific pkg_entry any given build
	 * concerns) so a future phase can run more than one build
	 * concurrently with no second parallel table (see the ADR's own
	 * Decision section 1). Still only ever one entry actually in this
	 * state at a time in this phase (PKG_MAX_CONCURRENT_JOBS == 1) --
	 * a pure storage relocation, no behavior change yet.
	 */
	int cache_hit;
	char build_lowerdir[PATH_MAX], build_upperdir[PATH_MAX];
	char build_workdir[PATH_MAX], build_merged[PATH_MAX];
	char build_argv_cmd[512];
	char *build_argv[4];
	char *build_envp[4];
	/* See pkg_fetch_completed()'s own comment for why this is a pipe
	 * at all (ADR-0087). -1 when this entry has no build output pipe
	 * currently open. */
	int build_output_rd;
	char build_output_captured[PKG_BUILD_OUTPUT_CAPTURE_MAX + 1];
	int build_output_captured_len;
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
	/* ADR-0122: optional -- empty means this recipe never opts into
	 * precompiled-artifact fetch, always builds from source. When set,
	 * it's the ONLY thing that makes a fetched artifact trustworthy: a
	 * git-tracked, versioned expectation the daemon verifies a fetched
	 * <name>-<version>.tar.gz against (pkg_run_capture_sha256(), same
	 * verify-before-trust discipline pkg_source/pkg_sha256 already has)
	 * before ever treating it as real -- the plain HTTP artifact server
	 * (pkg_artifact_*, below) is never itself a trust boundary. */
	char artifact_sha256[PKG_SHA256_MAX];
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
static char g_pkgbuild_rootfs[PATH_MAX];
/* Where a hostbuild job's own harvested output lands (ADR-0056) --
 * <g_artifacts_dir>/<name>/..., a plain host directory, never
 * container-visible. */
static char g_artifacts_dir[PATH_MAX];

/*
 * ADR-0157 Phase 1: everything genuinely per-*request* rather than
 * per-package -- a dependency chain's own resolved install order and
 * where its result is headed -- moved onto this struct from bare
 * module statics (was g_chains[0].name/_image/_is_hostbuild/
 * _build_image/_target_version, g_chains[0].dep_queue/_count/_pos/_is_upgrade).
 * Sized to PKG_MAX_CONCURRENT_JOBS (1 for Phase 1 -- a later phase
 * raises this and adds the config surface, not this struct's shape).
 *
 * name/image also double as this daemon's own coarse "one job of any
 * kind in flight" busy lock -- pkg_image_recipe_apply_start() (a
 * third, unrelated job kind: a bulk artifact fetch with no pkg_entry
 * of its own at all) sets these two fields directly as a pure busy
 * marker, exactly as it already did on the old bare globals, never
 * touching dep_queue/is_hostbuild/etc. A name+image STRING pair
 * (never a struct pkg_entry* here) is what makes that sharing still
 * work correctly after this move -- a pointer couldn't represent "busy
 * for a reason that isn't any single package."
 */
#define PKG_MAX_CONCURRENT_JOBS 1

struct pkg_chain {
	/* Was g_chains[0].name -- "" means this chain slot is idle. */
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
	 * "kanxeo-builder"), as opposed to the always-shared
	 * g_pkgbuild_rootfs every ordinary install uses. */
	char build_image[PKG_IMAGE_NAME_MAX];
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
};

static struct pkg_chain g_chains[PKG_MAX_CONCURRENT_JOBS];

/*
 * ADR-0157 Phase 1: which pkg_entry currently owns the open build-
 * output capture pipe (ADR-0087) -- deliberately a separate pointer,
 * not derived from g_chains[0].name/image: pkg_build_output_close()
 * can run (via pkg_build_spawn_failed()) AFTER the owning chain has
 * already been cleared back to idle, so looking the entry up by name
 * at that point would fail. Set the instant a build's own output pipe
 * is opened (pkg_fetch_completed()), cleared back to NULL once
 * pkg_build_output_close() actually closes it. NULL means no build
 * output pipe is currently open (pkg_build_output_fd() returns -1).
 */
static struct pkg_entry *g_build_output_entry;

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
	 * NULL means "plain recursive copy, no manifest" -- merge_tree()'s
	 * hostbuild caller (ADR-0056) passes e=NULL since a harvested
	 * artifact has nothing to ever pkg_delete(), so there's no
	 * manifest to record it in.
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
	/* depends and artifact_sha256 are both optional -- fine if absent */
	extract_line_value(buf, "pkg_depends=", out->depends, sizeof(out->depends));
	extract_line_value(buf, "pkg_artifact_sha256=", out->artifact_sha256,
	                    sizeof(out->artifact_sha256));
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

	{
		DIR *d = opendir(name_dir);
		struct dirent *de;
		char best[PKG_VERSION_MAX];
		int have_best = 0;

		if (d == NULL)
			return -1;
		while ((de = readdir(d)) != NULL) {
			char candidate[PATH_MAX];
			struct stat st;

			if (de->d_name[0] == '.')
				continue;
			snprintf(candidate, sizeof(candidate), "%s/%s/build.sh", name_dir, de->d_name);
			if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			if (!have_best || pkg_version_compare(de->d_name, best) > 0) {
				snprintf(best, sizeof(best), "%s", de->d_name);
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

static int run_subprocess(const char *bin, char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		logstore_write("kanxeod", "error", "run_subprocess %s: fork failed: %s", bin,
		                strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execve(bin, argv, environ);
		perror(bin);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		logstore_write("kanxeod", "error", "run_subprocess %s: waitpid failed: %s", bin,
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
			logstore_write("kanxeod", "error",
			                "run_subprocess %s: exec failed (missing binary or bad path?)",
			                bin);
		else
			logstore_write("kanxeod", "error", "run_subprocess %s: exited with status %d",
			                bin, WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr, "%s: killed by signal %d\n", bin, WTERMSIG(status));
		logstore_write("kanxeod", "error", "run_subprocess %s: killed by signal %d", bin,
		                WTERMSIG(status));
	}
	return -1;
}

/*
 * Whether every entry in tarball_path's own listing shares the same
 * single top-level path component -- the shape --strip-components=1
 * assumes (a real release tarball's own "foo-1.2.3/" wrapping
 * directory). Confirmed a real, previously-undiscovered gap (ADR-0093):
 * git archive without --prefix= (gitea's own archive-download REST
 * endpoint, used by kanxeo.recipe's own self-build source snapshot)
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
static const char *url_basename(const char *url)
{
	const char *slash = strrchr(url, '/');

	return slash != NULL ? slash + 1 : url;
}

/*
 * __pkgbuild is a single, reserved, reused container path (v1
 * serializes builds to one at a time) -- without this, a later
 * build's /build/pkg-dest would silently inherit leftover files from
 * whatever the previous build (chained dependency or a completely
 * unrelated earlier install) left in the same upperdir. Called before
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
static int merge_tree(const char *src_root, const char *dst_root, const char *relpath,
                       struct pkg_entry *e)
{
	char src_dir[PATH_MAX];
	DIR *d;
	struct dirent *de;

	snprintf(src_dir, sizeof(src_dir), "%s%s%s", src_root, relpath[0] ? "/" : "", relpath);
	d = opendir(src_dir);
	if (d == NULL)
		return relpath[0] == '\0' ? 0 : -1;

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
			return -1;
		}

		if (S_ISDIR(st.st_mode)) {
			snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_root, child_rel);
			persist_mkdir_p(dst_path);
			if (merge_tree(src_root, dst_root, child_rel, e) != 0) {
				closedir(d);
				return -1;
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
			if (copy_file_simple(src_path, dst_path) != 0) {
				closedir(d);
				return -1;
			}
			chmod(dst_path, st.st_mode & 0777);
			if (pkg_entry_add_file(e, child_rel) != 0) {
				closedir(d);
				return -1;
			}
		} else if (S_ISLNK(st.st_mode)) {
			char target[PATH_MAX];
			ssize_t len;
			char dst_parent[PATH_MAX], *slash;

			len = readlink(src_path, target, sizeof(target) - 1);
			if (len < 0) {
				closedir(d);
				return -1;
			}
			target[len] = '\0';

			snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_root, child_rel);
			snprintf(dst_parent, sizeof(dst_parent), "%s", dst_path);
			slash = strrchr(dst_parent, '/');
			if (slash != NULL) {
				*slash = '\0';
				persist_mkdir_p(dst_parent);
			}
			unlink(dst_path); /* EEXIST tolerance for a re-install/upgrade,
			                    * same spirit copy_file_simple()'s own
			                    * O_CREAT|O_TRUNC already has for regular files */
			if (symlink(target, dst_path) != 0) {
				closedir(d);
				return -1;
			}
			if (pkg_entry_add_file(e, child_rel) != 0) {
				closedir(d);
				return -1;
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
static int image_produce_new_version(const char *image,
                                      int (*mutate)(const char *staging_rootfs, void *ctx),
                                      void *ctx)
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
		return -1;

	if (image_current_version(image, old_version, sizeof(old_version)) != IMAGE_OK)
		return -1;
	image_version_rootfs_path(image, old_version, old_rootfs, sizeof(old_rootfs));

	snprintf(staging, sizeof(staging), "%s/%s/.staging.%d", g_images_dir, image, (int)getpid());
	persist_remove_tree(staging); /* clear any leftover from a prior crashed attempt */
	if (persist_mkdir_p(staging) != 0)
		return -1;

	if (copy_tree_hardlink(old_rootfs, staging) != 0) {
		persist_remove_tree(staging);
		return -1;
	}

	if (mutate(staging, ctx) != 0) {
		persist_remove_tree(staging);
		return -1;
	}

	manifest_str = build_image_manifest_string(image);
	if (image_hash_manifest_string(manifest_str, new_version, sizeof(new_version)) != 0) {
		persist_remove_tree(staging);
		return -1;
	}

	image_version_rootfs_path(image, new_version, new_rootfs, sizeof(new_rootfs));
	if (stat(new_rootfs, &st) == 0) {
		/* Already produced before -- discard this build, trust the
		 * existing immutable copy (see this function's own comment). */
		persist_remove_tree(staging);
	} else {
		snprintf(version_dir, sizeof(version_dir), "%s", new_rootfs);
		slash = strrchr(version_dir, '/'); /* drop the trailing "/rootfs" component */
		if (slash != NULL)
			*slash = '\0';
		if (persist_mkdir_p(version_dir) != 0 || rename(staging, new_rootfs) != 0) {
			persist_remove_tree(staging);
			return -1;
		}
	}

	return image_record_version(image, new_version) == IMAGE_OK ? 0 : -1;
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
	snprintf(g_pkgbuild_rootfs, sizeof(g_pkgbuild_rootfs), "%s/pkgbuild/rootfs", images_dir);
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
	if (snprintf(g_pkgbuild_rootfs, sizeof(g_pkgbuild_rootfs), "%s/pkgbuild/rootfs", images_dir) >=
	    (int)sizeof(g_pkgbuild_rootfs))
		return -1;
	if (snprintf(g_artifacts_dir, sizeof(g_artifacts_dir), "%s", artifacts_dir) >=
	    (int)sizeof(g_artifacts_dir))
		return -1;

	memset(g_packages, 0, sizeof(g_packages));
	memset(g_chains, 0, sizeof(g_chains));
	g_build_output_entry = NULL;
	image_recipe_init(pkg_dir);
	container_recipe_init(pkg_dir);
	return load_state();
}

/*
 * Live-copy fallback: stages a toolchain by copying from whatever host
 * kanxeod itself happens to be running on. Fine for dev/test convenience
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
enum pkg_error pkg_bootstrap_build_image(void)
{
	if (test_image_fixture_stage_toolchain(g_pkgbuild_rootfs) != 0)
		return PKG_ERR_SPAWN_FAILED;
	return PKG_OK;
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
	char *argv[] = { (char *)PKG_UNSQUASHFS_BIN, "-f", "-no-xattrs", "-d", g_pkgbuild_rootfs,
		          (char *)toolchain_path, NULL };

	/*
	 * Real, on-disk squashfs magic check ("hsqs", the little-endian
	 * bytes of 0x73717368) via a plain open()+read() -- the same
	 * precedent do_system_update()'s own image_path validation
	 * already established, deliberately not stat()+S_ISREG: a squashfs
	 * image's own bytes are equally valid whether backing a regular
	 * file (the real, intended operator usage -- scp'd onto a real
	 * filesystem path) or a raw block device (unsquashfs itself
	 * neither knows nor cares), so this also naturally supports the
	 * same "write to a raw scratch partition, point kanxeod at the
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

	if (persist_mkdir_p(g_pkgbuild_rootfs) != 0)
		return PKG_ERR_PERSIST_FAILED;

	if (run_subprocess(PKG_UNSQUASHFS_BIN, argv) != 0)
		return PKG_ERR_SPAWN_FAILED;

	return PKG_OK;
}

int pkg_toolchain_has_gcc(void)
{
	char gcc_path[PATH_MAX];
	struct stat st;

	snprintf(gcc_path, sizeof(gcc_path), "%s/usr/bin/gcc", g_pkgbuild_rootfs);
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
	 * (curl, openssl, ...) can verify a Kanxeo-issued cert without
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

		snprintf(bundle_dst, sizeof(bundle_dst), "%s/etc/ssl/certs/kanxeo-ca-bundle.pem",
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
	 * service directly, the same "Kanxeo owns the durable record,
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

int pkg_try_start_queued_rebuild(pid_t *out_pid, int *out_pidfd)
{
	if (g_chains[0].name[0] != '\0')
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

				if (pkg_install_start(entries[i].package, image, want_version, e != NULL,
				                       started_name, sizeof(started_name), out_pid,
				                       out_pidfd) == PKG_OK) {
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
static int resolve_chain(const char *name, const char *version, int force,
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

	existing = pkg_find(name, g_chains[0].image);
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
		if (resolve_chain(tok, NULL, 0, queue, count, visiting, visiting_count, err_out,
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
 * g_chains[0].dep_queue's own post-order construction (resolve_chain()) always
 * places that entry last, and it's the only entry a version pin
 * (g_chains[0].target_version) ever applies to (ADR-0107).
 */
static int current_fetch_is_target(void)
{
	return g_chains[0].dep_queue_pos == g_chains[0].dep_queue_count - 1;
}

/*
 * The version to resolve the currently-fetching/-building entry's own
 * recipe at: the job's requested pin, but ONLY for the top-level
 * target entry -- every dependency always resolves to its own highest
 * available version regardless of what the target is pinned to.
 * Returns NULL for "resolve to highest available", the pre-existing
 * behavior for every case that isn't an explicit top-level pin.
 */
static const char *current_fetch_effective_version(void)
{
	if (current_fetch_is_target() && g_chains[0].target_version[0] != '\0')
		return g_chains[0].target_version;
	return NULL;
}

/* ADR-0122: forward declarations -- defined below (near the end of this
 * file, alongside the rest of the local-cache/artifact-server module)
 * but needed here since start_fetch_for()/pkg_fetch_completed()/
 * pkg_build_completed() are the only call sites and all come first. */
static int pkg_cache_has(const char *name, const char *version);
static void pkg_cache_touch(const char *name, const char *version);
static void pkg_cache_save(const char *name, const char *version, const char *dest_dir);
static void pkg_cache_save_from_file(const char *name, const char *version, const char *src_path);
static int pkg_cache_extract(const char *name, const char *version, const char *out_dir);
static int pkg_artifact_is_configured(void);
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
static enum pkg_error start_fetch_for(const char *name, pid_t *out_pid, int *out_pidfd)
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

	if (find_recipe_path(name, current_fetch_effective_version(), recipe_path,
	                      sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0)
		return PKG_ERR_INVALID_RECIPE;

	/* ADR-0122: computed fresh for every queue entry (never left stale
	 * from a prior one) -- a hostbuild job's own artifact never belongs
	 * in the shared package cache (a one-shot host harvest, not
	 * something merged into any image, see g_chains[0].is_hostbuild's
	 * own doc), so it's never a cache candidate. Held in a local here
	 * (rather than written straight to e->cache_hit) since e isn't
	 * resolved/settled until just below -- a brand new entry gets a
	 * fresh memset() that would wipe a too-early write right back out;
	 * the forked child below reads this same local via its own
	 * fork()-inherited copy, not e->cache_hit, for the identical
	 * reason. */
	cache_hit = !g_chains[0].is_hostbuild && pkg_cache_has(recipe.name, recipe.version);

	e = pkg_find(name, g_chains[0].image);
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
		strncpy(e->image, g_chains[0].image, sizeof(e->image) - 1);
	}
	e->state = PKG_STATE_FETCHING;
	e->error[0] = '\0';
	e->cache_hit = cache_hit;

	if (persist_mkdir_p(g_sources_dir) != 0) {
		e->state = is_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "could not create sources directory");
		return PKG_ERR_PERSIST_FAILED;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		e->state = is_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "fork failed");
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
			char sha_out[128];
			pid_t sub;
			int status;

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
			    WEXITSTATUS(status) == 0 &&
			    pkg_run_capture_sha256(artifact_path, sha_out, sizeof(sha_out)) == 0 &&
			    strcasecmp(sha_out, recipe.artifact_sha256) == 0) {
				_exit(0); /* verified -- pkg_fetch_completed() stages straight from this file */
			}
			unlink(artifact_path); /* not found / wrong checksum -- discard, fall through */
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
		e->state = is_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "could not track fetch subprocess");
		return PKG_ERR_SPAWN_FAILED;
	}

	strncpy(g_chains[0].name, name, sizeof(g_chains[0].name) - 1);
	*out_pid = pid;
	*out_pidfd = pidfd;
	return PKG_OK;
}

enum pkg_error pkg_install_start(const char *name, const char *image, const char *version,
                                  int upgrade, char *out_started_name,
                                  size_t out_started_name_size, pid_t *out_pid, int *out_pidfd)
{
	struct pkg_recipe recipe;
	char recipe_path[PATH_MAX];
	struct pkg_entry *e;
	char visiting[PKG_MAX_DEP_CHAIN][PKG_NAME_MAX];
	int visiting_count = 0;
	char err[PKG_ERROR_MAX];
	enum pkg_error perr;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	if (image != NULL && image[0] != '\0' && !pkg_image_is_valid(image))
		return PKG_ERR_INVALID_NAME;
	if (g_chains[0].name[0] != '\0')
		return PKG_ERR_BUSY;

	if (find_recipe_path(name, version, recipe_path, sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0)
		return PKG_ERR_INVALID_RECIPE;

	snprintf(g_chains[0].image, sizeof(g_chains[0].image), "%s", normalize_image(image));
	snprintf(g_chains[0].target_version, sizeof(g_chains[0].target_version), "%s",
	         (version != NULL) ? version : "");
	g_chains[0].is_hostbuild = 0;

	e = pkg_find(name, g_chains[0].image);
	if (e != NULL && e->state == PKG_STATE_INSTALLED &&
	    (!upgrade || strcmp(e->version, recipe.version) == 0))
		return PKG_ERR_DUPLICATE;

	/* Resolve the full install order: name's own dependencies first
	 * (skipped if already satisfied), then name itself -- force=1 so
	 * an explicit upgrade target is queued even though it's already
	 * installed (resolve_chain()'s default "already installed, skip"
	 * rule is for pure dependencies, not the package actually asked for). */
	g_chains[0].dep_queue_count = 0;
	if (resolve_chain(name, version, 1, g_chains[0].dep_queue, &g_chains[0].dep_queue_count, visiting, &visiting_count,
	                   err, sizeof(err)) != 0)
		return PKG_ERR_INVALID_RECIPE;

	g_chains[0].dep_queue_pos = 0;
	g_chains[0].dep_queue_is_upgrade = (e != NULL && e->state == PKG_STATE_INSTALLED);

	perr = start_fetch_for(g_chains[0].dep_queue[0], out_pid, out_pidfd);
	if (perr != PKG_OK) {
		g_chains[0].dep_queue_count = 0;
		return perr;
	}
	snprintf(out_started_name, out_started_name_size, "%s", g_chains[0].dep_queue[0]);
	return PKG_OK;
}

enum pkg_error pkg_hostbuild_start(const char *name, const char *build_image, const char *version,
                                    int upgrade, pid_t *out_pid, int *out_pidfd)
{
	struct pkg_recipe recipe;
	char recipe_path[PATH_MAX];
	struct pkg_entry *e;
	char build_image_version[IMAGE_VERSION_MAX];
	enum pkg_error perr;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	if (build_image == NULL || build_image[0] == '\0' || !pkg_image_is_valid(build_image))
		return PKG_ERR_INVALID_NAME;
	if (g_chains[0].name[0] != '\0')
		return PKG_ERR_BUSY;

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
	 * name (e.g. a new commit under kanxeo.recipe's own tracked tag).
	 * Same rule as install: only actually re-run the build if the
	 * recipe's own version genuinely differs from what's already
	 * installed -- upgrade=1 with an unchanged version is still a
	 * no-op duplicate, since nothing would actually be different.
	 */
	e = pkg_find(name, PKG_HOSTBUILD_IMAGE);
	if (e != NULL && e->state == PKG_STATE_INSTALLED &&
	    (!upgrade || strcmp(e->version, recipe.version) == 0))
		return PKG_ERR_DUPLICATE;

	snprintf(g_chains[0].image, sizeof(g_chains[0].image), "%s", PKG_HOSTBUILD_IMAGE);
	snprintf(g_chains[0].build_image, sizeof(g_chains[0].build_image), "%s", build_image);
	snprintf(g_chains[0].target_version, sizeof(g_chains[0].target_version), "%s",
	         (version != NULL) ? version : "");
	g_chains[0].is_hostbuild = 1;

	/* A hostbuild job is always a single, standalone entry -- no
	 * resolve_chain(), pkg_depends is required empty above. */
	g_chains[0].dep_queue_count = 0;
	strncpy(g_chains[0].dep_queue[0], name, PKG_NAME_MAX - 1);
	g_chains[0].dep_queue[0][PKG_NAME_MAX - 1] = '\0';
	g_chains[0].dep_queue_count = 1;
	g_chains[0].dep_queue_pos = 0;
	g_chains[0].dep_queue_is_upgrade = 0;

	perr = start_fetch_for(name, out_pid, out_pidfd);
	if (perr != PKG_OK) {
		g_chains[0].dep_queue_count = 0;
		g_chains[0].is_hostbuild = 0;
		return perr;
	}
	return PKG_OK;
}

int pkg_fetch_completed(int exit_status, struct container_spec *spec_out, int *out_stdio_write_fd)
{
	struct pkg_entry *e = pkg_find(g_chains[0].name, g_chains[0].image);
	char recipe_path[PATH_MAX];
	struct pkg_recipe recipe;
	char sha_out[128];
	char container_base[PATH_MAX];
	char src_dir[PATH_MAX], dest_dir[PATH_MAX], recipe_dst[PATH_MAX], extra_dir[PATH_MAX];
	int is_final_upgrade;
	int i;

	if (e == NULL) {
		g_chains[0].name[0] = '\0';
		g_chains[0].dep_queue_count = 0;
		return 0;
	}
	is_final_upgrade = g_chains[0].dep_queue_is_upgrade && (g_chains[0].dep_queue_pos + 1 >= g_chains[0].dep_queue_count);

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

		e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		if (detail_len > 0)
			snprintf(e->error, sizeof(e->error), "fetch failed (curl exit status %d): %s",
			         exit_status, detail);
		else
			snprintf(e->error, sizeof(e->error), "fetch failed (curl exit status %d)", exit_status);
		logstore_write("kanxeod", "error", "pkg %s@%s: %s", e->name, e->image, e->error);
		g_chains[0].name[0] = '\0';
		g_chains[0].dep_queue_count = 0;
		return 0;
	}

	if (find_recipe_path(e->name, current_fetch_effective_version(), recipe_path,
	                      sizeof(recipe_path)) != 0 ||
	    parse_recipe(recipe_path, &recipe) != 0) {
		e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "recipe became unreadable mid-install");
		g_chains[0].name[0] = '\0';
		g_chains[0].dep_queue_count = 0;
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
				e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
				snprintf(e->error, sizeof(e->error), "checksum mismatch (source %d)", i);
				g_chains[0].name[0] = '\0';
				g_chains[0].dep_queue_count = 0;
				return 0;
			}
		}
	}

	snprintf(container_base, sizeof(container_base), "%s/%s", g_containers_dir,
	         PKG_BUILD_CONTAINER_NAME);
	/* A hostbuild job's own build container is rooted on build_image's
	 * own current version's rootfs (ADR-0107/0108, built up via
	 * ordinary `pkg install` beforehand), never the shared toolchain
	 * sandbox every regular install uses (ADR-0056). */
	if (g_chains[0].is_hostbuild) {
		char build_image_version[IMAGE_VERSION_MAX];

		/* pkg_hostbuild_start() already validated build_image has a
		 * current version before this job was ever queued -- a
		 * failure here is unreachable in practice; an empty lowerdir
		 * fails the subsequent overlay mount cleanly instead of
		 * silently reusing a stale path. */
		if (image_current_version(g_chains[0].build_image, build_image_version,
		                           sizeof(build_image_version)) == IMAGE_OK)
			image_version_rootfs_path(g_chains[0].build_image, build_image_version,
			                           e->build_lowerdir, sizeof(e->build_lowerdir));
		else
			e->build_lowerdir[0] = '\0';
	} else {
		snprintf(e->build_lowerdir, sizeof(e->build_lowerdir), "%s", g_pkgbuild_rootfs);
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
				if (pkg_cache_extract(e->name, recipe.version, dest_dir) != 0)
					prep_step = "extract cached artifact";
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
					logstore_write("kanxeod", "error",
					                "pkg %s@%s: could not prepare build container (%s): %s",
					                e->name, g_chains[0].image, prep_step,
					                strerror(prep_errno));
				else
					logstore_write("kanxeod", "error",
					                "pkg %s@%s: could not prepare build container (%s) -- see run_subprocess detail above",
					                e->name, g_chains[0].image, prep_step);
				e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
				snprintf(e->error, sizeof(e->error),
				         "could not prepare the build container (%s failed)", prep_step);
				g_chains[0].name[0] = '\0';
				g_chains[0].dep_queue_count = 0;
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
			logstore_write("kanxeod", "error",
			                "pkg %s@%s: could not prepare build container (create extra dir): %s",
			                e->name, g_chains[0].image, strerror(errno));
			e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
			snprintf(e->error, sizeof(e->error), "could not prepare the build container (create extra dir failed)");
			g_chains[0].name[0] = '\0';
			g_chains[0].dep_queue_count = 0;
			return 0;
		}
		for (i = 1; i < recipe.source_count; i++) {
			char src_path[PATH_MAX], extra_dst[PATH_MAX];

			snprintf(src_path, sizeof(src_path), "%s/%s-%s-%d.src", g_sources_dir, e->name,
			         recipe.version, i);
			snprintf(extra_dst, sizeof(extra_dst), "%s/%s", extra_dir,
			         url_basename(recipe.source[i]));
			if (copy_file_simple(src_path, extra_dst) != 0) {
				logstore_write("kanxeod", "error",
				                "pkg %s@%s: could not prepare build container (copy extra source %d): %s",
				                e->name, g_chains[0].image, i, strerror(errno));
				e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
				snprintf(e->error, sizeof(e->error),
				         "could not prepare the build container (copy extra source %d failed)", i);
				g_chains[0].name[0] = '\0';
				g_chains[0].dep_queue_count = 0;
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
		snprintf(e->build_argv_cmd, sizeof(e->build_argv_cmd),
		         ". /build/recipe.sh; cd /build/src && pkg_build && pkg_install");
	/*
	 * /usr/bin/bash, not /bin/sh -- caught empirically (ADR-0056) the
	 * first time a hostbuild job's own build_image was one of this
	 * project's OWN from-recipe images rather than the shared
	 * g_pkgbuild_rootfs toolchain sandbox: every from-recipe image
	 * follows this project's own no-/bin, usr/bin-only FHS convention
	 * (the exact same reasoning CONSOLE_DEFAULT_CMD in main.c already
	 * documents), so "/bin/sh" -- which happened to work for years
	 * only because the toolchain sandbox is a wholesale host /usr copy
	 * with a real bin -> usr/bin symlink -- fails outright
	 * (execve: No such file or directory) the moment the lowerdir is
	 * a real, minimal Kanxeo-built image instead. bash accepts the
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
	e->build_envp[3] = NULL;

	memset(spec_out, 0, sizeof(*spec_out));
	spec_out->ns.clone_flags =
	    CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWCGROUP | CLONE_INTO_CGROUP;
	spec_out->ns.hostname = PKG_BUILD_CONTAINER_NAME;
	spec_out->cg.name = PKG_BUILD_CONTAINER_NAME;
	spec_out->ov.lowerdir = e->build_lowerdir;
	spec_out->ov.upperdir = e->build_upperdir;
	spec_out->ov.workdir = e->build_workdir;
	spec_out->ov.merged = e->build_merged;
	spec_out->mnt.put_old_rel = ".old_root";
	spec_out->argv = e->build_argv;
	spec_out->envp = e->build_envp;

	e->build_output_captured_len = 0;
	/* ADR-0157 Phase 1: this entry is now the one owning the open
	 * build-output pipe, regardless of whether pipe2()/fcntl() below
	 * actually succeed (both failure branches still explicitly set
	 * build_output_rd to -1, which every reader already treats as
	 * "nothing to read") -- see g_build_output_entry's own doc comment
	 * for why pkg_build_output_close() needs this separate from
	 * g_chains[0].name/image. */
	g_build_output_entry = e;
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
	return 1;
}

void pkg_build_spawn_failed(void)
{
	struct pkg_entry *e = pkg_find(g_chains[0].name, g_chains[0].image);

	if (e != NULL) {
		int is_final_upgrade = g_chains[0].dep_queue_is_upgrade && (g_chains[0].dep_queue_pos + 1 >= g_chains[0].dep_queue_count);

		e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "could not start the build container");
	}
	g_chains[0].name[0] = '\0';
	g_chains[0].dep_queue_count = 0;
	/*
	 * registry_create() never spawned a container, so no epoll
	 * registration for its build_output_rd exists anywhere to drive
	 * its own EOF-triggered close (see register_pkg_build_output()/
	 * handle_pkg_build_output_event() in main.c) -- this is the one
	 * path that has to close it directly.
	 */
	pkg_build_output_close();
}

int pkg_build_output_fd(void)
{
	return g_build_output_entry != NULL ? g_build_output_entry->build_output_rd : -1;
}

static void pkg_build_output_append(struct pkg_entry *e, const char *data, int len)
{
	int take = (len > PKG_BUILD_OUTPUT_CAPTURE_MAX) ? PKG_BUILD_OUTPUT_CAPTURE_MAX : len;
	int new_total = e->build_output_captured_len + take;

	if (new_total > PKG_BUILD_OUTPUT_CAPTURE_MAX) {
		int overflow = new_total - PKG_BUILD_OUTPUT_CAPTURE_MAX;

		memmove(e->build_output_captured, e->build_output_captured + overflow,
		        e->build_output_captured_len - overflow);
		e->build_output_captured_len -= overflow;
	}
	memcpy(e->build_output_captured + e->build_output_captured_len, data + (len - take), take);
	e->build_output_captured_len += take;
}

int pkg_build_output_readable(char *new_data, int new_data_cap, int *new_data_len)
{
	char chunk[4096];
	ssize_t n;

	if (new_data_len != NULL)
		*new_data_len = 0;

	if (g_build_output_entry == NULL || g_build_output_entry->build_output_rd < 0)
		return 1;

	for (;;) {
		n = read(g_build_output_entry->build_output_rd, chunk, sizeof(chunk));
		if (n > 0) {
			pkg_build_output_append(g_build_output_entry, chunk, (int)n);
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
int pkg_build_output_snapshot(char *out, int out_cap)
{
	int take;

	if (g_build_output_entry == NULL)
		return 0;
	take = (g_build_output_entry->build_output_captured_len > out_cap)
	           ? out_cap
	           : g_build_output_entry->build_output_captured_len;

	memcpy(out, g_build_output_entry->build_output_captured, take);
	return take;
}

void pkg_build_output_close(void)
{
	if (g_build_output_entry != NULL && g_build_output_entry->build_output_rd >= 0) {
		close(g_build_output_entry->build_output_rd);
		g_build_output_entry->build_output_rd = -1;
	}
	g_build_output_entry = NULL;
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

int pkg_build_completed(const char *container_name, int exit_status, pid_t *out_pid,
                         int *out_pidfd, char *out_hostbuild_done_name)
{
	struct pkg_entry *e;
	char container_base[PATH_MAX];
	char dest_dir[PATH_MAX];
	int is_final, is_upgrade;

	out_hostbuild_done_name[0] = '\0';

	if (strcmp(container_name, PKG_BUILD_CONTAINER_NAME) != 0)
		return 0;

	e = pkg_find(g_chains[0].name, g_chains[0].image);
	if (e == NULL) {
		g_chains[0].name[0] = '\0';
		g_chains[0].dep_queue_count = 0;
		return 0;
	}

	is_final = (g_chains[0].dep_queue_pos + 1 >= g_chains[0].dep_queue_count);
	is_upgrade = is_final && g_chains[0].dep_queue_is_upgrade;

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

		pkg_build_output_readable(NULL, 0, NULL);
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

			logstore_write("kanxeod", "error", "pkg %s@%s: build container setup failed (%s)",
			                e->name, g_chains[0].image, step);
			snprintf(e->error, sizeof(e->error), "build container setup failed (%s)", step);
		} else if (exit_status >= 130 && exit_status <= 136) {
			const char *step = overlay_step_names[exit_status - 130];

			logstore_write("kanxeod", "error", "pkg %s@%s: build container setup failed (%s)",
			                e->name, g_chains[0].image, step);
			snprintf(e->error, sizeof(e->error), "build container setup failed (%s)", step);
		} else if (exit_status >= 141 && exit_status <= 255) {
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

			logstore_write("kanxeod", "error",
			                "pkg %s@%s: build container setup/exec failed: %s", e->name,
			                g_chains[0].image, strerror(real_errno));
			snprintf(e->error, sizeof(e->error), "build container setup/exec failed: %s",
			         strerror(real_errno));
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
			logstore_write("kanxeod", "error",
			                "pkg %s@%s: build failed with exit 127 (ambiguous: either an "
			                "execve() errno too large to encode, or a real \"command not "
			                "found\" from inside the recipe's own build script -- check the "
			                "build output logged separately)",
			                e->name, g_chains[0].image);
			snprintf(e->error, sizeof(e->error), "build failed (exit 127 -- see build output in logs)");
		} else {
			snprintf(e->error, sizeof(e->error), "build failed (exit status %d)", exit_status);
		}
		if (captured_len > 0)
			logstore_write("kanxeod", "error", "pkg %s@%s: build output: %s", e->name,
			                g_chains[0].image, captured);
		e->state = is_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		g_chains[0].name[0] = '\0';
		g_chains[0].dep_queue_count = 0; /* abort the rest of the chain -- a failed dependency
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

	snprintf(container_base, sizeof(container_base), "%s/%s", g_containers_dir,
	         PKG_BUILD_CONTAINER_NAME);
	snprintf(dest_dir, sizeof(dest_dir), "%s/upper/build/pkg-dest", container_base);

	/* Refresh version from the recipe -- for a fresh install this is
	 * the first time it's set; for an upgrade this is where the entry
	 * finally moves from the old version to the new one. */
	{
		char recipe_path[PATH_MAX];
		struct pkg_recipe recipe;

		if (find_recipe_path(e->name, current_fetch_effective_version(), recipe_path,
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

	if (g_chains[0].is_hostbuild) {
		/* A hostbuild's output is a standalone host artifact (a
		 * bzImage, a kanxeod-root squashfs's own components), not
		 * something that belongs inside any container image's
		 * rootfs -- harvested to a plain host directory instead of
		 * merged into an image, and with no manifest (NULL) since
		 * there is no image install to ever unlink it from
		 * (ADR-0056). */
		char artifact_dir[PATH_MAX];

		snprintf(artifact_dir, sizeof(artifact_dir), "%s/%s", g_artifacts_dir, e->name);
		if (persist_mkdir_p(artifact_dir) != 0 || merge_tree(dest_dir, artifact_dir, "", NULL) != 0) {
			e->state = PKG_STATE_FAILED;
			snprintf(e->error, sizeof(e->error), "failed to harvest the built artifact");
			g_chains[0].name[0] = '\0';
			g_chains[0].dep_queue_count = 0;
			g_chains[0].is_hostbuild = 0;
			return 0;
		}
	} else {
		struct install_mutate_ctx ctx;

		ctx.dest_dir = dest_dir;
		ctx.e = e;
		ctx.is_upgrade = is_upgrade;
		if (image_produce_new_version(g_chains[0].image, install_mutate, &ctx) != 0) {
			e->state = PKG_STATE_FAILED;
			snprintf(e->error, sizeof(e->error),
			         "failed to merge installed files into the target image");
			g_chains[0].name[0] = '\0';
			g_chains[0].dep_queue_count = 0;
			return 0;
		}

		/* ADR-0122: a fresh, real build's own output is saved into the
		 * local cache for next time (best-effort, LRU-evicting older
		 * entries as needed); a cache/artifact hit's own dest_dir was
		 * itself just extracted FROM the cache, so there's nothing new
		 * to save -- just bump its mtime so LRU eviction correctly
		 * treats it as recently used, not stale. */
		if (e->cache_hit)
			pkg_cache_touch(e->name, e->version);
		else
			pkg_cache_save(e->name, e->version, dest_dir);

		/*
		 * ALSO merge into g_pkgbuild_rootfs (the one shared, persistent
		 * toolchain sandbox every ordinary install's own build container
		 * uses as its lowerdir -- see g_pkgbuild_rootfs's own comment)
		 * -- not just the target image above. Found as a real, genuine
		 * gap, not speculative: sbsigntools.recipe (Part 5, bare-metal-
		 * readiness plan) needs bfd.h/uuid.h/efi.h from binutils-dev/
		 * libuuid/gnu-efi, each already merged into the "dev" target
		 * image by an earlier install -- but g_pkgbuild_rootfs is a
		 * fixed snapshot, staged once from this host's own real
		 * toolchain (pkg_bootstrap_build_image()) and never otherwise
		 * touched by a package install, so it never had bfd.h at all.
		 * Every prior recipe with a real pkg_depends (gcc on m4/
		 * binutils, autoconf on m4, ...) happened not to expose this:
		 * those dependencies' own tools already existed in this
		 * sandbox's real host toolchain, masking the gap until a
		 * recipe needed a header/library that only this project's own
		 * package manager -- never the bare host OS -- had ever
		 * installed. e=NULL: g_pkgbuild_rootfs is a build-time-only
		 * sandbox, never itself an installed image a manifest could
		 * ever need to unlink from (the same NULL-manifest shape the
		 * hostbuild-artifact merge above already uses). A failure here
		 * is deliberately non-fatal (best-effort) -- the real,
		 * authoritative install (the target-image merge just above)
		 * already succeeded; a future recipe losing this particular
		 * build-time visibility is a real but strictly smaller problem
		 * than unwinding an otherwise-successful install over it.
		 */
		merge_tree(dest_dir, g_pkgbuild_rootfs, "", NULL);
	}

	save_state();

	if (g_chains[0].is_hostbuild)
		snprintf(out_hostbuild_done_name, PKG_NAME_MAX, "%s", e->name);

	if (!is_final) {
		enum pkg_error perr;

		g_chains[0].dep_queue_pos++;
		perr = start_fetch_for(g_chains[0].dep_queue[g_chains[0].dep_queue_pos], out_pid, out_pidfd);
		if (perr == PKG_OK)
			return 1;
		/* couldn't start the next dependency -- abort the chain */
	}

	g_chains[0].name[0] = '\0';
	g_chains[0].dep_queue_count = 0;
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

enum pkg_error pkg_get_one(const char *name, const char *image, struct json_writer *w)
{
	struct pkg_entry *e = pkg_find(name, image);

	if (e == NULL)
		return PKG_ERR_NOT_FOUND;
	write_pkg_json(e, w);
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
	if (strcmp(g_chains[0].name, name) == 0 &&
	    strcmp(g_chains[0].image, normalize_image(image)) == 0)
		return PKG_ERR_BUSY;

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

	if (image_produce_new_version(normalize_image(image), delete_mutate, &ctx) != 0) {
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
static int g_repo_sync_interval_seconds;

static pid_t g_sync_pid = -1;
enum sync_state { SYNC_NEVER = 0, SYNC_RUNNING, SYNC_SUCCESS, SYNC_FAILED };
static enum sync_state g_sync_last_state = SYNC_NEVER;
static time_t g_sync_last_attempt;
static int g_sync_last_added;
static int g_sync_last_skipped;
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
 * verbatim (e.g. ".../itdlabs/kanxeo/src/branch/master/recipes/package",
 * exactly what a forge's own address bar shows while browsing a repo
 * -- an entirely natural thing to copy-paste) into a garbage owner
 * ("itdlabs/kanxeo/src/branch/master/pkg") and repo ("recipes"),
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
		 * own kanxeo.recipe pkg_source already relies on. */
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

	if (persist_mkdir_p(g_cache_dir) != 0)
		return;
	cache_tarball_path(name, version, final_path, sizeof(final_path));
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-%d", final_path, (int)getpid());
	unlink(tmp_path);

	{
		char *argv[] = { (char *)PKG_TAR_BIN, "-C", (char *)dest_dir, "-czf", tmp_path, ".", NULL };

		if (run_subprocess(PKG_TAR_BIN, argv) != 0) {
			unlink(tmp_path);
			return;
		}
	}
	if (stat(tmp_path, &st) != 0) {
		unlink(tmp_path);
		return;
	}
	size = (long long)st.st_size;
	if (size > g_cache_max_bytes) {
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
 * kanxeod) at a fixed, predictable path this daemon computes itself:
 * <base_url>/<name>-<version>.tar.gz -- the exact same naming
 * convention the local cache already uses. Trust never comes from
 * the server (see struct pkg_recipe's own artifact_sha256 comment):
 * the artifact is only ever accepted after verifying it against the
 * recipe's own git-tracked checksum.
 */

static char g_artifact_base_url[PKGARTIFACT_URL_MAX];
static char g_artifact_token[PKGARTIFACT_TOKEN_MAX];
static char g_artifact_config_path[PATH_MAX];

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
	jw_obj_close(w);
}

/* NULL leaves that field unchanged (partial PUT, same contract
 * pkg_repo_set_config() already has); "" for auth_token explicitly
 * clears it. */
enum pkg_error pkg_artifact_set_config(const char *base_url, const char *auth_token)
{
	if (base_url != NULL)
		snprintf(g_artifact_base_url, sizeof(g_artifact_base_url), "%s", base_url);
	if (auth_token != NULL)
		snprintf(g_artifact_token, sizeof(g_artifact_token), "%s", auth_token);

	if (save_artifact_config() != 0)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}

static int pkg_artifact_is_configured(void)
{
	return g_artifact_base_url[0] != '\0';
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
 * committed to git (the same class of gap kanxeo.recipe's own
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
		const char *tok = strstr(p, "{{SECRET:");
		const char *key_end;
		char key[128];
		size_t key_len;
		const struct json_value *jval;
		const char *val;

		if (tok == NULL) {
			jw_raw_text(&w, p, strlen(p));
			break;
		}
		jw_raw_text(&w, p, (size_t)(tok - p));

		key_end = strstr(tok + 9, "}}");
		if (key_end == NULL) {
			/* No closing "}}" anywhere -- not a real token, copy the
			 * rest verbatim rather than loop forever. */
			jw_raw_text(&w, tok, strlen(tok));
			break;
		}
		key_len = (size_t)(key_end - (tok + 9));
		if (key_len == 0 || key_len >= sizeof(key)) {
			jw_raw_text(&w, tok, (size_t)(key_end + 2 - tok));
			p = key_end + 2;
			continue;
		}
		memcpy(key, tok + 9, key_len);
		key[key_len] = '\0';

		jval = secrets != NULL ? json_object_get(secrets, key) : NULL;
		val = json_as_string(jval);
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
	if (g_chains[0].name[0] != '\0')
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
			enum pkg_error ierr =
			    (enum pkg_error)image_manifest_set(image, recipe.entries[i].package,
			                                        recipe.entries[i].mode,
			                                        recipe.entries[i].version);
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

	strncpy(g_chains[0].name, image, sizeof(g_chains[0].name) - 1);
	snprintf(g_chains[0].image, sizeof(g_chains[0].image), "%s", image);
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
	g_chains[0].name[0] = '\0';
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

	snprintf(image, sizeof(image), "%s", g_chains[0].name);

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
	g_chains[0].name[0] = '\0';
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
