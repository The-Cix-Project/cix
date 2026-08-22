#include "ntp.h"
#include "persist.h"
#include "registry.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * SNTP/NTPv4 client-mode wire packet, RFC 5905 Figure 8 -- the fixed
 * 48-byte layout every implementation on the wire agrees on. #pragma
 * pack is required here regardless of TCC's own known quirk (TCC
 * ignores __attribute__((packed)) entirely, ADR-0008): every field
 * below is already naturally aligned up through reference_id (12
 * bytes, a multiple of 4), but the four 8-byte timestamp pairs that
 * follow would otherwise get padded to 8-byte alignment by any
 * compiler, breaking the real 48-byte wire size. Each 64-bit NTP
 * timestamp is represented as two 32-bit halves (whole seconds since
 * the NTP epoch, then a binary fraction of a second) rather than one
 * uint64_t, so byte-order conversion stays a plain ntohl()/htonl()
 * per half with no 64-bit byte-swap helper needed.
 */
#pragma pack(push, 1)
struct ntp_wire_packet {
	uint8_t li_vn_mode;
	uint8_t stratum;
	uint8_t poll;
	int8_t precision;
	uint32_t root_delay;
	uint32_t root_dispersion;
	uint32_t reference_id;
	uint32_t reference_ts_sec;
	uint32_t reference_ts_frac;
	uint32_t origin_ts_sec;
	uint32_t origin_ts_frac;
	uint32_t receive_ts_sec;
	uint32_t receive_ts_frac;
	uint32_t transmit_ts_sec;
	uint32_t transmit_ts_frac;
};
#pragma pack(pop)

/* NTP's own epoch is 1900-01-01; Unix's is 1970-01-01 -- 70 years,
 * 17 of them leap, in seconds. A real, known, accepted limitation:
 * this constant (and the unsigned 32-bit seconds field it offsets)
 * is only valid for NTP timestamps before the well-known 2036
 * rollover -- the same boundary every NTPv4 implementation has, not
 * something specific to this hand-rolled client, and far enough out
 * not to matter for this project's own real deployment horizon. */
#define NTP_UNIX_EPOCH_DELTA 2208988800U

#define NTP_LI_VN_MODE_CLIENT 0x23 /* LI=0 (no warning), VN=4, Mode=3 (client) */
#define NTP_MODE_SERVER 4

#define NTP_SYNC_MAX_CANDIDATES (NTP_SERVER_MAX + NTP_MAX_UPSTREAM)

static char g_upstream[NTP_MAX_UPSTREAM][NTP_IP_STRLEN];
static int g_upstream_count;
static char g_state_path[512];

static char g_servers[NTP_SERVER_MAX][NTP_SERVER_NAME_MAX];
static char g_servers_state_path[512];

static struct {
	int active;
	char candidates[NTP_SYNC_MAX_CANDIDATES][NTP_IP_STRLEN];
	int candidate_count;
	int candidate_index;
	struct sockaddr_in target_addr;
	uint32_t sent_origin_sec;
	uint32_t sent_origin_frac;
	struct timespec t1; /* our own local send time for the current candidate */
} g_job;

static enum ntp_sync_result g_last_result = NTP_SYNC_NEVER;
static char g_last_synced_from[NTP_IP_STRLEN];
static time_t g_last_sync_unixtime;

/* ---- persistence: upstream list (plain "server A.B.C.D" lines, same
 * literal-format convention resolv.c's own nameserver list already
 * uses -- no JSON wrapper needed for a bare address list) ---- */

static int save_upstream_state(void)
{
	char buf[NTP_MAX_UPSTREAM * (NTP_IP_STRLEN + 16)];
	size_t off = 0;
	int i;

	for (i = 0; i < g_upstream_count; i++) {
		int written = snprintf(buf + off, sizeof(buf) - off, "server %s\n", g_upstream[i]);

		if (written < 0 || (size_t)written >= sizeof(buf) - off)
			return -1; /* unreachable given the size bound above and NTP_MAX_UPSTREAM's own cap */
		off += (size_t)written;
	}
	return persist_atomic_write(g_state_path, buf, off);
}

