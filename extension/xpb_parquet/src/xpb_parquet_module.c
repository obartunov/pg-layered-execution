/*
 * xpb_parquet_module.c — the Parquet source provider.
 *
 * Pure C. Includes xpb_parquet_shim.h, never an Arrow header: PostgreSQL and
 * Arrow headers never meet in one translation unit.
 *
 * The whole file is a translation between two contracts that already existed:
 *
 *   XpqColumn   (shim)   type, data, validity, nrows        -- owned by reader
 *   XpBatchColumn        type, data, validity, owns_data    -- borrowed here
 *
 * The translation is mechanical because both sides already agree on the two
 * things that usually do not survive a source boundary: dense typed arrays,
 * and a validity bitmap whose SET BIT MEANS VALID. Arrow's convention and the
 * batch contract's convention are the same, so nothing is inverted and nothing
 * is copied for NULL handling.
 *
 * One batch == one Parquet row group. That is not a simplification to be
 * removed later: a row group is the unit the format gives statistics for and
 * the unit pruning works on, and the contract's borrow window is exactly "until
 * the next next_batch()", which is exactly how long the shim holds a row group.
 * The two windows coincide by construction rather than by care.
 */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "common/int.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

#include "xpb_colbatch.h"
#include "xpb_source.h"
#include "xpb_parquet_shim.h"

PG_MODULE_MAGIC;

#define XPQ_ERRBUF  512

typedef struct XpqSourceState
{
    XpqReader  *reader;
    char       *path;

    /* Projection: file column indices, in batch column order. */
    int         cols[XPCB_MAX_COLS];
    XpbColType  types[XPCB_MAX_COLS];
    int         ncols;

    /* Row-group cursor. rg_wanted[i] is false for a pruned row group. */
    int         nrow_groups;
    int         next_rg;
    bool       *rg_wanted;

    /* Predicate, as narrow as the contract is today: one closed range on the
     * leading projected column. Applied by the OPERATORS, not here -- this
     * source does no filtering, exactly like the heap and zlfs sources do none
     * beyond their own range. It is kept only to drive row-group pruning. */
    bool        has_pred;
    int64       pred_lo;
    int64       pred_hi;

    /* Observability. */
    int         rg_pruned;
    int         rg_read;
} XpqSourceState;

/* ────────────────────────────────────────────────────────── next_batch ── */

static bool
xpq_next_batch(XpBatchSource *src, XpColumnBatch *batch)
{
    XpqSourceState *st = (XpqSourceState *) src->private_state;
    XpqColumn       cols[XPCB_MAX_COLS];
    char            errbuf[XPQ_ERRBUF];
    int             rg;

    /* Skip row groups the metadata excluded. */
    while (st->next_rg < st->nrow_groups && !st->rg_wanted[st->next_rg])
    {
        st->rg_pruned++;
        st->next_rg++;
    }
    if (st->next_rg >= st->nrow_groups)
        return false;

    rg = st->next_rg++;

    errbuf[0] = '\0';
    if (xpq_read_row_group(st->reader, rg, st->cols, st->ncols,
                           cols, errbuf, sizeof(errbuf)) != 0)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("xpb_parquet: reading row group %d of \"%s\" failed", rg, st->path),
                 errdetail("%s", errbuf[0] ? errbuf : "no detail")));

    st->rg_read++;

    /*
     * A row group may hold more rows than one batch can carry. Refuse rather
     * than silently truncate: a short batch here would be a silent row drop,
     * which is the defect class this project spent two phases removing.
     */
    if (cols[0].nrows > batch->capacity)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("xpb_parquet: row group %d has " INT64_FORMAT " rows, batch capacity is %d",
                        rg, (int64) cols[0].nrows, batch->capacity),
                 errhint("Write the file with a smaller row_group_size, or raise XPCB_BATCH_CAP.")));

    batch->nrows = (int) cols[0].nrows;
    batch->ncols = st->ncols;
    batch->selection = NULL;
    batch->nselected = 0;

    for (int c = 0; c < st->ncols; c++)
    {
        if (cols[c].nrows != cols[0].nrows)
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("xpb_parquet: column %d of row group %d has " INT64_FORMAT
                            " rows, column 0 has " INT64_FORMAT,
                            c, rg, (int64) cols[c].nrows, (int64) cols[0].nrows)));

        /*
         * BORROWED, both the values and the validity bitmap. The shim holds
         * the row group alive until the next xpq_read_row_group(), which is
         * the next call to this function -- the same window the contract gives
         * a borrowed column. Nothing is copied here for any column, including
         * the ones the shim itself had to concatenate: that copy happened
         * inside the shim and is counted there.
         */
        xpcb_col_borrow(&batch->cols[c], st->types[c],
                        (void *) cols[c].data, (uint8 *) cols[c].validity);
    }

    return true;
}

