/*
 * ADR-0381 release 1b: which firmware each device's driver declares, and
 * which of it the host lacks. See fwscan.h.
 */
#include "fwscan.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct fwscan_result g_installed;
static int g_have; /* a scan has been installed */

/* The module behind dir's bound driver, by its sysfs module link; "" for
 * an unbound device or a built-in driver, which has no module link. */
static void driver_module(const char *dir, char *out, size_t out_size)
{
	char link[PATH_MAX], target[PATH_MAX];
	const char *base;
	ssize_t n;

	out[0] = '\0';
	if (snprintf(link, sizeof(link), "%s/driver/module", dir) >= (int)sizeof(link))
		return;
	n = readlink(link, target, sizeof(target) - 1);
	if (n <= 0)
		return;
	target[n] = '\0';
	base = strrchr(target, '/');
	snprintf(out, out_size, "%s", base != NULL ? base + 1 : target);
}

struct name_ctx {
	struct fwscan_result *r;
	const char *firmware_dir, *root_firmware_dir;
};

static int file_present(const char *dir, const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	if (dir == NULL || dir[0] == '\0' ||
	    snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
		return 0;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void add_name(const char *name, void *arg)
{
	struct name_ctx *c = arg;
	struct fwscan_name *n;

	if (c->r->name_count >= FWSCAN_NAMES_MAX || strlen(name) >= FWSCAN_NAME_MAX) {
		c->r->truncated = 1;
		return;
	}
	n = &c->r->names[c->r->name_count++];
	snprintf(n->name, sizeof(n->name), "%s", name);
	n->present = file_present(c->firmware_dir, name) || file_present(c->root_firmware_dir, name);
}

/* The module's index in r, reading its declarations the first time. */
static int module_index(struct fwscan_result *r, const char *name, struct name_ctx *c)
{
	struct fwscan_module *m;
	int i;

	for (i = 0; i < r->module_count; i++)
		if (strcmp(r->modules[i].name, name) == 0)
			return i;
	if (r->module_count >= FWSCAN_MODULES_MAX) {
		r->truncated = 1;
		return -1;
	}
	m = &r->modules[r->module_count];
	snprintf(m->name, sizeof(m->name), "%s", name);
	m->first = r->name_count;
	m->unreadable = kmod_declared_firmware(name, add_name, c) < 0;
	m->count = r->name_count - m->first;
	return r->module_count++;
}

/* Adds module name and its modules.dep closure to the device. */
static void device_add_module(struct fwscan_result *r, struct fwscan_device *d, const char *name,
                              struct name_ctx *c)
{
	char deps[FWSCAN_DEVICE_MODULES_MAX][KMOD_NAME_MAX];
	int k, j, ndeps, idx;

	ndeps = kmod_dependencies(name, deps, FWSCAN_DEVICE_MODULES_MAX - 1);
	if (ndeps < 0)
		ndeps = 0;
	for (k = -1; k < ndeps; k++) {
		const char *m = k < 0 ? name : deps[k];

		idx = module_index(r, m, c);
		if (idx < 0)
			continue;
		for (j = 0; j < d->module_count && d->modules[j] != idx; j++)
			;
		if (j < d->module_count)
			continue;
		if (d->module_count >= FWSCAN_DEVICE_MODULES_MAX) {
			r->truncated = 1;
			return;
		}
		d->modules[d->module_count++] = idx;
	}
}

void fwscan_scan(struct fwscan_result *r, const char *firmware_dir, const char *root_firmware_dir,
                 const char *os_containers_dir)
{
	static struct discovered_device recs[DEVICE_ENUM_MAX];
	struct name_ctx c = { r, firmware_dir, root_firmware_dir };
	int n, i;

	memset(r, 0, sizeof(*r));
	n = device_enumerate(recs, DEVICE_ENUM_MAX, os_containers_dir);
	for (i = 0; i < n && r->device_count < FWSCAN_DEVICES_MAX; i++) {
		struct fwscan_device *d = &r->devices[r->device_count];
		char mod[KMOD_NAME_MAX];
		DIR *dir;
		struct dirent *de;

		if (recs[i].sysfs[0] == '\0')
			continue;
		memset(d, 0, sizeof(*d));
		snprintf(d->id, sizeof(d->id), "%s", recs[i].id);
		driver_module(recs[i].sysfs, mod, sizeof(mod));
		if (mod[0] != '\0')
			device_add_module(r, d, mod, &c);
		/* A USB device's driver binds to its interfaces, its direct
		 * children; the device itself carries the generic usb driver. */
		dir = opendir(recs[i].sysfs);
		while (dir != NULL && (de = readdir(dir)) != NULL) {
			char child[PATH_MAX];
			struct stat st;

			if (de->d_name[0] == '.' ||
			    snprintf(child, sizeof(child), "%s/%s", recs[i].sysfs, de->d_name) >=
			        (int)sizeof(child) ||
			    lstat(child, &st) != 0 || !S_ISDIR(st.st_mode))
				continue;
			driver_module(child, mod, sizeof(mod));
			if (mod[0] != '\0')
				device_add_module(r, d, mod, &c);
		}
		if (dir != NULL)
			closedir(dir);
		if (d->module_count > 0)
			r->device_count++;
	}
}

int fwscan_install(const struct fwscan_result *r)
{
	int changed = !g_have || memcmp(&g_installed, r, sizeof(*r)) != 0;

	memcpy(&g_installed, r, sizeof(*r));
	g_have = 1;
	return changed;
}

void fwscan_write_device(struct json_writer *w, const char *id)
{
	const struct fwscan_result *r = &g_installed;
	const struct fwscan_device *d = NULL;
	int i, k, j, declared = 0, complete = !r->truncated;

	jw_key(w, "firmware");
	for (i = 0; g_have && i < r->device_count; i++)
		if (strcmp(r->devices[i].id, id) == 0) {
			d = &r->devices[i];
			break;
		}
	if (d == NULL) {
		jw_null(w);
		return;
	}
	jw_obj_open(w);
	jw_key(w, "modules");
	jw_arr_open(w);
	for (k = 0; k < d->module_count; k++) {
		const struct fwscan_module *m = &r->modules[d->modules[k]];

		jw_str(w, m->name);
		declared += m->count;
		if (m->unreadable)
			complete = 0;
	}
	jw_arr_close(w);
	jw_key(w, "declared");
	jw_int(w, declared);
	jw_key(w, "missing");
	jw_arr_open(w);
	for (k = 0; k < d->module_count; k++) {
		const struct fwscan_module *m = &r->modules[d->modules[k]];

		for (j = m->first; j < m->first + m->count; j++)
			if (!r->names[j].present)
				jw_str(w, r->names[j].name);
	}
	jw_arr_close(w);
	jw_key(w, "complete");
	jw_bool(w, complete);
	jw_obj_close(w);
}
