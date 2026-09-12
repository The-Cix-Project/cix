#define _GNU_SOURCE
#include "netconf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int netconf_parse(const char *path, char *out_ip, size_t ip_size, int *out_prefix,
                   char *out_gateway, size_t gateway_size, char *out_interface,
                   size_t interface_size)
{
	FILE *f;
	char line[256];
	int have_ip = 0, have_prefix = 0, have_interface = 0;

	out_ip[0] = '\0';
	out_gateway[0] = '\0';
	out_interface[0] = '\0';
	*out_prefix = 0;

	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		if (strncmp(line, "ip=", 3) == 0) {
			snprintf(out_ip, ip_size, "%s", line + 3);
			have_ip = 1;
		} else if (strncmp(line, "prefix=", 7) == 0) {
			*out_prefix = atoi(line + 7);
			have_prefix = 1;
		} else if (strncmp(line, "gateway=", 8) == 0) {
			snprintf(out_gateway, gateway_size, "%s", line + 8);
		} else if (strncmp(line, "interface=", 10) == 0) {
			snprintf(out_interface, interface_size, "%s", line + 10);
			have_interface = 1;
		}
	}
	fclose(f);
	/*
	 * The gateway line is optional, matching the fact that a gateway
	 * is: a box reachable only on its own subnet is an ordinary
	 * install. An interface, an address and a prefix are what decide
	 * whether this file describes a usable network.
	 */
	return (have_ip && have_prefix && have_interface) ? 0 : -1;
}

int netconf_write(const char *path, const char *ip, int prefix, const char *gateway,
                   const char *interface)
{
	char tmp[1024];
	char body[512];
	int fd;
	int len;
	ssize_t written;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	len = snprintf(body, sizeof(body), "ip=%s\nprefix=%d\ngateway=%s\ninterface=%s\n", ip, prefix,
	                gateway != NULL ? gateway : "", interface);
	if (len < 0 || len >= (int)sizeof(body)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	written = write(fd, body, (size_t)len);
	if (written != (ssize_t)len) {
		if (written >= 0)
			errno = EIO;
		close(fd);
		unlink(tmp);
		return -1;
	}
	/*
	 * fsync before rename, not after: rename(2) makes the new contents
	 * visible atomically, but only the fsync guarantees those contents
	 * actually reached the disk first. Without it a power loss moments
	 * after this call can leave the rename durable and the data not --
	 * i.e. a net.conf that exists, is the right size, and is zeroes.
	 * This file decides whether the box comes back on the network, so
	 * it is worth the flush.
	 */
	if (fsync(fd) != 0) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	if (close(fd) != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}
