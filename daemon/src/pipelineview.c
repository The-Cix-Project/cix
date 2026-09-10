/*
 * See pipelineview.h for the row grain and why nothing here is stored.
 */
#include "pipelineview.h"

#include "esp.h"
#include "image.h"
#include "pipeline.h"
#include "pkg.h"
#include "containerdef.h"
#include "registry.h"
#include "srcresolve.h"

#include <stdio.h>
#include <string.h>

#define PIPELINEVIEW_MAX_PACKAGES 512
#define PIPELINEVIEW_MAX_RECIPE_VERSIONS 64
#define PIPELINEVIEW_MAX_IMAGES 32
#define PIPELINEVIEW_MAX_ESP_ENTRIES 32
#define PIPELINEVIEW_MAX_IMAGE_NAMES 64
#define PIPELINEVIEW_MAX_MANIFEST 256
#define PIPELINEVIEW_MAX_DEPLOYMENTS 256

/*
 * Which of two positions is the one to report for a package.
 *
 * "Worst" is not a severity ranking -- it is the EARLIEST stage that is
 * not ok, because a pipeline stops at its first problem and everything
 * after it is consequence rather than cause. A package that could not
 * be downloaded and therefore also could not be built should read
 * "could not download"; ranking by severity would report the build.
 */
static int position_is_worse(enum pipeline_stage a_stage, enum pipeline_status a_status,
                              enum pipeline_stage b_stage, enum pipeline_status b_status)
{
	if (a_status == PIPELINE_OK)
		return 0;
	if (b_status == PIPELINE_OK)
		return 1;
	return a_stage < b_stage;
}

static void write_position(struct json_writer *w, enum pipeline_stage stage,
                            enum pipeline_status status, const char *reason)
{
	jw_key(w, "stage");
	jw_str(w, pipeline_stage_name(stage));
	jw_key(w, "status");
	jw_str(w, pipeline_status_name(status));
	jw_key(w, "reason");
	jw_str(w, reason != NULL ? reason : "");
}

/*
 * The deploy stage, once, for the host.
 *
 * An A/B boot entry carries its remaining try count in its own filename
 * (`cix-a+3.conf`), and confirm_boot() renames it to the bare form once
 * the boot is good. So an entry still carrying '+' is a boot that has
 * not been confirmed -- which is exactly `blocked`: the deploy has not
 * finished, and nothing says it failed either.
 */
static void write_deploy(struct json_writer *w)
{
	struct esp_entry entries[PIPELINEVIEW_MAX_ESP_ENTRIES];
	int n, i;

	jw_key(w, "deploy");
	jw_obj_open(w);
	n = esp_entries_list(entries, PIPELINEVIEW_MAX_ESP_ENTRIES);
	for (i = 0; i < n; i++) {
		if (!entries[i].is_running_slot)
			continue;
		jw_key(w, "entry");
		jw_str(w, entries[i].name);
		if (strchr(entries[i].name, '+') != NULL)
			write_position(w, PIPELINE_DEPLOY, PIPELINE_BLOCKED,
			                "this boot has not been confirmed yet -- the entry still "
			                "carries a try count, so a failure here falls back to the "
			                "other slot");
		else
			write_position(w, PIPELINE_DEPLOY, PIPELINE_OK,
			                "booted and confirmed from this slot");
		jw_obj_close(w);
		return;
	}
	/*
	 * No entry claims to be the running slot. Honest rather than
	 * assumed: this is every development sandbox, where cixd was not
	 * started as init and there is no ESP at all.
	 */
	jw_key(w, "entry");
	jw_null(w);
	write_position(w, PIPELINE_DEPLOY, PIPELINE_NOT_IMPLEMENTED,
	                "this daemon did not boot from an A/B slot, so there is no deploy to report");
	jw_obj_close(w);
}

/*
 * The image rows (#371).
 *
 * An image's position is derived, not stored: its manifest names the
 * packages it is composed of, and the worst position among those is
 * where the image itself stands -- an image cannot be further along
 * than the package it is waiting for. An image with a current version
 * and nothing wrong reports `install`, the furthest stage it actually
 * reached; one with no current version yet is blocked at `acquire`,
 * which is also exactly where a fork would attach.
 */
