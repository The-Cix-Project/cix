#include "pkg.h"
#include "linux_compat.h"
#include "logstore.h"
#include "namecheck.h"
#include "persist.h"
#include "pki.h"
#include "test_image_fixture.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* PKG_CURL_BIN now lives in pkg.h -- shared with main.c's own
 * bootstrap-fetch mechanism (ADR-0065), one real definition. */
#define PKG_TAR_BIN "/usr/bin/tar"
#define PKG_SHA256SUM_BIN "/usr/bin/sha256sum"
#define PKG_RM_BIN "/bin/rm"
#define PKG_UNSQUASHFS_BIN "/usr/bin/unsquashfs"

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

/* v1 serializes installs: at most one job in flight. Empty = idle. */
static char g_current_job_name[PKG_NAME_MAX];
/* The target image the in-flight job (name above, plus every
 * dependency it pulls in) merges into -- always normalized (never
 * empty; see normalize_image()), valid exactly when g_current_job_name
 * is non-empty. For a hostbuild job this is always PKG_HOSTBUILD_IMAGE
 * (where the resulting pkg_entry is filed, not where the build
 * container's own lowerdir comes from -- see g_current_job_build_image
 * below). */
static char g_current_job_image[PKG_IMAGE_NAME_MAX];
/* True exactly while the in-flight job is a hostbuild (ADR-0056) --
 * set explicitly at the start of every job (pkg_install_start() clears
 * it, pkg_hostbuild_start() sets it), never left stale from a prior
 * job, so it's safe to read any time g_current_job_name is non-empty. */
static int g_current_job_is_hostbuild;
/* Only meaningful while g_current_job_is_hostbuild is true: the real
 * image whose rootfs supplies the build container's own lowerdir
 * (e.g. "kanxeo-builder"), as opposed to the always-shared
 * g_pkgbuild_rootfs every ordinary install uses. */
static char g_current_job_build_image[PKG_IMAGE_NAME_MAX];

/* The resolved install order for the current job: dependencies first
 * (post-order), the originally-requested package last. g_dep_queue_pos
 * is the index currently fetching/building; g_dep_queue_is_upgrade is
 * true only when the LAST entry is an explicit upgrade of an already-
 * installed package (dependencies are only ever fresh-installed if
 * missing, never auto-upgraded as a side effect). */
static char g_dep_queue[PKG_MAX_DEP_CHAIN][PKG_NAME_MAX];
static int g_dep_queue_count;
static int g_dep_queue_pos;
static int g_dep_queue_is_upgrade;

/* Static storage for the pending build's container_spec inputs --
 * valid from pkg_fetch_completed() returning success through the
 * caller's immediately-following registry_create() call. Safe as
 * module-level statics given v1's one-job-at-a-time serialization. */
static char g_build_lowerdir[PATH_MAX], g_build_upperdir[PATH_MAX];
static char g_build_workdir[PATH_MAX], g_build_merged[PATH_MAX];
static char g_build_argv_cmd[512];
static char *g_build_argv[4];
static char *g_build_envp[4];
/*
 * The build container's own stdout/stderr, captured via a pipe (see
 * struct container_spec's capture_output field) since there is no
 * other way to see a recipe's real build-script output: the async
 * pipeline captures no logs of its own, and this project's minimal
 * install has no SSH/console access to read the daemon's own
 * inherited stdout on a real remote box (ADR-0034).
 *
 * g_build_output_rd is registered directly with the daemon's own
 * epoll loop by the caller (main.c, via pkg_build_output_fd()) right
 * after the build container is spawned, and drained incrementally as
 * data arrives (pkg_build_output_readable(), called on EPOLLIN) into
 * g_build_output_captured below -- NOT read in one shot after the
 * container exits. ADR-0087: a plain pipe's kernel buffer is 64KB;
 * any build whose combined stdout+stderr exceeds that would block
 * forever on its next write() if nothing drains the pipe while it's
 * still running, which is exactly what a single post-exit read did
 * before this fix (confirmed live: a real `perl` build reproducibly
 * deadlocked this way). g_build_output_wr is the caller's (main.c's)
 * own copy of the write end, returned via pkg_fetch_completed()'s
 * out-param so it can be closed right after the child inherits its
 * own duplicate. -1 when no build is in flight or the pipe2() call
 * itself failed (capture is a diagnostic nicety, never a reason to
 * fail the build).
 */
