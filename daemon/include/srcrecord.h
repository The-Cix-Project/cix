#ifndef SRCRECORD_H
#define SRCRECORD_H

#include "srcupstream.h"

/*
 * What the stages after discovery found for one package, whatever kind
 * discovers it (ADR-0323: every stage reports, and an authenticated
 * release waits here for the author stage). One small document per
 * package, under pkg/discovery/.
 *
 * This used to live in srcgitea's per-package listing document, which
 * was right while gitea-tags was the only kind that authenticated. The
 * kernel's signed checksum list (rung 2) made it a second writer, and a
 * record of the pipeline belongs to the package, not to the kind that
 * found the release -- so it is its own module, read by the source
 * catalogue for every kind alike.
 */

/*
 * What a later stage found for one version -- authenticate or author,
 * with ADR-0256's status words and a reason. The source catalogue
 * applies it only while `version` is still the version that resolves.
 */
struct srcrecord_note {
	char version[SRCUPSTREAM_VERSION_MAX];
	char stage[16];
	char status[16];
	char reason[256];
};

/*
 * An authenticated release waiting for the author stage: the version,
 * the main source url as the recipe spells it (placeholders kept), the
 * sha256 of those bytes, and how that sha256 was established.
 */
struct srcrecord_candidate {
	char version[SRCUPSTREAM_VERSION_MAX];
	char url[512];
	char sha256[65];
	char verification[256];
};

int srcrecord_init(const char *dir);

/* NULL clears it. 0, or -1. */
int srcrecord_store_note(const char *package, const struct srcrecord_note *note);
/* 0 with the note, or -1 when there is none. */
int srcrecord_note(const char *package, struct srcrecord_note *out);

/* NULL clears it. 0, or -1. */
int srcrecord_store_candidate(const char *package, const struct srcrecord_candidate *c);
/* 0 with a complete candidate, or -1 when there is none. */
int srcrecord_candidate(const char *package, struct srcrecord_candidate *out);

#endif /* SRCRECORD_H */
