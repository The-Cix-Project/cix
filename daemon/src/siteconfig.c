#include "siteconfig.h"
#include "dns.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_state_path[512];
static char g_instance_name[SITECONFIG_NAME_MAX];
static char g_site_name[SITECONFIG_NAME_MAX];
static char g_domain_suffix[SITECONFIG_NAME_MAX];

static void apply_defaults(void)
{
	snprintf(g_instance_name, sizeof(g_instance_name), "cix");
	g_site_name[0] = '\0';
	snprintf(g_domain_suffix, sizeof(g_domain_suffix), "internal");
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const char *instance_name, *site_name, *domain_suffix;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted state yet -- defaults already applied */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		/* A malformed/truncated/empty persisted file is recoverable --
		 * apply_defaults() already ran before load_state() was called,
		 * so falling through and returning success here just means
		 * "boot with defaults," never a fatal error. cixd runs as
		 * real PID 1 on an installed system (main()'s own init sequence
		 * treats any siteconfig_init() failure as fatal, returning 1
		 * straight out of main -- which is a kernel panic there, not an
		 * ordinary process exit) -- confirmed the hard way: a bad
		 * restore wrote an empty site_config.json and took the whole
		 * box down with "Attempted to kill init!" rather than just
		 * losing a site-identity setting an operator can trivially
		 * re-set via PUT /system/site. No corrupted config file should
		 * ever be able to do that. */
		fprintf(stderr, "%s: malformed persisted site config, using defaults\n", g_state_path);
		return 0;
	}

	instance_name = json_as_string(json_object_get(root, "instance_name"));
	site_name = json_as_string(json_object_get(root, "site_name"));
	domain_suffix = json_as_string(json_object_get(root, "domain_suffix"));
	/* instance_name absent -- a file persisted before this field existed --
	 * is not an error, the default already applied by apply_defaults()
	 * stands; present but invalid is -- same non-fatal posture as the
	 * malformed-JSON case above, for the same reason. */
	if ((instance_name != NULL && !dns_name_is_valid(instance_name)) ||
	    domain_suffix == NULL || !dns_name_is_valid(domain_suffix) ||
	    (site_name != NULL && site_name[0] != '\0' && !dns_name_is_valid(site_name))) {
		json_free(root);
		fprintf(stderr, "%s: invalid persisted site config, using defaults\n", g_state_path);
		return 0;
	}

	if (instance_name != NULL)
		snprintf(g_instance_name, sizeof(g_instance_name), "%s", instance_name);
	snprintf(g_site_name, sizeof(g_site_name), "%s", site_name != NULL ? site_name : "");
	snprintf(g_domain_suffix, sizeof(g_domain_suffix), "%s", domain_suffix);
	json_free(root);
	return 0;
}

void siteconfig_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

int siteconfig_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	apply_defaults();
	return load_state();
}

enum siteconfig_error siteconfig_set(const char *instance_name, const char *site_name,
                                      const char *domain_suffix)
{
	struct json_writer w;
	char new_site_name[SITECONFIG_NAME_MAX];
	int rc;

	if (!dns_name_is_valid(instance_name))
		return SITECONFIG_ERR_INVALID_INSTANCE_NAME;
	if (site_name != NULL && site_name[0] != '\0' && !dns_name_is_valid(site_name))
		return SITECONFIG_ERR_INVALID_SITE_NAME;
	if (!dns_name_is_valid(domain_suffix))
		return SITECONFIG_ERR_INVALID_DOMAIN_SUFFIX;

	snprintf(new_site_name, sizeof(new_site_name), "%s", site_name != NULL ? site_name : "");

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "instance_name");
	jw_str(&w, instance_name);
	jw_key(&w, "site_name");
	jw_str(&w, new_site_name);
	jw_key(&w, "domain_suffix");
	jw_str(&w, domain_suffix);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	if (rc != 0)
		return SITECONFIG_ERR_PERSIST_FAILED;

	snprintf(g_instance_name, sizeof(g_instance_name), "%s", instance_name);
	snprintf(g_site_name, sizeof(g_site_name), "%s", new_site_name);
	snprintf(g_domain_suffix, sizeof(g_domain_suffix), "%s", domain_suffix);
	return SITECONFIG_OK;
}

void siteconfig_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "instance_name");
	jw_str(w, g_instance_name);
	jw_key(w, "site_name");
	jw_str(w, g_site_name);
	jw_key(w, "domain_suffix");
	jw_str(w, g_domain_suffix);
	jw_obj_close(w);
}

/*
 * This install's own name. Exposed (issue #63) because the factory
 * reset requires it typed back as confirmation -- a boolean can be sent
 * by a client that misunderstood the call; a name can only be sent by
 * something that looked it up first.
 */
const char *siteconfig_instance_name(void)
{
	return g_instance_name;
}

const char *siteconfig_domain_suffix(void)
{
	return g_domain_suffix;
}

void siteconfig_host_fqdn(char *buf, size_t bufsize)
{
	if (g_site_name[0] != '\0')
		snprintf(buf, bufsize, "%s.%s.%s", g_instance_name, g_site_name, g_domain_suffix);
	else
		snprintf(buf, bufsize, "%s.%s", g_instance_name, g_domain_suffix);
}

void siteconfig_qualify(const char *label, char *buf, size_t bufsize)
{
	/* Gated on site_name specifically, not domain_suffix alone --
	 * domain_suffix defaults to a non-empty "internal" (ADR-0046) even
	 * on a completely fresh, unconfigured install, but site_name
	 * defaults to "" ("no site tier"). Qualifying by default whenever
	 * domain_suffix merely has its own default value would mean every
	 * bare name, on every install, gets silently qualified from the
	 * very first request with zero operator action -- confirmed the
	 * hard way against this project's own test suite, which uses bare
	 * names pervasively and broke immediately under that version.
	 * site_name only becomes non-empty once an operator has genuinely
	 * opted into a site identity (PUT /system/site) -- the correct,
	 * much narrower trigger for a real behavior change, matching this
	 * project's own "no daemon-side code qualifies a name automatically"
	 * baseline (ADR-0046) staying true until an operator asks otherwise.
	 */
	if (label == NULL || strchr(label, '.') != NULL || g_site_name[0] == '\0') {
		snprintf(buf, bufsize, "%s", label != NULL ? label : "");
		return;
	}
	snprintf(buf, bufsize, "%s.%s.%s", label, g_site_name, g_domain_suffix);
}
