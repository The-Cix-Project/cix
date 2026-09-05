#include "quotamap.h"
#include <mntent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/quota.h>
#include "json.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Project id 0 is reserved (an untagged inode's fsx_projid reads back
 * as 0 -- see linux/fs.h's own struct fsxattr comment) and must never
 * be handed out as a real assignment, so real allocation starts here.
 */
#define QUOTAMAP_FIRST_ID 1000

struct quotamap_entry {
	char name[REGISTRY_NAME_MAX];
	uint32_t project_id;
	int in_use;
};

static struct quotamap_entry g_entries[QUOTAMAP_MAX];
static uint32_t g_next_id;
static char g_state_path[PATH_MAX];

static int save_state(void)
{
	struct json_writer w;
	int rc, i;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "next_id");
	jw_int(&w, (long long)g_next_id);
	jw_key(&w, "entries");
	jw_arr_open(&w);
	for (i = 0; i < QUOTAMAP_MAX; i++) {
		if (!g_entries[i].in_use)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, g_entries[i].name);
		jw_key(&w, "project_id");
		jw_int(&w, (long long)g_entries[i].project_id);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jentries, *jnext_id;
	size_t i;
	int count = 0;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL) {
		g_next_id = QUOTAMAP_FIRST_ID;
		return 0;
	}

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted quota project-id map\n", g_state_path);
		return -1;
	}

	jnext_id = json_object_get(root, "next_id");
	g_next_id = jnext_id != NULL ? (uint32_t)json_as_number(jnext_id) : QUOTAMAP_FIRST_ID;
	if (g_next_id < QUOTAMAP_FIRST_ID)
		g_next_id = QUOTAMAP_FIRST_ID;

	jentries = json_object_get(root, "entries");
	if (jentries != NULL && jentries->type == JSON_ARRAY) {
		if (jentries->u.array.count > QUOTAMAP_MAX) {
			json_free(root);
			fprintf(stderr, "%s: more entries persisted than QUOTAMAP_MAX\n", g_state_path);
			return -1;
		}
		for (i = 0; i < jentries->u.array.count; i++) {
			const struct json_value *item = jentries->u.array.items[i];
			const char *name = json_as_string(json_object_get(item, "name"));
			const struct json_value *jpid = json_object_get(item, "project_id");

			if (name == NULL || jpid == NULL) {
				json_free(root);
				fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
				return -1;
			}
			snprintf(g_entries[count].name, sizeof(g_entries[count].name), "%s", name);
			g_entries[count].project_id = (uint32_t)json_as_number(jpid);
			g_entries[count].in_use = 1;
			count++;
		}
	}
	json_free(root);
	return 0;
}

void quotamap_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

int quotamap_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >=
	    (int)sizeof(g_state_path))
		return -1;
	memset(g_entries, 0, sizeof(g_entries));
	g_next_id = QUOTAMAP_FIRST_ID;
	return load_state();
}

int quotamap_get_or_assign(const char *name, uint32_t *out_projid)
{
	int i, free_slot = -1;

	for (i = 0; i < QUOTAMAP_MAX; i++) {
		if (!g_entries[i].in_use) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (strcmp(g_entries[i].name, name) == 0) {
			*out_projid = g_entries[i].project_id;
			return 0;
		}
	}

	if (free_slot < 0)
		return -1;

	snprintf(g_entries[free_slot].name, sizeof(g_entries[free_slot].name), "%s", name);
	g_entries[free_slot].project_id = g_next_id;
	g_entries[free_slot].in_use = 1;
	g_next_id++;

	if (save_state() != 0) {
		g_entries[free_slot].in_use = 0;
		g_next_id--;
		return -1;
	}

	*out_projid = g_entries[free_slot].project_id;
	return 0;
}

/*
 * Applying a project quota to a filesystem, moved here from main.c
 * (ADR-0249).
 *
 * quotamap.c already owned which project id a name gets; the call that
 * makes that id mean something on disk lived in main.c, where the
 * volume handlers and container creation both reached for it as a
 * static. Two callers in two different concerns is what a module is
 * for, and keeping the assignment and the application apart meant
 * neither file owned "quota".
 */
