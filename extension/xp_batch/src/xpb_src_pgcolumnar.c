/*
 * xpb_src_pgcolumnar.c — XpcnBatchSource: serves pgcolumnar row groups as
 *                        compact int32 batches through the fold reader API.
 *
 * Built against commandprompt/pgcolumnar at 5b20ae8 (VERSION 1.0-alpha5) with
 * patches/pgcolumnar/0001-export-fold-reader-api.patch applied: the six
 * symbols below are local in a stock build (PostgreSQL compiles extension
 * modules with -fvisibility=hidden) and cannot be linked against otherwise.
 *
 * Layout the fold API hands back, per row group:
 *
 *   validity[col]  bitmap over rows-in-group; bit r set = value present
 *   packed[col]    DENSE stream of the present values only, so row r's value
 *                  sits at packed + (present values before r) * attlen -- it
 *                  is not indexable by r, and a column with any NULL has to be
 *                  walked in row order
 *   deleteMask     bit r set = row deleted; the value slot still exists
 *   skipVec/vecStart  vectors ruled out by a skip predicate; vecStart is
 *                  cumulative row spans with a [vcount] terminator
 *   decodeSkipped  true if loading the group skipped DECODING any vector
 *
 * decodeSkipped is the contract that matters here, and it is live in alpha5:
 * pgcolumnar_native_decode_chunk() is handed rs->nativeSkipVec and reports how
 * many vectors it actually decoded, so a scan with keys DOES leave vectors
 * undecoded. (The comment on the field in columnar_reader.c still says it is
 * "False always, today"; it is not, and this source ran into it on its first
 * pruning scan.) The rule is pgcolumnar's own, from its fold consumer in
 * columnar_vector.c (#512): honour the per-vector map, or refuse. The dense
 * stream reserves each vector's span whether or not it was decoded, so the
 * slots of a skipped vector must be COUNTED and never READ -- reading them
 * returns whatever the decode buffer held.
 *
 * Scope, matching the other two sources: int4 columns, no varlena, no outer
 * join, single-threaded.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "utils/fmgroids.h"
#include "access/tableam.h"
#include "nodes/bitmapset.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "xpb_colbatch.h"
#include "access/table.h"

/* ── pgcolumnar fold reader API (see the export patch) ── */

typedef struct PgColumnarReadState PgColumnarReadState;

extern uint64 PgColumnarStorageId(Relation rel);
extern PgColumnarReadState *PgColumnarBeginRead(Relation rel, Snapshot snapshot,
                                                ParallelTableScanDesc parallelScan,
                                                Bitmapset *projectedColumns,
                                                int nkeys, ScanKey keys);
extern void PgColumnarEndRead(PgColumnarReadState *readState);
extern bool PgColumnarReadFoldNextGroup(PgColumnarReadState *readState);
extern void PgColumnarReadFoldGroupInfo(PgColumnarReadState *readState, uint64 *nrows,
                                        const char **deleteMask, uint32 *deleteMaskLen,
                                        const bool **skipVec, bool *decodeSkipped,
                                        const uint32 **vecStart, int *vectorCount);
extern bool PgColumnarReadFoldColumn(PgColumnarReadState *readState, int attidx,
                                     const char **validity, const char **packed,
                                     int16 *attlen, const uint32 **vecRawLen);

/* Defined here, used from the pipelines; declared so the definitions are
 * checked against a prototype. */
extern XpBatchSource *xpcn_source_create(Oid relid, int16 *requested_attnos,
                                         int ncols, bool has_pred,
                                         int32 pred_lo, int32 pred_hi);
extern void xpcn_source_stats(XpBatchSource *src, int64 *groups,
                              int64 *groups_copied, int64 *rows,
                              int64 *bytes_copied);
extern void xpcn_source_pruning(XpBatchSource *src, int64 *groups_read,
                                int64 *group_rows_seen,
                                int64 *rows_vec_skipped, int64 *rows_emitted);

/* ── source state ── */

