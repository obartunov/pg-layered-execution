/*-------------------------------------------------------------------------
 * xpb_groupagg2.c
 *
 * XpGroupAgg2 — composite StreamingAgg for (int4, int4) GROUP BY.
 *
 * Handles exactly:
 *   SELECT k1, k2, agg(v) FROM t GROUP BY k1, k2 ORDER BY k1, k2
 *
 * BatchProperties contract (composite):
 *   key_attno   = k1 (primary)
 *   key_attno2  = k2 (secondary)
 *   order_scope = STREAM: lexicographic (k1,k2) non-decreasing across all tuples
 *   Revoke:     first tuple where (k1,k2) < (prev_k1,prev_k2) → NONE (hash)
 *
 * STREAM path: O(1) state, one open group (cur_k1, cur_k2)
 *   flush on any (k1,k2) change
 *   output already in (k1,k2) order → Sort eliminated via pathkeys
 *
 * NONE fallback: open-addressing hash, capacity from estimate_num_groups()
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include <math.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/pg_class.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "catalog/pg_statistic.h"

#include "xpb_native.h"

/* =========================================================
 * Predicate classification (per @yoda):
 *
 *  Class 1 — stream-key bounds:
 *    predicate on leading STREAM key column
 *    role: scan boundary condition (early exit)
 *    e.g. period_key BETWEEN 100 AND 200
 *
 *  Class 2 — non-key pushable predicates:
 *    predicate on non-key int4 column
 *    role: LiveBitmap on surviving pages (Phase 2)
 *    e.g. amount_dr > 50000, org_key = 25
 *
 *  Class 3 — residual:
 *    not safely pushable (OR, non-int4, complex expr)
 *    role: ExecQual on survivors
 * ========================================================= */
#define XPB_MAX_PREDS 8

#include "xpb_groupagg2_types.h"
#include "xpb_cost.h"
#include "xpb_qual.h"
#include "xpb_typeops.h"
#include "xpb_scalar_batch.h"
#include "portability/instr_time.h"
#include "storage/read_stream.h"
#include "utils/builtins.h"
#include "xpb_simd.h"
#include "xpb_zlfs.h"

/*
 * Legacy type aliases — replaced by XpbQual in xpb_qual.h.
 * Kept here during Q1 migration for compiler compatibility;
 * will be removed after serialization is migrated to xpb_qual_serialize().
 */
typedef XpbQualPred  PushedPred;
typedef XpbQual      PushedQualFragment;
/* PQ_* → XQO_* migration complete; aliases removed */


extern bool xp_batch_enabled;

bool   xpb_groupagg2_enabled        = false;
bool   xpb_groupagg2_bitmap_enabled  = false;
bool   xpb_groupagg2_elastic_enabled = false;
int    xpb_groupagg2_elastic_cap     = 4096;  /* rows per batch, ~128KB for 3 cols */
bool   xpb_groupagg2_simd_enabled     = false;
bool   xpb_groupagg2_local_partial    = false;
int    xpb_groupagg2_local_hash_cap   = 2048;  /* per-batch local hash size (L1-target) */

/*
 * Cost floor factors (per @yoda discipline):
 * Each represents guaranteed saving vs generic path.
 * Conservative: only claim what is ALWAYS true in envelope.
 * Runtime wins (STREAM/Class1/bitmap) are NOT included here.
 */
/* Cost factors defined in xpb_cost.c (see xpb_cost.h) */


/* =========================================================
 * Composite group entry
 * ========================================================= */
typedef struct CGroupEntry
{
    int64   k1;           /* primary key (int4 or int8) */
    int64   k2;           /* secondary key (int4 or int8) */
    int64   sum_val;      /* sum(agg_col) */
    int64   count;        /* count(*) */
    bool    occupied;
} CGroupEntry;

/* =========================================================
 * State
 * ========================================================= */
typedef struct XpGroupAgg2State
{
    CustomScanState css;

    Relation         rel;
    BlockNumber      nblocks;
    Snapshot         snapshot;
    Buffer           vmbuf;

    AttrNumber       k1_attno;    /* primary GROUP BY col */
    AttrNumber       k2_attno;    /* secondary GROUP BY col */
    AttrNumber       agg_attno;   /* aggregate col (0 = count(*)) */
    const XpbTypeOps *k1ops;   /* type descriptor for GROUP BY key 1 */
    const XpbTypeOps *k2ops;   /* type descriptor for GROUP BY key 2 */
    const XpbTypeOps *vagg_ops; /* type descriptor for aggregated column */
    Oid              agg_outtype; /* output type of agg: INT8OID or NUMERICOID */

    /* BatchProperties — composite */
    BatchProperties  batch_props;
    bool             use_streaming;

    /* output buffer */
    CGroupEntry     *output_buf;
    int64            output_ngroups;
    int64            output_pos;
    bool             computed;

    /* Pushed qual fragment (int4 predicates, AND-combined) */
    XpbQual          pqfrag;   /* unified qual engine */
    int64            pages_early_exit;    /* pages skipped by STREAM range exit */
    bool             use_bitmap;          /* Phase 2: LiveBitmap path */
    /* Cost floor components (for EXPLAIN observability) */
    double           cost_c1_scan;
    double           cost_c2_group;
    double           cost_c3_output;
    double           cost_c4_qual;

    /* stats for EXPLAIN */
    int64            pages_scanned;
    int64            pages_rejected;       /* page-level pruning */
    int64            tuples_visited;       /* all tuples seen (before qual) */
    int64            tuples_passed_qual;   /* tuples after tuple-level qual */
    int64            tuples_processed;     /* legacy: tuples fed to agg */
    int64            group_transitions;
    const char      *agg_path_used;
    int64            hash_cap_used;

    /* LOCAL_PARTIAL metrics */
    int64            lp_input_tuples;
    int64            lp_partials_emitted;
    int64            lp_batches;
    int64            lp_max_groups_in_batch;
    const char      *grouping_scope;  /* NONE / STREAM_FINAL / LOCAL_PARTIAL */

    /* ZLFS zone (if available) */
    bool             zlfs_used;
    int64            zlfs_zone_rows;
    double           zlfs_scan_ms;

} XpGroupAgg2State;

/* cgrp_getattr removed: use k1ops->getattr_fn(htup, attno, tupdesc) */


/* =========================================================
 * LiveBitmap — per-page bitmap of tuples passing pushed qual
 * 256 tuples max per page (8192 / ~32 bytes/row)
 * ========================================================= */
#define XPB_PAGE_TUPLES_MAX 256
#define XPB_BM_WORDS  ((XPB_PAGE_TUPLES_MAX + 63) / 64)  /* = 4 */

#ifndef XPB_SIMD_H  /* LiveBitmap also defined in xpb_simd.h */
typedef struct LiveBitmap {
    uint64  words[XPB_BM_WORDS];
    int     ntuples;
    int     nsurvivors;
} LiveBitmap;
#endif

static inline void lbm_set_all(LiveBitmap *bm, int n)
{
    bm->ntuples = n;
    bm->nsurvivors = n;
    int full = n / 64, rem = n % 64;
    for (int i = 0; i < full; i++) bm->words[i] = ~(uint64)0;
    if (rem) bm->words[full] = ((uint64)1 << rem) - 1;
    for (int i = full + (rem?1:0); i < XPB_BM_WORDS; i++) bm->words[i] = 0;
}
static inline void lbm_clear_bit(LiveBitmap *bm, int i)
{ bm->words[i/64] &= ~((uint64)1 << (i%64)); }
static inline bool lbm_test(const LiveBitmap *bm, int i)
{ return (bm->words[i/64] >> (i%64)) & 1; }

/* Apply one predicate to a page, updating bitmap in place.
 * Uses ItemId array to iterate actual heap tuples on page.
 */
static int
lbm_apply_pred_page(LiveBitmap *bm, const PushedPred *pred,
                    Page page, TupleDesc tupdesc, int nslots,
                    OffsetNumber *slots,
                    const XpbTypeOps *ops)  /* type for getattr */
{
    int survivors = 0;
    int64 rhs = pred->rhs;
    for (int ti = 0; ti < nslots; ti++)
    {
        if (!lbm_test(bm, ti)) continue;
        HeapTupleHeader htup = (HeapTupleHeader)
            PageGetItem(page, PageGetItemId(page, slots[ti]));
        int64 val = ops->getattr_fn(htup, pred->attno, tupdesc);
        bool ok;
        switch (pred->op) {
            case XQO_LT: ok = val <  rhs; break;
            case XQO_LE: ok = val <= rhs; break;
            case XQO_EQ: ok = val == rhs; break;
            case XQO_GE: ok = val >= rhs; break;
            case XQO_GT: ok = val >  rhs; break;
            default:    ok = true;       break;
        }
        if (ok) survivors++;
        else    lbm_clear_bit(bm, ti);
    }
    bm->nsurvivors = survivors;
    return survivors;
}

/* pqfrag_test_tuple removed — use xpb_qual_test_tuple() from xpb_qual.c */


/* Lexicographic (k1,k2) comparison */
#define COMPOSITE_LT(pk1,pk2,ck1,ck2)  \
    ((pk1) < (ck1) || ((pk1) == (ck1) && (pk2) < (ck2)))

static int
cgroup_cmp(const void *a, const void *b)
{
    const CGroupEntry *ga = (const CGroupEntry *) a;
    const CGroupEntry *gb = (const CGroupEntry *) b;
    if (ga->k1 != gb->k1) return ga->k1 < gb->k1 ? -1 : 1;
    if (ga->k2 != gb->k2) return ga->k2 < gb->k2 ? -1 : 1;
    return 0;
}

/* =========================================================
 * xpga2_exec
 * ========================================================= */
/* read_stream callback for sequential scan */
static BlockNumber
xpga2_stream_cb(ReadStream *stream, void *cb_data, void *per_buffer_data)
{
    BlockRangeReadStreamPrivate *p = (BlockRangeReadStreamPrivate *) cb_data;
    if (p->current_blocknum >= p->last_exclusive)
        return InvalidBlockNumber;
    return p->current_blocknum++;
}

/*
 * Drain the per-batch local partial hash into the global hash and clear it.
 *
 * Called both at the end of a batch and when the local table fills mid-batch.
 * Draining and reusing is what keeps local_ht L1-resident -- its whole reason
 * to exist -- while losing nothing: growing it would defeat the purpose, and
 * dropping the row that did not fit is what this used to do. The elastic
 * buffer and ScalarBatch in the same function already work this way.
 */
static inline void
xpga2_drain_local(CGroupEntry *local_ht, int local_cap,
                  CGroupEntry *htable, int64 hash_cap, int64 *ngroups)
{
    for (int i = 0; i < local_cap; i++)
    {
        if (!local_ht[i].occupied) continue;
        uint32 h = (uint32)local_ht[i].k1 * 2654435761u
                 ^ (uint32)local_ht[i].k2 * 2246822519u;
        int64 sl = (int64)(h & (uint32)(hash_cap - 1));
        for (int64 pr = 0; pr < hash_cap; pr++)
        {
            int64 idx = (sl + pr) & (hash_cap - 1);
            CGroupEntry *ge = &htable[idx];
            if (!ge->occupied) { *ge = local_ht[i]; (*ngroups)++; break; }
            if (ge->k1 == local_ht[i].k1 && ge->k2 == local_ht[i].k2)
            {
                ge->sum_val += local_ht[i].sum_val;
                ge->count   += local_ht[i].count;
                break;
            }
        }
    }
    memset(local_ht, 0, local_cap * sizeof(CGroupEntry));
}