static void write_images(struct json_writer *w, int *stage_counts)
{
	static char names[PIPELINEVIEW_MAX_IMAGE_NAMES][PKG_IMAGE_NAME_MAX];
	static struct image_manifest_entry man[PIPELINEVIEW_MAX_MANIFEST];
	static struct pkg_position positions[PIPELINEVIEW_MAX_IMAGES];
	char version[PKG_VERSION_MAX];
	int n, i, m, j, p, np;

	n = image_list_names(names, PIPELINEVIEW_MAX_IMAGE_NAMES);
	jw_key(w, "images");
	jw_arr_open(w);
	for (i = 0; i < n; i++) {
		enum pipeline_stage stage = PIPELINE_INSTALL;
		enum pipeline_status status = PIPELINE_OK;
		const char *reason = "";
		const char *blocked_on = NULL;
		int have_version = image_current_version(names[i], version, sizeof(version)) == IMAGE_OK;

		if (!have_version) {
			stage = PIPELINE_ACQUIRE;
			status = PIPELINE_BLOCKED;
			reason = "this image has no current version yet";
			version[0] = '\0';
		}

		m = 0;
		if (image_manifest_read(names[i], man, &m, PIPELINEVIEW_MAX_MANIFEST) != IMAGE_OK)
			m = 0;
		for (j = 0; j < m; j++) {
			np = pkg_positions(man[j].package, positions, PIPELINEVIEW_MAX_IMAGES);
			for (p = 0; p < np; p++) {
				if (strcmp(positions[p].image, names[i]) != 0)
					continue;
				if (positions[p].status == PIPELINE_OK)
					continue;
				if (status == PIPELINE_OK ||
				    position_is_worse(positions[p].stage, positions[p].status, stage, status)) {
					stage = positions[p].stage;
					status = positions[p].status;
					reason = positions[p].error;
					blocked_on = man[j].package;
				}
			}
		}

		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, names[i]);
		jw_key(w, "version");
		if (version[0] == '\0')
			jw_null(w);
		else
			jw_str(w, version);
		write_position(w, stage, status, reason);
		/*
		 * The package this image is waiting on, when it is waiting on
		 * one. An edge already exists from every image to every package
		 * in its manifest (see write_edges); this names WHICH of them
		 * is the live one, so a renderer can highlight rather than
		 * invent.
		 */
		jw_key(w, "blocked_on");
		if (blocked_on == NULL) {
			jw_null(w);
		} else {
			jw_obj_open(w);
			jw_key(w, "kind");
			jw_str(w, pipeline_kind_name(PIPELINE_KIND_PACKAGE));
			jw_key(w, "name");
			jw_str(w, blocked_on);
			jw_obj_close(w);
		}
		jw_obj_close(w);
		stage_counts[stage]++;
	}
	jw_arr_close(w);
}

/*
 * The deployment rows (#371) -- what this platform used to call
 * "container recipes".
 *
 * A deployment's position is where its container actually is, which is
 * why `verify` exists as a stage: a container that started and is not
 * ready is neither ok nor failed, and reporting it as running was the
 * dnsmasq crash-loop's whole disguise.
 */
