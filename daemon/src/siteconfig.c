#include "siteconfig.h"
#include "dns.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_state_path[512];
static char g_site_name[SITECONFIG_NAME_MAX];
static char g_domain_suffix[SITECONFIG_NAME_MAX];

static void apply_defaults(void)
{
	g_site_name[0] = '\0';
	snprintf(g_domain_suffix, sizeof(g_domain_suffix), "internal");
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const char *site_name, *domain_suffix;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted state yet -- defaults already applied */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed persisted site config\n", g_state_path);
		return -1;
	}

	site_name = json_as_string(json_object_get(root, "site_name"));
	domain_suffix = json_as_string(json_object_get(root, "domain_suffix"));
	if (domain_suffix == NULL || !dns_name_is_valid(domain_suffix) ||
	    (site_name != NULL && site_name[0] != '\0' && !dns_name_is_valid(site_name))) {
		json_free(root);
		fprintf(stderr, "%s: invalid persisted site config\n", g_state_path);
		return -1;
	}

	snprintf(g_site_name, sizeof(g_site_name), "%s", site_name != NULL ? site_name : "");
	snprintf(g_domain_suffix, sizeof(g_domain_suffix), "%s", domain_suffix);
	json_free(root);
	return 0;
}

int siteconfig_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	apply_defaults();
	return load_state();
}

enum siteconfig_error siteconfig_set(const char *site_name, const char *domain_suffix)
{
	struct json_writer w;
	char new_site_name[SITECONFIG_NAME_MAX];
	int rc;

	if (site_name != NULL && site_name[0] != '\0' && !dns_name_is_valid(site_name))
		return SITECONFIG_ERR_INVALID_SITE_NAME;
	if (!dns_name_is_valid(domain_suffix))
		return SITECONFIG_ERR_INVALID_DOMAIN_SUFFIX;

	snprintf(new_site_name, sizeof(new_site_name), "%s", site_name != NULL ? site_name : "");

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "site_name");
	jw_str(&w, new_site_name);
	jw_key(&w, "domain_suffix");
	jw_str(&w, domain_suffix);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	if (rc != 0)
		return SITECONFIG_ERR_PERSIST_FAILED;

	snprintf(g_site_name, sizeof(g_site_name), "%s", new_site_name);
	snprintf(g_domain_suffix, sizeof(g_domain_suffix), "%s", domain_suffix);
	return SITECONFIG_OK;
}

void siteconfig_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "site_name");
	jw_str(w, g_site_name);
	jw_key(w, "domain_suffix");
	jw_str(w, g_domain_suffix);
	jw_obj_close(w);
}
