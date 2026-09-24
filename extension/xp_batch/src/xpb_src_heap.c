/*
 * xpb_src_heap.c — HeapBatchSource: scans heap into compact column batches.
 *
 * TWO PATHS, CHOSEN EXPLICITLY
 * ---------------------------
 * XPB_HEAP_FIXED    Raw fixed-offset access.  Computes each column's byte
 *                   offset once and applies it to every tuple, which is only
 *                   valid while every attribute up to the highest one read is
 *                   fixed-width and NOT NULL.  int4 columns only.  This is
 *                   the path benchmarks 02 and 04 measure.
 *
 * XPB_HEAP_DEFORM   PostgreSQL tuple deformation.  Handles nullable
 *                   attributes, varlena before a requested column, and mixed
 *                   fixed-width types.  Slower per row; correct on any
 *                   layout the type set covers.
 *
 * The caller states which one it wants.  There is no fallback hidden inside
 * the scan loop: xpb_heap_layout_supports_fixed() answers whether a layout is
 * eligible, and the caller decides.  Asking for XPB_HEAP_FIXED on a layout
 * that cannot support it is an error, not a silent downgrade -- that guard is
 * what 326b116 added and it is not weakened by the generic path existing.
 *
 * LOCKING
 * -------
 * The fixed path walks blocks itself and holds a share lock on the page while
 * copying, which is safe because it only ever reads fixed-width byval data.
 * The deform path cannot do that: a varlena may be external, and detoasting
 * under a buffer lock is not allowed.  It therefore goes through the table AM
 * (table_beginscan/heap_getnext), which hands back a tuple whose buffer is
 * pinned but unlocked -- the same position any ordinary executor node
 * deforms from.
 *
 * OWNERSHIP ON THE DEFORM PATH
 * ----------------------------
 * Column arrays and every numeric/varlena payload they point at are
 * allocated in the source's own per-batch context, which is reset at the top
 * of each next_batch().  Columns are therefore marked BORROWED: valid until
 * the next next_batch()/rescan()/end(), exactly the contract's borrow window.
 * That keeps the pointer-valued types out of per-value pfree() entirely.
 */
#include "postgres.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "access/tupmacs.h"
#include "catalog/pg_type.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "xpb_colbatch.h"
#include "xpb_src_heap.h"

/* Column offsets for reg_buh fixed schema */
typedef struct HeapColDef
{
    int     offset;     /* byte offset from tuple data start */
    int     width;      /* 4 or 8; the fixed path copies exactly this many */
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

    XpbHeapPath     path;

    /* Column extraction */
    int             ncols;
    HeapColDef      coldefs[XPCB_MAX_COLS];     /* fixed path only  */
    int16           attnos[XPCB_MAX_COLS];
    XpbColType      coltypes[XPCB_MAX_COLS];

    /* Deform path */
    TableScanDesc   scan;
    MemoryContext   batch_cxt;      /* reset per batch; owns payloads */
    Datum          *dvalues;        /* heap_deform_tuple scratch      */
    bool           *dnulls;

    /* Optional predicate on first column (period_key range) */
    bool            has_pred;
    int32           pred_lo;
    int32           pred_hi;

    /* Page pruning */
    int64           pages_rejected;
    int64           pages_scanned;
    int64           tuples_visited;
    int64           tuples_passed;

    /*
     * Deform accounting, observational and outside the row loop's inner work.
     * heap_deform_tuple() deforms EVERY attribute of the tuple descriptor,
     * not just the ones requested and not just up to the highest requested
     * attnum -- so attrs_deformed grows with the width of the table rather
     * than with the projection.  Counted so that can be stated from a
     * measurement instead of from reading the source.
     */
    int64           tuples_deformed;
    int64           attrs_deformed;
    int             attrs_requested;

    /* projected path */
    int16           max_attno;              /* highest requested, 1-based   */
    signed char     want[MaxTupleAttributeNumber]; /* attnum-1 -> batch col, or -1 */
    int64           tuples_scanned;
    int64           attributes_walked;
    int64           attributes_materialized;
} HeapBatchState;

/* ── Type mapping ── */

