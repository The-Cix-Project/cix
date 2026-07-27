#include "container.h"
#include "dns.h"
#include "http.h"
#include "json.h"
#include "linux_compat.h"
#include "network.h"
#include "pki.h"
#include "pkg.h"
#include "registry.h"
#include "rtnetlink.h"
#include "staticfile.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_PORT 7620
#define DEFAULT_BIND "127.0.0.1"
#define DEFAULT_WEB_ROOT "web"
#define BASE_DIR "/var/lib/kanxeo"
#define IMAGES_DIR BASE_DIR "/images"
#define CONTAINERS_DIR BASE_DIR "/containers"
#define NETWORKS_STATE_PATH BASE_DIR "/networks.json"
#define DNS_RECORDS_STATE_PATH BASE_DIR "/dns_records.json"
#define PKI_DIR BASE_DIR "/pki"
#define PKI_CERTS_STATE_PATH PKI_DIR "/pki_certs.json"
#define PKG_DIR BASE_DIR "/pkg"
#define PKG_INSTALLED_STATE_PATH PKG_DIR "/pkg_installed.json"
#define MAX_EVENTS 64
#define CONTAINERS_PREFIX "/v1/containers/"
#define NETWORKS_PREFIX "/v1/networks/"
#define DNS_RECORDS_PREFIX "/v1/dns/records/"
#define DNS_SERVERS_PREFIX "/v1/dns/servers/"
#define PKI_CERTS_PREFIX "/v1/pki/certs/"
#define PKG_PREFIX "/v1/pkg/"

enum conn_kind { CONN_LISTENER, CONN_CLIENT, CONN_CONTAINER, CONN_PKG_FETCH };

struct conn {
	enum conn_kind kind;
	int fd;
	struct http_conn http;        /* CONN_CLIENT only */
	struct registry_entry *entry; /* CONN_CONTAINER only */
	pid_t pkg_fetch_pid;          /* CONN_PKG_FETCH only */
};

static int g_epfd;
static struct conn g_listener_conn;
static const char *g_web_root;
static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static int ensure_dir(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

static int name_is_valid(const char *name)
{
	size_t i;

	if (name == NULL || name[0] == '\0')
		return 0;
	for (i = 0; name[i] != '\0'; i++) {
		char c = name[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return 0;
	}
	if (i >= REGISTRY_NAME_MAX)
		return 0;
	return 1;
}

static void respond_json(int fd, int status, const char *status_text, struct json_writer *w)
{
	http_set_blocking(fd);
	http_write_response(fd, status, status_text, "application/json", w->buf, w->len);
}

static void respond_error(int fd, int status, const char *status_text, const char *msg)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "error");
	jw_str(&w, msg);
	jw_obj_close(&w);
	respond_json(fd, status, status_text, &w);
	jw_free(&w);
}

static void handle_health(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "status");
	jw_str(&w, "ok");
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "containers");
	registry_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_get_one(int fd, const char *name)
{
	struct registry_entry *e = registry_find(name);
	struct json_writer w;

	if (e == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}
	jw_init(&w);
	registry_write_json_one(e, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void register_container_pidfd(struct registry_entry *entry)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		/*
		 * The container is already running with real resources
		 * committed; failing to register its pidfd would leave it
		 * permanently stuck reporting "running" even after it
		 * exits. Under this kind of memory pressure the daemon is
		 * already in a bad state, so fail loudly rather than carry
		 * a silently-wrong registry entry forever.
		 */
		perror("malloc (container reactor conn)");
		abort();
	}
	cc->kind = CONN_CONTAINER;
	cc->fd = entry->handle.pidfd;
	cc->entry = entry;
	entry->reactor_conn = cc;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD pidfd");
		abort();
	}
}

/*
 * Same shape as register_container_pidfd(), for a plain fork()'d
 * subprocess instead of a clone3()'d container -- Phase 10's package
 * fetch step (a `curl` subprocess) needs the exact same non-blocking
 * "tell me via epoll when this exits" treatment a container's own
 * CLONE_PIDFD-obtained pidfd already gets, via the explicit
 * sys_pidfd_open() equivalent for an already-forked pid.
 */
static void register_pkg_fetch_pidfd(pid_t pid, int pidfd)
{
	struct conn *cc;
	struct kx_epoll_event ev;

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		perror("malloc (pkg fetch reactor conn)");
		abort();
	}
	cc->kind = CONN_PKG_FETCH;
	cc->fd = pidfd;
	cc->pkg_fetch_pid = pid;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = cc;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, cc->fd, &ev) != 0) {
		perror("epoll_ctl ADD pkg fetch pidfd");
		abort();
	}
}

