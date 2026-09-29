/*-------------------------------------------------------------------------
 * xpb_pageagg.c
 *
 * XpPageAgg — page-level aggregate probe node (pP1-pP4)
 *
 * Goal: measure cost of changing unit of work from tuple to page.
 * Comparison: NativeBatchAgg (~90ms/1M) vs PageBatchAgg (this node).
 *
 * Architecture (fused single node, no scan/agg separation):
 *   XpPageAgg owns the full pipeline:
 *     ReadBuffer → [all_visible fast path] → line pointer loop →
 *     [simple int qual] → aggregate update
 *
 * all_visible fast path: bench tables are 100% all_visible after VACUUM.
 * On an all_visible page: skip HeapTupleSatisfiesMVCC entirely.
 *
 * Invariants (from plan):
 *   1. Probe node, not framework — measurement corridor only
 *   2. No generic page transport API
 *   3. No slot arrays
 *   4. No new core hooks
 *   5. Simple qual only: int4_col op const (not ExecQual)
 *
 * Scope: count(*), count(*) WHERE int4 < const, sum(int4)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/pg_class.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "xpb_pageagg.h"
#include "storage/itemid.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* from xp_batch.c */
extern bool xp_batch_enabled;

/* =========================================================
 * GUC
 * ========================================================= */
bool xpb_pageagg_enabled      = false;
bool xpb_pageagg_persistent   = false; /* pS2+S3: use side table */
bool xpb_pageagg_summary       = false; /* pPD1+2: build page summaries */
bool xpb_pageagg_skip          = false; /* pPD1: min/max page skip */
bool xpb_pageagg_fastcount     = false; /* pPD2: row_count shortcut */

/* =========================================================
 * Page-level summary (pPD1+pPD2)
 *
 * Built lazily in xppa_build_summaries() from one scan pass.
 * Stored as palloc'd array indexed by BlockNumber.
 * ========================================================= */


/* =========================================================
 * Simple qual representation (no ExprState)
 * Encodes: attno op const_val  where op ∈ {<, <=, =, >=, >}
 * Only int4 column vs int4 literal — measurement scope.
 * ========================================================= */
typedef enum SimpleQualOp { SQ_LT, SQ_LE, SQ_EQ, SQ_GE, SQ_GT } SimpleQualOp;

typedef struct SimpleQualPred
{
    AttrNumber   attno;
    SimpleQualOp op;
    int32        rhs;
} SimpleQualPred;

#define SQ_MAX_PREDS 8

typedef struct SimpleQual
{
    int          npreds;
    SimpleQualPred preds[SQ_MAX_PREDS];
} SimpleQual;

/* =========================================================
 * Aggregate type (same as NativeBatch)
 * ========================================================= */
typedef enum PageAggType { PA_COUNT, PA_SUM } PageAggType;

/* =========================================================
 * State
 * ========================================================= */
typedef struct XpPageAggState
{
    CustomScanState  css;

    /* relation */
    Relation         rel;
    BlockNumber      nblocks;

    /* snapshot */
    Snapshot         snapshot;
    Buffer           vmbuf;        /* visibility map buffer */
    BufferAccessStrategy strategy; /* pPB1: BAS_BULKREAD for cold I/O */

    /* aggregate config */
    PageAggType      agg_type;
    AttrNumber       agg_attno;

    /* multi-predicate qual */
    SimpleQual       qual;

    /* results */
    int64            total_count;
    int64            total_sum;
    bool             computed;

    /* pPD1+pPD2: page summaries */
    XpPageSummary   *page_summaries;    /* [nblocks], NULL if not built */
    bool             summaries_built;

    /* stats */
    int64            blocks_scanned;
    int64            blocks_all_visible;
    int64            blocks_skipped;    /* pPD1: min/max skip */
    int64            blocks_fastcount;  /* pPD2: row_count shortcut */
    int64            tuples_examined;
    int64            tuples_passed;
    int64            chunks_total;      /* probe: total chunks on surviving pages */
    int64            chunks_skipped;    /* probe: chunks skipped by chunk summary */
} XpPageAggState;

/* =========================================================
 * Forward declarations
 * ========================================================= */
static Plan *xppa_plan_path(PlannerInfo *root, RelOptInfo *rel,
                             CustomPath *best_path, List *tlist,
                             List *clauses, List *custom_plans);
static Node *xppa_create_state(CustomScan *cscan);
static void  xppa_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *xppa_exec(CustomScanState *node);
static void  xppa_end(CustomScanState *node);
static void  xppa_rescan(CustomScanState *node);
static void  xppa_explain(CustomScanState *node, List *ancestors,
                           ExplainState *es);