static void load_upstream_state(void)
{
	FILE *f;
	char line[128];

	g_upstream_count = 0;
	f = fopen(g_state_path, "r");
	if (f == NULL)
		return; /* nothing configured yet -- not an error */

	while (g_upstream_count < NTP_MAX_UPSTREAM && fgets(line, sizeof(line), f) != NULL) {
		char ip[NTP_IP_STRLEN];
		struct in_addr a;

		if (sscanf(line, "server %15s", ip) != 1)
			continue;
		if (inet_pton(AF_INET, ip, &a) != 1)
			continue; /* a hand-edited or corrupt line -- skip it, don't fail startup over it */
		snprintf(g_upstream[g_upstream_count], sizeof(g_upstream[g_upstream_count]), "%s", ip);
		g_upstream_count++;
	}
	fclose(f);
}

/* ---- persistence: registered server-container bindings (a plain
 * JSON array of container-name strings -- simpler than DNS/LDAP's own
 * server bindings, which need to persist a config/hosts path
 * alongside the name; this resource has no equivalent field, see
 * ntp.h's own header comment on why registration here is pure
 * bookkeeping) ---- */

static int find_server_slot(const char *container_name)
{
	int i;

	for (i = 0; i < NTP_SERVER_MAX; i++) {
		if (strcmp(g_servers[i], container_name) == 0)
			return i;
	}
	return -1;
}

static int save_server_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	ntp_server_write_json_list(&w);
	rc = persist_atomic_write(g_servers_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static void load_server_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int count = 0;

	memset(g_servers, 0, sizeof(g_servers));
	if (persist_read_file(g_servers_state_path, &buf, &len) != 0)
		return;
	if (buf == NULL)
		return; /* no persisted state yet -- first-ever startup */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted NTP server binding state\n", g_servers_state_path);
		return;
	}

	for (i = 0; i < root->u.array.count && count < NTP_SERVER_MAX; i++) {
		const char *container = json_as_string(json_object_get(root->u.array.items[i], "container"));

		if (container == NULL || container[0] == '\0')
			continue; /* skip a corrupt entry rather than fail the whole load */
		snprintf(g_servers[count], sizeof(g_servers[count]), "%s", container);
		count++;
	}
	json_free(root);
}

void ntp_repoint(const char *new_state_path, const char *new_servers_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
	snprintf(g_servers_state_path, sizeof(g_servers_state_path), "%s", new_servers_state_path);
}

int ntp_init(const char *state_path, const char *servers_state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	if (snprintf(g_servers_state_path, sizeof(g_servers_state_path), "%s", servers_state_path) >=
	    (int)sizeof(g_servers_state_path))
		return -1;

	load_upstream_state();
	load_server_state();
	memset(&g_job, 0, sizeof(g_job));
	g_last_result = NTP_SYNC_NEVER;
	g_last_synced_from[0] = '\0';
	g_last_sync_unixtime = 0;
	return 0;
}

enum ntp_error ntp_set_upstream(const char *const *addrs, int count)
{
	int i;

	if (count > NTP_MAX_UPSTREAM)
		return NTP_ERR_TOO_MANY;
	for (i = 0; i < count; i++) {
		struct in_addr a;

		if (addrs[i] == NULL || inet_pton(AF_INET, addrs[i], &a) != 1)
			return NTP_ERR_INVALID_IP;
	}

	g_upstream_count = count;
	for (i = 0; i < count; i++)
		snprintf(g_upstream[i], sizeof(g_upstream[i]), "%s", addrs[i]);

	if (save_upstream_state() != 0)
		return NTP_ERR_PERSIST_FAILED;
	return NTP_OK;
}

