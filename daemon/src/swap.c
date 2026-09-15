#include "swap.h"
#include "linux_compat.h"
#include "logstore.h"
#include "persist.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/swap.h>
#include <unistd.h>


static char g_state_path[512];
static char g_file_path[512];
static int g_enabled;
static int64_t g_size_mb;

static int save_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "enabled");
	jw_bool(&w, g_enabled);
	jw_key(&w, "size_mb");
	jw_int(&w, g_size_mb);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jenabled, *jsize;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted state yet */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed persisted swap state\n", g_state_path);
		return -1;
	}

	jenabled = json_object_get(root, "enabled");
	if (jenabled != NULL && jenabled->type == JSON_BOOL)
		g_enabled = jenabled->u.boolean;

	jsize = json_object_get(root, "size_mb");
	if (jsize != NULL)
		g_size_mb = (int64_t)json_as_number(jsize);

	json_free(root);
	return 0;
}

/*
 * Writes a real kernel swap-file header (version 2, the only format
 * every Linux kernel since 2.6 understands) directly into the first
 * page of fd -- see swap.h's own top comment for why this is done via
 * flat offset writes rather than a C struct. Layout (stable ABI,
 * unchanged for decades -- mm/swapfile.c's read_swap_header(), same
 * offsets util-linux's mkswap writes):
 *   offset 1024: uint32_t version (must be 1)
 *   offset 1028: uint32_t last_page (0-based index of the final page)
 *   offset 1032: uint32_t nr_badpages (0 -- we just created this file)
 *   last 10 bytes of the page: the magic string "SWAPSPACE2"
 * Everything else in the page (bootbits, uuid, volume label, padding)
 * is legitimately all-zero for a fresh file with no bad pages.
 */
static int write_swap_header(int fd, int64_t size_bytes, long page_size)
{
	unsigned char *hdr;
	uint32_t version = 1;
	uint32_t last_page = (uint32_t)((size_bytes / page_size) - 1);
	uint32_t nr_badpages = 0;
	ssize_t written;
	int rc;

	hdr = calloc(1, (size_t)page_size);
	if (hdr == NULL)
		return -1;

	memcpy(hdr + 1024, &version, sizeof(version));
	memcpy(hdr + 1028, &last_page, sizeof(last_page));
	memcpy(hdr + 1032, &nr_badpages, sizeof(nr_badpages));
	memcpy(hdr + page_size - 10, "SWAPSPACE2", 10);

	written = pwrite(fd, hdr, (size_t)page_size, 0);
	rc = (written == page_size) ? 0 : -1;
	free(hdr);
	return rc;
}

int swap_init(const char *state_path, const char *file_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	if (snprintf(g_file_path, sizeof(g_file_path), "%s", file_path) >= (int)sizeof(g_file_path))
		return -1;
	g_enabled = 0;
	g_size_mb = 0;
	if (load_state() != 0)
		return -1;

	if (g_enabled) {
		if (swapon(g_file_path, 0) != 0) {
			fprintf(stderr, "swap_init: swapon(%s) failed: %s (will not retry until "
			                "explicitly re-enabled)\n",
			        g_file_path, strerror(errno));
			g_enabled = 0;
			g_size_mb = 0;
			/* Best-effort reconciliation, matching every other
			 * subsystem's own "non-fatal startup step" posture --
			 * a stale/missing swap file must never block the
			 * daemon from starting. */
		}
	}
	return 0;
}

void swap_repoint(const char *new_file_path)
{
	snprintf(g_file_path, sizeof(g_file_path), "%s", new_file_path);
}

int swap_is_enabled(void)
{
	return g_enabled;
}

/*
 * btrfs refuses a copy-on-write swapfile outright: measured on
 * 192.168.15.95 on 2026-09-15, swapon(2) returned EINVAL and the
 * kernel logged "BTRFS warning (device vdb5): swapfile must not be
 * copy-on-write" -- so host swap could not be enabled on this platform
 * at all, through this module or through POST /v1/system/swap, which
 * failed identically (#472).
 *
 * Read from the kernel source rather than inferred, because the
 * folklore around this is wrong in a way that costs real time.
 * `btrfs_swap_activate()` (fs/btrfs/inode.c) refuses a swapfile that
 * is compressed, that is not NODATACOW, that is not NODATASUM, that
 * has holes, that is inline, or whose extents are shared. It does NOT
 * refuse PREALLOCATED extents -- its loop rejects only inline,
 * compressed and `disk_bytenr == 0` -- which is why fallocate() stays
 * below rather than being replaced by a zero-fill, as the widely
 * repeated advice would have it. A zero-fill would have meant writing
 * every byte of a multi-gigabyte file synchronously inside a
 * single-threaded event loop, for nothing.
 *
 * Two properties of the ioctl decide the shape of this function, both
 * from `btrfs_ioctl_setflags()` (fs/btrfs/ioctl.c):
 *
 *   The file must be EMPTY. FS_NOCOW_FL sets NODATACOW *and*
 *   NODATASUM -- the exact pair swapon needs -- but only when
 *   `i_size == 0`; on a file with extents it is silently ignored.
 *   Hence this sits between open(O_TRUNC) and the allocation below.
 *
 *   A compression flag blocks it. Setting NOCOW is refused with EINVAL
 *   when the file already carries FS_COMPR_FL or FS_NOCOMP_FL (which
 *   an inode can inherit from its directory), and clearing them in the
 *   same call does not help -- the check is against the OLD flags. So
 *   they are cleared first, in their own call.
 *
 * Best-effort against a filesystem with no such flags at all: ext4,
 * where host swap has always worked, has no NOCOW to set and answers
 * the GETFLAGS with a flags word this simply leaves alone. What is NOT
 * best-effort is silence -- if the flag was settable and did not
 * take, that is said here, because the alternative is a swapon failure
 * whose only explanation is in the kernel log (#472 again).
 */