static TupleTableSlot *
xpga2_exec(CustomScanState *node)
{
    XpGroupAgg2State *state  = (XpGroupAgg2State *) node;
    TupleTableSlot   *result = node->ss.ps.ps_ResultTupleSlot;

    /* Emit from output buffer */
    if (state->computed)
    {
        if (state->output_pos < state->output_ngroups)
        {
            CGroupEntry *g = &state->output_buf[state->output_pos++];
            ExecClearTuple(result);
            result->tts_values[0] = state->k1ops->to_datum_fn(g->k1);
            if (state->k2_attno > 0)
            {
                result->tts_values[1] = state->k2ops->to_datum_fn(g->k2);
                int64 agg_val = state->agg_attno > 0 ? g->sum_val : g->count;
                result->tts_values[2] = (state->agg_outtype == INT8OID)
                    ? Int64GetDatum(agg_val)
                    : DirectFunctionCall1(int8_numeric, Int64GetDatum(agg_val));
                result->tts_isnull[0] = false;
                result->tts_isnull[1] = false;
                result->tts_isnull[2] = false;
                result->tts_nvalid    = 3;
            }
            else
            {
                int64 agg_val = state->agg_attno > 0 ? g->sum_val : g->count;
                result->tts_values[1] = (state->agg_outtype == INT8OID)
                    ? Int64GetDatum(agg_val)
                    : DirectFunctionCall1(int8_numeric, Int64GetDatum(agg_val));
                result->tts_isnull[0] = false;
                result->tts_isnull[1] = false;
                result->tts_nvalid    = 2;
            }
            ExecStoreVirtualTuple(result);
            return result;
        }
        return ExecClearTuple(result);
    }
    state->computed = true;

    /* ===================================================
     * Single-pass scan with STREAM detection
     * =================================================== */
    {
        Relation     rel     = state->rel;
        Snapshot     snap    = state->snapshot;
        BlockNumber  nblocks = state->nblocks;
        TupleDesc    tupdesc = RelationGetDescr(rel);
        AttrNumber   k1att   = state->k1_attno;
        AttrNumber   k2att   = state->k2_attno;
        AttrNumber   vatt    = state->agg_attno;
        Oid          relid   = RelationGetRelid(rel);

        /* Streaming state */
        int64   prev_k1 = PG_INT64_MIN, prev_k2 = PG_INT32_MIN;
        bool    is_sorted = true;

        int64   cur_k1 = PG_INT64_MIN, cur_k2 = PG_INT32_MIN;
        int64   cur_sum = 0, cur_cnt = 0;
        bool    cur_set = false;

        /* Streaming output (growable) */
        int64        s_cap   = 4096;
        CGroupEntry *s_groups = palloc(s_cap * sizeof(CGroupEntry));
        int64        s_n      = 0;

        /* Hash fallback */
        int64        hash_cap = 1024; /* resized below from estimate */
        CGroupEntry *htable   = NULL;
        int64        ngroups  = 0;

        /*
         * Optimistic probe: is page 0 internally non-decreasing?
         *
         * This is a HASH PRE-ALLOCATION HINT AND NOTHING ELSE. It used to set
         * `is_sorted`, which in turn licensed whole-scan page skipping — one
         * page standing in for the relation. That was R1-11; see the note below
         * where the Class 1 bounds used to be computed.
         *
         * Being wrong here costs an allocation, never a row: when order is
         * violated later in the scan, `is_sorted` is cleared, the open group and
         * everything already streamed are migrated into the hash, and every
         * subsequent row is hashed. Note also that a page of EQUAL keys passes
         * this probe, which is how descending data was once reported ordered.
         */
        {
            int64 p_k1 = PG_INT64_MIN, p_k2 = PG_INT32_MIN;
            bool  p_ok = true;
            if (nblocks > 0)
            {
                Buffer  pb = ReadBufferExtended(rel, MAIN_FORKNUM, 0, RBM_NORMAL, NULL);
                LockBuffer(pb, BUFFER_LOCK_SHARE);
                Page    pp = BufferGetPage(pb);
                OffsetNumber pmx = PageGetMaxOffsetNumber(pp);
                for (OffsetNumber po = FirstOffsetNumber; po <= pmx; po++)
                {
                    ItemId plp = PageGetItemId(pp, po);
                    if (!ItemIdIsNormal(plp)) continue;
                    HeapTupleHeader phtup = (HeapTupleHeader) PageGetItem(pp, plp);
                    int64 pk1 = state->k1ops->getattr_fn(phtup, k1att, tupdesc);
                    int64 pk2 = (k2att > 0) ? state->k2ops->getattr_fn(phtup, k2att, tupdesc) : 0;
                    if (COMPOSITE_LT(pk1, pk2, p_k1, p_k2)) { p_ok = false; break; }
                    p_k1 = pk1; p_k2 = pk2;
                }
                LockBuffer(pb, BUFFER_LOCK_UNLOCK);
                ReleaseBuffer(pb);
                state->pages_scanned++;
            }
            if (!p_ok)
            {
                is_sorted = false;
                htable = palloc0(state->hash_cap_used * sizeof(CGroupEntry));
                hash_cap = state->hash_cap_used;
            }
        }

        /*
         * Class 1 STREAM scan boundary — REMOVED (R1-11).
         *
         * There was a tail exit (stop the scan when a page's leading key
         * exceeds the predicate's upper bound) and a prefix skip (skip a page
         * whose first and last keys are both below the lower bound). Both were
         * licensed by `is_sorted`, which at that point in the function had been
         * inferred from the ordering of PAGE 0 ALONE. A single ordered page
         * does not make the relation ordered, and both rules require a
         * whole-relation ordering invariant that nothing establishes.
         *
         * Measured consequences, both silent and both with the applicability
         * gate accepting naturally:
         *
         *   descending data (correlation -1.0), k1 <= 500
         *       page 0 holds the maximum key and is internally all-equal, so
         *       the page-0 probe reported "ordered"; the tail exit then fired
         *       on page 0 and the scan ended having visited 0 tuples.
         *       501 groups / sum 100200 became 0 groups.
         *
         *   nearly sorted data (correlation 0.9329), k1 <= 500
         *       501 groups / sum 100200 became 501 groups / sum 93430 --
         *       correct group count, aggregates short by 6.8%.
         *
         * Neither rule is replaced with another threshold or another statistic.
         * Until a real physical-order invariant exists, this node scans.
         *
         * Note for anyone restoring a page skip here: the per-page bounds used
         * by all three former skips were taken from the FIRST and LAST tuple on
         * the page, never a true min/max, so they were unsound on an unordered
         * page even per page. A sound page skip needs real per-page bounds.
         */

#ifdef XPB_DENSE_PROBE
        /*
         * ScalarBatch carrier probe.
         *
         * Route: scan → ScalarBatch → hash aggregate
         *
         * Two timed phases:
         *   FILL:  scan heap pages, extract k1/k2/val into ScalarBatch
         *   AGG:   hash aggregate over ScalarBatch columns
         *
         * ScalarBatch is flushed to consumer every SB_CAPACITY rows.
         * Consumer operates on dense int64[] — no tuple deform, no slots.
         */
        if (!is_sorted)  /* only on hash fallback */
        {
            #define SB_CAPACITY 65536
            ScalarBatch *sb = sb_create(SB_CAPACITY, 3); /* k1, k2, val */

            instr_time t0, t1;
            double fill_ms = 0, agg_ms = 0;

            /* ensure hash table exists */
            if (!htable)
            {
                htable = palloc0(state->hash_cap_used * sizeof(CGroupEntry));
                hash_cap = state->hash_cap_used;
            }

            /*
             * LOCAL_PARTIAL: per-batch local hash for L1-cache-friendly
             * partial aggregation. Each ScalarBatch is first aggregated
             * into a small local hash, then merged into the global hash.
             * This reduces global hash probes from SB_CAPACITY to ~ngroups.
             */
            bool use_local_partial = xpb_groupagg2_local_partial;
            int  local_cap = xpb_groupagg2_local_hash_cap;
            CGroupEntry *local_ht = NULL;

            if (use_local_partial)
            {
                /* Round up to power of 2 */
                int lc = 256;
                while (lc < local_cap) lc <<= 1;
                local_cap = lc;
                local_ht = palloc0(local_cap * sizeof(CGroupEntry));
                state->grouping_scope = "LOCAL_PARTIAL";
            }
            else
                state->grouping_scope = "NONE";

            INSTR_TIME_SET_CURRENT(t0);

            /* ── ZLFS zone check: use analytical projection if available ── */
            {
                int32 zlo = state->pqfrag.has_key_lo ? (int32)state->pqfrag.stream_key_lo : PG_INT32_MIN;
                int32 zhi = state->pqfrag.has_key_hi ? (int32)state->pqfrag.stream_key_hi : PG_INT32_MAX;
                ZlfsZone *zz = zlfs_lookup_valid_zone(RelationGetRelid(rel), zlo, zhi);
                if (zz) {
                    /* ZLFS path: scan zone columns directly into hash table */
                    int32 *col_pk = zz->cols[0];
                    int32 *col_ck = zz->cols[1];
                    int32 *col_dt = zz->cols[2];

                    for (int64 i = 0; i < zz->nrows; i++) {
                        int32 pk = col_pk[i], ck = col_ck[i];
                        int64 dt = (int64)col_dt[i];
                        uint32 h = (uint32)pk * 2654435761u ^ (uint32)ck * 2246822519u;
                        int sl = (int)(h & (hash_cap - 1));
                        for (int pr = 0; pr < hash_cap; pr++) {
                            int idx = (sl + pr) & (hash_cap - 1);
                            CGroupEntry *g = &htable[idx];
                            if (!g->occupied) {
                                g->k1 = pk; g->k2 = ck; g->sum_val = dt;
                                g->occupied = true; ngroups++; break;
                            }
                            if (g->k1 == pk && g->k2 == ck) { g->sum_val += dt; break; }
                        }
                    }

                    INSTR_TIME_SET_CURRENT(t1);
                    double total_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

                    state->zlfs_used = true;
                    state->zlfs_zone_rows = zz->nrows;
                    state->zlfs_scan_ms = total_ms;
                    state->output_ngroups = ngroups;
                    state->pages_scanned = 0;
                    state->tuples_visited = zz->nrows;
                    state->tuples_passed_qual = zz->nrows;
                    state->agg_path_used = "ZLFS zone scan (columnar)";

                    elog(NOTICE, "ZLFS: scan=%.1f ms  zone=[%d..%d]  rows=%ld  groups=%ld",
                         total_ms, zz->pred_lo, zz->pred_hi, zz->nrows, ngroups);

                    sb_free(sb);
                    goto xpga2_output;
                }
            }
            /* ── End ZLFS check, fall through to heap scan ── */

            /* has_page_reject / k1_off removed with the page reject (R1-11). */
            state->pages_rejected = 0;

            for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
            {
                Buffer buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno,
                                                RBM_NORMAL, NULL);
                LockBuffer(buf, BUFFER_LOCK_SHARE);
                Page page = BufferGetPage(buf);
                bool av = (visibilitymap_get_status(rel, blkno, &state->vmbuf)
                           & VISIBILITYMAP_ALL_VISIBLE) != 0;
                OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
                Oid  rel_oid = RelationGetRelid(rel);

                /*
                 * Page-level quick reject removed (R1-11). It sampled the first
                 * and last tuple on the page and treated them as the page's
                 * min/max; on a page that is not internally ordered a middle
                 * tuple can lie outside that interval, so "page definitely
                 * outside range" was not definite. Its own comment called it a
                 * heuristic for locally clustered data -- a heuristic may
                 * choose not to prune, it may not choose to drop rows.
                 *
                 * A sound version needs real per-page bounds, which means
                 * visiting every tuple on the page. That is a separate
                 * decision, not part of this repair.
                 */

                for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
                {
                    ItemId lp = PageGetItemId(page, off);
                    if (!ItemIdIsNormal(lp)) continue;
                    HeapTupleHeader htup = (HeapTupleHeader)
                        PageGetItem(page, lp);

                    if (!av)
                    {
                        HeapTupleData td;
                        td.t_data = htup;
                        td.t_len = ItemIdGetLength(lp);
                        td.t_tableOid = rel_oid;
                        ItemPointerSet(&td.t_self, blkno, off);
                        if (!HeapTupleSatisfiesVisibility(&td, snap, buf))
                            continue;
                    }

                    int r = sb->nrows;
                    /* Pushed qual check — same as elastic path */
                    if (state->pqfrag.npreds > 0 &&
                        !xpb_qual_test_tuple(&state->pqfrag, htup, tupdesc))
                    {
                        state->tuples_visited++;
                        continue;
                    }
                    state->tuples_passed_qual++;
                    state->tuples_visited++;

                    sb->cols[0][r] = state->k1ops->getattr_fn(htup, k1att, tupdesc);
                    sb->cols[1][r] = (k2att > 0) ? state->k2ops->getattr_fn(htup, k2att, tupdesc) : 0;
                    sb->cols[2][r] = (vatt > 0)
                        ? state->vagg_ops->getattr_fn(htup, vatt, tupdesc) : 0;
                    sb->nrows++;

                    if (sb->nrows >= SB_CAPACITY)
                    {
                        /* === Handoff: end fill, start agg === */
                        INSTR_TIME_SET_CURRENT(t1);
                        fill_ms += INSTR_TIME_GET_MILLISEC(t1)
                                 - INSTR_TIME_GET_MILLISEC(t0);

                        INSTR_TIME_SET_CURRENT(t0);

                        int64 *ck1 = sb->cols[0];
                        int64 *ck2 = sb->cols[1];
                        int64 *cv  = sb->cols[2];

                        if (use_local_partial)
                        {
                            /* ── LOCAL_PARTIAL: batch → local hash → merge → global ── */
                            memset(local_ht, 0, local_cap * sizeof(CGroupEntry));
                            int local_ngroups = 0;

                            /* Step 1: aggregate into local hash (L1-resident) */
                            for (int i = 0; i < sb->nrows; i++)
                            {
                                uint32 h = (uint32)ck1[i] * 2654435761u
                                         ^ (uint32)ck2[i] * 2246822519u;
                                int64 sl = (int64)(h & (uint32)(local_cap - 1));
                                bool placed = false;
                                for (int attempt = 0; attempt < 2 && !placed; attempt++)
                                {
                                    for (int pr = 0; pr < local_cap; pr++)
                                    {
                                        int idx = (int)((sl + pr) & (local_cap - 1));
                                        CGroupEntry *e = &local_ht[idx];
                                        if (!e->occupied) {
                                            e->k1 = ck1[i]; e->k2 = ck2[i];
                                            e->sum_val = cv[i]; e->count = 1;
                                            e->occupied = true; local_ngroups++;
                                            placed = true; break;
                                        }
                                        if (e->k1 == ck1[i] && e->k2 == ck2[i]) {
                                            e->sum_val += cv[i]; e->count++;
                                            placed = true; break;
                                        }
                                    }
                                    /* local table full: drain to global, then retry once */
                                    if (!placed)
                                    {
                                        xpga2_drain_local(local_ht, local_cap,
                                                          htable, hash_cap, &ngroups);
                                        state->lp_partials_emitted += local_ngroups;
                                        local_ngroups = 0;
                                    }
                                }
                            }

                            /* Step 2: merge local hash → global hash */
                            xpga2_drain_local(local_ht, local_cap,
                                              htable, hash_cap, &ngroups);

                            /*
                             * Metrics. With mid-batch draining, local_ngroups
                             * counts groups since the last drain, not since the
                             * start of the batch; lp_max_groups_in_batch is
                             * bounded by local_cap by construction.
                             */
                            state->lp_input_tuples += sb->nrows;
                            state->lp_partials_emitted += local_ngroups;
                            state->lp_batches++;
                            if (local_ngroups > state->lp_max_groups_in_batch)
                                state->lp_max_groups_in_batch = local_ngroups;
                        }
                        else
                        {
                            /* ── Direct path: batch rows → global hash ── */
                            for (int i = 0; i < sb->nrows; i++)
                            {
                                uint32 h = (uint32)ck1[i] * 2654435761u
                                         ^ (uint32)ck2[i] * 2246822519u;
                                int64 sl = (int64)(h & (uint32)(hash_cap - 1));
                                for (int64 pr = 0; pr < hash_cap; pr++)
                                {
                                    int64 idx = (sl + pr) & (hash_cap - 1);
                                    CGroupEntry *e = &htable[idx];
                                    if (!e->occupied) {
                                        e->k1 = ck1[i]; e->k2 = ck2[i];
                                        e->sum_val = cv[i]; e->count = 1;
                                        e->occupied = true; ngroups++;
                                        break;
                                    }
                                    if (e->k1 == ck1[i] && e->k2 == ck2[i]) {
                                        e->sum_val += cv[i]; e->count++;
                                        break;
                                    }
                                }
                            }
                        }

                        INSTR_TIME_SET_CURRENT(t1);
                        agg_ms += INSTR_TIME_GET_MILLISEC(t1)
                                - INSTR_TIME_GET_MILLISEC(t0);

                        sb_reset(sb);
                        INSTR_TIME_SET_CURRENT(t0);
                    }
                }

                state->pages_scanned++;
                LockBuffer(buf, BUFFER_LOCK_UNLOCK);
                ReleaseBuffer(buf);
            }

            /* Flush remaining */
            if (sb->nrows > 0)
            {
                INSTR_TIME_SET_CURRENT(t1);
                fill_ms += INSTR_TIME_GET_MILLISEC(t1)
                         - INSTR_TIME_GET_MILLISEC(t0);

                INSTR_TIME_SET_CURRENT(t0);
                int64 *ck1 = sb->cols[0];
                int64 *ck2 = sb->cols[1];
                int64 *cv  = sb->cols[2];

                if (use_local_partial)
                {
                    memset(local_ht, 0, local_cap * sizeof(CGroupEntry));
                    int local_ngroups = 0;
                    for (int i = 0; i < sb->nrows; i++)
                    {
                        uint32 h = (uint32)ck1[i] * 2654435761u
                                 ^ (uint32)ck2[i] * 2246822519u;
                        int64 sl = (int64)(h & (uint32)(local_cap - 1));
                        bool placed = false;
                        for (int attempt = 0; attempt < 2 && !placed; attempt++)
                        {
                            for (int pr = 0; pr < local_cap; pr++)
                            {
                                int idx = (int)((sl + pr) & (local_cap - 1));
                                CGroupEntry *e = &local_ht[idx];
                                if (!e->occupied) {
                                    e->k1 = ck1[i]; e->k2 = ck2[i];
                                    e->sum_val = cv[i]; e->count = 1;
                                    e->occupied = true; local_ngroups++;
                                    placed = true; break;
                                }
                                if (e->k1 == ck1[i] && e->k2 == ck2[i]) {
                                    e->sum_val += cv[i]; e->count++;
                                    placed = true; break;
                                }
                            }
                            if (!placed)
                            {
                                xpga2_drain_local(local_ht, local_cap,
                                                  htable, hash_cap, &ngroups);
                                state->lp_partials_emitted += local_ngroups;
                                local_ngroups = 0;
                            }
                        }
                    }
                    xpga2_drain_local(local_ht, local_cap,
                                      htable, hash_cap, &ngroups);
                    state->lp_input_tuples += sb->nrows;
                    state->lp_partials_emitted += local_ngroups;
                    state->lp_batches++;
                }
                else
                {
                    for (int i = 0; i < sb->nrows; i++)
                    {
                        uint32 h = (uint32)ck1[i] * 2654435761u
                                 ^ (uint32)ck2[i] * 2246822519u;
                        int64 sl = (int64)(h & (uint32)(hash_cap - 1));
                        for (int64 pr = 0; pr < hash_cap; pr++)
                        {
                            int64 idx = (sl + pr) & (hash_cap - 1);
                            CGroupEntry *e = &htable[idx];
                            if (!e->occupied) {
                                e->k1 = ck1[i]; e->k2 = ck2[i];
                                e->sum_val = cv[i]; e->count = 1;
                                e->occupied = true; ngroups++;
                                break;
                            }
                            if (e->k1 == ck1[i] && e->k2 == ck2[i]) {
                                e->sum_val += cv[i]; e->count++;
                                break;
                            }
                        }
                    }
                }

                INSTR_TIME_SET_CURRENT(t1);
                agg_ms += INSTR_TIME_GET_MILLISEC(t1)
                        - INSTR_TIME_GET_MILLISEC(t0);
            }

            if (local_ht) pfree(local_ht);

            /* tuples_processed and tuples_passed_qual tracked per-tuple in fill loop */
            if (use_local_partial)
            {
                state->agg_path_used = "ScalarBatch + LOCAL_PARTIAL";
                elog(NOTICE,
                     "LOCAL_PARTIAL: fill=%.0f ms  agg=%.0f ms  total=%.0f ms  "
                     "groups=%ld  batches=%ld  input=%ld  partials=%ld  "
                     "reduction=%.1f×  state->pages_rejected=%ld",
                     fill_ms, agg_ms, fill_ms + agg_ms, ngroups,
                     state->lp_batches, state->lp_input_tuples,
                     state->lp_partials_emitted,
                     state->lp_input_tuples > 0
                        ? (double)state->lp_input_tuples / state->lp_partials_emitted
                        : 0.0,
                     state->pages_rejected);
            }
            else
            {
                state->agg_path_used = "ScalarBatch carrier (hash)";
                elog(NOTICE,
                     "ScalarBatch: fill=%.0f ms  agg=%.0f ms  total=%.0f ms  "
                     "groups=%ld  state->pages_rejected=%ld  batch_cap=%d",
                     fill_ms, agg_ms, fill_ms + agg_ms, ngroups,
                     state->pages_rejected, SB_CAPACITY);
            }

            sb_free(sb);
            goto xpga2_output;
            #undef SB_CAPACITY
        }
#endif /* XPB_DENSE_PROBE */

        /* ═══════════════════════════════════════════════════════
         * Elastic Batch path: accumulate selected rows across pages
         * into ScalarBatch, flush to consumer when batch full.
         *
         * Flow: page → vis → extract → qual bitmap → selected → ScalarBatch
         *       → when full: dense consumer loop (STREAM or hash)
         * ═══════════════════════════════════════════════════════ */
        if (xpb_groupagg2_elastic_enabled)
        {
            int eb_cap = xpb_groupagg2_elastic_cap;
            int64 *eb_k1  = palloc(eb_cap * sizeof(int64));
            int64 *eb_k2  = palloc(eb_cap * sizeof(int64));
            int64 *eb_val = palloc(eb_cap * sizeof(int64));
            int    eb_n   = 0;

            BlockRangeReadStreamPrivate eb_rs_cb = {0, nblocks};
            ReadStream *eb_rs = read_stream_begin_relation(
                READ_STREAM_SEQUENTIAL | READ_STREAM_FULL,
                NULL, rel, MAIN_FORKNUM,
                xpga2_stream_cb, &eb_rs_cb, 0);

            Buffer eb_buf;
            while ((eb_buf = read_stream_next_buffer(eb_rs, NULL)) != InvalidBuffer)
            {
                Page    eb_page = BufferGetPage(eb_buf);
                BlockNumber eb_blkno = BufferGetBlockNumber(eb_buf);
                bool    eb_av = (visibilitymap_get_status(rel, eb_blkno, &state->vmbuf)
                                 & VISIBILITYMAP_ALL_VISIBLE) != 0;
                OffsetNumber eb_maxoff = PageGetMaxOffsetNumber(eb_page);

                /*
                 * Class 1 tail exit and prefix skip removed here (R1-11); see
                 * the note where the bounds used to be computed. Every page is
                 * read. The predicate is still applied per tuple below.
                 */

                /* Extract surviving tuples into elastic batch */
                for (OffsetNumber off = FirstOffsetNumber; off <= eb_maxoff; off++)
                {
                    ItemId lp = PageGetItemId(eb_page, off);
                    if (!ItemIdIsNormal(lp)) continue;
                    HeapTupleHeader htup = (HeapTupleHeader) PageGetItem(eb_page, lp);

                    if (!eb_av) {
                        HeapTupleData td;
                        td.t_data = htup; td.t_len = ItemIdGetLength(lp);
                        td.t_tableOid = relid;
                        ItemPointerSet(&td.t_self, eb_blkno, off);
                        if (!HeapTupleSatisfiesVisibility(&td, snap, eb_buf))
                            continue;
                    }

                    state->tuples_visited++;

                    /* Qual check */
                    if (state->pqfrag.npreds > 0 &&
                        !xpb_qual_test_tuple(&state->pqfrag, htup, tupdesc))
                        continue;
                    state->tuples_passed_qual++;

                    /* Accumulate into elastic batch */
                    eb_k1[eb_n]  = state->k1ops->getattr_fn(htup, k1att, tupdesc);
                    eb_k2[eb_n]  = (k2att > 0) ? state->k2ops->getattr_fn(htup, k2att, tupdesc) : 0;
                    eb_val[eb_n] = (vatt > 0) ? state->vagg_ops->getattr_fn(htup, vatt, tupdesc) : 0;
                    eb_n++;

                    /* Flush when batch full */
                    if (eb_n >= eb_cap)
                    {
                        /* Dense consumer loop over batch */
                        for (int bi = 0; bi < eb_n; bi++)
                        {
                            int64 k1 = eb_k1[bi], k2 = eb_k2[bi], val = eb_val[bi];

                            if (is_sorted && COMPOSITE_LT(k1, k2, prev_k1, prev_k2))
                            {
                                is_sorted = false;
                                if (!htable) {
                                    htable = palloc0(state->hash_cap_used * sizeof(CGroupEntry));
                                    hash_cap = state->hash_cap_used;
                                }
                                if (cur_set) {
                                    if (s_n >= s_cap) { s_cap *= 2; s_groups = repalloc(s_groups, s_cap * sizeof(CGroupEntry)); }
                                    s_groups[s_n].k1=cur_k1; s_groups[s_n].k2=cur_k2;
                                    s_groups[s_n].sum_val=cur_sum; s_groups[s_n].count=cur_cnt;
                                    s_groups[s_n].occupied=true; s_n++; cur_set=false;
                                }
                                for (int64 mi = 0; mi < s_n; mi++) {
                                    uint32 h = (uint32)s_groups[mi].k1 * 2654435761u ^ (uint32)s_groups[mi].k2 * 2246822519u;
                                    int64 sl = (int64)(h & (uint32)(hash_cap-1));
                                    for (int64 mp = 0; mp < hash_cap; mp++) {
                                        int64 idx = (sl+mp) & (hash_cap-1);
                                        if (!htable[idx].occupied) { htable[idx] = s_groups[mi]; ngroups++; break; }
                                        if (htable[idx].k1==s_groups[mi].k1 && htable[idx].k2==s_groups[mi].k2) {
                                            htable[idx].sum_val+=s_groups[mi].sum_val; htable[idx].count+=s_groups[mi].count; break; }
                                    }
                                }
                                s_n = 0;
                            }
                            prev_k1 = k1; prev_k2 = k2;

                            if (is_sorted) {
                                bool same = cur_set && cur_k1 == k1 && cur_k2 == k2;
                                if (!same) {
                                    if (cur_set) {
                                        if (s_n >= s_cap) { s_cap *= 2; s_groups = repalloc(s_groups, s_cap * sizeof(CGroupEntry)); }
                                        s_groups[s_n].k1=cur_k1; s_groups[s_n].k2=cur_k2;
                                        s_groups[s_n].sum_val=cur_sum; s_groups[s_n].count=cur_cnt;
                                        s_groups[s_n].occupied=true; s_n++; state->group_transitions++;
                                    }
                                    cur_k1=k1; cur_k2=k2; cur_sum=val; cur_cnt=1; cur_set=true;
                                } else { cur_sum += val; cur_cnt++; }
                            } else {
                                uint32 h = (uint32)k1 * 2654435761u ^ (uint32)k2 * 2246822519u;
                                int64 sl = (int64)(h & (uint32)(hash_cap-1));
                                for (int64 pr = 0; pr < hash_cap; pr++) {
                                    int64 idx = (sl+pr) & (hash_cap-1);
                                    if (!htable[idx].occupied) { htable[idx].k1=k1; htable[idx].k2=k2; htable[idx].sum_val=val; htable[idx].count=1; htable[idx].occupied=true; ngroups++; break; }
                                    if (htable[idx].k1==k1 && htable[idx].k2==k2) { htable[idx].sum_val+=val; htable[idx].count++; break; }
                                }
                            }
                        }
                        eb_n = 0;
                    }
                }

                ReleaseBuffer(eb_buf);
                state->pages_scanned++;
            }

            /* Flush remaining rows in batch */
            for (int bi = 0; bi < eb_n; bi++)
            {
                int64 k1 = eb_k1[bi], k2 = eb_k2[bi], val = eb_val[bi];
                if (is_sorted && COMPOSITE_LT(k1, k2, prev_k1, prev_k2)) {
                    is_sorted = false;
                    if (!htable) { htable = palloc0(state->hash_cap_used * sizeof(CGroupEntry)); hash_cap = state->hash_cap_used; }
                    if (cur_set) {
                        if (s_n >= s_cap) { s_cap *= 2; s_groups = repalloc(s_groups, s_cap * sizeof(CGroupEntry)); }
                        s_groups[s_n].k1=cur_k1; s_groups[s_n].k2=cur_k2; s_groups[s_n].sum_val=cur_sum; s_groups[s_n].count=cur_cnt; s_groups[s_n].occupied=true; s_n++; cur_set=false;
                    }
                    for (int64 mi = 0; mi < s_n; mi++) {
                        uint32 h = (uint32)s_groups[mi].k1 * 2654435761u ^ (uint32)s_groups[mi].k2 * 2246822519u;
                        int64 sl = (int64)(h & (uint32)(hash_cap-1));
                        for (int64 mp = 0; mp < hash_cap; mp++) {
                            int64 idx = (sl+mp) & (hash_cap-1);
                            if (!htable[idx].occupied) { htable[idx] = s_groups[mi]; ngroups++; break; }
                            if (htable[idx].k1==s_groups[mi].k1 && htable[idx].k2==s_groups[mi].k2) { htable[idx].sum_val+=s_groups[mi].sum_val; htable[idx].count+=s_groups[mi].count; break; }
                        }
                    }
                    s_n = 0;
                }
                prev_k1 = k1; prev_k2 = k2;
                if (is_sorted) {
                    bool same = cur_set && cur_k1 == k1 && cur_k2 == k2;
                    if (!same) {
                        if (cur_set) { if (s_n >= s_cap) { s_cap *= 2; s_groups = repalloc(s_groups, s_cap * sizeof(CGroupEntry)); } s_groups[s_n].k1=cur_k1; s_groups[s_n].k2=cur_k2; s_groups[s_n].sum_val=cur_sum; s_groups[s_n].count=cur_cnt; s_groups[s_n].occupied=true; s_n++; state->group_transitions++; }
                        cur_k1=k1; cur_k2=k2; cur_sum=val; cur_cnt=1; cur_set=true;
                    } else { cur_sum += val; cur_cnt++; }
                } else {
                    uint32 h = (uint32)k1 * 2654435761u ^ (uint32)k2 * 2246822519u;
                    int64 sl = (int64)(h & (uint32)(hash_cap-1));
                    for (int64 pr = 0; pr < hash_cap; pr++) {
                        int64 idx = (sl+pr) & (hash_cap-1);
                        if (!htable[idx].occupied) { htable[idx].k1=k1; htable[idx].k2=k2; htable[idx].sum_val=val; htable[idx].count=1; htable[idx].occupied=true; ngroups++; break; }
                        if (htable[idx].k1==k1 && htable[idx].k2==k2) { htable[idx].sum_val+=val; htable[idx].count++; break; }
                    }
                }
            }

            pfree(eb_k1); pfree(eb_k2); pfree(eb_val);
            read_stream_end(eb_rs);
            state->agg_path_used = is_sorted
                ? "ElasticBatch+STREAM" : "ElasticBatch+Hash";
            goto elastic_done;
        }

        /* Main scan loop — read_stream substrate */
        BlockRangeReadStreamPrivate rs_cb = {0, nblocks};
        ReadStream *rs = read_stream_begin_relation(
            READ_STREAM_SEQUENTIAL | READ_STREAM_FULL,
            NULL, rel, MAIN_FORKNUM,
            xpga2_stream_cb, &rs_cb, 0);

        Buffer buf;
        while ((buf = read_stream_next_buffer(rs, NULL)) != InvalidBuffer)
        {
            Page    page;
            bool    av;
            BlockNumber blkno = BufferGetBlockNumber(buf);

            av  = (visibilitymap_get_status(rel, blkno, &state->vmbuf)
                   & VISIBILITYMAP_ALL_VISIBLE) != 0;
            page = BufferGetPage(buf);

            /*
             * Class 1 tail exit and prefix skip removed here (R1-11); see the
             * note where the bounds used to be computed. Every page is read.
             */
            /* Phase 2/3: LiveBitmap path (scalar or SIMD mask build) */
            if (xpb_groupagg2_bitmap_enabled && state->pqfrag.npreds > 0)
            {
                /* Collect valid slot numbers */
                OffsetNumber pmaxoff = PageGetMaxOffsetNumber(page);
                OffsetNumber slots[XPB_PAGE_TUPLES_MAX];
                int nslots = 0;
                LiveBitmap bm;

                OffsetNumber poff;
                for (poff = FirstOffsetNumber;
                     poff <= pmaxoff && nslots < XPB_PAGE_TUPLES_MAX; poff++)
                {
                    ItemId plp = PageGetItemId(page, poff);
                    if (!ItemIdIsNormal(plp)) continue;
                    if (!av)
                    {
                        HeapTupleData tup2;
                        HeapTupleHeader ph = (HeapTupleHeader) PageGetItem(page, plp);
                        tup2.t_data = ph; tup2.t_len = ItemIdGetLength(plp);
                        tup2.t_tableOid = relid;
                        ItemPointerSet(&tup2.t_self, blkno, poff);
                        if (!HeapTupleSatisfiesVisibility(&tup2, snap, buf))
                            continue;
                    }
                    slots[nslots++] = poff;
                }

                /*
                 * The loop above stops at XPB_PAGE_TUPLES_MAX and used to leave
                 * the rest of the page unexamined -- rows silently absent from
                 * the aggregate. Whether a page can hold more than 256 visible
                 * tuples depends on BLCKSZ (226 max at 8 KB, 454 at 16 KB) and
                 * nothing in this extension constrains BLCKSZ, so it is refused
                 * rather than assumed away.
                 */
                if (poff <= pmaxoff)
                {
                    LockBuffer(buf, BUFFER_LOCK_UNLOCK);
                    ReleaseBuffer(buf);
                    ereport(ERROR,
                            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                             errmsg("XpGroupAgg2: page %u holds more than %d visible tuples",
                                    blkno, XPB_PAGE_TUPLES_MAX)));
                }

                /* Build bitmap: all nslots bits set */
                lbm_set_all(&bm, nslots);

                /* Apply predicates: SIMD or scalar mask build */
                /* BOUNDED NEGATIVE RESULT (see TRANSIENT_BATCH_EXPERIMENT.md):
 * Transient projection + SIMD does not beat scalar_interleaved.
 * Reason: projection eliminates short-circuit; extraction dominates.
 * Real SIMD gain requires persistent columnar carrier (extraction=0).
 * This branch is preserved for reference only. Not part of main path.
 */
if (false && xpb_groupagg2_simd_enabled && nslots > 0)
                {
                    /*
                     * Phase 3 SIMD path:
                     * For each predicate: extract column values into
                     * contiguous int32 array, then AVX-512 compare.
                     */
                    int64_t vals[XPB_PAGE_TUPLES_MAX];

                    for (int pi = 0; pi < state->pqfrag.npreds; pi++)
                    {
                        if (bm.nsurvivors == 0) break;
                        AttrNumber attno = state->pqfrag.preds[pi].attno;
                        /* Extract column values for all slots */
                        for (int ti = 0; ti < nslots; ti++)
                        {
                            HeapTupleHeader hh = (HeapTupleHeader)
                                PageGetItem(page, PageGetItemId(page, slots[ti]));
                            vals[ti] = state->k1ops->getattr_fn(hh, attno, tupdesc);
                        }
                        if (pi == 0)
                            lbm_build_full_mask_avx512(&bm, vals, nslots,
                                state->pqfrag.preds[pi].op,
                                state->pqfrag.preds[pi].rhs);
                        else
                            lbm_and_mask_avx512(&bm, vals, nslots,
                                state->pqfrag.preds[pi].op,
                                state->pqfrag.preds[pi].rhs);
                    }
                }
                else
                {
                    /* Phase 2 scalar mask build */
                    for (int pi = 0; pi < state->pqfrag.npreds; pi++)
                    {
                        if (bm.nsurvivors == 0) break;
                        lbm_apply_pred_page(&bm, &state->pqfrag.preds[pi],
                                            page, tupdesc, nslots, slots,
                                            state->k1ops);
                    }
                }
                state->tuples_visited += nslots;
                state->tuples_passed_qual += bm.nsurvivors;

                /* Aggregate only survivors */
                for (int ti = 0; ti < nslots && bm.nsurvivors > 0; ti++)
                {
                    if (!lbm_test(&bm, ti)) continue;
                    HeapTupleHeader htup2 = (HeapTupleHeader)
                        PageGetItem(page, PageGetItemId(page, slots[ti]));
                    int64 k1 = state->k1ops->getattr_fn(htup2, k1att, tupdesc);
                    int64 k2 = (k2att > 0) ? state->k2ops->getattr_fn(htup2, k2att, tupdesc) : 0;
                    int64 val = (vatt > 0) ? state->vagg_ops->getattr_fn(htup2, vatt, tupdesc) : 0;
                    prev_k1 = k1; prev_k2 = k2;
                    /* Streaming agg (STREAM path assumed — bitmap path
                     * only enabled when STREAM is likely) */
                    bool same = cur_set && cur_k1 == k1 && cur_k2 == k2;
                    if (!same) {
                        if (cur_set) {
                            if (s_n >= s_cap)
                            { s_cap *= 2; s_groups = repalloc(s_groups, s_cap * sizeof(CGroupEntry)); }
                            s_groups[s_n].k1=cur_k1; s_groups[s_n].k2=cur_k2;
                            s_groups[s_n].sum_val=cur_sum; s_groups[s_n].count=cur_cnt;
                            s_groups[s_n].occupied=true; s_n++;
                            state->group_transitions++;
                        }
                        cur_k1=k1; cur_k2=k2; cur_sum=val; cur_cnt=1; cur_set=true;
                    } else { cur_sum += val; cur_cnt++; }
                }

                /* Skip the tuple-by-tuple loop below for this page */
                ReleaseBuffer(buf);
                state->pages_scanned++;
                continue;  /* next page */
            }

            OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
            for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
            {
                ItemId lp = PageGetItemId(page, off);
                if (!ItemIdIsNormal(lp)) continue;
                HeapTupleHeader htup = (HeapTupleHeader) PageGetItem(page, lp);
                if (!av)
                {
                    HeapTupleData tup;
                    tup.t_data = htup; tup.t_len = ItemIdGetLength(lp);
                    tup.t_tableOid = relid;
                    ItemPointerSet(&tup.t_self, blkno, off);
                    if (!HeapTupleSatisfiesVisibility(&tup, snap, buf))
                        continue;
                }

                int64 k1  = state->k1ops->getattr_fn(htup, k1att, tupdesc);
                int64 k2  = (k2att > 0) ? state->k2ops->getattr_fn(htup, k2att, tupdesc) : 0;
                int64 val = (vatt > 0) ? state->vagg_ops->getattr_fn(htup, vatt, tupdesc) : 0;
                state->tuples_visited++;
                /* Apply pushed qual fragment */
                if (state->pqfrag.npreds > 0 &&
                    !xpb_qual_test_tuple(&state->pqfrag, htup, tupdesc))
                    continue;  /* tuple eliminated by pushed qual */
                state->tuples_passed_qual++;

                /* Check composite lexicographic order */
                if (is_sorted && COMPOSITE_LT(k1, k2, prev_k1, prev_k2))
                {
                    /*
                     * Order violated: composite STREAM revoked.
                     * Migrate streaming state to hash table.
                     */
                    is_sorted = false;
                    if (htable == NULL)
                    {
                        htable   = palloc0(state->hash_cap_used * sizeof(CGroupEntry));
                        hash_cap = state->hash_cap_used;
                    }
                    /* flush open group */
                    if (cur_set)
                    {
                        if (s_n >= s_cap)
                        { s_cap *= 2; s_groups = repalloc(s_groups, s_cap * sizeof(CGroupEntry)); }
                        s_groups[s_n].k1=cur_k1; s_groups[s_n].k2=cur_k2;
                        s_groups[s_n].sum_val=cur_sum; s_groups[s_n].count=cur_cnt;
                        s_groups[s_n].occupied=true; s_n++;
                        cur_set = false;
                    }
                    /* migrate s_groups → htable */
                    for (int64 mi = 0; mi < s_n; mi++)
                    {
                        uint32 h = (uint32)s_groups[mi].k1 * 2654435761u
                                 ^ (uint32)s_groups[mi].k2 * 2246822519u;
                        int64  slot = (int64)(h & (uint32)(hash_cap-1));
                        for (int64 mp = 0; mp < hash_cap; mp++) {
                            int64 idx = (slot+mp) & (hash_cap-1);
                            if (!htable[idx].occupied) {
                                htable[idx] = s_groups[mi]; ngroups++; break;
                            }
                            if (htable[idx].k1==s_groups[mi].k1 && htable[idx].k2==s_groups[mi].k2) {
                                htable[idx].sum_val+=s_groups[mi].sum_val;
                                htable[idx].count+=s_groups[mi].count; break;
                            }
                        }
                    }
                    s_n = 0;
                }
                prev_k1 = k1; prev_k2 = k2;

                if (is_sorted)
                {
                    /* Streaming path: flush on any composite key change */
                    bool same = cur_set && cur_k1 == k1 && cur_k2 == k2;
                    if (!same)
                    {
                        if (cur_set) /* flush previous group */
                        {
                            if (s_n >= s_cap)
                            { s_cap *= 2; s_groups = repalloc(s_groups, s_cap * sizeof(CGroupEntry)); }
                            s_groups[s_n].k1=cur_k1; s_groups[s_n].k2=cur_k2;
                            s_groups[s_n].sum_val=cur_sum; s_groups[s_n].count=cur_cnt;
                            s_groups[s_n].occupied=true; s_n++;
                            state->group_transitions++;
                        }
                        cur_k1=k1; cur_k2=k2; cur_sum=val; cur_cnt=1; cur_set=true;
                    }
                    else { cur_sum += val; cur_cnt++; }
                }
                else
                {
                    /* Hash path */
                    uint32 h = (uint32)k1 * 2654435761u ^ (uint32)k2 * 2246822519u;
                    int64  slot = (int64)(h & (uint32)(hash_cap-1));
                    for (int64 probe = 0; probe < hash_cap; probe++) {
                        int64 idx = (slot+probe) & (hash_cap-1);
                        if (!htable[idx].occupied) {
                            htable[idx].k1=k1; htable[idx].k2=k2;
                            htable[idx].sum_val=val; htable[idx].count=1;
                            htable[idx].occupied=true; ngroups++; break;
                        }
                        if (htable[idx].k1==k1 && htable[idx].k2==k2) {
                            htable[idx].sum_val+=val; htable[idx].count++; break;
                        }
                    }
                }
            }
            ReleaseBuffer(buf);
            state->pages_scanned++;
        }
        /* `scan_done` removed with its only jump, the Class 1 tail exit (R1-11).
         * The scan now ends only by exhausting the read stream. */
        read_stream_end(rs);
        elastic_done: ;

        /* Finalize */
        if (is_sorted)
        {
            /* Flush last open group */
            if (cur_set) {
                if (s_n >= s_cap)
                { s_cap *= 2; s_groups = repalloc(s_groups, s_cap * sizeof(CGroupEntry)); }
                s_groups[s_n].k1=cur_k1; s_groups[s_n].k2=cur_k2;
                s_groups[s_n].sum_val=cur_sum; s_groups[s_n].count=cur_cnt;
                s_groups[s_n].occupied=true; s_n++;
            }

            /*
             * Composite STREAM confirmed.
             * Output already in (k1,k2) lexicographic order.
             * No sort needed.
             */
            state->batch_props.sorted_by_key = true;
            state->batch_props.order_scope   = BATCH_ORDER_STREAM;
            state->batch_props.key_attno     = state->k1_attno;
            state->batch_props.order_source  = (state->k2_attno > 0)
                ? "heap scan: (k1,k2) monotonic order detected at runtime"
                : "heap scan: k1 monotonic order detected at runtime";
            state->use_streaming  = true;
            state->output_buf     = s_groups;
            state->output_ngroups = s_n;
            state->agg_path_used  = (state->k2_attno > 0)
                ? "StreamingAgg (STREAM, dual-key)"
                : "StreamingAgg (STREAM, single-key)";
        }
        else
        {
xpga2_output:   ;
            /*
             * HASH OVERFLOW GUARD: open-addressing without resize.
             * If ngroups reaches hash_cap, probe loops silently lose rows.
             * Check load factor and ERROR if overflow likely.
             *
             * v0 contract: explicit ERROR, not silent data loss.
             */
            if (ngroups > 0 && hash_cap > 0 &&
                (double)ngroups / (double)hash_cap > 0.95)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("xp_batch: XpGroupAgg2 hash table overflow — %ld groups in %ld slots (%.1f%% full)",
                                (long)ngroups, (long)hash_cap,
                                100.0 * ngroups / hash_cap),
                         errhint("Reduce the number of distinct GROUP BY keys or increase hash capacity.")));
            }

            /* Hash fallback */
            CGroupEntry *sorted = palloc(ngroups * sizeof(CGroupEntry));
            int64 si = 0;
            for (int64 i = 0; i < hash_cap && si < ngroups; i++)
                if (htable[i].occupied) sorted[si++] = htable[i];
            qsort(sorted, si, sizeof(CGroupEntry), cgroup_cmp);
            if (htable) pfree(htable);

            state->batch_props.sorted_by_key = false;
            state->batch_props.order_scope   = BATCH_ORDER_NONE;
            state->batch_props.order_source  =
                "composite order violated: hash fallback";
            state->use_streaming  = false;
            state->output_buf     = sorted;
            state->output_ngroups = si;
            if (!state->zlfs_used)
                state->agg_path_used  = "HashAgg+Sort (composite, no stream order)";
        }
        state->output_pos     = 0;
    }

    /* Emit first row */
    if (state->output_pos < state->output_ngroups)
    {
        CGroupEntry *g = &state->output_buf[state->output_pos++];
        ExecClearTuple(result);
        /* Write k1/k2 with correct Datum size to match TupleDescInitEntry */
        /* Type-driven datum formation via XpbTypeOps */
        result->tts_values[0] = state->k1ops->to_datum_fn(g->k1);
        if (state->k2_attno > 0)
        {
            result->tts_values[1] = state->k2ops->to_datum_fn(g->k2);
            int64 agg_val = state->agg_attno > 0 ? g->sum_val : g->count;
            result->tts_values[2] = (state->agg_outtype == INT8OID)
                ? Int64GetDatum(agg_val)
                : DirectFunctionCall1(int8_numeric, Int64GetDatum(agg_val));
            result->tts_isnull[0] = false;
            result->tts_isnull[1] = false;
            result->tts_isnull[2] = false;
            result->tts_nvalid    = 3;
        }
        else
        {
            int64 agg_val = state->agg_attno > 0 ? g->sum_val : g->count;
            result->tts_values[1] = (state->agg_outtype == INT8OID)
                ? Int64GetDatum(agg_val)
                : DirectFunctionCall1(int8_numeric, Int64GetDatum(agg_val));
            result->tts_isnull[0] = false;
            result->tts_isnull[1] = false;
            result->tts_nvalid    = 2;
        }
        elog(DEBUG1, "xpb: ExecStore slot=%p ops=%s natts=%d k1typid=%u datum0=%lu",
             (void*)result,
             result->tts_ops == &TTSOpsVirtual ? "Virtual" : "Other",
             result->tts_tupleDescriptor ? result->tts_tupleDescriptor->natts : -1,
             (result->tts_tupleDescriptor && result->tts_tupleDescriptor->natts > 0)
               ? TupleDescAttr(result->tts_tupleDescriptor,0)->atttypid : 0,
             (unsigned long)result->tts_values[0]);
        ExecStoreVirtualTuple(result);
        return result;
    }
    return ExecClearTuple(result);
}

