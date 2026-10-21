#include "kmodscan.h"
#include "kmod.h"
#include "kmodconfig.h"
#include "logstore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * ADR-0348's sweep and the shared load decision. See kmodscan.h.
 */

/* A sysfs attribute is at most one page, and a CPU's modalias, which
 * lists every feature the CPU has, is the long one: read it whole, so
 * the alias modprobe resolves is the device's own. */
#define KMODSCAN_ALIAS_MAX 4096
#define KMODSCAN_DEVICE_MAX 192
#define KMODSCAN_REASON_MAX 256
/* How deep under /sys/devices the sweep looks. A USB interface behind
 * a bridge and a hub sits about a dozen levels down; this leaves room
 * and still stops a pathological tree. */
#define KMODSCAN_DEPTH_MAX 24
/* Distinct aliases a sweep records. A host past this many is reported
 * as truncated rather than silently cut. */
#define KMODSCAN_ALIASES_MAX 4096
/* Modules recorded for one alias. A CPU's feature alias claims many;
 * past this the entry says the list is cut. */
#define KMODSCAN_MODULES_PER_ALIAS 64

struct kmodscan_entry {
	int alias;  /* index into g_aliases */
	char device[KMODSCAN_DEVICE_MAX];
	char module[KMOD_NAME_MAX];
	enum kmodscan_outcome outcome;
	char reason[KMODSCAN_REASON_MAX];
};

static const char *const g_outcome_names[] = {
	[KMODSCAN_LOADED] = "loaded",
	[KMODSCAN_ALREADY_LOADED] = "already_loaded",
	[KMODSCAN_BUILTIN] = "builtin",
	[KMODSCAN_UNCLAIMED] = "unclaimed",
	[KMODSCAN_REFUSED] = "refused",
	[KMODSCAN_FAILED] = "failed",
};

/* The last sweep's report. One per daemon start; the sweep never runs
 * concurrently with a read of it, since both are on the one thread. */
static struct kmodscan_entry *g_entries;
static int g_entry_count;
static int g_entry_cap;
static int g_ran;
static int g_truncated;
static long long g_started_at;
static long long g_duration_ms;
static char g_not_run[KMODSCAN_REASON_MAX] = "the sweep has not run since the daemon started";

/* Every distinct alias the sweep met, once each, kept for the report;
 * an entry names one by index, since one alias can resolve to several
 * modules. */
static char **g_aliases;
static int g_alias_count;

/* Lines said before the log store opened, in order. Bounded: a host
 * whose sweep says more than this keeps the first lines and a count of
 * the rest, and the report holds every entry regardless. */
#define KMODSCAN_HELD_MAX 512

struct held_line {
	char level[LOGSTORE_LEVEL_MAX];
	char *msg;
};

static struct held_line g_held[KMODSCAN_HELD_MAX];
static int g_held_count;
static int g_held_dropped;
static int g_log_open;

