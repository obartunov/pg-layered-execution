/*
 * xpb_contract_probe.c — read a relation through the batch contract and hand
 *                        back what the batch actually contained.
 *
 * This is a test harness for the contract itself, not part of any pipeline.
 * It exists so a SQL test can compare the batch layer's view of a table
 * against PostgreSQL's own view of the same table, per column, including
 * type and NULLability -- rather than inferring correctness from an
 * aggregate that happens to come out right.
 *
 * Everything here is deliberately slow and general: values are rendered to
 * text so the comparison can be written in SQL.  Nothing in this file is on
 * a measured path, and no operator calls into it.
 */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/tuplestore.h"
#include "utils/rel.h"

#include "xpb_colbatch.h"
#include "xpb_src_heap.h"

extern XpBatchSource *xpcn_source_create(Oid relid, int16 *requested_attnos,
                                        int ncols, bool has_pred,
                                        int32 pred_lo, int32 pred_hi);

PG_FUNCTION_INFO_V1(xpb_contract_probe);

/* Render one value of a batch column as text, or NULL. */
static char *
probe_render(const XpColumnBatch *b, int c, int r)
{
    const XpBatchColumn *col = &b->cols[c];

    if (xpcb_isnull(b, c, r))
        return NULL;

    switch (col->type)
    {
        case XPB_COL_INT4:
            return psprintf("%d", ((int32 *) col->data)[r]);
        case XPB_COL_INT8:
            return psprintf(INT64_FORMAT, ((int64 *) col->data)[r]);
        case XPB_COL_NUMERIC:
            return DatumGetCString(DirectFunctionCall1(numeric_out,
                                                       ((Datum *) col->data)[r]));
        case XPB_COL_VARLENA:
            return text_to_cstring(DatumGetTextPP(((Datum *) col->data)[r]));
        case XPB_COL_UNSET:
            break;
    }
    elog(ERROR, "xpb_contract_probe: column %d has no type", c);
    return NULL;                /* keep the compiler quiet */
}

/*
 * xpb_contract_probe(relname text, attnos int[], path text,
 *                    pred_lo int DEFAULT NULL, pred_hi int DEFAULT NULL)
 *   -> (rownum bigint, col int, coltype text, isnull bool, val text)
 *
 * path is 'fixed', 'deform', 'projected' or 'projected-early'.  Asking for
 * 'fixed' on a layout that cannot support it raises the guard's error -- which
 * is the point of several tests.
 *
 * The optional pred_lo/pred_hi pair applies a range predicate to the FIRST
 * requested column.  Because those two arguments must be allowed to be NULL,
 * the function cannot be STRICT, so the first three arguments are checked
 * here rather than by the executor.
 */
