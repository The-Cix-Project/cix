/*
 * The running configuration document (ADR-0206).
 *
 * One ordered, complete, redacted view of everything this host is
 * configured to do, derived fresh on every call. Nothing is stored:
 * a kept rendering would be a second source of truth for every setting
 * and would drift, which is what `One Source of Truth` exists to
 * prevent.
 *
 * THE SECTION LIST IS NOT WRITTEN HERE. It comes from the
 * ConfigDocument schema in docs/api/openapi.yaml, via
 * build/generated/config_sections.h, because ADR-0206's fifth point is
 * that the schema generates the vocabulary and nothing hand-maintains a
 * second copy of it. Agreement between two hand-maintained lists is
 * exactly what drifts, and every drift this project has suffered has
 * that shape -- openapi.yaml against api/README.md, a recipe's approved
 * checksum against the artifact it approves (#145), ADR-0203 against
 * the code it described.
 *
 * So the enforcement is structural rather than a review habit: the
 * generated CIX_CONFIG_SECTIONS(X) macro expands to one table entry per
 * schema section, each naming cfg_<section>. A section added to the
 * schema with no renderer here does not compile. Adding a configurable
 * subsystem to this platform is therefore a schema edit plus a
 * renderer, and forgetting either fails the build rather than shipping
 * a document that is quietly missing a subsystem -- which is worse than
 * no document at all, since completeness is its entire value.
 *
 * ORDER IS THE CONTRACT, not presentation. A section never depends on
 * one below it: identity and resolver before networks, networks before
 * the DNS and DHCP that sit on them, images before the packages
 * installed into them, containers last because they consume all of it.
 * A rendering whose order cannot be replayed is not replayable.
 *
 * SECRETS ARE NEVER RENDERED. That is checked rather than assumed: each
 * subsystem writer reused below was read first, and the ones that hold
 * a secret already emit a set/not-set boolean instead of the value
 * (`auth_token_set`, `bind_password_set`). Any future section whose
 * writer would emit a token, key or password must redact here rather
 * than be added as-is.
 *
 * Presentation is deliberately NOT here (ADR-0205): Cisco-style section
 * text, colour and paging are cixctl's job, and the dashboard renders
 * the same document its own way. Text is never parsed back, so its
 * format is not a contract -- which is what leaves the CLI free to
 * iterate without a daemon rebuild, an A/B slot write and a reboot.
 */
#include <stddef.h>
#include <string.h>

#include "config.h"

#include "backupconfig.h"
#include "containerdef.h"
#include "daemon_config.h"
#include "dhcp.h"
#include "diskrole.h"
#include "dns.h"
#include "image.h"
#include "json.h"
#include "kmodconfig.h"
#include "ldap.h"
#include "network.h"
#include "ntp.h"
#include "pkg.h"
#include "pkgpolicy.h"
#include "pki.h"
#include "registry.h"
#include "resolv.h"
#include "siteconfig.h"
#include "storageplacement.h"
#include "swap.h"
#include "sysctlconfig.h"
#include "syslogfwd.h"
#include "volume.h"
#include "zswap.h"

#include "generated/config_sections.h"

/*
 * diskrole and swap need to know where containers live and which disk
 * holds swap; those are main.c's to know, not this file's, so the
 * containers directory arrives as an argument and swap asks
 * storageplacement. Same split diskrole_write_json_list() already has.
 */
static const char *g_containers_dir;

static void cfg_site(struct json_writer *w) { siteconfig_write_json(w); }
static void cfg_daemon(struct json_writer *w) { daemon_config_write_json(w); }
static void cfg_resolver(struct json_writer *w) { resolv_write_json(w); }
static void cfg_time(struct json_writer *w) { ntp_write_json_config(w); }
static void cfg_ntp_servers(struct json_writer *w) { ntp_server_write_json_list(w); }
static void cfg_sysctl(struct json_writer *w) { sysctlconfig_write_json_list(w); }
static void cfg_kernel_modules(struct json_writer *w) { kmodconfig_write_json_list(w); }
static void cfg_zswap(struct json_writer *w) { zswap_write_json(w); }

static void cfg_swap(struct json_writer *w)
{
	swap_write_json(w, storageplacement_get(STORAGE_KIND_SWAP));
}

static void cfg_disk_roles(struct json_writer *w)
{
	diskrole_write_json_list(w, g_containers_dir);
}

static void cfg_volumes(struct json_writer *w) { volume_write_json_list(w); }
/*
 * The one writer in this file that does not emit a value.
 * backupconfig_write_json() writes bare members ("disk", "enabled",
 * "interval_hours") for a caller to splice into an object it already
 * opened, so calling it where a value belongs produced `"backup":,` and
 * a document that would not parse at all.
 *
 * Wrapped here rather than changed there, because its existing caller
 * (GET /system/backup-config) depends on exactly that shape, and this
 * is the second caller arriving -- the cost of adding one belongs to
 * the new caller, not to the working one.
 *
 * Found by parsing the real response from a real host, which is the
 * only reason it was found at all: it compiled cleanly, every gate
 * passed, and the daemon served 1.8 MB of JSON that was malformed 914
 * bytes in.
 */
static void cfg_backup(struct json_writer *w)
{
	jw_obj_open(w);
	backupconfig_write_json(w);
	jw_obj_close(w);
}

