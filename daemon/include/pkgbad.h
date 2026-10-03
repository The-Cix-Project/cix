#ifndef PKGBAD_H
#define PKGBAD_H

#include <stddef.h>

#include "json.h"

/*
 * ADR-0323 answer 4: a package version that failed at runtime is marked
 * bad, so the image that carries it is not rolled into again.
 *
 * A rolled container that crash-loops, or never becomes ready, goes
 * back to the image version it ran before. Then every package whose
 * version differs between the two image versions is recorded here,
 * because one of them is why, and the platform cannot tell which. Rolling
 * skips an image version that carries any of them. A mark ends when the
 * package has a newer recipe -- upstream published something newer --
 * or when an operator clears it (DELETE /v1/pkg/bad-versions/...).
 *
 * Persisted: a mark must outlive a restart, or the next boot's rolling
 * pass would walk the container straight back into the failure.
 */

#define PKGBAD_MAX 64
#define PKGBAD_NAME_MAX 64
#define PKGBAD_VERSION_MAX 72

struct pkgbad_entry {
	char package[PKGBAD_NAME_MAX];
	char version[PKGBAD_VERSION_MAX]; /* "<version>-<release>" as the image's manifest has it */
	char image[PKGBAD_NAME_MAX];
	char container[PKGBAD_NAME_MAX];  /* the container whose failure marked it */
	char from[PKGBAD_VERSION_MAX];    /* image version it went back to */
	char to[PKGBAD_VERSION_MAX];      /* image version it failed on */
	char reason[256];
	long long at;
};

int pkgbad_init(const char *path);

/* Adds e, replacing a mark for the same package and version. 0, or -1. */
int pkgbad_add(const struct pkgbad_entry *e);

/* 0 removed, 1 when there was no such mark, -1 when it could not be saved. */
int pkgbad_remove(const char *package, const char *version);

/* The mark for package at version, or NULL. Valid until the list next changes. */
const struct pkgbad_entry *pkgbad_find(const char *package, const char *version);

/* 1 when package at version is marked, 0 otherwise. */
int pkgbad_is_bad(const char *package, const char *version);

/*
 * Removes every mark `superseded` says is no longer current -- for the
 * daemon, "this package has a newer recipe than the marked version".
 * Returns how many were removed.
 */
int pkgbad_prune(int (*superseded)(const char *package, const char *version));

int pkgbad_count(void);
const struct pkgbad_entry *pkgbad_at(int i);

/* {"bad_versions":[{package, version, image, container, from, to, reason, at}, ...]} */
void pkgbad_write_json(struct json_writer *w);

#endif /* PKGBAD_H */
