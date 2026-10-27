#ifndef QUOTAMODE_H
#define QUOTAMODE_H

/*
 * ADR-0359: the quota accounting mode a btrfs gets when cixd makes it.
 *
 * One host setting, simple by default. It is applied at mkfs, so quotas
 * are on before the first byte, and a filesystem keeps its mode for its
 * life: simple accounting never counts extents older than its enable
 * (measured on 192.168.15.95, #613), so there is no live switch.
 */

enum quotamode {
	QUOTAMODE_SIMPLE = 0,
	QUOTAMODE_FULL
};

enum quotamode_error {
	QUOTAMODE_OK = 0,
	QUOTAMODE_ERR_PERSIST_FAILED
};

/* Loads the persisted mode. A host that never saved one is on simple. */
void quotamode_init(const char *path);

enum quotamode quotamode_get(void);

/* Persists the mode. It governs filesystems made from now on. */
enum quotamode_error quotamode_set(enum quotamode mode);

/* "simple" or "full". */
const char *quotamode_name(enum quotamode mode);

/* 0 with *out set for "simple" or "full", -1 otherwise. */
int quotamode_parse(const char *s, enum quotamode *out);

/* The mkfs.btrfs feature that turns this mode on: "squota" or "quota". */
const char *quotamode_mkfs_feature(enum quotamode mode);

struct json_writer;

/* GET /v1/storage/quota's body, and the configuration document's
 * storage_quota section: the setting, and each mounted btrfs's mode. */
void quotamode_write_json(struct json_writer *w, const char *containers_dir);

#endif
