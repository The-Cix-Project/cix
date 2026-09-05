#include "containerdef.h"
#include "diskrole.h"
#include "json.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct container_def g_defs[CONTAINERDEF_MAX];
static char g_state_path[PATH_MAX];

struct container_def *containerdef_find(const char *name)
{
	int i;

	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		if (g_defs[i].in_use && strcmp(g_defs[i].name, name) == 0)
			return &g_defs[i];
	}
	return NULL;
}

static int save_state(void)
{
	struct json_writer w;
	int rc, i, j;

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		struct container_def *d = &g_defs[i];

		if (!d->in_use)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, d->name);
		jw_key(&w, "depends_on");
		jw_arr_open(&w);
		for (j = 0; j < d->depends_on_count; j++)
			jw_str(&w, d->depends_on[j]);
		jw_arr_close(&w);
		jw_key(&w, "readiness");
		if (d->has_readiness) {
			jw_obj_open(&w);
			jw_key(&w, "tcp_port");
			jw_int(&w, d->readiness_tcp_port);
			jw_key(&w, "timeout_seconds");
			jw_int(&w, d->readiness_timeout_seconds);
			jw_obj_close(&w);
		} else {
			jw_null(&w);
		}
		jw_key(&w, "restart_policy");
		jw_str(&w, d->restart_policy);
		jw_key(&w, "restart_delay_seconds");
		jw_int(&w, d->restart_delay_seconds);
		jw_key(&w, "stopped");
		jw_bool(&w, d->stopped);
		jw_key(&w, "follow_rolling");
		jw_bool(&w, d->follow_rolling);
		jw_key(&w, "follow_rolling_jitter_seconds");
		if (d->has_follow_rolling_jitter)
			jw_int(&w, d->follow_rolling_jitter_seconds);
		else
			jw_null(&w);
		jw_key(&w, "body");
		jw_str(&w, d->body);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int containerdef_add(const char *name, const char *body, size_t body_len,
                      const char depends_on[][REGISTRY_NAME_MAX], int depends_on_count,
                      int has_readiness, int readiness_tcp_port, int readiness_timeout_seconds,
                      const char *restart_policy, int restart_delay_seconds, int follow_rolling,
                      int has_follow_rolling_jitter, int follow_rolling_jitter_seconds)
{
	struct container_def *d = containerdef_find(name);
	int i;

	if (d == NULL) {
		int slot = -1;

		for (i = 0; i < CONTAINERDEF_MAX; i++) {
			if (!g_defs[i].in_use) {
				slot = i;
				break;
			}
		}
		if (slot < 0)
			return -1;
		d = &g_defs[slot];
	} else {
		free(d->body);
	}

	memset(d->name, 0, sizeof(d->name));
	strncpy(d->name, name, sizeof(d->name) - 1);

	d->body = malloc(body_len + 1);
	if (d->body == NULL)
		return -1;
	memcpy(d->body, body, body_len);
	d->body[body_len] = '\0';
	d->body_len = body_len;

	d->depends_on_count = depends_on_count;
	for (i = 0; i < depends_on_count; i++) {
		memset(d->depends_on[i], 0, sizeof(d->depends_on[i]));
		strncpy(d->depends_on[i], depends_on[i], sizeof(d->depends_on[i]) - 1);
	}

	d->has_readiness = has_readiness;
	d->readiness_tcp_port = readiness_tcp_port;
	d->readiness_timeout_seconds = readiness_timeout_seconds;

	snprintf(d->restart_policy, sizeof(d->restart_policy), "%s", restart_policy);
	d->restart_delay_seconds = restart_delay_seconds;
	d->stopped = 0;              /* a fresh create/redefine is definitionally not stopped */
	d->consecutive_failures = 0; /* ...and not backed off either */
	d->follow_rolling = follow_rolling;
	d->has_follow_rolling_jitter = has_follow_rolling_jitter;
	d->follow_rolling_jitter_seconds = follow_rolling_jitter_seconds;

	d->in_use = 1;

	return save_state();
}