static const CustomPathMethods xppa_path_methods = {
    .CustomName     = "XpPageAgg",
    .PlanCustomPath = xppa_plan_path,
};
static const CustomScanMethods xppa_scan_methods = {
    .CustomName            = "XpPageAgg",
    .CreateCustomScanState = xppa_create_state,
};
static const CustomExecMethods xppa_exec_methods = {
    .CustomName        = "XpPageAgg",
    .BeginCustomScan   = xppa_begin,
    .ExecCustomScan    = xppa_exec,
    .EndCustomScan     = xppa_end,
    .ReScanCustomScan  = xppa_rescan,
    .ExplainCustomScan = xppa_explain,
};

/* =========================================================
 * fast_getattr_int4
 *
 * Read a fixed-width int4 attribute directly from tuple header.
 * Assumes: attno is fixed-size, not null (checked by caller).
 * attbyval=true, attlen=4.
 *
 * Uses att_addlength_pointer / att_align_nominal to skip to
 * the right offset without deforming the full tuple.
 *
 * For bench tables (single int column): attoff = 0 always.
 * ========================================================= */
static inline int32
fast_getattr_int4(HeapTupleHeader tup, int attnum, TupleDesc tupdesc)
{
    /*
     * Use standard heap_getattr approach but inline-friendly.
     * For fixed-width byval attrs we can compute offset directly.
     */
    uint8  *bp  = tup->t_bits;
    bool    hasnull = (tup->t_infomask & HEAP_HASNULL) != 0;

    if (hasnull && att_isnull(attnum - 1, bp))
        return 0;   /* null — caller checks sep */

    /* compute data pointer */
    char *tp = (char *) tup + tup->t_hoff;
    /* walk attrs 1..attnum-1 to find offset */
    for (int i = 0; i < attnum - 1; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        if (hasnull && att_isnull(i, bp))
            continue;
        if (attr->attlen > 0)
            tp += att_align_nominal(attr->attlen, attr->attalign);
        else
            tp += att_addlength_pointer(0, attr->attlen, tp);
    }
    /* tp now points at our int4 */
    return *((int32 *) tp);
}

/* =========================================================
 * xppa_scan_page — hot path for one heap page
 *
 * Returns:  count of qualifying tuples on this page.
 * Side effects: updates state->total_sum if agg_type == PA_SUM.
 * ========================================================= */
