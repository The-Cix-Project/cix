#include "pkg.h"
#include "linux_compat.h"
#include "namecheck.h"
#include "persist.h"

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

#define PKG_CURL_BIN "/usr/bin/curl"
#define PKG_TAR_BIN "/usr/bin/tar"
#define PKG_SHA256SUM_BIN "/usr/bin/sha256sum"
#define PKG_CP_BIN "/usr/bin/cp"
#define PKG_RM_BIN "/bin/rm"

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
	char source[PKG_URL_MAX];
	char sha256[PKG_SHA256_MAX];
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

/* v1 serializes installs: at most one job in flight. Empty = idle. */
static char g_current_job_name[PKG_NAME_MAX];
/* The target image the in-flight job (name above, plus every
 * dependency it pulls in) merges into -- always normalized (never
 * empty; see normalize_image()), valid exactly when g_current_job_name
 * is non-empty. */
static char g_current_job_image[PKG_IMAGE_NAME_MAX];

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
static char *g_build_envp[3];

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

static int parse_recipe(const char *path, struct pkg_recipe *out)
{
	char *buf;
	size_t len;
	int rc = 0;

	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return -1;

	memset(out, 0, sizeof(*out));
	if (extract_line_value(buf, "pkg_name=", out->name, sizeof(out->name)) != 0)
		rc = -1;
	if (extract_line_value(buf, "pkg_version=", out->version, sizeof(out->version)) != 0)
		rc = -1;
	if (extract_line_value(buf, "pkg_source=", out->source, sizeof(out->source)) != 0)
		rc = -1;
	if (extract_line_value(buf, "pkg_sha256=", out->sha256, sizeof(out->sha256)) != 0)
		rc = -1;
	/* depends is optional -- fine if absent */
	extract_line_value(buf, "pkg_depends=", out->depends, sizeof(out->depends));
	free(buf);

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
		return -1;
	}
	if (pid == 0) {
		execve(bin, argv, environ);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int extract_tarball(const char *tarball_path, const char *dest_dir)
{
	char *argv[] = { (char *)PKG_TAR_BIN, "-C",           (char *)dest_dir,
		          "--strip-components=1", "-xf", (char *)tarball_path, NULL };

	return run_subprocess(PKG_TAR_BIN, argv);
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

static int run_capture_sha256(const char *path, char *out, size_t out_size)
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
 * recording every regular file copied into e's manifest. Symlinks are
 * skipped (a documented, narrow v1 boundary -- most real `make
 * install DESTDIR=` output is regular files/directories; a package
 * that installs symlinks needs a later part, not attempted-and-wrong
 * here). */
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
              const char *images_dir)
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

	memset(g_packages, 0, sizeof(g_packages));
	g_current_job_name[0] = '\0';
	g_dep_queue_count = 0;
	g_dep_queue_pos = 0;
	g_dep_queue_is_upgrade = 0;
	return load_state();
}

/*
 * Containers get no /dev at all beyond whatever the shared lowerdir
 * image itself contains (confirmed by test/test_dns.c's own
 * ensure_dev_node() precedent -- no devtmpfs exists yet). A real
 * build's own ./configure script routinely redirects to /dev/null
 * while probing the compiler -- confirmed directly: bash's configure
 * failed outright with "cannot create /dev/null: Directory
 * nonexistent" before this existed, a generic blocker for any
 * package's build, not specific to one recipe. The standard
 * major:minor quintet below (all major 1, the kernel's own "mem"
 * driver) covers what a non-interactive build/install actually
 * touches; /dev/tty is deliberately omitted -- nothing in a batch
 * "./configure && make && make install" sequence needs a controlling
 * terminal.
 */
