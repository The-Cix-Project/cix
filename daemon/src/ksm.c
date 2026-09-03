#include "ksm.h"
#include "persist.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KSM_DIR "/sys/kernel/mm/ksm"

static struct ksm_config g_config;
static char g_state_path[512];

static void set_defaults(void)
{
	/*
	 * OFF by default, which is the opposite of zswap's choice and for
	 * a reason that matters: zswap costs nothing until the box is
	 * already swapping, whereas the KSM scanner burns CPU continuously
	 * from the moment it is enabled. It also merges nothing until a
	 * container has opted in, so a host that turns this on and changes
	 * nothing else has bought pure overhead.
	 *
	 * 100 and 20 are the kernel's own defaults, restated because this
	 * module writes every parameter on every apply and so needs a
	 * value it knows for each.
	 */
	g_config.enabled = 0;
	g_config.pages_to_scan = 100;
	g_config.sleep_millisecs = 20;
}

static int read_int_file(const char *name, long long *out)
{
	char path[256];
	char buf[64];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "%s/%s", KSM_DIR, name);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	*out = strtoll(buf, NULL, 10);
	return 0;
}

static int write_int_file(const char *name, long long value)
{
	char path[256];
	char buf[32];
	int fd, len;
	ssize_t written;

	snprintf(path, sizeof(path), "%s/%s", KSM_DIR, name);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	len = snprintf(buf, sizeof(buf), "%lld", value);
	written = write(fd, buf, (size_t)len);
	if (close(fd) != 0)
		return -1;
	return (written == len) ? 0 : -1;
}

int ksm_supported(void)
{
	return access(KSM_DIR, F_OK) == 0;
}

static enum ksm_error apply(const struct ksm_config *c)
{
	if (!ksm_supported())
		return KSM_ERR_UNSUPPORTED;
	/*
	 * Tuning before run, deliberately. Writing run=1 first would start
	 * the scanner under whatever values the previous configuration
	 * left, and on a busy host that window is real work done at the
	 * wrong rate.
	 */
	if (write_int_file("pages_to_scan", c->pages_to_scan) != 0 ||
	    write_int_file("sleep_millisecs", c->sleep_millisecs) != 0)
		return KSM_ERR_APPLY_FAILED;
	if (write_int_file("run", c->enabled ? 1 : 0) != 0)
		return KSM_ERR_APPLY_FAILED;
	return KSM_OK;
}

static int save_state(void)
{
	char buf[256];
	int n;

	n = snprintf(buf, sizeof(buf),
	             "{\"enabled\": %s, \"pages_to_scan\": %d, \"sleep_millisecs\": %d}\n",
	             g_config.enabled ? "true" : "false", g_config.pages_to_scan,
	             g_config.sleep_millisecs);
	if (n < 0 || n >= (int)sizeof(buf))
		return -1;
	return persist_atomic_write(g_state_path, buf, (size_t)n);
}

static void load_state(void)
{
	char *raw;
	size_t len;
	struct json_value *root;

	if (persist_read_file(g_state_path, &raw, &len) != 0 || raw == NULL)
		return; /* no persisted intent -- the defaults stand */
	root = json_parse(raw, len);
	free(raw);
	if (root == NULL)
		return;
	{
		const struct json_value *en = json_object_get(root, "enabled");
		const struct json_value *pts = json_object_get(root, "pages_to_scan");
		const struct json_value *slp = json_object_get(root, "sleep_millisecs");

		if (en != NULL && en->type == JSON_BOOL)
			g_config.enabled = en->u.boolean ? 1 : 0;
		if (pts != NULL && pts->type == JSON_NUMBER)
			g_config.pages_to_scan = (int)pts->u.number;
		if (slp != NULL && slp->type == JSON_NUMBER)
			g_config.sleep_millisecs = (int)slp->u.number;
	}
	json_free(root);
}

void ksm_init(const char *path)
{
	set_defaults();
	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	load_state();
	/*
	 * Applied unconditionally at startup, including when disabled: a
	 * reboot does not preserve /sys, so "off" has to be asserted as
	 * much as "on" -- and a kernel that defaults run=0 would otherwise
	 * make the persisted intent look applied when nothing had been.
	 */
	(void)apply(&g_config);
}

void ksm_repoint(const char *path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
}

const struct ksm_config *ksm_get(void)
{
	return &g_config;
}

enum ksm_error ksm_set(const struct ksm_config *next)
{
	struct ksm_config prev = g_config;
	enum ksm_error err;

	if (next == NULL)
		return KSM_ERR_INVALID;
	/*
	 * Both bounds are the kernel's own. pages_to_scan of 0 stops the
	 * scanner while claiming to run, which is a state an operator can
	 * reach by accident and never diagnose; the ceilings keep a typo
	 * from turning the scanner into the workload.
	 */
	if (next->pages_to_scan < 1 || next->pages_to_scan > 10000)
		return KSM_ERR_INVALID;
	if (next->sleep_millisecs < 1 || next->sleep_millisecs > 60000)
		return KSM_ERR_INVALID;
	if (!ksm_supported())
		return KSM_ERR_UNSUPPORTED;

	g_config = *next;
	if (save_state() != 0) {
		g_config = prev;
		return KSM_ERR_PERSIST_FAILED;
	}
	err = apply(&g_config);
	if (err != KSM_OK) {
		g_config = prev;
		(void)save_state();
	}
	return err;
}

void ksm_write_json(struct json_writer *w)
{
	static const char *const stats[] = { "pages_shared", "pages_sharing", "pages_unshared",
	                                      "pages_volatile", "full_scans" };
	long long v;
	size_t i;

	jw_key(w, "supported");
	jw_bool(w, ksm_supported());

	jw_key(w, "enabled");
	jw_bool(w, g_config.enabled);
	jw_key(w, "pages_to_scan");
	jw_int(w, g_config.pages_to_scan);
	jw_key(w, "sleep_millisecs");
	jw_int(w, g_config.sleep_millisecs);

	if (!ksm_supported())
		return;

	/*
	 * What the kernel actually has, separately from what was asked
	 * for. run is the discriminating one: it is 0, 1 or 2, and 2
	 * (unmerging) is a state nothing here sets but an operator can
	 * reach by hand -- reporting the configured boolean alone would
	 * hide it.
	 */
	if (read_int_file("run", &v) == 0) {
		jw_key(w, "kernel_run");
		jw_int(w, (int)v);
	}
	for (i = 0; i < sizeof(stats) / sizeof(stats[0]); i++) {
		if (read_int_file(stats[i], &v) != 0)
			continue;
		jw_key(w, stats[i]);
		jw_int(w, (long long)v);
	}
	/*
	 * The number that decides whether any of this was worth it. Newer
	 * kernels compute it; where it is absent the caller can still
	 * derive the picture from pages_sharing above.
	 */
	if (read_int_file("general_profit", &v) == 0) {
		jw_key(w, "general_profit_bytes");
		jw_int(w, (long long)v);
	}
}