static int64
xppa_scan_page(XpPageAggState *state, Buffer buf, Page page,
               bool all_visible, TupleDesc tupdesc)
{
    OffsetNumber  maxoff = PageGetMaxOffsetNumber(page);
    int64         n      = 0;
    int64         sum    = 0;
    bool          do_sum = (state->agg_type == PA_SUM);
    bool          do_qual = (state->qual.npreds > 0);
    AttrNumber    sa   = state->agg_attno;
    BlockNumber   blkno = BufferGetBlockNumber(buf);
    Oid           relid = RelationGetRelid(state->rel);

    /*
     * Chunk probe: divide page into 4 chunks by line pointer range.
     * Build coarse min/max for qual column(s) per chunk in first pass,
     * then skip chunks that can't contain qualifying tuples.
     *
     * Only active when qual is present AND qual column is period_key-like
     * (first pred's attno used for chunk summary).
     */
#define NCHUNKS 4
    bool  use_chunks = do_qual && all_visible && maxoff >= 16;
    int32 chunk_min[NCHUNKS];
    int32 chunk_max[NCHUNKS];
    bool  chunk_skip[NCHUNKS];

    if (use_chunks)
    {
        AttrNumber ca = state->qual.preds[0].attno;
        int chunk_size = (maxoff + NCHUNKS - 1) / NCHUNKS;

        for (int c = 0; c < NCHUNKS; c++)
        {
            chunk_min[c] = PG_INT32_MAX;
            chunk_max[c] = PG_INT32_MIN;
            chunk_skip[c] = false;
        }

        /* First pass: build chunk min/max */
        for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
        {
            ItemId lp = PageGetItemId(page, off);
            if (!ItemIdIsNormal(lp)) continue;
            HeapTupleHeader htup = (HeapTupleHeader) PageGetItem(page, lp);
            int32 val = fast_getattr_int4(htup, ca, tupdesc);
            int c = (off - 1) / chunk_size;
            if (c >= NCHUNKS) c = NCHUNKS - 1;
            if (val < chunk_min[c]) chunk_min[c] = val;
            if (val > chunk_max[c]) chunk_max[c] = val;
        }

        /* Determine which chunks can be skipped */
        for (int c = 0; c < NCHUNKS; c++)
        {
            if (chunk_min[c] > chunk_max[c]) { chunk_skip[c] = true; continue; }

            /* Check each pred against chunk range */
            bool skip = false;
            for (int qi = 0; qi < state->qual.npreds && !skip; qi++)
            {
                const SimpleQualPred *p = &state->qual.preds[qi];
                if (p->attno != ca) continue; /* only check matching column */
                switch (p->op)
                {
                    case SQ_LT:  skip = (chunk_min[c] >= p->rhs); break;
                    case SQ_LE:  skip = (chunk_min[c] >  p->rhs); break;
                    case SQ_EQ:  skip = (chunk_max[c] <  p->rhs || chunk_min[c] > p->rhs); break;
                    case SQ_GE:  skip = (chunk_max[c] <  p->rhs); break;
                    case SQ_GT:  skip = (chunk_max[c] <= p->rhs); break;
                }
            }
            chunk_skip[c] = skip;
        }

        /* Track stats */
        state->chunks_total += NCHUNKS;
        for (int c = 0; c < NCHUNKS; c++)
            if (chunk_skip[c]) state->chunks_skipped++;
    }

    int chunk_size = (maxoff + NCHUNKS - 1) / NCHUNKS;

    state->tuples_examined += maxoff;

    for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
    {
        /* Chunk skip check */
        if (use_chunks)
        {
            int c = (off - 1) / chunk_size;
            if (c >= NCHUNKS) c = NCHUNKS - 1;
            if (chunk_skip[c]) continue;
        }

        ItemId lp = PageGetItemId(page, off);

        if (unlikely(!ItemIdIsNormal(lp)))
            continue;

        HeapTupleHeader htup =
            (HeapTupleHeader) PageGetItem(page, lp);

        if (!all_visible)
        {
            HeapTupleData tup;
            tup.t_data    = htup;
            tup.t_len     = ItemIdGetLength(lp);
            tup.t_tableOid = relid;
            ItemPointerSet(&tup.t_self, blkno, off);

            if (!HeapTupleSatisfiesVisibility(&tup,
                                              state->snapshot,
                                              buf))
                continue;
        }

        /* multi-predicate qual — all must pass */
        if (do_qual)
        {
            bool pass_all = true;
            for (int qi = 0; qi < state->qual.npreds && pass_all; qi++)
            {
                const SimpleQualPred *p = &state->qual.preds[qi];
                int32 qval = fast_getattr_int4(htup, p->attno, tupdesc);
                switch (p->op)
                {
                    case SQ_LT: pass_all = (qval <  p->rhs); break;
                    case SQ_LE: pass_all = (qval <= p->rhs); break;
                    case SQ_EQ: pass_all = (qval == p->rhs); break;
                    case SQ_GE: pass_all = (qval >= p->rhs); break;
                    case SQ_GT: pass_all = (qval >  p->rhs); break;
                    default:    break;
                }
            }
            if (!pass_all) continue;
        }

        n++;
        if (do_sum)
            sum += fast_getattr_int4(htup, sa, tupdesc);
    }

    state->tuples_passed += n;
    state->total_sum     += sum;
    return n;
#undef NCHUNKS
}

/* =========================================================
 * xppa_exec — main aggregate loop
 * ========================================================= */

/*
 * xppa_build_summaries - pPD1+pPD2 build phase.
 * Single pass: min/max of projected int col + row_count per page.
 */