XpbColType
xpb_heap_coltype_for(Oid atttypid)
{
    switch (atttypid)
    {
        case INT4OID:       return XPB_COL_INT4;
        case INT8OID:       return XPB_COL_INT8;
        case NUMERICOID:    return XPB_COL_NUMERIC;
        case TEXTOID:
        case VARCHAROID:
        case BPCHAROID:
        case BYTEAOID:      return XPB_COL_VARLENA;
        default:            return XPB_COL_UNSET;
    }
}

/* ── Fixed-path eligibility ── */

bool
xpb_heap_layout_supports_fixed(Relation rel, const int16 *attnos, int ncols,
                               char **why)
{
    TupleDesc   td = RelationGetDescr(rel);
    int16       max_attno = 0;

    if (why)
        *why = NULL;

    for (int i = 0; i < ncols; i++)
    {
        if (attnos[i] < 1 || attnos[i] > td->natts)
        {
            if (why)
                *why = psprintf("attnum %d is out of range (1..%d)",
                                attnos[i], td->natts);
            return false;
        }
        {
            Oid t = TupleDescAttr(td, attnos[i] - 1)->atttypid;

            /*
             * Fixed-offset extraction copies a fixed number of bytes from a
             * computed offset, so it can serve any by-value fixed-width type
             * the batch carries -- int4 and int8.  A numeric or varlena has
             * no fixed width and is excluded by the prefix rule below in any
             * case.
             */
            if (t != INT4OID && t != INT8OID)
            {
                if (why)
                    *why = psprintf("column \"%s\" is not int4 or int8",
                                    NameStr(TupleDescAttr(td, attnos[i] - 1)->attname));
                return false;
            }
        }
        if (attnos[i] > max_attno)
            max_attno = attnos[i];
    }

    for (int a = 0; a < max_attno; a++)
    {
        Form_pg_attribute att = TupleDescAttr(td, a);

        if (att->attisdropped)
        {
            if (why)
                *why = psprintf("attnum %d is a dropped column", a + 1);
            return false;
        }
        if (att->attlen <= 0)
        {
            if (why)
                *why = psprintf("column \"%s\" is variable-width",
                                NameStr(att->attname));
            return false;
        }
        if (!att->attnotnull)
        {
            if (why)
                *why = psprintf("column \"%s\" is nullable", NameStr(att->attname));
            return false;
        }
    }
    return true;
}

/* ── Deform path ── */

/*
 * Copy one attribute into batch column 'c' at row 'r'.
 *
 * Pointer-valued types are detoasted and copied into the caller's current
 * context, which is the source's per-batch context.  Nothing here is freed
 * per value; the context reset at the top of the next next_batch() releases
 * the lot.
 */
static void
xpb_heap_store_value(HeapBatchState *st, XpColumnBatch *batch, int c, int r,
                     Datum value, bool isnull)
{
    XpBatchColumn *col = &batch->cols[c];

    if (isnull)
    {
        /* First NULL in this column: the bitmap appears only when needed. */
        xpcb_col_add_validity(col, batch->capacity);
        /*
         * The bitmap was allocated in the source's per-batch context along
         * with everything else this batch points at, so it is borrowed like
         * the data.  Leaving owns_validity set would have a consumer pfree()
         * it and the context reset free it again on the next batch.
         */
        col->owns_validity = false;
        xpcb_set_null(col, r);
        /*
         * Leave the data slot untouched.  It is never read for a NULL row,
         * and writing a placeholder is how sentinels get invented.
         */
        return;
    }

    if (col->validity)
        xpcb_set_valid(col, r);

    switch (col->type)
    {
        case XPB_COL_INT4:
            ((int32 *) col->data)[r] = DatumGetInt32(value);
            break;
        case XPB_COL_INT8:
            ((int64 *) col->data)[r] = DatumGetInt64(value);
            break;
        case XPB_COL_NUMERIC:
        case XPB_COL_VARLENA:
            /* detoast and copy: the tuple's buffer is only pinned */
            ((Datum *) col->data)[r] =
                PointerGetDatum(PG_DETOAST_DATUM_COPY(value));
            break;
        case XPB_COL_UNSET:
            elog(ERROR, "xp_batch: heap source column %d has no type", c);
    }
}

