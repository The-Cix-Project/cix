/*
 * ADR-0375 phase 2: the handover record's writer, reader and the kernel
 * checks that come before anything trusts it. The format is documented
 * in handover.h.
 */
#include "handover.h"

#include "json.h"
#include "linux_compat.h"
#include "persist.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

int handover_record_init(struct handover_record *rec, int max)
{
	memset(rec, 0, sizeof(*rec));
	rec->format = HANDOVER_FORMAT;
	if (max <= 0)
		return 0;
	rec->containers = calloc((size_t)max, sizeof(*rec->containers));
	return rec->containers == NULL ? -1 : 0;
}

void handover_record_free(struct handover_record *rec)
{
	free(rec->containers);
	rec->containers = NULL;
	rec->count = 0;
}

/* ---- writing ---- */

static void put_ip(struct json_writer *w, uint32_t ip_be)
{
	char s[INET_ADDRSTRLEN];
	struct in_addr a;

	a.s_addr = ip_be;
	if (inet_ntop(AF_INET, &a, s, sizeof(s)) == NULL)
		s[0] = '\0';
	jw_str(w, s);
}

static void put_container(struct json_writer *w, const struct handover_container *c)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, c->name);
	jw_key(w, "image");
	jw_str(w, c->image);
	jw_key(w, "image_version");
	jw_str(w, c->image_version);
	jw_key(w, "running");
	jw_bool(w, c->running);
	jw_key(w, "pid");
	jw_int(w, c->pid);
	jw_key(w, "started_at");
	jw_int(w, c->started_at);
	jw_key(w, "paused");
	jw_bool(w, c->paused);
	jw_key(w, "exit_status");
	jw_int(w, c->exit_status);
	jw_key(w, "term_signal");
	jw_int(w, c->term_signal);
	jw_key(w, "last_exit_reason");
	jw_str(w, c->last_exit_reason);
	jw_key(w, "restart_at");
	jw_int(w, c->restart_at);
	jw_key(w, "userns_enabled");
	jw_bool(w, c->userns_enabled);
	jw_key(w, "disk_quota_bytes");
	jw_int(w, c->disk_quota_bytes);
	jw_key(w, "reserve_devices");
	jw_bool(w, c->reserve_devices);
	jw_key(w, "ip_forward");
	jw_bool(w, c->ip_forward);
	jw_key(w, "capture_requested");
	jw_bool(w, c->capture_requested);
	jw_key(w, "procfuse_expected");
	jw_int(w, c->procfuse_expected);
	jw_key(w, "init_up");
	jw_bool(w, c->init_up);
	jw_key(w, "ready");
	jw_bool(w, c->ready);

	jw_key(w, "nets");
	jw_arr_open(w);
	for (i = 0; i < c->net_count; i++) {
		const struct registry_network_attachment *n = &c->nets[i];

		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, n->name);
		jw_key(w, "ip");
		put_ip(w, n->ip_be);
		jw_key(w, "veth_host");
		jw_str(w, n->veth_host);
		jw_key(w, "ifname");
		jw_str(w, n->ifname);
		jw_key(w, "container_bridge");
		jw_str(w, n->container_bridge);
		jw_obj_close(w);
	}
	jw_arr_close(w);

	jw_key(w, "file_paths");
	jw_arr_open(w);
	for (i = 0; i < c->file_count; i++)
		jw_str(w, c->file_paths[i]);
	jw_arr_close(w);

	jw_key(w, "dns_servers");
	jw_arr_open(w);
	for (i = 0; i < c->dns_server_count; i++)
		jw_str(w, c->dns_servers[i]);
	jw_arr_close(w);

	jw_key(w, "devices");
	jw_arr_open(w);
	for (i = 0; i < c->device_count; i++) {
		const struct registry_device_attachment *d = &c->devices[i];

		jw_obj_open(w);
		jw_key(w, "id");
		jw_str(w, d->id);
		jw_key(w, "dev_path");
		jw_str(w, d->dev_path);
		jw_key(w, "type");
		jw_int(w, (long long)d->type);
		jw_key(w, "major");
		jw_int(w, d->major);
		jw_key(w, "minor");
		jw_int(w, d->minor);
		jw_key(w, "live");
		jw_bool(w, d->live);
		jw_key(w, "shared");
		jw_bool(w, d->shared);
		jw_key(w, "ref");
		jw_str(w, d->ref);
		jw_obj_close(w);
	}
	jw_arr_close(w);

	jw_key(w, "pending_devices");
	jw_arr_open(w);
	for (i = 0; i < c->pending_device_count; i++) {
		jw_obj_open(w);
		jw_key(w, "ref");
		jw_str(w, c->pending_devices[i].ref);
		jw_key(w, "shared");
		jw_bool(w, c->pending_devices[i].shared);
		jw_obj_close(w);
	}
	jw_arr_close(w);

	jw_key(w, "interfaces");
	jw_arr_open(w);
	for (i = 0; i < c->interface_count; i++) {
		const struct registry_interface_record *r = &c->interfaces[i];

		jw_obj_open(w);
		jw_key(w, "granted");
		jw_str(w, r->granted);
		jw_key(w, "phy");
		jw_str(w, r->phy);
		jw_key(w, "anchor");
		jw_str(w, r->anchor);
		jw_key(w, "description");
		jw_str(w, r->description);
		jw_key(w, "driver");
		jw_str(w, r->driver);
		jw_key(w, "mac");
		jw_str(w, r->mac);
		jw_key(w, "mac_permanent");
		jw_bool(w, r->mac_permanent);
		jw_key(w, "ref");
		jw_str(w, r->ref);
		jw_key(w, "ifname");
		jw_str(w, r->ifname);
		jw_key(w, "left_host");
		jw_bool(w, r->left_host);
		jw_key(w, "host");
		jw_str(w, c->interface_hosts[i]);
		jw_obj_close(w);
	}
	jw_arr_close(w);

	jw_key(w, "services");
	jw_arr_open(w);
	for (i = 0; i < c->service_count; i++) {
		const struct handover_service *s = &c->services[i];

		jw_obj_open(w);
		jw_key(w, "state");
		jw_int(w, s->state);
		jw_key(w, "pid");
		jw_int(w, s->pid);
		jw_key(w, "has_exit");
		jw_bool(w, s->has_exit);
		jw_key(w, "exit_kind");
		jw_int(w, s->exit_kind);
		jw_key(w, "exit_value");
		jw_int(w, s->exit_value);
		jw_key(w, "restarts");
		jw_int(w, s->restarts);
		jw_key(w, "fail_reason");
		jw_int(w, s->fail_reason);
		jw_key(w, "fail_detail");
		jw_int(w, s->fail_detail);
		jw_key(w, "output_fd");
		jw_int(w, s->output_fd);
		jw_obj_close(w);
	}
	jw_arr_close(w);

	jw_key(w, "fds");
	jw_obj_open(w);
	jw_key(w, "init_control");
	jw_int(w, c->fd_init_control);
	jw_key(w, "init_report");
	jw_int(w, c->fd_init_report);
	jw_key(w, "capture");
	jw_int(w, c->fd_capture);
	jw_key(w, "bpf_prog");
	jw_int(w, c->fd_bpf_prog);
	jw_obj_close(w);

	jw_key(w, "captured_output");
	jw_str(w, c->captured_output);
	jw_obj_close(w);
}

