/*
 * See pipelineview.h for the row grain and why nothing here is stored.
 */
#include "pipelineview.h"

#include "esp.h"
#include "pipeline.h"
#include "pkg.h"
#include "srcresolve.h"

#include <stdio.h>
#include <string.h>

#define PIPELINEVIEW_MAX_PACKAGES 512
#define PIPELINEVIEW_MAX_RECIPE_VERSIONS 64
#define PIPELINEVIEW_MAX_IMAGES 32
#define PIPELINEVIEW_MAX_ESP_ENTRIES 32

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
