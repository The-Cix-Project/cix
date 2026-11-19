/*
 * ADR-0381 release 1b: which firmware each device's driver declares, and
 * which of it the host lacks. See fwscan.h.
 */
#include "fwscan.h"

#include "cbsrecipe.h"

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
	n->in_image = file_present(c->firmware_dir, name);
	n->present = n->in_image || file_present(c->root_firmware_dir, name);
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

/* Is path some record's own sysfs directory? A child that is a record of
 * its own (a PCI bridge's functions) answers for itself, never for its
 * parent. */
static int is_record(const struct discovered_device *recs, int n, const char *path)
{
	int k;

	for (k = 0; k < n; k++)
		if (recs[k].sysfs[0] != '\0' && strcmp(recs[k].sysfs, path) == 0)
			return 1;
	return 0;
}

/*
 * The modules for one directory: its bound driver's when it has one. When
 * it has none, the modules its modalias resolves to, since a driver whose
 * probe failed for a missing file leaves no driver link, and that is the
 * device this exists for (ADR-0381 decision 3). Returns how many it added.
 */
static int dir_add_modules(struct fwscan_result *r, struct fwscan_device *d, const char *dir,
                           struct name_ctx *c)
{
	char mod[KMOD_NAME_MAX], path[PATH_MAX], alias[512];
	char mods[8][KMOD_NAME_MAX];
	int more = 0, k, count;
	FILE *f;

	driver_module(dir, mod, sizeof(mod));
	if (mod[0] != '\0') {
		device_add_module(r, d, mod, c);
		return 1;
	}
	/* Bound, to a driver with no module link: nothing to read, since a
	 * driver's name is not always a module's (a USB device's own driver
	 * is the generic "usb"), and a bound device is never a re-probe
	 * target. Only an unbound one goes on to its modalias. */
	{
		char link[PATH_MAX], target[PATH_MAX];

		if (snprintf(link, sizeof(link), "%s/driver", dir) < (int)sizeof(link) &&
		    readlink(link, target, sizeof(target) - 1) > 0)
			return 0;
	}
	if (snprintf(path, sizeof(path), "%s/modalias", dir) >= (int)sizeof(path))
		return 0;
	f = fopen(path, "r");
	if (f == NULL)
		return 0;
	if (fgets(alias, sizeof(alias), f) == NULL)
		alias[0] = '\0';
	fclose(f);
	alias[strcspn(alias, "\n")] = '\0';
	if (alias[0] == '\0')
		return 0;
	count = kmod_resolve_alias(alias, mods, 8, &more);
	for (k = 0; k < count; k++)
		device_add_module(r, d, mods[k], c);
	if (more)
		r->truncated = 1;
	/* Unbound, and a module claims it: a re-probe target. */
	if (count > 0 && d->probe_count < FWSCAN_PROBE_MAX && strlen(dir) < FWSCAN_PATH_MAX)
		snprintf(d->probe[d->probe_count++], FWSCAN_PATH_MAX, "%s", dir);
	return count > 0 ? count : 0;
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
		DIR *dir;
		struct dirent *de;

		if (recs[i].sysfs[0] == '\0')
			continue;
		memset(d, 0, sizeof(*d));
		snprintf(d->id, sizeof(d->id), "%s", recs[i].id);
		dir_add_modules(r, d, recs[i].sysfs, &c);
		/* A USB device's driver binds to its interfaces, its direct
		 * children; the device itself carries the generic usb driver. */
		dir = opendir(recs[i].sysfs);
		while (dir != NULL && (de = readdir(dir)) != NULL) {
			char child[PATH_MAX];
			struct stat st;

			if (de->d_name[0] == '.' ||
			    snprintf(child, sizeof(child), "%s/%s", recs[i].sysfs, de->d_name) >=
			        (int)sizeof(child) ||
			    lstat(child, &st) != 0 || !S_ISDIR(st.st_mode) || is_record(recs, n, child))
				continue;
			dir_add_modules(r, d, child, &c);
		}
		if (dir != NULL)
			closedir(dir);
		if (d->module_count > 0)
			r->device_count++;
	}
}

struct provider_match {
	struct fwscan_result *r;
};