int handover_write(const char *path, const struct handover_record *rec, char *err,
                   size_t err_size)
{
	struct json_writer w;
	int i, rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "format");
	jw_int(&w, HANDOVER_FORMAT);
	jw_key(&w, "written_by");
	jw_str(&w, rec->written_by);
	jw_key(&w, "written_at");
	jw_int(&w, rec->written_at);
	jw_key(&w, "procfuse_pid");
	jw_int(&w, rec->procfuse_pid);
	jw_key(&w, "containers");
	jw_arr_open(&w);
	for (i = 0; i < rec->count; i++)
		put_container(&w, &rec->containers[i]);
	jw_arr_close(&w);
	jw_obj_close(&w);
	rc = persist_atomic_write(path, w.buf, w.len);
	if (rc != 0)
		snprintf(err, err_size, "could not write %s: %s", path, strerror(errno));
	jw_free(&w);
	return rc == 0 ? 0 : -1;
}

/* ---- reading ---- */

/* Each getter records the first missing key in *missing and answers a
 * zero value, so a reader checks once at the end of a container. */
static const struct json_value *need(const struct json_value *o, const char *key,
                                     const char **missing)
{
	const struct json_value *v = json_object_get(o, key);

	if (v == NULL && *missing == NULL)
		*missing = key;
	return v;
}

static long long get_int(const struct json_value *o, const char *key, const char **missing)
{
	const struct json_value *v = need(o, key, missing);

	if (v == NULL)
		return 0;
	if (v->type == JSON_BOOL)
		return v->u.boolean ? 1 : 0;
	return (long long)json_as_number(v);
}

