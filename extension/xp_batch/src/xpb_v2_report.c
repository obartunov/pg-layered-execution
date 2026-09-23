/*
 * xpb_v2_report.c — the Benchmark 05-A register report, on the typed contract
 *
 *   source(reg2 | reg2_col | ZLFS zone, period BETWEEN lo AND hi)
 *     -> join dim_company   (company_key int4 -> company_group)
 *     -> join dim_account2  (account_key int8 -> account_group)
 *     -> GROUP BY company_group, account_group, company_key
 *     -> SUM(debit_cents), SUM(credit_cents), and their difference
 *
 * Separate from xpb_1c_register_report(), which is what benchmarks 02 and 04
 * measure and which stays on the int4 fixed-offset fast path with its
 * checksums unmoved.  This one reads the v2 row shape: a varlena at attnum 2
 * ahead of everything it touches, so the heap arm runs on the GENERIC DEFORM
 * PATH.  That is deliberate and is recorded in the report line -- its source
 * timing is not comparable with benchmark 04's fixed-offset source, because
 * it is not the same mechanism.
 *
 * Only the period predicate changes between measurements.  The joins, the
 * grouping and the aggregate count are fixed, so that what moves across
 * selectivity points is the source and nothing else.
 *
 * WHY int8 MONEY AND NOT numeric
 * ------------------------------
 * Every batch source can carry an int8.  None can carry a numeric: the ZLFS
 * zone format holds int4 and int8, and the pgColumnar source refuses a
 * variable-width stream by name.  Aggregating the numeric columns would
 * therefore leave nothing to compare across paths, which is the whole point
 * of the exercise.  The numeric columns stay in the row -- they are what puts
 * the heap arm on the deform path -- and are measured separately on heap
 * alone.  int8 sums here are exact, so sum(debit) - sum(credit) equals
 * sum(debit - credit) group by group, which the gate checks.
 */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "access/table.h"
#include "utils/tuplestore.h"

#include "xpb_colbatch.h"
#include "xpb_src_heap.h"
#include "xpb_zlfs.h"

extern XpBatchSource *xpb_zlfs_source_create(ZlfsZone *zone, int16 *attnos, int ncols);
extern XpBatchSource *xpcn_source_create(Oid relid, int16 *requested_attnos,
                                         int ncols, bool has_pred,
                                         int32 pred_lo, int32 pred_hi);
extern void xpcn_source_pruning(XpBatchSource *src, int64 *groups_read,
                                int64 *group_rows_seen,
                                int64 *rows_vec_skipped, int64 *rows_emitted);

PG_FUNCTION_INFO_V1(xpb_v2_register_report);

#define V2_DIM1_CAP  256        /* 50 companies   */
#define V2_DIM2_CAP  1024       /* 200 accounts   */
#define V2_GRP_CAP   16384
#define V2_GRP_LOAD  (V2_GRP_CAP * 3 / 4)

/* batch column order; the predicate column must be first and int4 */
#define V2_C_PERIOD  0
#define V2_C_COMPANY 1
#define V2_C_ACCOUNT 2
#define V2_C_DEBIT   3
#define V2_C_CREDIT  4
#define V2_NCOLS     5

typedef struct V2Dim1 { bool occupied; int32 key; int32 payload; } V2Dim1;
typedef struct V2Dim2 { bool occupied; int64 key; int32 payload; } V2Dim2;

typedef struct V2Group
{
    bool    occupied;
    int32   company_group;
    int32   account_group;
    int32   company_key;
    int64   debit;
    int64   credit;
} V2Group;

/*
 * Dimensions are read through the typed heap source on the deform path, so
 * they inherit its guards rather than addressing tuples themselves.  A NULL
 * key is skipped: under SQL semantics it can never equal a fact key, so
 * storing it could only ever produce a wrong match.
 */