/*
 * Real ext4 project-quota device resolution (Part 4, bare-metal-
 * readiness plan, ADR-0062). quotactl(2)'s own "special" argument
 * needs the real block device backing wherever CONTAINERS_DIR actually
 * lives -- which is CONTAINERS_DEVICE only under a real --init-mode
 * boot; every daemon-linked test and any --data-dir= override instead
 * points g_base_dir at an ordinary directory on whatever filesystem the
 * host/test environment's own root happens to be (see CONTAINERS_
 * DEVICE's own comment above for the tmpfs-fallback case, which is a
 * third possibility again). Hardcoding CONTAINERS_DEVICE here would be
 * silently wrong in both of those cases -- this project's own bare-
 * metal-readiness plan flagged this exact question explicitly ("device-
 * path resolution... must be confirmed, not assumed"), so it's resolved
 * for real instead: walk /proc/mounts and pick the longest-matching
 * mount point for `path` (the same "find the owning mount" algorithm
 * findmnt/df use internally), returning 0 and filling out_device on
 * success. -1 (errno set) if /proc/mounts can't be read or path isn't
 * under any mount point at all (should never happen for a legitimately
 * mounted directory).
 *
 * Deliberately does not decode octal-escaped whitespace in /proc/mounts'
 * own mountpoint field (e.g. "\040" for a literal space) -- no path this
 * project ever mounts anything at (CONTAINERS_DIR, PKI_DIR, or any
 * --data-dir=/mkdtemp() test path) contains a space, so handling that
 * general case would be real, unexercised complexity for a scenario
 * that can't occur here.
 */
static int resolve_backing_device(const char *path, char *out_device, size_t out_size)
{
	char real_path[PATH_MAX];
	FILE *f;
	char line[PATH_MAX * 2];
	size_t best_len = 0;
	int found = 0;

	if (realpath(path, real_path) == NULL)
		return -1;

	f = fopen("/proc/mounts", "r");
	if (f == NULL)
		return -1;

	while (fgets(line, sizeof(line), f) != NULL) {
		char device[PATH_MAX];
		char mountpoint[PATH_MAX];
		size_t mp_len;

		if (sscanf(line, "%4095s %4095s", device, mountpoint) != 2)
			continue;

		mp_len = strlen(mountpoint);
		if (strncmp(real_path, mountpoint, mp_len) != 0)
			continue;
		/* Exact match, or the next real_path char must be '/' -- so a
		 * mountpoint of "/var" never matches a real_path of
		 * "/variant". */
		if (real_path[mp_len] != '\0' && real_path[mp_len] != '/')
			continue;
		if (mp_len < best_len)
			continue;

		best_len = mp_len;
		if (snprintf(out_device, out_size, "%s", device) >= (int)out_size) {
			fclose(f);
			errno = ENAMETOOLONG;
			return -1;
		}
		found = 1;
	}
	fclose(f);

	if (!found) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

/*
 * Sets a real, kernel-enforced hard limit of quota_bytes for project id
 * projid on whatever device backs base_path (the container's own
 * container_base -- CONTAINERS_DIR/<name> by default, or
 * <disk's mount_path>/containers/<name> for a disk-placed container,
 * task #638 -- NOT always CONTAINERS_DIR itself: a container placed on
 * an alternate disk must have its quota set against THAT disk's own
 * backing device, or the limit would silently apply to the wrong
 * filesystem entirely while the actual files live elsewhere), via a
 * real quotactl(2) Q_SETQUOTA call -- independent of and order-agnostic with
 * src/overlay.c's own FS_IOC_FSSETXATTR tagging (that call says "these
 * files belong to project X"; this one says "project X's own limit is
 * Y" -- setting a limit for a project id the kernel has never seen an
 * inode tagged with yet is a completely normal, harmless no-op until
 * one shows up). dqb_bhardlimit is in real quota *blocks* (always
 * 1024 bytes each, regardless of the filesystem's own block size --
 * see /usr/include/x86_64-linux-gnu/sys/quota.h's own struct dqblk
 * comment), not raw bytes, hence the rounding-up conversion. No soft
 * limit / grace-period policy -- dqb_bsoftlimit is set equal to the
 * hard limit, so writes are refused (EDQUOT) the instant the real limit
 * is hit, not merely warned about after some grace period this project
 * has no mechanism to surface to an operator anyway. Returns 0 on
 * success, -1 (errno set by quotactl(2) -- ENOTSUP/EOPNOTSUPP if the
 * backing filesystem doesn't have the project-quota feature enabled at
 * all, exactly what a filesystem cix-install.c didn't create via
 * mkfs.ext4 -O quota -E quotatype=prjquota reports) otherwise.
 */
int quotamap_apply(const char *base_path, uint32_t projid, long long quota_bytes)
{
	char device[PATH_MAX];
	struct dqblk dq;

	if (resolve_backing_device(base_path, device, sizeof(device)) != 0)
		return -1;

	memset(&dq, 0, sizeof(dq));
	dq.dqb_bhardlimit = (uint64_t)((quota_bytes + 1023) / 1024);
	dq.dqb_bsoftlimit = dq.dqb_bhardlimit;
	dq.dqb_valid = QIF_BLIMITS;

	if (quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), device, (int)projid, (caddr_t)&dq) != 0)
		return -1;
	return 0;
}
