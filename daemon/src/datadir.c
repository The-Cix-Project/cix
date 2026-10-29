#include "datadir.h"
#include "treecopy.h"

#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/*
 * Mount targets strictly beneath the data directory, read once per
 * report from /proc/self/mounts. A walk skips every subtree whose root
 * is one of them: those bytes are on another filesystem.
 */
static char **g_mounts;
static size_t g_mount_count;

/* /proc/self/mounts escapes space, tab, newline and backslash as \ooo. */
static void unescape_mount_path(char *s)
{
	char *r = s, *w = s;

	while (*r != '\0') {
		if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] >= '0' && r[2] <= '7' &&
		    r[3] >= '0' && r[3] <= '7') {
			*w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
			r += 4;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

static void mounts_free(void)
{
	size_t i;

	for (i = 0; i < g_mount_count; i++)
		free(g_mounts[i]);
	free(g_mounts);
	g_mounts = NULL;
	g_mount_count = 0;
}

static void mounts_load(const char *base_dir)
{
	FILE *f = fopen("/proc/self/mounts", "r");
	char line[PATH_MAX * 2];
	size_t base_len = strlen(base_dir);
	size_t cap = 0;

	mounts_free();
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *target, *sp = strchr(line, ' ');
		char **grown;

		if (sp == NULL)
			continue;
		target = sp + 1;
		sp = strchr(target, ' ');
		if (sp == NULL)
			continue;
		*sp = '\0';
		unescape_mount_path(target);
		if (strncmp(target, base_dir, base_len) != 0 || target[base_len] != '/')
			continue;
		if (g_mount_count == cap) {
			cap = cap == 0 ? 16 : cap * 2;
			grown = realloc(g_mounts, cap * sizeof(*g_mounts));
			if (grown == NULL)
				break;
			g_mounts = grown;
		}
		g_mounts[g_mount_count] = strdup(target);
		if (g_mounts[g_mount_count] != NULL)
			g_mount_count++;
	}
	fclose(f);
}

static int is_mount(const char *path)
{
	size_t i;

	for (i = 0; i < g_mount_count; i++)
		if (strcmp(g_mounts[i], path) == 0)
			return 1;
	return 0;
}

/* nftw() takes no context argument, so the running totals are
 * file-scope. Every caller is a helper child or a test. */
static long long g_walk_bytes;
static long long g_walk_mounts;

static int walk_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	if (ftwbuf->level > 0 && typeflag == FTW_D && is_mount(path)) {
		g_walk_mounts++;
		return FTW_SKIP_SUBTREE;
	}
	if (typeflag != FTW_NS)
		g_walk_bytes += (long long)sb->st_blocks * 512;
	return FTW_CONTINUE;
}

static const char *owner_of(const char *name, const struct datadir_live *live, const char **live_path)
{
	*live_path = NULL;
	if (strcmp(name, "logs") == 0) {
		*live_path = live->log_dir;
		return "log-store";
	}
	if (strcmp(name, "rebuildable") == 0) {
		*live_path = live->rebuildable_dir;
		return "rebuildable-storage";
	}
	if (strcmp(name, "volumes") == 0) {
		*live_path = live->volumes_dir;
		return "volumes";
	}
	if (strcmp(name, "containers") == 0) {
		*live_path = live->containers_dir;
		return "container-pivots";
	}
	if (strcmp(name, "swap") == 0) {
		*live_path = live->swap_dir;
		return "swap";
	}
	/* ADR-0364: these live in STATE_DIR, so a copy here is one the move
	 * left behind -- unless the move failed and the daemon is reading
	 * it this boot, which the live path says. */
	if (strcmp(name, "diskroles.json") == 0) {
		*live_path = live->disk_roles_path;
		return "disk-roles";
	}
	if (strcmp(name, "storage_placement.json") == 0) {
		*live_path = live->storage_placement_path;
		return "storage-placement";
	}
	return "unknown";
}

static int name_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/*
 * Who owns one name, and whether it is in use. At the top level that is
 * owner_of() against the owner's live path. One level down (parent set)
 * it is the rules datadir.h states: a container's directory, the
 * discard queue's trash, or the parent's own owner.
 */
static const char *classify(const char *base_dir, const char *parent, const char *name,
                            const char *path, const struct datadir_live *live,
                            const char **live_path, int *in_use)
{
	const char *owner;

	if (strcmp(name, ".discard") == 0) {
		*live_path = NULL;
		*in_use = 0;
		return "discard-queue";
	}
	if (parent == NULL) {
		owner = owner_of(name, live, live_path);
		*in_use = *live_path != NULL && strcmp(*live_path, path) == 0;
		return owner;
	}
	if (strcmp(parent, "containers") == 0) {
		*live_path = NULL;
		*in_use = live->container_defined != NULL && live->container_defined(name);
		return "container";
	}
	{
		char parent_path[PATH_MAX];

		snprintf(parent_path, sizeof(parent_path), "%s/%s", base_dir, parent);
		owner = owner_of(parent, live, live_path);
		*in_use = *live_path != NULL && strcmp(*live_path, parent_path) == 0;
		return owner;
	}
}

