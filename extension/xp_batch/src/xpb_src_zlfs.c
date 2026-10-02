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

    /*
     * Borrow pointers directly into the zone arrays -- zero copy.
     *
     * Lifetime: the zone is owned by the ZLFS registry, not by this source,
     * and it outlives the scan, so these borrowed pointers stay valid for the
     * whole pipeline rather than only until the next next_batch().  That is
     * stronger than the contract promises; consumers must not rely on it.
     *
     * Validity is the one thing that cannot always be borrowed.  The zone's
     * bitmap covers the WHOLE zone, so a batch starting at row `cursor` needs
     * bits [cursor, cursor+chunk) starting at bit 0.  When cursor is a
     * multiple of 8 that is just a shifted pointer; otherwise the bits have
     * to be restated into a small owned buffer.  Consumers cannot tell the
     * difference, which is the point of per-column ownership.
     */
    batch->nrows = chunk;
    batch->ncols = st->cols_used;

    for (int c = 0; c < st->cols_used; c++)
    {
        int             zc = st->col_map[c];
        XpBatchColumn  *bc = &batch->cols[c];
        uint8          *valid = NULL;
        bool            valid_owned = false;

        if (zz->col_validity[zc] != NULL)
        {
            if ((st->cursor & 7) == 0)
            {
                valid = zz->col_validity[zc] + (st->cursor >> 3);
            }
            else
            {
                size_t  nb = XPCB_VALIDITY_BYTES(chunk);
                uint8  *buf = palloc0(nb);

                for (int r = 0; r < chunk; r++)
                {
                    int64 z = st->cursor + r;

                    if (zz->col_validity[zc][z >> 3] & (1 << (z & 7)))
                        buf[r >> 3] |= (1 << (r & 7));
                }
                valid = buf;
                valid_owned = true;
            }
        }

        switch (zz->col_types[zc])
        {
            case ZLFS_COL_INT4:
                xpcb_col_borrow(bc, XPB_COL_INT4,
                                (int32 *) zz->cols[zc] + st->cursor, valid);
                break;
            case ZLFS_COL_INT8:
                xpcb_col_borrow(bc, XPB_COL_INT8,
                                (int64 *) zz->cols[zc] + st->cursor, valid);
                break;
            default:
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("ZLFS source: zone column %d has unsupported type %d",
                                zc, (int) zz->col_types[zc])));
        }
        bc->owns_validity = valid_owned;
    }

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
    src->caps.supports_rescan = true;        /* the zone is already in memory */
    src->private_state = st;
    return src;
}