static void write_deployments(struct json_writer *w, int *stage_counts)
{
	static char names[PIPELINEVIEW_MAX_DEPLOYMENTS][PKG_IMAGE_NAME_MAX];
	int n, i;

	n = container_recipe_list_names(names, PIPELINEVIEW_MAX_DEPLOYMENTS);
	jw_key(w, "deployments");
	jw_arr_open(w);
	for (i = 0; i < n; i++) {
		enum pipeline_stage stage = PIPELINE_AUTHOR;
		enum pipeline_status status = PIPELINE_BLOCKED;
		const char *reason = "declared, never applied on this host";
		const struct registry_entry *e = registry_find(names[i]);
		const struct container_def *def = containerdef_find(names[i]);
		char waiting_reason[320];
		const char *blocked_on_image = NULL;

		/*
		 * ADR-0270: applied, and waiting for its image to be realized.
		 *
		 * This is where image failure REACHES the deployment -- there is
		 * no callback and nothing stored, exactly as ADR-0256 requires:
		 * the status is computed here, at read time, from the image the
		 * definition says it is waiting on. A replay that failed for a
		 * reason that was not the image reports that instead, because
		 * "blocked on an image" would be untrue and would send an
		 * operator to look at the wrong thing.
		 */
		if (def != NULL && def->awaiting_image[0] != '\0') {
			stage = PIPELINE_ACQUIRE;
			blocked_on_image = def->awaiting_image;
			if (def->last_replay_error[0] != '\0') {
				status = PIPELINE_FAILED;
				snprintf(waiting_reason, sizeof(waiting_reason),
				          "image %s is ready, but creating this deployment failed: %s",
				          def->awaiting_image, def->last_replay_error);
			} else {
				status = PIPELINE_BLOCKED;
				snprintf(waiting_reason, sizeof(waiting_reason),
				          "waiting for image %s to be built", def->awaiting_image);
			}
			reason = waiting_reason;
		} else if (e != NULL) {
			if (!e->running) {
				stage = PIPELINE_DEPLOY;
				status = PIPELINE_FAILED;
				reason = e->last_exit_reason[0] != '\0' ? e->last_exit_reason
				                                         : "the container is not running";
			} else if (!e->ready) {
				stage = PIPELINE_VERIFY;
				status = PIPELINE_BLOCKED;
				reason = "started, not yet ready";
			} else {
				stage = PIPELINE_VERIFY;
				status = PIPELINE_OK;
				reason = "";
			}
		}

		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, names[i]);
		jw_key(w, "image");
		if (blocked_on_image != NULL)
			jw_str(w, blocked_on_image);
		else if (e != NULL && e->image[0] != '\0')
			jw_str(w, e->image);
		else
			jw_null(w);
		/* ADR-0270: name what is holding this one up, in the shape
		 * ADR-0269 gave every other blocked edge. */
		if (blocked_on_image != NULL) {
			jw_key(w, "blocked_on");
			jw_obj_open(w);
			jw_key(w, "kind");
			jw_str(w, "image");
			jw_key(w, "name");
			jw_str(w, blocked_on_image);
			jw_obj_close(w);
		}
		write_position(w, stage, status, reason);
		jw_obj_close(w);
		stage_counts[stage]++;
	}
	jw_arr_close(w);
}

/*
 * The edges (#371). STRUCTURAL, not failure-driven -- this is the one
 * thing a first design got backwards.
 *
 * An edge exists because a deployment NAMES an image and an image's
 * manifest NAMES packages. It exists when everything is healthy, and
 * that is the point: edges drawn only where something is stuck give a
 * working host a picture of disconnected boxes, which is the opposite
 * of what a graph is for. Status colours an edge; blocking annotates
 * one; neither creates one.
 *
 * `follow_rolling` is its own edge kind rather than a flag on the
 * ordinary one, because it means something different and permanent:
 * this deployment chases its image's current_version forever, not just
 * once. It is emitted only where it is LIVE -- the flag is stored but
 * inert on a restart:"no" def, and drawing an arrow for a rule that
 * never fires would be a lie in the most load-bearing place on the page.
 */
