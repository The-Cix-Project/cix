#include "syslogfwd.h"
#include "logstore.h"
#include "persist.h"
#include "registry.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static char g_targets[SYSLOG_TARGET_MAX][SYSLOG_TARGET_NAME_MAX];
static char g_state_path[512];
static int g_sockfd = -1;

/* ---- persistence + registration bookkeeping, mirrors ntp.c's own
 * server-list section exactly (JSON array of {"container": "..."}) ---- */

static int find_target_slot(const char *container_name)
{
	int i;

	for (i = 0; i < SYSLOG_TARGET_MAX; i++) {
		if (strcmp(g_targets[i], container_name) == 0)
			return i;
	}
	return -1;
}

static int save_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	syslogfwd_target_write_json_list(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static void load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int count = 0;

	memset(g_targets, 0, sizeof(g_targets));
	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return;
	if (buf == NULL)
		return; /* no persisted state yet -- first-ever startup */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted syslog forward-target state\n", g_state_path);
		return;
	}

	for (i = 0; i < root->u.array.count && count < SYSLOG_TARGET_MAX; i++) {
		const char *container = json_as_string(json_object_get(root->u.array.items[i], "container"));

		if (container == NULL || container[0] == '\0')
			continue; /* skip a corrupt entry rather than fail the whole load */
		snprintf(g_targets[count], sizeof(g_targets[count]), "%s", container);
		count++;
	}
	json_free(root);
}

void syslogfwd_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

int syslogfwd_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;

	load_state();

	g_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (g_sockfd < 0)
		return -1;
	fcntl(g_sockfd, F_SETFL, fcntl(g_sockfd, F_GETFL, 0) | O_NONBLOCK);
	return 0;
}

enum syslogfwd_error syslogfwd_target_register(const char *container_name)
{
	struct registry_entry *entry;
	int slot = -1;
	int i;

	if (container_name == NULL || container_name[0] == '\0')
		return SYSLOGFWD_ERR_CONTAINER_NOT_FOUND;
	if (find_target_slot(container_name) >= 0)
		return SYSLOGFWD_ERR_DUPLICATE;

	entry = registry_find(container_name);
	if (entry == NULL)
		return SYSLOGFWD_ERR_CONTAINER_NOT_FOUND;
	if (!entry->running)
		return SYSLOGFWD_ERR_CONTAINER_NOT_RUNNING;

	for (i = 0; i < SYSLOG_TARGET_MAX; i++) {
		if (g_targets[i][0] == '\0') {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return SYSLOGFWD_ERR_FULL;

	snprintf(g_targets[slot], sizeof(g_targets[slot]), "%s", container_name);
	if (save_state() != 0) {
		g_targets[slot][0] = '\0';
		return SYSLOGFWD_ERR_PERSIST_FAILED;
	}
	return SYSLOGFWD_OK;
}

enum syslogfwd_error syslogfwd_target_unregister(const char *container_name)
{
	int slot = find_target_slot(container_name);

	if (slot < 0)
		return SYSLOGFWD_ERR_NOT_FOUND;
	g_targets[slot][0] = '\0';
	save_state();
	return SYSLOGFWD_OK;
}

void syslogfwd_target_forget(const char *container_name)
{
	int slot = find_target_slot(container_name);

	if (slot >= 0) {
		g_targets[slot][0] = '\0';
		save_state();
	}
}

void syslogfwd_target_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < SYSLOG_TARGET_MAX; i++) {
		if (g_targets[i][0] == '\0')
			continue;
		jw_obj_open(w);
		jw_key(w, "container");
		jw_str(w, g_targets[i]);
		jw_obj_close(w);
	}
	jw_arr_close(w);
}

/* ---- the actual fire-and-forget RFC 3164 UDP send ---- */

#define SYSLOG_FACILITY_LOCAL0 16
#define SYSLOG_DATAGRAM_MAX 1024 /* RFC 3164's own traditional datagram-size guidance */

void syslogfwd_send(const char *container, const char *level, const char *msg)
{
	int i;
	int severity;
	int pri;
	char timestr[16]; /* "Mmm dd hh:mm:ss\0" */
	char datagram[SYSLOG_DATAGRAM_MAX];
	int dlen;
	time_t now;
	struct tm tm_now;

	if (g_sockfd < 0)
		return;

	for (i = 0; i < SYSLOG_TARGET_MAX; i++) {
		if (g_targets[i][0] == '\0')
			continue;
		if (strcmp(g_targets[i], container) == 0)
			continue; /* never forward a syslog target's own captured stdout back to itself */
		{
			struct registry_entry *entry = registry_find(g_targets[i]);
			struct sockaddr_in addr;
			struct in_addr a;

			if (entry == NULL || !entry->running)
				continue;

			severity = logstore_level_severity(level);
			pri = SYSLOG_FACILITY_LOCAL0 * 8 + severity;

			now = time(NULL);
			localtime_r(&now, &tm_now);
			strftime(timestr, sizeof(timestr), "%b %e %T", &tm_now);

			dlen = snprintf(datagram, sizeof(datagram), "<%d>%s %s thincd: %s", pri, timestr,
			                 container, msg);
			if (dlen < 0)
				continue;
			if (dlen >= (int)sizeof(datagram))
				dlen = (int)sizeof(datagram) - 1;

			memset(&addr, 0, sizeof(addr));
			addr.sin_family = AF_INET;
			addr.sin_port = htons(SYSLOG_UDP_PORT);
			a.s_addr = entry->nets[0].ip_be;
			addr.sin_addr = a;

			/* Best-effort: EAGAIN/ENETUNREACH/etc are silently dropped, see
			 * this file's own header comment -- UDP syslog has never had
			 * delivery guarantees, and logstore.c already holds the
			 * durable copy of everything sent here. */
			(void)sendto(g_sockfd, datagram, (size_t)dlen, 0, (struct sockaddr *)&addr, sizeof(addr));
		}
	}
}