static bool
xpb_heap_deform_next_batch(XpBatchSource *src, XpColumnBatch *batch)
{
    HeapBatchState *st = src->private_state;
    TupleDesc       td = RelationGetDescr(st->rel);
    MemoryContext   old;
    HeapTuple       tup;
    int             nrows = 0;

    if (st->done)
        return false;

    xpcb_reset(batch);
    batch->ncols = st->ncols;

    /*
     * Everything this batch points at lives here, and the previous batch's
     * payloads die here.  Borrowed-until-next-call, as documented.
     */
    MemoryContextReset(st->batch_cxt);
    old = MemoryContextSwitchTo(st->batch_cxt);

    for (int c = 0; c < st->ncols; c++)
    {
        XpBatchColumn *col = &batch->cols[c];

        col->type = st->coltypes[c];
        col->data = palloc(batch->capacity * xpcb_type_width(col->type));
        col->validity = NULL;           /* allocated on the first NULL */
        col->owns_data = false;         /* the context owns it, not the batch */
        col->owns_validity = false;
    }

    while (nrows < batch->capacity &&
           (tup = heap_getnext(st->scan, ForwardScanDirection)) != NULL)
    {
        st->tuples_visited++;

        heap_deform_tuple(tup, td, st->dvalues, st->dnulls);
        st->tuples_deformed++;
        st->attrs_deformed += td->natts;

        /* Predicate on the first requested column */
        if (st->has_pred)
        {
            int     a0 = st->attnos[0] - 1;
            int32   key;

            if (st->dnulls[a0])
                continue;               /* NULL passes no range predicate */
            key = DatumGetInt32(st->dvalues[a0]);
            if (key < st->pred_lo || key > st->pred_hi)
                continue;
        }

        st->tuples_passed++;

        for (int c = 0; c < st->ncols; c++)
        {
            int a = st->attnos[c] - 1;

            xpb_heap_store_value(st, batch, c, nrows,
                                 st->dvalues[a], st->dnulls[a]);
        }
        nrows++;
    }

    MemoryContextSwitchTo(old);

    if (nrows == 0)
    {
        st->done = true;
        return false;
    }

    batch->nrows = nrows;
    return true;
}


/* ── Projected path ── */

/*
 * Walk a heap tuple by PostgreSQL's own rules, but materialize only the
 * attributes the batch asked for.
 *
 * This is a structural mirror of heap_deform_tuple() in
 * access/common/heaptuple.c, using the SAME inline helpers from
 * access/tupmacs.h -- fetch_att_noerr(), align_fetch_then_add(),
 * first_null_attr() -- and the same three-phase shape:
 *
 *   1. a prefix whose offsets are cached in the tuple descriptor
 *      (firstNonCachedOffsetAttr), where no alignment arithmetic is needed;
 *   2. a run with no NULLs, where each attribute is aligned and stepped over;
 *   3. a tail that may contain NULLs, where a NULL occupies no space.
 *
 * Nothing about alignment, short or external varlena headers, or NULL bitmap
 * interpretation is reinvented here; it is delegated to those helpers.  The
 * only thing this does differently from heap_deform_tuple is:
 *
 *   - it stops at the HIGHEST REQUESTED attnum instead of natts, and
 *   - it stores into the batch's typed columns instead of a Datum/isnull
 *     array of width natts, so unused attributes cost a step and nothing
 *     more.
 *
 * An unused attribute is still WALKED -- that is unavoidable, since a later
 * attribute's position depends on it -- but it is never materialized and, for
 * a varlena, never detoasted.  align_fetch_then_add() reads a varlena's
 * length header because that is what locating the next attribute requires;
 * it does not follow a TOAST pointer.
 */