/* =========================================================
 * Planner / State boilerplate
 * ========================================================= */
void xpga2_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                    RelOptInfo *output_rel, void *extra);

static Plan *xpga2_plan_path(PlannerInfo *root, RelOptInfo *rel,
                              CustomPath *best_path, List *tlist,
                              List *clauses, List *custom_plans);
static Node *xpga2_create_state(CustomScan *cscan);
static void  xpga2_begin(CustomScanState *node, EState *estate, int eflags);
static void  xpga2_end(CustomScanState *node);
static void  xpga2_rescan(CustomScanState *node);
static void  xpga2_explain(CustomScanState *node, List *ancestors, ExplainState *es);

static const CustomPathMethods xpga2_path_methods = {
    .CustomName     = "XpGroupAgg2",
    .PlanCustomPath = xpga2_plan_path,
};
static const CustomScanMethods xpga2_scan_methods = {
    .CustomName            = "XpGroupAgg2",
    .CreateCustomScanState = xpga2_create_state,
};
static const CustomExecMethods xpga2_exec_methods = {
    .CustomName        = "XpGroupAgg2",
    .BeginCustomScan   = xpga2_begin,
    .ExecCustomScan    = xpga2_exec,
    .EndCustomScan     = xpga2_end,
    .ReScanCustomScan  = xpga2_rescan,
    .ExplainCustomScan = xpga2_explain,
};