static int ensure_std_dev_nodes(const char *rootfs)
{
	static const struct {
		const char *name;
		unsigned int major, minor;
	} nodes[] = {
		{ "null", 1, 3 }, { "zero", 1, 5 }, { "full", 1, 7 }, { "random", 1, 8 },
		{ "urandom", 1, 9 },
	};
	char dev_dir[PATH_MAX];
	size_t i;

	snprintf(dev_dir, sizeof(dev_dir), "%s/dev", rootfs);
	if (persist_mkdir_p(dev_dir) != 0)
		return -1;

	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s", dev_dir, nodes[i].name);
		if (mknod(path, S_IFCHR | 0666, makedev(nodes[i].major, nodes[i].minor)) != 0 &&
		    errno != EEXIST)
			return -1;
	}
	return 0;
}

enum pkg_error pkg_bootstrap_build_image(void)
{
	static const char *const subdirs[] = { "include", "lib", "lib64", "bin", "libexec" };
	static const struct {
		const char *link;
		const char *target;
	} compat[] = {
		{ "bin", "usr/bin" }, { "lib", "usr/lib" }, { "lib64", "usr/lib64" }, { "sbin", "usr/bin" },
	};
	char usr_dst[PATH_MAX];
	size_t i;

	if (persist_mkdir_p(g_pkgbuild_rootfs) != 0)
		return PKG_ERR_PERSIST_FAILED;
	snprintf(usr_dst, sizeof(usr_dst), "%s/usr", g_pkgbuild_rootfs);
	if (persist_mkdir_p(usr_dst) != 0)
		return PKG_ERR_PERSIST_FAILED;

	for (i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
		char src[PATH_MAX], dst[PATH_MAX];
		struct stat src_st, dst_st;

		snprintf(src, sizeof(src), "/usr/%s", subdirs[i]);
		snprintf(dst, sizeof(dst), "%s/usr/%s", g_pkgbuild_rootfs, subdirs[i]);

		if (stat(src, &src_st) != 0)
			continue; /* not present on this host -- skip, not fatal */
		if (stat(dst, &dst_st) == 0)
			continue; /* already staged -- idempotent */

		{
			char *argv[] = { (char *)PKG_CP_BIN, "-a", src, dst, NULL };

			if (run_subprocess(PKG_CP_BIN, argv) != 0)
				return PKG_ERR_SPAWN_FAILED;
		}
	}

	for (i = 0; i < sizeof(compat) / sizeof(compat[0]); i++) {
		char linkpath[PATH_MAX];

		snprintf(linkpath, sizeof(linkpath), "%s/%s", g_pkgbuild_rootfs, compat[i].link);
		symlink(compat[i].target, linkpath); /* EEXIST tolerated -- idempotent */
	}

	/*
	 * Targeted extras beyond the wholesale /usr/{include,lib,lib64,bin,
	 * libexec} copy above, each found by an actual build failing, not
	 * guessed at -- staging the *entirety* of e.g. /usr/share (584M on
	 * this host, almost none of it relevant to building software) just
	 * to reach one tool's own data files would bloat this image for no
	 * real benefit. Extend this list as a real build surfaces a real
	 * need for one, the same "verify empirically, don't speculate"
	 * discipline this project applies everywhere else:
	 *
	 * - /etc/alternatives: Debian's "alternatives" system points many
	 *   /usr/bin/* tools (awk, and others a real build can reach for)
	 *   at an *absolute* /etc/alternatives/<name> symlink -- cp -a on
	 *   /usr/bin above preserves that symlink exactly as-is, target
	 *   string included, so it still reads /etc/alternatives/<name>
	 *   once inside the container. Confirmed directly: bash's own
	 *   config.status failed outright ("awk: command not found")
	 *   because /etc was never staged at all.
	 * - /usr/share/bison: bison itself needs its own bundled M4 macro
	 *   library (m4sugar.m4 and friends) at *run* time, not just build
	 *   time, to generate a parser from any .y grammar -- confirmed
	 *   directly building iproute2's tc (its ematch grammar needs
	 *   bison): "bison: .../m4sugar.m4: cannot open: No such file or
	 *   directory".
	 */
	{
		static const struct {
			const char *src;
			const char *rel_dst;
		} extras[] = {
			{ "/etc/alternatives", "etc/alternatives" },
			{ "/usr/share/bison", "usr/share/bison" },
			{ "/usr/share/autoconf", "usr/share/autoconf" },
			{ "/usr/share/perl", "usr/share/perl" },
		};

		for (i = 0; i < sizeof(extras) / sizeof(extras[0]); i++) {
			char dst[PATH_MAX], dst_parent[PATH_MAX], *slash;
			struct stat st;

			snprintf(dst, sizeof(dst), "%s/%s", g_pkgbuild_rootfs, extras[i].rel_dst);
			if (stat(extras[i].src, &st) != 0 || stat(dst, &st) == 0)
				continue; /* not present on this host, or already staged */

			snprintf(dst_parent, sizeof(dst_parent), "%s", dst);
			slash = strrchr(dst_parent, '/');
			if (slash != NULL)
				*slash = '\0';

			{
				char *argv[] = { (char *)PKG_CP_BIN, "-a", (char *)extras[i].src, dst,
					          NULL };

				if (persist_mkdir_p(dst_parent) != 0 || run_subprocess(PKG_CP_BIN, argv) != 0)
					return PKG_ERR_SPAWN_FAILED;
			}
		}
	}