/* One selector of one output against every name the image lacks. */
static void match_selector(const char *output, const char *recipe, const char *version,
                           const char *pattern, int glob, void *arg)
{
	struct provider_match *m = arg;
	char path[FWSCAN_NAME_MAX + 16];
	int i;

	for (i = 0; i < m->r->name_count; i++) {
		struct fwscan_name *n = &m->r->names[i];

		if (n->in_image)
			continue;
		snprintf(path, sizeof(path), "lib/firmware/%s", n->name);
		if (!cbs_selector_matches(pattern, glob, path))
			continue;
		if (n->provider[0] == '\0') {
			snprintf(n->provider, sizeof(n->provider), "%s", output);
			snprintf(n->recipe, sizeof(n->recipe), "%s", recipe);
			snprintf(n->version, sizeof(n->version), "%s", version);
		} else if (strcmp(n->provider, output) != 0) {
			n->ambiguous = 1;
		}
	}
}

void fwscan_match_providers(struct fwscan_result *r, fwscan_provider_walk walk)
{
	struct provider_match m = { r };
	int i, absent = 0;

	for (i = 0; i < r->name_count; i++)
		if (!r->names[i].in_image)
			absent = 1;
	if (absent && walk != NULL && walk(match_selector, &m) < 0)
		r->truncated = 1;
}

void fwscan_for_each_absent(void (*fn)(const char *device, const char *module,
                                       const struct fwscan_name *n, void *ctx),
                            void *ctx)
{
	const struct fwscan_result *r = &g_installed;
	int i, k, j;

	for (i = 0; g_have && i < r->device_count; i++) {
		const struct fwscan_device *d = &r->devices[i];

		for (k = 0; k < d->module_count; k++) {
			const struct fwscan_module *m = &r->modules[d->modules[k]];

			for (j = m->first; j < m->first + m->count; j++)
				if (!r->names[j].in_image)
					fn(d->id, m->name, &r->names[j], ctx);
		}
	}
}

void fwscan_for_each_probe_target(void (*fn)(const char *dir, void *ctx), void *ctx)
{
	const struct fwscan_result *r = &g_installed;
	int i, k, declares;

	for (i = 0; g_have && i < r->device_count; i++) {
		const struct fwscan_device *d = &r->devices[i];

		for (k = 0, declares = 0; k < d->module_count; k++)
			declares += r->modules[d->modules[k]].count;
		if (declares == 0)
			continue; /* its modules need no firmware: nothing to wait for */
		for (k = 0; k < d->probe_count; k++)
			fn(d->probe[k], ctx);
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
	/* Release 3: the outputs claiming a name the firmware image lacks,
	 * each once, and the missing names no output claims. */
	jw_key(w, "provided_by");
	jw_arr_open(w);
	for (k = 0; k < d->module_count; k++) {
		const struct fwscan_module *m = &r->modules[d->modules[k]];

		for (j = m->first; j < m->first + m->count; j++) {
			const struct fwscan_name *n = &r->names[j];
			int seen = 0, k2, j2;

			if (n->in_image || n->provider[0] == '\0' || n->ambiguous)
				continue;
			/* Said once: an earlier name of this device with this output. */
			for (k2 = 0; k2 <= k && !seen; k2++) {
				const struct fwscan_module *m2 = &r->modules[d->modules[k2]];
				int end = k2 < k ? m2->first + m2->count : j;

				for (j2 = m2->first; j2 < end && !seen; j2++)
					seen = !r->names[j2].in_image && !r->names[j2].ambiguous &&
					       strcmp(r->names[j2].provider, n->provider) == 0;
			}
			if (!seen)
				jw_str(w, n->provider);
		}
	}
	jw_arr_close(w);
	jw_key(w, "unprovided");
	jw_arr_open(w);
	for (k = 0; k < d->module_count; k++) {
		const struct fwscan_module *m = &r->modules[d->modules[k]];

		for (j = m->first; j < m->first + m->count; j++)
			if (!r->names[j].present && r->names[j].provider[0] == '\0')
				jw_str(w, r->names[j].name);
	}
	jw_arr_close(w);
	jw_key(w, "complete");
	jw_bool(w, complete);
	jw_obj_close(w);
}
