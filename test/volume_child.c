/*
 * Exec target for test_volume.c (issue #88). argv[1] "write" writes a
 * known marker into the mounted volume; anything else reads it back.
 * Deliberately tiny and dependency-free -- it runs inside a minimal
 * container image whose only staged runtime is glibc.
 */
#include <stdio.h>

#define MARKER_PATH "/vol/marker.txt"
#define MARKER "PERSISTED\n"

int main(int argc, char **argv)
{
	FILE *f;

	if (argc > 1) {
		f = fopen(MARKER_PATH, "w");
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
