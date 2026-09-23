/*
 * xpb_src_heap.c — HeapBatchSource: scans heap into compact column batches.
 *
 * Uses raw fixed-offset access for NOT NULL int32 columns with
 */
#include "postgres.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "xpb_colbatch.h"

/* Column offsets for reg_buh fixed schema */
typedef struct HeapColDef
{
    int     offset;     /* byte offset from tuple data start */
} HeapColDef;

typedef struct HeapBatchState
{
    Relation        rel;
    Snapshot        snap;
    BlockNumber     nblocks;
    BlockNumber     cur_block;
    OffsetNumber    cur_offset;
    Buffer          vmbuf;
    Oid             rel_oid;
    bool            done;

    /* Column extraction */
    int             ncols;
    HeapColDef      coldefs[XPCB_MAX_COLS];

    /* Optional predicate on first column (period_key range) */
    bool            has_pred;
    int32           pred_lo;
    int32           pred_hi;

    /* Page pruning */
    int64           pages_rejected;
    int64           pages_scanned;
    int64           tuples_visited;
    int64           tuples_passed;
} HeapBatchState;

static bool
xpb_heap_next_batch(XpBatchSource *src, XpColumnBatch *batch)
{
    HeapBatchState *st = src->private_state;

    if (st->done)
        return false;

    xpcb_reset(batch);
    batch->ncols = st->ncols;

    /*
     * Allocate owned column arrays if the consumer did not pre-allocate.
     * This path only runs on layouts the guard in xpb_heap_source_create()
     * accepted: every addressed attribute fixed-width and NOT NULL.  So every
     * column is int4 with no validity bitmap -- the cheap case.
     */
    for (int c = 0; c < st->ncols; c++)
    {
        if (!batch->cols[c].data)
            xpcb_col_alloc(&batch->cols[c], XPB_COL_INT4, batch->capacity, false);
    }

    /* Type dispatch happens here, once per batch -- never in the row loop. */
    int32 *out[XPCB_MAX_COLS];

    for (int c = 0; c < st->ncols; c++)
        out[c] = xpcb_i32(batch, c);

    int nrows = 0;
    int cap = batch->capacity;

    while (st->cur_block < st->nblocks && nrows < cap)
    {
        Buffer buf = ReadBufferExtended(st->rel, MAIN_FORKNUM,
                                        st->cur_block, RBM_NORMAL, NULL);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buf);
        bool all_visible = (visibilitymap_get_status(st->rel, st->cur_block,
                            &st->vmbuf) & VISIBILITYMAP_ALL_VISIBLE) != 0;
        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

        /* No page pruning in heap source — data not strictly sorted.
         * Per-tuple predicate handles filtering. */

        st->pages_scanned++;

        OffsetNumber off;
        for (off = st->cur_offset; off <= maxoff && nrows < cap; off++)
        {
            ItemId lp = PageGetItemId(page, off);
            if (!ItemIdIsNormal(lp)) continue;
            HeapTupleHeader htup = (HeapTupleHeader) PageGetItem(page, lp);

            /* Visibility check */
            if (!all_visible)
            {
                HeapTupleData td;
                td.t_data = htup;
                td.t_len = ItemIdGetLength(lp);
                td.t_tableOid = st->rel_oid;
                ItemPointerSet(&td.t_self, st->cur_block, off);
                if (!HeapTupleSatisfiesVisibility(&td, st->snap, buf))
                    continue;
            }

            st->tuples_visited++;

            char *d = (char *)htup + htup->t_hoff;

            /* Predicate on first col */
            int32 pk = *(int32 *)(d + st->coldefs[0].offset);
            if (st->has_pred && (pk < st->pred_lo || pk > st->pred_hi))
                continue;

            st->tuples_passed++;

            /* Extract all columns */
            for (int c = 0; c < st->ncols; c++)
                out[c][nrows] = *(int32 *)(d + st->coldefs[c].offset);

            nrows++;
        }
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        ReleaseBuffer(buf);

        if (off > maxoff)
        {
            /* Finished this page, move to next */
            st->cur_block++;
            st->cur_offset = FirstOffsetNumber;
        }
        else
        {
            /* Batch full mid-page, resume here next call */
            st->cur_offset = off;
        }
    }

    if (nrows == 0)
    {
        st->done = true;
        return false;
    }

    batch->nrows = nrows;
    return true;
}

static void
xpb_heap_rescan(XpBatchSource *src)
{
    HeapBatchState *st = src->private_state;
    st->cur_block = 0;
    st->cur_offset = FirstOffsetNumber;
    st->done = false;
    st->pages_rejected = 0;
    st->pages_scanned = 0;
    st->tuples_visited = 0;
    st->tuples_passed = 0;
}

static void
xpb_heap_end(XpBatchSource *src)
{
    HeapBatchState *st = src->private_state;
    if (st->vmbuf != InvalidBuffer)
        ReleaseBuffer(st->vmbuf);
    table_close(st->rel, AccessShareLock);
}

