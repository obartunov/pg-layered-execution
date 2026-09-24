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
 * xpb_contract_probe(relname text, attnos int[], path text)
 *   -> (rownum bigint, col int, coltype text, isnull bool, val text)
 *
 * path is 'fixed' or 'deform'.  Asking for 'fixed' on a layout that cannot
 * support it raises the guard's error -- which is the point of several tests.
 */
Datum
xpb_contract_probe(PG_FUNCTION_ARGS)
{
    text           *relname = PG_GETARG_TEXT_PP(0);
    ArrayType      *attarr = PG_GETARG_ARRAYTYPE_P(1);
    text           *pathname = PG_GETARG_TEXT_PP(2);
    ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *store;
    MemoryContext   oldcxt;
    XpbHeapPath     path;
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

    pstr = text_to_cstring(pathname);
    if (strcmp(pstr, "fixed") == 0)
        path = XPB_HEAP_FIXED;
    else if (strcmp(pstr, "deform") == 0)
        path = XPB_HEAP_DEFORM;
    else if (strcmp(pstr, "projected") == 0)
        path = XPB_HEAP_PROJECTED;
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_contract_probe: path must be 'fixed', 'deform' or 'projected', got \"%s\"",
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
    src = xpb_heap_source_create_ex(relid, attnos, ncols, path,
                                    false, 0, 0);
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
