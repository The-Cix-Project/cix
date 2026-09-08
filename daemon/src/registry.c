#include "registry.h"
#include "cixinit_table.h"
#include "containerdef.h"
#include "internal.h"
#include "linux_compat.h"
#include "logstore.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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


int registry_list_names(char out_names[][REGISTRY_NAME_MAX], int max)
{
	int i, count = 0;

	for (i = 0; i < REGISTRY_MAX_CONTAINERS && count < max; i++) {
		if (g_entries[i].in_use)
			snprintf(out_names[count++], REGISTRY_NAME_MAX, "%s", g_entries[i].name);
	}
	return count;
}

enum registry_error registry_create(const char *name, const char *image,
                                     const char *image_version, const struct container_spec *spec,
                                     const struct registry_network_attachment *nets, int net_count,
                                     int ip_forward,
                                     const struct registry_device_attachment *devices,
                                     int device_count,
                                     const char file_paths[][CONTAINER_FILE_PATH_MAX],
                                     int file_count, const char *disk_name,
                                     const char dns_server_ips[][RESOLV_IP_STRLEN],
                                     int dns_server_count,
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
	memset(e->image, 0, sizeof(e->image));
	strncpy(e->image, image, sizeof(e->image) - 1);
	memset(e->image_version, 0, sizeof(e->image_version));
	if (image_version != NULL)
		strncpy(e->image_version, image_version, sizeof(e->image_version) - 1);
	/* Issue #61: remember the lowerdir actually used, rather than leaving
	 * readers to reconstruct it from image/image_version -- a
	 * reconstruction that silently produces nothing at all for a build
	 * container (synthetic image name, no version). */
	memset(e->lowerdir, 0, sizeof(e->lowerdir));
	if (spec->ov.lowerdir != NULL)
		strncpy(e->lowerdir, spec->ov.lowerdir, sizeof(e->lowerdir) - 1);
	e->running = 1;
	e->exit_status = 0;
	e->term_signal = 0;
	e->last_exit_reason[0] = '\0';
	e->paused = 0;
	e->teardown_kind = REGISTRY_TEARDOWN_NONE;
	e->disk_quota_bytes = 0;
	e->started_at = time(NULL);
	e->in_use = 1;
	e->reactor_conn = NULL;
	e->output_fd = -1;
	e->capture_requested = 0;
	e->captured_output[0] = '\0';
	e->captured_output_len = 0;
	memset(e->nets, 0, sizeof(e->nets));
	e->net_count = net_count;
	for (i = 0; i < net_count; i++)
		e->nets[i] = nets[i];
	e->ip_forward = ip_forward;
	memset(e->devices, 0, sizeof(e->devices));
	e->device_count = device_count;
	for (i = 0; i < device_count; i++)
		e->devices[i] = devices[i];
	memset(e->interfaces, 0, sizeof(e->interfaces));
	e->interface_count = spec->interface_count;
	for (i = 0; i < spec->interface_count; i++)
		strncpy(e->interfaces[i], spec->interfaces[i], sizeof(e->interfaces[i]) - 1);
	memset(e->file_paths, 0, sizeof(e->file_paths));
	e->file_count = file_count;
	for (i = 0; i < file_count; i++)
		strncpy(e->file_paths[i], file_paths[i], sizeof(e->file_paths[i]) - 1);
	memset(e->sysctls, 0, sizeof(e->sysctls));
	e->sysctl_count = spec->sysctl_count;
	for (i = 0; i < spec->sysctl_count; i++)
		e->sysctls[i] = spec->sysctls[i];
	memset(e->cap_add, 0, sizeof(e->cap_add));
	e->cap_add_count = spec->cap_add_count;
	for (i = 0; i < spec->cap_add_count; i++)
		strncpy(e->cap_add[i], spec->cap_add[i], sizeof(e->cap_add[i]) - 1);
	memset(e->services, 0, sizeof(e->services));
	e->service_count = 0;
	e->init_control_fd = -1;
	e->init_report_fd = -1;
	e->init_up = 0;
	e->ready = 0;
	memset(e->env, 0, sizeof(e->env));
	for (i = 0; i < CONTAINER_MAX_ENV && spec->envp[i] != NULL; i++) {
		const char *entry = spec->envp[i];
		const char *eq = strchr(entry, '=');
		size_t key_len = (size_t)(eq - entry);

		if (key_len >= sizeof(e->env[i].key))
			key_len = sizeof(e->env[i].key) - 1;
		memcpy(e->env[i].key, entry, key_len);
		e->env[i].key[key_len] = '\0';
		strncpy(e->env[i].value, eq + 1, sizeof(e->env[i].value) - 1);
	}
	e->env_count = i;
	memset(e->disk_name, 0, sizeof(e->disk_name));
	if (disk_name != NULL)
		strncpy(e->disk_name, disk_name, sizeof(e->disk_name) - 1);
	memset(e->dns_server_ips, 0, sizeof(e->dns_server_ips));
	e->dns_server_count = dns_server_count;
	for (i = 0; i < dns_server_count; i++)
		strncpy(e->dns_server_ips[i], dns_server_ips[i], sizeof(e->dns_server_ips[i]) - 1);

	*out = e;
	return REGISTRY_OK;
}