int containerdef_set_body(const char *name, const char *body, size_t body_len)
{
	struct container_def *d = containerdef_find(name);
	char *copy;

	if (d == NULL)
		return -1;

	copy = malloc(body_len + 1);
	if (copy == NULL)
		return -1;
	memcpy(copy, body, body_len);
	copy[body_len] = '\0';

	free(d->body);
	d->body = copy;
	d->body_len = body_len;

	return save_state();
}

int containerdef_remove(const char *name)
{
	struct container_def *d = containerdef_find(name);

	if (d == NULL)
		return 0;
	free(d->body);
	memset(d, 0, sizeof(*d));
	return save_state();
}

int containerdef_set_stopped(const char *name, int stopped)
{
	struct container_def *d = containerdef_find(name);

	if (d == NULL)
		return 0;
	d->stopped = stopped;
	return save_state();
}

/*
 * Recursive DFS, post-order: name's own depends_on (each recursed
 * into first) land in out_order before name itself -- the same shape
 * pkg.c's own resolve_chain() already established for pkg_depends,
 * applied here to depends_on instead. done[] dedupes a name already
 * resolved via another branch (a legitimate diamond, not a cycle);
 * visiting[] (the current DFS path) catches a genuine cycle. Returns
 * 0 if name was resolved (already done, or newly added to out_order),
 * -1 if it couldn't be (unknown definition, or a cycle) -- logged
 * here, not returned as a message, since this runs at boot/
 * reconciliation time with no single HTTP response to attach it to.
 */
static int visit(const char *name, char out_order[][REGISTRY_NAME_MAX], int *count,
                  char visiting[][REGISTRY_NAME_MAX], int *visiting_count,
                  char done[][REGISTRY_NAME_MAX], int *done_count)
{
	struct container_def *def;
	int i;

	for (i = 0; i < *done_count; i++) {
		if (strcmp(done[i], name) == 0)
			return 0;
	}
	for (i = 0; i < *visiting_count; i++) {
		if (strcmp(visiting[i], name) == 0) {
			fprintf(stderr,
			        "containerdef: circular depends_on involving '%s' -- skipped\n", name);
			return -1;
		}
	}

	def = containerdef_find(name);
	if (def == NULL) {
		fprintf(stderr, "containerdef: '%s' depends_on an unknown definition -- skipped\n",
		        name);
		return -1;
	}

	strncpy(visiting[*visiting_count], name, REGISTRY_NAME_MAX - 1);
	visiting[*visiting_count][REGISTRY_NAME_MAX - 1] = '\0';
	(*visiting_count)++;

	for (i = 0; i < def->depends_on_count; i++) {
		if (visit(def->depends_on[i], out_order, count, visiting, visiting_count, done,
		          done_count) != 0) {
			(*visiting_count)--;
			return -1;
		}
	}

	(*visiting_count)--;

	strncpy(done[*done_count], name, REGISTRY_NAME_MAX - 1);
	done[*done_count][REGISTRY_NAME_MAX - 1] = '\0';
	(*done_count)++;

	strncpy(out_order[*count], name, REGISTRY_NAME_MAX - 1);
	out_order[*count][REGISTRY_NAME_MAX - 1] = '\0';
	(*count)++;
	return 0;
}

