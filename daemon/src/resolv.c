#include "resolv.h"
#include "persist.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

static char g_nameservers[RESOLV_MAX_NAMESERVERS][RESOLV_IP_STRLEN];
static int g_count;
static char g_state_path[512];

int resolv_init(const char *state_path)
{
	FILE *f;
	char line[128];

	snprintf(g_state_path, sizeof(g_state_path), "%s", state_path);
	g_count = 0;

	f = fopen(state_path, "r");
	if (f == NULL)
		return 0; /* nothing configured yet -- not an error */

	while (g_count < RESOLV_MAX_NAMESERVERS && fgets(line, sizeof(line), f) != NULL) {
		char ip[RESOLV_IP_STRLEN];
		struct in_addr a;

		if (sscanf(line, "nameserver %15s", ip) != 1)
			continue;
		if (inet_pton(AF_INET, ip, &a) != 1)
			continue; /* a hand-edited or corrupt line -- skip it, don't fail startup over it */
		snprintf(g_nameservers[g_count], sizeof(g_nameservers[g_count]), "%s", ip);
		g_count++;
	}
	fclose(f);
	return 0;
}

enum resolv_error resolv_set(const char *const *nameservers, int count)
{
	char buf[RESOLV_MAX_NAMESERVERS * (RESOLV_IP_STRLEN + 16)];
	size_t off = 0;
	int i;

	if (count > RESOLV_MAX_NAMESERVERS)
		return RESOLV_ERR_TOO_MANY;
	for (i = 0; i < count; i++) {
		struct in_addr a;

		if (nameservers[i] == NULL || inet_pton(AF_INET, nameservers[i], &a) != 1)
			return RESOLV_ERR_INVALID_IP;
	}

	for (i = 0; i < count; i++) {
		int written = snprintf(buf + off, sizeof(buf) - off, "nameserver %s\n", nameservers[i]);

		if (written < 0 || (size_t)written >= sizeof(buf) - off)
			return RESOLV_ERR_INVALID_IP; /* unreachable given the size bound above and the count cap */
		off += (size_t)written;
	}

	if (persist_atomic_write(g_state_path, buf, off) != 0)
		return RESOLV_ERR_PERSIST_FAILED;

	g_count = count;
	for (i = 0; i < count; i++)
		snprintf(g_nameservers[i], sizeof(g_nameservers[i]), "%s", nameservers[i]);
	return RESOLV_OK;
}

void resolv_write_json(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "nameservers");
	jw_arr_open(w);
	for (i = 0; i < g_count; i++)
		jw_str(w, g_nameservers[i]);
	jw_arr_close(w);
	jw_obj_close(w);
}
