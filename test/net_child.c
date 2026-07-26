/*
 * Exec target for test_container_net.c. Binds 0.0.0.0:<fixed port> --
 * it doesn't need to know its own assigned IP, the OS accepts on
 * whatever address eth0 ends up configured with -- accepts one
 * connection, echoes one byte, exits 0.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NET_CHILD_PORT 17700

int main(void)
{
	int lfd, cfd;
	struct sockaddr_in addr;
	char byte;

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
	if (listen(lfd, 1) != 0) {
		perror("listen");
		return 1;
	}

	cfd = accept(lfd, NULL, NULL);
	if (cfd < 0) {
		perror("accept");
		return 1;
	}
	if (read(cfd, &byte, 1) != 1 || write(cfd, &byte, 1) != 1) {
		perror("echo byte");
		return 1;
	}
	close(cfd);
	close(lfd);
	return 0;
}
