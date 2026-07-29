/*
 * xpb_batch_groupagg.c — independent batch GROUP BY SUM consumer
 *
 * Reads from any XpBatchSource via the compact batch contract.
 * Has NO dependency on ZLFS, heap, or specific storage internals.
 *
 * SQL entry point: xpb_batch_groupby(lo, hi, source_mode)
 *   source_mode: 'zlfs', 'heap', 'slot', 'copy'
 */
#include "postgres.h"
#include "funcapi.h"
#include "catalog/namespace.h"
#include "executor/tuptable.h"
#include "access/htup_details.h"
#include "portability/instr_time.h"
#include "utils/builtins.h"

#include "xpb_colbatch.h"
#include "xpb_zlfs.h"

/* Provider constructors (defined in xpb_src_*.c) */
extern XpBatchSource *xpb_zlfs_source_create(ZlfsZone *zone, int16 *requested_attnos, int ncols);
extern XpBatchSource *xpb_heap_source_create(Oid relid, int16 *requested_attnos, int ncols,
                                              bool has_pred, int32 pred_lo, int32 pred_hi);
extern void xpb_heap_source_stats(XpBatchSource *src, int64 *pr, int64 *ps,
                                   int64 *tv, int64 *tp);

/* ── Hash aggregate over compact batches ── */

#define GRP_CAP      16384
#define GRP_MAX_LOAD (GRP_CAP * 3 / 4)

typedef struct CGroupEntry
{
    int32   k1, k2;
    int64   sum;
    bool    occupied;
} CGroupEntry;

typedef struct BatchAggResult
{
    CGroupEntry *htable;
    int          ngroups;
    double       source_ms;
    double       agg_ms;
    double       total_ms;
    int64        rows_consumed;
    int          batches_consumed;
} BatchAggResult;

static void
batch_aggregate(XpBatchSource *source, BatchAggResult *res)
{
    CGroupEntry *ht = palloc0(GRP_CAP * sizeof(CGroupEntry));
    int ngroups = 0;
    int64 total_rows = 0;
    int   nbatches = 0;

    XpColumnBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.ncols = 3;
    batch.capacity = XPCB_BATCH_CAP;

    /*
     * For heap provider: pre-allocate column arrays (provider fills them).
     * For ZLFS provider: provider will borrow pointers (owns_data=false).
     * We track our own allocations separately to free them at the end.
     */
    int32 *own_cols[XPCB_MAX_COLS] = {NULL};
    bool consumer_owns = false;

    instr_time t_total_start, t_total_end;
    instr_time t_src_start, t_src_end;
    double source_accum = 0.0;

    INSTR_TIME_SET_CURRENT(t_total_start);

    while (true)
    {
        batch.nrows = 0;
        batch.selection = NULL;
        batch.nselected = 0;

        INSTR_TIME_SET_CURRENT(t_src_start);
        bool got = source->ops->next_batch(source, &batch);
        INSTR_TIME_SET_CURRENT(t_src_end);
        source_accum += INSTR_TIME_GET_MILLISEC(t_src_end) -
                        INSTR_TIME_GET_MILLISEC(t_src_start);

        if (!got)
            break;

        /* First owned batch: save our allocated pointers */
        if (!consumer_owns && batch.owns_data)
        {
            consumer_owns = true;
            for (int c = 0; c < batch.ncols; c++)
                own_cols[c] = batch.int32_cols[c];
        }

        nbatches++;
        int nrows = batch.nrows;
        int32 *col_pk = batch.int32_cols[0];
        int32 *col_ck = batch.int32_cols[1];
        int32 *col_dt = batch.int32_cols[2];

        for (int i = 0; i < nrows; i++)
        {
            int32 pk = col_pk[i];
            int32 ck = col_ck[i];
            int64 dt = (int64)col_dt[i];

            uint32 h = (uint32)pk * 2654435761u ^ (uint32)ck * 2246822519u;
            int sl = (int)(h & (GRP_CAP - 1));
            for (int pr = 0; pr < GRP_CAP; pr++)
            {
                int idx = (sl + pr) & (GRP_CAP - 1);
                CGroupEntry *g = &ht[idx];
                if (!g->occupied)
                {
                    if (ngroups >= GRP_MAX_LOAD)
                        ereport(ERROR,
                                (errmsg("batch_groupagg: hash overflow (%d groups)", ngroups)));
                    g->k1 = pk; g->k2 = ck; g->sum = dt;
                    g->occupied = true;
                    ngroups++;
                    break;
                }
                if (g->k1 == pk && g->k2 == ck)
                {
                    g->sum += dt;
                    break;
                }
            }
        }
        total_rows += nrows;
    }

    INSTR_TIME_SET_CURRENT(t_total_end);

    res->htable = ht;
    res->ngroups = ngroups;
    res->source_ms = source_accum;
    res->total_ms = INSTR_TIME_GET_MILLISEC(t_total_end) -
                    INSTR_TIME_GET_MILLISEC(t_total_start);
    res->agg_ms = res->total_ms - res->source_ms;
    res->rows_consumed = total_rows;
    res->batches_consumed = nbatches;

    /* Free consumer-owned column allocations */
    if (consumer_owns)
    {
        for (int c = 0; c < batch.ncols; c++)
            if (own_cols[c])
                pfree(own_cols[c]);
    }
}

