/*
 * xpb_src_zlfs.c — ZlfsBatchSource: serves ZLFS zone columns as
 *                   borrowed compact batches (zero materialization cost).
 */
#include "postgres.h"
#include "xpb_colbatch.h"
#include "xpb_zlfs.h"

typedef struct ZlfsBatchState
{
    ZlfsZone   *zone;
    int64       cursor;     /* next row to emit */
    int         cols_used;  /* how many cols to expose (3 for benchmark) */
    int         col_map[XPCB_MAX_COLS]; /* ZLFS col index per batch col */
} ZlfsBatchState;

static bool
zlfs_next_batch(XpBatchSource *src, XpColumnBatch *batch)
{
    ZlfsBatchState *st = src->private_state;
    ZlfsZone       *zz = st->zone;

    if (st->cursor >= zz->nrows)
        return false;

    int64 remaining = zz->nrows - st->cursor;
    int   chunk = (remaining > batch->capacity) ? batch->capacity : (int)remaining;

    /* Borrow pointers directly into zone arrays — zero copy */
    batch->nrows = chunk;
    batch->owns_data = false;
    batch->ncols = st->cols_used;
    for (int c = 0; c < st->cols_used; c++)
        batch->int32_cols[c] = zz->cols[st->col_map[c]] + st->cursor;

    batch->selection = NULL;
    batch->nselected = 0;

    st->cursor += chunk;
    return true;
}

static void
zlfs_rescan(XpBatchSource *src)
{
    ZlfsBatchState *st = src->private_state;
    st->cursor = 0;
}

static void
zlfs_end(XpBatchSource *src)
{
    /* zone memory owned by ZLFS registry, not by us */
}

static const XpBatchSourceOps zlfs_batch_ops = {
    .next_batch = zlfs_next_batch,
    .rescan     = zlfs_rescan,
    .end        = zlfs_end,
};

/*
 * Create a ZlfsBatchSource for the given zone.
 * requested_attnos: logical attribute numbers the consumer wants.
 * ncols: number of requested columns.
 *
 * Resolves each attno to the zone's physical column index.
 * Consumer never needs to know zone's internal column layout.
 */
XpBatchSource *
xpb_zlfs_source_create(ZlfsZone *zone, int16 *requested_attnos, int ncols)
{
    ZlfsBatchState *st = palloc0(sizeof(ZlfsBatchState));
    st->zone = zone;
    st->cursor = 0;
    st->cols_used = ncols;

    for (int i = 0; i < ncols; i++)
    {
        int16 wanted = requested_attnos[i];
        int found = -1;
        for (int c = 0; c < zone->ncols; c++)
        {
            if (zone->col_attnos[c] == wanted)
            { found = c; break; }
        }
        if (found < 0)
            ereport(ERROR,
                    (errmsg("ZLFS source: zone does not contain attno %d", wanted)));
        st->col_map[i] = found;
    }

    XpBatchSource *src = palloc(sizeof(XpBatchSource));
    src->ops = &zlfs_batch_ops;
    src->private_state = st;
    return src;
}
