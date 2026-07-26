#include "network.h"
#include "registry.h"
#include "rtnetlink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct network_def g_networks[NETWORK_MAX];
static char g_state_path[PATH_MAX];

static int network_name_is_valid(const char *name)
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
	if (i >= NETWORK_NAME_MAX)
		return 0;
	return 1;
}

static uint32_t mask_for_prefix(int prefix_len)
{
	if (prefix_len <= 0)
		return 0;
	return (uint32_t)0xFFFFFFFFu << (32 - prefix_len);
}

static int host_max_for_prefix(int prefix_len)
{
	return (int)(1u << (32 - prefix_len)) - 2;
}

static int ranges_overlap(uint32_t a_base_be, int a_prefix, uint32_t b_base_be, int b_prefix)
{
	int common = a_prefix < b_prefix ? a_prefix : b_prefix;
	uint32_t mask = mask_for_prefix(common);

	return (ntohl(a_base_be) & mask) == (ntohl(b_base_be) & mask);
}

/*
 * Rewrites the persisted file atomically: a crash mid-write must
 * never corrupt this state, since it's the only record of which
 * bridges this daemon is responsible for across a restart. Writes to
 * a temp file in the same directory, fsyncs, then renames over the
 * real path (rename() is atomic on the same filesystem).
 */
static int save_state(void)
{
	char tmp_path[PATH_MAX + 8];
	struct json_writer w;
	int fd;
	ssize_t written;
	size_t len;

	if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_state_path) >= (int)sizeof(tmp_path))
		return -1;

	jw_init(&w);
	network_write_json_list(&w);
	len = w.len;

	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		jw_free(&w);
		return -1;
	}
	written = write(fd, w.buf, len);
	jw_free(&w);
	if (written < 0 || (size_t)written != len) {
		close(fd);
		return -1;
	}
	if (fsync(fd) != 0) {
		close(fd);
		return -1;
	}
	close(fd);

	if (rename(tmp_path, g_state_path) != 0)
		return -1;
	return 0;
}

/*
 * Parses one persisted entry into slot. Deliberately strict: this
 * file is only ever written by network_create()'s validated path, so
 * a malformed entry means real corruption (disk fault, manual
 * tampering) -- silently dropping it would be exactly the "restart
 * forgets a live bridge" bug this module exists to prevent. Returns 0
 * on success, -1 on any validation failure.
 */
static int parse_persisted_entry(const struct json_value *item, struct network_def *slot, int idx)
{
	const char *name = json_as_string(json_object_get(item, "name"));
	const char *subnet = json_as_string(json_object_get(item, "subnet"));
	const struct json_value *jprefix = json_object_get(item, "prefix_len");
	int prefix_len;
	struct in_addr addr;
	int i;

	if (!network_name_is_valid(name) || subnet == NULL || jprefix == NULL)
		return -1;
	prefix_len = (int)json_as_number(jprefix);
	if (prefix_len < 8 || prefix_len > 30)
		return -1;
	if (inet_pton(AF_INET, subnet, &addr) != 1)
		return -1;
	if ((ntohl(addr.s_addr) & ~mask_for_prefix(prefix_len)) != 0)
		return -1;

	for (i = 0; i < idx; i++) {
		if (strcmp(g_networks[i].name, name) == 0)
			return -1;
		if (ranges_overlap(addr.s_addr, prefix_len, g_networks[i].base_be,
		                    g_networks[i].prefix_len))
			return -1;
	}

	memset(slot, 0, sizeof(*slot));
	strncpy(slot->name, name, sizeof(slot->name) - 1);
	slot->base_be = addr.s_addr;
	slot->prefix_len = prefix_len;
	slot->gateway_be = htonl(ntohl(addr.s_addr) | 1);
	slot->in_use = 1;
	return 0;
}

static int load_state(void)
{
	int fd;
	struct stat st;
	char *buf;
	ssize_t n;
	struct json_value *root;
	size_t i;
	int rc = 0;

	fd = open(g_state_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT)
			return 0;
		perror(g_state_path);
		return -1;
	}
	if (fstat(fd, &st) != 0) {
		close(fd);
		return -1;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (buf == NULL) {
		close(fd);
		return -1;
	}
	n = read(fd, buf, (size_t)st.st_size);
	close(fd);
	if (n < 0 || (size_t)n != (size_t)st.st_size) {
		free(buf);
		return -1;
	}
	buf[n] = '\0';

	root = json_parse(buf, (size_t)n);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted network state\n", g_state_path);
		return -1;
	}
	if (root->u.array.count > NETWORK_MAX) {
		json_free(root);
		fprintf(stderr, "%s: more networks persisted than NETWORK_MAX\n", g_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count; i++) {
		if (parse_persisted_entry(root->u.array.items[i], &g_networks[i], (int)i) != 0) {
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
			rc = -1;
			break;
		}
	}
	json_free(root);
	return rc;
}

int network_init(const char *state_path)
{
	int fd;
	int i;

	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;

	memset(g_networks, 0, sizeof(g_networks));
	if (load_state() != 0)
		return -1;

	fd = rtnl_open();
	if (fd < 0) {
		perror("rtnl_open (network init)");
		return -1;
	}
	for (i = 0; i < NETWORK_MAX; i++) {
		if (!g_networks[i].in_use)
			continue;
		if (rtnl_bridge_create(fd, g_networks[i].name) != 0 && errno != EEXIST) {
			perror("rtnl_bridge_create");
			rtnl_close(fd);
			return -1;
		}
		if (rtnl_addr_add_ipv4(fd, g_networks[i].name, g_networks[i].gateway_be,
		                        g_networks[i].prefix_len) != 0 &&
		    errno != EEXIST) {
			perror("rtnl_addr_add_ipv4 (bridge gateway)");
			rtnl_close(fd);
			return -1;
		}
		if (rtnl_link_set_up(fd, g_networks[i].name) != 0) {
			perror("rtnl_link_set_up (bridge)");
			rtnl_close(fd);
			return -1;
		}
	}
	rtnl_close(fd);
	return 0;
}

