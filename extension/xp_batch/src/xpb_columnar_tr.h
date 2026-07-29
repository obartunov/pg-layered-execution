/*
 * xpb_columnar_tr.h
 *
 * Columnar TR carrier for transient intermediate states.
 * Dense int64 column arrays, no tuples, no slots on the hot path.
 *
 * v0: fixed-width int64 only, no NULLs, no compression.
 */
#ifndef XPB_COLUMNAR_TR_H
#define XPB_COLUMNAR_TR_H

#include "postgres.h"
#include "utils/memutils.h"

#define CTR_MAX_COLS 8

typedef struct ColumnarTR
{
    int32           ncols;
    int64           nrows;
    int64           capacity;
    int64          *cols[CTR_MAX_COLS];  /* cols[col][row] */
    MemoryContext   mcxt;                /* owner context */
} ColumnarTR;

static inline ColumnarTR *
ctr_create(int ncols, int64 capacity, MemoryContext mcxt)
{
    MemoryContext old = MemoryContextSwitchTo(mcxt);
    ColumnarTR *ctr = palloc0(sizeof(ColumnarTR));
    ctr->ncols = ncols;
    ctr->capacity = capacity;
    ctr->nrows = 0;
    ctr->mcxt = mcxt;
    for (int i = 0; i < ncols; i++)
        ctr->cols[i] = palloc(capacity * sizeof(int64));
    MemoryContextSwitchTo(old);
    return ctr;
}

static inline void
ctr_append_row(ColumnarTR *ctr, int64 *values)
{
    Assert(ctr->nrows < ctr->capacity);
    for (int c = 0; c < ctr->ncols; c++)
        ctr->cols[c][ctr->nrows] = values[c];
    ctr->nrows++;
}

static inline void
ctr_free(ColumnarTR *ctr)
{
    for (int c = 0; c < ctr->ncols; c++)
        if (ctr->cols[c]) pfree(ctr->cols[c]);
    pfree(ctr);
}

/* Bytes used (for metrics) */
static inline int64
ctr_bytes(ColumnarTR *ctr)
{
    return sizeof(ColumnarTR) + ctr->ncols * ctr->nrows * sizeof(int64);
}

#endif /* XPB_COLUMNAR_TR_H */
