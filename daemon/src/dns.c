#include "dns.h"
#include "linux_compat.h"
#include "persist.h"
#include "registry.h"

#include <arpa/inet.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct dns_record g_records[DNS_MAX_RECORDS];
static char g_state_path[PATH_MAX];
static struct dns_server_binding g_bindings[DNS_SERVER_MAX];

int dns_name_is_valid(const char *name)
{
	size_t i;
	size_t label_len = 0;
	size_t total_len;

	if (name == NULL || name[0] == '\0')
		return 0;
	total_len = strlen(name);
	if (total_len >= DNS_NAME_MAX)
		return 0;

	for (i = 0; name[i] != '\0'; i++) {
		char c = name[i];

		if (c == '.') {
			if (label_len == 0)
				return 0; /* empty label, e.g. leading/double dot */
			label_len = 0;
			continue;
		}
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		      c == '-'))
			return 0;
		label_len++;
		if (label_len > 63)
			return 0;
	}
	if (label_len == 0)
		return 0; /* trailing dot */
	return 1;
}

static int save_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	dns_write_json_list(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int parse_persisted_entry(const struct json_value *item, struct dns_record *slot, int idx)
{
	const char *name = json_as_string(json_object_get(item, "name"));
	const char *ip = json_as_string(json_object_get(item, "ip"));
	const char *owner = json_as_string(json_object_get(item, "owner"));
	struct in_addr addr;
	int i;

	if (!dns_name_is_valid(name) || ip == NULL || inet_pton(AF_INET, ip, &addr) != 1)
		return -1;

	for (i = 0; i < idx; i++) {
		if (strcmp(g_records[i].name, name) == 0)
			return -1;
	}

	memset(slot, 0, sizeof(*slot));
	strncpy(slot->name, name, sizeof(slot->name) - 1);
	slot->ip_be = addr.s_addr;
	if (owner != NULL)
		strncpy(slot->owner_container, owner, sizeof(slot->owner_container) - 1);
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
		return 0; /* no persisted state yet -- first-ever startup */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted DNS record state\n", g_state_path);
		return -1;
	}
	if (root->u.array.count > DNS_MAX_RECORDS) {
		json_free(root);
		fprintf(stderr, "%s: more records persisted than DNS_MAX_RECORDS\n", g_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count; i++) {
		if (parse_persisted_entry(root->u.array.items[i], &g_records[count], count) != 0) {
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
			rc = -1;
			break;
		}
		count++;
	}
	json_free(root);
	return rc;
}

int dns_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;

	memset(g_records, 0, sizeof(g_records));
	memset(g_bindings, 0, sizeof(g_bindings));
	return load_state();
}

struct dns_record *dns_record_find(const char *name)
{
	int i;

	for (i = 0; i < DNS_MAX_RECORDS; i++) {
		if (g_records[i].name[0] != '\0' && strcmp(g_records[i].name, name) == 0)
			return &g_records[i];
	}
	return NULL;
}

enum dns_error dns_record_create(const char *name, uint32_t ip_be, const char *owner_container,
                                  struct dns_record **out)
{
	int i, slot = -1;
	struct dns_record *e;

	if (!dns_name_is_valid(name))
		return DNS_ERR_INVALID_NAME;
	if (dns_record_find(name) != NULL)
		return DNS_ERR_DUPLICATE;

	for (i = 0; i < DNS_MAX_RECORDS; i++) {
		if (g_records[i].name[0] == '\0') {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return DNS_ERR_FULL;

	e = &g_records[slot];
	memset(e, 0, sizeof(*e));
	strncpy(e->name, name, sizeof(e->name) - 1);
	e->ip_be = ip_be;
	if (owner_container != NULL)
		strncpy(e->owner_container, owner_container, sizeof(e->owner_container) - 1);

	if (save_state() != 0) {
		memset(e, 0, sizeof(*e));
		return DNS_ERR_PERSIST_FAILED;
	}

	dns_server_sync_all();
	*out = e;
	return DNS_OK;
}

enum dns_error dns_record_delete(const char *name)
{
	struct dns_record *e = dns_record_find(name);

	if (e == NULL)
		return DNS_ERR_NOT_FOUND;

	memset(e, 0, sizeof(*e));
	if (save_state() != 0)
		return DNS_ERR_PERSIST_FAILED;

	dns_server_sync_all();
	return DNS_OK;
}

void dns_record_forget_owner(const char *container_name)
{
	struct dns_record *e = dns_record_find(container_name);

	if (e == NULL || strcmp(e->owner_container, container_name) != 0)
		return;

	memset(e, 0, sizeof(*e));
	save_state();
	dns_server_sync_all();
}

void dns_write_json_one(const struct dns_record *rec, struct json_writer *w)
{
	struct in_addr a;
	char ipstr[INET_ADDRSTRLEN];

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, rec->name);
	a.s_addr = rec->ip_be;
	inet_ntop(AF_INET, &a, ipstr, sizeof(ipstr));
	jw_key(w, "ip");
	jw_str(w, ipstr);
	jw_key(w, "owner");
	if (rec->owner_container[0] != '\0')
		jw_str(w, rec->owner_container);
	else
		jw_null(w);
	jw_obj_close(w);
}

void dns_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < DNS_MAX_RECORDS; i++) {
		if (g_records[i].name[0] != '\0')
			dns_write_json_one(&g_records[i], w);
	}
	jw_arr_close(w);
}