/*
 * xpga2_extract_preds: extract AND-list of simple int4 predicates from quals.
 * Returns number of predicates extracted. Non-pushable predicates set has_residual.
 */
/*
 * xpga2_extract_preds: classify predicates into three classes.
 * k1_attno: the leading STREAM key column (Class 1 target).
 * Called at path-add time; k1_attno known from GROUP BY.
 */
/* xpga2_extract_preds removed — use xpb_qual_extract() from xpb_qual.c */

void
xpga2_add_path(PlannerInfo *root, RelOptInfo *input_rel,
               RelOptInfo *output_rel, void *extra)
{
    /*
     * Applicability guard: decline early rather than add a path that cannot
     * execute correctly.  All guards fire before add_path() is called so there
     * is zero planning overhead for declined shapes.
     */
    if (!xpb_groupagg2_enabled) return;
    if (!input_rel || input_rel->relid == 0) return;

    /* Must be a single base relation (not join output) */
    {
        RangeTblEntry *rte0 = planner_rt_fetch(input_rel->relid, root);
        if (rte0->rtekind != RTE_RELATION) return;
        /* Base path must be a plain sequential scan */
        Path *base0 = input_rel->cheapest_total_path;
        if (base0->pathtype != T_SeqScan &&
            base0->pathtype != T_CustomScan) return;
    }

    /* Need 1 or 2 GROUP BY columns, int4/int8 */
    int ngroup_cols = list_length(root->parse->groupClause);
    if (ngroup_cols < 1 || ngroup_cols > 2) return;

    AttrNumber k1att = 0, k2att = 0, vatt = 0;
    Oid        k1type = INT4OID, k2type = INT4OID;
    int reloid = 0;

    ListCell *lc;
    int kidx = 0;
    foreach(lc, root->parse->groupClause)
    {
        SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
        TargetEntry *tle = get_sortgroupclause_tle(sgc, root->parse->targetList);
        if (!tle || !IsA(tle->expr, Var)) return;
        Var *v = (Var *) tle->expr;
        if (v->vartype != INT4OID && v->vartype != INT8OID) return;
        if (kidx == 0) { k1att = v->varattno; k1type = v->vartype; }
        else           { k2att = v->varattno; k2type = v->vartype; }
        kidx++;
    }

    /* Find aggregate (sum int4 or count(*)).
     * SCOPE GUARD: XpGroupAgg2 supports exactly ONE aggregate expression.
     * Decline if the tlist has more than one Aggref — otherwise the extra
     * aggregates silently return 0 (wrong results, not crash).
     */
    {
        int naggs = 0;
        foreach(lc, root->processed_tlist)
        {
            TargetEntry *te = (TargetEntry *) lfirst(lc);
            if (IsA(te->expr, Aggref))
                naggs++;
        }
        if (naggs != 1)
        {
            elog(DEBUG2, "xp_batch: XpGroupAgg2 declined — %d aggregates (only 1 supported)", naggs);
            return;
        }
    }
    foreach(lc, root->processed_tlist)
    {
        TargetEntry *te = (TargetEntry *) lfirst(lc);
        if (!IsA(te->expr, Aggref)) continue;
        Aggref *agg = (Aggref *) te->expr;
        char   *nm  = get_func_name(agg->aggfnoid);
        if (nm && strcmp(nm, "sum") == 0 && list_length(agg->args) == 1)
        {
            TargetEntry *a = (TargetEntry *) linitial(agg->args);
            if (!IsA(a->expr, Var) ||
                (((Var *)a->expr)->vartype != INT4OID &&
                 ((Var *)a->expr)->vartype != INT8OID)) return;
            vatt = ((Var *)a->expr)->varattno;
        }
        else if (nm && strcmp(nm, "count") == 0 && agg->args == NIL)
            vatt = 0;
        else return;
        break;
    }

    RangeTblEntry *rte = planner_rt_fetch(input_rel->relid, root);
    reloid = (int) rte->relid;

    /*
     * NULL SCOPE GUARD: XpGroupAgg2 treats NULL as 0 in getattr_fn.
     * This gives wrong results: NULL key grouped with 0, NULL value
     * added to sum instead of skipped. Decline for nullable columns.
     *
     * v0 contract: correct result OR explicit decline. No silent corruption.
     */
    {
        Relation rel_check = table_open((Oid) reloid, AccessShareLock);
        TupleDesc td = RelationGetDescr(rel_check);
        bool nullable = false;

        if (k1att > 0 && !TupleDescAttr(td, k1att - 1)->attnotnull)
            nullable = true;
        if (k2att > 0 && !TupleDescAttr(td, k2att - 1)->attnotnull)
            nullable = true;
        if (vatt > 0 && !TupleDescAttr(td, vatt - 1)->attnotnull)
            nullable = true;

        table_close(rel_check, AccessShareLock);

        if (nullable)
        {
            elog(DEBUG2, "xp_batch: XpGroupAgg2 declined — nullable column in key or agg (NULL semantics not supported)");
            return;
        }
    }

    /* STREAM feasibility gate.
     *
     * XpGroupAgg2 wins only when STREAM activates (k1 correlated with
     * heap order). When STREAM fails → hash fallback → slower than
     * vanilla HashAgg. Use heap correlation of k1 column as proxy.
     *
     * pg_statistic stores correlation per column. We read it via syscache.
     * Threshold: |correlation| >= 0.8 → STREAM likely → accept.
     */
    {
        float4 k1_corr = 0.0;
        HeapTuple statstuple = SearchSysCache3(STATRELATTINH,
                                                ObjectIdGetDatum(rte->relid),
                                                Int16GetDatum(k1att),
                                                BoolGetDatum(false));
        if (HeapTupleIsValid(statstuple))
        {
            Oid ltopr = get_opfamily_member(1976,
                                             INT4OID, INT4OID,
                                             BTLessStrategyNumber);
            if (!OidIsValid(ltopr) && k1type == INT8OID)
                ltopr = get_opfamily_member(1976,
                                             INT8OID, INT8OID,
                                             BTLessStrategyNumber);
            AttStatsSlot sslot;
            if (OidIsValid(ltopr) &&
                get_attstatsslot(&sslot, statstuple,
                                 STATISTIC_KIND_CORRELATION, ltopr,
                                 ATTSTATSSLOT_NUMBERS) &&
                sslot.nnumbers >= 1)
            {
                k1_corr = sslot.numbers[0];
                free_attstatsslot(&sslot);
            }
            ReleaseSysCache(statstuple);
        }

        if (fabs(k1_corr) < 0.8)
        {
            elog(DEBUG2, "xp_batch: XpGroupAgg2 declined — k1 attno=%d correlation=%.3f < 0.8, STREAM unlikely",
                 k1att, k1_corr);
            return;
        }
        elog(DEBUG2, "xp_batch: k1 attno=%d correlation=%.3f — STREAM feasible", k1att, k1_corr);
    }

    /* NDV estimate for hash_cap */
    List *grp_exprs = get_sortgrouplist_exprs(
        root->parse->groupClause, root->parse->targetList);
    double dNumGroups = estimate_num_groups(root, grp_exprs,
                                            input_rel->rows, NULL, NULL);
    int64 est_groups = (int64) clamp_row_est(dNumGroups);
    int64 hash_cap   = 4096;
    while (hash_cap < est_groups * 4) hash_cap *= 2;

    CustomPath *cpath = makeNode(CustomPath);
    cpath->path.pathtype    = T_CustomScan;
    cpath->path.parent      = output_rel;
    cpath->path.pathtarget  = output_rel->reltarget;
    double cost_c1=0, cost_c2=0, cost_c3=0, cost_c4=0;
    cpath->path.rows = clamp_row_est(dNumGroups);
    /*
     * Guaranteed floor cost — single call to xpb_groupagg_floor_cost().
     * That function is the authoritative cost center; see xpb_cost.c.
     * npreds=0 here; C4 is added below after pqfrag_tmp is extracted.
     */
    {
        double relpages = (input_rel->pages > 0)
                        ? (double)input_rel->pages : 1.0;
        XpbFloorCost fc = xpb_groupagg_floor_cost(
            relpages, input_rel->rows,
            clamp_row_est(dNumGroups), 0 /* npreds added below */);
        cpath->path.startup_cost = fc.startup;
        cpath->path.total_cost   = fc.total;
        cost_c1 = fc.c1_scan;
        cost_c2 = fc.c2_group;
        cost_c3 = fc.c3_output;
    }
    /* Composite pathkeys: ORDER BY (k1, k2) */
    cpath->path.pathkeys    = make_pathkeys_for_sortclauses(
        root, root->parse->groupClause, root->parse->targetList);
    cpath->custom_paths     = NIL;
    /* Extract pushable predicates from base relation quals */
    XpbQual pqfrag_tmp;     /* unified qual engine */
    List *base_quals = NIL;
    {
        ListCell *qlc;
        foreach(qlc, input_rel->baserestrictinfo)
        {
            RestrictInfo *ri = (RestrictInfo *) lfirst(qlc);
            base_quals = lappend(base_quals, ri->clause);
        }
    }
    xpb_qual_extract(base_quals, &pqfrag_tmp, k1att);
    /* C4: qual floor — update total_cost now that preds are known */
    if (xpb_qual_npreds(&pqfrag_tmp) > 0)
    {
        /* C4 via unified qual engine — single source of truth */
        double qual_floor = xpb_qual_npreds(&pqfrag_tmp) * input_rel->rows
                          * cpu_operator_cost * xpb_cost_qual_factor;
        cpath->path.total_cost += qual_floor;
        cost_c4 = qual_floor;
        elog(DEBUG2,
             "xpga2 cost: C1_scan=%.0f C2_group=%.0f C3_out=%.0f "
             "C4_qual=%.0f total=%.0f  pages=%.0f nrows=%.0f ngroups=%.0f npreds=%d",
             cpath->path.startup_cost,
             input_rel->rows * cpu_operator_cost * xpb_cost_group_factor,
             clamp_row_est(dNumGroups) * cpu_tuple_cost * xpb_cost_output_factor,
             qual_floor, cpath->path.total_cost,
             (double)(input_rel->pages > 0 ? input_rel->pages : 1),
             input_rel->rows, clamp_row_est(dNumGroups), pqfrag_tmp.npreds);
    }
    else
    {
        elog(DEBUG2,
             "xpga2 cost: C1_scan=%.0f C2_group=%.0f C3_out=%.0f "
             "C4_qual=0 total=%.0f  pages=%.0f nrows=%.0f ngroups=%.0f",
             cpath->path.startup_cost,
             input_rel->rows * cpu_operator_cost * xpb_cost_group_factor,
             clamp_row_est(dNumGroups) * cpu_tuple_cost * xpb_cost_output_factor,
             cpath->path.total_cost,
             (double)(input_rel->pages > 0 ? input_rel->pages : 1),
             input_rel->rows, clamp_row_est(dNumGroups));
    }
    elog(DEBUG2, "xp_batch: XpGroupAgg2 pushed preds=%d residual=%s",
         pqfrag_tmp.npreds, pqfrag_tmp.has_residual ? "yes" : "no");

    /* Don't take path if residual quals remain (correctness: we
     * cannot apply ExecQual inside XpGroupAgg2 yet) */
    /*
     * Any residual at all is a refusal, not just "nothing was pushable".
     * The residual clauses are discarded below (scan.plan.qual = NIL with
     * scanrelid = 0) and this node never calls ExecQual, so taking the path
     * with npreds > 0 && has_residual answered the query as though the
     * unrepresented clause had been applied.
     */
    if (pqfrag_tmp.has_residual)
        return;

    /* HAVING is not examined here and no upper node re-applies it. */
    if (root->parse->havingQual != NULL)
        return;


    /* Encode pqfrag into custom_private as (npreds, [attno,op,rhs]*n, residual) */
    cpath->custom_private   = list_make4(
        makeInteger(k1att), makeInteger(k2att),
        makeInteger(vatt),  makeInteger(reloid));
    /* k1/k2 column types (INT4OID or INT8OID) for output TupleDesc */
    cpath->custom_private = lappend(cpath->custom_private,
                                     makeInteger((int64) k1type));
    cpath->custom_private = lappend(cpath->custom_private,
                                     makeInteger((int64) k2type));
    cpath->custom_private   = lappend(cpath->custom_private,
                                       makeInteger((int)Min(hash_cap, INT_MAX)));
    /* preds: [npreds, has_residual, attno0, op0, rhs0, attno1, op1, rhs1, ...] */
    cpath->custom_private   = lappend(cpath->custom_private,
                                       makeInteger(pqfrag_tmp.npreds));
    cpath->custom_private   = lappend(cpath->custom_private,
                                       makeInteger(pqfrag_tmp.has_residual ? 1 : 0));
    for (int pi = 0; pi < pqfrag_tmp.npreds; pi++)
    {
        cpath->custom_private = lappend(cpath->custom_private,
                                         makeInteger(pqfrag_tmp.preds[pi].attno));
        cpath->custom_private = lappend(cpath->custom_private,
                                         makeInteger((int)pqfrag_tmp.preds[pi].op));
        cpath->custom_private = lappend(cpath->custom_private,
                                         makeInteger(pqfrag_tmp.preds[pi].rhs));
    }
    cpath->methods          = &xpga2_path_methods;
    /* Serialize cost floor components (×100 as integers) for EXPLAIN.
     * Precision: 0.01 cost unit = adequate for display purposes. */
    cpath->custom_private = lappend(cpath->custom_private,
                                     makeInteger((int)(cost_c1 * 100)));
    cpath->custom_private = lappend(cpath->custom_private,
                                     makeInteger((int)(cost_c2 * 100)));
    cpath->custom_private = lappend(cpath->custom_private,
                                     makeInteger((int)(cost_c3 * 100)));
    cpath->custom_private = lappend(cpath->custom_private,
                                     makeInteger((int)(cost_c4 * 100)));
    add_path(output_rel, (Path *) cpath);

    elog(DEBUG2, "xp_batch: XpGroupAgg2 path k1=%d k2=%d agg=%d est=%ld",
         k1att, k2att, vatt, est_groups);
}

