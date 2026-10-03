#ifndef CIX_RECIPE_FORMAT_H
#define CIX_RECIPE_FORMAT_H

/*
 * A recipe's filename (ADR-0305): `<name>/<version>/build.cbs`, a CBS
 * recipe in CPDL. It is the only recipe there is -- shell recipes do
 * not exist (cix#569, ADR-0329) -- and nothing sniffs content.
 *
 * These live in the shared include tree rather than daemon/include/
 * pkg.h because more than the daemon has to agree about them: tests
 * that lay out a recipe store use the same constant, and one literal
 * spelled several times is a parallel implementation of the rule
 * however short each copy is.
 *
 * The extension is not a preference of this project's either -- cbs
 * refuses any recipe path that does not end in it, in `build` as well
 * as `explain`.
 */
#define PKG_RECIPE_CBS_SUFFIX ".cbs"
#define PKG_RECIPE_CBS_FILE "build" PKG_RECIPE_CBS_SUFFIX

#endif /* CIX_RECIPE_FORMAT_H */