typedef struct XpcnBatchState
{
    Relation    rel;
    Snapshot    snap;
    PgColumnarReadState *rs;

    int         ncols;
    int         attidx[XPCB_MAX_COLS];      /* 0-based attribute index */
    XpbColType  coltype[XPCB_MAX_COLS];     /* int4 or int8 */
    int16       colwidth[XPCB_MAX_COLS];    /* 4 or 8 bytes                */

    /*
     * Range predicate on the FIRST requested column, the same contract the
     * heap source takes.  It is also handed to the reader as scan keys, so
     * pgcolumnar can drop whole row groups on its zone maps -- but that
     * pruning is approximate by construction (a surviving group still holds
     * non-matching rows), so the per-row recheck below is not optional.
     */
    bool        has_pred;
    int32       pred_lo;
    int32       pred_hi;

    /* current row group */
    int64       group_rows;                 /* rows in the loaded group */
    int64       cursor;                     /* next row of the group to emit */
    bool        borrowed;                   /* group served without a copy */
    const void *bor[XPCB_MAX_COLS];         /* borrowed dense streams */
    void       *own[XPCB_MAX_COLS];         /* materialized streams */
    uint8      *ownvalid[XPCB_MAX_COLS];    /* validity, when the group has NULLs */
    bool        has_nulls[XPCB_MAX_COLS];   /* this group, this column */
    int64       own_cap;                    /* rows each own[] can hold */

    /*
     * Accounting.  Only groups the fold reader HANDED US are counted: a group
     * pgcolumnar eliminated on its zone maps never reaches this source, so
     * groups_total is "row groups read", not "row groups in the relation".
     * The total comes from pgcolumnar.row_group, and the difference is what
     * pruning skipped.  Nothing here is derived from timings.
     */
    int64       groups_total;               /* row groups READ              */
    int64       groups_copied;              /* of those, materialized       */
    int64       group_rows_seen;            /* rows represented by them     */
    int64       rows_vec_skipped;           /* rows in ruled-out vectors    */
    int64       rows_total;                 /* rows emitted into batches    */
    int64       bytes_copied;
} XpcnBatchState;

/*
 * Load the next row group and decide how this source will serve it.
 *
 * Returns false at end of scan.  A group is BORROWED when every requested
 * column has a value for every row (no NULLs), nothing in it is deleted, no
 * vector was ruled out, and the dense stream is 4-byte aligned: then the dense
 * stream IS the column array and the batch can point straight at it.  That is
 * the case the benchmark table produces, and the case worth measuring.
 * Anything else is materialized by walking the group once.
 */
