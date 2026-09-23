/*
 * xpb_projection.c
 *
 * Minimal columnar projection experiment.
 *
 * Creates a dense int32[] projection of reg_buh analytical columns,
 * then runs the same GROUP BY query on projection vs heap.
 *
 * Projection: 4 × int32 arrays (period_key, company_key, account_key, amount_dt)
 * = 160MB for 10M rows. Compare with heap: 586MB (73K pages × 8KB).
 *
 * Projection scan: sequential array access, no buffer manager,
 * no tuple deform, no visibility check, no page locking.
 */
#include "postgres.h"
#include "funcapi.h"
#include "portability/instr_time.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/namespace.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

PG_FUNCTION_INFO_V1(projection_experiment);

/* Projection: 4 analytical columns as dense arrays */
typedef struct Projection {
    int32  *period_key;
    int32  *company_key;
    int32  *account_key;
    int32  *amount_dt;
    int64   nrows;
    double  build_ms;
} Projection;

/* Hash table for GROUP BY (period, company) */
#define AGG_CAP 16384
typedef struct AggEntry {
    int32 k1, k2;
    int64 sum;
    bool  occ;
} AggEntry;

static void
agg_insert(AggEntry *ht, int32 k1, int32 k2, int32 val)
{
    uint32 h = (uint32)k1 * 2654435761u ^ (uint32)k2 * 2246822519u;
    int sl = (int)(h & (AGG_CAP - 1));
    for (int pr = 0; pr < AGG_CAP; pr++) {
        int idx = (sl + pr) & (AGG_CAP - 1);
        if (!ht[idx].occ) {
            ht[idx].k1 = k1; ht[idx].k2 = k2;
            ht[idx].sum = val; ht[idx].occ = true; return;
        }
        if (ht[idx].k1 == k1 && ht[idx].k2 == k2) {
            ht[idx].sum += val; return;
        }
    }
}

