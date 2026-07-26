/*
 * Exec target for test_overlay.c. Runs as PID 1 inside the container,
 * post pivot_root into the overlay-merged root. Reports (via
 * /status.txt, written into upperdir by copy-up) whether it saw
 * lowerdir content correctly and whether a prior container's write
 * already existed at this path -- the two facts test_overlay.c needs
 * to prove shared-lowerdir visibility and per-container upperdir
 * isolation.
 */
#include "overlay_test_common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define STATUS_PATH "/status.txt"

int main(void)
{
	int prior_existed;
	int lower_ok = 0;
	char buf[64];
	FILE *f;
	size_t n;

	prior_existed = (access(STATUS_PATH, F_OK) == 0);

	f = fopen("/lower_marker.txt", "r");
	if (f != NULL) {
		n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = '\0';
		lower_ok = (strcmp(buf, OVERLAY_LOWER_MARKER_CONTENT) == 0);
		fclose(f);
	}

	f = fopen(STATUS_PATH, "w");
	if (f == NULL) {
		perror("fopen " STATUS_PATH);
		return 126;
	}
	fprintf(f, "PID=%d\n", (int)getpid());
	fprintf(f, "PRIOR_EXISTED=%s\n", prior_existed ? "yes" : "no");
	fprintf(f, "LOWER_OK=%s\n", lower_ok ? "yes" : "no");
	fclose(f);

	return 42;
}
