#ifndef OVERLAY_TEST_COMMON_H
#define OVERLAY_TEST_COMMON_H

/* Shared between test_overlay.c (writes it into lowerdir) and
 * overlay_child.c (reads it back through the merged view) so the two
 * never drift out of sync on what "correct" looks like. */
#define OVERLAY_LOWER_MARKER_CONTENT "LOWER_MARKER_V1\n"

#endif /* OVERLAY_TEST_COMMON_H */