static int g_build_output_rd = -1;
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
/* Sliding-window tail buffer g_build_output_readable() incrementally
 * fills (see its own comment) -- pkg_build_completed() logs straight
 * from this on a build failure, replacing what used to be a single
 * blocking drain-to-EOF loop performed at that point instead. */
static char g_build_output_captured[PKG_BUILD_OUTPUT_CAPTURE_MAX + 1];
static int g_build_output_captured_len;

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

static void image_rootfs_path(const char *image, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s/rootfs", g_images_dir, normalize_image(image));
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

/* Unlinks every one of e's manifested files from e's own target image
 * -- does NOT touch e->files itself (caller decides when to forget the
 * list, e.g. only once a replacement build has actually succeeded for
 * an in-place upgrade). Shared by pkg_delete() and the upgrade path
 * in pkg_build_completed(). */
static void unlink_manifest_files(const struct pkg_entry *e)
{
	char rootfs[PATH_MAX];
	int i;

	image_rootfs_path(e->image, rootfs, sizeof(rootfs));
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
	/* depends is optional -- fine if absent */
	extract_line_value(buf, "pkg_depends=", out->depends, sizeof(out->depends));
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
	/* Drain and reap even if the buffer filled before EOF -- otherwise
	 * a large listing leaves tar blocked writing to a full pipe,
	 * leaking a zombie child. */
	while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
		;
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

		snprintf(recipe_path, sizeof(recipe_path), "%s/%s.recipe", g_recipes_dir, e->name);
		if (parse_recipe(recipe_path, &recipe) == 0 && strcmp(recipe.version, e->version) != 0) {
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
	g_current_job_name[0] = '\0';
	g_current_job_is_hostbuild = 0;
	g_dep_queue_count = 0;
	g_dep_queue_pos = 0;
	g_dep_queue_is_upgrade = 0;
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

enum pkg_error pkg_seed_image_baseline(const char *image)
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
	char target_rootfs[PATH_MAX];
	size_t i;

	image_rootfs_path(image, target_rootfs, sizeof(target_rootfs));

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

	return PKG_OK;
}

void pkg_write_json_recipes(struct json_writer *w)
{
	DIR *d;
	struct dirent *de;

	jw_arr_open(w);
	d = opendir(g_recipes_dir);
	if (d != NULL) {
		while ((de = readdir(d)) != NULL) {
			size_t len = strlen(de->d_name);
			char path[PATH_MAX];
			struct pkg_recipe r;

			if (len <= 7 || strcmp(de->d_name + len - 7, ".recipe") != 0)
				continue;
			snprintf(path, sizeof(path), "%s/%s", g_recipes_dir, de->d_name);
			if (parse_recipe(path, &r) != 0)
				continue;
			jw_obj_open(w);
			jw_key(w, "name");
			jw_str(w, r.name);
			jw_key(w, "version");
			jw_str(w, r.version);
			jw_key(w, "depends");
			jw_str(w, r.depends);
			jw_obj_close(w);
		}
		closedir(d);
	}
	jw_arr_close(w);
}

enum pkg_error pkg_recipe_get(const char *name, struct json_writer *w)
{
	char recipe_path[PATH_MAX];
	struct pkg_recipe r;
	char *content;
	size_t content_len;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;

	snprintf(recipe_path, sizeof(recipe_path), "%s/%s.recipe", g_recipes_dir, name);
	if (parse_recipe(recipe_path, &r) != 0 || strcmp(r.name, name) != 0)
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
	jw_key(w, "content");
	jw_str(w, content);
	jw_obj_close(w);
	free(content);
	return PKG_OK;
}

enum pkg_error pkg_recipe_add(const char *name, const char *content)
{
	char staging_path[PATH_MAX];
	char recipe_path[PATH_MAX];
	struct pkg_recipe parsed;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;

	if (persist_mkdir_p(g_recipes_dir) != 0)
		return PKG_ERR_PERSIST_FAILED;

	if (snprintf(staging_path, sizeof(staging_path), "%s/.%s.recipe.new", g_recipes_dir, name) >=
	    (int)sizeof(staging_path))
		return PKG_ERR_INVALID_NAME;
	if (persist_atomic_write(staging_path, content, strlen(content)) != 0)
		return PKG_ERR_PERSIST_FAILED;

	if (parse_recipe(staging_path, &parsed) != 0 || strcmp(parsed.name, name) != 0) {
		unlink(staging_path);
		return PKG_ERR_INVALID_RECIPE;
	}

	snprintf(recipe_path, sizeof(recipe_path), "%s/%s.recipe", g_recipes_dir, name);
	if (rename(staging_path, recipe_path) != 0) {
		unlink(staging_path);
		return PKG_ERR_PERSIST_FAILED;
	}
	return PKG_OK;
}

enum pkg_error pkg_recipe_delete(const char *name)
{
	char recipe_path[PATH_MAX];
	struct stat st;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;

	snprintf(recipe_path, sizeof(recipe_path), "%s/%s.recipe", g_recipes_dir, name);
	if (stat(recipe_path, &st) != 0)
		return PKG_ERR_NOT_FOUND;
	if (unlink(recipe_path) != 0)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
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
 */
static int resolve_chain(const char *name, int force, char queue[][PKG_NAME_MAX], int *count,
                          char visiting[][PKG_NAME_MAX], int *visiting_count, char *err_out,
                          size_t err_out_size)
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

	existing = pkg_find(name, g_current_job_image);
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

	snprintf(recipe_path, sizeof(recipe_path), "%s/%s.recipe", g_recipes_dir, name);
	if (parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0) {
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
		if (resolve_chain(tok, 0, queue, count, visiting, visiting_count, err_out, err_out_size) !=
		    0)
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
	pid_t pid;
	int pidfd;

	snprintf(recipe_path, sizeof(recipe_path), "%s/%s.recipe", g_recipes_dir, name);
	if (parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0)
		return PKG_ERR_INVALID_RECIPE;

	e = pkg_find(name, g_current_job_image);
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
		strncpy(e->image, g_current_job_image, sizeof(e->image) - 1);
	}
	e->state = PKG_STATE_FETCHING;
	e->error[0] = '\0';

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

	strncpy(g_current_job_name, name, sizeof(g_current_job_name) - 1);
	*out_pid = pid;
	*out_pidfd = pidfd;
	return PKG_OK;
}

enum pkg_error pkg_install_start(const char *name, const char *image, int upgrade,
                                  char *out_started_name, size_t out_started_name_size,
                                  pid_t *out_pid, int *out_pidfd)
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
	if (g_current_job_name[0] != '\0')
		return PKG_ERR_BUSY;

	snprintf(recipe_path, sizeof(recipe_path), "%s/%s.recipe", g_recipes_dir, name);
	if (parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0)
		return PKG_ERR_INVALID_RECIPE;

	snprintf(g_current_job_image, sizeof(g_current_job_image), "%s", normalize_image(image));
	g_current_job_is_hostbuild = 0;

	e = pkg_find(name, g_current_job_image);
	if (e != NULL && e->state == PKG_STATE_INSTALLED &&
	    (!upgrade || strcmp(e->version, recipe.version) == 0))
		return PKG_ERR_DUPLICATE;

	/* Resolve the full install order: name's own dependencies first
	 * (skipped if already satisfied), then name itself -- force=1 so
	 * an explicit upgrade target is queued even though it's already
	 * installed (resolve_chain()'s default "already installed, skip"
	 * rule is for pure dependencies, not the package actually asked for). */
	g_dep_queue_count = 0;
	if (resolve_chain(name, 1, g_dep_queue, &g_dep_queue_count, visiting, &visiting_count, err,
	                   sizeof(err)) != 0)
		return PKG_ERR_INVALID_RECIPE;

	g_dep_queue_pos = 0;
	g_dep_queue_is_upgrade = (e != NULL && e->state == PKG_STATE_INSTALLED);

	perr = start_fetch_for(g_dep_queue[0], out_pid, out_pidfd);
	if (perr != PKG_OK) {
		g_dep_queue_count = 0;
		return perr;
	}
	snprintf(out_started_name, out_started_name_size, "%s", g_dep_queue[0]);
	return PKG_OK;
}

enum pkg_error pkg_hostbuild_start(const char *name, const char *build_image, int upgrade,
                                    pid_t *out_pid, int *out_pidfd)
{
	struct pkg_recipe recipe;
	char recipe_path[PATH_MAX];
	struct pkg_entry *e;
	char build_rootfs[PATH_MAX];
	struct stat st;
	enum pkg_error perr;

	if (!pkg_name_is_valid(name))
		return PKG_ERR_INVALID_NAME;
	if (build_image == NULL || build_image[0] == '\0' || !pkg_image_is_valid(build_image))
		return PKG_ERR_INVALID_NAME;
	if (g_current_job_name[0] != '\0')
		return PKG_ERR_BUSY;

	snprintf(recipe_path, sizeof(recipe_path), "%s/%s.recipe", g_recipes_dir, name);
	if (parse_recipe(recipe_path, &recipe) != 0 || strcmp(recipe.name, name) != 0)
		return PKG_ERR_INVALID_RECIPE;
	/* Dependency resolution targets "merge into an image" -- meaningless
	 * for a one-shot artifact harvest. Every prerequisite must already
	 * be baked into build_image's own rootfs (built up via ordinary
	 * `pkg install` first). */
	if (recipe.depends[0] != '\0')
		return PKG_ERR_INVALID_RECIPE;

	/* build_image must already exist -- there is no sane default the
	 * way PKG_DEFAULT_IMAGE is for an ordinary install. */
	image_rootfs_path(build_image, build_rootfs, sizeof(build_rootfs));
	if (stat(build_rootfs, &st) != 0 || !S_ISDIR(st.st_mode))
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

	snprintf(g_current_job_image, sizeof(g_current_job_image), "%s", PKG_HOSTBUILD_IMAGE);
	snprintf(g_current_job_build_image, sizeof(g_current_job_build_image), "%s", build_image);
	g_current_job_is_hostbuild = 1;

	/* A hostbuild job is always a single, standalone entry -- no
	 * resolve_chain(), pkg_depends is required empty above. */
	g_dep_queue_count = 0;
	strncpy(g_dep_queue[0], name, PKG_NAME_MAX - 1);
	g_dep_queue[0][PKG_NAME_MAX - 1] = '\0';
	g_dep_queue_count = 1;
	g_dep_queue_pos = 0;
	g_dep_queue_is_upgrade = 0;

	perr = start_fetch_for(name, out_pid, out_pidfd);
	if (perr != PKG_OK) {
		g_dep_queue_count = 0;
		g_current_job_is_hostbuild = 0;
		return perr;
	}
	return PKG_OK;
}

int pkg_fetch_completed(int exit_status, struct container_spec *spec_out, int *out_stdio_write_fd)
{
	struct pkg_entry *e = pkg_find(g_current_job_name, g_current_job_image);
	char recipe_path[PATH_MAX];
	struct pkg_recipe recipe;
	char sha_out[128];
	char container_base[PATH_MAX];
	char src_dir[PATH_MAX], dest_dir[PATH_MAX], recipe_dst[PATH_MAX], extra_dir[PATH_MAX];
	int is_final_upgrade;
	int i;

	if (e == NULL) {
		g_current_job_name[0] = '\0';
		g_dep_queue_count = 0;
		return 0;
	}
	is_final_upgrade = g_dep_queue_is_upgrade && (g_dep_queue_pos + 1 >= g_dep_queue_count);

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
		g_current_job_name[0] = '\0';
		g_dep_queue_count = 0;
		return 0;
	}

	snprintf(recipe_path, sizeof(recipe_path), "%s/%s.recipe", g_recipes_dir, e->name);
	if (parse_recipe(recipe_path, &recipe) != 0) {
		e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "recipe became unreadable mid-install");
		g_current_job_name[0] = '\0';
		g_dep_queue_count = 0;
		return 0;
	}

	/*
	 * recipe.version, not e->version -- during an in-place upgrade
	 * e->version is deliberately still the OLD version until this job
	 * actually succeeds; each source on disk was fetched under the
	 * NEW version's name by start_fetch_for(). Every fetched source
	 * (the main one at index 0, and any extras) is verified against
	 * its own sha256 before any of them are touched further -- one
	 * bad entry fails the whole job, matching the single-source
	 * case's own existing all-or-nothing guarantee (ADR-0036).
	 */
	for (i = 0; i < recipe.source_count; i++) {
		char src_path[PATH_MAX];

		snprintf(src_path, sizeof(src_path), "%s/%s-%s-%d.src", g_sources_dir, e->name,
		         recipe.version, i);
		if (pkg_run_capture_sha256(src_path, sha_out, sizeof(sha_out)) != 0 ||
		    strcasecmp(sha_out, recipe.sha256[i]) != 0) {
			e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
			snprintf(e->error, sizeof(e->error), "checksum mismatch (source %d)", i);
			g_current_job_name[0] = '\0';
			g_dep_queue_count = 0;
			return 0;
		}
	}

	snprintf(container_base, sizeof(container_base), "%s/%s", g_containers_dir,
	         PKG_BUILD_CONTAINER_NAME);
	/* A hostbuild job's own build container is rooted on build_image's
	 * rootfs (built up via ordinary `pkg install` beforehand), never
	 * the shared toolchain sandbox every regular install uses
	 * (ADR-0056). */
	if (g_current_job_is_hostbuild)
		image_rootfs_path(g_current_job_build_image, g_build_lowerdir, sizeof(g_build_lowerdir));
	else
		snprintf(g_build_lowerdir, sizeof(g_build_lowerdir), "%s", g_pkgbuild_rootfs);
	snprintf(g_build_upperdir, sizeof(g_build_upperdir), "%s/upper", container_base);
	snprintf(g_build_workdir, sizeof(g_build_workdir), "%s/work", container_base);
	snprintf(g_build_merged, sizeof(g_build_merged), "%s/merged", container_base);

	snprintf(src_dir, sizeof(src_dir), "%s/build/src", g_build_upperdir);
	snprintf(dest_dir, sizeof(dest_dir), "%s/build/pkg-dest", g_build_upperdir);
	snprintf(recipe_dst, sizeof(recipe_dst), "%s/build/recipe.sh", g_build_upperdir);
	snprintf(extra_dir, sizeof(extra_dir), "%s/build/extra", g_build_upperdir);

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
		snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", g_build_upperdir);

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
			} else if (persist_mkdir_p(src_dir) != 0) {
				prep_step = "create src dir";
				prep_errno = errno;
			} else if (persist_mkdir_p(dest_dir) != 0) {
				prep_step = "create dest dir";
				prep_errno = errno;
			} else if (persist_mkdir_p(tmp_dir) != 0) {
				prep_step = "create tmp dir";
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
					                e->name, g_current_job_image, prep_step,
					                strerror(prep_errno));
				else
					logstore_write("kanxeod", "error",
					                "pkg %s@%s: could not prepare build container (%s) -- see run_subprocess detail above",
					                e->name, g_current_job_image, prep_step);
				e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
				snprintf(e->error, sizeof(e->error),
				         "could not prepare the build container (%s failed)", prep_step);
				g_current_job_name[0] = '\0';
				g_dep_queue_count = 0;
				return 0;
			}
		}
	}

	/* Sources beyond index 0 are plain files, never extracted -- copied
	 * verbatim into /build/extra/<basename-of-their-own-URL> for
	 * pkg_build()/pkg_install() to reference directly (ADR-0036).
	 * Basename collisions across multiple extra URLs are a stated
	 * recipe-author responsibility, not auto-resolved here. */
	if (recipe.source_count > 1) {
		if (persist_mkdir_p(extra_dir) != 0) {
			logstore_write("kanxeod", "error",
			                "pkg %s@%s: could not prepare build container (create extra dir): %s",
			                e->name, g_current_job_image, strerror(errno));
			e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
			snprintf(e->error, sizeof(e->error), "could not prepare the build container (create extra dir failed)");
			g_current_job_name[0] = '\0';
			g_dep_queue_count = 0;
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
				                e->name, g_current_job_image, i, strerror(errno));
				e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
				snprintf(e->error, sizeof(e->error),
				         "could not prepare the build container (copy extra source %d failed)", i);
				g_current_job_name[0] = '\0';
				g_dep_queue_count = 0;
				return 0;
			}
		}
	}

	snprintf(g_build_argv_cmd, sizeof(g_build_argv_cmd),
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
	g_build_argv[0] = "/usr/bin/bash";
	g_build_argv[1] = "-c";
	g_build_argv[2] = g_build_argv_cmd;
	g_build_argv[3] = NULL;
	g_build_envp[0] = "PKG_DESTDIR=/build/pkg-dest";
	g_build_envp[1] = "PATH=/usr/bin:/bin";
	g_build_envp[2] = "HOME=/build";
	g_build_envp[3] = NULL;

	memset(spec_out, 0, sizeof(*spec_out));
	spec_out->ns.clone_flags =
	    CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWCGROUP | CLONE_INTO_CGROUP;
	spec_out->ns.hostname = PKG_BUILD_CONTAINER_NAME;
	spec_out->cg.name = PKG_BUILD_CONTAINER_NAME;
	spec_out->ov.lowerdir = g_build_lowerdir;
	spec_out->ov.upperdir = g_build_upperdir;
	spec_out->ov.workdir = g_build_workdir;
	spec_out->ov.merged = g_build_merged;
	spec_out->mnt.put_old_rel = ".old_root";
	spec_out->argv = g_build_argv;
	spec_out->envp = g_build_envp;

	g_build_output_captured_len = 0;
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
				g_build_output_rd = output_pipe[0];
				*out_stdio_write_fd = output_pipe[1];
			} else {
				close(output_pipe[0]);
				close(output_pipe[1]);
				g_build_output_rd = -1;
				*out_stdio_write_fd = -1;
			}
		} else {
			g_build_output_rd = -1;
			*out_stdio_write_fd = -1;
		}
	}

	e->state = PKG_STATE_BUILDING;
	return 1;
}