static bool
xpcn_load_group(XpcnBatchState *st)
{
    uint64          nrows;
    const char     *dmask;
    uint32          dlen;
    const bool     *skipVec;
    bool            decodeSkipped;
    const uint32   *vecStart;
    int             vcount;
    const char     *validity[XPCB_MAX_COLS];
    const char     *packed[XPCB_MAX_COLS];
    const uint32   *vecRawLen;
    int16           attlen;
    bool            dense = true;
    int             c;
    int64           r;

    if (!PgColumnarReadFoldNextGroup(st->rs))
        return false;

    PgColumnarReadFoldGroupInfo(st->rs, &nrows, &dmask, &dlen,
                                &skipVec, &decodeSkipped, &vecStart, &vcount);

    if (decodeSkipped && (skipVec == NULL || vecStart == NULL || vcount <= 0))
        ereport(ERROR,
                (errmsg("xp_batch: pgcolumnar skipped decoding vectors of a row "
                        "group and gave no per-vector map"),
                 errdetail("Without skipVec/vecStart this source cannot tell a "
                           "decoded vector from one left undecoded, and the "
                           "dense stream reserves space for both.")));

    st->groups_total++;
    st->group_rows = (int64) nrows;
    st->group_rows_seen += (int64) nrows;
    st->cursor = 0;
    for (c = 0; c < st->ncols; c++)
        st->has_nulls[c] = false;

    for (c = 0; c < st->ncols; c++)
    {
        if (!PgColumnarReadFoldColumn(st->rs, st->attidx[c], &validity[c],
                                      &packed[c], &attlen, &vecRawLen))
            ereport(ERROR,
                    (errmsg("xp_batch: column %d is absent from this row group",
                            st->attidx[c] + 1),
                     errdetail("A row group written before the column was added "
                               "has no stream for it.")));
        if (attlen != st->colwidth[c])
            ereport(ERROR,
                    (errmsg("xp_batch: column %d has attlen %d, %d expected from its type",
                            st->attidx[c] + 1, attlen, st->colwidth[c])));
        if (((uintptr_t) packed[c] & (uintptr_t) (st->colwidth[c] - 1)) != 0)
            dense = false;      /* unaligned for its width: copy, not borrow */
    }

    if (dmask != NULL)
        dense = false;
    if (skipVec != NULL && vecStart != NULL && vcount > 0)
        for (c = 0; c < vcount; c++)
            if (skipVec[c])
            {
                dense = false;
                break;
            }

    /* all-present check: one pass over the validity bitmaps */
    if (dense)
    {
        int64   whole = st->group_rows / 8;
        int     tail = (int) (st->group_rows % 8);

        for (c = 0; c < st->ncols && dense; c++)
        {
            for (r = 0; r < whole; r++)
                if ((unsigned char) validity[c][r] != 0xFF)
                {
                    dense = false;
                    break;
                }
            if (dense && tail > 0 &&
                ((unsigned char) validity[c][whole] & ((1 << tail) - 1))
                != (unsigned char) ((1 << tail) - 1))
                dense = false;
        }
    }

    /*
     * With a predicate, a group can only be borrowed if every row in it
     * passes.  One pass over the key column decides that, which is an order of
     * magnitude cheaper than the copy it avoids; a group straddling the
     * boundary takes the copy path and is filtered there.
     */
    if (dense && st->has_pred)
    {
        const int32 *k = (const int32 *) packed[0];    /* int4 by construction */

        for (r = 0; r < st->group_rows; r++)
            if (k[r] < st->pred_lo || k[r] > st->pred_hi)
            {
                dense = false;
                break;
            }
    }

    if (dense)
    {
        st->borrowed = true;
        for (c = 0; c < st->ncols; c++)
            st->bor[c] = (const void *) packed[c];
        st->rows_total += st->group_rows;
        return true;
    }

    /*
     * Materialize.  Deleted rows and rows in a ruled-out vector are dropped,
     * so the emitted group is shorter than nrows; a NULL cannot be represented
     * in an int32 batch and is refused rather than silently turned into a
     * value.  Per-column present counters advance in row order because the
     * stream is dense -- that is the whole reason this walk exists.
     */
    st->borrowed = false;
    if (st->own_cap < st->group_rows)
    {
        for (c = 0; c < st->ncols; c++)
        {
            if (st->own[c])
                pfree(st->own[c]);
            st->own[c] = palloc(st->group_rows * st->colwidth[c]);
            if (st->ownvalid[c])
                pfree(st->ownvalid[c]);
            st->ownvalid[c] = palloc(XPCB_VALIDITY_BYTES(st->group_rows));
        }
        st->own_cap = st->group_rows;
    }
    /* start all-valid; the walk clears a bit when the group says NULL */
    for (c = 0; c < st->ncols; c++)
        memset(st->ownvalid[c], 0xFF, XPCB_VALIDITY_BYTES(st->group_rows));

    {
        int64   present[XPCB_MAX_COLS];
        int64   out = 0;
        int     curVec = 0;

        memset(present, 0, sizeof(present));

        for (r = 0; r < st->group_rows; r++)
        {
            bool    skip = false;

            if (skipVec != NULL && vecStart != NULL && vcount > 0)
            {
                while (curVec < vcount && r >= (int64) vecStart[curVec + 1])
                    curVec++;
                if (curVec < vcount && skipVec[curVec])
                {
                    skip = true;
                    st->rows_vec_skipped++;
                }
            }
            if (!skip && dmask != NULL && (uint32) (r >> 3) < dlen &&
                (dmask[r >> 3] & (1 << (r & 7))) != 0)
                skip = true;

            for (c = 0; c < st->ncols; c++)
            {
                bool present_here =
                    ((validity[c][r >> 3] >> (r & 7)) & 1) != 0;

                if (!present_here)
                {
                    /*
                     * A NULL occupies no slot in the dense stream, so
                     * present[c] must NOT advance here -- that is what keeps
                     * the following values aligned.  The batch carries the
                     * NULL in its validity bitmap; there is no sentinel.
                     */
                    if (!skip)
                    {
                        st->ownvalid[c][out >> 3] &= ~(1 << (out & 7));
                        st->has_nulls[c] = true;
                    }
                    continue;
                }
                if (!skip)
                    memcpy((char *) st->own[c] + out * st->colwidth[c],
                           packed[c] + present[c] * (size_t) st->colwidth[c],
                           st->colwidth[c]);
                present[c]++;
            }

            /*
             * Recheck the predicate on the row itself. Group- and vector-level
             * pruning is approximate -- a surviving vector still holds rows
             * outside the range -- so without this the source emits them and
             * the aggregate quietly grows extra groups. The counters above
             * have already advanced, which is why the check sits here and not
             * before the column loop.
             */
            if (!skip && st->has_pred)
            {
                int32   key = ((const int32 *) st->own[0])[out];

                /* A NULL key passes no range predicate.  The predicate column
                 * is int4 NOT NULL by construction, so this is a guard, not a
                 * path the benchmark exercises. */
                if (((st->ownvalid[0][out >> 3] >> (out & 7)) & 1) == 0 ||
                    key < st->pred_lo || key > st->pred_hi)
                    skip = true;
            }

            if (skip)
            {
                /*
                 * The row is dropped, but the column loop above has already
                 * written into slot `out` -- including clearing validity bits
                 * for any NULL it carried.  `out` does not advance, so the
                 * next accepted row reuses this slot, and a stale clear bit
                 * would make its present value look NULL.  The bitmap is only
                 * ever cleared here, never set (it starts as 0xFF per group),
                 * so the drop has to restore it.
                 *
                 * xpb_src_heap.c does the same for the projected path; this
                 * source was missing it because its predicate recheck sits
                 * after the column loop rather than before it.
                 */
                for (c = 0; c < st->ncols; c++)
                    st->ownvalid[c][out >> 3] |= (1 << (out & 7));
            }
            else
                out++;
        }

        st->group_rows = out;
        st->rows_total += out;
        for (c = 0; c < st->ncols; c++)
            st->bytes_copied += out * (int64) st->colwidth[c];
        st->groups_copied++;
    }

    return true;
}