static void
xpq_rescan(XpBatchSource *src)
{
    XpqSourceState *st = (XpqSourceState *) src->private_state;

    st->next_rg = 0;
    st->rg_pruned = 0;
    st->rg_read = 0;
}

static void
xpq_end(XpBatchSource *src)
{
    XpqSourceState *st = (XpqSourceState *) src->private_state;

    if (st->reader)
    {
        xpq_close(st->reader);
        st->reader = NULL;
    }
}

static const XpBatchSourceOps xpq_ops = {
    .next_batch = xpq_next_batch,
    .rescan     = xpq_rescan,
    .end        = xpq_end,
};

/* ─────────────────────────────────────────────────────────── create ── */

/*
 * Map the shim's physical type to a batch column type. Only the two integer
 * widths the batch contract has. Anything else refuses here rather than being
 * coerced, because a coercion invented at the boundary is exactly the kind of
 * reinterpretation the typed contract exists to prevent.
 */
static XpbColType
xpq_map_type(XpqColType t, const char *colname)
{
    switch (t)
    {
        case XPQ_COL_INT32: return XPB_COL_INT4;
        case XPQ_COL_INT64: return XPB_COL_INT8;
        case XPQ_COL_UNSUPPORTED:
            break;
    }
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("xpb_parquet: column \"%s\" has a physical type the batch contract has no type for",
                    colname ? colname : "?"),
             errdetail("The contract carries int4, int8, numeric and varlena; "
                       "this milestone maps only the two integer widths.")));
    return XPB_COL_UNSET;       /* unreachable */
}

static XpBatchSource *
xpq_create(const XpbSourceRequest *req)
{
    XpqSourceState *st;
    XpBatchSource  *src;
    char            errbuf[XPQ_ERRBUF];

    if (req->uri == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_parquet: a file path is required (XpbSourceRequest.uri)")));
    if (req->colnames == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_parquet: column names are required (XpbSourceRequest.colnames)"),
                 errdetail("Parquet columns are identified by name, not by attno.")));
    if (req->ncols <= 0 || req->ncols > XPCB_MAX_COLS)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_parquet: projection must name 1..%d columns, got %d",
                        XPCB_MAX_COLS, req->ncols)));

    st = (XpqSourceState *) palloc0(sizeof(XpqSourceState));
    st->path = pstrdup(req->uri);

    errbuf[0] = '\0';
    st->reader = xpq_open(st->path, errbuf, sizeof(errbuf));
    if (st->reader == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FILE),
                 errmsg("xpb_parquet: cannot open \"%s\"", st->path),
                 errdetail("%s", errbuf[0] ? errbuf : "no detail")));

    /* Resolve the projection by name, and refuse an unknown column rather than
     * silently producing fewer columns than asked for. */
    st->ncols = req->ncols;
    for (int c = 0; c < req->ncols; c++)
    {
        const char *name = req->colnames[c];
        int         idx  = xpq_column_index(st->reader, name);

        if (idx < 0)
        {
            xpq_close(st->reader);
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_COLUMN),
                     errmsg("xpb_parquet: \"%s\" has no column named \"%s\"",
                            st->path, name ? name : "?")));
        }
        /*
         * A repeated column is deduplicated by the reader, which would leave
         * the batch with fewer columns than the caller is about to read. The
         * shim catches it too; refusing here gives the better message and
         * keeps the failure at the boundary where the mistake was made.
         */
        for (int p = 0; p < c; p++)
            if (st->cols[p] == idx)
            {
                xpq_close(st->reader);
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("xpb_parquet: column \"%s\" named twice in the projection",
                                name),
                         errdetail("Each batch column must be a distinct file column.")));
            }

        st->cols[c]  = idx;
        st->types[c] = xpq_map_type(xpq_column_type(st->reader, idx), name);
    }

    st->nrow_groups = xpq_num_row_groups(st->reader);
    st->next_rg     = 0;
    st->has_pred    = req->has_pred;
    st->pred_lo     = req->pred_lo;
    st->pred_hi     = req->pred_hi;

    /*
     * Every row group is wanted at this milestone. Pruning is commit 0004 and
     * will set these from the footer's declared statistics only.
     */
    st->rg_wanted = (bool *) palloc(sizeof(bool) * Max(st->nrow_groups, 1));
    for (int i = 0; i < st->nrow_groups; i++)
        st->rg_wanted[i] = true;

    src = (XpBatchSource *) palloc0(sizeof(XpBatchSource));
    src->ops = &xpq_ops;
    src->private_state = st;
    return src;
}