int registry_network_in_use(const char *network_name)
{
	int i, j;

	for (i = 0; i < REGISTRY_MAX_CONTAINERS; i++) {
		if (!g_entries[i].in_use)
			continue;
		for (j = 0; j < g_entries[i].net_count; j++) {
			if (strcmp(g_entries[i].nets[j].name, network_name) == 0)
				return 1;
		}
	}
	return 0;
}

int registry_image_in_use(const char *image)
{
	int i;

	for (i = 0; i < REGISTRY_MAX_CONTAINERS; i++) {
		if (g_entries[i].in_use && strcmp(g_entries[i].image, image) == 0)
			return 1;
	}
	return 0;
}

static int ip_in_use(uint32_t candidate_be)
{
	int i, j;

	for (i = 0; i < REGISTRY_MAX_CONTAINERS; i++) {
		if (!g_entries[i].in_use)
			continue;
		for (j = 0; j < g_entries[i].net_count; j++) {
			if (g_entries[i].nets[j].ip_be == candidate_be)
				return 1;
		}
	}
	return 0;
}

int registry_alloc_ip(uint32_t network_base_be, int host_min, int host_max, uint32_t exclude_be,
                       uint32_t *out_ip_be)
{
	uint32_t network_base_host = ntohl(network_base_be);
	int host;

	for (host = host_min; host <= host_max; host++) {
		uint32_t candidate_be = htonl(network_base_host | (uint32_t)host);

		if (candidate_be == exclude_be)
			continue;
		if (!ip_in_use(candidate_be)) {
			*out_ip_be = candidate_be;
			return 0;
		}
	}
	return -1;
}

int registry_ip_available(uint32_t candidate_be)
{
	return !ip_in_use(candidate_be);
}

int registry_ip_holder(uint32_t candidate_be, char *out_name, size_t out_name_size,
                        int *out_tearing_down)
{
	int i, j;

	if (out_tearing_down != NULL)
		*out_tearing_down = 0;
	for (i = 0; i < REGISTRY_MAX_CONTAINERS; i++) {
		if (!g_entries[i].in_use)
			continue;
		for (j = 0; j < g_entries[i].net_count; j++) {
			if (g_entries[i].nets[j].ip_be != candidate_be)
				continue;
			if (out_name != NULL && out_name_size > 0)
				snprintf(out_name, out_name_size, "%s", g_entries[i].name);
			if (out_tearing_down != NULL)
				*out_tearing_down = g_entries[i].teardown_kind != REGISTRY_TEARDOWN_NONE;
			return 1;
		}
	}
	return 0;
}

static void registry_mark_exited_with(struct registry_entry *entry, int status, int sig);

void registry_mark_exited(struct registry_entry *entry)
{
	int status, sig;

	if (container_wait(&entry->handle, &status, &sig) == 0)
		registry_mark_exited_with(entry, status, sig);
}

/* The bookkeeping half of registry_mark_exited(), kept separate from
 * the waitid() that produces the status so the two concerns stay
 * readable. */
static void registry_mark_exited_with(struct registry_entry *entry, int status, int sig)
{
	{
		char diag[256];
		ssize_t diag_len;

		entry->running = 0;
		entry->exit_status = status;
		entry->term_signal = sig;

		/*
		 * Prefer the child's own real diagnostic text (the ONLY thing
		 * that can tell "overlay mount failed" apart from "the exec'd
		 * program's own real exit code happened to land in the same
		 * numeric range," see container_decode_exit_status()'s own
		 * comment) -- fall back to the fixed category decode only when
		 * the pipe had nothing (a genuinely clean exit, or a container
		 * whose diag pipe was already consumed/unavailable).
		 */
		diag_len = container_read_diag(&entry->handle, diag, sizeof(diag));
		if (diag_len > 0) {
			snprintf(entry->last_exit_reason, sizeof(entry->last_exit_reason), "%s", diag);
		} else {
			container_decode_exit_status(status, sig, entry->last_exit_reason,
			                              sizeof(entry->last_exit_reason));
		}

		/*
		 * Severity tracks INTENT, not just the exit code (issue #79).
		 * A deliberate stop/delete SIGKILLs the container -- a nonzero
		 * (signal-9) exit that is not an error and must not read like
		 * one in a log an operator scans for real problems; the
		 * matching [audit] POST .../stop|DELETE line already records
		 * the action, so this is an info-level confirmation, not a
		 * second alarm. Only a genuinely unprompted abnormal exit
		 * (teardown_kind still NONE) stays at error level.
		 */
		if (entry->teardown_kind == REGISTRY_TEARDOWN_STOP) {
			logstore_write("cixd", "info", "container %s stopped (%s)",
			                entry->name, entry->last_exit_reason);
		} else if (entry->teardown_kind == REGISTRY_TEARDOWN_DELETE) {
			logstore_write("cixd", "info", "container %s removed (%s)",
			                entry->name, entry->last_exit_reason);
		} else if (status != 0) {
			logstore_write("cixd", "error", "container %s exited abnormally: %s",
			                entry->name, entry->last_exit_reason);
		}
	}
}