static bool
xpb_heap_projected_next_batch(XpBatchSource *src, XpColumnBatch *batch)
{
    HeapBatchState *st = src->private_state;
    TupleDesc       td = RelationGetDescr(st->rel);
    MemoryContext   old;
    HeapTuple       tup;
    int             nrows = 0;

    if (st->done)
        return false;

    xpcb_reset(batch);
    batch->ncols = st->ncols;

    MemoryContextReset(st->batch_cxt);
    old = MemoryContextSwitchTo(st->batch_cxt);

    for (int c = 0; c < st->ncols; c++)
    {
        XpBatchColumn *col = &batch->cols[c];

        col->type = st->coltypes[c];
        col->data = palloc(batch->capacity * xpcb_type_width(col->type));
        col->validity = NULL;
        col->owns_data = false;
        col->owns_validity = false;
    }

    while (nrows < batch->capacity &&
           (tup = heap_getnext(st->scan, ForwardScanDirection)) != NULL)
    {
        HeapTupleHeader     tuphdr = tup->t_data;
        bool                hasnulls = HeapTupleHasNulls(tup);
        uint8              *bp = tuphdr->t_bits;
        const char         *tp = (const char *) tuphdr + tuphdr->t_hoff;
        CompactAttribute   *cattr = NULL;
        int                 tup_natts = HeapTupleHeaderGetNatts(tuphdr);
        int                 scan_natts;
        int                 first_null;
        int                 first_uncached;
        uint32              off = 0;
        int                 attnum = 0;
        bool                keep = true;
        Datum               d;

        st->tuples_visited++;
        st->tuples_scanned++;

        /*
         * Only attributes up to the highest one requested need visiting.
         * Everything past it cannot move anything we read.
         */
        scan_natts = Min(tup_natts, (int) st->max_attno);

        /*
         * A tuple shorter than the projection would need getmissingattr()
         * semantics for the remainder.  Refused rather than guessed: this is
         * an experimental decoder and a wrong answer here is silent
         * corruption.
         */
        if (tup_natts < (int) st->max_attno)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("xp_batch: projected path met a tuple with %d attributes, %d required",
                            tup_natts, st->max_attno),
                     errdetail("A tuple written before a column was added needs missing-value semantics, which this experimental path does not implement.")));

        first_null = hasnulls ? first_null_attr(bp, scan_natts) : scan_natts;
        first_uncached = Min(td->firstNonCachedOffsetAttr, scan_natts);
        if (hasnulls)
            first_uncached = Min(first_uncached, first_null);

        /* phase 1: cached offsets, no alignment arithmetic */
        for (; attnum < first_uncached; attnum++)
        {
            cattr = TupleDescCompactAttr(td, attnum);
            st->attributes_walked++;

            if (st->want[attnum] >= 0)
            {
                d = fetch_att_noerr(tp + cattr->attcacheoff,
                                    cattr->attbyval, cattr->attlen);
                xpb_heap_store_value(st, batch, st->want[attnum], nrows, d, false);
                st->attributes_materialized++;
            }
        }
        if (first_uncached > 0)
            off = cattr->attcacheoff + cattr->attlen;

        /* phase 2: no NULLs in this run */
        for (; attnum < first_null; attnum++)
        {
            cattr = TupleDescCompactAttr(td, attnum);
            st->attributes_walked++;

            /*
             * Called for unused attributes too: stepping over a varlena
             * requires reading its length header, which is exactly what this
             * does and no more.  The Datum it returns is discarded.
             */
            d = align_fetch_then_add(tp, &off, cattr->attbyval, cattr->attlen,
                                     cattr->attalignby);
            if (st->want[attnum] >= 0)
            {
                xpb_heap_store_value(st, batch, st->want[attnum], nrows, d, false);
                st->attributes_materialized++;
            }
        }

        /* phase 3: NULLs possible; a NULL occupies no space */
        for (; attnum < scan_natts; attnum++)
        {
            st->attributes_walked++;

            if (att_isnull(attnum, bp))
            {
                if (st->want[attnum] >= 0)
                {
                    xpb_heap_store_value(st, batch, st->want[attnum], nrows,
                                         (Datum) 0, true);
                    st->attributes_materialized++;
                }
                continue;
            }

            cattr = TupleDescCompactAttr(td, attnum);
            d = align_fetch_then_add(tp, &off, cattr->attbyval, cattr->attlen,
                                     cattr->attalignby);
            if (st->want[attnum] >= 0)
            {
                xpb_heap_store_value(st, batch, st->want[attnum], nrows, d, false);
                st->attributes_materialized++;
            }
        }

        /* predicate on the first requested column, after it is in the batch */
        if (st->has_pred)
        {
            int32 key = ((const int32 *) batch->cols[0].data)[nrows];

            if (xpcb_isnull(batch, 0, nrows) ||
                key < st->pred_lo || key > st->pred_hi)
                keep = false;
        }

        if (keep)
        {
            st->tuples_passed++;
            nrows++;
        }
        else
        {
            /*
             * The row is dropped, so any validity bit written for it must be
             * reset -- the next row reuses this slot and a stale clear bit
             * would make a present value look NULL.
             */
            for (int c = 0; c < st->ncols; c++)
                if (batch->cols[c].validity)
                    xpcb_set_valid(&batch->cols[c], nrows);
        }
    }

    MemoryContextSwitchTo(old);

    if (nrows == 0)
    {
        st->done = true;
        return false;
    }

    batch->nrows = nrows;
    return true;
}