static Plan *
xpga2_plan_path(PlannerInfo *root, RelOptInfo *rel,
                CustomPath *best_path, List *tlist,
                List *clauses, List *custom_plans)
{
    CustomScan *cscan = makeNode(CustomScan);
    cscan->methods              = &xpga2_scan_methods;
    cscan->scan.scanrelid       = 0;
    cscan->custom_plans         = NIL;
    cscan->scan.plan.qual       = NIL;

    /*
     * scanrelid = 0, so the two target lists play different roles and must not
     * be the same list:
     *
     *   scan.plan.targetlist   Vars of INDEX_VAR whose varattno is a POSITION
     *                          in custom_scan_tlist.
     *   custom_scan_tlist      the expressions those positions denote.
     *
     * Both used to be set to the INDEX_VAR list, so entry i of
     * custom_scan_tlist was Var(INDEX_VAR, i) — a reference to itself. Nothing
     * in execution noticed, because the only consumers read exprType(). But
     * EXPLAIN (VERBOSE) deparses the target list, and resolving INDEX_VAR means
     * following it into custom_scan_tlist, which pointed straight back. That
     * loop never terminated and could not be interrupted: statement_timeout did
     * not fire, pg_terminate_backend() did not end it, and the backend held a
     * relation lock until an immediate restart. Two were observed, at 39 and 9
     * minutes, on a plan whose non-VERBOSE form returns in 0.04 ms.
     *
     * Both get the real expressions. set_customscan_references() then builds an
     * index over custom_scan_tlist and rewrites scan.plan.targetlist into
     * INDEX_VAR references against it — that conversion is setrefs' job, not
     * this function's. Hand-building the INDEX_VAR list here is what created
     * the self-reference: the entries matched each other literally, so setrefs
     * raised no complaint and nothing in execution cared, because the only
     * consumers of custom_scan_tlist read exprType().
     *
     * exprType() of each entry is unchanged by this, so ExecTypeFromTL() and
     * the agg-output-type lookup in xpga2_begin behave exactly as before.
     */
    cscan->scan.plan.targetlist = tlist;
    cscan->custom_scan_tlist    = copyObject(tlist);
    cscan->custom_private       = best_path->custom_private;
    return (Plan *) cscan;
}

