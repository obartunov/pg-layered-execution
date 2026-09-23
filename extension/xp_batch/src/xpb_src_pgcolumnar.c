/*
 * xpb_src_pgcolumnar.c — XpcnBatchSource: serves pgcolumnar row groups as
 *                        compact int32 batches through the fold reader API.
 *
 * Built against commandprompt/pgcolumnar at 5b20ae8 (VERSION 1.0-alpha5) with
 * patches/pgcolumnar-alpha5/0001-export-fold-reader-api.patch applied: the six
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

/* ── source state ── */

typedef struct XpcnBatchState
{
    Relation    rel;
    Snapshot    snap;
    PgColumnarReadState *rs;

    int         ncols;
    int         attidx[XPCB_MAX_COLS];      /* 0-based attribute index */

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
    const int32 *bor[XPCB_MAX_COLS];        /* borrowed dense streams */
    int32      *own[XPCB_MAX_COLS];         /* materialized streams */
    int64       own_cap;                    /* rows each own[] can hold */

    /* accounting, so a borrow can be told from a copy in the report */
    int64       groups_total;
    int64       groups_copied;
    int64       rows_total;
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
    st->cursor = 0;

    for (c = 0; c < st->ncols; c++)
    {
        if (!PgColumnarReadFoldColumn(st->rs, st->attidx[c], &validity[c],
                                      &packed[c], &attlen, &vecRawLen))
            ereport(ERROR,
                    (errmsg("xp_batch: column %d is absent from this row group",
                            st->attidx[c] + 1),
                     errdetail("A row group written before the column was added "
                               "has no stream for it.")));
        if (attlen != sizeof(int32))
            ereport(ERROR,
                    (errmsg("xp_batch: column %d has attlen %d, 4 required",
                            st->attidx[c] + 1, attlen)));
        if (((uintptr_t) packed[c] & (sizeof(int32) - 1)) != 0)
            dense = false;      /* unaligned: copy rather than borrow */
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
        const int32 *k = (const int32 *) packed[0];

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
            st->bor[c] = (const int32 *) packed[c];
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
            st->own[c] = palloc(st->group_rows * sizeof(int32));
        }
        st->own_cap = st->group_rows;
    }

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
                    skip = true;
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
                    if (!skip)
                        ereport(ERROR,
                                (errmsg("xp_batch: NULL in column %d of a "
                                        "pgcolumnar row group",
                                        st->attidx[c] + 1),
                                 errdetail("The compact batch has no null "
                                           "representation.")));
                    continue;
                }
                if (!skip)
                    memcpy(&st->own[c][out],
                           packed[c] + present[c] * sizeof(int32),
                           sizeof(int32));
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
                int32   key = st->own[0][out];

                if (key < st->pred_lo || key > st->pred_hi)
                    skip = true;
            }

            if (!skip)
                out++;
        }

        st->group_rows = out;
        st->rows_total += out;
        st->bytes_copied += out * (int64) st->ncols * (int64) sizeof(int32);
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
     * Validity is NULL because this source does not yet carry NULLs into the
     * batch: xpcn_load_group() still raises an error when a surviving row has
     * a NULL in a requested column.  Once that is lifted the present bitmap
     * borrows straight into the column, since both use bit-set-means-valid.
     */
    for (c = 0; c < st->ncols; c++)
        xpcb_col_borrow(&batch->cols[c], XPB_COL_INT4,
                        st->borrowed ? (int32 *) (st->bor[c] + st->cursor)
                                     : st->own[c] + st->cursor,
                        NULL);

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

    PgColumnarEndRead(st->rs);
    table_close(st->rel, AccessShareLock);
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
        int attidx = requested_attnos[c] - 1;

        if (attidx < 0 || attidx >= RelationGetDescr(st->rel)->natts)
            ereport(ERROR, (errmsg("xp_batch: attno %d out of range (1..%d)",
                                   requested_attnos[c],
                                   RelationGetDescr(st->rel)->natts)));
        st->attidx[c] = attidx;
        proj = bms_add_member(proj, attidx);
    }

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