int containerdef_resolve_order(char out_order[][REGISTRY_NAME_MAX])
{
	char visiting[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	char done[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	int visiting_count = 0, done_count = 0, count = 0;
	int i;

	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		if (!g_defs[i].in_use)
			continue;
		/* Return value ignored -- a failure was already logged by
		 * visit() itself, and simply means this definition (and
		 * whichever of its own dependents already recursed into it
		 * this pass) isn't in out_order; the loop just moves on. */
		visit(g_defs[i].name, out_order, &count, visiting, &visiting_count, done, &done_count);
	}
	return count;
}

/*
 * Issue #88: echo a definition's own declared volumes back out.
 *
 * Sourced from the persisted request body rather than mirrored onto
 * either struct, for the same reason "image" already is: the body IS
 * the definition, and a second copy of a field is a second thing that
 * can disagree with it. Shared by both readers -- registry.c for a live
 * container and write_stopped_def_json_one() below for a stopped one --
 * so the two can never drift into rendering the same field differently.
 *
 * `root` is the caller's already-parsed body (NULL is fine, and simply
 * writes an empty array) so neither caller pays for a second parse of a
 * body it has already parsed.
 */
void containerdef_write_json_volumes(const struct json_value *root, struct json_writer *w)
{
	const struct json_value *jvols;
	size_t i;

	jw_key(w, "volumes");
	jw_arr_open(w);
	jvols = root != NULL ? json_object_get(root, "volumes") : NULL;
	if (jvols != NULL && jvols->type == JSON_ARRAY) {
		for (i = 0; i < jvols->u.array.count; i++) {
			const struct json_value *item = jvols->u.array.items[i];
			const char *name = json_as_string(json_object_get(item, "name"));
			const char *path = json_as_string(json_object_get(item, "path"));
			const struct json_value *jro = json_object_get(item, "read_only");

			if (name == NULL || path == NULL)
				continue;
			jw_obj_open(w);
			jw_key(w, "name");
			jw_str(w, name);
			jw_key(w, "path");
			jw_str(w, path);
			jw_key(w, "read_only");
			jw_bool(w, jro != NULL && jro->type == JSON_BOOL && jro->u.boolean);
			jw_obj_close(w);
		}
	}
	jw_arr_close(w);
}

static void write_stopped_def_json_one(struct container_def *d, struct json_writer *w)
{
	struct json_value *root;
	const char *image;
	int j;

	root = json_parse(d->body, d->body_len);
	image = root != NULL ? json_as_string(json_object_get(root, "image")) : NULL;

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, d->name);
	jw_key(w, "image");
	jw_str(w, image != NULL ? image : "");
	jw_key(w, "status");
	jw_str(w, "stopped");
	jw_key(w, "paused");
	jw_bool(w, 0);
	jw_key(w, "pid");
	jw_null(w);
	jw_key(w, "exit_status");
	jw_null(w);
	jw_key(w, "term_signal"); /* schema parity with a live entry (issue #78) */
	jw_null(w);
	jw_key(w, "networks");
	jw_arr_open(w);
	jw_arr_close(w);
	jw_key(w, "ip_forward");
	jw_bool(w, 0);
	jw_key(w, "devices");
	jw_arr_open(w);
	jw_arr_close(w);
	jw_key(w, "interfaces");
	jw_arr_open(w);
	jw_arr_close(w);
	jw_key(w, "files");
	jw_arr_open(w);
	jw_arr_close(w);
	jw_key(w, "sysctls");
	jw_obj_open(w);
	jw_obj_close(w);
	jw_key(w, "cmd");
	jw_arr_open(w);
	if (root != NULL) {
		const struct json_value *jcmd = json_object_get(root, "cmd");

		if (jcmd != NULL && jcmd->type == JSON_ARRAY) {
			size_t k;

			for (k = 0; k < jcmd->u.array.count; k++) {
				const char *s = json_as_string(jcmd->u.array.items[k]);

				if (s != NULL)
					jw_str(w, s);
			}
		}
	}
	jw_arr_close(w);
	containerdef_write_json_volumes(root, w);
	jw_key(w, "restart");
	jw_str(w, d->restart_policy);
	jw_key(w, "restart_delay_seconds");
	jw_int(w, d->restart_delay_seconds);
	jw_key(w, "follow_rolling");
	jw_bool(w, d->follow_rolling);
	jw_key(w, "follow_rolling_jitter_seconds");
	if (d->has_follow_rolling_jitter)
		jw_int(w, d->follow_rolling_jitter_seconds);
	else
		jw_null(w);
	/*
	 * The definition's OWN flag, not a constant (#268).
	 *
	 * This wrote 1 unconditionally, on the reading that an entry
	 * appearing here is by definition not running. But "stopped" does
	 * not mean "not running" -- it means an operator stopped it. It is
	 * set only by POST .../stop and cleared by containerdef_add(), and
	 * nothing in the crash path touches it.
	 *
	 * So a container that FAILED to start reported the same value as
	 * one deliberately stopped, and the one field that tells those
	 * apart was the one field being overwritten. That is precisely the
	 * distinction needed to answer "is this box in the state it should
	 * be in" without calling an operator's own deliberate stop a fault
	 * -- and with a constant here, no client could ask it at all.
	 *
	 * The "status" key above already says "stopped" for every entry in
	 * this function, so the constant was not even carrying information
	 * a caller lacked. Verified live on 192.168.15.95: a container
	 * whose execve fails reports status "stopped" with nothing having
	 * called stop.
	 */
	jw_key(w, "stopped");
	jw_bool(w, d->stopped);
	jw_key(w, "depends_on");
	jw_arr_open(w);
	for (j = 0; j < d->depends_on_count; j++)
		jw_str(w, d->depends_on[j]);
	jw_arr_close(w);
	jw_key(w, "readiness");
	if (d->has_readiness) {
		jw_obj_open(w);
		jw_key(w, "tcp_port");
		jw_int(w, d->readiness_tcp_port);
		jw_key(w, "timeout_seconds");
		jw_int(w, d->readiness_timeout_seconds);
		jw_obj_close(w);
	} else {
		jw_null(w);
	}
	jw_obj_close(w);

	json_free(root);
}