static void write_entry(struct json_writer *w, const char *base_dir, const char *parent,
                        const char *list_dir, const char *name, const struct datadir_live *live,
                        long long *total)
{
	char path[PATH_MAX];
	struct stat st;
	const char *type, *owner, *live_path;
	const char *error = NULL;
	int in_use;
	long long bytes = 0, mounts = 0;

	snprintf(path, sizeof(path), "%s/%s", list_dir, name);
	owner = classify(base_dir, parent, name, path, live, &live_path, &in_use);
	if (lstat(path, &st) != 0) {
		/* Gone between the readdir and the stat. */
		return;
	}
	if (S_ISDIR(st.st_mode) && is_mount(path)) {
		type = "mount";
	} else if (S_ISDIR(st.st_mode)) {
		type = "directory";
		g_walk_bytes = 0;
		g_walk_mounts = 0;
		if (nftw(path, walk_cb, 16, FTW_PHYS | FTW_ACTIONRETVAL) != 0)
			error = strerror(errno);
		bytes = g_walk_bytes;
		mounts = g_walk_mounts;
	} else {
		type = S_ISREG(st.st_mode) ? "file" : S_ISLNK(st.st_mode) ? "symlink" : "other";
		bytes = (long long)st.st_blocks * 512;
	}
	*total += bytes;

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, name);
	jw_key(w, "type");
	jw_str(w, type);
	jw_key(w, "bytes");
	jw_int(w, bytes);
	jw_key(w, "owner");
	jw_str(w, owner);
	jw_key(w, "in_use");
	jw_bool(w, in_use);
	jw_key(w, "live_path");
	if (live_path != NULL)
		jw_str(w, live_path);
	else
		jw_null(w);
	jw_key(w, "mounts_skipped");
	jw_int(w, mounts);
	jw_key(w, "error");
	if (error != NULL)
		jw_str(w, error);
	else
		jw_null(w);
	jw_obj_close(w);
}

int datadir_write_json(struct json_writer *w, const char *base_dir, const char *entry,
                       const struct datadir_live *live, long long measured_at)
{
	char list_dir[PATH_MAX];
	DIR *d;
	struct dirent *de;
	struct statvfs vfs;
	char **names = NULL;
	size_t count = 0, cap = 0, i;
	long long total = 0;
	int have_vfs;

	if (entry != NULL)
		snprintf(list_dir, sizeof(list_dir), "%s/%s", base_dir, entry);
	else
		snprintf(list_dir, sizeof(list_dir), "%s", base_dir);
	d = opendir(list_dir);
	if (d == NULL)
		return -1;
	while ((de = readdir(d)) != NULL) {
		char **grown;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (count == cap) {
			cap = cap == 0 ? 16 : cap * 2;
			grown = realloc(names, cap * sizeof(*names));
			if (grown == NULL)
				break;
			names = grown;
		}
		names[count] = strdup(de->d_name);
		if (names[count] != NULL)
			count++;
	}
	closedir(d);
	qsort(names, count, sizeof(*names), name_cmp);
	mounts_load(base_dir);
	have_vfs = statvfs(base_dir, &vfs) == 0;

	jw_obj_open(w);
	jw_key(w, "path");
	jw_str(w, list_dir);
	jw_key(w, "entry");
	if (entry != NULL)
		jw_str(w, entry);
	else
		jw_null(w);
	jw_key(w, "persistent");
	jw_bool(w, live->fault == NULL);
	jw_key(w, "fault");
	if (live->fault != NULL)
		jw_str(w, live->fault);
	else
		jw_null(w);
	jw_key(w, "filesystem");
	if (have_vfs) {
		jw_obj_open(w);
		jw_key(w, "size_bytes");
		jw_int(w, (long long)vfs.f_blocks * (long long)vfs.f_frsize);
		jw_key(w, "used_bytes");
		jw_int(w, (long long)(vfs.f_blocks - vfs.f_bfree) * (long long)vfs.f_frsize);
		jw_key(w, "free_bytes");
		jw_int(w, (long long)vfs.f_bavail * (long long)vfs.f_frsize);
		jw_obj_close(w);
	} else {
		jw_null(w);
	}
	jw_key(w, "entries");
	jw_arr_open(w);
	for (i = 0; i < count; i++) {
		write_entry(w, base_dir, entry, list_dir, names[i], live, &total);
		free(names[i]);
	}
	jw_arr_close(w);
	free(names);
	mounts_free();
	jw_key(w, "entries_bytes");
	jw_int(w, total);
	jw_key(w, "measured_at");
	jw_int(w, measured_at);
	jw_obj_close(w);
	return 0;
}

int datadir_move(const char *source_dir, const char *target_dir, int copy_logs, long long now,
                 char *reason, size_t reason_size)
{
	char src[PATH_MAX], dst[PATH_MAX], aside[PATH_MAX];
	struct stat st;

	if (reason_size > 0)
		reason[0] = '\0';
	if (!copy_logs)
		return 0;
	if (snprintf(src, sizeof(src), "%s/logs", source_dir) >= (int)sizeof(src) ||
	    snprintf(dst, sizeof(dst), "%s/logs", target_dir) >= (int)sizeof(dst) ||
	    snprintf(aside, sizeof(aside), "%s/logs.replaced-%lld", target_dir, now) >=
	            (int)sizeof(aside)) {
		snprintf(reason, reason_size, "a path under %s or %s is too long", source_dir,
		         target_dir);
		return -1;
	}
	if (lstat(src, &st) != 0)
		return 0; /* no log store here yet: nothing to carry */
	/*
	 * A target that held the data directory before (a move back) has a
	 * logs/ of its own from then. Merging the two would mix segments
	 * of two histories, so the old one is set aside under a dated name,
	 * never deleted: this runs at boot, where nothing may be lost.
	 */
	if (lstat(dst, &st) == 0 && rename(dst, aside) != 0) {
		snprintf(reason, reason_size, "could not set aside the target's own %s: %s", dst,
		         strerror(errno));
		return -1;
	}
	if (treecopy_recursive(src, dst) != 0) {
		snprintf(reason, reason_size, "copying %s to %s failed: %s", src, dst,
		         treecopy_last_error());
		return -1;
	}
	return 0;
}
