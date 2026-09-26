#ifndef CIX_RECIPE_FORMAT_H
#define CIX_RECIPE_FORMAT_H

/*
 * What makes a recipe a CBS recipe (ADR-0305): its filename.
 *
 * `<name>/<version>/build.sh` is a shell recipe and
 * `<name>/<version>/build.cbs` a CBS recipe in CPDL 0.1. Nothing sniffs
 * content and no recipe declares its own language, because a filename
 * cannot disagree with what will actually run.
 *
 * These live here, in the shared include tree, because THREE things
 * have to agree about them and each would otherwise carry its own copy
 * of the string:
 *
 *   - the daemon, resolving a published version to its recipe file and
 *     deciding which parser validates a publish
 *   - cixctl, deciding which format it is publishing from the file it
 *     was pointed at
 *   - test_toolchain_policy, walking the recipe corpus to count what
 *     builds with gcc -- a gate that COUNTS, so a format it fails to
 *     recognise is a silent zero rather than an error
 *
 * Three literals spelling one rule is a parallel implementation of the
 * rule ADR-0305 exists to state, however short each copy is. This
 * header rather than daemon/include/pkg.h deliberately: cixctl is a
 * pure REST client and must not include the daemon's API surface just
 * to learn a filename.
 *
 * The extension is not a preference of this project's either -- cbs
 * refuses any recipe path that does not end in it, in `build` as well
 * as `explain`.
 */
#define PKG_RECIPE_CBS_SUFFIX ".cbs"
#define PKG_RECIPE_SHELL_SUFFIX ".sh"
#define PKG_RECIPE_CBS_FILE "build" PKG_RECIPE_CBS_SUFFIX
#define PKG_RECIPE_SHELL_FILE "build" PKG_RECIPE_SHELL_SUFFIX

#endif /* CIX_RECIPE_FORMAT_H */