void pkg_build_spawn_failed(void)
{
	struct pkg_entry *e = pkg_find(g_current_job_name, g_current_job_image);

	if (e != NULL) {
		int is_final_upgrade = g_dep_queue_is_upgrade && (g_dep_queue_pos + 1 >= g_dep_queue_count);

		e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "could not start the build container");
	}
	g_current_job_name[0] = '\0';
	g_dep_queue_count = 0;
	/*
	 * registry_create() never spawned a container, so no epoll
	 * registration for g_build_output_rd exists anywhere to drive
	 * its own EOF-triggered close (see register_pkg_build_output()/
	 * handle_pkg_build_output_event() in main.c) -- this is the one
	 * path that has to close it directly.
	 */
	pkg_build_output_close();
}

int pkg_build_output_fd(void)
{
	return g_build_output_rd;
}

static void pkg_build_output_append(const char *data, int len)
{
	int take = (len > PKG_BUILD_OUTPUT_CAPTURE_MAX) ? PKG_BUILD_OUTPUT_CAPTURE_MAX : len;
	int new_total = g_build_output_captured_len + take;

	if (new_total > PKG_BUILD_OUTPUT_CAPTURE_MAX) {
		int overflow = new_total - PKG_BUILD_OUTPUT_CAPTURE_MAX;

		memmove(g_build_output_captured, g_build_output_captured + overflow,
		        g_build_output_captured_len - overflow);
		g_build_output_captured_len -= overflow;
	}
	memcpy(g_build_output_captured + g_build_output_captured_len, data + (len - take), take);
	g_build_output_captured_len += take;
}

