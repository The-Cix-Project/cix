#include "api_ldap.h"

#include "apiresp.h"
#include "apiroute.h"
#include "daemonpaths.h"
#include "http.h"
#include "json.h"
#include "ldap.h"
#include "hostauth.h"
#include "registry.h"
#include "namecheck.h"
#include "subid.h"

#include <stdio.h>
#include <string.h>

static void respond_ldap_server_error(int fd, enum ldap_server_error err)
{
	switch (err) {
	case LDAP_SERVER_ERR_INVALID_PATH:
		respond_error(fd, 400, "Bad Request", "invalid config_path");
		break;
	case LDAP_SERVER_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "this container is already registered");
		break;
	case LDAP_SERVER_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "LDAP server binding table full");
		break;
	case LDAP_SERVER_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "failed to persist LDAP server binding");
		break;
	case LDAP_SERVER_ERR_NOT_FOUND:
	default:
		respond_error(fd, 404, "Not Found", "no such LDAP server binding");
		break;
	}
}

/*
 * Mirrors handle_dns_server_create()/handle_dns_server_list()/
 * handle_dns_server_delete() exactly (task #725) -- same REST shape,
 * same "container must already exist and be running" validation.
 * Unlike DNS, registration itself never writes to the container's
 * filesystem (see ldap.h's own header comment for why) -- task #726's
 * CRUD layer resolves /proc/<pid>/root/<config_path> fresh at write
 * time instead.
 */
void handle_ldap_server_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *container_name, *config_path;
	struct registry_entry *entry;
	enum ldap_server_error serr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	container_name = json_as_string(json_object_get(root, "container"));
	config_path = json_as_string(json_object_get(root, "config_path"));

	if (container_name == NULL || config_path == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "container/config_path missing");
		return;
	}

	entry = registry_find(container_name);
	if (entry == NULL || !entry->running) {
		json_free(root);
		respond_error(fd, 404, "Not Found", "no such running container");
		return;
	}

	serr = ldap_server_register(container_name, config_path);

	if (serr != LDAP_SERVER_OK) {
		json_free(root);
		respond_ldap_server_error(fd, serr);
		return;
	}

	/* Task #726: populate the freshly-registered server's own config
	 * file from Cix's own record store, the dns_server_sync_all()
	 * analog -- a fresh/replacement glauth instance starts current
	 * instead of empty. */
	ldap_record_sync_all();

	/* container_name/config_path still point into root -- build the
	 * response before freeing it (see handle_dns_server_create()'s own
	 * identical comment). */
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container_name);
	jw_key(&w, "config_path");
	jw_str(&w, config_path);
	jw_obj_close(&w);
	json_free(root);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