static const XpBatchSourceOps heap_batch_ops = {
    .next_batch = xpb_heap_next_batch,
    .rescan     = xpb_heap_rescan,
    .end        = xpb_heap_end,
};

/*
 * Create a HeapBatchSource.
 * offsets: byte offsets of columns in tuple data.
 * ncols: number of columns.
 * pred_lo/pred_hi: predicate range on first column (or has_pred=false).
 */
/*
 * Create a HeapBatchSource.
 * requested_attnos: logical attribute numbers to extract.
 * ncols: number of columns.
 * Predicate on first requested column (range filter).
 */
XpBatchSource *
xpb_heap_source_create(Oid relid, int16 *requested_attnos, int ncols,
                        bool has_pred, int32 pred_lo, int32 pred_hi)
{
    HeapBatchState *st = palloc0(sizeof(HeapBatchState));
    st->rel = table_open(relid, AccessShareLock);
    st->snap = GetActiveSnapshot();
    st->nblocks = RelationGetNumberOfBlocks(st->rel);
    st->rel_oid = RelationGetRelid(st->rel);
    st->vmbuf = InvalidBuffer;
    st->ncols = ncols;
    st->cur_offset = FirstOffsetNumber;

    /* Compute byte offsets from TupleDesc */
    TupleDesc td = RelationGetDescr(st->rel);

    /*
     * The offsets below are computed once and then applied to every tuple, so
     * they are only valid for a layout in which each attribute sits at the same
     * place in every tuple.  That holds only while every attribute up to the
     * highest one we read is fixed-width and NOT NULL:
     *
     *   - a varlena (attlen = -1) has no fixed width, and the loop would add
     *     -1 to the running offset;
     *   - a NULL attribute occupies no storage, so one NULL anywhere before a
     *     column we read shifts that column in that tuple only;
     *   - a dropped column is NULL in every tuple written after the drop, so
     *     it shifts the same way.
     *
     * None of these announce themselves.  Without this check the source
     * silently returns values read from the wrong bytes -- with the right row
     * count, the right group count, and an internally consistent result.  The
     * caller's per-attnum type check cannot catch it: it asks about the
     * attributes we read, and the ones that break the layout are the ones in
     * between.  Verified by test/heap_layout_guard.sh.
     *
     * Attributes after the last one we read are unconstrained: they can shift
     * freely without moving anything we address.  t_hoff is read per tuple, so
     * the null bitmap's own effect on the data start is already handled.
     */
    int16 max_attno = 0;
    for (int i = 0; i < ncols; i++)
    {
        int16 attno = requested_attnos[i];
        if (attno < 1 || attno > td->natts)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("HeapBatchSource: attno %d out of range (1..%d) for \"%s\"",
                            attno, td->natts, RelationGetRelationName(st->rel))));
        if (attno > max_attno)
            max_attno = attno;
    }

    for (int a = 0; a < max_attno; a++)
    {
        Form_pg_attribute att = TupleDescAttr(td, a);

        if (att->attisdropped)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("HeapBatchSource: \"%s\" has a dropped column at attnum %d",
                            RelationGetRelationName(st->rel), a + 1),
                     errdetail("Fixed-offset access requires every attribute up to attnum %d to be fixed-width and NOT NULL.",
                               max_attno)));
        if (att->attlen <= 0)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("HeapBatchSource: \"%s\".%s is variable-width (attlen %d)",
                            RelationGetRelationName(st->rel),
                            NameStr(att->attname), att->attlen),
                     errdetail("Fixed-offset access requires every attribute up to attnum %d to be fixed-width and NOT NULL.",
                               max_attno)));
        if (!att->attnotnull)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("HeapBatchSource: \"%s\".%s is nullable",
                            RelationGetRelationName(st->rel),
                            NameStr(att->attname)),
                     errdetail("Fixed-offset access requires every attribute up to attnum %d to be fixed-width and NOT NULL.",
                               max_attno)));
    }

    for (int i = 0; i < ncols; i++)
    {
        int16 attno = requested_attnos[i];
        int offset = 0;

        for (int a = 0; a < attno - 1; a++)
        {
            Form_pg_attribute prev = TupleDescAttr(td, a);
            offset = att_align_nominal(offset, prev->attalign);
            offset += prev->attlen;
        }
        Form_pg_attribute attr = TupleDescAttr(td, attno - 1);
        offset = att_align_nominal(offset, attr->attalign);
        st->coldefs[i].offset = offset;
    }

    st->has_pred = has_pred;
    st->pred_lo = pred_lo;
    st->pred_hi = pred_hi;

    XpBatchSource *src = palloc(sizeof(XpBatchSource));
    src->ops = &heap_batch_ops;
    src->private_state = st;
    return src;
}

/* Accessor for stats */
void
xpb_heap_source_stats(XpBatchSource *src, int64 *pages_rej, int64 *pages_scan,
                       int64 *tuples_vis, int64 *tuples_pass)
{
    HeapBatchState *st = src->private_state;
    *pages_rej = st->pages_rejected;
    *pages_scan = st->pages_scanned;
    *tuples_vis = st->tuples_visited;
    *tuples_pass = st->tuples_passed;
}