static void set_nocow(int fd)
{
	long flags = 0;

	if (ioctl(fd, CIX_FS_IOC_GETFLAGS, &flags) != 0)
		return; /* no inode flags on this filesystem */
	if ((flags & CIX_FS_NOCOW_FL) != 0)
		return;
	if ((flags & (CIX_FS_COMPR_FL | CIX_FS_NOCOMP_FL)) != 0) {
		long cleared = flags & ~(CIX_FS_COMPR_FL | CIX_FS_NOCOMP_FL);

		if (ioctl(fd, CIX_FS_IOC_SETFLAGS, &cleared) != 0) {
			logstore_write("cixd", "warn",
			               "swap: %s inherits a compression flag that could not be "
			               "cleared, so it cannot be made no-COW: %s",
			               g_file_path, strerror(errno));
			return;
		}
		flags = cleared;
	}
	flags |= CIX_FS_NOCOW_FL;
	if (ioctl(fd, CIX_FS_IOC_SETFLAGS, &flags) != 0) {
		logstore_write("cixd", "warn",
		               "swap: could not make %s no-COW: %s -- a btrfs swap file will be "
		               "refused by the kernel without it",
		               g_file_path, strerror(errno));
		return;
	}
	flags = 0;
	if (ioctl(fd, CIX_FS_IOC_GETFLAGS, &flags) == 0 && (flags & CIX_FS_NOCOW_FL) == 0)
		logstore_write("cixd", "warn",
		               "swap: the no-COW flag did not take on %s -- the file was expected "
		               "to be empty at this point",
		               g_file_path);
}

enum swap_error swap_enable(int64_t size_mb)
{
	int64_t size_bytes;
	long page_size;
	int fd;

	if (g_enabled)
		return SWAP_ERR_ALREADY_ENABLED;
	if (size_mb < SWAP_MIN_MB || size_mb > SWAP_MAX_MB)
		return SWAP_ERR_INVALID_SIZE;

	page_size = sysconf(_SC_PAGESIZE);
	size_bytes = size_mb * 1024 * 1024;
	/* Round down to a whole number of pages -- swapon(2) requires the
	 * file's usable size to be page-aligned; last_page in the header
	 * above must match exactly what's really allocated. */
	size_bytes -= size_bytes % page_size;

	fd = open(g_file_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (fd < 0) {
		logstore_write("cixd", "error", "swap: cannot create %s: %s", g_file_path,
		               strerror(errno));
		return SWAP_ERR_IO;
	}

	set_nocow(fd);

	/* fallocate(), not ftruncate() -- a swap file must have every
	 * block genuinely backed on disk (no holes); ftruncate() alone
	 * would leave it sparse and swapon(2) would reject it. */
	if (fallocate(fd, 0, 0, size_bytes) != 0) {
		logstore_write("cixd", "error", "swap: cannot allocate %lld bytes for %s: %s",
		               (long long)size_bytes, g_file_path, strerror(errno));
		close(fd);
		unlink(g_file_path);
		return SWAP_ERR_IO;
	}
	if (write_swap_header(fd, size_bytes, page_size) != 0) {
		logstore_write("cixd", "error", "swap: cannot write the swap header to %s: %s",
		               g_file_path, strerror(errno));
		close(fd);
		unlink(g_file_path);
		return SWAP_ERR_IO;
	}
	close(fd);

	if (swapon(g_file_path, 0) != 0) {
		/*
		 * The filesystem's own reason goes to the KERNEL log, not to
		 * errno -- btrfs answers a plain EINVAL and writes "swapfile
		 * must not be copy-on-write" (or "must not have holes", or
		 * "must not be compressed") to dmesg. That cost a full
		 * cross-reference against `logs --source=kernel` to diagnose
		 * (#472), so say here that the reason is over there.
		 */
		logstore_write("cixd", "error",
		               "swap: swapon(%s) failed: %s -- if this is EINVAL, the filesystem "
		               "refused the file and said why in the kernel log",
		               g_file_path, strerror(errno));
		unlink(g_file_path);
		return SWAP_ERR_IO;
	}

	g_enabled = 1;
	g_size_mb = size_bytes / (1024 * 1024);
	if (save_state() != 0) {
		swapoff(g_file_path);
		unlink(g_file_path);
		g_enabled = 0;
		g_size_mb = 0;
		return SWAP_ERR_PERSIST_FAILED;
	}
	return SWAP_OK;
}

enum swap_error swap_disable(void)
{
	if (!g_enabled)
		return SWAP_ERR_NOT_ENABLED;

	if (swapoff(g_file_path) != 0)
		return SWAP_ERR_IO;
	unlink(g_file_path); /* best-effort -- swap is already off either way */

	g_enabled = 0;
	g_size_mb = 0;
	if (save_state() != 0)
		return SWAP_ERR_PERSIST_FAILED;
	return SWAP_OK;
}

void swap_write_json(struct json_writer *w, const char *disk_name)
{
	jw_obj_open(w);
	jw_key(w, "enabled");
	jw_bool(w, g_enabled);
	jw_key(w, "size_mb");
	jw_int(w, g_size_mb);
	jw_key(w, "path");
	jw_str(w, g_enabled ? g_file_path : "");
	jw_key(w, "disk");
	if (disk_name != NULL)
		jw_str(w, disk_name);
	else
		jw_null(w);
	jw_obj_close(w);
}