/*
 * slot_aggregate — negative control: ZLFS data → TupleTableSlot → row-at-a-time
 *
 * Measures the cost of converting compact columns back to row representation.
 * Uses Virtual slots (Datum/isnull arrays) — the cheapest slot type.
 */
static void
slot_aggregate(ZlfsZone *zone, BatchAggResult *res)
{
    CGroupEntry *ht = palloc0(GRP_CAP * sizeof(CGroupEntry));
    int ngroups = 0;

    int32 *col_pk = zone->cols[0];
    int32 *col_ck = zone->cols[1];
    int32 *col_dt = zone->cols[2];

    /* Create a virtual TupleTableSlot with 3 columns */
    TupleDesc tdesc = CreateTemplateTupleDesc(3);
    TupleDescInitEntry(tdesc, 1, "period_key", INT4OID, -1, 0);
    TupleDescInitEntry(tdesc, 2, "company_key", INT4OID, -1, 0);
    TupleDescInitEntry(tdesc, 3, "amount_dt", INT4OID, -1, 0);
    TupleTableSlot *slot = MakeSingleTupleTableSlot(tdesc, &TTSOpsVirtual);

    instr_time t0, t1, t2, t3;

    /* Phase 1: materialize all rows through slots */
    INSTR_TIME_SET_CURRENT(t0);

    for (int64 i = 0; i < zone->nrows; i++)
    {
        ExecClearTuple(slot);
        slot->tts_values[0] = Int32GetDatum(col_pk[i]);
        slot->tts_values[1] = Int32GetDatum(col_ck[i]);
        slot->tts_values[2] = Int32GetDatum(col_dt[i]);
        slot->tts_isnull[0] = false;
        slot->tts_isnull[1] = false;
        slot->tts_isnull[2] = false;
        ExecStoreVirtualTuple(slot);

        /* Read back from slot and aggregate */
        int32 pk = DatumGetInt32(slot->tts_values[0]);
        int32 ck = DatumGetInt32(slot->tts_values[1]);
        int64 dt = (int64)DatumGetInt32(slot->tts_values[2]);

        uint32 h = (uint32)pk * 2654435761u ^ (uint32)ck * 2246822519u;
        int sl = (int)(h & (GRP_CAP - 1));
        for (int pr = 0; pr < GRP_CAP; pr++)
        {
            int idx = (sl + pr) & (GRP_CAP - 1);
            CGroupEntry *g = &ht[idx];
            if (!g->occupied)
            {
                if (ngroups >= GRP_MAX_LOAD)
                    ereport(ERROR, (errmsg("slot_agg: hash overflow")));
                g->k1 = pk; g->k2 = ck; g->sum = dt;
                g->occupied = true; ngroups++; break;
            }
            if (g->k1 == pk && g->k2 == ck) { g->sum += dt; break; }
        }
    }

    INSTR_TIME_SET_CURRENT(t1);

    /*
     * Phase timing: run materialization-only and aggregate-only separately
     * to get clean phase split without per-row timer overhead.
     */

    /* Phase 2: materialization only (no aggregate) */
    INSTR_TIME_SET_CURRENT(t2);
    for (int64 i = 0; i < zone->nrows; i++)
    {
        ExecClearTuple(slot);
        slot->tts_values[0] = Int32GetDatum(col_pk[i]);
        slot->tts_values[1] = Int32GetDatum(col_ck[i]);
        slot->tts_values[2] = Int32GetDatum(col_dt[i]);
        slot->tts_isnull[0] = false;
        slot->tts_isnull[1] = false;
        slot->tts_isnull[2] = false;
        ExecStoreVirtualTuple(slot);
        /* read back to prevent dead-code elimination */
        (void) DatumGetInt32(slot->tts_values[0]);
    }
    INSTR_TIME_SET_CURRENT(t3);

    ExecDropSingleTupleTableSlot(slot);

    double total_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);
    double mat_only = INSTR_TIME_GET_MILLISEC(t3) - INSTR_TIME_GET_MILLISEC(t2);
    double agg_est = total_ms - mat_only;

    res->htable = ht;
    res->ngroups = ngroups;
    res->total_ms = total_ms;
    res->source_ms = mat_only;
    res->agg_ms = (agg_est > 0) ? agg_est : 0;
    res->rows_consumed = zone->nrows;
    res->batches_consumed = 0;
}