struct network_def *network_find(const char *name)
{
	int i;

	for (i = 0; i < NETWORK_MAX; i++) {
		if (g_networks[i].in_use && strcmp(g_networks[i].name, name) == 0)
			return &g_networks[i];
	}
	return NULL;
}

enum network_error network_create(const char *name, const char *subnet_str, int prefix_len,
                                   struct network_def **out)
{
	struct in_addr addr;
	int i, slot = -1;
	int fd;
	struct network_def *e;

	if (!network_name_is_valid(name))
		return NETWORK_ERR_INVALID_NAME;
	if (network_find(name) != NULL)
		return NETWORK_ERR_DUPLICATE;

	if (prefix_len < 8 || prefix_len > 30 || inet_pton(AF_INET, subnet_str, &addr) != 1)
		return NETWORK_ERR_INVALID_SUBNET;
	if ((ntohl(addr.s_addr) & ~mask_for_prefix(prefix_len)) != 0)
		return NETWORK_ERR_INVALID_SUBNET;

	for (i = 0; i < NETWORK_MAX; i++) {
		if (g_networks[i].in_use &&
		    ranges_overlap(addr.s_addr, prefix_len, g_networks[i].base_be,
		                    g_networks[i].prefix_len))
			return NETWORK_ERR_OVERLAP;
	}

	for (i = 0; i < NETWORK_MAX; i++) {
		if (!g_networks[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return NETWORK_ERR_FULL;

	e = &g_networks[slot];
	memset(e, 0, sizeof(*e));
	strncpy(e->name, name, sizeof(e->name) - 1);
	e->base_be = addr.s_addr;
	e->prefix_len = prefix_len;
	e->gateway_be = htonl(ntohl(addr.s_addr) | 1);

	fd = rtnl_open();
	if (fd < 0)
		return NETWORK_ERR_CREATE_FAILED;
	if (rtnl_bridge_create(fd, e->name) != 0) {
		rtnl_close(fd);
		return NETWORK_ERR_CREATE_FAILED;
	}
	if (rtnl_addr_add_ipv4(fd, e->name, e->gateway_be, e->prefix_len) != 0) {
		rtnl_link_delete(fd, e->name);
		rtnl_close(fd);
		return NETWORK_ERR_CREATE_FAILED;
	}
	if (rtnl_link_set_up(fd, e->name) != 0) {
		rtnl_link_delete(fd, e->name);
		rtnl_close(fd);
		return NETWORK_ERR_CREATE_FAILED;
	}
	rtnl_close(fd);

	e->in_use = 1;
	if (save_state() != 0) {
		/*
		 * Can't persist -- a restart would forget this bridge while
		 * it keeps running in the kernel, exactly the bug this
		 * module exists to prevent. Roll back rather than report
		 * success on a lie.
		 */
		e->in_use = 0;
		fd = rtnl_open();
		if (fd >= 0) {
			rtnl_link_delete(fd, e->name);
			rtnl_close(fd);
		}
		return NETWORK_ERR_CREATE_FAILED;
	}

	*out = e;
	return NETWORK_OK;
}

enum network_error network_delete(const char *name)
{
	struct network_def *e = network_find(name);
	int fd;

	if (e == NULL)
		return NETWORK_ERR_NOT_FOUND;
	if (registry_network_in_use(name))
		return NETWORK_ERR_IN_USE;

	fd = rtnl_open();
	if (fd < 0 || rtnl_link_delete(fd, name) != 0) {
		if (fd >= 0)
			rtnl_close(fd);
		return NETWORK_ERR_DELETE_FAILED;
	}
	rtnl_close(fd);

	e->in_use = 0;
	if (save_state() != 0)
		return NETWORK_ERR_DELETE_FAILED;
	return NETWORK_OK;
}

int network_alloc_ip(const char *name, uint32_t *out_ip_be)
{
	struct network_def *net = network_find(name);

	if (net == NULL)
		return -1;
	return registry_alloc_ip(net->base_be, 2, host_max_for_prefix(net->prefix_len), out_ip_be);
}

void network_write_json_one(const struct network_def *net, struct json_writer *w)
{
	struct in_addr a;
	char subnet_str[INET_ADDRSTRLEN];
	char gateway_str[INET_ADDRSTRLEN];

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, net->name);
	a.s_addr = net->base_be;
	inet_ntop(AF_INET, &a, subnet_str, sizeof(subnet_str));
	jw_key(w, "subnet");
	jw_str(w, subnet_str);
	jw_key(w, "prefix_len");
	jw_int(w, net->prefix_len);
	a.s_addr = net->gateway_be;
	inet_ntop(AF_INET, &a, gateway_str, sizeof(gateway_str));
	jw_key(w, "gateway");
	jw_str(w, gateway_str);
	jw_obj_close(w);
}

void network_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < NETWORK_MAX; i++) {
		if (g_networks[i].in_use)
			network_write_json_one(&g_networks[i], w);
	}
	jw_arr_close(w);
}
