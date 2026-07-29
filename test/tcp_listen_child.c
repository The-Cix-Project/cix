/*
 * Exec target for test_container_restart.c's readiness-check
 * scenarios (Phase 13 part 2, ADR-0026). Sleeps argv[2] seconds
 * (default 3) before binding+listening on argv[1] -- the deliberate
 * delay is the whole point: it's what proves wait_for_tcp_ready()
 * genuinely waited for the socket to exist, rather than the test
 * merely observing that the container's *process* had started (which
 * net_child.c's own immediate bind already covers for other tests).
 * Runs forever afterward, accepting and immediately dropping each
 * connection -- callers only need connect() to succeed, never a
 * byte exchanged.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 0;
	int delay_seconds = argc > 2 ? atoi(argv[2]) : 3;
	int lfd;
	struct sockaddr_in addr;

	if (port < 1 || port > 65535) {
		fprintf(stderr, "usage: tcp_listen_child PORT [DELAY_SECONDS]\n");
		return 1;
	}

	sleep((unsigned int)delay_seconds);

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0) {
		perror("socket");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("bind");
		return 1;
	}
	if (listen(lfd, 5) != 0) {
		perror("listen");
		return 1;
	}

	for (;;) {
		int cfd = accept(lfd, NULL, NULL);

		if (cfd >= 0)
			close(cfd);
	}
}