/*
 * copy_batch_aggregate — ZLFS columns copied into owned batch before aggregate.
 * Measures cost of copying vs borrowing.
 */
static void
copy_batch_aggregate(ZlfsZone *zone, BatchAggResult *res)
{
    CGroupEntry *ht = palloc0(GRP_CAP * sizeof(CGroupEntry));
    int ngroups = 0;
    int64 total_rows = 0;
    int nbatches = 0;
    int64 cursor = 0;
    int cap = XPCB_BATCH_CAP;

    int32 *buf_pk = palloc(cap * sizeof(int32));
    int32 *buf_ck = palloc(cap * sizeof(int32));
    int32 *buf_dt = palloc(cap * sizeof(int32));

    instr_time t0, t1;
    double copy_accum = 0.0, agg_accum = 0.0;

    INSTR_TIME_SET_CURRENT(t0);

    while (cursor < zone->nrows)
    {
        instr_time t_phase;

        int64 remaining = zone->nrows - cursor;
        int chunk = (remaining > cap) ? cap : (int)remaining;

        /* Copy phase */
        INSTR_TIME_SET_CURRENT(t_phase);
        memcpy(buf_pk, zone->cols[0] + cursor, chunk * sizeof(int32));
        memcpy(buf_ck, zone->cols[1] + cursor, chunk * sizeof(int32));
        memcpy(buf_dt, zone->cols[2] + cursor, chunk * sizeof(int32));
        {
            instr_time t_now;
            INSTR_TIME_SET_CURRENT(t_now);
            copy_accum += INSTR_TIME_GET_MILLISEC(t_now) - INSTR_TIME_GET_MILLISEC(t_phase);
            INSTR_TIME_SET_CURRENT(t_phase);
        }

        /* Aggregate phase */
        for (int i = 0; i < chunk; i++)
        {
            int32 pk = buf_pk[i], ck = buf_ck[i];
            int64 dt = (int64)buf_dt[i];
            uint32 h = (uint32)pk * 2654435761u ^ (uint32)ck * 2246822519u;
            int sl = (int)(h & (GRP_CAP - 1));
            for (int pr = 0; pr < GRP_CAP; pr++)
            {
                int idx = (sl + pr) & (GRP_CAP - 1);
                CGroupEntry *g = &ht[idx];
                if (!g->occupied)
                {
                    if (ngroups >= GRP_MAX_LOAD)
                        ereport(ERROR, (errmsg("copy_agg: hash overflow")));
                    g->k1 = pk; g->k2 = ck; g->sum = dt;
                    g->occupied = true; ngroups++; break;
                }
                if (g->k1 == pk && g->k2 == ck) { g->sum += dt; break; }
            }
        }
        {
            instr_time t_now;
            INSTR_TIME_SET_CURRENT(t_now);
            agg_accum += INSTR_TIME_GET_MILLISEC(t_now) - INSTR_TIME_GET_MILLISEC(t_phase);
        }

        total_rows += chunk;
        cursor += chunk;
        nbatches++;
    }

    INSTR_TIME_SET_CURRENT(t1);

    pfree(buf_pk); pfree(buf_ck); pfree(buf_dt);

    res->htable = ht;
    res->ngroups = ngroups;
    res->total_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);
    res->source_ms = copy_accum;
    res->agg_ms = agg_accum;
    res->rows_consumed = total_rows;
    res->batches_consumed = nbatches;
}

/* ── SQL entry point ── */

PG_FUNCTION_INFO_V1(xpb_batch_groupby);