static void
xppa_build_summaries(XpPageAggState *state)
{
    Relation        rel     = state->rel;
    Snapshot        snap    = state->snapshot;
    BlockNumber     nblocks = state->nblocks;
    TupleDesc       tupdesc = RelationGetDescr(rel);
    AttrNumber      attno   = state->agg_attno;
    Buffer          vmbuf2  = InvalidBuffer;
    Oid             relid   = RelationGetRelid(rel);

    state->page_summaries = (XpPageSummary *)
        palloc0(nblocks * sizeof(XpPageSummary));

    for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
    {
        XpPageSummary  *s    = &state->page_summaries[blkno];
        Buffer          buf;
        Page            page;
        OffsetNumber    maxoff;
        bool            av;

        s->min_val   = PG_INT32_MAX;
        s->max_val   = PG_INT32_MIN;
        s->row_count = 0;
        s->valid     = false;

        av = (visibilitymap_get_status(rel, blkno, &vmbuf2) &
              VISIBILITYMAP_ALL_VISIBLE) != 0;
        s->all_visible = av;

        buf    = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
                                    state->strategy);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page   = BufferGetPage(buf);
        maxoff = PageGetMaxOffsetNumber(page);

        for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
        {
            ItemId          lp   = PageGetItemId(page, off);
            HeapTupleHeader htup;

            if (!ItemIdIsNormal(lp))
                continue;

            htup = (HeapTupleHeader) PageGetItem(page, lp);

            if (!av)
            {
                HeapTupleData tup;
                tup.t_data     = htup;
                tup.t_len      = ItemIdGetLength(lp);
                tup.t_tableOid = relid;
                ItemPointerSet(&tup.t_self, blkno, off);
                if (!HeapTupleSatisfiesVisibility(&tup, snap, buf))
                    continue;
            }

            s->row_count++;
            if (attno > 0)
            {
                int32 v = fast_getattr_int4(htup, attno, tupdesc);
                if (v < s->min_val) s->min_val = v;
                if (v > s->max_val) s->max_val = v;
            }
        }

        s->valid = true;
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        ReleaseBuffer(buf);
    }

    if (BufferIsValid(vmbuf2))
        ReleaseBuffer(vmbuf2);

    state->summaries_built = true;
}

/*
 * pPD1: can we skip this page given multi-predicate qual?
 * Returns true when ANY predicate proves the page is entirely outside range.
 * Uses page min/max (on the leading key column, attno matching pred).
 * Currently only applies to predicates on the summary column (attno=1 for period_key).
 */
static inline bool
xppa_page_can_skip(const XpPageSummary *s, const SimpleQual *q,
                   AttrNumber summary_attno)
{
    if (!s->valid || s->min_val > s->max_val)
        return false;

    /*
     * A summary describes ONE column.  Without that column's identity there is
     * nothing to compare a predicate against, so nothing can be skipped: the
     * persistent side table records only (relfilenode, relpages, blkno) and no
     * column, which is why it cannot drive pruning either.
     */
    if (summary_attno <= 0)
        return false;

    for (int i = 0; i < q->npreds; i++)
    {
        const SimpleQualPred *p = &q->preds[i];

        /*
         * Only a predicate on the summarised column can prune.  This used to
         * apply every predicate to whatever column the summary happened to be
         * built over -- agg_attno, the column being SUMMED -- so
         * `sum(amount_dt) WHERE period_key < 10` compared amount_dt's min/max
         * against period_key's bound and skipped whole pages before the
         * visibility check and before the qual.  Measured: 0 instead of
         * 21 929 000.  The old comment here described this rule; the code did
         * not implement it.
         */
        if (p->attno != summary_attno)
            continue;

        bool skip = false;
        switch (p->op)
        {
            case SQ_LT:  skip = (s->min_val >= p->rhs); break;
            case SQ_LE:  skip = (s->min_val >  p->rhs); break;
            case SQ_EQ:  skip = (s->max_val <  p->rhs || s->min_val > p->rhs); break;
            case SQ_GE:  skip = (s->max_val <  p->rhs); break;
            case SQ_GT:  skip = (s->max_val <= p->rhs); break;
        }
        if (skip) return true;   /* one pred is enough to eliminate page */
    }
    return false;
}

/*
 * pPD2: can we count this page without scanning tuples?
 * Only safe when page is all_visible and summary is valid.
 */
static inline bool
xppa_page_can_fastcount(const XpPageSummary *s)
{
    return s->valid && s->all_visible;
}

