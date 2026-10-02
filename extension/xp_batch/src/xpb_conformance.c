/*
 * xpb_conformance.c -- one contract test, any provider.
 *
 * Every source so far got its own ad-hoc test: the heap source has
 * contract_tests.sh, ZLFS has zlfs_v01.sql, Parquet got benchmarks/08 and three
 * scripts of its own. The checks overlap, none of them is reusable, and the
 * next source would need a fourth set.
 *
 * This drives a provider through the lifecycle the contract defines and reports
 * one row per check, including the checks it SKIPS and why. It tests the
 * contract, not a format: it never names a source, and everything it knows it
 * learns from the registry, the request it made, and the batches it got back.
 *
 * What it cannot do here, and where that is covered instead:
 *
 *   cancel / statement_timeout   needs a scan long enough to interrupt and a
 *                                second session to do it; the shell wrapper
 *                                does that, and parquet_error_paths.sh measures
 *                                it (10 ms timeout fired in 26 ms against
 *                                389 ms before the interrupt check existed).
 *   ERROR cleanup                needs a failure injected mid-scan; covered by
 *                                provider_negative.sh over ten failure modes,
 *                                and by the descriptor count in
 *                                parquet_error_paths.sh.
 *   type mismatch refusal        depends on the data a provider is pointed at;
 *                                provider_negative.sh does it with a file built
 *                                for the purpose.
 *
 * Those are reported as skipped with that reason rather than silently absent,
 * because a conformance report that hides what it did not check is worth less
 * than no report.
 */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "xpb_colbatch.h"
#include "xpb_source.h"

PG_FUNCTION_INFO_V1(xpb_source_conformance);

typedef struct ConfState
{
    Tuplestorestate    *store;
    TupleDesc           desc;
    int                 npass;
    int                 nfail;
    int                 nskip;
} ConfState;

static void
conf_row(ConfState *cs, const char *check, const char *status, const char *detail)
{
    Datum   vals[3];
    bool    nulls[3] = { false, false, false };

    vals[0] = CStringGetTextDatum(check);
    vals[1] = CStringGetTextDatum(status);
    vals[2] = CStringGetTextDatum(detail ? detail : "");
    tuplestore_putvalues(cs->store, cs->desc, vals, nulls);

    if (strcmp(status, "pass") == 0)       cs->npass++;
    else if (strcmp(status, "skip") == 0)  cs->nskip++;
    else                                   cs->nfail++;
}

static void
conf_check(ConfState *cs, const char *check, bool ok, const char *detail)
{
    conf_row(cs, check, ok ? "pass" : "FAIL", detail);
}

/*
 * Drain a source, counting rows and batches and noting what the columns looked
 * like. Returns the row count; fills the out-params with what the first batch
 * declared, which is what the projection check compares against the request.
 */
static int64
conf_drain(XpBatchSource *src, int ncols, int *nbatches_out,
           XpbColType *types_out, bool *saw_validity_out, int *max_cols_out)
{
    XpColumnBatch   batch;
    int64           rows = 0;
    int             nbatches = 0;

    *saw_validity_out = false;
    *max_cols_out = 0;

    memset(&batch, 0, sizeof(batch));
    batch.ncols = ncols;
    batch.capacity = XPCB_BATCH_CAP;

    while (true)
    {
        CHECK_FOR_INTERRUPTS();

        batch.nrows = 0;
        if (!src->ops->next_batch(src, &batch))
            break;

        if (nbatches == 0)
            for (int c = 0; c < batch.ncols && c < XPCB_MAX_COLS; c++)
                types_out[c] = batch.cols[c].type;

        if (batch.ncols > *max_cols_out)
            *max_cols_out = batch.ncols;

        for (int c = 0; c < batch.ncols && c < XPCB_MAX_COLS; c++)
            if (batch.cols[c].validity != NULL)
                *saw_validity_out = true;

        rows += batch.nrows;
        nbatches++;
    }

    *nbatches_out = nbatches;
    return rows;
}