static void handle_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jname, *jimage, *jcmd, *jmem, *jpids, *jnetworks, *jip_forward, *jroutes;
	const char *name, *image;
	char lowerdir[PATH_MAX];
	char container_base[PATH_MAX];
	char upperdir[PATH_MAX], workdir[PATH_MAX], merged[PATH_MAX];
	struct stat st;
	struct container_spec spec;
	struct registry_entry *entry;
	enum registry_error rerr;
	char *argv_buf[64];
	char *empty_envp[1];
	size_t argc, i;
	struct json_writer w;
	struct registry_network_attachment net_attachments[CONTAINER_MAX_NETWORKS];
	int net_count = 0;
	int ip_forward = 0;
	struct route_spec route_specs[CONTAINER_MAX_ROUTES];
	int route_count = 0;
	const struct json_value *jdns_register;
	int dns_register = 0;
	const struct json_value *jpki_issue, *jpki_cert_dir, *jpki_days;
	int pki_issue = 0;
	char pki_cert_dir_buf[PATH_MAX];
	int pki_days = 365;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	jname = json_object_get(root, "name");
	jimage = json_object_get(root, "image");
	jcmd = json_object_get(root, "cmd");
	jnetworks = json_object_get(root, "networks");
	jip_forward = json_object_get(root, "ip_forward");
	jroutes = json_object_get(root, "routes");
	jdns_register = json_object_get(root, "dns_register");
	jpki_issue = json_object_get(root, "pki_issue");
	jpki_cert_dir = json_object_get(root, "pki_cert_dir");
	jpki_days = json_object_get(root, "pki_days");
	name = json_as_string(jname);
	image = json_as_string(jimage);
	ip_forward = (jip_forward != NULL && jip_forward->type == JSON_BOOL && jip_forward->u.boolean);
	dns_register = (jdns_register != NULL && jdns_register->type == JSON_BOOL &&
	                jdns_register->u.boolean);
	pki_issue = (jpki_issue != NULL && jpki_issue->type == JSON_BOOL && jpki_issue->u.boolean);
	snprintf(pki_cert_dir_buf, sizeof(pki_cert_dir_buf), "%s",
	         json_as_string(jpki_cert_dir) != NULL ? json_as_string(jpki_cert_dir) :
	                                                  "/etc/kanxeo-tls");
	if (jpki_days != NULL)
		pki_days = (int)json_as_number(jpki_days);

	if (!name_is_valid(name) || image == NULL || image[0] == '\0' || jcmd == NULL ||
	    jcmd->type != JSON_ARRAY || jcmd->u.array.count == 0 ||
	    jcmd->u.array.count >= (sizeof(argv_buf) / sizeof(argv_buf[0]))) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name/image/cmd missing or invalid");
		return;
	}
	if (jnetworks != NULL) {
		if (jnetworks->type != JSON_ARRAY || jnetworks->u.array.count == 0 ||
		    jnetworks->u.array.count > CONTAINER_MAX_NETWORKS) {
			json_free(root);
			respond_error(fd, 400, "Bad Request",
			              "networks must be a non-empty array of at most 64 entries");
			return;
		}
		for (i = 0; i < jnetworks->u.array.count; i++) {
			const char *n = json_as_string(jnetworks->u.array.items[i]);

			if (n == NULL || network_find(n) == NULL) {
				json_free(root);
				respond_error(fd, 400, "Bad Request", "unknown network");
				return;
			}
		}
	}
	if (dns_register && jnetworks == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "dns_register requires networks");
		return;
	}
	if (pki_issue && !pki_ca_bootstrapped()) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "pki_issue requires the CA to be bootstrapped -- POST /v1/pki/ca first");
		return;
	}
	if (jroutes != NULL) {
		if (jroutes->type != JSON_ARRAY || jroutes->u.array.count > CONTAINER_MAX_ROUTES) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "routes must be an array of at most 8 entries");
			return;
		}
		route_count = (int)jroutes->u.array.count;
		for (i = 0; i < (size_t)route_count; i++) {
			const struct json_value *item = jroutes->u.array.items[i];
			const char *dest = json_as_string(json_object_get(item, "dest"));
			const char *via = json_as_string(json_object_get(item, "via"));
			const struct json_value *jprefix = json_object_get(item, "prefix_len");
			struct in_addr dest_addr, via_addr;
			long prefix_len;

			if (dest == NULL || via == NULL || jprefix == NULL ||
			    inet_pton(AF_INET, dest, &dest_addr) != 1 ||
			    inet_pton(AF_INET, via, &via_addr) != 1) {
				json_free(root);
				respond_error(fd, 400, "Bad Request", "invalid routes entry");
				return;
			}
			prefix_len = (long)json_as_number(jprefix);
			if (prefix_len < 0 || prefix_len > 32) {
				json_free(root);
				respond_error(fd, 400, "Bad Request", "routes prefix_len must be 0-32");
				return;
			}
			route_specs[i].dest_be = dest_addr.s_addr;
			route_specs[i].dest_prefix_len = (int)prefix_len;
			route_specs[i].gateway_be = via_addr.s_addr;
		}
	}

	argc = jcmd->u.array.count;
	for (i = 0; i < argc; i++) {
		const char *s = json_as_string(jcmd->u.array.items[i]);

		if (s == NULL) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "cmd must be an array of strings");
			return;
		}
		argv_buf[i] = (char *)s;
	}
	argv_buf[argc] = NULL;
	empty_envp[0] = NULL;

	snprintf(lowerdir, sizeof(lowerdir), "%s/%s/rootfs", IMAGES_DIR, image);
	if (stat(lowerdir, &st) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "image rootfs does not exist");
		return;
	}

	if (registry_find(name) != NULL) {
		json_free(root);
		respond_error(fd, 409, "Conflict", "a container with this name already exists");
		return;
	}

	if (jnetworks != NULL) {
		net_count = (int)jnetworks->u.array.count;
		for (i = 0; i < (size_t)net_count; i++) {
			const char *n = json_as_string(jnetworks->u.array.items[i]);
			uint32_t ip_be;

			if (network_alloc_ip(n, &ip_be) != 0) {
				json_free(root);
				respond_error(fd, 500, "Internal Server Error", "no free IP addresses");
				return;
			}
			memset(net_attachments[i].name, 0, sizeof(net_attachments[i].name));
			strncpy(net_attachments[i].name, n, sizeof(net_attachments[i].name) - 1);
			net_attachments[i].ip_be = ip_be;
		}
	}

	snprintf(container_base, sizeof(container_base), "%s/%s", CONTAINERS_DIR, name);
	if (mkdir(container_base, 0755) != 0 && errno != EEXIST) {
		json_free(root);
		respond_error(fd, 500, "Internal Server Error", "failed to create container directory");
		return;
	}
	snprintf(upperdir, sizeof(upperdir), "%s/upper", container_base);
	snprintf(workdir, sizeof(workdir), "%s/work", container_base);
	snprintf(merged, sizeof(merged), "%s/merged", container_base);

	memset(&spec, 0, sizeof(spec));
	spec.ns.clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET |
	                       CLONE_NEWCGROUP | CLONE_INTO_CGROUP;
	spec.ns.hostname = name;
	spec.cg.name = name;
	jmem = json_object_get(root, "memory_max");
	spec.cg.memory_max = jmem != NULL ? (long long)json_as_number(jmem) : 0;
	jpids = json_object_get(root, "pids_max");
	spec.cg.pids_max = jpids != NULL ? (long long)json_as_number(jpids) : 0;
	spec.cg.cpu_max = NULL;
	spec.ov.lowerdir = lowerdir;
	spec.ov.upperdir = upperdir;
	spec.ov.workdir = workdir;
	spec.ov.merged = merged;
	spec.mnt.put_old_rel = ".old_root";
	spec.net_count = net_count;
	for (i = 0; i < (size_t)net_count; i++) {
		struct network_def *net = network_find(net_attachments[i].name);

		spec.nets[i].bridge = net->name;
		spec.nets[i].container_ip_be = net_attachments[i].ip_be;
		spec.nets[i].gateway_ip_be = net->gateway_be;
		spec.nets[i].prefix_len = net->prefix_len;
	}
	spec.ip_forward = ip_forward;
	spec.route_count = route_count;
	for (i = 0; i < (size_t)route_count; i++)
		spec.routes[i] = route_specs[i];
	spec.argv = argv_buf;
	spec.envp = empty_envp;

	rerr = registry_create(name, &spec, net_attachments, net_count, ip_forward, &entry);
	/*
	 * Safe to free the JSON tree now even though spec.ns.hostname,
	 * spec.cg.name and spec.argv[] point into it: registry_create()
	 * has already returned, meaning container_create()'s clone3() has
	 * already happened. From that point on the child is a fully
	 * independent process with its own copy-on-write view of this
	 * memory -- nothing this process does to it afterward (including
	 * freeing it) is visible to the child, by the basic guarantee of
	 * copy-on-write.
	 */
	json_free(root);

	if (rerr == REGISTRY_ERR_DUPLICATE) {
		respond_error(fd, 409, "Conflict", "a container with this name already exists");
		return;
	}
	if (rerr == REGISTRY_ERR_FULL) {
		respond_error(fd, 500, "Internal Server Error", "container table full");
		return;
	}
	if (rerr == REGISTRY_ERR_CREATE_FAILED) {
		respond_error(fd, 500, "Internal Server Error", "failed to create container");
		return;
	}

	register_container_pidfd(entry);

	if (dns_register) {
		/* entry->name, not the local `name`, which pointed into
		 * root and is no longer valid after json_free() above. */
		struct dns_record *rec;
		enum dns_error derr = dns_record_create(entry->name, spec.nets[0].container_ip_be,
		                                         entry->name, &rec);

		if (derr != DNS_OK)
			fprintf(stderr,
			        "%s: dns_register requested but auto-registration failed (err=%d)\n",
			        entry->name, (int)derr);
	}

	if (pki_issue) {
		/* CN/SAN = the container's own name, matching pki_cert_create()'s
		 * existing manual-call default-SAN-to-name behavior. No IP SAN --
		 * pki_issue doesn't require networks, unlike dns_register, since
		 * delivery via /proc/<pid>/root/ works for any running container
		 * regardless of networking. */
		const char *pki_sans[1];
		struct json_writer scratch;
		enum pki_error perr;

		pki_sans[0] = entry->name;
		jw_init(&scratch);
		perr = pki_cert_create(entry->name, pki_sans, 1, pki_days, entry->name, &scratch);
		jw_free(&scratch);

		if (perr != PKI_OK) {
			fprintf(stderr,
			        "%s: pki_issue requested but cert issuance failed (err=%d)\n",
			        entry->name, (int)perr);
		} else {
			enum pki_error derr2 =
			    pki_cert_deliver(entry->name, entry->handle.pid, pki_cert_dir_buf);

			if (derr2 != PKI_OK)
				fprintf(stderr,
				        "%s: pki_issue cert issued but delivery into the container failed (err=%d)\n",
				        entry->name, (int)derr2);
		}
	}

	jw_init(&w);
	registry_write_json_one(entry, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_delete(int fd, const char *name)
{
	struct registry_entry *e = registry_find(name);
	struct conn *cc;

	if (e == NULL) {
		respond_error(fd, 404, "Not Found", "no such container");
		return;
	}

	if (e->reactor_conn != NULL) {
		cc = e->reactor_conn;
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		free(cc);
		e->reactor_conn = NULL;
	}

	registry_remove(name);
	dns_server_forget(name);
	dns_record_forget_owner(name);
	pki_cert_forget_owner(name);
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_network_error(int fd, enum network_error err)
{
	switch (err) {
	case NETWORK_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid network name");
		break;
	case NETWORK_ERR_INVALID_SUBNET:
		respond_error(fd, 400, "Bad Request", "invalid subnet/prefix_len");
		break;
	case NETWORK_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "a network with this name already exists");
		break;
	case NETWORK_ERR_OVERLAP:
		respond_error(fd, 400, "Bad Request", "subnet overlaps an existing network");
		break;
	case NETWORK_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "network table full");
		break;
	case NETWORK_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such network");
		break;
	case NETWORK_ERR_IN_USE:
		respond_error(fd, 409, "Conflict", "network is still in use by a container");
		break;
	case NETWORK_ERR_CREATE_FAILED:
	case NETWORK_ERR_DELETE_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "network operation failed");
		break;
	}
}

