#include "kmodscan.h"
#include "events.h"
#include "kmod.h"
#include "kmodconfig.h"
#include "logstore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * ADR-0348's sweeps, the module queue's jobs and the shared load
 * decision. See kmodscan.h.
 */

/* How deep under /sys/devices a sweep looks. A USB interface behind a
 * bridge and a hub sits about a dozen levels down; this leaves room and
 * still stops a pathological tree. */
#define KMODSCAN_DEPTH_MAX 24
/* Distinct aliases a sweep records. A host past this many is reported
 * as truncated rather than silently cut. */
#define KMODSCAN_ALIASES_MAX 4096
/* Modules recorded for one alias. A CPU's feature alias claims many;
 * past this the alias is marked and its line says the list is cut. */
#define KMODSCAN_MODULES_PER_ALIAS 64
/* Entries a report read back may hold: every alias at its cap. */
#define KMODSCAN_ENTRIES_MAX (KMODSCAN_ALIASES_MAX * KMODSCAN_MODULES_PER_ALIAS)

static const char *const g_outcome_names[] = {
	[KMODSCAN_LOADED] = "loaded",
	[KMODSCAN_ALREADY_LOADED] = "already_loaded",
	[KMODSCAN_BUILTIN] = "builtin",
	[KMODSCAN_UNCLAIMED] = "unclaimed",
	[KMODSCAN_REFUSED] = "refused",
	[KMODSCAN_FAILED] = "failed",
	[KMODSCAN_UNLOADED] = "unloaded",
};

static const char *const g_trigger_names[] = {
	[KMODSCAN_TRIGGER_START] = "start",
	[KMODSCAN_TRIGGER_REQUEST] = "request",
	[KMODSCAN_TRIGGER_OVERFLOW] = "overflow",
};

/* The last sweep's report, and whether one is queued or running. Both
 * are the parent's: a sweep in the child fills a report of its own. */
static struct kmodscan_report g_last = {
	.not_run = "no sweep has run since the daemon started",
};
static int g_sweep_pending;

const char *kmodscan_outcome_name(enum kmodscan_outcome o)
{
	if ((int)o < 0 || (size_t)o >= sizeof(g_outcome_names) / sizeof(g_outcome_names[0]))
		return "failed";
	return g_outcome_names[o];
}

static const char *trigger_name(enum kmodscan_trigger t)
{
	if ((int)t < 0 || (size_t)t >= sizeof(g_trigger_names) / sizeof(g_trigger_names[0]))
		return "start";
	return g_trigger_names[t];
}

/* modprobe's output on one line, without trailing whitespace. */
static void fold_lines(char *s)
{
	size_t i, n;

	for (i = 0; s[i] != '\0'; i++) {
		if (s[i] == '\n' || s[i] == '\r' || s[i] == '\t')
			s[i] = ' ';
	}
	n = i;
	while (n > 0 && s[n - 1] == ' ')
		s[--n] = '\0';
}

/* The verdict for name and the dependencies modules.dep lists for it.
 * An unreadable file leaves only the name to decide on, and modprobe
 * could not load anything without it. */
static int refused(const char *name, enum kmod_origin origin, char *why, size_t why_size)
{
	char deps[KMODPOLICY_DEPS_MAX][KMOD_NAME_MAX];
	int ndeps = kmod_dependencies(name, deps, KMODPOLICY_DEPS_MAX);

	return kmod_policy_verdict(kmodpolicy_current(), name, deps, ndeps < 0 ? 0 : ndeps, origin,
	                           why, why_size) != KMOD_ALLOW;
}