static void get_str(const struct json_value *o, const char *key, char *out, size_t out_size,
                    const char **missing)
{
	const char *s = json_as_string(need(o, key, missing));

	snprintf(out, out_size, "%s", s != NULL ? s : "");
}

static const struct json_value *get_arr(const struct json_value *o, const char *key,
                                        size_t max, const char **missing)
{
	const struct json_value *v = need(o, key, missing);

	if (v == NULL)
		return NULL;
	if (v->type != JSON_ARRAY || v->u.array.count > max) {
		if (*missing == NULL)
			*missing = key;
		return NULL;
	}
	return v;
}

static void get_container(const struct json_value *o, struct handover_container *c,
                          const char **missing)
{
	const struct json_value *a, *fds;
	size_t i;

	memset(c, 0, sizeof(*c));
	get_str(o, "name", c->name, sizeof(c->name), missing);
	get_str(o, "image", c->image, sizeof(c->image), missing);
	get_str(o, "image_version", c->image_version, sizeof(c->image_version), missing);
	c->running = (int)get_int(o, "running", missing);
	c->pid = (pid_t)get_int(o, "pid", missing);
	c->started_at = get_int(o, "started_at", missing);
	c->paused = (int)get_int(o, "paused", missing);
	c->exit_status = (int)get_int(o, "exit_status", missing);
	c->term_signal = (int)get_int(o, "term_signal", missing);
	get_str(o, "last_exit_reason", c->last_exit_reason, sizeof(c->last_exit_reason), missing);
	c->restart_at = get_int(o, "restart_at", missing);
	c->userns_enabled = (int)get_int(o, "userns_enabled", missing);
	c->disk_quota_bytes = get_int(o, "disk_quota_bytes", missing);
	c->reserve_devices = (int)get_int(o, "reserve_devices", missing);
	c->ip_forward = (int)get_int(o, "ip_forward", missing);
	c->capture_requested = (int)get_int(o, "capture_requested", missing);
	c->procfuse_expected = (int)get_int(o, "procfuse_expected", missing);
	c->init_up = (int)get_int(o, "init_up", missing);
	c->ready = (int)get_int(o, "ready", missing);

	a = get_arr(o, "nets", CONTAINER_MAX_NETWORKS, missing);
	for (i = 0; a != NULL && i < a->u.array.count; i++) {
		const struct json_value *n = a->u.array.items[i];
		struct registry_network_attachment *t = &c->nets[i];
		char ip[INET_ADDRSTRLEN];
		struct in_addr addr;

		addr.s_addr = 0;
		get_str(n, "name", t->name, sizeof(t->name), missing);
		get_str(n, "ip", ip, sizeof(ip), missing);
		if (inet_pton(AF_INET, ip, &addr) != 1 && *missing == NULL)
			*missing = "nets[].ip";
		t->ip_be = addr.s_addr;
		get_str(n, "veth_host", t->veth_host, sizeof(t->veth_host), missing);
		get_str(n, "ifname", t->ifname, sizeof(t->ifname), missing);
		get_str(n, "container_bridge", t->container_bridge, sizeof(t->container_bridge),
		        missing);
		c->net_count++;
	}

	a = get_arr(o, "file_paths", CONTAINER_MAX_FILES, missing);
	for (i = 0; a != NULL && i < a->u.array.count; i++) {
		const char *s = json_as_string(a->u.array.items[i]);

		snprintf(c->file_paths[i], sizeof(c->file_paths[i]), "%s", s != NULL ? s : "");
		c->file_count++;
	}

	a = get_arr(o, "dns_servers", RESOLV_MAX_NAMESERVERS, missing);
	for (i = 0; a != NULL && i < a->u.array.count; i++) {
		const char *s = json_as_string(a->u.array.items[i]);

		snprintf(c->dns_servers[i], sizeof(c->dns_servers[i]), "%s", s != NULL ? s : "");
		c->dns_server_count++;
	}

	a = get_arr(o, "devices", CONTAINER_MAX_DEVICES, missing);
	for (i = 0; a != NULL && i < a->u.array.count; i++) {
		const struct json_value *d = a->u.array.items[i];
		struct registry_device_attachment *t = &c->devices[i];

		get_str(d, "id", t->id, sizeof(t->id), missing);
		get_str(d, "dev_path", t->dev_path, sizeof(t->dev_path), missing);
		t->type = (enum device_node_type)get_int(d, "type", missing);
		t->major = (unsigned int)get_int(d, "major", missing);
		t->minor = (unsigned int)get_int(d, "minor", missing);
		t->live = (int)get_int(d, "live", missing);
		t->shared = (int)get_int(d, "shared", missing);
		get_str(d, "ref", t->ref, sizeof(t->ref), missing);
		c->device_count++;
	}

	a = get_arr(o, "pending_devices", CONTAINER_MAX_DEVICES, missing);
	for (i = 0; a != NULL && i < a->u.array.count; i++) {
		const struct json_value *d = a->u.array.items[i];

		get_str(d, "ref", c->pending_devices[i].ref, sizeof(c->pending_devices[i].ref),
		        missing);
		c->pending_devices[i].shared = (int)get_int(d, "shared", missing);
		c->pending_device_count++;
	}

	a = get_arr(o, "interfaces", CONTAINER_MAX_INTERFACES, missing);
	for (i = 0; a != NULL && i < a->u.array.count; i++) {
		const struct json_value *r = a->u.array.items[i];
		struct registry_interface_record *t = &c->interfaces[i];

		get_str(r, "granted", t->granted, sizeof(t->granted), missing);
		get_str(r, "phy", t->phy, sizeof(t->phy), missing);
		get_str(r, "anchor", t->anchor, sizeof(t->anchor), missing);
		get_str(r, "description", t->description, sizeof(t->description), missing);
		get_str(r, "driver", t->driver, sizeof(t->driver), missing);
		get_str(r, "mac", t->mac, sizeof(t->mac), missing);
		t->mac_permanent = (int)get_int(r, "mac_permanent", missing);
		get_str(r, "ref", t->ref, sizeof(t->ref), missing);
		get_str(r, "ifname", t->ifname, sizeof(t->ifname), missing);
		t->left_host = (int)get_int(r, "left_host", missing);
		get_str(r, "host", c->interface_hosts[i], sizeof(c->interface_hosts[i]), missing);
		c->interface_count++;
	}

	a = get_arr(o, "services", REGISTRY_MAX_SERVICES, missing);
	for (i = 0; a != NULL && i < a->u.array.count; i++) {
		const struct json_value *s = a->u.array.items[i];
		struct handover_service *t = &c->services[i];

		t->state = (int)get_int(s, "state", missing);
		t->pid = (pid_t)get_int(s, "pid", missing);
		t->has_exit = (int)get_int(s, "has_exit", missing);
		t->exit_kind = (int)get_int(s, "exit_kind", missing);
		t->exit_value = (int)get_int(s, "exit_value", missing);
		t->restarts = (int)get_int(s, "restarts", missing);
		t->fail_reason = (int)get_int(s, "fail_reason", missing);
		t->fail_detail = (int)get_int(s, "fail_detail", missing);
		t->output_fd = (int)get_int(s, "output_fd", missing);
		c->service_count++;
	}

	fds = need(o, "fds", missing);
	c->fd_init_control = fds != NULL ? (int)get_int(fds, "init_control", missing) : -1;
	c->fd_init_report = fds != NULL ? (int)get_int(fds, "init_report", missing) : -1;
	c->fd_capture = fds != NULL ? (int)get_int(fds, "capture", missing) : -1;
	c->fd_bpf_prog = fds != NULL ? (int)get_int(fds, "bpf_prog", missing) : -1;

	get_str(o, "captured_output", c->captured_output, sizeof(c->captured_output), missing);
	c->captured_output_len = (int)strlen(c->captured_output);
}

