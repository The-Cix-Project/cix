#include "wireless.h"
#include "logstore.h"
#include "nl80211.h"
#include "persist.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host's domain plus every radio that keeps its own. A host with
 * more than this many radios reports the first ones the kernel sends. */
#define WIRELESS_DOMAINS_MAX 32

static char g_country[3];   /* "" when none is configured */
static char g_apply_error[160];
static char g_state_path[512];

int wireless_country_valid(const char *c)
{
	return c != NULL && strlen(c) == 2 && c[0] >= 'A' && c[0] <= 'Z' && c[1] >= 'A' &&
	       c[1] <= 'Z';
}

const char *wireless_country(void)
{
	return g_country[0] != '\0' ? g_country : NULL;
}

static int save_state(void)
{
	char buf[64];
	int n;

	if (g_state_path[0] == '\0')
		return -1;
	if (g_country[0] != '\0')
		n = snprintf(buf, sizeof(buf), "{\"country\":\"%s\"}\n", g_country);
	else
		n = snprintf(buf, sizeof(buf), "{\"country\":null}\n");
	if (n < 0 || (size_t)n >= sizeof(buf))
		return -1;
	return persist_atomic_write(g_state_path, buf, (size_t)n);
}

/*
 * Sends the configured country, or the world domain when none is set,
 * and records the outcome. A refusal is kept rather than returned: the
 * setting is what was asked for, and the kernel's own fields in the
 * read say what took.
 */
static void apply(void)
{
	const char *alpha2 = g_country[0] != '\0' ? g_country : "00";

	if (nl80211_reg_set(alpha2) == 0) {
		g_apply_error[0] = '\0';
		logstore_write("cixd", "info", "wireless: sent regulatory domain %s (ADR-0354)", alpha2);
		return;
	}
	snprintf(g_apply_error, sizeof(g_apply_error), "%s", strerror(errno));
	logstore_write("cixd", "warn", "wireless: the kernel refused regulatory domain %s: %s",
	                alpha2, g_apply_error);
}

void wireless_init(const char *path)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;

	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	g_country[0] = '\0';
	g_apply_error[0] = '\0';
	if (persist_read_file(path, &buf, &len) == 0 && buf != NULL) {
		root = json_parse(buf, len);
		if (root != NULL) {
			const char *c = json_as_string(json_object_get(root, "country"));

			/* A file gone bad leaves no setting rather than refusing
			 * to boot, and says so. */
			if (wireless_country_valid(c))
				snprintf(g_country, sizeof(g_country), "%s", c);
			else if (c != NULL)
				logstore_write("cixd", "warn",
				                "wireless: ignoring an invalid country \"%s\" in %s", c,
				                path);
			json_free(root);
		}
	}
	free(buf);
	/*
	 * Nothing is sent when nothing is configured. The kernel starts on
	 * the world domain by itself, and a host that never asked for a
	 * country must not have one written for it -- a test daemon in a
	 * user-namespaced build container included, which would be refused.
	 */
	if (g_country[0] != '\0')
		apply();
}

enum wireless_error wireless_set(const char *country)
{
	char previous[sizeof(g_country)];

	if (country != NULL && !wireless_country_valid(country))
		return WIRELESS_ERR_INVALID;
	memcpy(previous, g_country, sizeof(previous));
	snprintf(g_country, sizeof(g_country), "%s", country != NULL ? country : "");
	if (save_state() != 0) {
		memcpy(g_country, previous, sizeof(g_country));
		return WIRELESS_ERR_PERSIST_FAILED;
	}
	apply();
	return WIRELESS_OK;
}

/*
 * The name of the radio with this wiphy index, when it is on the host:
 * /sys/class/ieee80211/<name>/index holds the index. A radio a
 * container holds is in another namespace's sysfs and has no name here,
 * which the read says as null rather than guessing "phy" + index.
 */
static int phy_name_for(int wiphy, char *buf, size_t buf_size)
{
	DIR *d = opendir("/sys/class/ieee80211");
	struct dirent *ent;
	int found = 0;

	if (d == NULL)
		return 0;
	while (!found && (ent = readdir(d)) != NULL) {
		char path[PATH_MAX];
		FILE *f;
		int idx;

		if (ent->d_name[0] == '.')
			continue;
		if (snprintf(path, sizeof(path), "/sys/class/ieee80211/%s/index", ent->d_name) >=
		    (int)sizeof(path))
			continue;
		f = fopen(path, "r");
		if (f == NULL)
			continue;
		if (fscanf(f, "%d", &idx) == 1 && idx == wiphy) {
			snprintf(buf, buf_size, "%s", ent->d_name);
			found = 1;
		}
		fclose(f);
	}
	closedir(d);
	return found;
}

void wireless_write_json(struct json_writer *w)
{
	struct nl80211_regdom doms[WIRELESS_DOMAINS_MAX];
	int n, i, host = -1;

	n = nl80211_reg_list(doms, WIRELESS_DOMAINS_MAX);
	for (i = 0; i < n; i++)
		if (doms[i].wiphy < 0) {
			host = i;
			break;
		}

	jw_obj_open(w);
	jw_key(w, "country");
	if (g_country[0] != '\0')
		jw_str(w, g_country);
	else
		jw_null(w);
	jw_key(w, "apply_error");
	if (g_apply_error[0] != '\0')
		jw_str(w, g_apply_error);
	else
		jw_null(w);
	jw_key(w, "kernel");
	jw_obj_open(w);
	jw_key(w, "country");
	if (host >= 0 && doms[host].country[0] != '\0')
		jw_str(w, doms[host].country);
	else
		jw_null(w);
	jw_obj_close(w);
	/* Only a radio with a domain of its own: every other one, a
	 * container's included, follows the host's (ADR-0354). */
	jw_key(w, "radios");
	jw_arr_open(w);
	for (i = 0; i < n; i++) {
		char phy[64];

		if (doms[i].wiphy < 0)
			continue;
		jw_obj_open(w);
		jw_key(w, "wiphy");
		jw_int(w, doms[i].wiphy);
		jw_key(w, "phy");
		if (phy_name_for(doms[i].wiphy, phy, sizeof(phy)))
			jw_str(w, phy);
		else
			jw_null(w);
		jw_key(w, "country");
		if (doms[i].country[0] != '\0')
			jw_str(w, doms[i].country);
		else
			jw_null(w);
		jw_key(w, "source");
		jw_str(w, doms[i].self_managed ? "self-managed" : "driver");
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}