	if (ensure_std_dev_nodes(g_pkgbuild_rootfs) != 0)
		return PKG_ERR_PERSIST_FAILED;

	/*
	 * Same "the pkgbuild image only ever staged /usr, nothing at the
	 * real root" gap /dev above already fixed -- confirmed directly
	 * (autoconf's own config.guess failed outright: "cannot create a
	 * temporary directory in /tmp"). Real build systems' own temp-file
	 * handling (config.guess, mktemp, plenty of Makefiles) assumes a
	 * writable, sticky-bit /tmp exists, the same standard FHS baseline
	 * assumption /dev/null already covers.
	 */
	{
		char tmp_dir[PATH_MAX];

		snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", g_pkgbuild_rootfs);
		if (persist_mkdir_p(tmp_dir) != 0)
			return PKG_ERR_PERSIST_FAILED;
		if (chmod(tmp_dir, 01777) != 0)
			return PKG_ERR_PERSIST_FAILED;
	}

	return PKG_OK;
}

enum pkg_error pkg_seed_image_runtime(const char *image)
{
	static const struct {
		const char *src;
		const char *rel_dst;
	} runtime_libs[] = {
		{ "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", "lib64/ld-linux-x86-64.so.2" },
		{ "/usr/lib/x86_64-linux-gnu/libc.so.6", "lib/x86_64-linux-gnu/libc.so.6" },
		{ "/usr/lib/x86_64-linux-gnu/libtinfo.so.6", "lib/x86_64-linux-gnu/libtinfo.so.6" },
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
	snprintf(tarball_path, sizeof(tarball_path), "%s/%s-%s.tarball", g_sources_dir, recipe.name,
	         recipe.version);

	pid = fork();
	if (pid < 0) {
		perror("fork");
		e->state = is_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "fork failed");
		return PKG_ERR_SPAWN_FAILED;
	}
	if (pid == 0) {
		char *argv[] = { (char *)PKG_CURL_BIN, "-fsSL", "-o", tarball_path, recipe.source, NULL };

		execve(PKG_CURL_BIN, argv, environ);
		_exit(127);
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

int pkg_fetch_completed(int exit_status, struct container_spec *spec_out)
{
	struct pkg_entry *e = pkg_find(g_current_job_name, g_current_job_image);
	char recipe_path[PATH_MAX];
	struct pkg_recipe recipe;
	char tarball_path[PATH_MAX];
	char sha_out[128];
	char container_base[PATH_MAX];
	char src_dir[PATH_MAX], dest_dir[PATH_MAX], recipe_dst[PATH_MAX];
	int is_final_upgrade;

	if (e == NULL) {
		g_current_job_name[0] = '\0';
		g_dep_queue_count = 0;
		return 0;
	}
	is_final_upgrade = g_dep_queue_is_upgrade && (g_dep_queue_pos + 1 >= g_dep_queue_count);

	if (exit_status != 0) {
		e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "fetch failed (curl exit status %d)", exit_status);
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
	/* recipe.version, not e->version -- during an in-place upgrade
	 * e->version is deliberately still the OLD version until this job
	 * actually succeeds; the tarball on disk was fetched under the
	 * NEW version's name by start_fetch_for(). */
	snprintf(tarball_path, sizeof(tarball_path), "%s/%s-%s.tarball", g_sources_dir, e->name,
	         recipe.version);

	if (run_capture_sha256(tarball_path, sha_out, sizeof(sha_out)) != 0 ||
	    strcasecmp(sha_out, recipe.sha256) != 0) {
		e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "checksum mismatch");
		g_current_job_name[0] = '\0';
		g_dep_queue_count = 0;
		return 0;
	}

	snprintf(container_base, sizeof(container_base), "%s/%s", g_containers_dir,
	         PKG_BUILD_CONTAINER_NAME);
	snprintf(g_build_lowerdir, sizeof(g_build_lowerdir), "%s", g_pkgbuild_rootfs);
	snprintf(g_build_upperdir, sizeof(g_build_upperdir), "%s/upper", container_base);
	snprintf(g_build_workdir, sizeof(g_build_workdir), "%s/work", container_base);
	snprintf(g_build_merged, sizeof(g_build_merged), "%s/merged", container_base);

	snprintf(src_dir, sizeof(src_dir), "%s/build/src", g_build_upperdir);
	snprintf(dest_dir, sizeof(dest_dir), "%s/build/pkg-dest", g_build_upperdir);
	snprintf(recipe_dst, sizeof(recipe_dst), "%s/build/recipe.sh", g_build_upperdir);

	if (reset_build_container_dir(container_base) != 0 || persist_mkdir_p(src_dir) != 0 ||
	    persist_mkdir_p(dest_dir) != 0 || copy_file_simple(recipe_path, recipe_dst) != 0 ||
	    extract_tarball(tarball_path, src_dir) != 0) {
		e->state = is_final_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "could not prepare the build container");
		g_current_job_name[0] = '\0';
		g_dep_queue_count = 0;
		return 0;
	}