static bool
xpq_describe(const XpbSourceRequest *req, XpbColType *types, int *ncols_out)
{
    XpqReader  *r;
    char        errbuf[XPQ_ERRBUF];

    if (req->uri == NULL || req->colnames == NULL || req->ncols <= 0)
        return false;

    errbuf[0] = '\0';
    r = xpq_open(req->uri, errbuf, sizeof(errbuf));
    if (r == NULL)
        return false;

    for (int c = 0; c < req->ncols; c++)
    {
        int idx = xpq_column_index(r, req->colnames[c]);

        if (idx < 0)
        {
            xpq_close(r);
            return false;
        }
        switch (xpq_column_type(r, idx))
        {
            case XPQ_COL_INT32: types[c] = XPB_COL_INT4; break;
            case XPQ_COL_INT64: types[c] = XPB_COL_INT8; break;
            default:
                xpq_close(r);
                return false;
        }
    }
    *ncols_out = req->ncols;
    xpq_close(r);
    return true;
}

static const XpbSourceProvider parquet_provider = {
    .name     = "parquet",
    .create   = xpq_create,
    .describe = xpq_describe,
};

void
_PG_init(void)
{
    xpb_register_source_provider(&parquet_provider);
}

/* ──────────────────────────────────────────────── SQL probe for 0002 ── */

/*
 * xpq_scan(path, cols, lo, hi) -> one row of totals.
 *
 * Deliberately not a planner integration. It drives the provider through the
 * generic contract and reports what the file gave up, so the questions this
 * milestone has to answer -- does the boundary hold, what was decoded, what was
 * copied -- are answerable before any planner plumbing exists.
 *
 * It aggregates in this file ONLY because the generic operators live in
 * xp_batch and are reached through the planner, not through a function call.
 * Nothing here is a Parquet-specific operator: it is a test harness that reads
 * the batch contract exactly as an operator would.
 */
PG_FUNCTION_INFO_V1(xpq_scan);

