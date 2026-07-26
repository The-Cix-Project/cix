#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <linux/netlink.h>

int main(void)
{
	int epfd = epoll_create1(0);
	if (epfd < 0) {
		perror("epoll_create1");
		return 1;
	}
	close(epfd);

	int nlfd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (nlfd < 0) {
		perror("socket(AF_NETLINK)");
		return 1;
	}
	close(nlfd);

	if (prctl(PR_SET_NAME, "tcc-smoke") != 0) {
		perror("prctl");
		return 1;
	}

#ifndef SYS_pivot_root
#error "SYS_pivot_root not defined by this libc's syscall.h"
#endif
#ifndef SYS_clone3
#error "SYS_clone3 not defined by this libc's syscall.h"
#endif

	printf("toolchain smoke test OK: SYS_pivot_root=%d SYS_clone3=%d\n",
	       (int)SYS_pivot_root, (int)SYS_clone3);
	return 0;
}