static bool
xpcn_next_batch(XpBatchSource *src, XpColumnBatch *batch)
{
    XpcnBatchState *st = src->private_state;
    int64           remaining;
    int             chunk;
    int             c;

    while (st->cursor >= st->group_rows)
        if (!xpcn_load_group(st))
            return false;

    remaining = st->group_rows - st->cursor;
    chunk = (remaining > batch->capacity) ? batch->capacity : (int) remaining;

    batch->nrows = chunk;
    batch->ncols = st->ncols;
    batch->selection = NULL;
    batch->nselected = 0;

    /*
     * Every column is borrowed: either straight from pgcolumnar's decoded
     * stream (st->borrowed, the all-present zero-copy case) or from this
     * source's own per-group buffer.  Both live until the next group is
     * loaded, which is exactly the contract's borrow window.
     *
     * Validity is exposed only for a column that actually saw a NULL in this
     * group: pgcolumnar's present bitmap and the batch contract both use
     * bit-set-means-valid, so the group's own bitmap is handed over directly
     * rather than restated.  A column with no NULLs passes validity = NULL,
     * which the contract reads as all-present.
     */
    for (c = 0; c < st->ncols; c++)
    {
        void   *data;
        uint8  *valid = NULL;

        if (st->borrowed)
        {
            /* all-present by definition of the borrow test */
            data = (char *) st->bor[c] + st->cursor * st->colwidth[c];
        }
        else
        {
            data = (char *) st->own[c] + st->cursor * st->colwidth[c];
            if (st->has_nulls[c])
            {
                /*
                 * The group's bitmap starts at row 0 of the group, and this
                 * batch starts at st->cursor.  The chunking below advances
                 * the cursor by batch->capacity, a multiple of 8, so the
                 * byte offset is exact; a non-multiple would need the bits
                 * restated, as the ZLFS source does.
                 */
                /*
                 * The exposed bitmap is a byte-aligned slice of the group's
                 * own, so the cursor must sit on a byte boundary. An Assert
                 * alone left a non-assert build shifting the validity window
                 * by up to 7 bits -- present values read as NULL and NULLs as
                 * present. The ZLFS source restates the bits instead; this one
                 * requires the alignment, so it says so.
                 */
                if ((st->cursor & 7) != 0)
                    ereport(ERROR,
                            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                             errmsg("pgcolumnar source: batch capacity must be a multiple of 8 to carry NULLs"),
                             errdetail("Cursor %ld is not byte-aligned.", (long) st->cursor)));
                valid = st->ownvalid[c] + (st->cursor >> 3);
            }
        }
        xpcb_col_borrow(&batch->cols[c], st->coltype[c], data, valid);
    }

    st->cursor += chunk;
    return true;
}

