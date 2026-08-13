#include "resolv.h"
#include "persist.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char g_nameservers[RESOLV_MAX_NAMESERVERS][RESOLV_IP_STRLEN];
static int g_count;
static char g_state_path[512];

/*
 * ADR-0141 Phase 2: repoints where the next resolv_set() write lands,
 * without reloading g_nameservers[] -- see network_repoint()'s own doc
 * comment for the shared reasoning. Deliberately does NOT touch the
 * real /etc/resolv.conf bind mount -- this module has no mount(2)
 * knowledge of its own (main.c owns that, same split boot_init()'s own
 * bind-mount setup already has); the caller (main.c's own migration
 * finalize step) is responsible for unmounting and re-establishing
 * that bind mount against new_state_path itself, the same lesson
 * Part 103's own boot-order race already taught: a bind mount is tied
 * to the inode it captured at mount time, not the path.
 */
void resolv_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

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

	/*
	 * NOT persist_atomic_write(): this file is bind-mounted onto
	 * /etc/resolv.conf at boot (boot_init(), main.c) so kanxeod's own
	 * curl/openssl subprocesses -- and every future host-level tool --
	 * resolve through it live. A bind mount binds to the INODE that
	 * was at this path at mount time, not the path itself -- persist_
	 * atomic_write()'s rename(tmp, path) swaps in a NEW inode, which
	 * silently detaches /etc/resolv.conf's own bind mount from any
	 * future write (confirmed live: a PUT after boot updated this
	 * module's own state and GET reported it correctly, but the box's
	 * real outbound curl fetches kept failing to resolve the exact
	 * name just configured, until the next reboot re-did the bind
	 * mount against the by-then-current file). An in-place O_TRUNC
	 * write, same inode throughout, is what this header's own doc
	 * comment already promises ("takes effect, immediately, no reboot
	 * needed") -- trading persist_atomic_write()'s crash-safety
	 * (a crash mid-write could in principle leave a truncated file)
	 * for that liveness, a reasonable trade for a small, rarely-
	 * written file where the alternative is silently not working at
	 * all until an operator happens to reboot.
	 */
	{
		int fd = open(g_state_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (fd < 0)
			return RESOLV_ERR_PERSIST_FAILED;
		if (write(fd, buf, off) != (ssize_t)off || fsync(fd) != 0) {
			close(fd);
			return RESOLV_ERR_PERSIST_FAILED;
		}
		close(fd);
	}

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