static void write_edges(struct json_writer *w)
{
	static char inames[PIPELINEVIEW_MAX_IMAGE_NAMES][PKG_IMAGE_NAME_MAX];
	static struct image_manifest_entry man[PIPELINEVIEW_MAX_MANIFEST];
	static char dnames[PIPELINEVIEW_MAX_DEPLOYMENTS][PKG_IMAGE_NAME_MAX];
	int n, i, m, j;

	jw_key(w, "edges");
	jw_arr_open(w);

	n = image_list_names(inames, PIPELINEVIEW_MAX_IMAGE_NAMES);
	for (i = 0; i < n; i++) {
		m = 0;
		if (image_manifest_read(inames[i], man, &m, PIPELINEVIEW_MAX_MANIFEST) != IMAGE_OK)
			continue;
		for (j = 0; j < m; j++) {
			jw_obj_open(w);
			jw_key(w, "from_kind");
			jw_str(w, pipeline_kind_name(PIPELINE_KIND_IMAGE));
			jw_key(w, "from");
			jw_str(w, inames[i]);
			jw_key(w, "to_kind");
			jw_str(w, pipeline_kind_name(PIPELINE_KIND_PACKAGE));
			jw_key(w, "to");
			jw_str(w, man[j].package);
			jw_key(w, "relation");
			jw_str(w, "manifest");
			jw_obj_close(w);
		}
	}

	n = container_recipe_list_names(dnames, PIPELINEVIEW_MAX_DEPLOYMENTS);
	for (i = 0; i < n; i++) {
		const struct registry_entry *e = registry_find(dnames[i]);

		if (e == NULL || e->image[0] == '\0')
			continue;
		jw_obj_open(w);
		jw_key(w, "from_kind");
		jw_str(w, pipeline_kind_name(PIPELINE_KIND_DEPLOYMENT));
		jw_key(w, "from");
		jw_str(w, dnames[i]);
		jw_key(w, "to_kind");
		jw_str(w, pipeline_kind_name(PIPELINE_KIND_IMAGE));
		jw_key(w, "to");
		jw_str(w, e->image);
		jw_key(w, "relation");
		{
			/*
			 * follow_rolling lives on the containerdef, not the registry
			 * entry, and is STORED-BUT-INERT on a restart_policy "no"
			 * def -- apply_rolling_container_restarts() skips those
			 * entirely. So both conditions are required before drawing
			 * the arrow: a rule that never fires must not be rendered as
			 * one that does.
			 */
			const struct container_def *d = containerdef_find(dnames[i]);
			int live_rolling = d != NULL && d->follow_rolling &&
			                    strcmp(d->restart_policy, "no") != 0;

			jw_str(w, live_rolling ? "follow-rolling" : "image");
		}
		jw_obj_close(w);
	}
	jw_arr_close(w);
}