enum kmodscan_outcome kmodscan_load(const char *name, enum kmod_origin origin, const char *options,
                                    char *reason, size_t reason_size)
{
	if (reason_size > 0)
		reason[0] = '\0';
	/* Asked first, so a blacklisted module that something else loaded
	 * is reported as refused rather than as already loaded: the report
	 * is about what the policy says. */
	if (refused(name, origin, reason, reason_size))
		return KMODSCAN_REFUSED;
	if (kmod_is_builtin(name))
		return KMODSCAN_BUILTIN;
	if (options == NULL)
		options = kmodconfig_get_options(name);
	if (kmod_is_loaded(name)) {
		/*
		 * modprobe on a loaded module succeeds and does nothing, so
		 * options asked for now would be reported as applied when the
		 * kernel never saw them. Module parameters are set when a module
		 * is inserted, and only the operator can decide whether to
		 * unload a live one, so an explicit request is told why.
		 */
		if (origin == KMOD_ORIGIN_REQUEST && options != NULL && options[0] != '\0')
			snprintf(reason, reason_size,
			         "%s is already loaded -- module parameters are set when a module is "
			         "inserted, so unload it first (DELETE /v1/system/kmod/%s) to change them",
			         name, name);
		return KMODSCAN_ALREADY_LOADED;
	}
	if (kmod_load(name, options, reason, reason_size) != 0) {
		fold_lines(reason);
		if (reason[0] == '\0')
			snprintf(reason, reason_size, "modprobe failed and said nothing about why");
		return KMODSCAN_FAILED;
	}
	if (reason_size > 0)
		reason[0] = '\0';
	return KMODSCAN_LOADED;
}

void kmodscan_report_init(struct kmodscan_report *r)
{
	memset(r, 0, sizeof(*r));
}

void kmodscan_report_free(struct kmodscan_report *r)
{
	int i;

	for (i = 0; i < r->alias_count; i++)
		free(r->aliases[i]);
	free(r->aliases);
	free(r->alias_more);
	free(r->entries);
	kmodscan_report_init(r);
}

int kmodscan_report_changed_modules(const struct kmodscan_report *r)
{
	int i;

	for (i = 0; i < r->entry_count; i++) {
		if (r->entries[i].outcome == KMODSCAN_LOADED || r->entries[i].outcome == KMODSCAN_UNLOADED)
			return 1;
	}
	return 0;
}

static struct kmodscan_entry *entry_add(struct kmodscan_report *r, int alias, const char *device,
                                        const char *module)
{
	struct kmodscan_entry *e;

	if (r->entry_count == r->entry_cap) {
		int cap = r->entry_cap == 0 ? 64 : r->entry_cap * 2;
		struct kmodscan_entry *grown = realloc(r->entries, (size_t)cap * sizeof(*grown));

		if (grown == NULL)
			return NULL;
		r->entries = grown;
		r->entry_cap = cap;
	}
	e = &r->entries[r->entry_count++];
	memset(e, 0, sizeof(*e));
	e->alias = alias;
	snprintf(e->device, sizeof(e->device), "%s", device);
	snprintf(e->module, sizeof(e->module), "%s", module);
	return e;
}

/* Records alias and returns its index, or -1 when the report is full
 * (marked truncated) or out of memory. */
static int alias_add(struct kmodscan_report *r, const char *alias)
{
	if (r->alias_count == KMODSCAN_ALIASES_MAX) {
		r->truncated = 1;
		return -1;
	}
	if (r->alias_count == r->alias_cap) {
		int cap = r->alias_cap == 0 ? 64 : r->alias_cap * 2;
		char **grown = realloc(r->aliases, (size_t)cap * sizeof(*grown));
		unsigned char *more;

		if (grown == NULL)
			return -1;
		r->aliases = grown;
		more = realloc(r->alias_more, (size_t)cap);
		if (more == NULL)
			return -1;
		r->alias_more = more;
		r->alias_cap = cap;
	}
	if ((r->aliases[r->alias_count] = strdup(alias)) == NULL) {
		r->truncated = 1;
		return -1;
	}
	r->alias_more[r->alias_count] = 0;
	return r->alias_count++;
}

static int alias_seen(const struct kmodscan_report *r, const char *alias)
{
	int i;

	for (i = 0; i < r->alias_count; i++) {
		if (strcmp(r->aliases[i], alias) == 0)
			return 1;
	}
	return 0;
}

/* One device's modalias, recorded once per distinct alias, with each
 * module that claims it decided and loaded. */
static void resolve_alias(struct kmodscan_report *r, const char *alias, const char *device,
                          enum kmod_origin origin)
{
	char mods[KMODSCAN_MODULES_PER_ALIAS][KMOD_NAME_MAX];
	int n, i, more, idx;