int handover_read(const char *path, struct handover_record *rec, char *err, size_t err_size)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;
	const struct json_value *arr;
	const char *missing = NULL;
	long long format;
	size_t i;

	memset(rec, 0, sizeof(*rec));
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL) {
		snprintf(err, err_size, "no handover record at %s", path);
		return -1;
	}
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_OBJECT) {
		if (root != NULL)
			json_free(root);
		snprintf(err, err_size, "the handover record at %s is not a JSON object", path);
		return -1;
	}
	format = get_int(root, "format", &missing);
	if (missing == NULL && format > HANDOVER_FORMAT) {
		json_free(root);
		snprintf(err, err_size,
		         "the handover record is format %lld, newer than this build's %d", format,
		         HANDOVER_FORMAT);
		return -1;
	}
	rec->format = (int)format;
	get_str(root, "written_by", rec->written_by, sizeof(rec->written_by), &missing);
	rec->written_at = get_int(root, "written_at", &missing);
	rec->procfuse_pid = (pid_t)get_int(root, "procfuse_pid", &missing);
	arr = get_arr(root, "containers", REGISTRY_MAX_CONTAINERS, &missing);
	if (missing != NULL) {
		json_free(root);
		snprintf(err, err_size, "the handover record has no valid \"%s\"", missing);
		return -1;
	}
	if (handover_record_init(rec, (int)arr->u.array.count) != 0) {
		json_free(root);
		snprintf(err, err_size, "out of memory reading the handover record");
		return -1;
	}
	rec->format = (int)format;
	get_str(root, "written_by", rec->written_by, sizeof(rec->written_by), &missing);
	rec->written_at = get_int(root, "written_at", &missing);
	rec->procfuse_pid = (pid_t)get_int(root, "procfuse_pid", &missing);
	for (i = 0; i < arr->u.array.count; i++) {
		get_container(arr->u.array.items[i], &rec->containers[i], &missing);
		if (missing != NULL) {
			snprintf(err, err_size,
			         "the handover record's container %d has no valid \"%s\"", (int)i,
			         missing);
			json_free(root);
			handover_record_free(rec);
			return -1;
		}
		rec->count++;
	}
	json_free(root);
	return 0;
}

