/*
 * xpb_colbatch.h — compact columnar batch contract
 *
 * A batch is a set of dense int32 column arrays with an optional
 * selection vector.  Providers fill batches; consumers read them.
 * Neither side knows the other's storage internals.
 */
#ifndef XPB_COLBATCH_H
#define XPB_COLBATCH_H

#include "postgres.h"
#include <stdint.h>

#define XPCB_MAX_COLS   8
#define XPCB_BATCH_CAP  65536   /* max rows per batch */

/* ── Compact columnar batch ── */

typedef struct XpColumnBatch
{
    int         nrows;          /* rows in this batch                    */
    int         capacity;       /* allocated capacity                    */
    int         ncols;          /* number of columns                     */

    int32      *int32_cols[XPCB_MAX_COLS];  /* dense column arrays       */
    bool        owns_data;      /* if false, arrays are borrowed         */

    /* Optional selection vector (NULL = all rows selected) */
    uint32     *selection;
    int         nselected;      /* valid only if selection != NULL        */
} XpColumnBatch;

/* ── Batch source provider contract ── */

typedef struct XpBatchSource XpBatchSource;

typedef struct XpBatchSourceOps
{
    /*
     * Fill 'batch' with the next chunk of rows.
     * Returns true if batch has rows, false when exhausted.
     * Provider may borrow pointers (owns_data=false) or copy.
     */
    bool    (*next_batch)(XpBatchSource *src, XpColumnBatch *batch);

    /* Reset for rescan */
    void    (*rescan)(XpBatchSource *src);

    /* Release resources */
    void    (*end)(XpBatchSource *src);
} XpBatchSourceOps;

struct XpBatchSource
{
    const XpBatchSourceOps *ops;
    void                   *private_state;
};

/* ── Helpers ── */

static inline void
xpcb_init(XpColumnBatch *b, int ncols, int capacity)
{
    memset(b, 0, sizeof(*b));
    b->ncols = ncols;
    b->capacity = capacity;
    b->owns_data = true;
    for (int c = 0; c < ncols; c++)
        b->int32_cols[c] = palloc(capacity * sizeof(int32));
}

static inline void
xpcb_reset(XpColumnBatch *b)
{
    b->nrows = 0;
    b->nselected = 0;
    b->selection = NULL;
}

static inline void
xpcb_free(XpColumnBatch *b)
{
    if (b->owns_data)
    {
        for (int c = 0; c < b->ncols; c++)
            if (b->int32_cols[c])
                pfree(b->int32_cols[c]);
    }
    if (b->selection)
        pfree(b->selection);
    memset(b, 0, sizeof(*b));
}

#endif /* XPB_COLBATCH_H */