Datum
projection_experiment(PG_FUNCTION_ARGS)
{
    int32 lo = PG_GETARG_INT32(0);
    int32 hi = PG_GETARG_INT32(1);

    InitMaterializedSRF(fcinfo, 0);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    Oid relid = RelnameGetRelid("reg_buh");
    if (!OidIsValid(relid))
        ereport(ERROR, (errmsg("table reg_buh not found")));
    Relation rel = table_open(relid, AccessShareLock);
    BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
    Snapshot snap = GetActiveSnapshot();
    Oid rel_oid = RelationGetRelid(rel);

    instr_time t0, t1;
    Datum vals[4];
    bool nulls[4] = {false, false, false, false};
    int range = hi - lo + 1;

    /* ═══ BUILD PROJECTION ═══ */
    int64 capacity = (int64)nblocks * 200;  /* ~136 rows/page, with margin */
    int32 *col_pk  = palloc(capacity * sizeof(int32));
    int32 *col_ck  = palloc(capacity * sizeof(int32));
    int32 *col_ak  = palloc(capacity * sizeof(int32));
    int32 *col_dt  = palloc(capacity * sizeof(int32));
    int64 nrows = 0;
    Buffer vmbuf = InvalidBuffer;

    INSTR_TIME_SET_CURRENT(t0);
    for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
    {
        Buffer buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buf);
        bool av = (visibilitymap_get_status(rel, blkno, &vmbuf)
                   & VISIBILITYMAP_ALL_VISIBLE) != 0;
        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

        for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
        {
            ItemId lp = PageGetItemId(page, off);
            if (!ItemIdIsNormal(lp)) continue;
            HeapTupleHeader htup = (HeapTupleHeader) PageGetItem(page, lp);

            if (!av) {
                HeapTupleData td;
                td.t_data = htup; td.t_len = ItemIdGetLength(lp);
                td.t_tableOid = rel_oid;
                ItemPointerSet(&td.t_self, blkno, off);
                if (!HeapTupleSatisfiesVisibility(&td, snap, buf)) continue;
            }

            char *d = (char *)htup + htup->t_hoff;
            col_pk[nrows]  = *(int32 *)(d);
            col_ck[nrows]  = *(int32 *)(d + 4);
            col_ak[nrows]  = *(int32 *)(d + 8);
            col_dt[nrows]  = *(int32 *)(d + 20);
            nrows++;
        }
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        ReleaseBuffer(buf);
    }
    if (vmbuf != InvalidBuffer) ReleaseBuffer(vmbuf);
    INSTR_TIME_SET_CURRENT(t1);
    double build_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

    elog(NOTICE, "Projection built: %ld rows, %.0f ms, %ld MB (4 cols × int32)",
         nrows, build_ms, nrows * 4 * 4 / (1024*1024));

    /* ═══ PATH A: PROJECTION SCAN ═══ */
    {
        AggEntry *ht = palloc0(AGG_CAP * sizeof(AggEntry));

        INSTR_TIME_SET_CURRENT(t0);
        /* Sequential scan of column arrays — no buffer mgr, no tuple deform */
        for (int64 i = 0; i < nrows; i++)
        {
            int32 pk = col_pk[i];
            if (pk < lo || pk > hi) continue;
            agg_insert(ht, pk, col_ck[i], col_dt[i]);
        }
        INSTR_TIME_SET_CURRENT(t1);
        double scan_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

        int64 *totals = palloc0(range * sizeof(int64));
        int ngroups = 0;
        for (int i = 0; i < AGG_CAP; i++) {
            if (!ht[i].occ) continue;
            ngroups++;
            if (ht[i].k1 >= lo && ht[i].k1 <= hi)
                totals[ht[i].k1 - lo] += ht[i].sum;
        }

        for (int i = 0; i < range; i++) {
            vals[0] = Int32GetDatum(lo + i);
            vals[1] = Int64GetDatum(totals[i]);
            vals[2] = CStringGetTextDatum("projection");
            vals[3] = Float8GetDatum(scan_ms);
            tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
        }
        elog(NOTICE, "Projection scan: %.1f ms, %d groups (build=%.0f ms, total=%.1f ms)",
             scan_ms, ngroups, build_ms, build_ms + scan_ms);
        pfree(ht); pfree(totals);
    }

    /* ═══ PATH B: HEAP SCAN (same query, same hash agg) ═══ */
    {
        AggEntry *ht = palloc0(AGG_CAP * sizeof(AggEntry));
        vmbuf = InvalidBuffer;

        INSTR_TIME_SET_CURRENT(t0);
        for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
        {
            Buffer buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            Page page = BufferGetPage(buf);
            bool av = (visibilitymap_get_status(rel, blkno, &vmbuf)
                       & VISIBILITYMAP_ALL_VISIBLE) != 0;
            OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

            for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
            {
                ItemId lp = PageGetItemId(page, off);
                if (!ItemIdIsNormal(lp)) continue;
                HeapTupleHeader htup = (HeapTupleHeader) PageGetItem(page, lp);

                if (!av) {
                    HeapTupleData td;
                    td.t_data = htup; td.t_len = ItemIdGetLength(lp);
                    td.t_tableOid = rel_oid;
                    ItemPointerSet(&td.t_self, blkno, off);
                    if (!HeapTupleSatisfiesVisibility(&td, snap, buf)) continue;
                }

                char *d = (char *)htup + htup->t_hoff;
                int32 pk = *(int32 *)(d);
                if (pk < lo || pk > hi) continue;
                agg_insert(ht, pk, *(int32 *)(d + 4), *(int32 *)(d + 20));
            }
            LockBuffer(buf, BUFFER_LOCK_UNLOCK);
            ReleaseBuffer(buf);
        }
        if (vmbuf != InvalidBuffer) ReleaseBuffer(vmbuf);
        INSTR_TIME_SET_CURRENT(t1);
        double scan_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

        int64 *totals = palloc0(range * sizeof(int64));
        int ngroups = 0;
        for (int i = 0; i < AGG_CAP; i++) {
            if (!ht[i].occ) continue;
            ngroups++;
            if (ht[i].k1 >= lo && ht[i].k1 <= hi)
                totals[ht[i].k1 - lo] += ht[i].sum;
        }

        for (int i = 0; i < range; i++) {
            vals[0] = Int32GetDatum(lo + i);
            vals[1] = Int64GetDatum(totals[i]);
            vals[2] = CStringGetTextDatum("heap_scan");
            vals[3] = Float8GetDatum(scan_ms);
            tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
        }
        elog(NOTICE, "Heap scan: %.1f ms, %d groups",
             scan_ms, ngroups);
        pfree(ht); pfree(totals);
    }

    pfree(col_pk); pfree(col_ck); pfree(col_ak); pfree(col_dt);
    table_close(rel, AccessShareLock);
    return (Datum) 0;
}