int pkg_build_output_readable(void)
{
	char chunk[4096];
	ssize_t n;

	if (g_build_output_rd < 0)
		return 1;

	for (;;) {
		n = read(g_build_output_rd, chunk, sizeof(chunk));
		if (n > 0) {
			pkg_build_output_append(chunk, (int)n);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0; /* drained everything available right now */
		return 1;         /* EOF (n == 0) or a real read error */
	}
}

void pkg_build_output_close(void)
{
	if (g_build_output_rd >= 0) {
		close(g_build_output_rd);
		g_build_output_rd = -1;
	}
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

	e = pkg_find(g_current_job_name, g_current_job_image);
	if (e == NULL) {
		g_current_job_name[0] = '\0';
		g_dep_queue_count = 0;
		return 0;
	}

	is_final = (g_dep_queue_pos + 1 >= g_dep_queue_count);
	is_upgrade = is_final && g_dep_queue_is_upgrade;

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
		 * g_build_output_captured has already been incrementally
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

		pkg_build_output_readable();
		captured_len = g_build_output_captured_len;
		memcpy(captured, g_build_output_captured, captured_len);
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
			                e->name, g_current_job_image, step);
			snprintf(e->error, sizeof(e->error), "build container setup failed (%s)", step);
		} else if (exit_status >= 130 && exit_status <= 136) {
			const char *step = overlay_step_names[exit_status - 130];

			logstore_write("kanxeod", "error", "pkg %s@%s: build container setup failed (%s)",
			                e->name, g_current_job_image, step);
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
			                g_current_job_image, strerror(real_errno));
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
			                e->name, g_current_job_image);
			snprintf(e->error, sizeof(e->error), "build failed (exit 127 -- see build output in logs)");
		} else {
			snprintf(e->error, sizeof(e->error), "build failed (exit status %d)", exit_status);
		}
		if (captured_len > 0)
			logstore_write("kanxeod", "error", "pkg %s@%s: build output: %s", e->name,
			                g_current_job_image, captured);
		e->state = is_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		g_current_job_name[0] = '\0';
		g_dep_queue_count = 0; /* abort the rest of the chain -- a failed dependency
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
	g_build_output_captured_len = 0;

	snprintf(container_base, sizeof(container_base), "%s/%s", g_containers_dir,
	         PKG_BUILD_CONTAINER_NAME);
	snprintf(dest_dir, sizeof(dest_dir), "%s/upper/build/pkg-dest", container_base);

	if (is_upgrade) {
		/* Unlink the OLD manifest's files first -- a version that
		 * renamed/dropped files shouldn't leave the old ones behind.
		 * Only after this do we lose track of the old file list. */
		unlink_manifest_files(e);
		pkg_entry_free_files(e);
	}

	/* Refresh version from the recipe -- for a fresh install this is
	 * the first time it's set; for an upgrade this is where the entry
	 * finally moves from the old version to the new one. */
	{
		char recipe_path[PATH_MAX];
		struct pkg_recipe recipe;

		snprintf(recipe_path, sizeof(recipe_path), "%s/%s.recipe", g_recipes_dir, e->name);
		if (parse_recipe(recipe_path, &recipe) == 0) {
			strncpy(e->version, recipe.version, sizeof(e->version) - 1);
			strncpy(e->depends, recipe.depends, sizeof(e->depends) - 1);
		}
	}

	if (g_current_job_is_hostbuild) {
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
			g_current_job_name[0] = '\0';
			g_dep_queue_count = 0;
			g_current_job_is_hostbuild = 0;
			return 0;
		}
	} else {
		char target_rootfs[PATH_MAX];

		image_rootfs_path(g_current_job_image, target_rootfs, sizeof(target_rootfs));
		if (persist_mkdir_p(target_rootfs) != 0 ||
		    pkg_seed_image_baseline(g_current_job_image) != PKG_OK ||
		    merge_tree(dest_dir, target_rootfs, "", e) != 0) {
			e->state = PKG_STATE_FAILED;
			snprintf(e->error, sizeof(e->error),
			         "failed to merge installed files into the target image");
			g_current_job_name[0] = '\0';
			g_dep_queue_count = 0;
			return 0;
		}

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

	e->state = PKG_STATE_INSTALLED;
	e->error[0] = '\0';
	save_state();

	if (g_current_job_is_hostbuild)
		snprintf(out_hostbuild_done_name, PKG_NAME_MAX, "%s", e->name);

	if (!is_final) {
		enum pkg_error perr;

		g_dep_queue_pos++;
		perr = start_fetch_for(g_dep_queue[g_dep_queue_pos], out_pid, out_pidfd);
		if (perr == PKG_OK)
			return 1;
		/* couldn't start the next dependency -- abort the chain */
	}

	g_current_job_name[0] = '\0';
	g_dep_queue_count = 0;
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
		snprintf(recipe_path, sizeof(recipe_path), "%s/%s.recipe", g_recipes_dir, e->name);
		if (parse_recipe(recipe_path, &recipe) == 0 && strcmp(recipe.version, e->version) != 0) {
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

enum pkg_error pkg_delete(const char *name, const char *image)
{
	struct pkg_entry *e = pkg_find(name, image);

	if (e == NULL || e->state != PKG_STATE_INSTALLED)
		return PKG_ERR_NOT_FOUND;
	if (strcmp(g_current_job_name, name) == 0 &&
	    strcmp(g_current_job_image, normalize_image(image)) == 0)
		return PKG_ERR_BUSY;

	unlink_manifest_files(e);
	pkg_entry_free_files(e);
	memset(e, 0, sizeof(*e));

	if (save_state() != 0)
		return PKG_ERR_PERSIST_FAILED;
	return PKG_OK;
}
