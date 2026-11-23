#ifndef CIX_INSTALLQ_H
#define CIX_INSTALLQ_H

#include "json.h"
#include "pkg.h"

/*
 * installq -- explicit installs waiting for work in flight to end
 * (ADR-0393, #711).
 *
 * An install blocked only by a running job for the same package and
 * image, another job installing a dependency it shares, or every build
 * slot taken is kept here, exactly as requested, in arrival order, and
 * started by the daemon when a package job ends. Persisted in
 * INSTALL_QUEUE_PATH, so it survives a restart and an in-place switch.
 * The daemon owns announcing changes (ADR-0339); this module only keeps
 * the list.
 */

#define INSTALLQ_MAX 64

struct installq_entry {
	char name[PKG_NAME_MAX];
	char image[PKG_IMAGE_NAME_MAX];
	char version[PKG_VERSION_MAX]; /* "" for the newest */
	int upgrade;
	int keep_on_failure;
	long long queued_at;
	char waiting_for[192];
};

/* Appends e. Returns its position (1 = next), -1 when the queue is full
 * or could not be saved, or -2 when the same name and image is already
 * queued, with *existing set to that entry's position. */
int installq_add(const struct installq_entry *e, int *existing);

int installq_count(void);

/* The i-th entry, oldest first; NULL past the end. */
const struct installq_entry *installq_at(int i);

/* Removes the entry for name and image. 0, or -1 when none is queued. */
int installq_remove(const char *name, const char *image);

/* {"queued":[{...,"position":N}],"depth":N,"capacity":INSTALLQ_MAX} */
void installq_write_json(struct json_writer *w);

#endif