/*
 * Freezes (freeze=1) or thaws (freeze=0) e via the cgroup v2 freezer:
 * writes "1"/"0" to cgroup.freeze underneath e->handle.cgroup_fd (an
 * O_PATH dir fd from cgroup_create() -- see include/container.h;
 * openat() against an O_PATH dir fd to open a real file underneath it
 * is standard, well-defined usage, only read/write directly ON the
 * O_PATH fd itself is disallowed). Real kernel-level freeze, not
 * SIGSTOP -- uninterceptable/unignorable by the frozen process. On
 * success also updates e->paused to match. Returns 0 on success, -1
 * (errno set by whichever syscall failed) otherwise. Not static:
 * registry_remove() below needs this too (a frozen process can't be
 * killed by a normal signal -- the freezer blocks delivery -- so
 * removal must thaw first), and main.c's own POST .../pause and
 * .../unpause handlers call it directly, one mechanism either way.
 */
int registry_set_paused(struct registry_entry *e, int freeze)
{
	int freeze_fd;
	const char *val = freeze ? "1" : "0";

	freeze_fd = openat(e->handle.cgroup_fd, "cgroup.freeze", O_WRONLY);
	if (freeze_fd < 0)
		return -1;
	if (write(freeze_fd, val, 1) != 1) {
		close(freeze_fd);
		return -1;
	}
	close(freeze_fd);
	e->paused = freeze;
	return 0;
}

int registry_remove(const char *name)
{
	struct registry_entry *e = registry_find(name);

	if (e == NULL)
		return -1;

	if (e->running) {
		/*
		 * A frozen cgroup blocks signal delivery to every task in it --
		 * SIGKILL sent to a still-frozen container would queue but never
		 * actually terminate the process, leaving a hung, unkillable
		 * entry. Thaw first, UNCONDITIONALLY (ADR-0180 hardening: the
		 * old `if (e->paused)` guard trusted a flag that can in
		 * principle go stale, and a stale flag here means this exact
		 * wait blocks the whole daemon forever -- thawing an already-
		 * thawed cgroup is a harmless no-op write). Failure is still
		 * ignored (cgroup already gone/unfreezable -- the SIGKILL below
		 * is attempted exactly as before).
		 */
		registry_set_paused(e, 0);
		sys_pidfd_send_signal(e->handle.pidfd, SIGKILL);
		registry_mark_exited(e);
	}

	if (e->interface_count > 0) {
		const char *iface_ptrs[CONTAINER_MAX_INTERFACES];
		int i;

		for (i = 0; i < e->interface_count; i++)
			iface_ptrs[i] = e->interfaces[i];
		container_net_teardown_interfaces(iface_ptrs, e->interface_count,
		                                   e->handle.interfaces_netns_fd);
	}

	close(e->handle.pidfd);
	if (e->handle.bpf_prog_fd >= 0)
		close(e->handle.bpf_prog_fd);
	close(e->handle.cgroup_fd);
	registry_close_init_fds(e);
	e->in_use = 0;
	return 0;
}

/* ADR-0180 -- see registry.h's own declaration comment for the full
 * lifecycle contract this participates in. */
void registry_begin_kill(struct registry_entry *e, int teardown_kind)
{
	registry_set_paused(e, 0);
	/*
	 * ADR-0260: SIGTERM, not SIGKILL. pid 1 is cix-init, which stops
	 * the services in reverse dependency order and exits; the caller
	 * (main.c's begin_container_stop) arms the SIGKILL that follows if
	 * that does not finish within the container's grace.
	 */
	sys_pidfd_send_signal(e->handle.pidfd, SIGTERM);
	e->teardown_kind = teardown_kind;
	e->teardown_started_at = time(NULL);   /* issue #119 */
	e->teardown_stall_reported = 0;
}

int registry_network_attach(struct registry_entry *e, const struct registry_network_attachment *net)
{
	int i;

	if (e->net_count >= CONTAINER_MAX_NETWORKS)
		return -1;
	for (i = 0; i < e->net_count; i++) {
		if (strcmp(e->nets[i].name, net->name) == 0)
			return -1;
	}
	e->nets[e->net_count] = *net;
	e->net_count++;
	return 0;
}

int registry_network_detach(struct registry_entry *e, const char *network_name,
                             struct registry_network_attachment *out)
{
	int i;

	for (i = 0; i < e->net_count; i++) {
		if (strcmp(e->nets[i].name, network_name) != 0)
			continue;
		*out = e->nets[i];
		for (; i < e->net_count - 1; i++)
			e->nets[i] = e->nets[i + 1];
		e->net_count--;
		return 0;
	}
	return -1;
}

void registry_set_pending_devices(struct registry_entry *e, const char pending[][96], int count)
{
	int i;

	e->pending_device_count = 0;
	for (i = 0; i < count && i < CONTAINER_MAX_DEVICES; i++) {
		snprintf(e->pending_devices[e->pending_device_count], sizeof(e->pending_devices[0]), "%s",
		         pending[i]);
		e->pending_device_count++;
	}
}

void registry_clear_pending_device(struct registry_entry *e, const char *ref)
{
	int i;

	for (i = 0; i < e->pending_device_count; i++) {
		if (strcmp(e->pending_devices[i], ref) != 0)
			continue;
		for (; i < e->pending_device_count - 1; i++)
			snprintf(e->pending_devices[i], sizeof(e->pending_devices[0]), "%s",
			         e->pending_devices[i + 1]);
		e->pending_device_count--;
		return;
	}
}

