/*
 * xpb_columnar_pipeline.c
 *
 * End-to-end 3-stage pipeline proof on 60K+ group intermediate.
 *
 * Stage 1: Scan reg_buh → GROUP BY (period, company, account)
 *          → 60,000 groups × 6 columns [k1, k2, k3, sum_dt, sum_ct, sum_qty]
 *
 * Stage 2: Re-aggregate by (period, company) reading ONLY k1, k2, sum_dt
 *          → 6,000 groups (column-selective: 3 of 6 columns)
 *
 * Stage 3: Final by period → 120 groups
 *
 * Two paths:
 *   A) ColumnarTR: stage1 → 6 column arrays → stage2 reads 3 arrays → stage3
 *   B) AoS struct: stage1 → struct array → stage2 walks all structs → stage3
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

#include "xpb_columnar_tr.h"
#include "utils/tuplestore.h"

PG_FUNCTION_INFO_V1(ctr_pipeline);

/* Wide group: 3 keys + 3 aggregates */
typedef struct WideGroup {
    int32 k1, k2, k3;       /* period, company, account */
    int64 sum_dt, sum_ct, sum_qty;
    bool  occupied;
} WideGroup;

#define WHASH_CAP 131072  /* 128K slots, power of 2 */

Datum
ctr_pipeline(PG_FUNCTION_ARGS)
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

    /* ═══ STAGE 1: scan → hash agg (shared between both paths) ═══ */
    WideGroup *ht = palloc0(WHASH_CAP * sizeof(WideGroup));
    int ngroups = 0;
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
            int32 k1 = *(int32 *)(d);       /* period_key */
            if (k1 < lo || k1 > hi) continue;
            int32 k2 = *(int32 *)(d + 4);   /* company_key */
            int32 k3 = *(int32 *)(d + 8);   /* account_key */
            int32 vdt = *(int32 *)(d + 20);  /* amount_dt */
            int32 vct = *(int32 *)(d + 24);  /* amount_ct */
            int32 vqty = *(int32 *)(d + 28); /* qty */

            uint32 h = (uint32)k1 * 2654435761u ^ (uint32)k2 * 2246822519u
                     ^ (uint32)k3 * 0x9e3779b9u;
            int sl = (int)(h & (WHASH_CAP - 1));
            for (int pr = 0; pr < WHASH_CAP; pr++) {
                int idx = (sl + pr) & (WHASH_CAP - 1);
                WideGroup *g = &ht[idx];
                if (!g->occupied) {
                    g->k1 = k1; g->k2 = k2; g->k3 = k3;
                    g->sum_dt = vdt; g->sum_ct = vct; g->sum_qty = vqty;
                    g->occupied = true; ngroups++; break;
                }
                if (g->k1 == k1 && g->k2 == k2 && g->k3 == k3) {
                    g->sum_dt += vdt; g->sum_ct += vct; g->sum_qty += vqty;
                    break;
                }
            }
        }
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        ReleaseBuffer(buf);
    }
    if (vmbuf != InvalidBuffer) ReleaseBuffer(vmbuf);
    INSTR_TIME_SET_CURRENT(t1);
    double scan_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

    int range = hi - lo + 1;

    /* ═══ PATH A: ColumnarTR pipeline ═══ */
    {
        INSTR_TIME_SET_CURRENT(t0);
        /* Materialize hash → 6 column arrays */
        ColumnarTR *ctr = ctr_create(6, ngroups, CurrentMemoryContext);
        for (int i = 0; i < WHASH_CAP; i++) {
            if (!ht[i].occupied) continue;
            int64 row[6] = { ht[i].k1, ht[i].k2, ht[i].k3,
                             ht[i].sum_dt, ht[i].sum_ct, ht[i].sum_qty };
            ctr_append_row(ctr, row);
        }
        INSTR_TIME_SET_CURRENT(t1);
        double mat_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

        /* Stage 2: column-selective re-agg by (k1, k2) — reads ONLY cols 0,1,3 */
        INSTR_TIME_SET_CURRENT(t0);
        int64 *col_k1 = ctr->cols[0];
        int64 *col_k2 = ctr->cols[1];
        int64 *col_dt = ctr->cols[3];  /* skip cols 2(k3), 4(ct), 5(qty) */

        /* Hash into (period×company) = max 6000 slots */
        #define S2CAP 16384
        typedef struct { int32 k1,k2; int64 sum; bool occ; } S2Entry;
        S2Entry *s2 = palloc0(S2CAP * sizeof(S2Entry));

        for (int64 i = 0; i < ctr->nrows; i++) {
            int32 pk = (int32)col_k1[i];
            int32 ck = (int32)col_k2[i];
            int64 dt = col_dt[i];
            uint32 h = (uint32)pk * 2654435761u ^ (uint32)ck * 2246822519u;
            int sl = (int)(h & (S2CAP - 1));
            for (int pr = 0; pr < S2CAP; pr++) {
                int idx = (sl + pr) & (S2CAP - 1);
                if (!s2[idx].occ) { s2[idx].k1=pk; s2[idx].k2=ck; s2[idx].sum=dt; s2[idx].occ=true; break; }
                if (s2[idx].k1==pk && s2[idx].k2==ck) { s2[idx].sum+=dt; break; }
            }
        }

        /* Stage 3: final by period */
        int64 *totals = palloc0(range * sizeof(int64));
        for (int i = 0; i < S2CAP; i++)
            if (s2[i].occ) totals[s2[i].k1 - lo] += s2[i].sum;

        INSTR_TIME_SET_CURRENT(t1);
        double s23_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

        for (int i = 0; i < range; i++) {
            if (totals[i] == 0) continue;
            vals[0] = Int32GetDatum(lo + i);
            vals[1] = Int64GetDatum(totals[i]);
            vals[2] = CStringGetTextDatum("columnar_tr");
            vals[3] = Float8GetDatum(scan_ms + mat_ms + s23_ms);
            tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
        }
        elog(NOTICE, "CTR: scan=%.0f mat=%.3f s23=%.3f total=%.1f ms | groups=%d bytes=%ld",
             scan_ms, mat_ms, s23_ms, scan_ms+mat_ms+s23_ms, ngroups, ctr_bytes(ctr));
        pfree(totals); pfree(s2); ctr_free(ctr);
        #undef S2CAP
    }

    /* ═══ PATH B: AoS struct pipeline ═══ */
    {
        INSTR_TIME_SET_CURRENT(t0);

        /* Stage 2: walk struct array, access .k1, .k2, .sum_dt (touches all 48 bytes per struct) */
        #define S2CAP 16384
        typedef struct { int32 k1,k2; int64 sum; bool occ; } S2Entry;
        S2Entry *s2 = palloc0(S2CAP * sizeof(S2Entry));

        for (int i = 0; i < WHASH_CAP; i++) {
            if (!ht[i].occupied) continue;
            int32 pk = ht[i].k1;
            int32 ck = ht[i].k2;
            int64 dt = ht[i].sum_dt;
            uint32 h = (uint32)pk * 2654435761u ^ (uint32)ck * 2246822519u;
            int sl = (int)(h & (S2CAP - 1));
            for (int pr = 0; pr < S2CAP; pr++) {
                int idx = (sl + pr) & (S2CAP - 1);
                if (!s2[idx].occ) { s2[idx].k1=pk; s2[idx].k2=ck; s2[idx].sum=dt; s2[idx].occ=true; break; }
                if (s2[idx].k1==pk && s2[idx].k2==ck) { s2[idx].sum+=dt; break; }
            }
        }

        /* Stage 3 */
        int64 *totals = palloc0(range * sizeof(int64));
        for (int i = 0; i < S2CAP; i++)
            if (s2[i].occ) totals[s2[i].k1 - lo] += s2[i].sum;

        INSTR_TIME_SET_CURRENT(t1);
        double s23_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

        for (int i = 0; i < range; i++) {
            if (totals[i] == 0) continue;
            vals[0] = Int32GetDatum(lo + i);
            vals[1] = Int64GetDatum(totals[i]);
            vals[2] = CStringGetTextDatum("aos_struct");
            vals[3] = Float8GetDatum(scan_ms + s23_ms);
            tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
        }
        elog(NOTICE, "AoS: scan=%.0f s23=%.3f total=%.1f ms | groups=%d struct_bytes=%ld",
             scan_ms, s23_ms, scan_ms+s23_ms, ngroups,
             (long)ngroups * (long)sizeof(WideGroup));
        pfree(totals); pfree(s2);
        #undef S2CAP
    }

    pfree(ht);
    table_close(rel, AccessShareLock);
    return (Datum) 0;
}