Datum
xpq_scan(PG_FUNCTION_ARGS)
{
    char           *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char           *colspec = text_to_cstring(PG_GETARG_TEXT_PP(1));
    bool            has_pred = !PG_ARGISNULL(2) && !PG_ARGISNULL(3);
    int64           lo = has_pred ? PG_GETARG_INT64(2) : 0;
    int64           hi = has_pred ? PG_GETARG_INT64(3) : 0;

    ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *store;
    MemoryContext   oldcxt;

    const XpbSourceProvider *prov;
    XpbSourceRequest req;
    XpBatchSource   *src;
    XpColumnBatch    batch;

    const char     *names[XPCB_MAX_COLS];
    int             ncols = 0;
    char           *tok, *rest;

    int64           rows = 0, nbatches = 0, nulls_col0 = 0;
    int64           sum_last = 0;
    bool            sum_overflow = false;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("xpq_scan: set-valued context required")));

    prov = xpb_find_source_provider("parquet");
    if (prov == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("xpb_parquet: provider not registered"),
                 errhint("LOAD 'xpb_parquet' first.")));

    /* Split the comma-separated projection. */
    rest = colspec;
    while ((tok = strtok_r(rest, ",", &rest)) != NULL)
    {
        while (*tok == ' ') tok++;
        if (ncols >= XPCB_MAX_COLS)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("xpq_scan: at most %d columns", XPCB_MAX_COLS)));
        names[ncols++] = tok;
    }

    memset(&req, 0, sizeof(req));
    req.uri      = path;
    req.colnames = names;
    req.ncols    = ncols;
    req.has_pred = has_pred;
    req.pred_lo  = lo;
    req.pred_hi  = hi;

    src = prov->create(&req);

    memset(&batch, 0, sizeof(batch));
    batch.capacity = XPCB_BATCH_CAP;
    batch.ncols    = ncols;

    /*
     * The consumer loop. This is what an operator does: hoist the typed
     * pointer once per column per batch, then a tight row loop. No Parquet
     * concept appears below this line.
     */
    while (src->ops->next_batch(src, &batch))
    {
        int      n = batch.nrows;
        int      last = batch.ncols - 1;
        int32   *c0_i32 = NULL;
        int64   *c0_i64 = NULL;
        int32   *cl_i32 = NULL;
        int64   *cl_i64 = NULL;

        nbatches++;
        rows += n;

        if (batch.cols[0].type == XPB_COL_INT4) c0_i32 = xpcb_i32(&batch, 0);
        else                                    c0_i64 = xpcb_i64(&batch, 0);
        if (batch.cols[last].type == XPB_COL_INT4) cl_i32 = xpcb_i32(&batch, last);
        else                                       cl_i64 = xpcb_i64(&batch, last);

        for (int rr = 0; rr < n; rr++)
        {
            if (xpcb_isnull(&batch, 0, rr))
                nulls_col0++;

            /* Sum the LAST projected column, skipping NULLs -- SQL sum()
             * semantics, so the harness can compare against PostgreSQL. */
            if (!xpcb_isnull(&batch, last, rr))
            {
                int64 v = cl_i32 ? (int64) cl_i32[rr] : cl_i64[rr];

                if (pg_add_s64_overflow(sum_last, v, &sum_last))
                {
                    sum_overflow = true;
                    sum_last = 0;
                }
            }
        }
        (void) c0_i32; (void) c0_i64;
        CHECK_FOR_INTERRUPTS();
    }

    oldcxt = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
    tupdesc = CreateTemplateTupleDesc(11);
    TupleDescInitEntry(tupdesc,  1, "rows",            INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc,  2, "batches",         INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc,  3, "sum_last_col",    INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc,  4, "nulls_first_col", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc,  5, "row_groups_total", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc,  6, "row_groups_read", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc,  7, "row_groups_pruned", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc,  8, "decoded_values",  INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc,  9, "bytes_read",      INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 10, "copy_bytes",      INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 11, "arrow_chunks",    INT8OID, -1, 0);
    tupdesc = BlessTupleDesc(tupdesc);

    store = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult  = store;
    rsinfo->setDesc    = tupdesc;

    {
        XpqSourceState *st = (XpqSourceState *) src->private_state;
        Datum   vals[11];
        bool    nulls[11] = {false, false, false, false, false,
                             false, false, false, false, false, false};

        vals[0] = Int64GetDatum(rows);
        vals[1] = Int64GetDatum(nbatches);
        if (sum_overflow) nulls[2] = true;
        else              vals[2] = Int64GetDatum(sum_last);
        vals[3] = Int64GetDatum(nulls_col0);
        vals[4] = Int32GetDatum(st->nrow_groups);
        vals[5] = Int32GetDatum(st->rg_read);
        vals[6] = Int32GetDatum(st->rg_pruned);
        vals[7] = Int64GetDatum(xpq_decoded_values(st->reader));
        vals[8] = Int64GetDatum(xpq_bytes_read(st->reader));
        vals[9] = Int64GetDatum(xpq_copy_bytes(st->reader));
        vals[10] = Int64GetDatum(xpq_chunks_seen(st->reader));
        tuplestore_putvalues(store, tupdesc, vals, nulls);
    }

    MemoryContextSwitchTo(oldcxt);
    src->ops->end(src);
    return (Datum) 0;
}