void containerdef_write_json_inactive_list(struct json_writer *w,
                                           int (*is_live)(const char *name))
{
	int i;

	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		if (g_defs[i].in_use && !is_live(g_defs[i].name))
			write_stopped_def_json_one(&g_defs[i], w);
	}
}

int containerdef_write_json_inactive_one(const char *name, struct json_writer *w)
{
	struct container_def *d = containerdef_find(name);

	if (d == NULL)
		return 0;
	write_stopped_def_json_one(d, w);
	return 1;
}

static int parse_persisted_entry(const struct json_value *item, struct container_def *slot)
{
	const char *name = json_as_string(json_object_get(item, "name"));
	const char *body = json_as_string(json_object_get(item, "body"));
	const struct json_value *jdeps = json_object_get(item, "depends_on");
	const struct json_value *jready = json_object_get(item, "readiness");
	const struct json_value *jrestart_policy = json_object_get(item, "restart_policy");
	const struct json_value *jrestart_delay = json_object_get(item, "restart_delay_seconds");
	const struct json_value *jstopped = json_object_get(item, "stopped");
	const struct json_value *jfollow_rolling = json_object_get(item, "follow_rolling");
	const struct json_value *jfollow_rolling_jitter =
	    json_object_get(item, "follow_rolling_jitter_seconds");
	size_t i;

	if (name == NULL || name[0] == '\0' || strlen(name) >= REGISTRY_NAME_MAX || body == NULL)
		return -1;

	memset(slot, 0, sizeof(*slot));
	strncpy(slot->name, name, sizeof(slot->name) - 1);

	slot->body_len = strlen(body);
	slot->body = malloc(slot->body_len + 1);
	if (slot->body == NULL)
		return -1;
	memcpy(slot->body, body, slot->body_len + 1); /* includes the NUL */

	if (jdeps != NULL) {
		if (jdeps->type != JSON_ARRAY) {
			free(slot->body);
			return -1;
		}
		for (i = 0; i < jdeps->u.array.count && slot->depends_on_count < CONTAINERDEF_MAX_DEPENDS;
		     i++) {
			const char *dep = json_as_string(jdeps->u.array.items[i]);

			if (dep == NULL) {
				free(slot->body);
				return -1;
			}
			strncpy(slot->depends_on[slot->depends_on_count], dep,
			        sizeof(slot->depends_on[slot->depends_on_count]) - 1);
			slot->depends_on_count++;
		}
	}

	if (jready != NULL && jready->type == JSON_OBJECT) {
		const struct json_value *jport = json_object_get(jready, "tcp_port");
		const struct json_value *jtimeout = json_object_get(jready, "timeout_seconds");

		if (jport == NULL) {
			free(slot->body);
			return -1;
		}
		slot->has_readiness = 1;
		slot->readiness_tcp_port = (int)json_as_number(jport);
		slot->readiness_timeout_seconds =
		    jtimeout != NULL ? (int)json_as_number(jtimeout) : 30;
	}

	if (jrestart_policy != NULL) {
		const char *rp = json_as_string(jrestart_policy);

		if (rp == NULL || (strcmp(rp, "no") != 0 && strcmp(rp, "always") != 0 &&
		                    strcmp(rp, "on-failure") != 0 && strcmp(rp, "unless-stopped") != 0)) {
			/* A value we ourselves wrote must be one of these four --
			 * a present-but-invalid value is real corruption, same
			 * posture as every other field here. "no" became a real,
			 * persisted policy with ADR-0181 (issue #73: every container
			 * is persisted; restart governs auto-restart only). Before
			 * that it could never appear here, because a "no" container
			 * had no definition at all -- so this reader rejected it as
			 * corruption. Missing this one spot when the writer changed
			 * meant a daemon that had created any default container
			 * refused to load its own state file on the next boot
			 * ("invalid entry at index 0") and never came back up. */
			free(slot->body);
			return -1;
		}
		snprintf(slot->restart_policy, sizeof(slot->restart_policy), "%s", rp);
	} else {
		/* Backward compat: a file from Phase 13 parts 1-2 predates this
		 * field and only ever encoded restart:"always" implicitly, by
		 * the definition existing at all. */
		snprintf(slot->restart_policy, sizeof(slot->restart_policy), "always");
	}
	slot->restart_delay_seconds = jrestart_delay != NULL
	                                   ? (int)json_as_number(jrestart_delay)
	                                   : CONTAINERDEF_DEFAULT_RESTART_DELAY_SECONDS;
	slot->stopped = (jstopped != NULL && jstopped->type == JSON_BOOL && jstopped->u.boolean);
	/* Absent means a file predating Part 5 -- naturally defaults to 0
	 * (never followed rolling updates before this feature existed). */
	slot->follow_rolling =
	    (jfollow_rolling != NULL && jfollow_rolling->type == JSON_BOOL && jfollow_rolling->u.boolean);
	/* Absent or JSON null both mean "no per-container override" -- the
	 * former predates this field, the latter is what save_state()
	 * itself always writes for an unset override, same "null means
	 * absent" shape has_readiness above uses for a nested object. */
	slot->has_follow_rolling_jitter =
	    (jfollow_rolling_jitter != NULL && jfollow_rolling_jitter->type == JSON_NUMBER);
	slot->follow_rolling_jitter_seconds =
	    slot->has_follow_rolling_jitter ? (int)json_as_number(jfollow_rolling_jitter) : 0;

	slot->in_use = 1;
	return 0;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int rc = 0;
	int count = 0;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0;

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted container definitions\n", g_state_path);
		return -1;
	}
	if (root->u.array.count > CONTAINERDEF_MAX) {
		json_free(root);
		fprintf(stderr, "%s: more definitions persisted than CONTAINERDEF_MAX\n", g_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count; i++) {
		if (parse_persisted_entry(root->u.array.items[i], &g_defs[count]) != 0) {
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
			rc = -1;
			break;
		}
		count++;
	}
	json_free(root);
	return rc;
}