void handle_ldap_server_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	ldap_server_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_ldap_server_delete(int fd, const char *name)
{
	enum ldap_server_error serr = ldap_server_unregister(name);

	if (serr != LDAP_SERVER_OK) {
		respond_ldap_server_error(fd, serr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/* ---- LDAP uid/gid allocation config (task #748) ---- */

void handle_ldap_config_get(int fd)
{
	struct json_writer w;
	char effective[600];

	jw_init(&w);
	/*
	 * ldap_config_write_json() opens the object and writes the stored
	 * fields; effective_client_uri is appended into that same object
	 * before it closes, because it is not stored anywhere -- it is what
	 * ldap_client containers are actually handed right now, after
	 * derivation from registered servers and after health filtering
	 * (issues #66, #81, #84).
	 *
	 * Worth surfacing precisely because #84 was invisible for as long
	 * as it existed: with an explicit client_uri configured, health
	 * filtering was silently inert, and nothing anywhere reported what
	 * a client would really receive. Configuration and effect are two
	 * different questions and now have two different fields.
	 */
	ldap_config_write_json_open(&w);
	jw_key(&w, "effective_client_uri");
	if (ldap_effective_client_uri(effective, sizeof(effective)))
		jw_str(&w, effective);
	else
		jw_null(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_ldap_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	int start_uid, start_gid;
	enum ldap_record_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	/* Issue #66: two independent groups of fields, each optional as a
	 * group -- the allocation floors (both-or-neither, unchanged
	 * semantics) and the client-login fields (each individually
	 * optional; absent = unchanged, "" = clear). A body touching only
	 * one group leaves the other exactly as it was. */
	{
		const struct json_value *juid = json_object_get(root, "start_uid");
		const struct json_value *jgid = json_object_get(root, "start_gid");
		const char *client_uri = json_as_string(json_object_get(root, "client_uri"));
		const char *base_dn = json_as_string(json_object_get(root, "base_dn"));
		const char *bind_dn = json_as_string(json_object_get(root, "bind_dn"));
		const char *bind_password = json_as_string(json_object_get(root, "bind_password"));
		const struct json_value *jtls = json_object_get(root, "client_tls");
		const struct json_value *jtlsport = json_object_get(root, "client_tls_port");

		if (juid != NULL || jgid != NULL) {
			start_uid = (int)json_as_number(juid);
			start_gid = (int)json_as_number(jgid);
			rerr = ldap_config_set(start_uid, start_gid);
			if (rerr != LDAP_RECORD_OK) {
				json_free(root);
				respond_error(fd, 400, "Bad Request",
				              "start_uid/start_gid must both be > 0");
				return;
			}
		}
		if (client_uri != NULL || base_dn != NULL || bind_dn != NULL ||
		    bind_password != NULL) {
			rerr = ldap_config_set_client(client_uri, base_dn, bind_dn, bind_password);
			if (rerr != LDAP_RECORD_OK) {
				json_free(root);
				respond_error(fd, 500, "Internal Server Error",
				              "failed to persist LDAP config");
				return;
			}
		}
		/* #414: a third independent group, same absent-means-unchanged
		 * rule as the two above. -1 is the "leave it" sentinel, which is
		 * why an absent field is not simply read as 0 -- that would turn
		 * TLS off on every body that did not mention it. */
		if (jtls != NULL || jtlsport != NULL) {
			int tls = jtls != NULL && jtls->type == JSON_BOOL ? (jtls->u.boolean ? 1 : 0) :
			                                                     -1;
			int tlsport = jtlsport != NULL && jtlsport->type == JSON_NUMBER ?
			                  (int)jtlsport->u.number :
			                  -1;

			rerr = ldap_config_set_client_tls(tls, tlsport);
			if (rerr != LDAP_RECORD_OK) {
				json_free(root);
				respond_error(fd, 400, "Bad Request",
				              "client_tls must be a boolean and client_tls_port a port number 1-65535");
				return;
			}
		}
	}
	json_free(root);

	jw_init(&w);
	ldap_config_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* ---- LDAP user/group CRUD (task #726) ---- */

static void respond_ldap_record_error(int fd, enum ldap_record_error err)
{
	switch (err) {
	case LDAP_RECORD_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid name");
		break;
	case LDAP_RECORD_ERR_INVALID_FIELD:
		respond_error(fd, 400, "Bad Request", "invalid uidnumber/gidnumber");
		break;
	case LDAP_RECORD_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "already exists");
		break;
	case LDAP_RECORD_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "LDAP record table full");
		break;
	case LDAP_RECORD_ERR_GROUP_NOT_FOUND:
		respond_error(fd, 400, "Bad Request",
		              "primarygroup or a secondary_groups entry does not name an existing group");
		break;
	case LDAP_RECORD_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "failed to persist LDAP record");
		break;
	case LDAP_RECORD_ERR_NOT_FOUND:
	default:
		respond_error(fd, 404, "Not Found", "no such LDAP user/group");
		break;
	}
}

void handle_ldap_group_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	const struct json_value *jgidnumber;
	int gidnumber;
	struct ldap_group *g;
	enum ldap_record_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	jgidnumber = json_object_get(root, "gidnumber");
	/* Omitted (task #748): auto-allocate from the configurable
	 * start_gid pool (GET/PUT /v1/ldap/config), same shape the
	 * container-creation LDAP hook already uses for uidnumber. */
	gidnumber = jgidnumber != NULL ? (int)json_as_number(jgidnumber) : ldap_gid_alloc();

	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}

	rerr = ldap_group_create(name, gidnumber, &g);
	json_free(root); /* g points into ldap.c's own record store, not root -- safe past here */
	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
		return;
	}

	jw_init(&w);
	ldap_group_write_json_one(g, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

void handle_ldap_group_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "groups");
	ldap_group_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_ldap_group_get_one(int fd, const char *name)
{
	struct ldap_group *g = ldap_group_find(name);
	struct json_writer w;

	if (g == NULL) {
		respond_error(fd, 404, "Not Found", "no such LDAP group");
		return;
	}
	jw_init(&w);
	ldap_group_write_json_one(g, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_ldap_group_update(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jgidnumber, *jname;
	int gidnumber;
	const char *new_name;
	struct ldap_group *g;
	enum ldap_record_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	jgidnumber = json_object_get(root, "gidnumber");
	gidnumber = jgidnumber != NULL ? (int)json_as_number(jgidnumber) : 0;
	/* ADR-0147: an explicit body "name" differing from the URL path's
	 * own {name} renames the group; omitted or identical means no
	 * rename, matching every other PUT field's own "give it to change
	 * it" convention here. */
	jname = json_object_get(root, "name");
	new_name = jname != NULL ? json_as_string(jname) : NULL;

	/*
	 * #370: changing a configured admin group's GIDNUMBER orphans every
	 * one of its members -- users carry gids, and nothing rewrites
	 * theirs when the group's own gid moves -- so the group empties,
	 * gating deactivates, and write authentication is off for the whole
	 * API while hostauth-config still names the group and the group
	 * still exists.
	 *
	 * RENAMING IS NOT GUARDED, because it is already solved: ADR-0147's
	 * hostauth_rename_admin_group() rewrites admin_groups in place
	 * before ldap_group_update() commits, precisely so a renamed admin
	 * group never drops out of gating. Guarding it here refused a
	 * working, tested feature -- caught by test_hostauth's own
	 * "admin_groups did not follow the rename" case, which is what a
	 * regression test is for.
	 */
	if (hostauth_group_is_admin_group(name)) {
		struct ldap_group *cur = ldap_group_find(name);

		if (gidnumber != 0 && cur != NULL && gidnumber != cur->gidnumber) {
			json_free(root);
			respond_error(fd, 409, "Conflict",
			              "this group is named in admin_groups -- changing its gidnumber "
			              "would orphan every member and turn off write authentication for "
			              "the whole API; move the members first");
			return;
		}
	}

	rerr = ldap_group_update(name, new_name, gidnumber, &g);
	json_free(root);
	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
		return;
	}

	jw_init(&w);
	ldap_group_write_json_one(g, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_ldap_group_delete(int fd, const char *name)
{
	enum ldap_record_error rerr;

	/*
	 * #370: deleting a group named in admin_groups turns write-gating
	 * off for the whole API, silently, leaving hostauth-config still
	 * naming it. Refused outright rather than only when it holds the
	 * last admin -- a configured admin group that does not exist is a
	 * broken configuration either way, and "you may delete it while
	 * someone else is also an admin" is a rule nobody would predict.
	 */
	if (hostauth_group_is_admin_group(name)) {
		respond_error(fd, 409, "Conflict",
		              "this group is named in admin_groups -- deleting it would turn off "
		              "write authentication for the whole API; change "
		              "PUT /v1/system/hostauth-config first");
		return;
	}
	rerr = ldap_group_delete(name);

	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * Shared by create (task #726's POST) and update (PUT): parses every
 * user field out of root, leaving *password NULL when the "password"
 * key is absent from the body at all -- ldap_user_create()/update()
 * both treat NULL as "no credential change," matching PUT's own
 * documented "omit password to keep the existing one" semantics.
 */
static void parse_ldap_user_body(const struct json_value *root, const char **name, int *uidnumber,
                                  int *has_uidnumber, int *primarygroup,
                                  int secondary_groups[LDAP_USER_MAX_SECONDARY_GROUPS],
                                  int *secondary_group_count, const char **givenname,
                                  const char **sn, const char **mail, const char **loginshell,
                                  const char **homedirectory, const char **password, int *disabled,
                                  const char **ssh_public_key, int *can_search)
{
	const struct json_value *jdisabled = json_object_get(root, "disabled");
	const struct json_value *juidnumber = json_object_get(root, "uidnumber");
	const struct json_value *jsecondary = json_object_get(root, "secondary_groups");
	const struct json_value *jcan_search = json_object_get(root, "can_search");

	*name = json_as_string(json_object_get(root, "name"));
	*uidnumber = juidnumber != NULL ? (int)json_as_number(juidnumber) : 0;
	*has_uidnumber = juidnumber != NULL;
	*primarygroup = (int)json_as_number(json_object_get(root, "primarygroup"));
	*secondary_group_count = 0;
	if (jsecondary != NULL && jsecondary->type == JSON_ARRAY) {
		size_t i;

		for (i = 0; i < jsecondary->u.array.count && (int)i < LDAP_USER_MAX_SECONDARY_GROUPS; i++)
			secondary_groups[i] = (int)json_as_number(jsecondary->u.array.items[i]);
		*secondary_group_count = (int)i;
	}
	*givenname = json_as_string(json_object_get(root, "givenname"));
	*sn = json_as_string(json_object_get(root, "sn"));
	*mail = json_as_string(json_object_get(root, "mail"));
	*loginshell = json_as_string(json_object_get(root, "loginshell"));
	*homedirectory = json_as_string(json_object_get(root, "homedirectory"));
	*password = json_as_string(json_object_get(root, "password"));
	*disabled = (jdisabled != NULL && jdisabled->type == JSON_BOOL && jdisabled->u.boolean);
	*ssh_public_key = json_as_string(json_object_get(root, "ssh_public_key"));
	*can_search = (jcan_search != NULL && jcan_search->type == JSON_BOOL && jcan_search->u.boolean);
}

void handle_ldap_user_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name, *givenname, *sn, *mail, *loginshell, *homedirectory, *password;
	const char *ssh_public_key;
	int uidnumber, has_uidnumber, primarygroup, disabled, can_search;
	int secondary_groups[LDAP_USER_MAX_SECONDARY_GROUPS], secondary_group_count;
	struct ldap_user *u;
	enum ldap_record_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	parse_ldap_user_body(root, &name, &uidnumber, &has_uidnumber, &primarygroup, secondary_groups,
	                      &secondary_group_count, &givenname, &sn, &mail, &loginshell,
	                      &homedirectory, &password, &disabled, &ssh_public_key, &can_search);
	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}
	/* Omitted (task #748): auto-allocate from the configurable
	 * start_uid pool, same allocator the container-creation LDAP hook
	 * already uses. */
	if (!has_uidnumber)
		uidnumber = ldap_uid_alloc();

	/*
	 * ADR-0144 task #838: can_search now settable at create time via
	 * the real public API, not just the internal container-provisioning
	 * path (main.c's own auto-created-account call site) -- a real,
	 * durable bind/service account (nslcd's own binddn, or a live
	 * AuthorizedKeysCommand's own search bind) needs exactly this
	 * capability and has no other legitimate way to get it. owner_
	 * container stays NULL here: that field marks accounts this
	 * daemon itself auto-provisions and tears down with a specific
	 * container, not an operator-created one.
	 */
	rerr = ldap_user_create(name, uidnumber, primarygroup, secondary_groups, secondary_group_count,
	                         givenname, sn, mail, loginshell, homedirectory, password, disabled, NULL,
	                         can_search, ssh_public_key, &u);
	json_free(root); /* u points into ldap.c's own record store, not root -- safe past here */
	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
		return;
	}

	jw_init(&w);
	ldap_user_write_json_one(u, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

void handle_ldap_user_update(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *body_name, *givenname, *sn, *mail, *loginshell, *homedirectory, *password;
	const char *ssh_public_key;
	int uidnumber, has_uidnumber, primarygroup, disabled, can_search;
	int secondary_groups[LDAP_USER_MAX_SECONDARY_GROUPS], secondary_group_count;
	struct ldap_user *u;
	enum ldap_record_error rerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	parse_ldap_user_body(root, &body_name, &uidnumber, &has_uidnumber, &primarygroup,
	                      secondary_groups, &secondary_group_count, &givenname, &sn, &mail,
	                      &loginshell, &homedirectory, &password, &disabled, &ssh_public_key,
	                      &can_search);
	/* ADR-0147: body_name differing from the URL path's own {name} now
	 * renames the user -- previously discarded entirely (comment used
	 * to read "the URL path's name is authoritative for PUT, not the
	 * body's own", true for every OTHER field but left renaming with
	 * no path at all). */
	(void)has_uidnumber; /* PUT is full-field-replacement -- an omitted uidnumber here is 0,
	                       * same pre-existing semantics as every other omittable PUT field
	                       * (e.g. givenname/sn) resetting to empty; auto-allocation is only
	                       * for create, where "I don't have an opinion" is a real, common case. */

	/*
	 * #370: a PUT is a full-record replacement, so it can disable this
	 * user or drop every admin gid from them. Doing that to the LAST
	 * admin turns write authentication off for the whole API. Judged
	 * against the record the request PROPOSES -- the stored one cannot
	 * answer this, since the question is exactly what the change does.
	 * Only the last admin is protected: while another admin remains,
	 * removing this one is an ordinary, safe administrative act.
	 */
	if (hostauth_user_is_admin(name) &&
	    !hostauth_gids_are_admin(primarygroup, secondary_groups, secondary_group_count, disabled) &&
	    hostauth_admin_user_count() <= 1) {
		json_free(root);
		respond_error(fd, 409, "Conflict",
		              "this is the only remaining admin user -- disabling it or removing it "
		              "from every admin group would turn off write authentication for the "
		              "whole API; add another admin first");
		return;
	}

	rerr = ldap_user_update(name, body_name, uidnumber, primarygroup, secondary_groups,
	                         secondary_group_count, givenname, sn, mail, loginshell, homedirectory,
	                         password, disabled, ssh_public_key, can_search, &u);
	json_free(root);
	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
		return;
	}

	jw_init(&w);
	ldap_user_write_json_one(u, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_ldap_user_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "users");
	ldap_user_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_ldap_user_get_one(int fd, const char *name)
{
	struct ldap_user *u = ldap_user_find(name);
	struct json_writer w;

	if (u == NULL) {
		respond_error(fd, 404, "Not Found", "no such LDAP user");
		return;
	}
	jw_init(&w);
	ldap_user_write_json_one(u, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_ldap_user_delete(int fd, const char *name)
{
	enum ldap_record_error rerr;

	/*
	 * #370: deleting the last admin turns write authentication off for
	 * the whole API, leaving admin_groups still naming a group that
	 * now has nobody in it. Guarded the same way the PUT above is, and
	 * for the same reason -- while another admin remains this is an
	 * ordinary operation and stays allowed.
	 */
	if (hostauth_user_is_admin(name) && hostauth_admin_user_count() <= 1) {
		respond_error(fd, 409, "Conflict",
		              "this is the only remaining admin user -- deleting it would turn off "
		              "write authentication for the whole API; add another admin first");
		return;
	}
	rerr = ldap_user_delete(name);

	if (rerr != LDAP_RECORD_OK) {
		respond_ldap_record_error(fd, rerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}