static Node *
xpga2_create_state(CustomScan *cscan)
{
    XpGroupAgg2State *s = palloc0(sizeof(XpGroupAgg2State));
    NodeSetTag(s, T_CustomScanState);
    s->css.methods = &xpga2_exec_methods;
    return (Node *) s;
}

static void
xpga2_begin(CustomScanState *node, EState *estate, int eflags)
{
    XpGroupAgg2State *state = (XpGroupAgg2State *) node;
    CustomScan       *cscan = (CustomScan *) node->ss.ps.plan;
    List             *priv  = cscan->custom_private;

    state->k1_attno   = (AttrNumber) intVal(list_nth(priv, 0));
    state->k2_attno   = (AttrNumber) intVal(list_nth(priv, 1));
    state->agg_attno  = (AttrNumber) intVal(list_nth(priv, 2));
    int reloid        =              intVal(list_nth(priv, 3));
    /* priv[4/5] = k1type/k2type (added for int8 support) */
    {
        Oid k1typid = (list_length(priv) >= 6)
                     ? (Oid) intVal(list_nth(priv, 4)) : INT4OID;
        Oid k2typid = (list_length(priv) >= 6)
                     ? (Oid) intVal(list_nth(priv, 5)) : INT4OID;
        state->k1ops = xpb_typeops_lookup(k1typid);
        state->k2ops = xpb_typeops_lookup(k2typid);
        if (!state->k1ops) state->k1ops = &xpb_typeops_int4;
        if (!state->k2ops) state->k2ops = &xpb_typeops_int4;
        /* vagg_ops: type of aggregated column. */
        /* TODO: serialize agg_typid separately. For now: */
        /* look up from rel at begin time (after rel is opened). */
        state->vagg_ops = &xpb_typeops_int4;
    }
    /* Agg output type: sum(int4)→INT8OID, sum(int8)→NUMERICOID */
    {
        CustomScan  *cscan2 = (CustomScan *) node->ss.ps.plan;
        int agg_tlist_idx = (state->k2_attno > 0) ? 2 : 1;
        TargetEntry *te2    = (TargetEntry *)
            list_nth(cscan2->custom_scan_tlist, agg_tlist_idx);
        state->agg_outtype  = exprType((Node *) te2->expr);
    }
    state->hash_cap_used = (list_length(priv) >= 7)
                         ? (int64) intVal(list_nth(priv, 6)) : 16384;
    /* Load pqfrag from custom_private[7..] */
    memset(&state->pqfrag, 0, sizeof(state->pqfrag));
    if (list_length(priv) >= 9)
    {
        state->pqfrag.npreds      = intVal(list_nth(priv, 7));
        state->pqfrag.has_residual = intVal(list_nth(priv, 8)) != 0;
        for (int pi = 0; pi < state->pqfrag.npreds && pi < XPB_MAX_PREDS; pi++)
        {
            int base = 9 + pi * 3;
            if (list_length(priv) < base + 3) break;
            state->pqfrag.preds[pi].attno = intVal(list_nth(priv, base));
            state->pqfrag.preds[pi].op    = intVal(list_nth(priv, base+1));
            state->pqfrag.preds[pi].rhs   = intVal(list_nth(priv, base+2));
        }
    }

    /* Recompute Class1 stream bounds from loaded predicates */
    {
        AttrNumber k1a = state->k1_attno;
        for (int pi = 0; pi < state->pqfrag.npreds; pi++)
        {
            PushedPred *pp = &state->pqfrag.preds[pi];
            if (pp->attno != k1a) continue;  /* only k1 column */
            if (pp->op == XQO_GE || pp->op == XQO_GT)
            {
                int64 lo = (pp->op==XQO_GE) ? pp->rhs : pp->rhs+1;
                if (!state->pqfrag.has_key_lo || lo > state->pqfrag.stream_key_lo)
                    state->pqfrag.stream_key_lo = lo;
                state->pqfrag.has_key_lo = true;
            }
            if (pp->op == XQO_LE || pp->op == XQO_LT)
            {
                int64 hi = (pp->op==XQO_LE) ? pp->rhs : pp->rhs-1;
                if (!state->pqfrag.has_key_hi || hi < state->pqfrag.stream_key_hi)
                    state->pqfrag.stream_key_hi = hi;
                state->pqfrag.has_key_hi = true;
            }
            if (pp->op == XQO_EQ)
            {
                state->pqfrag.stream_key_lo = pp->rhs; state->pqfrag.has_key_lo = true;
                state->pqfrag.stream_key_hi = pp->rhs; state->pqfrag.has_key_hi = true;
            }
            state->pqfrag.n_key_bounds++;
        }
    }

    state->rel      = table_open((Oid) reloid, AccessShareLock);
    /* Set vagg_ops from actual agg column type in relation schema */
    if (state->agg_attno > 0)
    {
        TupleDesc relDesc = RelationGetDescr(state->rel);
        Oid agg_typid = TupleDescAttr(relDesc, state->agg_attno - 1)->atttypid;
        const XpbTypeOps *aops = xpb_typeops_lookup(agg_typid);
        state->vagg_ops = aops ? aops : &xpb_typeops_int4;
    }
    state->nblocks  = RelationGetNumberOfBlocks(state->rel);
    state->snapshot = estate->es_snapshot;
    state->vmbuf    = InvalidBuffer;
    state->computed = false;
    state->output_pos = 0;
    BatchPropertiesInit(&state->batch_props);
    state->tuples_passed_qual = 0;
    state->tuples_visited = 0;
    state->pages_rejected = 0;
    state->pages_early_exit   = 0;
    state->use_bitmap         = false;

    /* Resolve cached typeops + fixed offsets for fast qual eval */
    if (state->pqfrag.npreds > 0)
        xpb_qual_resolve_cached(&state->pqfrag, RelationGetDescr(state->rel));

    /* Deserialize cost floor components from tail of custom_private */
    {
        int plen = list_length(priv);
        /* c1..c4 are last 4 elements */
        if (plen >= 4)
        {
            state->cost_c1_scan   = intVal(list_nth(priv, plen-4)) / 100.0;
            state->cost_c2_group  = intVal(list_nth(priv, plen-3)) / 100.0;
            state->cost_c3_output = intVal(list_nth(priv, plen-2)) / 100.0;
            state->cost_c4_qual   = intVal(list_nth(priv, plen-1)) / 100.0;
        }
    }

    /*
     * Create a TTSOpsVirtual result slot using TupleDesc derived from
     * custom_scan_tlist.  ExecTypeFromTL reads the real column types
     * from the planner's target list:
     *   k1/k2 → INT4OID or INT8OID (from GROUP BY column types)
     *   agg   → actual sum() return type (INT8 for int4 input,
     *           NUMERIC for int8 input)
     * This is the correct Custom Scan result-slot contract:
     * slot ops = TTSOpsVirtual (required by ExecStoreVirtualTuple),
     * TupleDesc = planner-derived (no hardcoded type assumptions).
     */
    {
        CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
        TupleDesc   td    = ExecTypeFromTL(cscan->custom_scan_tlist);
        BlessTupleDesc(td);
        node->ss.ps.ps_ResultTupleSlot =
            ExecAllocTableSlot(&estate->es_tupleTable,
                               td, &TTSOpsVirtual, 0);
    }
}

