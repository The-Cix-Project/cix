/*
 * Exec target for test_container_net.c/test_daemon_net.c/test_networks.c.
 * Binds 0.0.0.0:<fixed port> -- doesn't need to know its own assigned
 * IP(s), the OS accepts on whatever address any attached interface
 * ends up configured with. Accepts argv[1] connections (default 1,
 * matching every existing single-network caller unchanged; a
 * multi-homed container being tested from more than one of its
 * networks passes a higher count), echoing one byte per connection,
 * then exits 0.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NET_CHILD_PORT 17700

int main(int argc, char **argv)
{
	int lfd;
	struct sockaddr_in addr;
	int want_connections = argc > 1 ? atoi(argv[1]) : 1;
	int i;

	if (want_connections < 1)
		want_connections = 1;

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0) {
		perror("socket");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(NET_CHILD_PORT);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("bind");
		return 1;
	}
	if (listen(lfd, want_connections) != 0) {
		perror("listen");
		return 1;
	}

	for (i = 0; i < want_connections; i++) {
		int cfd;
		char byte;

		cfd = accept(lfd, NULL, NULL);
		if (cfd < 0) {
			perror("accept");
			return 1;
		}
		if (read(cfd, &byte, 1) != 1 || write(cfd, &byte, 1) != 1) {
			perror("echo byte");
			close(cfd);
			return 1;
		}
		close(cfd);
	}
	close(lfd);
	return 0;
}