/* Rebuilds a plain struct device_spec[] from e->devices[] -- the
 * shape container_dev_bpf_attach() itself takes, shared by both
 * registry_device_live_attach()/_detach() below so the two don't
 * duplicate this translation. */
static void devices_to_specs(const struct registry_entry *e, struct device_spec *out)
{
	int i;

	for (i = 0; i < e->device_count; i++) {
		out[i].type = e->devices[i].type;
		out[i].major = e->devices[i].major;
		out[i].minor = e->devices[i].minor;
		snprintf(out[i].dev_path, sizeof(out[i].dev_path), "%s", e->devices[i].dev_path);
	}
}

int registry_device_live_attach(struct registry_entry *e, const struct device_spec *new_device,
                                 const struct registry_device_attachment *new_attachment)
{
	struct device_spec specs[CONTAINER_MAX_DEVICES];
	int new_prog_fd;
	int old_prog_fd;
	int i;

	if (e->device_count >= CONTAINER_MAX_DEVICES) {
		errno = ENOSPC;
		return -1;
	}
	for (i = 0; i < e->device_count; i++) {
		if (strcmp(e->devices[i].id, new_attachment->id) == 0) {
			errno = EEXIST;
			return -1;
		}
	}

	devices_to_specs(e, specs);
	specs[e->device_count] = *new_device;

	if (container_dev_bpf_attach(e->handle.cgroup_fd, specs, e->device_count + 1, &new_prog_fd) != 0)
		return -1;

	old_prog_fd = e->handle.bpf_prog_fd;
	e->handle.bpf_prog_fd = new_prog_fd;
	if (old_prog_fd >= 0)
		close(old_prog_fd);

	e->devices[e->device_count] = *new_attachment;
	e->device_count++;
	return 0;
}

int registry_device_live_detach(struct registry_entry *e, const char *id,
                                 struct registry_device_attachment *out)
{
	struct device_spec specs[CONTAINER_MAX_DEVICES];
	int idx = -1;
	int i;
	int new_prog_fd;
	int old_prog_fd;
	int remaining;

	for (i = 0; i < e->device_count; i++) {
		if (strcmp(e->devices[i].id, id) == 0) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		errno = ENOENT;
		return -1;
	}

	*out = e->devices[idx];

	/* specs[] built from every OTHER currently-granted device, in
	 * e->devices[]'s own order minus idx -- the exact list the
	 * replacement program (or the real BPF_PROG_DETACH below, if
	 * that list is empty) needs to enforce. */
	remaining = 0;
	for (i = 0; i < e->device_count; i++) {
		if (i == idx)
			continue;
		specs[remaining].type = e->devices[i].type;
		specs[remaining].major = e->devices[i].major;
		specs[remaining].minor = e->devices[i].minor;
		snprintf(specs[remaining].dev_path, sizeof(specs[remaining].dev_path), "%s",
		         e->devices[i].dev_path);
		remaining++;
	}

	old_prog_fd = e->handle.bpf_prog_fd;
	if (remaining > 0) {
		if (container_dev_bpf_attach(e->handle.cgroup_fd, specs, remaining, &new_prog_fd) != 0)
			return -1;
	} else {
		/* See this function's own header comment: a live detach to
		 * zero devices needs a real, explicit revoke -- container_dev_
		 * bpf_attach()'s own "device_count == 0 -> attach nothing"
		 * shortcut would silently leave the old, now-stale program
		 * (still granting the device just removed) in effect. */
		if (old_prog_fd >= 0 && container_dev_bpf_detach(e->handle.cgroup_fd, old_prog_fd) != 0)
			return -1;
		new_prog_fd = -1;
	}

	e->handle.bpf_prog_fd = new_prog_fd;
	if (old_prog_fd >= 0)
		close(old_prog_fd);

	for (i = idx; i < e->device_count - 1; i++)
		e->devices[i] = e->devices[i + 1];
	e->device_count--;
	return 0;
}

static void registry_write_json_service(const struct registry_entry *entry, int idx,
                                        struct json_writer *w);