static void
xpga2_end(CustomScanState *node)
{
    XpGroupAgg2State *state = (XpGroupAgg2State *) node;
    if (BufferIsValid(state->vmbuf)) ReleaseBuffer(state->vmbuf);
    if (state->rel) table_close(state->rel, AccessShareLock);
}

static void
xpga2_rescan(CustomScanState *node)
{
    XpGroupAgg2State *state = (XpGroupAgg2State *) node;
    state->computed   = false;
    state->output_pos = 0;
    state->output_ngroups = 0;
}

static void
xpga2_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
    XpGroupAgg2State *state = (XpGroupAgg2State *) node;

    /* Unified qual EXPLAIN */
    xpb_qual_explain(&state->pqfrag, es);
    ExplainPropertyText("GroupAgg Path",
        state->agg_path_used ? state->agg_path_used : "pending", es);

    if (state->zlfs_used && es->analyze)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "%.1f ms, %ld rows scanned",
                 state->zlfs_scan_ms, state->zlfs_zone_rows);
        ExplainPropertyText("ZLFS Scan", buf, es);
        ExplainPropertyText("ZLFS Columns", "period_key, company_key, amount_dt (3 of 4)", es);
    }

    /* Cost floor breakdown */
    if (!state->zlfs_used)
    {
        char cbuf[256];
        double total = state->cost_c1_scan + state->cost_c2_group
                     + state->cost_c3_output + state->cost_c4_qual;
        snprintf(cbuf, sizeof(cbuf),
                 "C1=%.0f C2=%.0f C3=%.0f C4=%.0f total=%.0f",
                 state->cost_c1_scan, state->cost_c2_group,
                 state->cost_c3_output, state->cost_c4_qual, total);
        ExplainPropertyText("CostFloor", cbuf, es);
    }

    const char *scope_str;
    switch (state->batch_props.order_scope) {
        case BATCH_ORDER_STREAM: scope_str = "STREAM"; break;
        case BATCH_ORDER_LOCAL:  scope_str = "LOCAL";  break;
        default:                 scope_str = "NONE";   break;
    }
    ExplainPropertyText("BatchProperties.order_scope", scope_str, es);

    if (state->batch_props.sorted_by_key && es->analyze)
    {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "(k1=attno%d, k2=attno%d)  source: %s",
                 state->k1_attno, state->k2_attno,
                 state->batch_props.order_source);
        ExplainPropertyText("BatchProperties.sorted_by_key", buf, es);
    }

    if (es->analyze)
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "%ld", state->output_ngroups);
        ExplainPropertyText("Groups", buf, es);
        snprintf(buf, sizeof(buf), "%ld", state->group_transitions);
        ExplainPropertyText("Group Transitions", buf, es);

        if (state->zlfs_used)
        {
            /* ZLFS path: zone-specific counters */
            snprintf(buf, sizeof(buf), "%ld", state->zlfs_zone_rows);
            ExplainPropertyText("Zone Rows Scanned", buf, es);
            snprintf(buf, sizeof(buf), "%.1f ms", state->zlfs_scan_ms);
            ExplainPropertyText("Zone Scan Time", buf, es);
        }
        else
        {
            /* Heap path: page and tuple breakdown */
            snprintf(buf, sizeof(buf), "%u", state->nblocks);
            ExplainPropertyText("Pages Total", buf, es);

            if (state->pages_rejected > 0)
            {
                snprintf(buf, sizeof(buf), "%ld", state->pages_rejected);
                ExplainPropertyText("Pages Rejected (page pruning)", buf, es);
            }
            if (state->pages_early_exit > 0)
            {
                snprintf(buf, sizeof(buf), "%ld", state->pages_early_exit);
                ExplainPropertyText("Pages Skipped (STREAM exit)", buf, es);
            }
            snprintf(buf, sizeof(buf), "%ld", state->pages_scanned);
            ExplainPropertyText("Pages Scanned", buf, es);

            snprintf(buf, sizeof(buf), "%ld", state->tuples_visited);
            ExplainPropertyText("Tuples Visited", buf, es);

            if (state->pqfrag.npreds > 0)
            {
                snprintf(buf, sizeof(buf), "%ld / %ld (%.1f%%)",
                         state->tuples_passed_qual,
                         state->tuples_visited,
                         state->tuples_visited > 0
                         ? 100.0*state->tuples_passed_qual/state->tuples_visited
                         : 0.0);
                ExplainPropertyText("Tuples Passed Qual", buf, es);
            }
        }

        /* LOCAL_PARTIAL metrics */
        if (state->lp_batches > 0)
        {
            ExplainPropertyText("Batch Grouping", "LOCAL_PARTIAL", es);
            snprintf(buf, sizeof(buf), "%ld", state->lp_input_tuples);
            ExplainPropertyText("LP Input Tuples", buf, es);
            snprintf(buf, sizeof(buf), "%ld", state->lp_partials_emitted);
            ExplainPropertyText("LP Partials Emitted", buf, es);
            snprintf(buf, sizeof(buf), "%.1f×",
                     state->lp_partials_emitted > 0
                     ? (double)state->lp_input_tuples / state->lp_partials_emitted
                     : 0.0);
            ExplainPropertyText("LP Reduction Ratio", buf, es);
            snprintf(buf, sizeof(buf), "%ld",
                     state->lp_batches > 0
                     ? state->lp_partials_emitted / state->lp_batches
                     : 0);
            ExplainPropertyText("LP Avg Groups/Batch", buf, es);
            snprintf(buf, sizeof(buf), "%ld", state->lp_max_groups_in_batch);
            ExplainPropertyText("LP Max Groups In Batch", buf, es);
            snprintf(buf, sizeof(buf), "%ld batches", state->lp_batches);
            ExplainPropertyText("LP Batches", buf, es);
            ExplainPropertyText("Merge Required", "true (internal)", es);
        }
    }
}

void xpga2_register(void);
void xpga2_register(void)
{
    RegisterCustomScanMethods(&xpga2_scan_methods);
}