void containerdef_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}


/*
 * Issue #76: a one-shot rewrite of the legacy `ldap_login` key to
 * `ldap_client` in every persisted definition.
 *
 * This is a migration, not a compatibility shim, and the difference
 * matters: the API does not accept `ldap_login` any more -- an unknown
 * field is a 400 (issue #68) -- so a definition written before the
 * rename would fail on its next start, which is a container silently
 * not coming back after a daemon restart. Converting the stored state
 * once, at load, is what a clean cut-over looks like when there is
 * already state on disk.
 *
 * Parsed and rebuilt rather than string-replaced: `ldap_login` could
 * legitimately appear inside a VALUE (a container's own cmd, an env
 * var, a staged file's content), and rewriting those would corrupt a
 * definition to save a few lines.
 */
static int migrate_ldap_login_key(void)
{
	int i, migrated = 0;

	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		struct container_def *d = &g_defs[i];
		struct json_value *root;
		struct json_writer w;
		size_t k;
		int found = 0;

		if (!d->in_use || d->body == NULL)
			continue;
		root = json_parse(d->body, d->body_len);
		if (root == NULL || root->type != JSON_OBJECT) {
			json_free(root);
			continue;
		}
		for (k = 0; k < root->u.object.count; k++)
			if (strcmp(root->u.object.keys[k], "ldap_login") == 0)
				found = 1;
		if (!found) {
			json_free(root);
			continue;
		}

		jw_init(&w);
		jw_obj_open(&w);
		for (k = 0; k < root->u.object.count; k++) {
			jw_key(&w, strcmp(root->u.object.keys[k], "ldap_login") == 0 ? "ldap_client"
			                                                             : root->u.object.keys[k]);
			jw_value(&w, root->u.object.values[k]);
		}
		jw_obj_close(&w);
		json_free(root);

		if (w.buf != NULL) {
			free(d->body);
			d->body = malloc(w.len + 1);
			if (d->body != NULL) {
				memcpy(d->body, w.buf, w.len);
				d->body[w.len] = '\0';
				d->body_len = w.len;
				migrated++;
			} else {
				d->body_len = 0;
			}
		}
		jw_free(&w);
	}
	if (migrated > 0) {
		fprintf(stderr, "containerdef: migrated %d definition(s) from ldap_login to ldap_client\n",
		        migrated);
		return save_state();
	}
	return 0;
}

