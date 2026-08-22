/*
 * Exec target for test_volume.c (issue #88). argv[1] "write" writes a
 * known marker into the mounted volume, "sleep" stays alive so a test
 * can act on a RUNNING container (issue #92 part 2's live attach needs
 * something to attach TO); anything else reads the marker back.
 * Deliberately tiny and dependency-free -- it runs inside a minimal
 * container image whose only staged runtime is glibc.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MARKER_PATH "/vol/marker.txt"
/* The exec test writes through a volume mounted at a different path
 * (issue #62) -- same marker, reached from wherever it is mounted. */
#define ALT_MARKER_PATH "/execvol/marker.txt"
#define MARKER "PERSISTED\n"

int main(int argc, char **argv)
{
	FILE *f;

	if (argc > 1 && strcmp(argv[1], "sleep") == 0) {
		/* Long enough for a test to attach a volume and inspect the
		 * result, short enough that a leaked container cannot outlive
		 * the suite by much. */
		sleep(120);
		return 0;
	}
	if (argc > 1) {
		f = fopen(MARKER_PATH, "w");
		if (f == NULL)
			f = fopen(ALT_MARKER_PATH, "w");
		if (f == NULL) {
			printf("WRITE-FAILED\n");
			return 1;
		}
		fputs(MARKER, f);
		fclose(f);
		printf("WROTE\n");
		return 0;
	}
	f = fopen(MARKER_PATH, "r");
	if (f == NULL) {
		printf("MISSING\n");
		return 1;
	}
	{
		char buf[64];

		if (fgets(buf, sizeof(buf), f) != NULL)
			printf("READ:%s", buf);
	}
	fclose(f);
	return 0;
}