int dns_write_hosts_file(const char *abs_path)
{
	/*
	 * DNS_MAX_RECORDS * (dotted-quad + space + DNS_NAME_MAX + newline)
	 * is at most ~256 * 270 =~ 69KB; sized comfortably above that so
	 * the truncation guard below is a defensive backstop that should
	 * never actually trigger, not a real, silent record-dropping path.
	 */
	char buf[131072];
	char parent[DNS_SERVER_PATH_MAX + 32];
	char *slash;
	size_t off = 0;
	int i;

	/*
	 * abs_path's parent directories aren't guaranteed to exist -- a
	 * minimal container image may have no /etc at all. mkdir -p them
	 * first (through the same /proc/<pid>/root/ path, so this still
	 * goes through the container's real overlay correctly).
	 */
	if (snprintf(parent, sizeof(parent), "%s", abs_path) >= (int)sizeof(parent))
		return -1;
	slash = strrchr(parent, '/');
	if (slash != NULL && slash != parent) {
		*slash = '\0';
		if (persist_mkdir_p(parent) != 0)
			return -1;
	}

	for (i = 0; i < DNS_MAX_RECORDS; i++) {
		struct in_addr a;
		char ipstr[INET_ADDRSTRLEN];
		int written;

		if (g_records[i].name[0] == '\0')
			continue;
		a.s_addr = g_records[i].ip_be;
		inet_ntop(AF_INET, &a, ipstr, sizeof(ipstr));
		written = snprintf(buf + off, sizeof(buf) - off, "%s %s\n", ipstr, g_records[i].name);
		if (written < 0 || (size_t)written >= sizeof(buf) - off)
			break; /* record set too large for the buffer; truncate gracefully */
		off += (size_t)written;
	}

	return persist_atomic_write(abs_path, buf, off);
}

static struct dns_server_binding *binding_find(const char *container_name)
{
	int i;

	for (i = 0; i < DNS_SERVER_MAX; i++) {
		if (g_bindings[i].container_name[0] != '\0' &&
		    strcmp(g_bindings[i].container_name, container_name) == 0)
			return &g_bindings[i];
	}
	return NULL;
}

static int hosts_path_is_valid(const char *path)
{
	return path != NULL && path[0] == '/' && strstr(path, "..") == NULL;
}

enum dns_server_error dns_server_register(const char *container_name, pid_t pid, int pidfd,
                                           const char *hosts_path)
{
	char full_path[DNS_SERVER_PATH_MAX + 32];
	int i, slot = -1;

	if (!hosts_path_is_valid(hosts_path))
		return DNS_SERVER_ERR_INVALID_PATH;
	if (binding_find(container_name) != NULL)
		return DNS_SERVER_ERR_DUPLICATE;

	for (i = 0; i < DNS_SERVER_MAX; i++) {
		if (g_bindings[i].container_name[0] == '\0') {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return DNS_SERVER_ERR_FULL;

	if (snprintf(full_path, sizeof(full_path), "/proc/%d/root%s", (int)pid, hosts_path) >=
	    (int)sizeof(full_path))
		return DNS_SERVER_ERR_INVALID_PATH;

	if (dns_write_hosts_file(full_path) != 0)
		return DNS_SERVER_ERR_WRITE_FAILED;
	sys_pidfd_send_signal(pidfd, SIGHUP);

	memset(&g_bindings[slot], 0, sizeof(g_bindings[slot]));
	strncpy(g_bindings[slot].container_name, container_name,
	        sizeof(g_bindings[slot].container_name) - 1);
	strncpy(g_bindings[slot].hosts_path, hosts_path, sizeof(g_bindings[slot].hosts_path) - 1);
	return DNS_SERVER_OK;
}

enum dns_server_error dns_server_unregister(const char *container_name)
{
	struct dns_server_binding *b = binding_find(container_name);

	if (b == NULL)
		return DNS_SERVER_ERR_NOT_FOUND;
	memset(b, 0, sizeof(*b));
	return DNS_SERVER_OK;
}

void dns_server_forget(const char *container_name)
{
	struct dns_server_binding *b = binding_find(container_name);

	if (b != NULL)
		memset(b, 0, sizeof(*b));
}

void dns_server_sync_all(void)
{
	int i;

	for (i = 0; i < DNS_SERVER_MAX; i++) {
		char full_path[DNS_SERVER_PATH_MAX + 32];
		struct registry_entry *entry;

		if (g_bindings[i].container_name[0] == '\0')
			continue;
		entry = registry_find(g_bindings[i].container_name);
		if (entry == NULL || !entry->running)
			continue;

		if (snprintf(full_path, sizeof(full_path), "/proc/%d/root%s",
		             (int)entry->handle.pid, g_bindings[i].hosts_path) >=
		    (int)sizeof(full_path))
			continue;
		if (dns_write_hosts_file(full_path) != 0)
			continue;
		sys_pidfd_send_signal(entry->handle.pidfd, SIGHUP);
	}
}

void dns_server_write_json_one(const struct dns_server_binding *binding, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "container");
	jw_str(w, binding->container_name);
	jw_key(w, "hosts_path");
	jw_str(w, binding->hosts_path);
	jw_obj_close(w);
}

void dns_server_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < DNS_SERVER_MAX; i++) {
		if (g_bindings[i].container_name[0] != '\0')
			dns_server_write_json_one(&g_bindings[i], w);
	}
	jw_arr_close(w);
}