void ntp_write_json_config(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "upstream");
	jw_arr_open(w);
	for (i = 0; i < g_upstream_count; i++)
		jw_str(w, g_upstream[i]);
	jw_arr_close(w);
	jw_obj_close(w);
}

enum ntp_error ntp_server_register(const char *container_name)
{
	struct registry_entry *entry;
	int slot = -1;
	int i;

	if (container_name == NULL || container_name[0] == '\0')
		return NTP_ERR_CONTAINER_NOT_FOUND;
	if (find_server_slot(container_name) >= 0)
		return NTP_ERR_DUPLICATE;

	entry = registry_find(container_name);
	if (entry == NULL)
		return NTP_ERR_CONTAINER_NOT_FOUND;
	if (!entry->running)
		return NTP_ERR_CONTAINER_NOT_RUNNING;

	for (i = 0; i < NTP_SERVER_MAX; i++) {
		if (g_servers[i][0] == '\0') {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return NTP_ERR_FULL;

	snprintf(g_servers[slot], sizeof(g_servers[slot]), "%s", container_name);
	if (save_server_state() != 0) {
		g_servers[slot][0] = '\0';
		return NTP_ERR_PERSIST_FAILED;
	}
	return NTP_OK;
}

enum ntp_error ntp_server_unregister(const char *container_name)
{
	int slot = find_server_slot(container_name);

	if (slot < 0)
		return NTP_ERR_NOT_FOUND;
	g_servers[slot][0] = '\0';
	save_server_state();
	return NTP_OK;
}

void ntp_server_forget(const char *container_name)
{
	int slot = find_server_slot(container_name);

	if (slot >= 0) {
		g_servers[slot][0] = '\0';
		save_server_state();
	}
}

/* Writes the bare array value only (matching dns_server_write_json_
 * list()'s own convention) -- the caller wraps it in {"servers": [...]}
 * both for the persisted-state save (save_server_state() above) and
 * for GET /v1/ntp/servers's own response (main.c). */
/* Issue #81: uniform enumerator, see dns_server_list_containers(). */
int ntp_server_list_containers(char out[][NTP_SERVER_NAME_MAX], int max)
{
	int i, n = 0;

	for (i = 0; i < NTP_SERVER_MAX && n < max; i++) {
		if (g_servers[i][0] != '\0')
			snprintf(out[n++], NTP_SERVER_NAME_MAX, "%s", g_servers[i]);
	}
	return n;
}

void ntp_server_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < NTP_SERVER_MAX; i++) {
		if (g_servers[i][0] == '\0')
			continue;
		jw_obj_open(w);
		jw_key(w, "container");
		jw_str(w, g_servers[i]);
		jw_obj_close(w);
	}
	jw_arr_close(w);
}

/* ---- the actual SNTP client ---- */

static uint64_t timespec_to_ntp(const struct timespec *ts)
{
	uint32_t sec = (uint32_t)ts->tv_sec + NTP_UNIX_EPOCH_DELTA;
	uint32_t frac = (uint32_t)((double)ts->tv_nsec * 4294967296.0 / 1e9);

	return ((uint64_t)sec << 32) | frac;
}

static double ntp_halves_to_seconds(uint32_t sec, uint32_t frac)
{
	return (double)(sec - NTP_UNIX_EPOCH_DELTA) + (double)frac / 4294967296.0;
}

static void build_candidate_list(void)
{
	int i;
	struct in_addr a;
	char ipstr[NTP_IP_STRLEN];

	g_job.candidate_count = 0;

	for (i = 0; i < NTP_SERVER_MAX && g_job.candidate_count < NTP_SYNC_MAX_CANDIDATES; i++) {
		struct registry_entry *entry;

		if (g_servers[i][0] == '\0')
			continue;
		entry = registry_find(g_servers[i]);
		if (entry == NULL || !entry->running)
			continue;
		a.s_addr = entry->nets[0].ip_be;
		inet_ntop(AF_INET, &a, ipstr, sizeof(ipstr));
		snprintf(g_job.candidates[g_job.candidate_count], NTP_IP_STRLEN, "%s", ipstr);
		g_job.candidate_count++;
	}
	for (i = 0; i < g_upstream_count && g_job.candidate_count < NTP_SYNC_MAX_CANDIDATES; i++) {
		snprintf(g_job.candidates[g_job.candidate_count], NTP_IP_STRLEN, "%s", g_upstream[i]);
		g_job.candidate_count++;
	}
}