void registry_write_json_one(const struct registry_entry *entry, struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, entry->name);
	jw_key(w, "image");
	jw_str(w, entry->image);
	jw_key(w, "image_version");
	jw_str(w, entry->image_version);
	jw_key(w, "userns");
	jw_bool(w, entry->userns_enabled);
	jw_key(w, "status");
	/* ADR-0180: a running entry mid-async-teardown reports the
	 * teardown itself ("deleting"/"stopping"), not a misleading
	 * "running" -- the SIGKILL is already sent, the entry is gone the
	 * moment the kernel finishes; this transient state is also the
	 * operator-visible explanation for why a same-name create 409s
	 * during the window. */
	jw_str(w, !entry->running ? "exited"
	           : entry->teardown_kind == REGISTRY_TEARDOWN_DELETE ? "deleting"
	           : entry->teardown_kind == REGISTRY_TEARDOWN_STOP   ? "stopping"
	           : entry->paused                                     ? "paused"
	                                                               : "running");
	jw_key(w, "paused");
	jw_bool(w, entry->running && entry->paused);
	jw_key(w, "pid");
	jw_int(w, (long long)entry->handle.pid);
	jw_key(w, "exit_status");
	if (entry->running)
		jw_null(w);
	else
		jw_int(w, entry->exit_status);
	/*
	 * issue #78: 0 means "exited normally, exit_status is a real code";
	 * nonzero means "killed by this signal, exit_status is that signal
	 * number, not a code" -- lets a client tell a SIGKILL (9, from
	 * stop/delete) apart from a genuine `exit 9`. null while running,
	 * same as exit_status.
	 */
	jw_key(w, "term_signal");
	if (entry->running)
		jw_null(w);
	else
		jw_int(w, entry->term_signal);
	jw_key(w, "exit_reason");
	if (entry->running)
		jw_null(w);
	else
		jw_str(w, entry->last_exit_reason);
	jw_key(w, "captured_output");
	if (entry->capture_requested)
		jw_str(w, entry->captured_output);
	else
		jw_null(w);
	jw_key(w, "networks");
	jw_arr_open(w);
	for (i = 0; i < entry->net_count; i++) {
		struct in_addr a;
		char ipstr[INET_ADDRSTRLEN];

		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, entry->nets[i].name);
		a.s_addr = entry->nets[i].ip_be;
		inet_ntop(AF_INET, &a, ipstr, sizeof(ipstr));
		jw_key(w, "ip");
		jw_str(w, ipstr);
		jw_key(w, "ifname");
		jw_str(w, entry->nets[i].ifname);
		jw_key(w, "live");
		jw_bool(w, entry->nets[i].veth_host[0] != '\0');
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_key(w, "ip_forward");
	jw_bool(w, entry->ip_forward);
	jw_key(w, "devices");
	jw_arr_open(w);
	for (i = 0; i < entry->device_count; i++) {
		jw_obj_open(w);
		jw_key(w, "id");
		jw_str(w, entry->devices[i].id);
		jw_key(w, "dev_path");
		jw_str(w, entry->devices[i].dev_path);
		jw_key(w, "live");
		jw_bool(w, entry->devices[i].live);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_key(w, "pending_devices");
	jw_arr_open(w);
	for (i = 0; i < entry->pending_device_count; i++)
		jw_str(w, entry->pending_devices[i]);
	jw_arr_close(w);
	jw_key(w, "interfaces");
	jw_arr_open(w);
	for (i = 0; i < entry->interface_count; i++)
		jw_str(w, entry->interfaces[i]);
	jw_arr_close(w);
	jw_key(w, "files");
	jw_arr_open(w);
	for (i = 0; i < entry->file_count; i++)
		jw_str(w, entry->file_paths[i]);
	jw_arr_close(w);
	/*
	 * Issue #248: what this container offers as a console, in
	 * declaration order -- the first is what an attach with no
	 * selector gets. An empty array is the honest answer for a
	 * container that declares none, and is what lets a client say
	 * "no console" instead of showing a control that cannot work.
	 */
	jw_key(w, "consoles");
	jw_arr_open(w);
	for (i = 0; i < entry->console_count; i++) {
		int a;

		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, entry->consoles[i].name);
		jw_key(w, "cmd");
		jw_arr_open(w);
		for (a = 0; a < entry->consoles[i].argc; a++)
			jw_str(w, entry->consoles[i].argv[a]);
		jw_arr_close(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_key(w, "sysctls");
	jw_obj_open(w);
	for (i = 0; i < entry->sysctl_count; i++) {
		jw_key(w, entry->sysctls[i].key);
		jw_str(w, entry->sysctls[i].value);
	}
	jw_obj_close(w);
	jw_key(w, "cap_add");
	jw_arr_open(w);
	for (i = 0; i < entry->cap_add_count; i++)
		jw_str(w, entry->cap_add[i]);
	jw_arr_close(w);
	/*
	 * ADR-0260: what this container runs, in the order cix-init runs
	 * it, each entry the declaration plus the live state derived from
	 * cix-init's reports.
	 */
	jw_key(w, "ready");
	jw_bool(w, entry->ready);
	jw_key(w, "services");
	jw_arr_open(w);
	for (i = 0; i < entry->service_count; i++)
		registry_write_json_service(entry, i, w);
	jw_arr_close(w);
	jw_key(w, "env");
	jw_obj_open(w);
	for (i = 0; i < entry->env_count; i++) {
		jw_key(w, entry->env[i].key);
		jw_str(w, entry->env[i].value);
	}
	jw_obj_close(w);
	jw_key(w, "disk");
	if (entry->disk_name[0] != '\0')
		jw_str(w, entry->disk_name);
	else
		jw_null(w);
	jw_key(w, "dns_servers");
	jw_arr_open(w);
	for (i = 0; i < entry->dns_server_count; i++)
		jw_str(w, entry->dns_server_ips[i]);
	jw_arr_close(w);
	{
		/*
		 * "restart"/"depends_on" are sourced live from containerdef.c,
		 * not mirrored onto registry_entry -- that module is already
		 * the one source of truth for "should this container persist
		 * and auto-restart," and a registry_entry only exists for as
		 * long as the process itself is alive/recently-exited anyway.
		 */
		struct container_def *def = containerdef_find(entry->name);
		/*
		 * Issue #88: volumes come from the persisted body too, for the
		 * same reason -- a registry_entry carries resolved host paths,
		 * never the volume NAMES a caller actually asked for, and it is
		 * the names that make the read-back useful. Parsed here and
		 * freed below rather than mirrored onto the entry.
		 */
		struct json_value *body_root = NULL;

		if (def != NULL && def->body != NULL)
			body_root = json_parse(def->body, def->body_len);
		containerdef_write_json_volumes(body_root, w);
		json_free(body_root);

		jw_key(w, "restart");
		jw_str(w, def != NULL ? def->restart_policy : "no");
		jw_key(w, "restart_delay_seconds");
		if (def != NULL)
			jw_int(w, def->restart_delay_seconds);
		else
			jw_null(w);
		jw_key(w, "stopped");
		jw_bool(w, def != NULL && def->stopped);
		jw_key(w, "follow_rolling");
		jw_bool(w, def != NULL && def->follow_rolling);
		jw_key(w, "follow_rolling_jitter_seconds");
		if (def != NULL && def->has_follow_rolling_jitter)
			jw_int(w, def->follow_rolling_jitter_seconds);
		else
			jw_null(w);
		jw_key(w, "depends_on");
		jw_arr_open(w);
		if (def != NULL) {
			for (i = 0; i < def->depends_on_count; i++)
				jw_str(w, def->depends_on[i]);
		}
		jw_arr_close(w);
		jw_key(w, "readiness");
		if (def != NULL && def->has_readiness) {
			jw_obj_open(w);
			jw_key(w, "tcp_port");
			jw_int(w, def->readiness_tcp_port);
			jw_key(w, "timeout_seconds");
			jw_int(w, def->readiness_timeout_seconds);
			jw_obj_close(w);
		} else {
			jw_null(w);
		}
	}
	/*
	 * Resource limits, read live from the real cgroup (entry->handle.
	 * cgroup_fd stays a valid O_PATH fd for as long as this entry
	 * remains registered -- registry_remove() is the only thing that
	 * closes it, well after this) -- not mirrored from whatever was
	 * requested at creation, so this reflects what's actually
	 * enforced right now. Found missing entirely until now: creation
	 * accepted memory_max/cpu_max/pids_max, but nothing ever read them
	 * back -- confirmed live, the user could set a pkgbuild sandbox's
	 * limits but had no way to see them again afterward (ADR-0165).
	 * null means unlimited for all three, a deliberate, uniform
	 * "nothing configured" reading, not a read failure.
	 */
	{
		long long memory_max, pids_max, memory_swap_max;
		int mem_unlimited = 1, pids_unlimited = 1, swap_unlimited = 1;
		char cpu_max[64];
		int have_cpu_max = 0;

		if (cgroup_read_single_value(entry->handle.cgroup_fd, "memory.max", &memory_max,
		                              &mem_unlimited) != 0)
			mem_unlimited = 1;
		if (cgroup_read_single_value(entry->handle.cgroup_fd, "memory.swap.max", &memory_swap_max,
		                              &swap_unlimited) != 0)
			swap_unlimited = 1;
		if (cgroup_read_single_value(entry->handle.cgroup_fd, "pids.max", &pids_max,
		                              &pids_unlimited) != 0)
			pids_unlimited = 1;
		if (cgroup_read_cpu_max(entry->handle.cgroup_fd, cpu_max, sizeof(cpu_max)) == 0 &&
		    strncmp(cpu_max, "max ", 4) != 0)
			have_cpu_max = 1;

		jw_key(w, "memory_max");
		if (mem_unlimited)
			jw_null(w);
		else
			jw_int(w, memory_max);
		/* Issue #52: read back live like the rest. Zero is a real
		 * value here (this container may not swap), so it is reported
		 * as 0 rather than folded into the null that means "no limit"
		 * -- the two are opposite settings. */
		jw_key(w, "memory_swap_max");
		if (swap_unlimited)
			jw_null(w);
		else
			jw_int(w, memory_swap_max);
		jw_key(w, "cpu_max");
		if (have_cpu_max)
			jw_str(w, cpu_max);
		else
			jw_null(w);
		jw_key(w, "pids_max");
		if (pids_unlimited)
			jw_null(w);
		else
			jw_int(w, pids_max);

		/* Issue #49: cpuset read back live like the three above ("" =
		 * the kernel's own no-restriction state -> null); disk quota
		 * from the creation-time mirror (see registry.h). These two
		 * used to be completely write-only -- combined with unknown-
		 * field dropping (#68) a typo'd limit was undetectable. */
		{
			char cpuset[256];

			jw_key(w, "cpuset_cpus");
			if (cgroup_read_cpuset(entry->handle.cgroup_fd, cpuset, sizeof(cpuset)) == 0 &&
			    cpuset[0] != '\0')
				jw_str(w, cpuset);
			else
				jw_null(w);
			jw_key(w, "disk_quota_bytes");
			if (entry->disk_quota_bytes > 0)
				jw_int(w, entry->disk_quota_bytes);
			else
				jw_null(w);
		}
	}
	jw_obj_close(w);
}

/*
 * Liveness predicate handed to containerdef_write_json_inactive_list()
 * so it can tell a running definition (already emitted above from the
 * live registry) apart from a kept-but-not-running one, without
 * containerdef.c needing to know the registry exists.
 */
static int registry_name_is_live(const char *name)
{
	return registry_find(name) != NULL;
}

void registry_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < REGISTRY_MAX_CONTAINERS; i++) {
		if (g_entries[i].in_use)
			registry_write_json_one(&g_entries[i], w);
	}
	/*
	 * Inactive-but-defined containers (ADR-0045, extended by ADR-0181's
	 * persist-all) have no live entry above at all -- without this,
	 * stopping a container (or just a daemon restart, now that every
	 * container is persisted) makes a name simply vanish from this list
	 * with no way to find it again short of already knowing to POST
	 * .../start blind. containerdef.c already supplies
	 * restart/depends_on/readiness for every live entry above (see
	 * registry_write_json_one()); this is the same dependency, one
	 * direction further. We inject registry_name_is_live() so the "is
	 * this def actually running?" test is decided here, where the
	 * registry is visible -- containerdef.c stays free of any
	 * back-dependency on this module.
	 */
	containerdef_write_json_inactive_list(w, registry_name_is_live);
	jw_arr_close(w);
}

