/*
 * xpb_scalar_batch.h
 *
 * ScalarBatch — first-class dense carrier for native scalar data.
 *
 * Not a TupleTableSlot replacement. A consumer-shaped batch:
 * contains only what the consumer needs, in the form it needs.
 *
 * Supported types: int4, int8, date, timestamp (TypeOps line).
 * No varlena, no text, no collation.
 *
 * Layout:
 *   nrows    — number of valid rows
 *   capacity — allocated size
 *   cols[]   — array of column vectors (int64 each)
 *   ncols    — number of columns
 */

#ifndef XPB_SCALAR_BATCH_H
#define XPB_SCALAR_BATCH_H

#include "postgres.h"

#define SB_MAX_COLS 4    /* k1, k2, val, (spare) */

typedef struct ScalarBatch
{
    int         nrows;
    int         capacity;
    int         ncols;
    int64      *cols[SB_MAX_COLS];  /* dense column arrays */
} ScalarBatch;

static inline ScalarBatch *
sb_create(int capacity, int ncols)
{
    ScalarBatch *sb = palloc0(sizeof(ScalarBatch));
    sb->capacity = capacity;
    sb->ncols = ncols;
    sb->nrows = 0;
    for (int i = 0; i < ncols && i < SB_MAX_COLS; i++)
        sb->cols[i] = palloc(capacity * sizeof(int64));
    return sb;
}

static inline void
sb_reset(ScalarBatch *sb)
{
    sb->nrows = 0;
}

static inline void
sb_free(ScalarBatch *sb)
{
    for (int i = 0; i < sb->ncols; i++)
        if (sb->cols[i]) pfree(sb->cols[i]);
    pfree(sb);
}

#endif  /* XPB_SCALAR_BATCH_H */
