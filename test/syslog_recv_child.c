/*
 * Exec target for test_syslogfwd.c (logging epic Part 2, ADR-0127):
 * binds UDP 0.0.0.0:514 inside its own container's netns, waits for
 * exactly one datagram (or a 5s timeout), and prints its raw bytes to
 * stdout with a fixed marker prefix, then exits. Proves syslogfwd_send()
 * actually reaches a real container over the real network at the real
 * RFC 3164 port, wire format and all -- not just that the REST
 * registration bookkeeping is self-consistent. The received bytes land
 * back in the consolidated log store automatically, via the exact same
 * transparent container-stdout capture this whole epic's Part 1 added
 * (logstore.c/ADR-0126) -- this fixture needs no bind-mounted output
 * file or other special plumbing, it just prints what it caught.
 */
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

int main(void)
{
	int sock;
	struct sockaddr_in addr;
	struct timeval tv;
	char buf[1024];
	ssize_t n;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return 1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(514);
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
		return 1;

	tv.tv_sec = 5;
	tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
	if (n <= 0)
		return 0; /* timeout -- nothing received, not a fixture error */
	buf[n] = '\0';

	fprintf(stdout, "syslog-recv-child-got: %s\n", buf);
	fflush(stdout);
	return 0;
}
