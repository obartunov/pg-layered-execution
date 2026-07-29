#ifndef XPB_PAGEAGG_H
#define XPB_PAGEAGG_H

#include "postgres.h"

/* Page-level summary (pPD1+pPD2, pS2+pS3) */
typedef struct XpPageSummary
{
    int32   min_val;
    int32   max_val;
    int32   row_count;
    bool    valid;
    bool    all_visible;
} XpPageSummary;

/* Persistent summary loader (xpb_summary.c) */
extern XpPageSummary *xpb_load_summaries(Relation rel, BlockNumber nblocks);

#endif /* XPB_PAGEAGG_H */