Datum
xpb_batch_groupby(PG_FUNCTION_ARGS)
{
    int32 lo = PG_GETARG_INT32(0);
    int32 hi = PG_GETARG_INT32(1);
    char *mode = text_to_cstring(PG_GETARG_TEXT_PP(2));

    InitMaterializedSRF(fcinfo, 0);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    Oid relid = RelnameGetRelid("reg_buh");
    if (!OidIsValid(relid))
        ereport(ERROR, (errmsg("table reg_buh not found")));

    XpBatchSource *source;

    if (strcmp(mode, "zlfs") == 0)
    {
        /* ZLFS provider: zero-copy borrowed batches */
        zlfs_ensure_registry();
        zlfs_scan_directory();
        ZlfsZone *zone = zlfs_lookup_valid_zone(relid, lo, hi);
        if (!zone)
            ereport(ERROR, (errmsg("no valid ZLFS zone for [%d..%d]", lo, hi)));

        int16 req_attnos[3] = { 1, 2, 6 };  /* period_key, company_key, amount_dt */
        source = xpb_zlfs_source_create(zone, req_attnos, 3);
    }
    else if (strcmp(mode, "heap") == 0)
    {
        /* Heap provider: decode tuples into compact batches */
        int16 heap_attnos[3] = { 1, 2, 6 };  /* period_key, company_key, amount_dt */
        source = xpb_heap_source_create(relid, heap_attnos, 3, true, lo, hi);
    }
    else if (strcmp(mode, "slot") == 0 || strcmp(mode, "copy") == 0)
    {
        /* Slot/copy control paths — direct ZLFS access for comparison */
        zlfs_ensure_registry();
        zlfs_scan_directory();
        ZlfsZone *zone = zlfs_lookup_valid_zone(relid, lo, hi);
        if (!zone)
            ereport(ERROR, (errmsg("no valid ZLFS zone for [%d..%d]", lo, hi)));

        BatchAggResult res;
        memset(&res, 0, sizeof(res));

        if (strcmp(mode, "slot") == 0)
            slot_aggregate(zone, &res);
        else
            copy_batch_aggregate(zone, &res);

        /* Emit results */
        Datum vals[4];
        bool nulls[4] = {false, false, false, false};
        for (int i = 0; i < GRP_CAP; i++)
        {
            CGroupEntry *g = &res.htable[i];
            if (!g->occupied) continue;
            vals[0] = Int32GetDatum(g->k1);
            vals[1] = Int32GetDatum(g->k2);
            vals[2] = Int64GetDatum(g->sum);
            vals[3] = Float8GetDatum(res.total_ms);
            tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
        }

        elog(NOTICE, "batch_groupby [%d..%d] mode=%s: "
             "total=%.1f ms  materialize=%.1f ms  agg=%.1f ms  "
             "rows=%ld  groups=%d",
             lo, hi, mode,
             res.total_ms, res.source_ms, res.agg_ms,
             res.rows_consumed, res.ngroups);

        pfree(res.htable);
        return (Datum) 0;
    }
    else
    {
        ereport(ERROR, (errmsg("unknown mode: %s (use zlfs, heap, or slot)", mode)));
    }

    /* Run the generic aggregate — it knows nothing about ZLFS or heap */
    BatchAggResult res;
    memset(&res, 0, sizeof(res));
    batch_aggregate(source, &res);

    /* Emit results */
    Datum vals[4];
    bool nulls[4] = {false, false, false, false};
    for (int i = 0; i < GRP_CAP; i++)
    {
        CGroupEntry *g = &res.htable[i];
        if (!g->occupied) continue;
        vals[0] = Int32GetDatum(g->k1);
        vals[1] = Int32GetDatum(g->k2);
        vals[2] = Int64GetDatum(g->sum);
        vals[3] = Float8GetDatum(res.total_ms);
        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
    }

    /* Report timing breakdown */
    elog(NOTICE, "batch_groupby [%d..%d] mode=%s: "
         "total=%.1f ms  source=%.1f ms  agg=%.1f ms  "
         "rows=%ld  batches=%d  groups=%d",
         lo, hi, mode,
         res.total_ms, res.source_ms, res.agg_ms,
         res.rows_consumed, res.batches_consumed, res.ngroups);

    /* Heap stats */
    if (strcmp(mode, "heap") == 0)
    {
        int64 pr, ps, tv, tp;
        xpb_heap_source_stats(source, &pr, &ps, &tv, &tp);
        elog(NOTICE, "  heap: pages_rejected=%ld pages_scanned=%ld "
             "tuples_visited=%ld tuples_passed=%ld",
             pr, ps, tv, tp);
    }

    source->ops->end(source);
    pfree(res.htable);

    return (Datum) 0;
}