/* ------------------------------------------------------------------ */
/* ADR-0260: services.                                                 */

const char *registry_service_state_name(int state)
{
	switch (state) {
	case REGISTRY_SVC_PENDING: return "pending";
	case REGISTRY_SVC_STARTING: return "starting";
	case REGISTRY_SVC_RUNNING: return "running";
	case REGISTRY_SVC_EXITED: return "exited";
	case REGISTRY_SVC_RESTART_WAIT: return "restart-wait";
	case REGISTRY_SVC_STOPPED: return "stopped";
	case REGISTRY_SVC_FAILED: return "failed";
	default: return "?";
	}
}

int registry_service_index(const struct registry_entry *e, const char *name)
{
	int i;

	if (name == NULL)
		return -1;
	for (i = 0; i < e->service_count; i++)
		if (strcmp(e->services[i].def.name, name) == 0)
			return i;
	return -1;
}

void registry_set_services(struct registry_entry *e, const struct cixinit_table *table,
                           const int output_fds[], int control_fd, int report_fd)
{
	int i;

	memset(e->services, 0, sizeof(e->services));
	e->service_count = table->count;
	for (i = 0; i < table->count; i++) {
		e->services[i].def = table->svc[i];
		e->services[i].body_index = table->order[i];
		e->services[i].state = REGISTRY_SVC_PENDING;
		e->services[i].output_fd = output_fds != NULL ? output_fds[i] : -1;
	}
	e->init_control_fd = control_fd;
	e->init_report_fd = report_fd;
	e->init_up = 0;
	e->ready = 0;
}

