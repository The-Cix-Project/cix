#include "treesize.h"

#include <ftw.h>
#include <sys/stat.h>

/* nftw() takes no context argument, so the running total is file-scope.
 * Every caller is a helper child or the single-threaded daemon. */
static long long g_treesize_total;

static int treesize_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	(void)path;
	(void)ftwbuf;
	if (typeflag == FTW_F || typeflag == FTW_SL)
		g_treesize_total += (long long)sb->st_size;
	return 0;
}

int treesize_bytes(const char *root, long long *out_bytes)
{
	g_treesize_total = 0;
	if (nftw(root, treesize_cb, 16, FTW_PHYS) != 0)
		return -1;
	*out_bytes = g_treesize_total;
	return 0;
}