static TupleTableSlot *
xppa_exec(CustomScanState *node)
{
    XpPageAggState   *state  = (XpPageAggState *) node;
    TupleTableSlot   *result = node->ss.ps.ps_ResultTupleSlot;

    if (state->computed)
        return ExecClearTuple(result);
    state->computed = true;

    /* pPD1+pPD2: build page summaries if requested */
    if (xpb_pageagg_summary && !state->summaries_built)
        xppa_build_summaries(state);

    {
        Relation    rel      = state->rel;
        BlockNumber nblocks  = state->nblocks;
        TupleDesc   tupdesc  = RelationGetDescr(rel);
        int64       count    = 0;
        bool        do_skip      = xpb_pageagg_skip && (state->qual.npreds > 0)
                                   && state->summaries_built;
        bool        do_fastcount = xpb_pageagg_fastcount
                                   && state->agg_type == PA_COUNT
                                   && !(state->qual.npreds > 0)
                                   && state->summaries_built;

        for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
        {
            Buffer          buf;
            Page            page;
            bool            all_visible;
            XpPageSummary  *s = state->summaries_built
                                ? &state->page_summaries[blkno]
                                : NULL;

            /* pPD1: min/max page skip */
            if (do_skip && s &&
                xppa_page_can_skip(s, &state->qual, state->agg_attno))
            {
                state->blocks_skipped++;
                continue;
            }

            /* pPD2: row_count shortcut for count(*) */
            if (do_fastcount && s && xppa_page_can_fastcount(s))
            {
                count += s->row_count;
                state->blocks_fastcount++;
                continue;
            }

            all_visible = (visibilitymap_get_status(rel, blkno, &state->vmbuf)
                           & VISIBILITYMAP_ALL_VISIBLE) != 0;

            buf  = ReadBufferExtended(rel, MAIN_FORKNUM, blkno,
                                      RBM_NORMAL, state->strategy);
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            page = BufferGetPage(buf);

            count += xppa_scan_page(state, buf, page, all_visible, tupdesc);

            LockBuffer(buf, BUFFER_LOCK_UNLOCK);
            ReleaseBuffer(buf);

            state->blocks_scanned++;
            if (all_visible)
                state->blocks_all_visible++;
        }

        state->total_count = count;
    }

    /* materialise result */
    ExecClearTuple(result);
    {
        Datum d = (state->agg_type == PA_SUM)
                  ? Int64GetDatum(state->total_sum)
                  : Int64GetDatum(state->total_count);
        result->tts_values[0] = d;
        result->tts_isnull[0] = false;
        result->tts_nvalid    = 1;
        ExecStoreVirtualTuple(result);
    }
    return result;
}

/* =========================================================
 * Planner / lifecycle boilerplate
 * ========================================================= */

void xppa_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                   RelOptInfo *output_rel, void *extra);

