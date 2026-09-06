#include "partlabel.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void partlabel_parent_name(const char *part_name, char *out, size_t out_size)
{
	size_t len = strlen(part_name);
	size_t end = len;

	while (end > 0 && isdigit((unsigned char)part_name[end - 1]))
		end--;
	if (end > 0 && end < len && part_name[end - 1] == 'p' && end >= 2 &&
	    isdigit((unsigned char)part_name[end - 2]))
		end--;
	if (end == 0 || end == len)
		end = len; /* no trailing digits -- already a whole disk name */
	snprintf(out, out_size, "%.*s", (int)end, part_name);
}

/*
 * The GPT is small and fixed: the header sits at LBA 1 with "EFI PART"
 * at its start and carries the entry array's LBA, entry count and entry
 * size; each entry holds a 72-byte UTF-16LE name at offset 56.
 */
void partlabel_read(const char *parent_dev_path, unsigned int partno, char *out, size_t out_size)
{
	unsigned char header[512];
	unsigned char entry[512];
	unsigned long long entry_lba;
	unsigned int entry_count, entry_size;
	unsigned long long offset;
	size_t i, n = 0;
	int fd;

	if (out_size == 0)
		return;
	out[0] = '\0';
	if (parent_dev_path == NULL || partno == 0)
		return;

	fd = open(parent_dev_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (pread(fd, header, sizeof(header), 512) != (ssize_t)sizeof(header) ||
	    memcmp(header, "EFI PART", 8) != 0) {
		close(fd); /* no GPT -- an MBR disk or no table at all */
		return;
	}
	entry_lba = (unsigned long long)header[72] | ((unsigned long long)header[73] << 8) |
	            ((unsigned long long)header[74] << 16) | ((unsigned long long)header[75] << 24) |
	            ((unsigned long long)header[76] << 32) | ((unsigned long long)header[77] << 40) |
	            ((unsigned long long)header[78] << 48) | ((unsigned long long)header[79] << 56);
	entry_count = (unsigned int)header[80] | ((unsigned int)header[81] << 8) |
	              ((unsigned int)header[82] << 16) | ((unsigned int)header[83] << 24);
	entry_size = (unsigned int)header[84] | ((unsigned int)header[85] << 8) |
	             ((unsigned int)header[86] << 16) | ((unsigned int)header[87] << 24);
	if (partno > entry_count || entry_size < 128 || entry_size > sizeof(entry)) {
		close(fd);
		return;
	}
	offset = entry_lba * 512ULL + (unsigned long long)(partno - 1) * entry_size;
	if (pread(fd, entry, entry_size, (off_t)offset) != (ssize_t)entry_size) {
		close(fd);
		return;
	}
	close(fd);

	/* Name: 36 UTF-16LE code units at offset 56, NUL-terminated. */
	for (i = 0; i < 36 && n + 1 < out_size; i++) {
		unsigned int lo = entry[56 + i * 2];
		unsigned int hi = entry[57 + i * 2];

		if (lo == 0 && hi == 0)
			break;
		out[n++] = (hi == 0 && lo >= 0x20 && lo < 0x7f) ? (char)lo : '?';
	}
	out[n] = '\0';
}

int partlabel_find(const char *label, char *out, size_t out_size)
{
	DIR *d;
	struct dirent *ent;
	int found = 0;

	if (label == NULL || label[0] == '\0' || out == NULL || out_size == 0)
		return -1;
	out[0] = '\0';

	d = opendir("/sys/class/block");
	if (d == NULL)
		return -1;

	while (!found && (ent = readdir(d)) != NULL) {
		char partfile[PATH_MAX];
		char parent_name[64];
		char parent_dev[PATH_MAX];
		char name[64];
		const char *suffix;
		unsigned int partno;
		FILE *f;
		long v = 0;

		if (ent->d_name[0] == '.')
			continue;

		/*
		 * Only partitions carry a "partition" file; a whole disk has
		 * none. Reading the number from it rather than parsing it out
		 * of the name is the kernel's own answer to the question, and
		 * it is right for nvme and loop naming without special cases.
		 */
		if (snprintf(partfile, sizeof(partfile), "/sys/class/block/%s/partition", ent->d_name) >=
		    (int)sizeof(partfile))
			continue;
		f = fopen(partfile, "r");
		if (f == NULL)
			continue;
		if (fscanf(f, "%ld", &v) != 1 || v <= 0) {
			fclose(f);
			continue;
		}
		fclose(f);
		partno = (unsigned int)v;

		partlabel_parent_name(ent->d_name, parent_name, sizeof(parent_name));
		if (parent_name[0] == '\0' || strcmp(parent_name, ent->d_name) == 0)
			continue; /* no parent split -- not a partition after all */

		/*
		 * The parent's own name must still prefix this one. Without
		 * the check a device whose name merely ends in digits could
		 * be paired with an unrelated disk and read the wrong GPT --
		 * the same class of mistake disk.c's own enumerate() made
		 * when it reported vda1's label against vdb1.
		 */
		suffix = ent->d_name + strlen(parent_name);
		if (suffix[0] == '\0')
			continue;

		if (snprintf(parent_dev, sizeof(parent_dev), "/dev/%s", parent_name) >=
		    (int)sizeof(parent_dev))
			continue;

		partlabel_read(parent_dev, partno, name, sizeof(name));
		if (name[0] != '\0' && strcmp(name, label) == 0) {
			if (snprintf(out, out_size, "/dev/%s", ent->d_name) < (int)out_size)
				found = 1;
			else
				out[0] = '\0';
		}
	}

	closedir(d);
	return found ? 0 : -1;
}

void partlabel_read_uuid(const char *parent_dev_path, unsigned int partno, char *out,
                         size_t out_size)
{
	unsigned char header[512];
	unsigned char entry[512];
	unsigned long long entry_lba;
	unsigned int entry_count, entry_size;
	unsigned long long offset;
	const unsigned char *g;
	int fd;

	if (out_size == 0)
		return;
	out[0] = '\0';
	if (parent_dev_path == NULL || partno == 0 || out_size < 37)
		return;

	fd = open(parent_dev_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (pread(fd, header, sizeof(header), 512) != (ssize_t)sizeof(header) ||
	    memcmp(header, "EFI PART", 8) != 0) {
		close(fd);
		return;
	}
	entry_lba = (unsigned long long)header[72] | ((unsigned long long)header[73] << 8) |
	            ((unsigned long long)header[74] << 16) | ((unsigned long long)header[75] << 24) |
	            ((unsigned long long)header[76] << 32) | ((unsigned long long)header[77] << 40) |
	            ((unsigned long long)header[78] << 48) | ((unsigned long long)header[79] << 56);
	entry_count = (unsigned int)header[80] | ((unsigned int)header[81] << 8) |
	              ((unsigned int)header[82] << 16) | ((unsigned int)header[83] << 24);
	entry_size = (unsigned int)header[84] | ((unsigned int)header[85] << 8) |
	             ((unsigned int)header[86] << 16) | ((unsigned int)header[87] << 24);
	if (partno > entry_count || entry_size < 128 || entry_size > sizeof(entry)) {
		close(fd);
		return;
	}
	offset = entry_lba * 512ULL + (unsigned long long)(partno - 1) * entry_size;
	if (pread(fd, entry, entry_size, (off_t)offset) != (ssize_t)entry_size) {
		close(fd);
		return;
	}
	close(fd);

	/* Unique partition GUID: 16 bytes at offset 16 of the entry. */
	g = entry + 16;
	snprintf(out, out_size,
	         "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2],
	         g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14],
	         g[15]);
}

int partlabel_uuid_for_device(const char *part_dev_path, char *out, size_t out_size)
{
	const char *base;
	char parent_name[64];
	char parent_dev[PATH_MAX];
	const char *suffix;
	unsigned long partno;
	char *endp;

	if (out == NULL || out_size == 0)
		return -1;
	out[0] = '\0';
	if (part_dev_path == NULL || part_dev_path[0] == '\0')
		return -1;

	base = strrchr(part_dev_path, '/');
	base = (base != NULL) ? base + 1 : part_dev_path;
	if (base[0] == '\0')
		return -1;

	partlabel_parent_name(base, parent_name, sizeof(parent_name));
	if (parent_name[0] == '\0' || strcmp(parent_name, base) == 0)
		return -1; /* a whole disk has no partition entry of its own */

	suffix = base + strlen(parent_name);
	if (suffix[0] == 'p')
		suffix++; /* nvme-style separator */
	if (suffix[0] < '0' || suffix[0] > '9')
		return -1;
	partno = strtoul(suffix, &endp, 10);
	if (endp == suffix || *endp != '\0' || partno == 0 || partno > 128)
		return -1;

	if (snprintf(parent_dev, sizeof(parent_dev), "/dev/%s", parent_name) >=
	    (int)sizeof(parent_dev))
		return -1;

	partlabel_read_uuid(parent_dev, (unsigned int)partno, out, out_size);
	return out[0] != '\0' ? 0 : -1;
}
