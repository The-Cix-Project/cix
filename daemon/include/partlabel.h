#ifndef PARTLABEL_H
#define PARTLABEL_H

#include <stddef.h>

/*
 * Finding the platform's own partitions by the labels the installer
 * wrote, instead of by a device path compiled into the binary.
 *
 * cix-install.c writes a GPT whose five partitions are NAMED --
 * cix-esp, cix-root-a, cix-root-b, cix-config, cix-containers -- and
 * every consumer of that layout then ignored the names and hardcoded
 * "/dev/vda1".."/dev/vda5". That works exactly as long as the disk
 * enumerates the way the developer's VM did.
 *
 * It does not always. Move the same disk from virtio to SATA and it
 * becomes sda; the loader still says root=/dev/vda2, the kernel finds
 * sda1..sda5, and the machine panics in a reboot loop with a correct,
 * fully-installed system underneath it. Measured on 192.168.15.95 when
 * the owner deliberately changed the disk configuration to see whether
 * the platform would cope. It did not.
 *
 * The disk has always carried the answer. This reads it.
 *
 * Deliberately dependency-free -- libc and nothing else -- because the
 * callers are the daemon, cix-install and cix-recover, and the last two
 * are small static tools that must not grow a link closure to ask a
 * question this simple. It is also why the GPT is parsed here directly
 * rather than shelling out to a partition tool: this runs as pid 1
 * before any of them are reachable.
 */

/*
 * The GPT partition name for partition <partno> (1-based) of the whole
 * disk at <parent_dev_path>. out is set to "" when there is no GPT, no
 * such entry, or the device cannot be read -- never left uninitialised.
 *
 * Only the ASCII range is decoded, which every name this platform
 * produces is; a non-ASCII code unit renders as '?' rather than
 * silently truncating the name to nothing.
 */
void partlabel_read(const char *parent_dev_path, unsigned int partno, char *out, size_t out_size);

/*
 * The whole-disk name a partition's kernel name belongs to:
 * "sda4" -> "sda", "nvme0n1p2" -> "nvme0n1", "vda" -> "vda".
 */
void partlabel_parent_name(const char *part_name, char *out, size_t out_size);

/*
 * Resolve a GPT partition label to the device path holding it, by
 * walking /sys/class/block and reading each partition's own GPT entry.
 *
 * Returns 0 with "/dev/<name>" in out, or -1 when no partition on any
 * disk carries that label. A -1 is a real answer -- the caller reports
 * it rather than falling back to a guess, because a guess is what this
 * module exists to remove.
 *
 * Requires /sys mounted; boot_init() mounts it well before the first
 * call. Reads only, and opens each disk O_RDONLY.
 */
int partlabel_find(const char *label, char *out, size_t out_size);

/*
 * The unique partition GUID (PARTUUID) for partition <partno> of the
 * disk at <parent_dev_path>, formatted the way the kernel prints and
 * parses it: 8-4-4-4-12 lowercase hex.
 *
 * This exists because a GPT LABEL cannot answer the one question that
 * matters most. The kernel's own root= parser understands /dev paths
 * and PARTUUID= and does NOT understand PARTLABEL=, so the loader
 * entry -- the single line that decides whether the machine boots at
 * all -- has to name the partition by UUID. cix-install wrote
 * root=/dev/vda2 from a compiled-in prefix, and a disk that came back
 * as sda turned a healthy install into a panic loop (#305).
 *
 * The first three fields are little-endian in the binary and big-endian
 * in the text, per the GPT/EFI spec; the last two are byte order as
 * stored. Getting that backwards produces a plausible-looking UUID that
 * matches nothing, so it is asserted in test_partlabel.c against a
 * known entry rather than eyeballed.
 *
 * out is set to "" on any failure -- never left holding a partial UUID
 * that a caller might write into a boot entry.
 */
void partlabel_read_uuid(const char *parent_dev_path, unsigned int partno, char *out,
                         size_t out_size);

/*
 * The PARTUUID of a partition named by its own device path
 * ("/dev/sda2"), which is the form callers actually hold: the
 * installer has already resolved cix-root-a to a device, and the
 * daemon knows which slot it is writing an entry for.
 *
 * Splits the parent disk and partition number out of the name and
 * reads that entry's GUID. Returns 0 on success, -1 on any failure
 * with out emptied -- a half-answer here would be written into a boot
 * entry, so there is no partial success.
 */
int partlabel_uuid_for_device(const char *part_dev_path, char *out, size_t out_size);

#endif