	if (alias_seen(r, alias) || (idx = alias_add(r, alias)) < 0)
		return;
	n = kmod_resolve_alias(alias, mods, KMODSCAN_MODULES_PER_ALIAS, &more);
	if (n <= 0) {
		struct kmodscan_entry *e = entry_add(r, idx, device, "");

		if (e != NULL)
			e->outcome = KMODSCAN_UNCLAIMED;
		return;
	}
	r->alias_more[idx] = (unsigned char)(more != 0);
	for (i = 0; i < n; i++) {
		struct kmodscan_entry *e = entry_add(r, idx, device, mods[i]);

		if (e == NULL)
			return;
		e->outcome = kmodscan_load(mods[i], origin, NULL, e->reason, sizeof(e->reason));
	}
}

/* Reads one modalias file; "" when it is empty or unreadable. */
static void read_modalias(const char *path, char *out, size_t out_size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	out[0] = '\0';
	if (fd < 0)
		return;
	n = read(fd, out, out_size - 1);
	close(fd);
	if (n <= 0) {
		out[0] = '\0';
		return;
	}
	out[n] = '\0';
	out[strcspn(out, "\n")] = '\0';
}

/* Depth-first over real directories only: sysfs links (driver,
 * subsystem, firmware_node) point back across the tree. */
static void sweep_dir(struct kmodscan_report *r, enum kmod_origin origin, char *path, size_t len,
                      size_t cap, int depth)
{
	DIR *d;
	struct dirent *de;

	if (depth > KMODSCAN_DEPTH_MAX)
		return;
	d = opendir(path);
	if (d == NULL)
		return;
	while ((de = readdir(d)) != NULL) {
		size_t nl = strlen(de->d_name);

		if (de->d_name[0] == '.')
			continue;
		if (len + 1 + nl + 1 > cap)
			continue;
		path[len] = '/';
		memcpy(path + len + 1, de->d_name, nl + 1);
		if (de->d_type == DT_DIR) {
			sweep_dir(r, origin, path, len + 1 + nl, cap, depth + 1);
		} else if (de->d_type == DT_REG && strcmp(de->d_name, "modalias") == 0) {
			/* Static: a page is too large for every frame of the
			 * walk, and it is used up before the walk recurses. */
			static char alias[KMODSCAN_ALIAS_MAX];

			read_modalias(path, alias, sizeof(alias));
			path[len] = '\0';
			if (alias[0] != '\0')
				resolve_alias(r, alias, path, origin);
		}
		path[len] = '\0';
	}
	closedir(d);
}

static long long monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

static enum kmod_origin sweep_origin(enum kmodscan_trigger t)
{
	/* A sweep that catches up on dropped uevents stands for the plugs it
	 * missed. Both origins are automatic, so the allowlist binds either. */
	return t == KMODSCAN_TRIGGER_OVERFLOW ? KMOD_ORIGIN_HOTPLUG : KMOD_ORIGIN_COLDPLUG;
}

static void run_sweep(struct kmodscan_report *r, enum kmodscan_trigger trigger)
{
	char path[PATH_MAX] = "/sys/devices";
	long long t0;

	r->trigger = trigger;
	if (access(KMOD_MODPROBE_BIN, X_OK) != 0) {
		snprintf(r->not_run, sizeof(r->not_run), "%s is not present, so no alias can be resolved",
		         KMOD_MODPROBE_BIN);
		return;
	}
	r->started_at = (long long)time(NULL);
	t0 = monotonic_ms();
	sweep_dir(r, sweep_origin(trigger), path, strlen(path), sizeof(path), 0);
	r->duration_ms = monotonic_ms() - t0;
	r->ran = 1;
}

/*
 * An explicit load. The name as asked is decided first, so a blacklisted
 * name is refused whatever modprobe would make of it; then it is
 * resolved, since it may be an alias, and every module it resolves to is
 * decided before any loads. A name nothing resolves -- or a host with no
 * modprobe to resolve it -- is handed to modprobe as it is, so the
 * failure is modprobe's own words.
 */
