#ifndef CIX_OSRELEASE_H
#define CIX_OSRELEASE_H

#include <stddef.h>

/*
 * The platform's own identity, as /etc/os-release.
 *
 * ONE definition, called from both places that stage the file -- the
 * control-plane root (image/src/mkbootroot.c) and every container image
 * (pkg_seed_image_baseline()). They are assembled by different programs
 * at different times, and two copies of this text would drift the way
 * every duplicated constant in this project has: silently, and in the
 * direction that makes a host and the containers on it disagree about
 * what they are.
 *
 * Renders into `out`. Returns 0 on success, -1 if `out` is NULL or too
 * small -- the content is short and fixed, so a truncated write means a
 * caller's buffer is wrong, not that the identity is unrepresentable.
 *
 * `build_version` is stamped as BUILD_ID. Pass NULL or "" to omit the
 * field entirely rather than write an empty or invented one.
 */
int osrelease_render(char *out, size_t out_size, const char *build_version);

/* Enough for the rendered content with room to spare. */
#define OSRELEASE_MAX 512

#endif /* CIX_OSRELEASE_H */
