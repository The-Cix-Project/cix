/*
 * Exec target for test_container_stats.c. Drives real, measurable CPU,
 * memory, and disk activity inside a running container -- so the test
 * can prove GET .../stats' numbers actually move, not just that the
 * endpoint returns 200 with plausible-looking zeros. Loops forever
 * (the test kills the container when done): each iteration touches a
 * fresh memory buffer (keeps memory.current genuinely nonzero, not
 * just a one-time peak), burns real CPU in a tight loop (so
 * cpu.usage_usec keeps advancing across two samples), and appends to a
 * real on-disk file (so disk.upper_bytes grows across two samples too).
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MEM_BUF_BYTES (4 * 1024 * 1024)
#define DISK_CHUNK_BYTES 8192
#define CPU_BURN_ITERATIONS 150000000UL

int main(void)
{
	static char mem_buf[MEM_BUF_BYTES];
	char disk_chunk[DISK_CHUNK_BYTES];

	memset(disk_chunk, 0x42, sizeof(disk_chunk));

	for (;;) {
		volatile unsigned long x = 0;
		unsigned long j;
		FILE *f;

		/* Disk write first, before the (comparatively slow) CPU burn
		 * below -- so a sample taken shortly after container start
		 * already sees real, nonzero disk usage, not just after the
		 * first full burn cycle completes. */
		f = fopen("/statsdata.bin", "ab");
		if (f != NULL) {
			fwrite(disk_chunk, 1, sizeof(disk_chunk), f);
			fclose(f);
		}

		memset(mem_buf, (int)(x & 0xff), sizeof(mem_buf));

		for (j = 0; j < CPU_BURN_ITERATIONS; j++)
			x += j;
		(void)x;

		usleep(50000);
	}
	return 0;
}