/*
 * ADR-0207 phase 3/4: pin "userns": false into every persisted
 * definition that lacks the key.
 *
 * Since phase 3 the create path resolves an omitted "userns" against
 * the platform default and splices the RESOLVED value into the body
 * it persists (create_container_persisted()), so a definition written
 * by that code always carries the key. A definition without it
 * therefore predates the default flip -- and the mode it actually ran
 * with was non-userns, because before phase 3 an omitted "userns"
 * meant false unconditionally. Pinning that history in, once at load,
 * is what keeps flipping the platform default from silently swapping
 * a pre-existing container's isolation mode (and with it its whole
 * storage layout) on its first revival after an upgrade -- the exact
 * flap the creation-time pin already prevents for post-flip
 * containers.
 *
 * Parsed and rebuilt rather than string-spliced, same reasoning as
 * migrate_ldap_login_key() above: "userns" could legitimately appear
 * inside a value (a cmd, an env var, a staged file's content).
 */
static int migrate_pin_userns_absent(void)
{
	int i, migrated = 0;

	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		struct container_def *d = &g_defs[i];
		struct json_value *root;
		struct json_writer w;
		size_t k;
		int found = 0;

		if (!d->in_use || d->body == NULL)
			continue;
		root = json_parse(d->body, d->body_len);
		if (root == NULL || root->type != JSON_OBJECT) {
			json_free(root);
			continue;
		}
		for (k = 0; k < root->u.object.count; k++)
			if (strcmp(root->u.object.keys[k], "userns") == 0)
				found = 1;
		if (found) {
			json_free(root);
			continue;
		}

		jw_init(&w);
		jw_obj_open(&w);
		for (k = 0; k < root->u.object.count; k++) {
			jw_key(&w, root->u.object.keys[k]);
			jw_value(&w, root->u.object.values[k]);
		}
		jw_key(&w, "userns");
		jw_bool(&w, 0);
		jw_obj_close(&w);
		json_free(root);

		if (w.buf != NULL) {
			free(d->body);
			d->body = malloc(w.len + 1);
			if (d->body != NULL) {
				memcpy(d->body, w.buf, w.len);
				d->body[w.len] = '\0';
				d->body_len = w.len;
				migrated++;
			} else {
				d->body_len = 0;
			}
		}
		jw_free(&w);
	}
	if (migrated > 0) {
		fprintf(stderr,
		        "containerdef: pinned userns:false into %d pre-existing definition(s)\n",
		        migrated);
		return save_state();
	}
	return 0;
}

