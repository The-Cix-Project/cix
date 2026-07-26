#include "test_net_cleanup.h"
#include "rtnetlink.h"

#include <stdio.h>
#include <unistd.h>

void test_cleanup_bridge(const char *name)
{
	int fd = rtnl_open();
	int i;
	int deleted = 0;

	if (fd < 0) {
		perror("rtnl_open (cleanup)");
		return;
	}
	for (i = 0; i < 20 && !deleted; i++) {
		if (rtnl_link_delete(fd, name) == 0) {
			deleted = 1;
			break;
		}
		usleep(100000);
	}
	rtnl_close(fd);
	if (!deleted)
		fprintf(stderr, "warning: could not delete %s during cleanup\n", name);
}