static void
xpcn_rescan(XpBatchSource *src)
{
    XpcnBatchState *st = src->private_state;

    /*
     * The fold API has no rescan entry point, and reopening here would have to
     * rebuild the projection and the scan keys from state this source does not
     * keep.  Nothing in the pipeline rescans a source today; refuse rather
     * than reopen a scan that silently differs from the first one.
     */
    (void) st;
    ereport(ERROR,
            (errmsg("xp_batch: the pgcolumnar batch source does not support rescan")));
}

static void
xpcn_end(XpBatchSource *src)
{
    XpcnBatchState *st = src->private_state;

    /* Idempotent, for the reason given in xpb_heap_end(). */
    if (st->rs != NULL)
    {
        PgColumnarEndRead(st->rs);
        st->rs = NULL;
    }
    if (st->rel != NULL)
    {
        table_close(st->rel, AccessShareLock);
        st->rel = NULL;
    }
}

static const XpBatchSourceOps xpcn_batch_ops = {
    .next_batch = xpcn_next_batch,
    .rescan     = xpcn_rescan,
    .end        = xpcn_end,
};

/*
 * Create a batch source over a pgcolumnar relation.
 *
 * requested_attnos are 1-based attribute numbers, as for the heap and ZLFS
 * sources; the projection handed to the reader is a Bitmapset of 0-based
 * indexes, which is pgcolumnar's own convention.
 */