int containerdef_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >=
	    (int)sizeof(g_state_path))
		return -1;
	memset(g_defs, 0, sizeof(g_defs));
	if (load_state() != 0)
		return -1;
	/* Issue #76: convert any pre-rename state once, before anything
	 * replays a definition through the create path that would now
	 * reject the old key. */
	if (migrate_ldap_login_key() != 0)
		return -1;
	/* ADR-0207: pin pre-flip definitions to the mode they actually
	 * ran with, before autostart replays any of them against the new
	 * default. */
	return migrate_pin_userns_absent();
}

/*
 * Part 5 (ADR-0124): raw string find/replace of the "image_version"
 * value already spliced into d->body at creation time (handle_create(),
 * daemon/src/main.c) -- same "the body is already known-valid JSON,
 * only surgery is needed" posture that splice itself uses, not a full
 * JSON re-serialize (this module has no generic "write an arbitrary
 * parsed tree back out" helper, and doesn't need one just for this).
 */
int containerdef_patch_image_version(const char *name, const char *new_version)
{
	static const char key[] = "\"image_version\":\"";
	struct container_def *d = containerdef_find(name);
	char *key_pos, *val_start, *val_end, *new_body;
	size_t prefix_len, suffix_len, new_version_len, new_body_len;

	if (d == NULL)
		return -1;

	key_pos = strstr(d->body, key);
	if (key_pos == NULL)
		return -1;
	val_start = key_pos + (sizeof(key) - 1);
	val_end = strchr(val_start, '"');
	if (val_end == NULL)
		return -1;

	prefix_len = (size_t)(val_start - d->body);
	suffix_len = d->body_len - (size_t)(val_end - d->body);
	new_version_len = strlen(new_version);
	new_body_len = prefix_len + new_version_len + suffix_len;

	new_body = malloc(new_body_len + 1);
	if (new_body == NULL)
		return -1;
	memcpy(new_body, d->body, prefix_len);
	memcpy(new_body + prefix_len, new_version, new_version_len);
	memcpy(new_body + prefix_len + new_version_len, val_end, suffix_len);
	new_body[new_body_len] = '\0';

	free(d->body);
	d->body = new_body;
	d->body_len = new_body_len;

	return save_state();
}

/*
 * ADR-0142 Section 4: see this function's own doc comment in
 * containerdef.h. new_disk == NULL writes the JSON literal null;
 * otherwise a quoted disk name. Uniformly computes [prefix, value_span,
 * suffix) around the value's own byte range (whether the existing
 * value was a quoted string or a bare `null`) and splices the new
 * value text in between -- the same "know the exact byte span, cut and
 * paste around it" shape containerdef_patch_image_version() already
 * uses, generalized to a value that isn't always a quoted string.
 */
