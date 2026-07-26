/*
 * Exec target for the router-forwarding scenario in
 * test_container_net.c/test_daemon_net.c: connects OUT to argv[1] (an
 * IPv4 dotted-quad) on the fixed test port, sends one byte, expects it
 * echoed back. Exits 0 on a successful round trip, 1 otherwise.
 *
 * The point of running this *inside* a container rather than just
 * connecting from the host (like every other connectivity check in
 * this project does): the host isn't a router in these tests, so a
 * host-originated connection would never prove anything about packets
 * actually being forwarded through a third container acting as a
 * router. This binary makes the connection using the calling
 * container's *own* routing table -- including whatever static routes
 * it was given -- which is the thing actually under test.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NET_CONNECT_PORT 17700

int main(int argc, char **argv)
{
	int attempt;
	struct sockaddr_in addr;
	char sbyte = 5, rbyte = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: net_connect DEST_IP\n");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(NET_CONNECT_PORT);
	if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
		fprintf(stderr, "invalid address: %s\n", argv[1]);
		return 1;
	}

	for (attempt = 0; attempt < 50; attempt++) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);

		if (fd < 0) {
			perror("socket");
			return 1;
		}
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			int ok = write(fd, &sbyte, 1) == 1 && read(fd, &rbyte, 1) == 1 && rbyte == sbyte;

			close(fd);
			return ok ? 0 : 1;
		}
		close(fd);
		usleep(100000);
	}
	fprintf(stderr, "connect to %s: timed out retrying\n", argv[1]);
	return 1;
}
