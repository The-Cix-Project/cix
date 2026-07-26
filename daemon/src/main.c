#include "container.h"
#include "http.h"
#include "json.h"
#include "linux_compat.h"
#include "network.h"
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
#include <unistd.h>

#define DEFAULT_PORT 7620
#define DEFAULT_BIND "127.0.0.1"
#define DEFAULT_WEB_ROOT "web"
#define BASE_DIR "/var/lib/kanxeo"
#define IMAGES_DIR BASE_DIR "/images"
#define CONTAINERS_DIR BASE_DIR "/containers"
#define NETWORKS_STATE_PATH BASE_DIR "/networks.json"
#define MAX_EVENTS 64
#define CONTAINERS_PREFIX "/v1/containers/"
#define NETWORKS_PREFIX "/v1/networks/"

enum conn_kind { CONN_LISTENER, CONN_CLIENT, CONN_CONTAINER };

struct conn {
	enum conn_kind kind;
	int fd;
	struct http_conn http;        /* CONN_CLIENT only */
	struct registry_entry *entry; /* CONN_CONTAINER only */
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

static void handle_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jname, *jimage, *jcmd, *jmem, *jpids, *jnetwork;
	const char *name, *image, *network;
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
	uint32_t ip_be = 0;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	jname = json_object_get(root, "name");
	jimage = json_object_get(root, "image");
	jcmd = json_object_get(root, "cmd");
	jnetwork = json_object_get(root, "network");
	name = json_as_string(jname);
	image = json_as_string(jimage);
	network = json_as_string(jnetwork);

	if (!name_is_valid(name) || image == NULL || image[0] == '\0' || jcmd == NULL ||
	    jcmd->type != JSON_ARRAY || jcmd->u.array.count == 0 ||
	    jcmd->u.array.count >= (sizeof(argv_buf) / sizeof(argv_buf[0]))) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name/image/cmd missing or invalid");
		return;
	}
	if (network != NULL && network[0] != '\0' && network_find(network) == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "unknown network");
		return;
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

	if (network != NULL && network[0] != '\0') {
		if (network_alloc_ip(network, &ip_be) != 0) {
			json_free(root);
			respond_error(fd, 500, "Internal Server Error", "no free IP addresses");
			return;
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
	if (ip_be != 0) {
		struct network_def *net = network_find(network);

		spec.net.bridge = net->name;
		spec.net.container_ip_be = ip_be;
		spec.net.gateway_ip_be = net->gateway_be;
		spec.net.prefix_len = net->prefix_len;
	}
	spec.argv = argv_buf;
	spec.envp = empty_envp;

	rerr = registry_create(name, &spec, ip_be, network, &entry);
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
	kx_epoll_ctl(g_epfd, EPOLL_CTL_DEL, cc->fd, NULL);
	registry_mark_exited(cc->entry);
	cc->entry->reactor_conn = NULL;
	free(cc);
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
	    ensure_dir(CONTAINERS_DIR) != 0)
		return 1;

	if (network_init(NETWORKS_STATE_PATH) != 0)
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
			else
				handle_client_event(cc);
		}
	}

	close(listen_fd);
	close(g_epfd);
	printf("kanxeod shutting down\n");
	return 0;
}