/*
 * Derived readiness (ADR-0260): every service with a probe has passed
 * it and every oneshot has exited 0. A service held stopped by an
 * operator is not counted against it -- the operator asked for that --
 * and neither is a daemon whose on_exit is "stop" once it has exited.
 */
static int derive_ready(const struct registry_entry *e)
{
	int i;

	if (!e->init_up || e->service_count == 0)
		return 0;
	for (i = 0; i < e->service_count; i++) {
		const struct registry_service *s = &e->services[i];

		if (s->state == REGISTRY_SVC_STOPPED)
			continue;
		if (s->def.type == CIXINIT_TYPE_ONESHOT) {
			if (!(s->state == REGISTRY_SVC_EXITED && s->has_exit &&
			      s->exit_kind == CIXINIT_EXIT_KIND_EXITED && s->exit_value == 0))
				return 0;
			continue;
		}
		if (s->state == REGISTRY_SVC_EXITED && s->def.on_exit == CIXINIT_ON_EXIT_STOP)
			continue;
		if (s->state != REGISTRY_SVC_RUNNING)
			return 0;
	}
	return 1;
}

int registry_apply_init_report(struct registry_entry *e, const struct cixinit_report *r)
{
	struct registry_service *s;
	int was_ready = e->ready;

	if (r->service == -1) {
		if (r->event == CIXINIT_EV_UP)
			e->init_up = 1;
		e->ready = derive_ready(e);
		return e->ready != was_ready;
	}
	if (r->service < 0 || r->service >= e->service_count)
		return 0;
	s = &e->services[r->service];
	switch (r->event) {
	case CIXINIT_EV_STARTING:
		s->state = REGISTRY_SVC_STARTING;
		s->pid = 0;
		s->fail_reason = 0;
		break;
	case CIXINIT_EV_STARTED:
		s->state = REGISTRY_SVC_STARTING;
		s->pid = (pid_t)r->a;
		break;
	case CIXINIT_EV_READY:
		s->state = REGISTRY_SVC_RUNNING;
		break;
	case CIXINIT_EV_EXITED:
		s->pid = 0;
		s->has_exit = 1;
		s->exit_kind = r->a;
		s->exit_value = r->b;
		s->state = REGISTRY_SVC_EXITED;
		break;
	case CIXINIT_EV_RESTART_IN:
		s->state = REGISTRY_SVC_RESTART_WAIT;
		s->restarts = r->b;
		break;
	case CIXINIT_EV_STOPPED:
		s->state = REGISTRY_SVC_STOPPED;
		s->pid = 0;
		break;
	case CIXINIT_EV_FAILED:
		s->fail_reason = r->a;
		s->fail_detail = r->b;
		/* A probe timeout is reported and the service carries on; the rest are terminal. */
		if (r->a != CIXINIT_FAIL_PROBE)
			s->state = REGISTRY_SVC_FAILED;
		break;
	default:
		break;
	}
	e->ready = derive_ready(e);
	return e->ready != was_ready;
}