/* ── Fixed path ── */

static bool
xpb_heap_next_batch(XpBatchSource *src, XpColumnBatch *batch)
{
    HeapBatchState *st = src->private_state;

    if (st->path == XPB_HEAP_DEFORM)
        return xpb_heap_deform_next_batch(src, batch);
    if (st->path == XPB_HEAP_PROJECTED)
        return xpb_heap_projected_next_batch(src, batch);

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
            xpcb_col_alloc(&batch->cols[c], st->coltypes[c], batch->capacity, false);
    }

    /* Type dispatch happens here, once per batch -- never in the row loop. */
    void *out[XPCB_MAX_COLS];

    for (int c = 0; c < st->ncols; c++)
        out[c] = (st->coltypes[c] == XPB_COL_INT8)
            ? (void *) xpcb_i64(batch, c)
            : (void *) xpcb_i32(batch, c);

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

            /* Extract all columns.  One predictable branch per column on a
             * width fixed at setup; no per-value dispatch. */
            for (int c = 0; c < st->ncols; c++)
            {
                const char *p = d + st->coldefs[c].offset;

                if (st->coldefs[c].width == 8)
                    ((int64 *) out[c])[nrows] = *(const int64 *) p;
                else
                    ((int32 *) out[c])[nrows] = *(const int32 *) p;
            }

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
    st->tuples_deformed = 0;
    st->attrs_deformed = 0;

    if (st->path != XPB_HEAP_FIXED)
    {
        /* Ends the borrow window for anything the last batch handed out. */
        table_rescan(st->scan, NULL);
        MemoryContextReset(st->batch_cxt);
    }
    st->tuples_scanned = 0;
    st->attributes_walked = 0;
    st->attributes_materialized = 0;
}