static void handle_network_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name, *subnet;
	const struct json_value *jprefix;
	int prefix_len;
	struct network_def *net;
	enum network_error nerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	subnet = json_as_string(json_object_get(root, "subnet"));
	jprefix = json_object_get(root, "prefix_len");

	if (name == NULL || subnet == NULL || jprefix == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name/subnet/prefix_len missing");
		return;
	}
	prefix_len = (int)json_as_number(jprefix);

	nerr = network_create(name, subnet, prefix_len, &net);
	json_free(root);

	if (nerr != NETWORK_OK) {
		respond_network_error(fd, nerr);
		return;
	}

	jw_init(&w);
	network_write_json_one(net, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_network_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "networks");
	network_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_network_get_one(int fd, const char *name)
{
	struct network_def *net = network_find(name);
	struct json_writer w;

	if (net == NULL) {
		respond_error(fd, 404, "Not Found", "no such network");
		return;
	}
	jw_init(&w);
	network_write_json_one(net, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_network_delete(int fd, const char *name)
{
	enum network_error nerr = network_delete(name);

	if (nerr != NETWORK_OK) {
		respond_network_error(fd, nerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_dns_error(int fd, enum dns_error err)
{
	switch (err) {
	case DNS_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid DNS record name");
		break;
	case DNS_ERR_INVALID_IP:
		respond_error(fd, 400, "Bad Request", "invalid ip");
		break;
	case DNS_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "a record with this name already exists");
		break;
	case DNS_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "DNS record table full");
		break;
	case DNS_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such DNS record");
		break;
	case DNS_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "DNS record operation failed");
		break;
	}
}

static void handle_dns_record_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name, *ip;
	struct in_addr addr;
	struct dns_record *rec;
	enum dns_error derr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	ip = json_as_string(json_object_get(root, "ip"));

	if (name == NULL || ip == NULL || inet_pton(AF_INET, ip, &addr) != 1) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name/ip missing or invalid");
		return;
	}

	derr = dns_record_create(name, addr.s_addr, NULL, &rec);
	json_free(root);

	if (derr != DNS_OK) {
		respond_dns_error(fd, derr);
		return;
	}

	jw_init(&w);
	dns_write_json_one(rec, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_dns_record_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "records");
	dns_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_dns_record_get_one(int fd, const char *name)
{
	struct dns_record *rec = dns_record_find(name);
	struct json_writer w;

	if (rec == NULL) {
		respond_error(fd, 404, "Not Found", "no such DNS record");
		return;
	}
	jw_init(&w);
	dns_write_json_one(rec, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_dns_record_delete(int fd, const char *name)
{
	enum dns_error derr = dns_record_delete(name);

	if (derr != DNS_OK) {
		respond_dns_error(fd, derr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_dns_server_error(int fd, enum dns_server_error err)
{
	switch (err) {
	case DNS_SERVER_ERR_INVALID_PATH:
		respond_error(fd, 400, "Bad Request", "invalid hosts_path");
		break;
	case DNS_SERVER_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "this container is already registered");
		break;
	case DNS_SERVER_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "DNS server binding table full");
		break;
	case DNS_SERVER_ERR_WRITE_FAILED:
		respond_error(fd, 500, "Internal Server Error", "failed to write hosts file");
		break;
	case DNS_SERVER_ERR_NOT_FOUND:
	default:
		respond_error(fd, 404, "Not Found", "no such DNS server binding");
		break;
	}
}

static void handle_dns_server_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *container_name, *hosts_path;
	struct registry_entry *entry;
	enum dns_server_error serr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	container_name = json_as_string(json_object_get(root, "container"));
	hosts_path = json_as_string(json_object_get(root, "hosts_path"));

	if (container_name == NULL || hosts_path == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "container/hosts_path missing");
		return;
	}

	entry = registry_find(container_name);
	if (entry == NULL || !entry->running) {
		json_free(root);
		respond_error(fd, 404, "Not Found", "no such running container");
		return;
	}

	serr = dns_server_register(container_name, entry->handle.pid, entry->handle.pidfd,
	                            hosts_path);

	if (serr != DNS_SERVER_OK) {
		json_free(root);
		respond_dns_server_error(fd, serr);
		return;
	}

	/*
	 * container_name/hosts_path still point into root -- build the
	 * response before freeing it, not after (freeing first and then
	 * reading through these pointers would be a use-after-free).
	 */
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container_name);
	jw_key(&w, "hosts_path");
	jw_str(&w, hosts_path);
	jw_obj_close(&w);
	json_free(root);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_dns_server_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	dns_server_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_dns_server_delete(int fd, const char *name)
{
	enum dns_server_error serr = dns_server_unregister(name);

	if (serr != DNS_SERVER_OK) {
		respond_dns_server_error(fd, serr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_pki_error(int fd, enum pki_error err)
{
	switch (err) {
	case PKI_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid name/common_name/sans");
		break;
	case PKI_ERR_NOT_BOOTSTRAPPED:
		respond_error(fd, 400, "Bad Request", "CA not bootstrapped -- POST /v1/pki/ca first");
		break;
	case PKI_ERR_ALREADY_BOOTSTRAPPED:
		respond_error(fd, 409, "Conflict", "CA is already bootstrapped");
		break;
	case PKI_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "a cert with this name already exists");
		break;
	case PKI_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "PKI cert table full");
		break;
	case PKI_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such CA / cert");
		break;
	case PKI_ERR_OPENSSL_FAILED:
	case PKI_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "PKI operation failed");
		break;
	}
}

static void handle_pki_ca_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root = NULL;
	const char *common_name = "Kanxeo Root CA";
	int days = 3650;
	enum pki_error perr;

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		if (json_as_string(json_object_get(root, "common_name")) != NULL)
			common_name = json_as_string(json_object_get(root, "common_name"));
		if (json_object_get(root, "days") != NULL)
			days = (int)json_as_number(json_object_get(root, "days"));
	}

	perr = pki_ca_create(common_name, days);
	json_free(root);

	if (perr != PKI_OK) {
		respond_pki_error(fd, perr);
		return;
	}

	{
		struct json_writer w;

		jw_init(&w);
		if (pki_ca_get(&w) != PKI_OK) {
			jw_free(&w);
			respond_error(fd, 500, "Internal Server Error", "CA created but could not be read back");
			return;
		}
		respond_json(fd, 201, "Created", &w);
		jw_free(&w);
	}
}