void kmodscan_say(const char *level, const char *fmt, ...)
{
	char msg[LOGSTORE_MSG_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (g_log_open) {
		logstore_write("cixd", level, "%s", msg);
		return;
	}
	if (g_held_count == KMODSCAN_HELD_MAX || (g_held[g_held_count].msg = strdup(msg)) == NULL) {
		g_held_dropped++;
		return;
	}
	snprintf(g_held[g_held_count].level, sizeof(g_held[0].level), "%s", level);
	g_held_count++;
}

void kmodscan_log_held(void)
{
	int i;

	g_log_open = 1;
	for (i = 0; i < g_held_count; i++) {
		logstore_write("cixd", g_held[i].level, "%s", g_held[i].msg);
		free(g_held[i].msg);
		g_held[i].msg = NULL;
	}
	if (g_held_dropped > 0)
		logstore_write("cixd", "warning",
		               "kmod: %d more start-up line(s) were not kept; GET /v1/system/kmod-scan "
		               "has every sweep entry",
		               g_held_dropped);
	g_held_count = g_held_dropped = 0;
}

const char *kmodscan_outcome_name(enum kmodscan_outcome o)
{
	if ((int)o < 0 || (size_t)o >= sizeof(g_outcome_names) / sizeof(g_outcome_names[0]))
		return "failed";
	return g_outcome_names[o];
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

enum kmodscan_outcome kmodscan_load(const char *name, enum kmod_origin origin, char *reason,
                                    size_t reason_size)
{
	char deps[KMODPOLICY_DEPS_MAX][KMOD_NAME_MAX];
	const char *options;
	int ndeps;

	if (reason_size > 0)
		reason[0] = '\0';
	/* Asked first, so a blacklisted module that something else loaded
	 * is reported as refused rather than as already loaded: the report
	 * is about what the policy says. */
	ndeps = kmod_dependencies(name, deps, KMODPOLICY_DEPS_MAX);
	if (kmod_policy_verdict(kmodpolicy_current(), name, deps, ndeps < 0 ? 0 : ndeps, origin,
	                        reason, reason_size) != KMOD_ALLOW)
		return KMODSCAN_REFUSED;
	if (kmod_is_builtin(name))
		return KMODSCAN_BUILTIN;
	if (kmod_is_loaded(name))
		return KMODSCAN_ALREADY_LOADED;
	options = kmodconfig_get_options(name);
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

static void report_reset(void)
{
	int i;

	for (i = 0; i < g_alias_count; i++)
		free(g_aliases[i]);
	free(g_entries);
	free(g_aliases);
	g_entries = NULL;
	g_aliases = NULL;
	g_entry_count = g_entry_cap = g_alias_count = 0;
	g_truncated = 0;
}

static struct kmodscan_entry *entry_add(void)
{
	if (g_entry_count == g_entry_cap) {
		int cap = g_entry_cap == 0 ? 64 : g_entry_cap * 2;
		struct kmodscan_entry *grown = realloc(g_entries, (size_t)cap * sizeof(*grown));

		if (grown == NULL)
			return NULL;
		g_entries = grown;
		g_entry_cap = cap;
	}
	memset(&g_entries[g_entry_count], 0, sizeof(g_entries[0]));
	return &g_entries[g_entry_count++];
}

static int alias_seen(const char *alias)
{
	int i;

	for (i = 0; i < g_alias_count; i++) {
		if (strcmp(g_aliases[i], alias) == 0)
			return 1;
	}
	return 0;
}

/* One device's modalias: recorded once per distinct alias, with each
 * module that claims it decided and loaded. */
static void sweep_alias(const char *alias, const char *device)
{
	char mods[KMODSCAN_MODULES_PER_ALIAS][KMOD_NAME_MAX];
	int n, i, more, idx;

	if (alias_seen(alias))
		return;
	if (g_alias_count == KMODSCAN_ALIASES_MAX) {
		g_truncated = 1;
		return;
	}
	if ((g_aliases[g_alias_count] = strdup(alias)) == NULL) {
		g_truncated = 1;
		return;
	}
	idx = g_alias_count++;

	n = kmod_resolve_alias(alias, mods, KMODSCAN_MODULES_PER_ALIAS, &more);
	if (n <= 0) {
		struct kmodscan_entry *e = entry_add();

		if (e == NULL)
			return;
		e->alias = idx;
		snprintf(e->device, sizeof(e->device), "%s", device);
		e->outcome = KMODSCAN_UNCLAIMED;
		return;
	}
	if (more)
		kmodscan_say("warning",
		             "kmod coldplug: more than %d modules claim %s (%s); the sweep decided the "
		             "first %d",
		             KMODSCAN_MODULES_PER_ALIAS, alias, device, KMODSCAN_MODULES_PER_ALIAS);
	for (i = 0; i < n; i++) {
		struct kmodscan_entry *e = entry_add();

		if (e == NULL)
			return;
		e->alias = idx;
		snprintf(e->device, sizeof(e->device), "%s", device);
		snprintf(e->module, sizeof(e->module), "%s", mods[i]);
		e->outcome = kmodscan_load(mods[i], KMOD_ORIGIN_COLDPLUG, e->reason, sizeof(e->reason));
		switch (e->outcome) {
		case KMODSCAN_LOADED:
			kmodscan_say("info", "kmod coldplug: loaded %s for %s (%s)", e->module, alias,
			             e->device);
			break;
		case KMODSCAN_REFUSED:
			kmodscan_say("info", "kmod coldplug: refused %s for %s (%s): %s", e->module, alias,
			             e->device, e->reason);
			break;
		case KMODSCAN_FAILED:
			kmodscan_say("error", "kmod coldplug: %s for %s (%s) failed: %s", e->module, alias,
			             e->device, e->reason);
			break;
		default:
			break;
		}
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
static void sweep_dir(char *path, size_t len, size_t cap, int depth)
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
			sweep_dir(path, len + 1 + nl, cap, depth + 1);
		} else if (de->d_type == DT_REG && strcmp(de->d_name, "modalias") == 0) {
			/* Static: a page is too large for every frame of the
			 * walk, and it is used up before the walk recurses. */
			static char alias[KMODSCAN_ALIAS_MAX];

			read_modalias(path, alias, sizeof(alias));
			path[len] = '\0';
			if (alias[0] != '\0')
				sweep_alias(alias, path);
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

void kmodscan_not_run(const char *why)
{
	report_reset();
	g_ran = 0;
	snprintf(g_not_run, sizeof(g_not_run), "%s", why);
}

void kmodscan_boot_sweep(void)
{
	char path[PATH_MAX] = "/sys/devices";
	long long t0;
	int counts[sizeof(g_outcome_names) / sizeof(g_outcome_names[0])] = { 0 };
	int i;

	if (!kmod_policy_autoload_covers(kmodpolicy_current(), KMOD_ORIGIN_COLDPLUG)) {
		kmodscan_not_run("autoload in kmod-policy does not include coldplug");
		return;
	}
	if (access(KMOD_MODPROBE_BIN, X_OK) != 0) {
		kmodscan_not_run("modprobe is not present, so no alias can be resolved");
		kmodscan_say("error", "kmod coldplug: %s is not present; the sweep did not run",
		             KMOD_MODPROBE_BIN);
		return;
	}
	report_reset();
	g_aliases = calloc(KMODSCAN_ALIASES_MAX, sizeof(*g_aliases));
	if (g_aliases == NULL) {
		kmodscan_not_run("out of memory for the sweep");
		return;
	}
	g_started_at = (long long)time(NULL);
	t0 = monotonic_ms();
	sweep_dir(path, strlen(path), sizeof(path), 0);
	g_duration_ms = monotonic_ms() - t0;
	g_ran = 1;
	g_not_run[0] = '\0';

	for (i = 0; i < g_entry_count; i++)
		counts[g_entries[i].outcome]++;
	kmodscan_say("info",
	             "kmod coldplug: %d aliases in %lld ms -- %d loaded, %d already loaded, "
	             "%d builtin, %d unclaimed, %d refused, %d failed%s (ADR-0348)",
	             g_alias_count, g_duration_ms, counts[KMODSCAN_LOADED],
	             counts[KMODSCAN_ALREADY_LOADED], counts[KMODSCAN_BUILTIN],
	             counts[KMODSCAN_UNCLAIMED], counts[KMODSCAN_REFUSED], counts[KMODSCAN_FAILED],
	             g_truncated ? ", truncated at the alias limit" : "");
}

void kmodscan_write_json(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "ran");
	jw_bool(w, g_ran);
	if (!g_ran) {
		jw_key(w, "not_run_reason");
		jw_str(w, g_not_run);
	} else {
		jw_key(w, "started_at");
		jw_int(w, g_started_at);
		jw_key(w, "duration_ms");
		jw_int(w, g_duration_ms);
		jw_key(w, "aliases");
		jw_int(w, g_alias_count);
		jw_key(w, "truncated");
		jw_bool(w, g_truncated);
	}
	jw_key(w, "entries");
	jw_arr_open(w);
	for (i = 0; i < g_entry_count; i++) {
		const struct kmodscan_entry *e = &g_entries[i];

		jw_obj_open(w);
		jw_key(w, "alias");
		jw_str(w, g_aliases[e->alias]);
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