static void registry_write_json_argv(const char *packed, struct json_writer *w)
{
	const char *p = packed;

	jw_arr_open(w);
	while (*p != '\0') {
		jw_str(w, p);
		p += strlen(p) + 1;
	}
	jw_arr_close(w);
}

static void registry_write_json_service(const struct registry_entry *entry, int idx,
                                        struct json_writer *w)
{
	const struct registry_service *s = &entry->services[idx];
	int j;

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, s->def.name);
	jw_key(w, "type");
	jw_str(w, cixinit_type_name(s->def.type));
	jw_key(w, "cmd");
	registry_write_json_argv(s->def.argv, w);
	jw_key(w, "after");
	jw_arr_open(w);
	for (j = 0; j < idx; j++)
		if ((s->def.after_mask & (1u << j)) != 0)
			jw_str(w, entry->services[j].def.name);
	jw_arr_close(w);
	jw_key(w, "ready_probe");
	if (s->def.ready_kind == CIXINIT_READY_NONE) {
		jw_null(w);
	} else {
		jw_obj_open(w);
		if (s->def.ready_kind == CIXINIT_READY_TCP) {
			jw_key(w, "tcp_port");
			jw_int(w, s->def.ready_port);
		} else if (s->def.ready_kind == CIXINIT_READY_SOCKET) {
			jw_key(w, "socket");
			jw_str(w, s->def.ready_path);
		} else {
			jw_key(w, "command");
			registry_write_json_argv(s->def.ready_path, w);
		}
		jw_key(w, "timeout_seconds");
		jw_int(w, s->def.ready_timeout_seconds);
		jw_obj_close(w);
	}
	jw_key(w, "on_exit");
	jw_str(w, cixinit_on_exit_name(s->def.on_exit));
	jw_key(w, "restart_delay_seconds");
	jw_int(w, s->def.restart_delay_seconds);
	jw_key(w, "stop_signal");
	jw_str(w, cixinit_signal_name(s->def.stop_signal));
	jw_key(w, "stop_timeout_seconds");
	jw_int(w, s->def.stop_timeout_seconds);
	jw_key(w, "uid");
	if (s->def.uid >= 0)
		jw_int(w, s->def.uid);
	else
		jw_null(w);
	jw_key(w, "gid");
	if (s->def.gid >= 0)
		jw_int(w, s->def.gid);
	else
		jw_null(w);

	jw_key(w, "state");
	jw_str(w, registry_service_state_name(s->state));
	jw_key(w, "pid");
	if (s->pid > 0)
		jw_int(w, (long long)s->pid);
	else
		jw_null(w);
	jw_key(w, "restarts");
	jw_int(w, s->restarts);
	jw_key(w, "last_exit");
	if (s->has_exit) {
		jw_obj_open(w);
		jw_key(w, "kind");
		jw_str(w, s->exit_kind == CIXINIT_EXIT_KIND_EXITED ? "exited"
		       : s->exit_kind == CIXINIT_EXIT_KIND_DUMPED ? "dumped" : "killed");
		jw_key(w, s->exit_kind == CIXINIT_EXIT_KIND_EXITED ? "status" : "signal");
		jw_int(w, s->exit_value);
		jw_obj_close(w);
	} else {
		jw_null(w);
	}
	jw_key(w, "failure");
	if (s->fail_reason != 0) {
		jw_obj_open(w);
		jw_key(w, "reason");
		jw_str(w, s->fail_reason == CIXINIT_FAIL_PROBE ? "probe-timeout"
		       : s->fail_reason == CIXINIT_FAIL_ONESHOT ? "oneshot-status"
		       : s->fail_reason == CIXINIT_FAIL_SPAWN ? "spawn"
		       : s->fail_reason == CIXINIT_FAIL_DEPENDENCY ? "dependency" : "?");
		jw_key(w, "detail");
		if (s->fail_reason == CIXINIT_FAIL_DEPENDENCY && s->fail_detail >= 0 &&
		    s->fail_detail < entry->service_count)
			jw_str(w, entry->services[s->fail_detail].def.name);
		else
			jw_int(w, s->fail_detail);
		jw_obj_close(w);
	} else {
		jw_null(w);
	}
	jw_obj_close(w);
}

void registry_close_init_fds(struct registry_entry *e)
{
	if (e->init_control_fd >= 0) {
		close(e->init_control_fd);
		e->init_control_fd = -1;
	}
	if (e->init_report_fd >= 0) {
		close(e->init_report_fd);
		e->init_report_fd = -1;
	}
}

void registry_set_consoles(const char *name, const struct registry_console *consoles, int count)
{
	struct registry_entry *e = registry_find(name);
	int i;

	if (e == NULL)
		return;
	memset(e->consoles, 0, sizeof(e->consoles));
	e->console_count = 0;
	if (consoles == NULL || count <= 0)
		return;
	if (count > REGISTRY_MAX_CONSOLES)
		count = REGISTRY_MAX_CONSOLES;
	for (i = 0; i < count; i++)
		e->consoles[i] = consoles[i];
	e->console_count = count;
}