static void
v2_dim1_build(V2Dim1 *d, Oid relid)
{
    int16           attnos[2] = {1, 2};
    XpBatchSource  *src = xpb_heap_source_create_ex(relid, attnos, 2,
                                                    XPB_HEAP_DEFORM, false, 0, 0);
    XpColumnBatch   batch;

    memset(d, 0, V2_DIM1_CAP * sizeof(V2Dim1));
    memset(&batch, 0, sizeof(batch));
    batch.capacity = 1024;
    batch.ncols = 2;

    while (src->ops->next_batch(src, &batch))
    {
        int32 *k = xpcb_i32(&batch, 0);
        int32 *p = xpcb_i32(&batch, 1);

        for (int r = 0; r < batch.nrows; r++)
        {
            uint32 h;

            if (xpcb_isnull(&batch, 0, r) || xpcb_isnull(&batch, 1, r))
                continue;
            h = (uint32) k[r] * 2654435761u;
            for (int i = 0; i < V2_DIM1_CAP; i++)
            {
                int idx = (h + i) & (V2_DIM1_CAP - 1);

                if (!d[idx].occupied)
                {
                    d[idx].occupied = true;
                    d[idx].key = k[r];
                    d[idx].payload = p[r];
                    break;
                }
                if (d[idx].key == k[r])
                    break;
            }
        }
        xpcb_release_owned(&batch);
    }
    src->ops->end(src);
}

static void
v2_dim2_build(V2Dim2 *d, Oid relid)
{
    int16           attnos[2] = {1, 2};
    XpBatchSource  *src = xpb_heap_source_create_ex(relid, attnos, 2,
                                                    XPB_HEAP_DEFORM, false, 0, 0);
    XpColumnBatch   batch;

    memset(d, 0, V2_DIM2_CAP * sizeof(V2Dim2));
    memset(&batch, 0, sizeof(batch));
    batch.capacity = 1024;
    batch.ncols = 2;

    while (src->ops->next_batch(src, &batch))
    {
        int64 *k = xpcb_i64(&batch, 0);     /* account_key is int8 */
        int32 *p = xpcb_i32(&batch, 1);

        for (int r = 0; r < batch.nrows; r++)
        {
            uint32 h;

            if (xpcb_isnull(&batch, 0, r) || xpcb_isnull(&batch, 1, r))
                continue;
            h = (uint32) k[r] * 2654435761u;
            for (int i = 0; i < V2_DIM2_CAP; i++)
            {
                int idx = (h + i) & (V2_DIM2_CAP - 1);

                if (!d[idx].occupied)
                {
                    d[idx].occupied = true;
                    d[idx].key = k[r];
                    d[idx].payload = p[r];
                    break;
                }
                if (d[idx].key == k[r])
                    break;
            }
        }
        xpcb_release_owned(&batch);
    }
    src->ops->end(src);
}

static inline bool
v2_dim1_lookup(const V2Dim1 *d, int32 key, int32 *payload)
{
    uint32 h = (uint32) key * 2654435761u;

    for (int i = 0; i < V2_DIM1_CAP; i++)
    {
        int idx = (h + i) & (V2_DIM1_CAP - 1);

        if (!d[idx].occupied) return false;
        if (d[idx].key == key) { *payload = d[idx].payload; return true; }
    }
    return false;
}

static inline bool
v2_dim2_lookup(const V2Dim2 *d, int64 key, int32 *payload)
{
    uint32 h = (uint32) key * 2654435761u;

    for (int i = 0; i < V2_DIM2_CAP; i++)
    {
        int idx = (h + i) & (V2_DIM2_CAP - 1);

        if (!d[idx].occupied) return false;
        if (d[idx].key == key) { *payload = d[idx].payload; return true; }
    }
    return false;
}