/*
 * xpb_source_conformance(provider, relname, uri, attnos, colnames, lo, hi)
 */
Datum
xpb_source_conformance(PG_FUNCTION_ARGS)
{
    ReturnSetInfo              *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    ConfState                   cs;
    char                       *pname;
    const XpbSourceProvider    *prov;
    XpbSourceRequest            req;
    XpBatchSource              *src;
    int16                       attnos[XPCB_MAX_COLS];
    const char                 *colnames[XPCB_MAX_COLS];
    XpbColType                  types[XPCB_MAX_COLS];
    XpbColType                  types2[XPCB_MAX_COLS];
    int                         ncols = 0;
    int                         nbatches, maxcols;
    bool                        saw_validity;
    int64                       rows, rows2;
    char                        buf[256];
    MemoryContext               oldcxt;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("xpb_source_conformance: set-valued context required")));

    pname = text_to_cstring(PG_GETARG_TEXT_PP(0));

    oldcxt = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
    cs.desc = CreateTemplateTupleDesc(3);
    TupleDescInitEntry(cs.desc, 1, "check",  TEXTOID, -1, 0);
    TupleDescInitEntry(cs.desc, 2, "status", TEXTOID, -1, 0);
    TupleDescInitEntry(cs.desc, 3, "detail", TEXTOID, -1, 0);
    cs.desc = BlessTupleDesc(cs.desc);
    cs.store = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult  = cs.store;
    rsinfo->setDesc    = cs.desc;
    MemoryContextSwitchTo(oldcxt);
    cs.npass = cs.nfail = cs.nskip = 0;

    /* ---- the request ---- */
    memset(&req, 0, sizeof(req));
    req.relid = InvalidOid;

    if (!PG_ARGISNULL(1))
    {
        req.relid = RelnameGetRelid(text_to_cstring(PG_GETARG_TEXT_PP(1)));
        if (!OidIsValid(req.relid))
            ereport(ERROR, (errmsg("xpb_source_conformance: relation not found")));
    }
    if (!PG_ARGISNULL(2))
        req.uri = text_to_cstring(PG_GETARG_TEXT_PP(2));

    if (!PG_ARGISNULL(3))
    {
        ArrayType  *a = PG_GETARG_ARRAYTYPE_P(3);
        Datum      *el;
        bool       *nul;
        int         n;

        deconstruct_array(a, INT4OID, 4, true, TYPALIGN_INT, &el, &nul, &n);
        if (n < 1 || n > XPCB_MAX_COLS)
            ereport(ERROR, (errmsg("xpb_source_conformance: 1..%d attnos", XPCB_MAX_COLS)));
        for (int i = 0; i < n; i++)
            attnos[i] = (int16) DatumGetInt32(el[i]);
        req.attnos = attnos;
        ncols = n;
    }
    if (!PG_ARGISNULL(4))
    {
        ArrayType  *a = PG_GETARG_ARRAYTYPE_P(4);
        Datum      *el;
        bool       *nul;
        int         n;

        deconstruct_array(a, TEXTOID, -1, false, TYPALIGN_INT, &el, &nul, &n);
        if (n < 1 || n > XPCB_MAX_COLS)
            ereport(ERROR, (errmsg("xpb_source_conformance: 1..%d colnames", XPCB_MAX_COLS)));
        for (int i = 0; i < n; i++)
            colnames[i] = TextDatumGetCString(el[i]);
        req.colnames = colnames;
        ncols = n;
    }
    req.ncols = ncols;

    if (!PG_ARGISNULL(5) && !PG_ARGISNULL(6))
    {
        req.has_pred = true;
        req.pred_lo  = PG_GETARG_INT32(5);
        req.pred_hi  = PG_GETARG_INT32(6);
    }

    /* ---- the provider ---- */
    prov = xpb_find_source_provider(pname);
    if (prov == NULL)
    {
        conf_check(&cs, "provider registered", false, "not in the registry");
        return (Datum) 0;
    }
    snprintf(buf, sizeof(buf), "filters_rows=%s describe=%s",
             prov->filters_rows ? "true" : "false",
             prov->describe ? "yes" : "no");
    conf_check(&cs, "provider registered", true, buf);

    /* ---- create ---- */
    src = prov->create(&req);
    conf_check(&cs, "create", src != NULL && src->ops != NULL,
               "a source with an ops table");
    if (src == NULL || src->ops == NULL)
        return (Datum) 0;

    conf_check(&cs, "ops complete", src->ops->next_batch && src->ops->end,
               src->ops->rescan ? "next_batch, rescan, end" : "next_batch, end (no rescan)");

    snprintf(buf, sizeof(buf), "supports_rescan=%s",
             src->caps.supports_rescan ? "true" : "false");
    conf_row(&cs, "capabilities declared", "pass", buf);

    /* ---- one batch / multiple batches ---- */
    for (int c = 0; c < XPCB_MAX_COLS; c++)
        types[c] = XPB_COL_UNSET;
    rows = conf_drain(src, ncols, &nbatches, types, &saw_validity, &maxcols);

    snprintf(buf, sizeof(buf), INT64_FORMAT " rows in %d batches", rows, nbatches);
    conf_check(&cs, "next_batch yields rows", rows > 0 && nbatches > 0, buf);
    conf_check(&cs, "exhaustion returns false", true,
               "the drain loop ended without an error");

    if (nbatches > 1)
        conf_check(&cs, "multiple batches", true, buf);
    else
        conf_row(&cs, "multiple batches", "skip",
                 "this source returned everything in one batch for this request");

    /* ---- projection ---- */
    snprintf(buf, sizeof(buf), "requested %d, delivered %d", ncols, maxcols);
    conf_check(&cs, "projection width", maxcols == ncols, buf);

    {
        bool    typed = true;
        char   *p = buf;
        size_t  left = sizeof(buf);

        for (int c = 0; c < ncols; c++)
        {
            int n = snprintf(p, left, "%s%s", c ? "," : "", xpcb_type_name(types[c]));
            if (n > 0 && (size_t) n < left) { p += n; left -= n; }
            if (types[c] == XPB_COL_UNSET)
                typed = false;
        }
        conf_check(&cs, "every column typed", typed, buf);
    }

    conf_row(&cs, "validity bitmap", "pass",
             saw_validity ? "observed: at least one column carried NULLs"
                          : "none in this data; the contract allows it either way");

    /* ---- rescan, only if declared ---- */
    if (src->caps.supports_rescan)
    {
        int     nbatches2;
        bool    saw_validity2;
        int     maxcols2;

        xpcb_source_rescan(src);
        rows2 = conf_drain(src, ncols, &nbatches2, types2, &saw_validity2, &maxcols2);

        snprintf(buf, sizeof(buf), "first " INT64_FORMAT ", after rescan " INT64_FORMAT,
                 rows, rows2);
        conf_check(&cs, "rescan returns the same rows", rows == rows2, buf);
    }
    else
        conf_row(&cs, "rescan returns the same rows", "skip",
                 "supports_rescan is false, so the caller may not rewind it");

    /* ---- end, and end again ---- */
    src->ops->end(src);
    src->ops->end(src);
    conf_check(&cs, "end is idempotent", true,
               "called twice; a second release must not double-free");

    /* ---- what this harness cannot do here ---- */
    conf_row(&cs, "cancel / statement_timeout", "skip",
             "needs a second session; the shell wrapper and parquet_error_paths.sh do it");
    conf_row(&cs, "ERROR cleanup", "skip",
             "needs an injected failure; provider_negative.sh covers ten modes");
    conf_row(&cs, "type mismatch refusal", "skip",
             "depends on the data; provider_negative.sh uses a file built for it");

    snprintf(buf, sizeof(buf), "%d pass, %d fail, %d skip", cs.npass, cs.nfail, cs.nskip);
    conf_row(&cs, "TOTAL", cs.nfail == 0 ? "pass" : "FAIL", buf);

    return (Datum) 0;
}
