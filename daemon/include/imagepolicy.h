#ifndef IMAGEPOLICY_H
#define IMAGEPOLICY_H

#include "json.h"

/*
 * ADR-0320: an image's policy decides how its three copies of one fact
 * agree -- the image recipe (git, synced into the store), the live
 * manifest, and the installed set -- rather than one copy being made
 * the only one. Operator state, never recipe content, per image name,
 * shaped like pkgpolicy (ADR-0188). An image with no saved policy has
 * the defaults, and "no policy" and "the defaults" are one state.
 */

/* Whether a changed image recipe reaching the store is applied by itself. */
enum image_policy_recipe {
	IMAGE_POLICY_RECIPE_MANUAL = 0, /* default: only an operator applies it */
	IMAGE_POLICY_RECIPE_FOLLOW,     /* the recipe in git drives the image */
};

/* What applying a recipe does. */
enum image_policy_apply {
	IMAGE_POLICY_APPLY_DECLARE = 0, /* default: set the manifest only */
	IMAGE_POLICY_APPLY_CONVERGE,    /* also install to match it */
};

/* Whether an apply may move a pin to an older version. */
enum image_policy_downgrade {
	IMAGE_POLICY_DOWNGRADE_REFUSE = 0, /* default: 409, unless the request allows it */
	IMAGE_POLICY_DOWNGRADE_ALLOW,
};

struct image_policy {
	enum image_policy_recipe recipe;
	enum image_policy_apply apply;
	enum image_policy_downgrade downgrade;
};

int imagepolicy_init(const char *path);

/* Fills *out with name's policy, or the defaults when it has none. */
void imagepolicy_get(const char *name, struct image_policy *out);

/* Saves name's policy; a policy equal to the defaults removes the entry.
 * Returns 0, or -1 if the table is full or the state could not be saved. */
int imagepolicy_set(const char *name, const struct image_policy *p);

const char *imagepolicy_recipe_name(enum image_policy_recipe v);
const char *imagepolicy_apply_name(enum image_policy_apply v);
const char *imagepolicy_downgrade_name(enum image_policy_downgrade v);
int imagepolicy_recipe_parse(const char *s, enum image_policy_recipe *out);
int imagepolicy_apply_parse(const char *s, enum image_policy_apply *out);
int imagepolicy_downgrade_parse(const char *s, enum image_policy_downgrade *out);

/* {"image":...,"recipe":...,"apply":...,"downgrade":...} */
void imagepolicy_write_json(struct json_writer *w, const char *name, const struct image_policy *p);

#endif /* IMAGEPOLICY_H */