Datum
xpb_v2_register_report(PG_FUNCTION_ARGS)
{
    int32           lo = PG_GETARG_INT32(0);
    int32           hi = PG_GETARG_INT32(1);
    char           *mode = text_to_cstring(PG_GETARG_TEXT_PP(2));
    ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *store;
    MemoryContext   oldcxt;
    Oid             fact_relid,
                    dim1_relid,
                    dim2_relid;
    /*
     * The batch's column ORDER is the same for every layout --
     * [period, company_key, account_key, debit_cents, credit_cents] -- so the
     * pipeline below is identical.  Only which attnums carry them differs.
     */
    int16           attnos_bad[V2_NCOLS] = {1, 3, 4, 8, 9};
    int16           attnos_fixed[V2_NCOLS] = {1, 2, 3, 4, 5};
    int16          *attnos;
    XpBatchSource  *src;
    XpColumnBatch   batch;
    V2Dim1         *dim1;
    V2Dim2         *dim2;
    V2Group        *ht;
    int32          *grp1_buf,
                   *grp2_buf;
    bool           *keep;
    int             ngroups = 0;
    int64           rows_in = 0;
    int             nbatches = 0;
    bool            is_pgcn = (strcmp(mode, "pgcolumnar") == 0);
    bool            is_heap = false;
    const char     *heap_path_used = "n/a";
    instr_time      t0, t1, tp, tn;
    double          build_ms = 0, open_ms = 0, source_ms = 0,
                    j1_ms = 0, j2_ms = 0, agg_ms = 0;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        (rsi->allowedModes & SFRM_Materialize) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("xpb_v2_register_report: set-valued context required")));

    /*
     * 05-B compares two physical layouts holding identical rows, so the fact
     * table is selectable.  reg2 remains the default, which is what 05-A
     * measured.
     */
    if (is_pgcn)
        fact_relid = RelnameGetRelid("reg2_col");
    else if (strncmp(mode, "bad", 3) == 0)
        fact_relid = RelnameGetRelid("reg2_bad");
    else if (strncmp(mode, "fixedlayout", 11) == 0)
        fact_relid = RelnameGetRelid("reg2_fixed");
    else
        fact_relid = RelnameGetRelid("reg2");

    attnos = (strncmp(mode, "fixedlayout", 11) == 0) ? attnos_fixed : attnos_bad;
    dim1_relid = RelnameGetRelid("dim_company");
    dim2_relid = RelnameGetRelid("dim_account2");
    if (!OidIsValid(fact_relid) || !OidIsValid(dim1_relid) || !OidIsValid(dim2_relid))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("xpb_v2_register_report: reg2/reg2_col, dim_company or dim_account2 not found")));

    oldcxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    tupdesc = CreateTemplateTupleDesc(7);
    TupleDescInitEntry(tupdesc, 1, "company_group", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, 2, "account_group", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, 3, "company_key", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, 4, "debit_turnover", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 5, "credit_turnover", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 6, "net_turnover", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 7, "total_ms", FLOAT8OID, -1, 0);
    store = tuplestore_begin_heap(true, false, work_mem);
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = store;
    rsi->setDesc = BlessTupleDesc(tupdesc);
    MemoryContextSwitchTo(oldcxt);

    INSTR_TIME_SET_CURRENT(t0);

    INSTR_TIME_SET_CURRENT(tp);
    dim1 = palloc0(V2_DIM1_CAP * sizeof(V2Dim1));
    dim2 = palloc0(V2_DIM2_CAP * sizeof(V2Dim2));
    v2_dim1_build(dim1, dim1_relid);
    v2_dim2_build(dim2, dim2_relid);
    ht = palloc0(V2_GRP_CAP * sizeof(V2Group));
    INSTR_TIME_SET_CURRENT(tn);
    build_ms = INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

    /*
     * Opening the source is timed on its own.  For ZLFS that includes the
     * registry scan, which reads and validates every zone FILE in the data
     * directory -- a cost that grows with zones belonging to other relations,
     * and one that would otherwise sit in total_ms with nothing to attribute
     * it to.  For pgColumnar it includes PgColumnarBeginRead, where zone-map
     * elimination happens.  Naming it keeps total = build + open + source +
     * operators, with nothing unaccounted.
     */
    INSTR_TIME_SET_CURRENT(tp);

    if (strcmp(mode, "zlfs") == 0)
    {
        ZlfsZone *zone = NULL;

        /*
         * The zone must cover EXACTLY the requested range.  A ZLFS zone is
         * materialized for one predicate range and the source applies no
         * predicate of its own, so taking "any valid zone for this relation"
         * silently answers a different question: with zones for 1..1 and
         * 1..12 both present, a request for 1..12 was served from the 1..1
         * zone and returned 83 334 rows instead of 1 000 008.
         */
        zlfs_ensure_registry();
        zlfs_scan_directory();
        if (zlfs_reg)
            for (int i = 0; i < zlfs_reg->nzones; i++)
                if (zlfs_reg->zones[i]->source_relid == fact_relid &&
                    zlfs_reg->zones[i]->pred_lo == lo &&
                    zlfs_reg->zones[i]->pred_hi == hi &&
                    zlfs_reg->zones[i]->freshness == ZLFS_VALID)
                { zone = zlfs_reg->zones[i]; break; }
        if (zone == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("xpb_v2_register_report: no VALID ZLFS zone for reg2 covering [%d..%d]",
                            lo, hi),
                     errhint("Build one with zlfs_build_zone('reg2','1,3,4,8,9',%d,%d).",
                             lo, hi)));
        src = xpb_zlfs_source_create(zone, attnos, V2_NCOLS);
    }
    else if (is_pgcn)
        src = xpcn_source_create(fact_relid, attnos, V2_NCOLS, true, lo, hi);
    else
    {
        /*
         * Three heap modes.
         *
         * The mode is "<table>" or "<table>-<path>":
         *
         *   table    heap        reg2         (05-A's table)
         *            bad         reg2_bad     varlena ahead of the projection
         *            fixedlayout reg2_fixed   projection is a fixed prefix
         *   path     (none)      whatever the layout admits, preferring fixed
         *            -deform     force the generic path even where fixed is
         *                        eligible.  Benchmark-only, and the whole
         *                        point of 05-B: it makes deform and fixed
         *                        comparable on ONE physical table.
         *            -fixed      force the fixed path.  Errors on a layout
         *                        that cannot support it -- never a silent
         *                        downgrade.
         *
         * Nothing here changes production path selection: 'heap' behaves
         * exactly as before, and the two forcing modes exist only for this
         * measurement.
         */
        char       *why = NULL;
        Relation    rel = table_open(fact_relid, AccessShareLock);
        bool        can_fixed = xpb_heap_layout_supports_fixed(rel, attnos,
                                                               V2_NCOLS, &why);
        XpbHeapPath want;

        table_close(rel, AccessShareLock);

        {
            const char *dash = strrchr(mode, '-');

            if (dash == NULL)
                want = can_fixed ? XPB_HEAP_FIXED : XPB_HEAP_DEFORM;
            else if (strcmp(dash, "-deform") == 0)
                want = XPB_HEAP_DEFORM;
            else if (strcmp(dash, "-fixed") == 0)
                want = XPB_HEAP_FIXED;
            else
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("xpb_v2_register_report: unknown source mode \"%s\"",
                                mode),
                         errhint("<table>[-deform|-fixed], where table is heap, bad or fixedlayout; or pgcolumnar, or zlfs.")));
        }

        heap_path_used = (want == XPB_HEAP_FIXED) ? "fixed" : "deform";
        src = xpb_heap_source_create_ex(fact_relid, attnos, V2_NCOLS, want,
                                        true, lo, hi);
        is_heap = true;
    }

    INSTR_TIME_SET_CURRENT(tn);
    open_ms = INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

    memset(&batch, 0, sizeof(batch));
    batch.capacity = XPCB_BATCH_CAP;
    batch.ncols = V2_NCOLS;

    /* Per-batch scratch, allocated once: at XPCB_BATCH_CAP these are about
     * 590 KB together, which does not belong on the stack. */
    grp1_buf = palloc(XPCB_BATCH_CAP * sizeof(int32));
    grp2_buf = palloc(XPCB_BATCH_CAP * sizeof(int32));
    keep = palloc(XPCB_BATCH_CAP * sizeof(bool));

    for (;;)
    {
        int32  *col_co;
        int64  *col_ac, *col_dt, *col_kt;
        int     nrows;

        INSTR_TIME_SET_CURRENT(tp);
        if (!src->ops->next_batch(src, &batch))
        {
            INSTR_TIME_SET_CURRENT(tn);
            source_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);
            break;
        }
        INSTR_TIME_SET_CURRENT(tn);
        source_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

        nbatches++;
        nrows = batch.nrows;
        rows_in += nrows;

        /* dispatch once per batch, never inside a row loop */
        col_co = xpcb_i32(&batch, V2_C_COMPANY);
        col_ac = xpcb_i64(&batch, V2_C_ACCOUNT);
        col_dt = xpcb_i64(&batch, V2_C_DEBIT);
        col_kt = xpcb_i64(&batch, V2_C_CREDIT);

        /* join 1: company_key -> company_group */
        INSTR_TIME_SET_CURRENT(tp);
        for (int r = 0; r < nrows; r++)
            keep[r] = !xpcb_isnull(&batch, V2_C_COMPANY, r) &&
                      v2_dim1_lookup(dim1, col_co[r], &grp1_buf[r]);
        INSTR_TIME_SET_CURRENT(tn);
        j1_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

        /* join 2: account_key -> account_group */
        INSTR_TIME_SET_CURRENT(tp);
        for (int r = 0; r < nrows; r++)
            if (keep[r])
                keep[r] = !xpcb_isnull(&batch, V2_C_ACCOUNT, r) &&
                          v2_dim2_lookup(dim2, col_ac[r], &grp2_buf[r]);
        INSTR_TIME_SET_CURRENT(tn);
        j2_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

        /* aggregate */
        INSTR_TIME_SET_CURRENT(tp);
        for (int r = 0; r < nrows; r++)
        {
            int32   g1, g2, ck;
            int64   dt, kt;
            uint32  h;

            if (!keep[r])
                continue;
            g1 = grp1_buf[r];
            g2 = grp2_buf[r];
            ck = col_co[r];
            dt = col_dt[r];
            kt = col_kt[r];

            h = (uint32) g1 * 2654435761u ^ (uint32) g2 * 2246822519u
              ^ (uint32) ck * 0x45d9f3bu;
            for (int probe = 0; probe < V2_GRP_CAP; probe++)
            {
                int       idx = (int) ((h + probe) & (V2_GRP_CAP - 1));
                V2Group  *g = &ht[idx];

                if (!g->occupied)
                {
                    if (ngroups >= V2_GRP_LOAD)
                        ereport(ERROR,
                                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                                 errmsg("v2_register_report: group hash overflow (%d groups, cap %d)",
                                        ngroups, V2_GRP_CAP)));
                    g->occupied = true;
                    g->company_group = g1; g->account_group = g2;
                    g->company_key = ck;
                    g->debit = dt; g->credit = kt;
                    ngroups++;
                    break;
                }
                if (g->company_group == g1 && g->account_group == g2 &&
                    g->company_key == ck)
                { g->debit += dt; g->credit += kt; break; }
            }
        }
        INSTR_TIME_SET_CURRENT(tn);
        agg_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

        xpcb_release_owned(&batch);
        CHECK_FOR_INTERRUPTS();
    }

    INSTR_TIME_SET_CURRENT(t1);

    {
        double  total_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);
        StringInfoData  extra;

        initStringInfo(&extra);
        if (is_pgcn)
        {
            int64 gr, grs, vskip, remit;

            xpcn_source_pruning(src, &gr, &grs, &vskip, &remit);
            appendStringInfo(&extra,
                             "  rowgroups_read=" INT64_FORMAT
                             " rows_in_read_groups=" INT64_FORMAT
                             " rows_vec_skipped=" INT64_FORMAT
                             " rows_emitted=" INT64_FORMAT,
                             gr, grs, vskip, remit);
        }
        else if (is_heap)
        {
            int64   td_, ad_;
            int     ar_;
            bool    isdef;

            xpb_heap_source_deform_stats(src, &td_, &ad_, &ar_, &isdef);
            appendStringInfo(&extra,
                             "  heap_path=%s  tuples_deformed=" INT64_FORMAT
                             " attrs_deformed=" INT64_FORMAT " attrs_requested=%d",
                             heap_path_used, td_, ad_, ar_);
        }

        elog(NOTICE,
             "v2_register_report [%d..%d] mode=%s: total=%.1f ms  build=%.1f ms  "
             "open=%.1f ms  source=%.1f ms  join1=%.1f ms  join2=%.1f ms  agg=%.1f ms  "
             "operators=%.1f ms  rows=" INT64_FORMAT "  batches=%d  groups=%d%s",
             lo, hi, mode, total_ms, build_ms, open_ms, source_ms, j1_ms, j2_ms, agg_ms,
             j1_ms + j2_ms + agg_ms, rows_in, nbatches, ngroups, extra.data);

        for (int i = 0; i < V2_GRP_CAP; i++)
        {
            Datum   vals[7];
            bool    nulls[7] = {false, false, false, false, false, false, false};

            if (!ht[i].occupied)
                continue;
            vals[0] = Int32GetDatum(ht[i].company_group);
            vals[1] = Int32GetDatum(ht[i].account_group);
            vals[2] = Int32GetDatum(ht[i].company_key);
            vals[3] = Int64GetDatum(ht[i].debit);
            vals[4] = Int64GetDatum(ht[i].credit);
            /* net is derived at emit time from two exact int64 sums, not by a
             * second pass over the data */
            vals[5] = Int64GetDatum(ht[i].debit - ht[i].credit);
            vals[6] = Float8GetDatum(total_ms);
            tuplestore_putvalues(store, rsi->setDesc, vals, nulls);
        }
    }

    src->ops->end(src);
    return (Datum) 0;
}