	snprintf(g_build_argv_cmd, sizeof(g_build_argv_cmd),
	         ". /build/recipe.sh; cd /build/src && pkg_build && pkg_install");
	g_build_argv[0] = "/bin/sh";
	g_build_argv[1] = "-c";
	g_build_argv[2] = g_build_argv_cmd;
	g_build_argv[3] = NULL;
	g_build_envp[0] = "PKG_DESTDIR=/build/pkg-dest";
	g_build_envp[1] = "PATH=/usr/bin:/bin";
	g_build_envp[2] = NULL;

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
}

int pkg_build_completed(const char *container_name, int exit_status, pid_t *out_pid,
                         int *out_pidfd)
{
	struct pkg_entry *e;
	char container_base[PATH_MAX];
	char dest_dir[PATH_MAX];
	int is_final, is_upgrade;

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
		e->state = is_upgrade ? PKG_STATE_INSTALLED : PKG_STATE_FAILED;
		snprintf(e->error, sizeof(e->error), "build failed (exit status %d)", exit_status);
		g_current_job_name[0] = '\0';
		g_dep_queue_count = 0; /* abort the rest of the chain -- a failed dependency
		                        * means the top-level install can't complete either */
		return 0;
	}

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

	{
		char target_rootfs[PATH_MAX];

		image_rootfs_path(g_current_job_image, target_rootfs, sizeof(target_rootfs));
		if (persist_mkdir_p(target_rootfs) != 0 ||
		    pkg_seed_image_runtime(g_current_job_image) != PKG_OK ||
		    merge_tree(dest_dir, target_rootfs, "", e) != 0) {
			e->state = PKG_STATE_FAILED;
			snprintf(e->error, sizeof(e->error),
			         "failed to merge installed files into the target image");
			g_current_job_name[0] = '\0';
			g_dep_queue_count = 0;
			return 0;
		}
	}

	e->state = PKG_STATE_INSTALLED;
	e->error[0] = '\0';
	save_state();

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