static void run_load(struct kmodscan_report *r, const struct kmodscan_job *job)
{
	char mods[KMODSCAN_MODULES_PER_ALIAS][KMOD_NAME_MAX];
	const char *options = job->has_options ? job->options : NULL;
	struct kmodscan_entry *e;
	int idx, n, i, more;

	if ((idx = alias_add(r, job->name)) < 0)
		return;
	{
		char why[KMODSCAN_REASON_MAX];

		if (refused(job->name, KMOD_ORIGIN_REQUEST, why, sizeof(why))) {
			if ((e = entry_add(r, idx, "", job->name)) != NULL) {
				e->outcome = KMODSCAN_REFUSED;
				snprintf(e->reason, sizeof(e->reason), "%s", why);
			}
			return;
		}
	}
	n = kmod_resolve_alias(job->name, mods, KMODSCAN_MODULES_PER_ALIAS, &more);
	if (n <= 0) {
		snprintf(mods[0], sizeof(mods[0]), "%s", job->name);
		n = 1;
	}
	r->alias_more[idx] = (unsigned char)(more != 0);
	for (i = 0; i < n; i++) {
		char why[KMODSCAN_REASON_MAX];

		if (!refused(mods[i], KMOD_ORIGIN_REQUEST, why, sizeof(why)))
			continue;
		if ((e = entry_add(r, idx, "", mods[i])) != NULL) {
			e->outcome = KMODSCAN_REFUSED;
			snprintf(e->reason, sizeof(e->reason), "%s, which %s resolves to", why, job->name);
		}
		return;
	}
	for (i = 0; i < n; i++) {
		if ((e = entry_add(r, idx, "", mods[i])) == NULL)
			return;
		e->outcome = kmodscan_load(mods[i], KMOD_ORIGIN_REQUEST, options, e->reason,
		                           sizeof(e->reason));
	}
}

static void run_unload(struct kmodscan_report *r, const struct kmodscan_job *job)
{
	struct kmodscan_entry *e;
	int idx;

	if ((idx = alias_add(r, job->name)) < 0 || (e = entry_add(r, idx, "", job->name)) == NULL)
		return;
	if (kmod_unload(job->name, e->reason, sizeof(e->reason)) == 0) {
		e->outcome = KMODSCAN_UNLOADED;
		e->reason[0] = '\0';
		return;
	}
	fold_lines(e->reason);
	e->outcome = KMODSCAN_FAILED;
}

void kmodscan_run(const struct kmodscan_job *job, struct kmodscan_report *out)
{
	switch (job->kind) {
	case KMODSCAN_JOB_SWEEP:
		run_sweep(out, job->trigger);
		break;
	case KMODSCAN_JOB_ALIAS:
		/* Asked again here, not only when the uevent was queued: the
		 * policy is the one at the moment the job runs. */
		if (kmod_policy_autoload_covers(kmodpolicy_current(), KMOD_ORIGIN_HOTPLUG))
			resolve_alias(out, job->alias, job->device, KMOD_ORIGIN_HOTPLUG);
		break;
	case KMODSCAN_JOB_LOAD:
		run_load(out, job);
		break;
	case KMODSCAN_JOB_UNLOAD:
		run_unload(out, job);
		break;
	}
}

/* ---- the report through a file: header, aliases, entries ---- */

#define KMODSCAN_WIRE_MAGIC 0x314d4b43u /* "CKM1" */

struct wire_header {
	uint32_t magic;
	int32_t ran;
	int32_t truncated;
	int32_t trigger;
	int32_t alias_count;
	int32_t entry_count;
	int64_t started_at;
	int64_t duration_ms;
	char not_run[KMODSCAN_REASON_MAX];
};