XpBatchSource *
xpcn_source_create(Oid relid, int16 *requested_attnos, int ncols,
                   bool has_pred, int32 pred_lo, int32 pred_hi)
{
    XpcnBatchState *st = palloc0(sizeof(XpcnBatchState));
    Bitmapset      *proj = NULL;
    XpBatchSource  *src;
    ScanKeyData     keys[2];
    int             nkeys = 0;
    int             c;

    if (ncols < 1 || ncols > XPCB_MAX_COLS)
        ereport(ERROR, (errmsg("xp_batch: %d columns requested, 1..%d supported",
                               ncols, XPCB_MAX_COLS)));

    st->rel = relation_open(relid, AccessShareLock);
    st->snap = GetActiveSnapshot();
    st->ncols = ncols;

    for (c = 0; c < ncols; c++)
    {
        int                 attidx = requested_attnos[c] - 1;
        Form_pg_attribute   att;

        if (attidx < 0 || attidx >= RelationGetDescr(st->rel)->natts)
            ereport(ERROR, (errmsg("xp_batch: attno %d out of range (1..%d)",
                                   requested_attnos[c],
                                   RelationGetDescr(st->rel)->natts)));
        att = TupleDescAttr(RelationGetDescr(st->rel), attidx);
        if (att->attisdropped)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("xp_batch: \"%s\" attnum %d is a dropped column",
                            RelationGetRelationName(st->rel), attidx + 1)));

        /*
         * Fixed-width integers only.  A numeric or varlena column reaches
         * this source as a variable-width dense stream with its own offset
         * table, which nothing here knows how to walk -- so it is refused by
         * name rather than read as though it were fixed width.
         */
        switch (att->atttypid)
        {
            case INT4OID:
                st->coltype[c] = XPB_COL_INT4;
                st->colwidth[c] = sizeof(int32);
                break;
            case INT8OID:
                st->coltype[c] = XPB_COL_INT8;
                st->colwidth[c] = sizeof(int64);
                break;
            default:
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("xp_batch: pgcolumnar source cannot carry \"%s\".%s (type %u)",
                                RelationGetRelationName(st->rel),
                                NameStr(att->attname), att->atttypid),
                         errdetail("This source carries int4 and int8.")));
        }

        st->attidx[c] = attidx;
        proj = bms_add_member(proj, attidx);
    }

    /*
     * The range predicate is applied as an int4 comparison, both in the scan
     * keys below and in the per-row recheck, so the first requested column
     * has to be int4.
     */
    if (has_pred && st->coltype[0] != XPB_COL_INT4)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("xp_batch: pgcolumnar range predicate needs an int4 first column, got %s",
                        xpcb_type_name(st->coltype[0]))));

    st->has_pred = has_pred;
    st->pred_lo = pred_lo;
    st->pred_hi = pred_hi;

    if (has_pred)
    {
        /*
         * Hand the range to the reader as well, so zone maps can drop whole
         * row groups before they are decoded -- that pruning is the thing a
         * columnar source is supposed to be good at, and leaving it out would
         * understate it. The keys address the FIRST requested column by its
         * attno, which is what pgcolumnar's skip evaluator expects.
         */
        ScanKeyInit(&keys[nkeys++], requested_attnos[0],
                    BTGreaterEqualStrategyNumber, F_INT4GE,
                    Int32GetDatum(pred_lo));
        ScanKeyInit(&keys[nkeys++], requested_attnos[0],
                    BTLessEqualStrategyNumber, F_INT4LE,
                    Int32GetDatum(pred_hi));
    }

    st->rs = PgColumnarBeginRead(st->rel, st->snap, NULL, proj,
                                 nkeys, nkeys ? keys : NULL);

    src = palloc0(sizeof(XpBatchSource));
    src->ops = &xpcn_batch_ops;
    /*
     * FALSE, and this is the capability's first real user: xpcn_rescan() raises
     * an error -- the fold API has no rescan entry point and reopening would
     * rebuild the projection from state this source does not keep. Declaring
     * true here (the first guess, corrected by the conformance harness) made a
     * caller believe it could rewind a source that cannot.
     */
    src->caps.supports_rescan = false;
    src->private_state = st;
    return src;
}

/* Reporting hook: how much of the scan was borrowed rather than copied. */
void
xpcn_source_stats(XpBatchSource *src, int64 *groups, int64 *groups_copied,
                  int64 *rows, int64 *bytes_copied)
{
    XpcnBatchState *st = src->private_state;

    *groups = st->groups_total;
    *groups_copied = st->groups_copied;
    *rows = st->rows_total;
    *bytes_copied = st->bytes_copied;
}

/*
 * Pruning counters, for benchmark 05-A.
 *
 * groups_read is row groups the fold reader HANDED US.  A group pgcolumnar
 * eliminated on its zone maps never arrives, so this is not the number of
 * groups in the relation -- that comes from pgcolumnar.row_group, and the
 * difference is what pruning skipped.  group_rows_seen is the rows those
 * groups represent, which is what actually had to be decoded; rows_emitted is
 * what survived the predicate.  Nothing here is inferred from a timing.
 */
void
xpcn_source_pruning(XpBatchSource *src, int64 *groups_read,
                    int64 *group_rows_seen, int64 *rows_vec_skipped,
                    int64 *rows_emitted)
{
    XpcnBatchState *st = src->private_state;

    *groups_read = st->groups_total;
    *group_rows_seen = st->group_rows_seen;
    *rows_vec_skipped = st->rows_vec_skipped;
    *rows_emitted = st->rows_total;
}
