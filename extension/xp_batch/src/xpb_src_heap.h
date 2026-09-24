/*
 * xpb_src_heap.h — HeapBatchSource public contract
 *
 * Two scan paths, chosen by the caller rather than inferred.  See the header
 * comment of xpb_src_heap.c for what each one assumes.
 */
#ifndef XPB_SRC_HEAP_H
#define XPB_SRC_HEAP_H

#include "postgres.h"
#include "utils/rel.h"

#include "xpb_colbatch.h"

typedef enum XpbHeapPath
{
    XPB_HEAP_FIXED = 0,     /* raw fixed-offset, fixed-width NOT NULL prefix */
    XPB_HEAP_DEFORM,        /* heap_deform_tuple, nullable and mixed types   */
    XPB_HEAP_PROJECTED,     /* walk like heap_deform_tuple, materialize only
                             * the requested attributes.  EXPERIMENTAL:
                             * benchmark 05-C, never auto-selected.          */
    XPB_HEAP_PROJECTED_EARLY /* as PROJECTED, but evaluate the range predicate
                             * the moment its attribute has been walked and
                             * abandon the tuple on rejection.  EXPERIMENTAL:
                             * benchmark 05-D, never auto-selected.          */
} XpbHeapPath;

/*
 * Is this layout eligible for the fixed-offset path?
 *
 * Returns false and, when 'why' is non-NULL, sets it to a palloc'd sentence
 * naming the first attribute that rules it out, so a caller that wants to
 * choose deliberately can say why it chose.  Does not raise errors.
 */
extern bool xpb_heap_layout_supports_fixed(Relation rel, const int16 *attnos,
                                           int ncols, char **why);

/*
 * Map a PostgreSQL type to its batch column type.  Returns XPB_COL_UNSET for
 * a type the batch contract does not cover -- deliberately a small set.
 */
extern XpbColType xpb_heap_coltype_for(Oid atttypid);

/*
 * Create a heap batch source.
 *
 * requested_attnos: logical attribute numbers to extract, in batch column
 * order.  The batch's column types are derived from the relation, not
 * supplied by the caller, so the batch always describes what it actually
 * holds.  Predicate, when has_pred, is a range filter on the FIRST requested
 * column, which must be int4.
 */
extern XpBatchSource *xpb_heap_source_create_ex(Oid relid,
                                                int16 *requested_attnos,
                                                int ncols, XpbHeapPath path,
                                                bool has_pred,
                                                int32 pred_lo, int32 pred_hi);

/* Fixed-path constructor, unchanged for existing callers. */
extern XpBatchSource *xpb_heap_source_create(Oid relid, int16 *requested_attnos,
                                             int ncols, bool has_pred,
                                             int32 pred_lo, int32 pred_hi);

extern void xpb_heap_source_deform_stats(XpBatchSource *src,
                                         int64 *tuples_deformed,
                                         int64 *attrs_deformed,
                                         int *attrs_requested,
                                         bool *is_deform_path);

/*
 * Projected-path accounting.  attributes_walked counts what the tuple layout
 * forced us to visit in order to locate later attributes; attributes_
 * materialized counts what was actually stored into the batch.  The two
 * differ exactly by the unused attributes a layout makes us step over.
 */
extern void xpb_heap_source_projected_stats(XpBatchSource *src,
                                            int64 *tuples_scanned,
                                            int64 *attributes_walked,
                                            int64 *attributes_materialized);

/*
 * Early-rejection accounting, split by outcome.  The point of the split is to
 * show that a rejected tuple genuinely stopped early: on an empty range with
 * the predicate at attnum 1, attributes_walked_rejected should be one per
 * tuple, not the whole row.  Incremented in the walker, never derived from
 * selectivity afterwards.
 */
extern void xpb_heap_source_early_stats(XpBatchSource *src,
                                        int64 *tuples_accepted,
                                        int64 *tuples_rejected_early,
                                        int64 *walked_accepted,
                                        int64 *walked_rejected,
                                        int64 *materialized_accepted,
                                        int64 *materialized_rejected);

extern void xpb_heap_source_stats(XpBatchSource *src, int64 *pages_rej,
                                  int64 *pages_scan, int64 *tuples_vis,
                                  int64 *tuples_pass);

#endif /* XPB_SRC_HEAP_H */
