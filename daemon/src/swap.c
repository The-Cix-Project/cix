#include "swap.h"
#include "persist.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	if (fd < 0)
		return SWAP_ERR_IO;

	/* fallocate(), not ftruncate() -- a swap file must have every
	 * block genuinely backed on disk (no holes); ftruncate() alone
	 * would leave it sparse and swapon(2) would reject it. */
	if (fallocate(fd, 0, 0, size_bytes) != 0) {
		close(fd);
		unlink(g_file_path);
		return SWAP_ERR_IO;
	}
	if (write_swap_header(fd, size_bytes, page_size) != 0) {
		close(fd);
		unlink(g_file_path);
		return SWAP_ERR_IO;
	}
	close(fd);

	if (swapon(g_file_path, 0) != 0) {
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

void swap_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "enabled");
	jw_bool(w, g_enabled);
	jw_key(w, "size_mb");
	jw_int(w, g_size_mb);
	jw_key(w, "path");
	jw_str(w, g_enabled ? g_file_path : "");
	jw_obj_close(w);
}
