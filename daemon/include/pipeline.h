#ifndef PIPELINE_H
#define PIPELINE_H

#include <stddef.h>

/*
 * ADR-0256: the eleven steps between "upstream released something" and
 * "this host is running it", and the one vocabulary the API, the CLI
 * and the web dashboard all speak.
 *
 * This exists because the platform performed all eleven and had a name
 * for none of them. It had four unrelated vocabularies instead -- the
 * source catalogue's four states, enum pkg_failure_kind's five, a
 * build-log line, and a boot-entry filename -- so the only question an
 * operator actually asks ("where is this package and what is stopping
 * it?") could not be asked at all without joining four things by hand.
 *
 * TWO AXES, NOT ONE LONGER LIST.
 *
 * A stage is WHERE; a status is WHAT HAPPENED THERE. The proof that
 * these are separate is `cancelled`, which was already in the old
 * failure enum and was already not a failure kind: a killed build is
 * the build stage with a different outcome, not a different place. Fold
 * them together and the enum becomes the cross product -- build,
 * build-cancelled, build-blocked -- which is a table pretending to be
 * a list.
 */

enum pipeline_stage {
	/* What has upstream published? (srcupstream/kernelpolicy caches) */
	PIPELINE_DISCOVER = 0,
	/* Which of those does policy want? (srcpolicy + srcdepth) */
	PIPELINE_RESOLVE,
	/* Are these really upstream's bytes? (ADR-0254, pgpverify) */
	PIPELINE_AUTHENTICATE,
	/* Is there a recipe for it? */
	PIPELINE_AUTHOR,
	/* Can we download it? */
	PIPELINE_FETCH,
	/*
	 * Is the archive usable? Split out of `build` by this ADR, and the
	 * one behaviour change it makes: a source archive that downloaded
	 * intact and is not a valid archive used to report as a BUILD
	 * failure, sending an operator to read a compile log for something
	 * that happened before any compiler ran.
	 */
	PIPELINE_UNPACK,
	/* Does it compile? */
	PIPELINE_BUILD,
	/* Does the build output merge into the image? */
	PIPELINE_INSTALL,
	/* Does the artifact reach the cache? */
	PIPELINE_PUBLISH,
	/* Do the images that consume it rebuild? */
	PIPELINE_ROLL,
	/* Does the new root boot AND serve on its configured address? */
	PIPELINE_DEPLOY,

	PIPELINE_STAGE_COUNT
};

enum pipeline_status {
	PIPELINE_OK = 0,
	/*
	 * Cannot proceed: waiting on something outside this stage -- an
	 * earlier stage that has not finished, or a person. A package whose
	 * policy resolved to a release nobody has written a recipe for is
	 * blocked at `author`, and that is a true and actionable statement
	 * rather than a failure of anything.
	 */
	PIPELINE_BLOCKED,
	PIPELINE_FAILED,
	/* Stopped by an operator, not by its own merits (issue #213). */
	PIPELINE_CANCELLED,
	/*
	 * This platform performs this stage by hand today. An honest answer
	 * that keeps a manual step visible as a POSITION IN A SEQUENCE
	 * rather than as an absence -- a green tick here would mean "we did
	 * not check", which is the one wrong answer that looks reassuring.
	 */
	PIPELINE_NOT_IMPLEMENTED
};

/*
 * The wire names. These are API enum values, CLI column contents and
 * web labels all at once, so they are fixed by ADR-0256 and changing
 * one is a contract change rather than a rename.
 */
const char *pipeline_stage_name(enum pipeline_stage s);
const char *pipeline_status_name(enum pipeline_status s);

/*
 * The verb a failure at this stage reads as: "could not unpack ...".
 * Kept beside the stage rather than written out at each call site, so
 * eleven stages cannot grow twelve ways of describing themselves.
 */
const char *pipeline_stage_verb(enum pipeline_stage s);

/* -1 when the name is not one of the eleven. */
int pipeline_stage_from_name(const char *name, enum pipeline_stage *out);

#endif /* PIPELINE_H */
