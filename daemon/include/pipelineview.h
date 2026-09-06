#ifndef PIPELINEVIEW_H
#define PIPELINEVIEW_H

#include "json.h"

/*
 * ADR-0256: GET /v1/pipeline -- the read-time join.
 *
 * Three owners already hold the truth and none of them can see the
 * others: the source catalogue (srcresolve.h) knows stages discover
 * through author, the package job records (pkg.h) know fetch through
 * roll, and the ESP's own boot entries know deploy. This joins them at
 * read time and STORES NOTHING, for the reason ADR-0155 proved the hard
 * way -- derived state with no invalidation event goes stale silently,
 * and a stale copy of "what is broken" is worse than no copy.
 *
 * ROW GRAIN. One row per package, because that is the grain recipes
 * have. Builds are per (package, image), so a row reports the WORST
 * position across that package's images with the per-image detail
 * underneath. Decided in the ADR rather than per surface, because the
 * CLI table and the web view both inherit it and they must not
 * disagree about what a row counts.
 *
 * DEPLOY IS NOT PER PACKAGE. It is one fact about this host -- did the
 * root that booted actually come up and serve? -- so it is reported
 * once, beside the package list, rather than repeated onto 116 rows
 * that would all carry the same value. Pretending otherwise would make
 * the summary counts meaningless.
 */
void pipelineview_write_json(struct json_writer *w);

#endif /* PIPELINEVIEW_H */