void pipelineview_write_json(struct json_writer *w)
{
	static char names[PIPELINEVIEW_MAX_PACKAGES][PKG_IMAGE_NAME_MAX];
	static char versions[PIPELINEVIEW_MAX_RECIPE_VERSIONS][PKG_VERSION_MAX];
	static struct pkg_position positions[PIPELINEVIEW_MAX_IMAGES];
	const char *vp[PIPELINEVIEW_MAX_RECIPE_VERSIONS];
	struct srcresolve_entry cat;
	int stage_counts[PIPELINE_STAGE_COUNT];
	int status_counts[5];
	int n, i, v, nv, np, p;

	memset(stage_counts, 0, sizeof(stage_counts));
	memset(status_counts, 0, sizeof(status_counts));
	n = pkg_recipe_list_names(names, PIPELINEVIEW_MAX_PACKAGES);

	jw_obj_open(w);
	jw_key(w, "packages");
	jw_arr_open(w);
	for (i = 0; i < n; i++) {
		char kind[32];
		enum pipeline_stage stage;
		enum pipeline_status status;
		const char *reason;

		if (pkg_recipe_upstream(names[i], kind, sizeof(kind)) != 0)
			kind[0] = '\0';
		nv = pkg_recipe_list_versions(names[i], versions, PIPELINEVIEW_MAX_RECIPE_VERSIONS);
		for (v = 0; v < nv; v++)
			vp[v] = versions[v];
		srcresolve_one(names[i], kind, vp, (size_t)nv, &cat);
		stage = cat.stage;
		status = cat.status;
		reason = cat.reason;

		np = pkg_positions(names[i], positions, PIPELINEVIEW_MAX_IMAGES);
		for (p = 0; p < np; p++) {
			if (position_is_worse(positions[p].stage, positions[p].status, stage, status)) {
				stage = positions[p].stage;
				status = positions[p].status;
				reason = positions[p].error;
			}
		}
		/*
		 * A package that is fine everywhere reports the furthest stage
		 * it actually reached, not a stage it is stuck at. `install`
		 * when something has installed it; `author` when a recipe
		 * exists and nothing has been built from it yet.
		 */
		if (status == PIPELINE_OK && np == 0)
			stage = PIPELINE_AUTHOR;

		stage_counts[stage]++;
		status_counts[status]++;

		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, names[i]);
		write_position(w, stage, status, reason);
		jw_key(w, "upstream");
		if (cat.kind[0] == '\0')
			jw_null(w);
		else
			jw_str(w, cat.kind);
		jw_key(w, "resolved_version");
		if (cat.resolved_version[0] == '\0')
			jw_null(w);
		else
			jw_str(w, cat.resolved_version);
		jw_key(w, "newest_recipe_version");
		if (cat.newest_recipe_version[0] == '\0')
			jw_null(w);
		else
			jw_str(w, cat.newest_recipe_version);
		jw_key(w, "images");
		jw_arr_open(w);
		for (p = 0; p < np; p++) {
			jw_obj_open(w);
			jw_key(w, "image");
			jw_str(w, positions[p].image);
			jw_key(w, "version");
			jw_str(w, positions[p].version);
			write_position(w, positions[p].stage, positions[p].status, positions[p].error);
			jw_obj_close(w);
		}
		jw_arr_close(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);

	/*
	 * The per-stage tally the pipeline view renders as a flow: how many
	 * packages stand at each of the eleven stages. Every stage is
	 * listed even at zero -- a pipeline drawn only from the stages that
	 * happen to be occupied is not a pipeline, it is a filtered list,
	 * and the empty stages are exactly the ones an operator wants to
	 * see are empty.
	 */
	jw_key(w, "stages");
	jw_arr_open(w);
	for (i = 0; i < PIPELINE_STAGE_COUNT; i++) {
		jw_obj_open(w);
		jw_key(w, "stage");
		jw_str(w, pipeline_stage_name((enum pipeline_stage)i));
		jw_key(w, "verb");
		jw_str(w, pipeline_stage_verb((enum pipeline_stage)i));
		jw_key(w, "packages");
		jw_num(w, (double)stage_counts[i]);
		jw_obj_close(w);
	}
	jw_arr_close(w);

	write_deploy(w);

	/*
	 * #371: the other three kinds, and the edges between them. Placed
	 * AFTER everything ADR-0256 already emitted, and adding keys only
	 * -- `packages`, `stages` and `deploy` are byte-identical to what
	 * they were, so the existing page and CLI keep working across the
	 * deploy that introduces this. A generalisation that broke its own
	 * predecessor's contract would be a regression wearing a feature's
	 * clothes.
	 *
	 * The stage tallies these add go into the same `stage_counts` the
	 * package loop filled, because the flow the dashboard draws is of
	 * the WHOLE delivery graph, not of packages with three more
	 * pictures beside it.
	 */
	write_images(w, stage_counts);
	write_deployments(w, stage_counts);
	write_edges(w);

	/*
	 * The stage list per kind, so a renderer draws each lane from the
	 * daemon's own answer rather than from a copy of the table that
	 * will drift. Same reason ADR-0256 emits `stages` at all.
	 */
	jw_key(w, "kinds");
	jw_arr_open(w);
	for (i = 0; i < PIPELINE_KIND_COUNT; i++) {
		enum pipeline_stage ks[PIPELINE_STAGE_COUNT];
		int kn = pipeline_kind_stages((enum pipeline_kind)i, ks);
		int q;

		jw_obj_open(w);
		jw_key(w, "kind");
		jw_str(w, pipeline_kind_name((enum pipeline_kind)i));
		jw_key(w, "stages");
		jw_arr_open(w);
		for (q = 0; q < kn; q++)
			jw_str(w, pipeline_stage_name(ks[q]));
		jw_arr_close(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);

	jw_key(w, "total");
	jw_num(w, (double)n);
	jw_key(w, "ok");
	jw_num(w, (double)status_counts[PIPELINE_OK]);
	jw_key(w, "blocked");
	jw_num(w, (double)status_counts[PIPELINE_BLOCKED]);
	jw_key(w, "failed");
	jw_num(w, (double)status_counts[PIPELINE_FAILED]);
	jw_key(w, "cancelled");
	jw_num(w, (double)status_counts[PIPELINE_CANCELLED]);
	jw_key(w, "not_implemented");
	jw_num(w, (double)status_counts[PIPELINE_NOT_IMPLEMENTED]);
	jw_obj_close(w);
}