static void
xpb_heap_end(XpBatchSource *src)
{
    HeapBatchState *st = src->private_state;

    if (st->path != XPB_HEAP_FIXED)
    {
        if (st->scan)
            table_endscan(st->scan);
        st->scan = NULL;
        if (st->batch_cxt)
            MemoryContextDelete(st->batch_cxt);
        st->batch_cxt = NULL;
    }
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
 * Fixed-path constructor.  Unchanged contract for existing callers: the
 * benchmark pipelines ask for this path by name and must keep getting the
 * error, not a quiet downgrade, on a layout it cannot address.
 */
XpBatchSource *
xpb_heap_source_create(Oid relid, int16 *requested_attnos, int ncols,
                        bool has_pred, int32 pred_lo, int32 pred_hi)
{
    return xpb_heap_source_create_ex(relid, requested_attnos, ncols,
                                     XPB_HEAP_FIXED, has_pred,
                                     pred_lo, pred_hi);
}

XpBatchSource *
xpb_heap_source_create_ex(Oid relid, int16 *requested_attnos, int ncols,
                          XpbHeapPath path, bool has_pred,
                          int32 pred_lo, int32 pred_hi)
{
    HeapBatchState *st = palloc0(sizeof(HeapBatchState));
    st->rel = table_open(relid, AccessShareLock);
    st->snap = GetActiveSnapshot();
    st->nblocks = RelationGetNumberOfBlocks(st->rel);
    st->rel_oid = RelationGetRelid(st->rel);
    st->vmbuf = InvalidBuffer;
    st->ncols = ncols;
    st->cur_offset = FirstOffsetNumber;
    st->path = path;

    if (ncols < 1 || ncols > XPCB_MAX_COLS)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("HeapBatchSource: %d columns requested (1..%d)",
                        ncols, XPCB_MAX_COLS)));

    /* Compute byte offsets from TupleDesc */
    TupleDesc td = RelationGetDescr(st->rel);

    /*
     * Derive the batch column types from the relation, so a batch always
     * describes what it actually holds rather than what a caller hoped for.
     */
    for (int i = 0; i < ncols; i++)
    {
        int16               attno = requested_attnos[i];
        Form_pg_attribute   att;

        if (attno < 1 || attno > td->natts)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("HeapBatchSource: attno %d out of range (1..%d) for \"%s\"",
                            attno, td->natts, RelationGetRelationName(st->rel))));

        att = TupleDescAttr(td, attno - 1);
        if (att->attisdropped)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("HeapBatchSource: \"%s\" attnum %d is a dropped column",
                            RelationGetRelationName(st->rel), attno)));

        st->attrs_requested = ncols;
        st->attnos[i] = attno;
        st->coltypes[i] = xpb_heap_coltype_for(att->atttypid);
        if (st->coltypes[i] == XPB_COL_UNSET)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("HeapBatchSource: \"%s\".%s has type %u, which the batch contract does not carry",
                            RelationGetRelationName(st->rel),
                            NameStr(att->attname), att->atttypid),
                     errdetail("Supported: int4, int8, numeric, and text/varchar/bpchar/bytea as varlena.")));
    }

    /*
     * Projection map for the projected path: attnum-1 -> batch column, or -1
     * for an attribute that must be walked but not materialized.  Built once
     * here, never per tuple.
     */
    memset(st->want, -1, sizeof(st->want));
    st->max_attno = 0;
    for (int i = 0; i < ncols; i++)
    {
        if (requested_attnos[i] > MaxTupleAttributeNumber)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("xp_batch: attno %d exceeds MaxTupleAttributeNumber",
                            requested_attnos[i])));
        if (st->want[requested_attnos[i] - 1] >= 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("xp_batch: attno %d requested twice; the projected path maps each attribute to one batch column",
                            requested_attnos[i])));
        st->want[requested_attnos[i] - 1] = (signed char) i;
        if (requested_attnos[i] > st->max_attno)
            st->max_attno = requested_attnos[i];
    }

    if (has_pred && st->coltypes[0] != XPB_COL_INT4)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("HeapBatchSource: range predicate needs an int4 first column, got %s",
                        xpcb_type_name(st->coltypes[0]))));

    if (path != XPB_HEAP_FIXED)
    {
        st->batch_cxt = AllocSetContextCreate(CurrentMemoryContext,
                                              "xpb heap deform batch",
                                              ALLOCSET_DEFAULT_SIZES);
        st->dvalues = palloc(td->natts * sizeof(Datum));
        st->dnulls = palloc(td->natts * sizeof(bool));
        st->scan = table_beginscan(st->rel, st->snap, 0, NULL, 0);

        st->has_pred = has_pred;
        st->pred_lo = pred_lo;
        st->pred_hi = pred_hi;

        XpBatchSource *dsrc = palloc(sizeof(XpBatchSource));
        dsrc->ops = &heap_batch_ops;
        dsrc->private_state = st;
        return dsrc;
    }

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
     *
     * The eligibility rule lives in xpb_heap_layout_supports_fixed() so that
     * the guard here and the answer a caller gets when it asks which path to
     * use cannot drift apart.
     */
    {
        char *why = NULL;

        if (!xpb_heap_layout_supports_fixed(st->rel, requested_attnos, ncols,
                                            &why))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("HeapBatchSource: \"%s\" cannot use the fixed-offset path: %s",
                            RelationGetRelationName(st->rel), why),
                     errdetail("Fixed-offset access requires every requested column to be int4 and every attribute up to the highest one read to be fixed-width and NOT NULL."),
                     errhint("Use XPB_HEAP_DEFORM for this layout.")));
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
        st->coldefs[i].width = attr->attlen;
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

/*
 * Deform accounting.  tuples_deformed is zero on the fixed-offset path by
 * construction -- it never calls heap_deform_tuple -- so a caller can prove
 * from the outside which mechanism actually ran, rather than trusting the
 * flag it passed in.
 */
void
xpb_heap_source_deform_stats(XpBatchSource *src, int64 *tuples_deformed,
                             int64 *attrs_deformed, int *attrs_requested,
                             bool *is_deform_path)
{
    HeapBatchState *st = src->private_state;

    *tuples_deformed = st->tuples_deformed;
    *attrs_deformed = st->attrs_deformed;
    *attrs_requested = st->attrs_requested;
    *is_deform_path = (st->path == XPB_HEAP_DEFORM);
}

void
xpb_heap_source_projected_stats(XpBatchSource *src, int64 *tuples_scanned,
                                int64 *attributes_walked,
                                int64 *attributes_materialized)
{
    HeapBatchState *st = src->private_state;

    *tuples_scanned = st->tuples_scanned;
    *attributes_walked = st->attributes_walked;
    *attributes_materialized = st->attributes_materialized;
}
