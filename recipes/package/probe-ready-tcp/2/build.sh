#
# Can cix-init's TCP readiness probe ever report ready? (#413)
#
# #413 was filed with the cause stated as OpenSSH's PerSourcePenalties
# defeating jump's `ready: {tcp_port: 22}`. Reading the evidence again,
# that is backwards: every penalty line in it carries a client SOURCE
# PORT, so those connections were ACCEPTED -- and if the probe's
# connect() had returned 0 the service would have been reported ready
# on the first turn. The penalty is what a probe retrying forever looks
# like from sshd's side, not what stops it.
#
# The suspect is probe_connect() in init/src/cix_init.c. It opens
# SOCK_STREAM|SOCK_NONBLOCK, calls connect(), and treats ONLY rc == 0
# as ready, closing the fd immediately either way. Its comment claims
# "a local connect completes synchronously on success or refusal;
# EINPROGRESS only means 'not this turn', and the next turn tries again
# from scratch" -- a claim about a mechanism with nothing naming what
# measured it. If a non-blocking TCP connect returns EINPROGRESS even
# for a listening local peer, that function can never return 1 for
# AF_INET on any kernel, and every tcp_port probe in the platform has
# always ended in its timeout.
#
# Two pieces of live evidence already point that way, neither of them
# proof: jump's sshd (the only tcp_port probe in any deployment recipe)
# reports failure {"reason":"probe-timeout"} while running, measured on
# 192.168.15.95 on v2.57.122; and nslcd in the same container, whose
# probe is AF_UNIX through the same probe_connect(), reports no failure
# at all.
#
# It cannot be measured where it matters from the dev sandbox, and the
# gated test does not discriminate: test_container_restart asserts only
# that depR started before depS and that its ready_probe is echoed
# back, all of which a probe that times out at 10s satisfies.
#
# WHAT THIS PRINTS, and what each line decides:
#
#   [1] nonblocking connect to a LISTENING loopback port.
#       rc == 0            -> the comment is right, hypothesis dead
#       rc == -1 EINPROGRESS -> probe_connect() can never say ready
#   [2] poll(POLLOUT, timeout 0) on that same fd, immediately.
#       POLLOUT set, SO_ERROR 0 -> the handshake finished inside the
#           syscall; an inline poll(0) after EINPROGRESS is enough
#       not set                 -> genuinely asynchronous; the fd has
#           to be carried across turns, like probe_pid already is
#   [3] control: nonblocking connect to a port nothing listens on.
#       Distinguishes "refused" from "in progress" -- both are rc == -1.
#   [4] control: BLOCKING connect to the listener. Expect 0.
#   [5] control: nonblocking AF_UNIX connect to a listening socket,
#       same flags. Expect 0 -- this is the nslcd case that works.
#
# Fails on purpose -- the build log is the product.
#
pkg_name="probe-ready-tcp"
pkg_version="2"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.122.tar.gz"
pkg_sha256="bc9fe250e39b6f8b885d88c67eedb1f052750c254744f691dc710699b8440745"
pkg_build_depends="bash coreutils tcc linux-headers"
pkg_build_image="cix-builder"
pkg_changelog="2: same as 1, whose own C file defined _GNU_SOURCE while the compile line already did (tcc: \"_GNU_SOURCE redefined\" under -Werror). 1: measures whether a non-blocking TCP connect to a listening local peer returns 0 or EINPROGRESS, which decides whether cix-init's tcp_port readiness probe can ever report ready (#413), and whether an inline poll(0) or a carried fd is the fix."