void
xppa_add_path(PlannerInfo *root, RelOptInfo *input_rel,
               RelOptInfo *output_rel, void *extra)
{
    CustomPath *cpath;
    Path       *base_path = input_rel->cheapest_total_path;
    int         agg_type  = PA_COUNT;
    int         agg_attno = 0;
    int         q_attno   = 0;
    int         q_op      = SQ_LT;
    int         q_rhs     = 0;
    bool        has_qual  = false;

    if (!xpb_pageagg_enabled) return;

    /* AGG_PLAIN only, no GROUP BY */
    if (root->parse->groupClause  != NIL ||
        root->parse->groupingSets != NIL ||
        root->parse->hasDistinctOn)
        return;

    /* base rel must be a plain table (no joins etc.) */
    if (base_path == NULL || input_rel->relid == 0)
        return;

    /* Single-table guard: decline for joins and subquery inputs.
     * Safe subset: exactly 1 base RTE_RELATION in the query. */
    {
        int _nr = 0; ListCell *_lc;
        foreach(_lc, root->parse->rtable) {
            RangeTblEntry *_r = (RangeTblEntry *) lfirst(_lc);
            if (_r->rtekind == RTE_RELATION && _r->relkind == RELKIND_RELATION)
                _nr++;
        }
        if (_nr != 1) return;
    }


    /*
     * This node emits exactly ONE value: xppa_begin installs a one-attribute
     * result slot.  So it may only be chosen when the target list is exactly
     * one bare Aggref -- not an expression over one, and not several.
     *
     * Without this:
     *   SELECT 'x' || sum(a) FROM t   returned 2000 where SQL gives x21929000
     *                                 (the expression discarded, a different
     *                                 value emitted -- silently)
     *   SELECT count(*), sum(a)       emitted one field for a two-column plan:
     *                                 "unexpected field count in D message",
     *                                 and a backend crash on some shapes.
     *
     * The old loop took the FIRST Aggref it found and ignored the rest of the
     * list, which is what let both shapes through.
     */
    {
        int          ntle = list_length(root->processed_tlist);
        TargetEntry *only;

        if (ntle != 1)
            return;
        only = (TargetEntry *) linitial(root->processed_tlist);
        if (!IsA(only->expr, Aggref))
            return;
    }

    /* detect aggregate type from processed_tlist */
    {
        ListCell *lc;
        foreach(lc, root->processed_tlist)
        {
            TargetEntry *te = (TargetEntry *) lfirst(lc);
            if (!IsA(te->expr, Aggref)) continue;

            Aggref *agg     = (Aggref *) te->expr;
            char   *aggname = get_func_name(agg->aggfnoid);

            if (aggname && strcmp(aggname, "count") == 0 && agg->args == NIL)
            {
                agg_type = PA_COUNT; agg_attno = 0;
            }
            else if (aggname && strcmp(aggname, "sum") == 0 &&
                     list_length(agg->args) == 1)
            {
                TargetEntry *arg = (TargetEntry *) linitial(agg->args);
                if (IsA(arg->expr, Var) &&
                    ((Var *)arg->expr)->vartype == INT4OID)
                {
                    agg_type  = PA_SUM;
                    agg_attno = ((Var *)arg->expr)->varattno;
                }
                else return;   /* unsupported sum type */
            }
            else return;
            break;
        }
    }

    /*
     * Extract simple int4 quals from base restriction clauses.
     * Multi-predicate: collect up to SQ_MAX_PREDS pushable preds.
     */
    int         npreds = 0;
    SimpleQualPred preds[SQ_MAX_PREDS];
    /*
     * Set by every clause this extractor cannot represent -- unsupported
     * shape, type or operator, or no room left in preds[].  A residual is
     * never re-applied: this node sets scan.plan.qual = NIL with
     * scanrelid = 0 and never calls ExecQual, so taking the path with one
     * answers the query as though the clause had been applied.
     */
    bool        has_residual = false;

    {
        ListCell *lc;
        foreach(lc, input_rel->baserestrictinfo)
        {
            RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
            Expr         *clause = rinfo->clause;

            if (npreds >= SQ_MAX_PREDS) { has_residual = true; break; }
            if (!IsA(clause, OpExpr)) { has_residual = true; continue; }
            OpExpr *op = (OpExpr *) clause;
            if (list_length(op->args) != 2) { has_residual = true; continue; }

            Expr *larg = (Expr *) linitial(op->args);
            Expr *rarg = (Expr *) lsecond(op->args);

            if (!IsA(larg, Var) || !IsA(rarg, Const)) { has_residual = true; continue; }
            Var   *v = (Var *) larg;
            Const *c = (Const *) rarg;

            if (v->vartype != INT4OID || c->consttype != INT4OID)
                { has_residual = true; continue; }
            if (c->constisnull) { has_residual = true; continue; }

            /* map operator to SimpleQualOp */
            char *opname = get_opname(op->opno);
            SimpleQualOp sq_op;
            if      (!opname)                    { has_residual = true; continue; }
            else if (strcmp(opname, "<")  == 0)  sq_op = SQ_LT;
            else if (strcmp(opname, "<=") == 0)  sq_op = SQ_LE;
            else if (strcmp(opname, "=")  == 0)  sq_op = SQ_EQ;
            else if (strcmp(opname, ">=") == 0)  sq_op = SQ_GE;
            else if (strcmp(opname, ">")  == 0)  sq_op = SQ_GT;
            else                                 { has_residual = true; continue; }

            preds[npreds].attno = v->varattno;
            preds[npreds].op    = sq_op;
            preds[npreds].rhs   = DatumGetInt32(c->constvalue);
            npreds++;
            /* continue collecting — no break */
        }
    }

    /*
     * Refuse rather than approximate.  An accepted Custom Scan must implement
     * the query's semantics exactly; whatever it does not implement has to be
     * a refusal before execution, never a silent approximation.
     */
    if (has_residual)
        return;

    /*
     * HAVING is never examined here and no upper node re-applies it, because
     * this path replaces the whole grouping rel.  Same refusal.
     */
    if (root->parse->havingQual != NULL)
        return;

    /* build path:
     * custom_private = [agg_type, agg_attno, npreds, (attno,op,rhs)*npreds, relid]
     */
    cpath = makeNode(CustomPath);
    cpath->path.pathtype       = T_CustomScan;
    cpath->path.parent         = output_rel;
    cpath->path.pathtarget     = output_rel->reltarget;
    cpath->path.rows           = 1;
    cpath->path.startup_cost   = 0;
    cpath->path.total_cost     = base_path->total_cost * 0.30;
    cpath->path.parallel_safe  = false;
    cpath->path.pathkeys       = NIL;
    cpath->flags               = 0;
    cpath->custom_paths        = NIL;
    cpath->custom_private      = list_make1(makeInteger(agg_type));
    cpath->custom_private      = lappend(cpath->custom_private, makeInteger(agg_attno));
    cpath->custom_private      = lappend(cpath->custom_private, makeInteger(npreds));
    for (int i = 0; i < npreds; i++)
    {
        cpath->custom_private = lappend(cpath->custom_private, makeInteger(preds[i].attno));
        cpath->custom_private = lappend(cpath->custom_private, makeInteger((int)preds[i].op));
        cpath->custom_private = lappend(cpath->custom_private, makeInteger(preds[i].rhs));
    }
    cpath->methods             = &xppa_path_methods;

    /* store base relid in custom_plans placeholder to find relation at exec */
    /* store relation OID (not relid/varno) for table_open in Begin */
    {
        RangeTblEntry *rte = planner_rt_fetch(input_rel->relid, root);
        cpath->custom_private = lappend(cpath->custom_private,
                                         makeInteger((int) rte->relid));
    }

    add_path(output_rel, (Path *) cpath);
    elog(DEBUG2, "xp_batch: XpPageAgg path added "
         "(agg=%d attno=%d has_qual=%d q=%d op=%d rhs=%d)",
         agg_type, agg_attno, has_qual, q_attno, q_op, q_rhs);
}