/*
 * The one section that can fail: reading the kernel routing table needs
 * an rtnetlink socket. network_write_routes_json() returns non-zero
 * BEFORE writing anything (checked -- it opens the socket and dumps
 * before its first jw_arr_open), so an empty array is safe and the
 * document stays well-formed. A failed read renders as "no routes"
 * rather than aborting the whole document, because one unavailable
 * subsystem should not deny an operator every other section.
 */
static void cfg_routes(struct json_writer *w)
{
	if (network_write_routes_json(w) != 0) {
		jw_arr_open(w);
		jw_arr_close(w);
	}
}

static void cfg_networks(struct json_writer *w) { network_write_json_list(w); }
static void cfg_dhcp(struct json_writer *w) { dhcp_write_json(w); }
static void cfg_dhcp_servers(struct json_writer *w) { dhcp_servers_write_json(w); }

static void cfg_dns_forwarders(struct json_writer *w)
{
	char list[DNS_FORWARDERS_MAX][DNS_FORWARDER_LEN];
	int n, i;

	n = dns_forwarders_get(list, DNS_FORWARDERS_MAX);
	jw_arr_open(w);
	for (i = 0; i < n; i++)
		jw_str(w, list[i]);
	jw_arr_close(w);
}

static void cfg_dns_servers(struct json_writer *w) { dns_server_write_json_list(w); }
static void cfg_dns_records(struct json_writer *w) { dns_write_json_list(w); }
static void cfg_pki(struct json_writer *w) { pki_write_json_list(w); }
static void cfg_ldap(struct json_writer *w) { ldap_config_write_json(w); }
static void cfg_ldap_servers(struct json_writer *w) { ldap_server_write_json_list(w); }
static void cfg_ldap_groups(struct json_writer *w) { ldap_group_write_json_list(w); }
static void cfg_ldap_users(struct json_writer *w) { ldap_user_write_json_list(w); }
static void cfg_syslog_targets(struct json_writer *w) { syslogfwd_target_write_json_list(w); }
static void cfg_package_repo(struct json_writer *w) { pkg_repo_write_json_config(w); }
static void cfg_package_artifacts(struct json_writer *w) { pkg_artifact_write_json_config(w); }
static void cfg_package_policies(struct json_writer *w) { pkgpolicy_write_json(w); }
static void cfg_packages(struct json_writer *w) { pkg_write_json_config(w); }
static void cfg_images(struct json_writer *w) { image_write_json_list(w); }
static void cfg_image_recipes(struct json_writer *w) { image_recipe_write_json_list(w); }
static void cfg_container_recipes(struct json_writer *w) { container_recipe_write_json_list(w); }
/*
 * #426: without the platform's own build containers. A running-config
 * document describes configuration, and a __pkgbuild-<n> is a
 * transient job the daemon created for itself -- it is gone by the
 * time anyone replays this, so including it only ever misled.
 */
static void cfg_containers(struct json_writer *w) { registry_write_json_list(w, 0, NULL); }

struct config_section {
	const char *name;
	enum config_kind kind;
	const char *key;
	enum config_apply_mode apply_mode;
	void (*render)(struct json_writer *w);
};

/*
 * The table, generated. Every entry names cfg_<section>, so a schema
 * section without a renderer is an undeclared-identifier error here,
 * with the section's own name in the message. Since ADR-0292 each
 * entry also carries how the section may be written -- read from the
 * same schema property, so the differ and the applier cannot be
 * working from a different idea of a section's shape than the renderer.
 */
#define X(name, kind, key, mode) { #name, kind, key, mode, cfg_##name },
static const struct config_section g_sections[] = { CIX_CONFIG_SECTIONS(X) };
#undef X

/* The generator counts the schema; this counts what was built from it.
 * They disagree only if the macro and the table drift, which cannot
 * happen while both come from the same expansion -- asserted anyway,
 * because a check that can never fire costs nothing and a wrong one
 * would be silent. */
typedef char config_section_count_matches
    [((int)(sizeof(g_sections) / sizeof(g_sections[0])) == CIX_CONFIG_SECTION_COUNT) ? 1 : -1];

void config_write_document(struct json_writer *w, const char *containers_dir)
{
	int i;

	g_containers_dir = containers_dir;
	jw_obj_open(w);
	for (i = 0; i < CIX_CONFIG_SECTION_COUNT; i++) {
		jw_key(w, g_sections[i].name);
		g_sections[i].render(w);
	}
	jw_obj_close(w);
}

int config_section_count(void)
{
	return CIX_CONFIG_SECTION_COUNT;
}

int config_section_index(const char *name)
{
	int i;

	for (i = 0; i < CIX_CONFIG_SECTION_COUNT; i++) {
		if (strcmp(g_sections[i].name, name) == 0)
			return i;
	}
	return -1;
}

const char *config_section_name(int i)
{
	return g_sections[i].name;
}

enum config_kind config_section_kind(int i)
{
	return g_sections[i].kind;
}

const char *config_section_key(int i)
{
	return g_sections[i].key;
}

enum config_apply_mode config_section_apply_mode(int i)
{
	return g_sections[i].apply_mode;
}

void config_render_section(int i, const char *containers_dir, struct json_writer *w)
{
	g_containers_dir = containers_dir;
	g_sections[i].render(w);
}