static int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t n = write(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
	char *p = buf;

	while (len > 0) {
		ssize_t n = read(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

int kmodscan_report_write(const struct kmodscan_report *r, int fd)
{
	struct wire_header h;
	int i;

	memset(&h, 0, sizeof(h));
	h.magic = KMODSCAN_WIRE_MAGIC;
	h.ran = r->ran;
	h.truncated = r->truncated;
	h.trigger = (int32_t)r->trigger;
	h.alias_count = r->alias_count;
	h.entry_count = r->entry_count;
	h.started_at = r->started_at;
	h.duration_ms = r->duration_ms;
	memcpy(h.not_run, r->not_run, sizeof(h.not_run));
	if (write_all(fd, &h, sizeof(h)) != 0)
		return -1;
	for (i = 0; i < r->alias_count; i++) {
		uint32_t len = (uint32_t)strlen(r->aliases[i]);
		unsigned char more = r->alias_more[i];

		if (write_all(fd, &len, sizeof(len)) != 0 || write_all(fd, &more, 1) != 0 ||
		    write_all(fd, r->aliases[i], len) != 0)
			return -1;
	}
	if (r->entry_count > 0 &&
	    write_all(fd, r->entries, (size_t)r->entry_count * sizeof(r->entries[0])) != 0)
		return -1;
	return 0;
}

int kmodscan_report_read(struct kmodscan_report *r, int fd)
{
	struct wire_header h;
	int i;

	kmodscan_report_init(r);
	if (read_all(fd, &h, sizeof(h)) != 0 || h.magic != KMODSCAN_WIRE_MAGIC ||
	    h.alias_count < 0 || h.alias_count > KMODSCAN_ALIASES_MAX || h.entry_count < 0 ||
	    h.entry_count > KMODSCAN_ENTRIES_MAX || h.trigger < 0 ||
	    (size_t)h.trigger >= sizeof(g_trigger_names) / sizeof(g_trigger_names[0]))
		return -1;
	r->ran = h.ran != 0;
	r->truncated = h.truncated != 0;
	r->trigger = (enum kmodscan_trigger)h.trigger;
	r->started_at = h.started_at;
	r->duration_ms = h.duration_ms;
	h.not_run[sizeof(h.not_run) - 1] = '\0';
	snprintf(r->not_run, sizeof(r->not_run), "%s", h.not_run);
	for (i = 0; i < h.alias_count; i++) {
		char alias[KMODSCAN_ALIAS_MAX];
		uint32_t len;
		unsigned char more;
		int idx;

		if (read_all(fd, &len, sizeof(len)) != 0 || len >= sizeof(alias) ||
		    read_all(fd, &more, 1) != 0 || read_all(fd, alias, len) != 0)
			goto bad;
		alias[len] = '\0';
		if ((idx = alias_add(r, alias)) < 0)
			goto bad;
		r->alias_more[idx] = more;
	}
	if (h.entry_count > 0) {
		r->entries = malloc((size_t)h.entry_count * sizeof(r->entries[0]));
		if (r->entries == NULL ||
		    read_all(fd, r->entries, (size_t)h.entry_count * sizeof(r->entries[0])) != 0)
			goto bad;
		r->entry_count = r->entry_cap = h.entry_count;
		for (i = 0; i < r->entry_count; i++) {
			struct kmodscan_entry *e = &r->entries[i];

			if (e->alias < 0 || e->alias >= r->alias_count || (int)e->outcome < 0 ||
			    (size_t)e->outcome >= sizeof(g_outcome_names) / sizeof(g_outcome_names[0]))
				goto bad;
			e->device[sizeof(e->device) - 1] = '\0';
			e->module[sizeof(e->module) - 1] = '\0';
			e->reason[sizeof(e->reason) - 1] = '\0';
		}
	}
	return 0;
bad:
	kmodscan_report_free(r);
	return -1;
}

/* ---- the parent's half ---- */

void kmodscan_report_say(const struct kmodscan_report *r, enum kmod_origin origin)
{
	const char *how = origin == KMOD_ORIGIN_HOTPLUG ? "hotplug" : "coldplug";
	int i;

	for (i = 0; i < r->alias_count; i++) {
		if (r->alias_more[i])
			logstore_write("cixd", "warning",
			               "kmod %s: more than %d modules claim %s; the first %d were decided",
			               how, KMODSCAN_MODULES_PER_ALIAS, r->aliases[i],
			               KMODSCAN_MODULES_PER_ALIAS);
	}
	for (i = 0; i < r->entry_count; i++) {
		const struct kmodscan_entry *e = &r->entries[i];
		const char *alias = r->aliases[e->alias];

		switch (e->outcome) {
		case KMODSCAN_LOADED:
			logstore_write("cixd", "info", "kmod %s: loaded %s for %s (%s)", how, e->module,
			               alias, e->device);
			break;
		case KMODSCAN_REFUSED:
			logstore_write("cixd", "info", "kmod %s: refused %s for %s (%s): %s", how,
			               e->module, alias, e->device, e->reason);
			break;
		case KMODSCAN_FAILED:
			logstore_write("cixd", "error", "kmod %s: %s for %s (%s) failed: %s", how,
			               e->module, alias, e->device, e->reason);
			break;
		default:
			break;
		}
	}
}

int kmodscan_sweep_pending(void)
{
	return g_sweep_pending;
}

void kmodscan_set_sweep_pending(int pending)
{
	pending = pending != 0;
	if (g_sweep_pending == pending)
		return;
	g_sweep_pending = pending;
	events_announce(EV_getKmodScan);
}

void kmodscan_adopt(struct kmodscan_report *r)
{
	int counts[sizeof(g_outcome_names) / sizeof(g_outcome_names[0])] = { 0 };
	int i;

	kmodscan_report_free(&g_last);
	g_last = *r;
	kmodscan_report_init(r);
	g_sweep_pending = 0;
	events_announce(EV_getKmodScan);

	if (!g_last.ran) {
		logstore_write("cixd", "error", "kmod sweep (%s) did not run: %s",
		               trigger_name(g_last.trigger), g_last.not_run);
		return;
	}
	kmodscan_report_say(&g_last, sweep_origin(g_last.trigger));
	for (i = 0; i < g_last.entry_count; i++)
		counts[g_last.entries[i].outcome]++;
	logstore_write("cixd", "info",
	               "kmod sweep (%s): %d aliases in %lld ms -- %d loaded, %d already loaded, "
	               "%d builtin, %d unclaimed, %d refused, %d failed%s (ADR-0348)",
	               trigger_name(g_last.trigger), g_last.alias_count, g_last.duration_ms,
	               counts[KMODSCAN_LOADED], counts[KMODSCAN_ALREADY_LOADED],
	               counts[KMODSCAN_BUILTIN], counts[KMODSCAN_UNCLAIMED], counts[KMODSCAN_REFUSED],
	               counts[KMODSCAN_FAILED], g_last.truncated ? ", truncated at the alias limit" : "");
}

void kmodscan_not_run(const char *why)
{
	kmodscan_report_free(&g_last);
	snprintf(g_last.not_run, sizeof(g_last.not_run), "%s", why);
	events_announce(EV_getKmodScan);
}

void kmodscan_boot_sweep(void)
{
	static struct kmodscan_job job; /* a page and more: not on the stack */
	struct kmodscan_report r;

	if (!kmod_policy_autoload_covers(kmodpolicy_current(), KMOD_ORIGIN_COLDPLUG)) {
		kmodscan_not_run("autoload in kmod-policy does not include coldplug");
		return;
	}
	memset(&job, 0, sizeof(job));
	job.kind = KMODSCAN_JOB_SWEEP;
	job.trigger = KMODSCAN_TRIGGER_START;
	kmodscan_report_init(&r);
	kmodscan_run(&job, &r);
	kmodscan_adopt(&r);
}

void kmodscan_write_json(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "ran");
	jw_bool(w, g_last.ran);
	jw_key(w, "running");
	jw_bool(w, g_sweep_pending);
	if (!g_last.ran) {
		jw_key(w, "not_run_reason");
		jw_str(w, g_last.not_run);
	} else {
		jw_key(w, "trigger");
		jw_str(w, trigger_name(g_last.trigger));
		jw_key(w, "started_at");
		jw_int(w, g_last.started_at);
		jw_key(w, "duration_ms");
		jw_int(w, g_last.duration_ms);
		jw_key(w, "aliases");
		jw_int(w, g_last.alias_count);
		jw_key(w, "truncated");
		jw_bool(w, g_last.truncated);
	}
	jw_key(w, "entries");
	jw_arr_open(w);
	for (i = 0; i < g_last.entry_count; i++) {
		const struct kmodscan_entry *e = &g_last.entries[i];

		jw_obj_open(w);
		jw_key(w, "alias");
		jw_str(w, g_last.aliases[e->alias]);
		jw_key(w, "device");
		jw_str(w, e->device);
		jw_key(w, "module");
		if (e->module[0] != '\0')
			jw_str(w, e->module);
		else
			jw_null(w);
		jw_key(w, "outcome");
		jw_str(w, kmodscan_outcome_name(e->outcome));
		if (e->reason[0] != '\0') {
			jw_key(w, "reason");
			jw_str(w, e->reason);
		}
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}

static kmodscan_broker_fn g_broker;

void kmodscan_set_broker(kmodscan_broker_fn fn)
{
	g_broker = fn;
}

enum kmodscan_answer kmodscan_ask(const struct kmodscan_job *job, struct kmodscan_report **out)
{
	*out = NULL;
	if (g_broker == NULL)
		return KMODSCAN_BUSY;
	return g_broker(job, out);
}