/* Sends one SNTP client request to g_job.candidates[g_job.candidate_index]
 * over sockfd. Records t1/the origin timestamp we sent so
 * ntp_sync_handle_reply() can validate the eventual response. */
static int send_query_to_current_candidate(int sockfd)
{
	struct ntp_wire_packet pkt;
	uint64_t t1_ntp;
	struct in_addr a;

	memset(&pkt, 0, sizeof(pkt));
	pkt.li_vn_mode = NTP_LI_VN_MODE_CLIENT;

	clock_gettime(CLOCK_REALTIME, &g_job.t1);
	t1_ntp = timespec_to_ntp(&g_job.t1);
	g_job.sent_origin_sec = (uint32_t)(t1_ntp >> 32);
	g_job.sent_origin_frac = (uint32_t)(t1_ntp & 0xffffffffU);
	pkt.transmit_ts_sec = htonl(g_job.sent_origin_sec);
	pkt.transmit_ts_frac = htonl(g_job.sent_origin_frac);

	memset(&g_job.target_addr, 0, sizeof(g_job.target_addr));
	g_job.target_addr.sin_family = AF_INET;
	g_job.target_addr.sin_port = htons(NTP_PORT);
	if (inet_pton(AF_INET, g_job.candidates[g_job.candidate_index], &a) != 1)
		return -1;
	g_job.target_addr.sin_addr = a;

	if (sendto(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&g_job.target_addr,
	           sizeof(g_job.target_addr)) != (ssize_t)sizeof(pkt))
		return -1;
	return 0;
}

enum ntp_start_error ntp_sync_start(int *out_sockfd)
{
	int fd;

	if (g_job.active)
		return NTP_START_BUSY;

	build_candidate_list();
	if (g_job.candidate_count == 0)
		return NTP_START_NO_CANDIDATES;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return NTP_START_SOCKET_FAILED;

	g_job.candidate_index = 0;
	if (send_query_to_current_candidate(fd) != 0) {
		close(fd);
		return NTP_START_SOCKET_FAILED;
	}

	g_job.active = 1;
	*out_sockfd = fd;
	return NTP_START_OK;
}

/* Shared by ntp_sync_handle_reply() (an unparseable/mismatched reply)
 * and ntp_sync_handle_timeout(): moves on to the next candidate on
 * the same socket, or finishes the job (exhausted) if none remain.
 * Returns 1 if the job is now finished (caller tears down), 0 if it
 * re-sent on the next candidate (caller re-arms the timeout only). */
static int advance_to_next_candidate(int sockfd)
{
	g_job.candidate_index++;
	if (g_job.candidate_index >= g_job.candidate_count) {
		g_job.active = 0;
		g_last_result = NTP_SYNC_FAILED;
		return 1;
	}
	if (send_query_to_current_candidate(sockfd) != 0) {
		g_job.active = 0;
		g_last_result = NTP_SYNC_FAILED;
		return 1;
	}
	return 0;
}