Datum
xpb_contract_probe(PG_FUNCTION_ARGS)
{
    text           *relname;
    ArrayType      *attarr;
    text           *pathname;
    ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *store;
    MemoryContext   oldcxt;
    XpbHeapPath     path = XPB_HEAP_DEFORM;
    bool            want_pgcn = false;
    XpBatchSource  *src;
    XpColumnBatch   batch;
    Oid             relid;
    int16           attnos[XPCB_MAX_COLS];
    int             ncols;
    int32          *attdata;
    char           *pstr;
    int64           rownum = 0;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        (rsi->allowedModes & SFRM_Materialize) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("xpb_contract_probe: set-valued context required")));

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("xpb_contract_probe: relname, attnos and path must not be NULL")));
    relname = PG_GETARG_TEXT_PP(0);
    attarr = PG_GETARG_ARRAYTYPE_P(1);
    pathname = PG_GETARG_TEXT_PP(2);

    /*
     * Half a range is not a predicate.  Accepting it silently would mean a
     * test that meant to exercise the early-predicate path quietly measured
     * the unfiltered one instead.
     */
    if (PG_ARGISNULL(3) != PG_ARGISNULL(4))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_contract_probe: pred_lo and pred_hi must both be given or both omitted")));

    pstr = text_to_cstring(pathname);
    /*
     * "pgcolumnar" is not a heap path, so it is dispatched separately below
     * rather than folded into the XpbHeapPath enum.  It is here because the
     * columnar source is the only one whose validity handling interacts with
     * the range predicate, and that interaction had no test.
     */
    if (strcmp(pstr, "pgcolumnar") == 0)
        want_pgcn = true;
    else if (strcmp(pstr, "fixed") == 0)
        path = XPB_HEAP_FIXED;
    else if (strcmp(pstr, "deform") == 0)
        path = XPB_HEAP_DEFORM;
    else if (strcmp(pstr, "projected") == 0)
        path = XPB_HEAP_PROJECTED;
    else if (strcmp(pstr, "projected-early") == 0)
        path = XPB_HEAP_PROJECTED_EARLY;
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_contract_probe: path must be 'fixed', 'deform', 'projected', 'projected-early' or 'pgcolumnar', got \"%s\"",
                        pstr)));

    relid = RelnameGetRelid(text_to_cstring(relname));
    if (!OidIsValid(relid))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("xpb_contract_probe: relation \"%s\" not found",
                        text_to_cstring(relname))));

    if (ARR_NDIM(attarr) != 1 || ARR_HASNULL(attarr) ||
        ARR_ELEMTYPE(attarr) != INT4OID)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_contract_probe: attnos must be a 1-D int[] without NULLs")));

    ncols = ArrayGetNItems(ARR_NDIM(attarr), ARR_DIMS(attarr));
    if (ncols < 1 || ncols > XPCB_MAX_COLS)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_contract_probe: %d columns requested (1..%d)",
                        ncols, XPCB_MAX_COLS)));
    attdata = (int32 *) ARR_DATA_PTR(attarr);
    for (int i = 0; i < ncols; i++)
        attnos[i] = (int16) attdata[i];

    /*
     * The tuple descriptor and the tuplestore outlive this call -- the
     * executor reads them after we return -- so both must be built in the
     * per-query context, not in the per-tuple context this function runs in,
     * which is reset underneath us.
     */
    oldcxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    tupdesc = CreateTemplateTupleDesc(5);
    TupleDescInitEntry(tupdesc, 1, "rownum", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 2, "col", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, 3, "coltype", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, 4, "is_null", BOOLOID, -1, 0);
    TupleDescInitEntry(tupdesc, 5, "val", TEXTOID, -1, 0);

    store = tuplestore_begin_heap(true, false, work_mem);
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = store;
    rsi->setDesc = BlessTupleDesc(tupdesc);
    MemoryContextSwitchTo(oldcxt);
    /*
     * Optional range predicate on the first requested column, so the
     * early-predicate path can be tested on any table rather than only
     * through the benchmark's fixed schema.
     */
    {
        bool    has_pred = !PG_ARGISNULL(3) && !PG_ARGISNULL(4);
        int32   lo = has_pred ? PG_GETARG_INT32(3) : 0;
        int32   hi = has_pred ? PG_GETARG_INT32(4) : 0;

        src = want_pgcn
            ? xpcn_source_create(relid, attnos, ncols, has_pred, lo, hi)
            : xpb_heap_source_create_ex(relid, attnos, ncols, path,
                                        has_pred, lo, hi);
    }
    memset(&batch, 0, sizeof(batch));
    batch.capacity = 1024;      /* small on purpose: exercise batch boundaries */
    batch.ncols = ncols;

    while (src->ops->next_batch(src, &batch))
    {
        for (int r = 0; r < batch.nrows; r++)
        {
            CHECK_FOR_INTERRUPTS();

            for (int c = 0; c < ncols; c++)
            {
                Datum   vals[5];
                bool    nulls[5] = {false, false, false, false, false};
                char   *rendered = probe_render(&batch, c, r);

                vals[0] = Int64GetDatum(rownum);
                vals[1] = Int32GetDatum(c);
                vals[2] = CStringGetTextDatum(xpcb_type_name(batch.cols[c].type));
                vals[3] = BoolGetDatum(rendered == NULL);
                if (rendered == NULL)
                    nulls[4] = true;
                else
                    vals[4] = CStringGetTextDatum(rendered);
                tuplestore_putvalues(store, rsi->setDesc, vals, nulls);
            }
            rownum++;
        }
        /* The probe never retains a borrowed pointer past this point. */
        xpcb_release_owned(&batch);
    }

    src->ops->end(src);
    return (Datum) 0;
}