static void handle_pki_ca_get(int fd)
{
	struct json_writer w;
	enum pki_error perr;

	jw_init(&w);
	perr = pki_ca_get(&w);
	if (perr != PKI_OK) {
		jw_free(&w);
		/*
		 * PKI_ERR_NOT_BOOTSTRAPPED means two different things
		 * depending on the endpoint: for POST /v1/pki/certs it's a
		 * genuine "you can't do this yet" precondition (400, via
		 * respond_pki_error below). Here, GET-ing a CA that doesn't
		 * exist yet is exactly the same shape as GET
		 * /v1/dns/records/{name} or /v1/networks/{name} on a
		 * missing resource -- 404, matching every other single-
		 * resource GET in this API, not respond_pki_error's generic
		 * (POST-precondition-oriented) 400 mapping.
		 */
		if (perr == PKI_ERR_NOT_BOOTSTRAPPED)
			respond_error(fd, 404, "Not Found", "CA not bootstrapped yet");
		else
			respond_pki_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pki_cert_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	const struct json_value *jsans, *jdays;
	const char *sans_buf[PKI_MAX_SANS];
	int san_count;
	int days = 365;
	enum pki_error perr;
	struct json_writer w;
	size_t i;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	jsans = json_object_get(root, "sans");
	jdays = json_object_get(root, "days");
	if (jdays != NULL)
		days = (int)json_as_number(jdays);

	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}

	if (jsans != NULL) {
		if (jsans->type != JSON_ARRAY || jsans->u.array.count == 0 ||
		    jsans->u.array.count > PKI_MAX_SANS) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "sans must be a non-empty array of at most 8 entries");
			return;
		}
		san_count = (int)jsans->u.array.count;
		for (i = 0; i < (size_t)san_count; i++) {
			sans_buf[i] = json_as_string(jsans->u.array.items[i]);
			if (sans_buf[i] == NULL) {
				json_free(root);
				respond_error(fd, 400, "Bad Request", "sans must be an array of strings");
				return;
			}
		}
	} else {
		sans_buf[0] = name;
		san_count = 1;
	}

	jw_init(&w);
	perr = pki_cert_create(name, sans_buf, san_count, days, NULL, &w);
	json_free(root);

	if (perr != PKI_OK) {
		jw_free(&w);
		respond_pki_error(fd, perr);
		return;
	}

	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

