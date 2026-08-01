/*
 * Trivial stand-in for the two real interactive children
 * run_subprocess_dual_console() actually drives in production (fdisk,
 * mokutil) -- test_dual_console.c's own subject under test is the
 * relay logic itself, not any particular real program's own behavior,
 * so this just echoes each line back with a fixed marker, proving
 * input reached it and its own output made the round trip. "QUIT"
 * exits cleanly (exit 0), matching a real interactive session ending.
 */
#include <stdio.h>
#include <string.h>

int main(void)
{
	char line[256];

	while (fgets(line, sizeof(line), stdin) != NULL) {
		size_t len = strlen(line);

		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';
		if (strcmp(line, "QUIT") == 0)
			break;
		printf("ECHO:%s\n", line);
		fflush(stdout);
	}
	return 0;
}
