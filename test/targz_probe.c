/* Minimal helper exec'd inside test_targz's shell-less chroot: the
 * whole point is that the archive gets built from in there, so this
 * must be a real, separately-staged binary rather than test code. */
#include "targz.h"

#include <stdio.h>

int main(int argc, char **argv)
{
	int code = -1;
	int rc;

	if (argc != 3) {
		fprintf(stderr, "usage: targz_probe <src-dir> <out.tar.gz>\n");
		return 2;
	}
	rc = targz_create(argv[1], argv[2], &code);
	fprintf(stderr, "targz_probe: rc=%d code=%d\n", rc, code);
	return rc == 0 ? 0 : 1;
}