int ntp_sync_handle_reply(int sockfd)
{
	struct ntp_wire_packet pkt;
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	ssize_t n;
	struct timespec t4_ts;
	double t1, t2, t3, t4, offset;
	struct timespec new_time;
	uint32_t origin_sec, origin_frac;

	n = recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromlen);
	if (n != (ssize_t)sizeof(pkt))
		return 0; /* short/garbage read -- stay PENDING, a real reply may still arrive */

	/* Not from the candidate we're currently waiting on (a stray
	 * unrelated packet, or a late reply from an earlier candidate this
	 * job already moved past) -- ignore, stay PENDING. */
	if (from.sin_addr.s_addr != g_job.target_addr.sin_addr.s_addr)
		return 0;

	origin_sec = ntohl(pkt.origin_ts_sec);
	origin_frac = ntohl(pkt.origin_ts_frac);
	if (origin_sec != g_job.sent_origin_sec || origin_frac != g_job.sent_origin_frac)
		return 0; /* doesn't echo back what we sent -- not a real matching reply, stay PENDING */

	if ((pkt.li_vn_mode & 0x07) != NTP_MODE_SERVER)
		return 0; /* not actually a server response */

	clock_gettime(CLOCK_REALTIME, &t4_ts);
	t1 = (double)g_job.t1.tv_sec + (double)g_job.t1.tv_nsec / 1e9;
	t2 = ntp_halves_to_seconds(ntohl(pkt.receive_ts_sec), ntohl(pkt.receive_ts_frac));
	t3 = ntp_halves_to_seconds(ntohl(pkt.transmit_ts_sec), ntohl(pkt.transmit_ts_frac));
	t4 = (double)t4_ts.tv_sec + (double)t4_ts.tv_nsec / 1e9;

	/* RFC 5905's own client offset calculation: theta = ((T2-T1)+(T3-T4))/2 */
	offset = ((t2 - t1) + (t3 - t4)) / 2.0;

	new_time.tv_sec = (time_t)(t4 + offset);
	new_time.tv_nsec = (long)(((t4 + offset) - (double)new_time.tv_sec) * 1e9);
	if (new_time.tv_nsec < 0) {
		new_time.tv_nsec += 1000000000L;
		new_time.tv_sec -= 1;
	}

	if (clock_settime(CLOCK_REALTIME, &new_time) != 0) {
		/* A real, reportable failure (e.g. no CAP_SYS_TIME) -- distinct
		 * from "no candidate answered": the reply itself was genuine and
		 * valid, applying it is what failed. Still counts this candidate
		 * as exhausted rather than retried, since retrying won't change
		 * a permission failure. */
		g_job.active = 0;
		g_last_result = NTP_SYNC_FAILED;
		return 1;
	}

	snprintf(g_last_synced_from, sizeof(g_last_synced_from), "%s",
	         g_job.candidates[g_job.candidate_index]);
	g_last_sync_unixtime = new_time.tv_sec;
	g_last_result = NTP_SYNC_OK;
	g_job.active = 0;
	return 1;
}

int ntp_sync_handle_timeout(int sockfd)
{
	return advance_to_next_candidate(sockfd);
}

void ntp_write_json_status(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "state");
	switch (g_last_result) {
	case NTP_SYNC_OK:
		jw_str(w, "ok");
		break;
	case NTP_SYNC_FAILED:
		jw_str(w, "failed");
		break;
	default:
		jw_str(w, "never");
		break;
	}
	jw_key(w, "synced_from");
	if (g_last_synced_from[0] != '\0')
		jw_str(w, g_last_synced_from);
	else
		jw_null(w);
	jw_key(w, "last_sync_unixtime");
	if (g_last_sync_unixtime != 0)
		jw_int(w, (long long)g_last_sync_unixtime);
	else
		jw_null(w);
	jw_obj_close(w);
}

enum ntp_error ntp_time_set(int64_t unix_seconds)
{
	struct timespec ts;

	if (unix_seconds < 0)
		return NTP_ERR_INVALID_TIME;
	ts.tv_sec = (time_t)unix_seconds;
	ts.tv_nsec = 0;
	if (clock_settime(CLOCK_REALTIME, &ts) != 0)
		return NTP_ERR_INVALID_TIME;
	return NTP_OK;
}

void ntp_write_json_time(struct json_writer *w)
{
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);
	jw_obj_open(w);
	jw_key(w, "unixtime");
	jw_int(w, (long long)now.tv_sec);
	jw_obj_close(w);
}