/* ---- the kernel's word ---- */

/* fd is open; want_sock: a SEQPACKET socket, else a FIFO. FD_CLOEXEC is
 * set back on it. 0, or -1. */
static int check_fd(int fd, int want_sock)
{
	struct stat st;

	if (fd < 0 || fcntl(fd, F_GETFD) == -1 || fstat(fd, &st) != 0)
		return -1;
	if (want_sock) {
		int type = 0;
		socklen_t tl = sizeof(type);

		if (!S_ISSOCK(st.st_mode) ||
		    getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_SEQPACKET)
			return -1;
	} else if (!S_ISFIFO(st.st_mode)) {
		return -1;
	}
	return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 ? 0 : -1;
}

/* Is pid listed in cgroup_root/<name>/cgroup.procs? */
static int pid_in_cgroup(const char *cgroup_root, const char *name, pid_t pid)
{
	char path[512], buf[8192];
	ssize_t n;
	int fd, found = 0;
	char *p, *save = NULL;

	if ((size_t)snprintf(path, sizeof(path), "%s/%s/cgroup.procs", cgroup_root, name) >=
	    sizeof(path))
		return 0;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	for (p = strtok_r(buf, "\n", &save); p != NULL && !found; p = strtok_r(NULL, "\n", &save))
		found = atol(p) == (long)pid;
	return found;
}

int handover_validate(const struct handover_record *rec, const char *cgroup_root, int *pidfds,
                      char *err, size_t err_size)
{
	int i, j;

	for (i = 0; i < rec->count; i++)
		pidfds[i] = -1;
	for (i = 0; i < rec->count; i++) {
		const struct handover_container *c = &rec->containers[i];
		const char *why = NULL;

		if (!c->running)
			continue;
		if (c->pid <= 0 || (pidfds[i] = sys_pidfd_open(c->pid, 0)) < 0)
			why = "its pid is not alive";
		else if (!pid_in_cgroup(cgroup_root, c->name, c->pid))
			why = "its pid is not in its cgroup";
		else if (check_fd(c->fd_init_control, 1) != 0 || check_fd(c->fd_init_report, 1) != 0)
			why = "a cix-init descriptor is not the SEQPACKET socket it should be";
		else if (c->fd_capture >= 0 && check_fd(c->fd_capture, 0) != 0)
			why = "its capture descriptor is not a pipe";
		else if (c->fd_bpf_prog >= 0 && fcntl(c->fd_bpf_prog, F_SETFD, FD_CLOEXEC) != 0)
			why = "its device program descriptor is not open";
		for (j = 0; why == NULL && j < c->service_count; j++)
			if (c->services[j].output_fd >= 0 && check_fd(c->services[j].output_fd, 0) != 0)
				why = "a service's output descriptor is not a pipe";
		if (why != NULL) {
			snprintf(err, err_size, "handover: container %s (pid %d): %s (ADR-0375)", c->name,
			         (int)c->pid, why);
			for (j = 0; j <= i; j++)
				if (pidfds[j] >= 0) {
					close(pidfds[j]);
					pidfds[j] = -1;
				}
			return -1;
		}
	}
	return 0;
}