pkg_build() {
	cat > /run/readytcp.c <<'EOF'
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int listener_port(int *out_fd)
{
	struct sockaddr_in a;
	socklen_t alen = sizeof(a);
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int one = 1;

	if (fd < 0) {
		printf("listener: socket failed: %s\n", strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		printf("listener: bind failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 8) != 0) {
		printf("listener: listen failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	if (getsockname(fd, (struct sockaddr *)&a, &alen) != 0) {
		printf("listener: getsockname failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	*out_fd = fd;
	return ntohs(a.sin_port);
}

/* Exactly cix_init.c's probe_connect(), minus the freestanding syscall
 * wrappers: same flags, same "only 0 counts" reading. */
static int nb_connect(int port, int *out_fd, int *out_errno)
{
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int rc;

	*out_fd = -1;
	*out_errno = 0;
	if (fd < 0)
		return -2;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short)port);
	rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
	*out_errno = errno;
	*out_fd = fd;
	return rc;
}

int main(void)
{
	int lfd = -1, port, fd, e, rc, i;

	port = listener_port(&lfd);
	if (port < 0)
		return 1;
	printf("listening on 127.0.0.1:%d\n\n", port);

	printf("[1] nonblocking connect to a LISTENING loopback port\n");
	rc = nb_connect(port, &fd, &e);
	printf("    connect rc=%d errno=%d (%s)\n", rc, rc == 0 ? 0 : e,
	        rc == 0 ? "none" : strerror(e));
	printf("    cix-init would report %s\n", rc == 0 ? "READY" : "not ready, and retry");

	if (rc != 0 && fd >= 0) {
		struct pollfd p;
		int soerr = -1;
		socklen_t slen = sizeof(soerr);

		printf("\n[2] poll(POLLOUT, timeout 0) on that same fd, immediately\n");
		p.fd = fd;
		p.events = POLLOUT;
		p.revents = 0;
		rc = poll(&p, 1, 0);
		printf("    poll rc=%d revents=0x%x (POLLOUT=%s POLLERR=%s POLLHUP=%s)\n", rc,
		        (unsigned)p.revents, (p.revents & POLLOUT) ? "yes" : "no",
		        (p.revents & POLLERR) ? "yes" : "no", (p.revents & POLLHUP) ? "yes" : "no");
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
		printf("    SO_ERROR=%d (%s)\n", soerr, soerr == 0 ? "connected" : strerror(soerr));
		printf("    => %s\n",
		        (rc > 0 && (p.revents & POLLOUT) && soerr == 0)
		                ? "handshake finished inside connect(): an inline poll(0) is enough"
		                : "not finished at timeout 0: the fd must be carried across turns");

		/* How many zero-timeout polls does it actually take? A number
		 * here, rather than a yes/no, says whether a bounded inline
		 * retry is honest or a carried fd is required. */
		for (i = 0; i < 1000; i++) {
			p.fd = fd;
			p.events = POLLOUT;
			p.revents = 0;
			if (poll(&p, 1, 0) > 0 && (p.revents & (POLLOUT | POLLERR | POLLHUP)))
				break;
		}
		printf("    polls until POLLOUT/POLLERR/POLLHUP: %d (of 1000)\n", i);
	}
	if (fd >= 0)
		close(fd);

	printf("\n[3] control: nonblocking connect to a port nothing listens on\n");
	rc = nb_connect(port + 1 > 65535 ? port - 1 : port + 1, &fd, &e);
	printf("    connect rc=%d errno=%d (%s)\n", rc, rc == 0 ? 0 : e,
	        rc == 0 ? "none" : strerror(e));
	if (fd >= 0)
		close(fd);

	printf("\n[4] control: BLOCKING connect to the listener\n");
	{
		struct sockaddr_in a;

		fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = htons((unsigned short)port);
		rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
		printf("    connect rc=%d errno=%d (%s)\n", rc, rc == 0 ? 0 : errno,
		        rc == 0 ? "none" : strerror(errno));
		if (fd >= 0)
			close(fd);
	}

	printf("\n[5] control: nonblocking AF_UNIX connect to a listening socket\n");
	{
		struct sockaddr_un u;
		int ufd, cfd;

		unlink("/run/readytcp.sock");
		ufd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		memset(&u, 0, sizeof(u));
		u.sun_family = AF_UNIX;
		snprintf(u.sun_path, sizeof(u.sun_path), "%s", "/run/readytcp.sock");
		if (bind(ufd, (struct sockaddr *)&u, sizeof(u)) != 0 || listen(ufd, 8) != 0) {
			printf("    unix listener failed: %s\n", strerror(errno));
		} else {
			cfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
			rc = connect(cfd, (struct sockaddr *)&u, sizeof(u));
			printf("    connect rc=%d errno=%d (%s)\n", rc, rc == 0 ? 0 : errno,
			        rc == 0 ? "none" : strerror(errno));
			printf("    cix-init would report %s -- this is the nslcd case\n",
			        rc == 0 ? "READY" : "not ready");
			close(cfd);
		}
		close(ufd);
		unlink("/run/readytcp.sock");
	}

	close(lfd);
	return 0;
}
EOF
	echo "=== compiling with tcc, the compiler cix-init itself is built with ==="
	tcc -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 /run/readytcp.c -o /run/readytcp
	echo
	echo "=== uname, so the answer is attributable to a kernel ==="
	uname -srm
	echo
	/run/readytcp
	echo
	echo "PROBE COMPLETE -- failing deliberately so the log is kept"
	return 1
}

pkg_install() {
	:
}