static Plan *
xppa_plan_path(PlannerInfo *root, RelOptInfo *rel,
               CustomPath *best_path, List *tlist,
               List *clauses, List *custom_plans)
{
    CustomScan *cscan = makeNode(CustomScan);

    cscan->methods              = &xppa_scan_methods;
    cscan->scan.plan.targetlist = tlist;
    cscan->scan.scanrelid       = 0;
    cscan->custom_plans         = NIL;
    cscan->custom_scan_tlist    = tlist;
    cscan->custom_private       = best_path->custom_private;

    return (Plan *) cscan;
}

static Node *
xppa_create_state(CustomScan *cscan)
{
    XpPageAggState *state = palloc0(sizeof(XpPageAggState));
    NodeSetTag(state, T_CustomScanState);
    state->css.methods = &xppa_exec_methods;
    return (Node *) state;
}

static void
xppa_begin(CustomScanState *node, EState *estate, int eflags)
{
    XpPageAggState *state = (XpPageAggState *) node;
    CustomScan     *cscan = (CustomScan *) node->ss.ps.plan;
    List           *priv  = cscan->custom_private;
    int             relid;

    /* decode custom_private: [agg_type, agg_attno, npreds, (attno,op,rhs)*n, relid] */
    ListCell *lc = list_head(priv);
#define NEXT_INT(lc) ({ int _v = intVal(lfirst(lc)); lc = lnext(priv, lc); _v; })

    state->agg_type  = (PageAggType) NEXT_INT(lc);
    state->agg_attno = (AttrNumber)  NEXT_INT(lc);

    state->qual.npreds = NEXT_INT(lc);
    for (int i = 0; i < state->qual.npreds && i < SQ_MAX_PREDS; i++)
    {
        state->qual.preds[i].attno = (AttrNumber) NEXT_INT(lc);
        state->qual.preds[i].op    = (SimpleQualOp) NEXT_INT(lc);
        state->qual.preds[i].rhs   = NEXT_INT(lc);
    }
    relid              =              NEXT_INT(lc);
#undef NEXT_INT

    /* open relation by OID — XpPageAgg is an upper node (scanrelid=0),
     * so we cannot use ExecOpenScanRelation. Use table_open directly.
     * Estate cleanup won't close it; we close in xppa_end. */
    state->rel      = table_open((Oid) relid, AccessShareLock);
    state->nblocks  = RelationGetNumberOfBlocks(state->rel);
    state->snapshot  = estate->es_snapshot;
    state->vmbuf     = InvalidBuffer;
    /*
     * pPB1: BAS_BULKREAD strategy.
     * Tells buffer manager we are doing a large sequential scan;
     * uses a ring buffer to avoid polluting shared_buffers.
     * Same heuristic as heapam: apply when table is large.
     */
    /*
     * BAS_BULKREAD ring buffer only helps when table > shared_buffers.
     * When table fits in shared_buffers (hot path), the ring buffer
     * thrashes the cache — warm-path 5ms becomes 90ms.
     * Use BAS_BULKREAD only when nblocks > NBuffers (truly IO-bound).
     * Threshold NBuffers/4 (heapam default) is wrong for PageAgg.
     */
    state->strategy  = (state->nblocks > (BlockNumber) NBuffers)
                       ? GetAccessStrategy(BAS_BULKREAD)
                       : NULL;

    /* result slot: int8 */
    {
        TupleDesc tdesc = CreateTemplateTupleDesc(1);
        TupleDescInitEntry(tdesc, 1, "result", INT8OID, -1, 0);
        TupleDescFinalize(tdesc);
        BlessTupleDesc(tdesc);
        node->ss.ps.ps_ResultTupleSlot =
            ExecAllocTableSlot(&estate->es_tupleTable, tdesc,
                               &TTSOpsVirtual, 0);
    }

    state->computed         = false;
    state->total_count      = 0;
    state->total_sum        = 0;
    /* pS2+pS3: load persistent summaries from side table */
    if (xpb_pageagg_persistent)
    {
        XpPageSummary *ps = xpb_load_summaries(state->rel, state->nblocks);
        if (ps != NULL)
        {
            state->page_summaries  = ps;
            state->summaries_built = true;
            elog(DEBUG2, "xp_batch: loaded persistent summaries for %s",
                 RelationGetRelationName(state->rel));
        }
        else
            elog(DEBUG2, "xp_batch: no valid summaries for %s (fallback)",
                 RelationGetRelationName(state->rel));
    }
    else
    {
        state->page_summaries    = NULL;
        state->summaries_built   = false;
    }
    state->blocks_scanned    = 0;
    state->blocks_all_visible = 0;
    state->blocks_skipped    = 0;
    state->blocks_fastcount  = 0;
    state->tuples_examined   = 0;
    state->tuples_passed     = 0;
    state->chunks_total      = 0;
    state->chunks_skipped    = 0;

    elog(DEBUG2, "xp_batch: XpPageAgg begin, rel=%s nblocks=%u "
         "agg=%d has_qual=%d",
         RelationGetRelationName(state->rel),
         state->nblocks, state->agg_type, (state->qual.npreds > 0));
}