static void handle_pki_cert_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "certs");
	pki_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pki_cert_get_one(int fd, const char *name)
{
	struct json_writer w;
	enum pki_error perr;

	jw_init(&w);
	perr = pki_cert_get_one(name, &w);
	if (perr != PKI_OK) {
		jw_free(&w);
		respond_pki_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pki_cert_delete(int fd, const char *name)
{
	enum pki_error perr = pki_cert_delete(name);

	if (perr != PKI_OK) {
		respond_pki_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_pkg_error(int fd, enum pkg_error err)
{
	switch (err) {
	case PKG_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid package name");
		break;
	case PKG_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such package");
		break;
	case PKG_ERR_INVALID_RECIPE:
		respond_error(fd, 400, "Bad Request", "no such recipe, or it failed to parse");
		break;
	case PKG_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "package is already installed");
		break;
	case PKG_ERR_BUSY:
		respond_error(fd, 409, "Conflict", "another package install is already in progress");
		break;
	case PKG_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "package table full");
		break;
	case PKG_ERR_SPAWN_FAILED:
	case PKG_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "package operation failed");
		break;
	}
}

static void handle_pkg_bootstrap(int fd)
{
	enum pkg_error perr = pkg_bootstrap_build_image();

	if (perr != PKG_OK) {
		respond_pkg_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void handle_pkg_recipes_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "recipes");
	pkg_write_json_recipes(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pkg_install(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	const struct json_value *jupgrade;
	int upgrade;
	char started_name[PKG_NAME_MAX];
	pid_t pid;
	int pidfd;
	enum pkg_error perr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	name = json_as_string(json_object_get(root, "name"));
	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}
	jupgrade = json_object_get(root, "upgrade");
	upgrade = (jupgrade != NULL && jupgrade->type == JSON_BOOL && jupgrade->u.boolean);

	perr = pkg_install_start(name, upgrade, started_name, sizeof(started_name), &pid, &pidfd);
	if (perr != PKG_OK) {
		json_free(root);
		respond_pkg_error(fd, perr);
		return;
	}

	/*
	 * started_name, not name -- if name needed a dependency installed
	 * first, that dependency (not name itself) is what's actually
	 * fetching right now, and that's the honest thing to describe.
	 */
	jw_init(&w);
	if (pkg_get_one(started_name, &w) != PKG_OK) {
		/* shouldn't happen -- pkg_install_start() just created it */
		jw_free(&w);
		json_free(root);
		respond_error(fd, 500, "Internal Server Error",
		              "package started but could not be read back");
		return;
	}
	json_free(root);
	register_pkg_fetch_pidfd(pid, pidfd);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}

static void handle_pkg_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "packages");
	pkg_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pkg_get_one(int fd, const char *name)
{
	struct json_writer w;
	enum pkg_error perr;

	jw_init(&w);
	perr = pkg_get_one(name, &w);
	if (perr != PKG_OK) {
		jw_free(&w);
		respond_pkg_error(fd, perr);
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void handle_pkg_delete(int fd, const char *name)
{
	enum pkg_error perr = pkg_delete(name);

	if (perr != PKG_OK) {
		respond_pkg_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void dispatch(int fd, const struct http_request *req)
{
	const char *name;

	if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/v1/health") == 0) {
		handle_health(fd);
		return;
	}
	if (strcmp(req->path, "/v1/containers") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, CONTAINERS_PREFIX, strlen(CONTAINERS_PREFIX)) == 0) {
		name = req->path + strlen(CONTAINERS_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_delete(fd, name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/networks") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_network_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_network_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, NETWORKS_PREFIX, strlen(NETWORKS_PREFIX)) == 0) {
		name = req->path + strlen(NETWORKS_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_network_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_network_delete(fd, name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/dns/records") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_dns_record_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_dns_record_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, DNS_RECORDS_PREFIX, strlen(DNS_RECORDS_PREFIX)) == 0) {
		name = req->path + strlen(DNS_RECORDS_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_dns_record_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_dns_record_delete(fd, name);
				return;
			}
		}
	}
	if (strcmp(req->path, "/v1/dns/servers") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_dns_server_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_dns_server_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, DNS_SERVERS_PREFIX, strlen(DNS_SERVERS_PREFIX)) == 0) {
		name = req->path + strlen(DNS_SERVERS_PREFIX);
		if (name[0] != '\0' && strcmp(req->method, "DELETE") == 0) {
			handle_dns_server_delete(fd, name);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pki/ca") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pki_ca_get(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_pki_ca_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pki/certs") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pki_cert_list(fd);
			return;
		}
		if (strcmp(req->method, "POST") == 0) {
			handle_pki_cert_create(fd, req->body, req->body_len);
			return;
		}
	}
	if (strncmp(req->path, PKI_CERTS_PREFIX, strlen(PKI_CERTS_PREFIX)) == 0) {
		name = req->path + strlen(PKI_CERTS_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_pki_cert_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_pki_cert_delete(fd, name);
				return;
			}
		}
	}
	/*
	 * These three reserved paths are checked before the generic
	 * PKG_PREFIX/{name} fallback below, exactly like every other
	 * resource's exact-match-then-prefix ordering in this dispatch --
	 * a package named "bootstrap"/"recipes"/"install" would be
	 * unreachable via GET/DELETE /v1/pkg/{name}, a deliberate,
	 * documented reserved-words boundary (recipes are operator-
	 * provisioned out of band, easily avoided in practice).
	 */
	if (strcmp(req->path, "/v1/pkg/bootstrap") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_pkg_bootstrap(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/recipes") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_recipes_list(fd);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg/install") == 0) {
		if (strcmp(req->method, "POST") == 0) {
			handle_pkg_install(fd, req->body, req->body_len);
			return;
		}
	}
	if (strcmp(req->path, "/v1/pkg") == 0) {
		if (strcmp(req->method, "GET") == 0) {
			handle_pkg_list(fd);
			return;
		}
	}
	if (strncmp(req->path, PKG_PREFIX, strlen(PKG_PREFIX)) == 0) {
		name = req->path + strlen(PKG_PREFIX);
		if (name[0] != '\0') {
			if (strcmp(req->method, "GET") == 0) {
				handle_pkg_get_one(fd, name);
				return;
			}
			if (strcmp(req->method, "DELETE") == 0) {
				handle_pkg_delete(fd, name);
				return;
			}
		}
	}

	/*
	 * Anything outside /v1/... isn't part of the API contract at all --
	 * it's the web dashboard's static assets (docs/adr/0010). An
	 * unrecognized /v1/... path still falls through to the JSON 404
	 * below, unchanged.
	 */
	if (strncmp(req->path, "/v1/", 4) != 0) {
		if (strcmp(req->method, "GET") == 0)
			static_serve(fd, g_web_root, req->path);
		else
			respond_error(fd, 404, "Not Found", "no such endpoint");
		return;
	}

	respond_error(fd, 404, "Not Found", "no such endpoint");
}

static void handle_client_event(struct conn *cc)
{
	char buf[4096];
	ssize_t n;
	struct http_request req;
	int pr;

	n = read(cc->fd, buf, sizeof(buf));
	if (n <= 0) {
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		close(cc->fd);
		http_conn_free(&cc->http);
		free(cc);
		return;
	}

	if (http_conn_feed(&cc->http, buf, (size_t)n) != 0) {
		respond_error(cc->fd, 400, "Bad Request", "request too large");
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		close(cc->fd);
		http_conn_free(&cc->http);
		free(cc);
		return;
	}

	pr = http_conn_try_parse(&cc->http, &req);
	if (pr < 0) {
		respond_error(cc->fd, 400, "Bad Request", "malformed request");
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		close(cc->fd);
		http_conn_free(&cc->http);
		free(cc);
		return;
	}
	if (pr == 1) {
		dispatch(cc->fd, &req);
		kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
		close(cc->fd);
		http_conn_free(&cc->http);
		free(cc);
	}
	/* pr == 0: request incomplete, keep waiting on this fd. */
}

static void handle_container_event(struct conn *cc)
{
	struct registry_entry *entry = cc->entry;
	pid_t pkg_pid;
	int pkg_pidfd;
	int chained;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	registry_mark_exited(entry);
	entry->reactor_conn = NULL;
	free(cc);

	/*
	 * Unconditional, exactly like dns_record_forget_owner()/
	 * pki_cert_forget_owner() on every container delete -- pkg.c
	 * decides relevance (no-op unless this is PKG_BUILD_CONTAINER_NAME),
	 * so this hook stays trivial regardless of which container exited.
	 * A return of 1 means a dependency chain is advancing into another
	 * fetch -- register its pidfd exactly like a fresh top-level
	 * install already does.
	 */
	chained = pkg_build_completed(entry->name, entry->exit_status, &pkg_pid, &pkg_pidfd);
	if (strcmp(entry->name, PKG_BUILD_CONTAINER_NAME) == 0)
		registry_remove(entry->name);
	if (chained)
		register_pkg_fetch_pidfd(pkg_pid, pkg_pidfd);
}

/*
 * Mirrors handle_container_event()'s shape for the package fetch
 * step's plain fork()'d curl subprocess: reap it (non-blocking here --
 * EPOLLIN on its pidfd already means it has exited), hand the exit
 * status to pkg_fetch_completed(), and if it says a build should
 * start, spawn it through the exact same registry_create() +
 * register_container_pidfd() path every other container already
 * goes through -- one source of truth for "what's running," the
 * package build container included.
 */
static void handle_pkg_fetch_event(struct conn *cc)
{
	int status;
	int exit_status;
	struct container_spec spec;

	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	if (waitpid(cc->pkg_fetch_pid, &status, 0) == cc->pkg_fetch_pid && WIFEXITED(status))
		exit_status = WEXITSTATUS(status);
	else
		exit_status = -1;
	close(cc->fd);
	free(cc);

	if (pkg_fetch_completed(exit_status, &spec)) {
		struct registry_entry *entry;
		enum registry_error rerr =
		    registry_create(PKG_BUILD_CONTAINER_NAME, &spec, NULL, 0, 0, &entry);

		if (rerr != REGISTRY_OK)
			pkg_build_spawn_failed();
		else
			register_container_pidfd(entry);
	}
}

static void accept_loop(void)
{
	int client_fd;
	struct conn *cc;
	struct kx_epoll_event ev;

	for (;;) {
		client_fd = accept4(g_listener_conn.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (client_fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			perror("accept4");
			break;
		}

		cc = malloc(sizeof(*cc));
		if (cc == NULL) {
			/* Under memory pressure, drop this one connection
			 * attempt -- unlike register_container_pidfd(), no
			 * persistent state has been committed yet, so this
			 * isn't fatal. */
			close(client_fd);
			continue;
		}
		cc->kind = CONN_CLIENT;
		cc->fd = client_fd;
		http_conn_init(&cc->http);

		memset(&ev, 0, sizeof(ev));
		ev.events = EPOLLIN;
		ev.data.ptr = cc;
		if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, client_fd, &ev) != 0) {
			perror("epoll_ctl ADD client");
			close(client_fd);
			free(cc);
		}
	}
}

int main(int argc, char **argv)
{
	int port = DEFAULT_PORT;
	const char *bind_addr = DEFAULT_BIND;
	const char *web_root = DEFAULT_WEB_ROOT;
	int i;
	int listen_fd;
	int opt = 1;
	struct sockaddr_in addr;
	struct kx_epoll_event ev;
	struct sigaction sa;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--port=", 7) == 0)
			port = atoi(argv[i] + 7);
		else if (strncmp(argv[i], "--bind=", 7) == 0)
			bind_addr = argv[i] + 7;
		else if (strncmp(argv[i], "--web-root=", 11) == 0)
			web_root = argv[i] + 11;
	}
	g_web_root = web_root;

	if (ensure_dir(BASE_DIR) != 0 || ensure_dir(IMAGES_DIR) != 0 ||
	    ensure_dir(CONTAINERS_DIR) != 0 || ensure_dir(PKI_DIR) != 0 ||
	    ensure_dir(PKI_DIR "/certs") != 0 || ensure_dir(PKG_DIR) != 0)
		return 1;

	if (network_init(NETWORKS_STATE_PATH) != 0)
		return 1;
	if (dns_init(DNS_RECORDS_STATE_PATH) != 0)
		return 1;
	if (pki_init(PKI_DIR, PKI_CERTS_STATE_PATH) != 0)
		return 1;
	if (pkg_init(PKG_DIR, PKG_INSTALLED_STATE_PATH, CONTAINERS_DIR, IMAGES_DIR) != 0)
		return 1;

	registry_init();

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	/*
	 * SOCK_CLOEXEC/EPOLL_CLOEXEC everywhere below: container_create()
	 * clone3()'s a new process for every container this daemon runs.
	 * Without close-on-exec, that child inherits a duplicate of every
	 * fd we hold open at the moment of the clone -- including the
	 * client connection socket for whatever request triggered the
	 * container's creation. The kernel won't deliver EOF on that
	 * connection to the client until *every* reference to it closes,
	 * so an un-CLOEXEC'd fd would silently stall that HTTP response
	 * until the container itself exits, defeating the entire
	 * non-blocking reactor. The container's own execve() closes any
	 * CLOEXEC fd immediately, well before it does real work.
	 */
	listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
		fprintf(stderr, "invalid --bind address: %s\n", bind_addr);
		return 1;
	}

	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("bind");
		return 1;
	}
	if (listen(listen_fd, 128) != 0) {
		perror("listen");
		return 1;
	}

	g_epfd = epoll_create1(EPOLL_CLOEXEC);
	if (g_epfd < 0) {
		perror("epoll_create1");
		return 1;
	}

	g_listener_conn.kind = CONN_LISTENER;
	g_listener_conn.fd = listen_fd;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.ptr = &g_listener_conn;
	if (kx_epoll_ctl(g_epfd, EPOLL_CTL_ADD, listen_fd, &ev) != 0) {
		perror("epoll_ctl ADD listener");
		return 1;
	}

	printf("kanxeod listening on %s:%d\n", bind_addr, port);
	fflush(stdout);

	while (!g_stop) {
		struct kx_epoll_event events[MAX_EVENTS];
		int n = kx_epoll_wait(g_epfd, events, MAX_EVENTS, -1);
		int j;
		struct conn *cc;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait");
			break;
		}

		for (j = 0; j < n; j++) {
			cc = events[j].data.ptr;
			if (cc->kind == CONN_LISTENER)
				accept_loop();
			else if (cc->kind == CONN_CONTAINER)
				handle_container_event(cc);
			else if (cc->kind == CONN_PKG_FETCH)
				handle_pkg_fetch_event(cc);
			else
				handle_client_event(cc);
		}
	}

	close(listen_fd);
	close(g_epfd);
	printf("kanxeod shutting down\n");
	return 0;
}