int containerdef_patch_disk(const char *name, const char *new_disk)
{
	struct container_def *d = containerdef_find(name);
	char *key_pos, *colon, *val_start, *val_end, *new_body;
	char new_value[DISKROLE_DISK_NAME_MAX + 4];
	size_t new_value_len, prefix_len, suffix_len, new_body_len;

	if (d == NULL)
		return -1;

	if (new_disk != NULL)
		snprintf(new_value, sizeof(new_value), "\"%s\"", new_disk);
	else
		snprintf(new_value, sizeof(new_value), "null");
	new_value_len = strlen(new_value);

	key_pos = strstr(d->body, "\"disk\"");
	if (key_pos != NULL) {
		colon = strchr(key_pos, ':');
		if (colon == NULL)
			return -1;
		val_start = colon + 1;
		while (*val_start == ' ' || *val_start == '\t')
			val_start++;
		if (*val_start == '"') {
			val_end = strchr(val_start + 1, '"');
			if (val_end == NULL)
				return -1;
			val_end++; /* past the closing quote */
		} else {
			val_end = val_start;
			while (*val_end != ',' && *val_end != '}' && *val_end != '\0' && *val_end != ' ' &&
			       *val_end != '\t' && *val_end != '\n' && *val_end != '\r')
				val_end++;
		}

		prefix_len = (size_t)(val_start - d->body);
		suffix_len = d->body_len - (size_t)(val_end - d->body);
		new_body_len = prefix_len + new_value_len + suffix_len;

		new_body = malloc(new_body_len + 1);
		if (new_body == NULL)
			return -1;
		memcpy(new_body, d->body, prefix_len);
		memcpy(new_body + prefix_len, new_value, new_value_len);
		memcpy(new_body + prefix_len + new_value_len, val_end, suffix_len);
		new_body[new_body_len] = '\0';
	} else {
		size_t trim = d->body_len;
		int n;

		while (trim > 0 && (d->body[trim - 1] == ' ' || d->body[trim - 1] == '\t' ||
		                     d->body[trim - 1] == '\n' || d->body[trim - 1] == '\r'))
			trim--;
		if (trim == 0 || d->body[trim - 1] != '}')
			return -1;

		new_body = malloc(trim + new_value_len + 32);
		if (new_body == NULL)
			return -1;
		memcpy(new_body, d->body, trim - 1);
		n = snprintf(new_body + (trim - 1), new_value_len + 32, ",\"disk\":%s}", new_value);
		new_body_len = (trim - 1) + (size_t)n;
	}

	free(d->body);
	d->body = new_body;
	d->body_len = new_body_len;

	return save_state();
}

/* ---- Part 5 (ADR-0124): rolling-restart jitter window config ---- */

static char g_rolling_config_path[PATH_MAX];
static int g_jitter_window_seconds = CONTAINERDEF_JITTER_DEFAULT_SECONDS;

static int save_rolling_config(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "jitter_window_seconds");
	jw_int(&w, g_jitter_window_seconds);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_rolling_config_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

void containerdef_rolling_config_repoint(const char *new_config_path)
{
	snprintf(g_rolling_config_path, sizeof(g_rolling_config_path), "%s", new_config_path);
}

int containerdef_rolling_config_init(const char *config_path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jwindow;

	if (snprintf(g_rolling_config_path, sizeof(g_rolling_config_path), "%s", config_path) >=
	    (int)sizeof(g_rolling_config_path))
		return -1;
	g_jitter_window_seconds = CONTAINERDEF_JITTER_DEFAULT_SECONDS;

	if (persist_read_file(config_path, &buf, &len) != 0 || buf == NULL)
		return 0; /* no persisted config yet -- the default stands */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return 0;

	jwindow = json_object_get(root, "jitter_window_seconds");
	if (jwindow != NULL) {
		double v = json_as_number(jwindow);

		if (v >= 0 && v <= CONTAINERDEF_JITTER_MAX_SECONDS)
			g_jitter_window_seconds = (int)v;
	}
	json_free(root);
	return 0;
}

int containerdef_jitter_window_get(void)
{
	return g_jitter_window_seconds;
}

int containerdef_jitter_window_set(int seconds)
{
	if (seconds < 0 || seconds > CONTAINERDEF_JITTER_MAX_SECONDS)
		return -1;
	g_jitter_window_seconds = seconds;
	return save_rolling_config();
}