static void
xppa_end(CustomScanState *node)
{
    XpPageAggState *state = (XpPageAggState *) node;
    if (BufferIsValid(state->vmbuf))
        ReleaseBuffer(state->vmbuf);
    if (state->strategy)
        FreeAccessStrategy(state->strategy);
    if (state->rel)
        table_close(state->rel, AccessShareLock);
}

static void
xppa_rescan(CustomScanState *node)
{
    XpPageAggState *state = (XpPageAggState *) node;
    state->computed = false;
    state->total_count = state->total_sum = 0;
    state->page_summaries   = NULL;
    state->summaries_built  = false;
    state->blocks_scanned   = state->blocks_all_visible = 0;
    state->blocks_skipped   = state->blocks_fastcount = 0;
    state->tuples_examined  = state->tuples_passed = 0;
    if (BufferIsValid(state->vmbuf))
    {
        ReleaseBuffer(state->vmbuf);
        state->vmbuf = InvalidBuffer;
    }
    /* strategy reused across rescans — owned by state until End */
}

static void
xppa_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
    XpPageAggState *state = (XpPageAggState *) node;
    char            buf[128];

    ExplainPropertyText("Page Aggregate", "XpPageAgg", es);
    ExplainPropertyText("Unit of work", "heap page (not tuple)", es);
    if (state->qual.npreds > 0)
    {
        static const char *sqop_names[] = {"<","<=","=",">=",">"};
        for (int i = 0; i < state->qual.npreds; i++)
        {
            const SimpleQualPred *p = &state->qual.preds[i];
            snprintf(buf, sizeof(buf), "col%d %s %d",
                     p->attno,
                     (p->op >= SQ_LT && p->op <= SQ_GT) ? sqop_names[p->op] : "?",
                     p->rhs);
            ExplainPropertyText("Pushed Pred", buf, es);
        }
    }

    if (es->analyze)
    {
        snprintf(buf, sizeof(buf), "%ld / %ld (%.0f%%)",
                 state->blocks_all_visible, state->blocks_scanned,
                 state->blocks_scanned > 0
                 ? 100.0 * state->blocks_all_visible / state->blocks_scanned
                 : 0.0);
        ExplainPropertyText("All-visible blocks", buf, es);

        snprintf(buf, sizeof(buf), "%ld / %ld",
                 state->tuples_passed, state->tuples_examined);
        ExplainPropertyText("Tuples passed/examined", buf, es);

        if (state->blocks_skipped > 0)
        {
            snprintf(buf, sizeof(buf), "%ld", state->blocks_skipped);
            ExplainPropertyText("Pages Skipped (pPD1)", buf, es);
        }
        if (state->blocks_fastcount > 0)
        {
            snprintf(buf, sizeof(buf), "%ld", state->blocks_fastcount);
            ExplainPropertyText("Pages FastCounted (pPD2)", buf, es);
        }
        if (state->chunks_total > 0)
        {
            snprintf(buf, sizeof(buf), "%ld / %ld (%.1f%%)",
                     state->chunks_skipped, state->chunks_total,
                     100.0 * state->chunks_skipped / state->chunks_total);
            ExplainPropertyText("Chunks Skipped (probe)", buf, es);
        }
    }
}

/* =========================================================
 * Registration
 * ========================================================= */

void xppa_register(void);

void
xppa_register(void)
{
    RegisterCustomScanMethods(&xppa_scan_methods);
}
