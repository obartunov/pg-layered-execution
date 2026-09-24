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
    XPB_HEAP_PROJECTED      /* walk like heap_deform_tuple, materialize only
                             * the requested attributes.  EXPERIMENTAL:
                             * benchmark 05-C, never auto-selected.          */
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

extern void xpb_heap_source_stats(XpBatchSource *src, int64 *pages_rej,
                                  int64 *pages_scan, int64 *tuples_vis,
                                  int64 *tuples_pass);

#endif /* XPB_SRC_HEAP_H */
