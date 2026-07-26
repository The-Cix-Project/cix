#include "registry.h"
#include "linux_compat.h"

#include <arpa/inet.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static struct registry_entry g_entries[REGISTRY_MAX_CONTAINERS];

void registry_init(void)
{
	memset(g_entries, 0, sizeof(g_entries));
}

struct registry_entry *registry_find(const char *name)
{
	int i;

	for (i = 0; i < REGISTRY_MAX_CONTAINERS; i++) {
		if (g_entries[i].in_use && strcmp(g_entries[i].name, name) == 0)
			return &g_entries[i];
	}
	return NULL;
}

enum registry_error registry_create(const char *name, const struct container_spec *spec,
                                     uint32_t ip_be, const char *network_name,
                                     struct registry_entry **out)
{
	int i, slot = -1;
	struct registry_entry *e;

	if (registry_find(name) != NULL)
		return REGISTRY_ERR_DUPLICATE;

	for (i = 0; i < REGISTRY_MAX_CONTAINERS; i++) {
		if (!g_entries[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return REGISTRY_ERR_FULL;

	e = &g_entries[slot];
	if (container_create(spec, &e->handle) != 0)
		return REGISTRY_ERR_CREATE_FAILED;

	memset(e->name, 0, sizeof(e->name));
	strncpy(e->name, name, sizeof(e->name) - 1);
	e->running = 1;
	e->exit_status = 0;
	e->in_use = 1;
	e->reactor_conn = NULL;
	e->ip_be = ip_be;
	memset(e->network, 0, sizeof(e->network));
	if (network_name != NULL)
		strncpy(e->network, network_name, sizeof(e->network) - 1);

	*out = e;
	return REGISTRY_OK;
}

int registry_network_in_use(const char *network_name)
{
	int i;

	for (i = 0; i < REGISTRY_MAX_CONTAINERS; i++) {
		if (g_entries[i].in_use && g_entries[i].network[0] != '\0' &&
		    strcmp(g_entries[i].network, network_name) == 0)
			return 1;
	}
	return 0;
}

int registry_alloc_ip(uint32_t network_base_be, int host_min, int host_max, uint32_t *out_ip_be)
{
	uint32_t network_base_host = ntohl(network_base_be);
	int host;
	int i;

	for (host = host_min; host <= host_max; host++) {
		uint32_t candidate_be = htonl(network_base_host | (uint32_t)host);
		int taken = 0;

		for (i = 0; i < REGISTRY_MAX_CONTAINERS; i++) {
			if (g_entries[i].in_use && g_entries[i].ip_be == candidate_be) {
				taken = 1;
				break;
			}
		}
		if (!taken) {
			*out_ip_be = candidate_be;
			return 0;
		}
	}
	return -1;
}

void registry_mark_exited(struct registry_entry *entry)
{
	int status;

	if (container_wait(&entry->handle, &status) == 0) {
		entry->running = 0;
		entry->exit_status = status;
	}
}

int registry_remove(const char *name)
{
	struct registry_entry *e = registry_find(name);

	if (e == NULL)
		return -1;

	if (e->running) {
		sys_pidfd_send_signal(e->handle.pidfd, SIGKILL);
		registry_mark_exited(e);
	}

	close(e->handle.pidfd);
	close(e->handle.cgroup_fd);
	e->in_use = 0;
	return 0;
}

void registry_write_json_one(const struct registry_entry *entry, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, entry->name);
	jw_key(w, "status");
	jw_str(w, entry->running ? "running" : "exited");
	jw_key(w, "pid");
	jw_int(w, (long long)entry->handle.pid);
	jw_key(w, "exit_status");
	if (entry->running)
		jw_null(w);
	else
		jw_int(w, entry->exit_status);
	jw_key(w, "network");
	if (entry->network[0] == '\0')
		jw_null(w);
	else
		jw_str(w, entry->network);
	jw_key(w, "ip");
	if (entry->ip_be == 0) {
		jw_null(w);
	} else {
		struct in_addr a;
		char ipstr[INET_ADDRSTRLEN];

		a.s_addr = entry->ip_be;
		inet_ntop(AF_INET, &a, ipstr, sizeof(ipstr));
		jw_str(w, ipstr);
	}
	jw_obj_close(w);
}

void registry_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < REGISTRY_MAX_CONTAINERS; i++) {
		if (g_entries[i].in_use)
			registry_write_json_one(&g_entries[i], w);
	}
	jw_arr_close(w);
}
