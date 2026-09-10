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

	/*
	 * #371 adds the four below, so the same vocabulary covers images,
	 * deployments and the host itself rather than packages alone.
	 * ADR-0256 says adding a stage is "a deliberate act with an ADR",
	 * and these are deliberate: each is a position something can
	 * genuinely be stuck at, separately observable, that no existing
	 * stage describes.
	 *
	 * Four rather than the six a first sketch had. `compose` was
	 * dropped because composing an image IS installing its packages --
	 * PIPELINE_INSTALL already says that, and a second name for it
	 * would be the two-vocabularies problem ADR-0256 exists to end.
	 * A package-side `verify` was dropped because it cannot be
	 * derived: a recipe's own self-tests run INSIDE pkg_build(), so a
	 * compile failure and a self-test failure are one event to this
	 * daemon.
	 */

	/*
	 * Can the thing this depends on be had? An image acquiring a
	 * package artifact, a deployment acquiring an image version.
	 *
	 * THIS IS WHERE FORKS LIVE. Acquiring may mean taking a cached
	 * artifact, or it may mean starting the pipeline that produces it
	 * -- and while that runs, this pipeline is BLOCKED here with
	 * blocked_on naming what it waits for. That pairing is the whole
	 * graph: an edge is a dependency, and a blocked acquire is an edge
	 * currently being traversed.
	 */
	PIPELINE_ACQUIRE,
	/* Host only: does mkbootroot produce a control-plane root?
	 * Separately observable -- GET /system/assembly has its own
	 * generation counter precisely because this fails on its own. */
	PIPELINE_ASSEMBLE,
	/* Host only: does that root reach an A/B slot? POST /system/update
	 * answers {"status":"staged","slot":...}, a distinct outcome from
	 * both assembling it and booting it. */
	PIPELINE_STAGE,
	/*
	 * Deployment only: is the container actually READY, not merely
	 * started? A container that execve()'d successfully and then died
	 * on its first file I/O reports running for a moment and ready
	 * never -- the exact shape of the dnsmasq pidfile crash-loop, where
	 * "status: running" was true and useless.
	 */
	PIPELINE_VERIFY,

	PIPELINE_STAGE_COUNT
};

/*
 * WHAT is moving through a pipeline (#371). Four kinds, ONE stage
 * vocabulary above and ONE status axis below -- not three private
 * vocabularies, which is the failure ADR-0256 was written to end and
 * which generalising it carelessly would reintroduce immediately.
 */
enum pipeline_kind {
	PIPELINE_KIND_PACKAGE = 0,
	PIPELINE_KIND_IMAGE,
	PIPELINE_KIND_DEPLOYMENT,
	PIPELINE_KIND_HOST,

	PIPELINE_KIND_COUNT
};

const char *pipeline_kind_name(enum pipeline_kind k);

/*
 * The ordered stages this kind actually passes through, written into
 * `out` (at most PIPELINE_STAGE_COUNT), returning how many.
 *
 * Every kind uses a SUBSET of the one enum, never a private list. The
 * subsets overlap heavily and that is the point: `author` means the
 * same thing for a package recipe and an image recipe, so an operator
 * learns it once and a renderer draws it once.
 */
int pipeline_kind_stages(enum pipeline_kind k, enum pipeline_stage *out);

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
int pipeline_status_from_name(const char *name, enum pipeline_status *out);

#endif /* PIPELINE_H */
