/*-------------------------------------------------------------------------
 *
 * xp_batch.c
 *    Batch execution provider (APF/XP p1–p3)
 *
 * p1: planner hooks — set_rel_pathlist_hook + create_upper_paths_hook
 * p2: planner→executor link — batch_method wired in xpb_begin()
 * p3: end-to-end smoke test — BatchState initialized, scan opened,
 *     xpb_exec_batch() drives ExecProcNodeBatch fallback loop
 *
 * Hot path invariant: ExecProcNode() is never modified.
 * The batch protocol lives alongside it as a parallel entry point.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/optimizer.h"   /* seq_page_cost, cpu_tuple_cost */

#include "access/tableam.h"
#include "catalog/pg_class.h"
#include "commands/explain_format.h"
#include "executor/execBatch.h"
#include "executor/executor.h"
#include "executor/execScan.h"
#include "executor/tuptable.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"

/* XpBatchSort — defined in xpb_sort.c */
extern void xpbs_register(void);

/* Policy P1 thresholds — exported for provider path addition */
extern int xpb_agg_fused_threshold;
extern int xpb_sort_fused_threshold;

/* p6a.2 fused drain — declared here, used in xpb_sort.c */
struct Tuplesortstate;
extern long xpb_drain_into_tuplesort(CustomScanState *css,
                                     struct Tuplesortstate *sortstate);

/* XpBatchAgg — defined in xpb_agg.c */
extern void xpba_register(void);

/* XpGroupAgg2 — composite StreamingAgg, defined in xpb_groupagg2.c */
extern bool xpb_groupagg2_enabled;
extern bool xpb_groupagg2_bitmap_enabled;
extern bool xpb_groupagg2_elastic_enabled;
extern int  xpb_groupagg2_elastic_cap;
extern bool   xpb_groupagg2_simd_enabled;
extern bool   xpb_groupagg2_local_partial;
extern int    xpb_groupagg2_local_hash_cap;
extern double xpb_cost_scan_page_factor;
extern double xpb_cost_scan_tuple_factor;
extern double xpb_cost_group_factor;
extern double xpb_cost_output_factor;
extern double xpb_cost_qual_factor;
extern void xpga2_register(void);
extern void xpga2_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                           RelOptInfo *output_rel, void *extra);

/* XpGroupAgg — defined in xpb_groupagg.c */
extern bool xpb_groupagg_enabled;
extern void xpga_register(void);
extern void xpga_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                          RelOptInfo *output_rel, void *extra);

/* XpColdNative — defined in xpb_coldnative.c */
extern bool xpb_coldnative_enabled;
extern int  xpb_coldnative_prefetch;
extern bool xpb_coldnative_readstream;
extern void cnative_register(void);
extern void cnative_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                             RelOptInfo *output_rel, void *extra);

/* XpPageAgg — defined in xpb_pageagg.c */
extern bool xpb_pageagg_enabled;
extern bool xpb_pageagg_persistent;
extern bool xpb_pageagg_summary;
extern bool xpb_pageagg_skip;
extern bool xpb_pageagg_fastcount;
extern void xppa_register(void);
extern void xppa_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                          RelOptInfo *output_rel, void *extra);

/* XpNativeBatch — defined in xpb_native.c */
extern bool xpb_native_agg_enabled;
extern int  xpb_native_batch_cap;
extern bool xpb_native_bypass_qual;
extern bool xpb_native_snap_any;
extern void xpbn_register(void);
extern void xpbn_agg_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                              RelOptInfo *output_rel, void *extra);

extern void xpba_add_path(PlannerInfo *root, RelOptInfo *input_rel,
                          RelOptInfo *output_rel, void *extra);
extern void xpbs_add_path(PlannerInfo *root,
                          RelOptInfo *input_rel,
                          RelOptInfo *output_rel);

#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "utils/guc.h"
#include "utils/tuplesort.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "xpb_source.h"   /* xpb_register_builtin_providers */

PG_MODULE_MAGIC;

/* ---------- tunables ---------- */
static bool xp_batch_enabled    = false;
static bool xp_batch_batchscan = false;
static int  xp_batch_size       = 1024;    /* tuples per batch */
bool        xp_batch_fused_drain  = false; /* p6a.2: fused dispatch compression */

/*
 * Policy P1: row thresholds for automatic fused path selection.
 *
 * Derived from sweep measurements (rows where fused < baseline stably):
 *   AGG:  crossover 300K→500K, conservative threshold = 500K
 *   SORT: fused never wins stably (tuplesort materialization dominates)
 *         threshold = 0 effectively disables auto-enable for Sort
 */
int          xpb_agg_fused_threshold  = 500000;
int          xpb_sort_fused_threshold = 0;      /* DISABLED: Sort fused not beneficial */

/* ---------- saved hooks ---------- */
static set_rel_pathlist_hook_type   prev_rel_pathlist  = NULL;
static create_upper_paths_hook_type prev_upper_paths   = NULL;

/* ---------- forward declarations ---------- */
static void xpb_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
                              Index rti, RangeTblEntry *rte);
static void xpb_upper_paths(PlannerInfo *root, UpperRelationKind stage,
                             RelOptInfo *input_rel, RelOptInfo *output_rel,
                             void *extra);

/* ---------- CustomPath methods ---------- */
static Plan *xpb_plan_scan_path(PlannerInfo *root, RelOptInfo *rel,
                                 CustomPath *best_path, List *tlist,
                                 List *clauses, List *custom_plans);

static const CustomPathMethods xpb_scan_path_methods = {
    .CustomName     = "XpBatchScan",
    .PlanCustomPath = xpb_plan_scan_path,
};

/* ---------- CustomScan methods ---------- */
static Node *xpb_create_scan_state(CustomScan *cscan);

static const CustomScanMethods xpb_scan_methods = {
    .CustomName            = "XpBatchScan",
    .CreateCustomScanState = xpb_create_scan_state,
};

/* ---------- CustomScanState ---------- */
typedef struct XpBatchScanState
{
    CustomScanState css;            /* must be first */

    /* p3: batch protocol state */
    BatchState      batch;          /* current batch buffer            */
    int             batch_pos;      /* next slot index to return       */
    bool            batch_exhausted;/* true after last batch           */

    /* p3: heap scan (lazy-opened in first exec call) */
    struct TableScanDescData *scandesc;
} XpBatchScanState;

/* ---------- exec methods ---------- */
static void             xpb_begin(CustomScanState *node, EState *estate,
                                   int eflags);
static TupleTableSlot  *xpb_exec(CustomScanState *node);
static void             xpb_end(CustomScanState *node);
static void             xpb_rescan(CustomScanState *node);
static void             xpb_explain(CustomScanState *node, List *ancestors,
                                     ExplainState *es);

static const CustomExecMethods xpb_exec_methods = {
    .CustomName      = "XpBatchScan",
    .BeginCustomScan = xpb_begin,
    .ExecCustomScan  = xpb_exec,
    .EndCustomScan   = xpb_end,
    .ReScanCustomScan= xpb_rescan,
    .ExplainCustomScan = xpb_explain,
};

/*
 * xpb_batch_method
 *
 * Native batch callback registered in PlanState.batch_method (p2 field).
 * Called by ExecProcNodeBatch() when batch_method != NULL.
 *
 * p3: drives table_scan_getnextslot() in a tight loop, filling batch->slots.
 * This is the "safe copy" path — no zero-copy, no TAM API changes.
 */
static void
xpb_batch_method(PlanState *pstate, BatchState *batch)
{
    XpBatchScanState   *state = (XpBatchScanState *) pstate;
    ScanDirection       dir   = pstate->state->es_direction;
    TupleTableSlot     *scan_slot = state->css.ss.ss_ScanTupleSlot;
    struct TableScanDescData *scandesc = state->scandesc;
    int                 n = 0;

    batch->nslots = 0;
    batch->done   = false;

    /* lazy open — same pattern as SeqScan */
    if (scandesc == NULL)
    {
        scandesc = table_beginscan(state->css.ss.ss_currentRelation,
                                   pstate->state->es_snapshot,
                                   0, NULL, 0);
        state->scandesc = scandesc;
        state->css.ss.ss_currentScanDesc = scandesc;
    }

    while (n < batch->capacity)
    {
        ExecClearTuple(scan_slot);

        if (!table_scan_getnextslot(scandesc, dir, scan_slot))
        {
            batch->done = true;
            break;
        }

        /*
         * Raw transport: no qual applied here.
         *
         * batch_method is an internal primitive for raw data movement
         * between known compatible providers. Scan semantics (qual,
         * projection) are NOT reproduced here — that would violate
         * contract #4 (dispatcher stays in core).
         *
         * Callers that need semantics-complete output must use either:
         *   - tuple-at-a-time: ExecProcNode → xpb_exec → ExecScanExtended
         *   - fused fast path: xpb_drain_into_tuplesort (qual applied there)
         */
        ExecCopySlot(batch->slots[n], scan_slot);
        n++;
    }

    batch->nslots = n;
}

/* =========================================================
 * _PG_init / _PG_fini
 * ========================================================= */

void _PG_init(void);
void _PG_fini(void);

void
_PG_init(void)
{
    /*
     * Before any GUC: the registry is what an operator asks for a source, so
     * the built-in providers must be there for the whole life of the backend.
     */
    xpb_register_builtin_providers();

    DefineCustomBoolVariable(
        "xp_batch.enabled",
        "Enable XP batch execution provider",
        NULL,
        &xp_batch_enabled,
        false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.batchscan",
        "Enable XpBatchScan scan path (default off)",
        NULL, &xp_batch_batchscan, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "xp_batch.batch_size",
        "Number of tuples per batch",
        NULL,
        &xp_batch_size,
        1024, 16, 65536,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "xp_batch.agg_fused_threshold",
        "Min rows for automatic AGG fused path (0=manual via fused_drain)",
        "Derived from crossover sweep: fused wins >= 500K for AGG",
        &xpb_agg_fused_threshold,
        500000, 0, INT_MAX,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "xp_batch.sort_fused_threshold",
        "Min rows for automatic SORT fused path (0=disabled)",
        "Sort fused not beneficial at any measured scale; disabled by default",
        &xpb_sort_fused_threshold,
        0, 0, INT_MAX,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.fused_drain",
        "p6a.2: fused XpBatchSort→XpBatchScan fast path",
        "Compresses dispatch stack: Sort calls Scan directly",
        &xp_batch_fused_drain,
        false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    /* Cost floor factors (GUC for runtime calibration) */
    DefineCustomRealVariable(
        "xp_batch.cost_scan_page_factor",
        "Scan page cost factor vs SeqScan (no slot overhead)",
        NULL, &xpb_cost_scan_page_factor, 0.95, 0.1, 1.0,
        PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomRealVariable(
        "xp_batch.cost_scan_tuple_factor",
        "Per-tuple scan cost factor (lighter batch processing)",
        NULL, &xpb_cost_scan_tuple_factor, 0.85, 0.1, 1.0,
        PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomRealVariable(
        "xp_batch.cost_group_factor",
        "Grouping cost factor (no ExprContext reset per tuple)",
        NULL, &xpb_cost_group_factor, 0.80, 0.1, 1.0,
        PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomRealVariable(
        "xp_batch.cost_output_factor",
        "Output emission cost factor (conservative, real bottleneck)",
        NULL, &xpb_cost_output_factor, 1.00, 0.1, 2.0,
        PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomRealVariable(
        "xp_batch.cost_qual_factor",
        "Pushed qual cost factor (scalar check, no dispatcher)",
        NULL, &xpb_cost_qual_factor, 0.90, 0.1, 1.0,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.groupagg2_simd",
        "Phase 3: use AVX-512 SIMD for LiveBitmap mask construction",
        NULL, &xpb_groupagg2_simd_enabled, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.groupagg2_local_partial",
        "LOCAL_PARTIAL: per-batch local hash for L1-friendly partial aggregation on composite keys",
        NULL, &xpb_groupagg2_local_partial, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "xp_batch.groupagg2_local_hash_cap",
        "LOCAL_PARTIAL: local hash table capacity (power of 2, target L1 cache)",
        NULL, &xpb_groupagg2_local_hash_cap, 2048, 256, 65536,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.groupagg2_bitmap",
        "Phase 2: use per-page LiveBitmap for Class2 predicates",
        NULL, &xpb_groupagg2_bitmap_enabled, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.groupagg2_elastic",
        "Elastic batch: accumulate across pages, flush on cap/semantic boundary",
        NULL, &xpb_groupagg2_elastic_enabled, false,
        PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomIntVariable(
        "xp_batch.groupagg2_elastic_cap",
        "Elastic batch row capacity",
        NULL, &xpb_groupagg2_elastic_cap, 4096, 256, 131072,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.groupagg2",
        "Enable XpGroupAgg2: composite (int4,int4) StreamingAgg probe",
        NULL, &xpb_groupagg2_enabled, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.groupagg",
        "Enable XpGroupAgg: property-carrying batch GROUP BY probe",
        NULL, &xpb_groupagg_enabled, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.coldnative",
        "Enable XpColdNative: prefetch + page loop + native sum",
        NULL, &xpb_coldnative_enabled, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.coldnative_readstream",
        "Variant A: use read_stream API (true async I/O pipeline)",
        NULL, &xpb_coldnative_readstream, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "xp_batch.coldnative_prefetch",
        "Prefetch lookahead distance in pages for XpColdNative",
        NULL, &xpb_coldnative_prefetch,
        16, 0, 256,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.pageagg_persistent",
        "pS2+pS3: use persistent page summaries from xp_page_summary",
        NULL, &xpb_pageagg_persistent, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.pageagg_summary",
        "Build page min/max/count summaries (pPD1+pPD2)",
        NULL, &xpb_pageagg_summary, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.pageagg_skip",
        "pPD1: skip pages using min/max summary",
        NULL, &xpb_pageagg_skip, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.pageagg_fastcount",
        "pPD2: use row_count summary for count(*)",
        NULL, &xpb_pageagg_fastcount, false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.pageagg",
        "Enable XpPageAgg — page-level aggregate probe",
        "pP measurement: page-by-page heap scan, no table_scan_getnextslot",
        &xpb_pageagg_enabled,
        false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.native_bypass_qual",
        "Exp 2: skip ExprState qual evaluation in NativeBatchScan",
        "Measures cost of qual ExprState machinery",
        &xpb_native_bypass_qual,
        false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.native_snap_any",
        "Exp 1: use SnapshotAny (skip MVCC visibility check)",
        "Measures MVCC visibility cost — NOT SAFE for production",
        &xpb_native_snap_any,
        false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "xp_batch.native_batch_cap",
        "Tuples per native drain call",
        "Exp 4: sweep 512..8192 to find optimal batch size",
        &xpb_native_batch_cap,
        4096, 64, 65536,
        PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "xp_batch.native_agg",
        "Enable NativeBatchScan -> NativeBatchAgg corridor",
        "pN measurement: no slot transport, count(*) native loop",
        &xpb_native_agg_enabled,
        false,
        PGC_USERSET, 0, NULL, NULL, NULL);

    MarkGUCPrefixReserved("xp_batch");
    RegisterCustomScanMethods(&xpb_scan_methods);
    xpbs_register();    /* XpBatchSort methods */
    xpba_register();    /* XpBatchAgg methods */
    xpbn_register();    /* XpNativeBatch methods */
    xppa_register();    /* XpPageAgg methods */
    cnative_register(); /* XpColdNative methods */
    xpga_register();    /* XpGroupAgg methods */
    xpga2_register();   /* XpGroupAgg2 methods */

    prev_rel_pathlist       = set_rel_pathlist_hook;
    set_rel_pathlist_hook   = xpb_rel_pathlist;

    prev_upper_paths        = create_upper_paths_hook;
    create_upper_paths_hook = xpb_upper_paths;

    elog(DEBUG1, "xp_batch: loaded");
}

void
_PG_fini(void)
{
    set_rel_pathlist_hook   = prev_rel_pathlist;
    create_upper_paths_hook = prev_upper_paths;
}

/* =========================================================
 * Hook: base rel (p1)
 * ========================================================= */

static void
xpb_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
                  Index rti, RangeTblEntry *rte)
{
    CustomPath *cpath;

    if (prev_rel_pathlist)
        prev_rel_pathlist(root, rel, rti, rte);

    if (!xp_batch_enabled || !xp_batch_batchscan)
        return;
    if (rel->reloptkind != RELOPT_BASEREL)
        return;
    if (rte->relkind != RELKIND_RELATION)
        return;

    /*
     * Applicability guard: XpBatchScan is a single-table scan path.
     * It is NOT safe as a scan leaf inside a join tree because
     * custom_scan_tlist may not include all Vars needed by join nodes.
     *
     * Conservative guard: if the query involves more than one
     * regular relation (i.e., has joins), decline to add XpBatchScan.
     * This matches the current capability envelope: heap-only,
     * single-table, aggregation-only paths.
     */
    {
        int n_relations = 0;
        ListCell *rlc;
        foreach(rlc, root->parse->rtable)
        {
            RangeTblEntry *other = (RangeTblEntry *) lfirst(rlc);
            if (other->rtekind == RTE_RELATION &&
                other->relkind == RELKIND_RELATION)
                n_relations++;
        }
        if (n_relations != 1)
            return;  /* join/subquery: XpBatchScan not safe */
    }

    cpath = makeNode(CustomPath);
    cpath->path.pathtype        = T_CustomScan;
    cpath->path.parent          = rel;
    cpath->path.pathtarget      = rel->reltarget;
    cpath->path.param_info      = NULL;
    cpath->path.parallel_aware  = false;
    cpath->path.parallel_safe   = rel->consider_parallel;
    cpath->path.parallel_workers = 0;
    cpath->path.pathkeys        = NIL;
    cpath->flags                = 0;
    cpath->custom_paths         = NIL;
    cpath->custom_private       = NIL;
    cpath->methods              = &xpb_scan_path_methods;

    /*
     * XpBatchScan cost: slightly above SeqScan.
     * XpBatchScan alone (under vanilla HashAgg) is not faster than SeqScan.
     * It wins only when downstream XpGroupAgg2 provides STREAM advantage.
     * XpGroupAgg2 uses its own scan loop — XpBatchScan is redundant there.
     * Cost penalty ensures vanilla SeqScan is preferred for scan-under-HashAgg.
     */
    {
        double relpages = (rel->pages > 0) ? (double)rel->pages : 1.0;
        cpath->path.rows         = rel->rows;
        cpath->path.startup_cost = 0.0;
        cpath->path.total_cost   = relpages * seq_page_cost
                                 + rel->rows * cpu_tuple_cost * 1.1;  /* 10% penalty */
    }

    add_path(rel, (Path *) cpath);
    elog(DEBUG2, "xp_batch: BatchScan path added for relid %u", rte->relid);
}

/* =========================================================
 * Hook: upper rels (p1)
 * ========================================================= */

static void
xpb_upper_paths(PlannerInfo *root, UpperRelationKind stage,
                 RelOptInfo *input_rel, RelOptInfo *output_rel,
                 void *extra)
{
    if (prev_upper_paths)
        prev_upper_paths(root, stage, input_rel, output_rel, extra);

    if (!xp_batch_enabled)
        return;
    if (stage == UPPERREL_ORDERED)
    {
        xpbs_add_path(root, input_rel, output_rel);
    }
    else if (stage == UPPERREL_GROUP_AGG)
    {
        elog(DEBUG2, "xp_batch: GROUP_AGG hook reached");
        xpba_add_path(root, input_rel, output_rel, extra);
        xpbn_agg_add_path(root, input_rel, output_rel, extra);
        xppa_add_path(root, input_rel, output_rel, extra);
        cnative_add_path(root, input_rel, output_rel, extra);
        xpga_add_path(root, input_rel, output_rel, extra);
        xpga2_add_path(root, input_rel, output_rel, extra);
        elog(DEBUG2, "xp_batch: GROUP_AGG hook done");
    }
    else
        return;

    elog(DEBUG2, "xp_batch: upper hook done stage=%d, input paths: %d",
         (int)stage, list_length(input_rel->pathlist));
}

/* =========================================================
 * CustomPath → Plan (p1)
 * ========================================================= */

static Plan *
xpb_plan_scan_path(PlannerInfo *root, RelOptInfo *rel,
                    CustomPath *best_path, List *tlist,
                    List *clauses, List *custom_plans)
{
    CustomScan *cscan;

    cscan = makeNode(CustomScan);

    cscan->methods              = &xpb_scan_methods;
    cscan->scan.plan.targetlist = tlist;

    cscan->scan.scanrelid =
        (best_path->path.parent ? best_path->path.parent->relid : 0);

    cscan->custom_plans         = custom_plans;
    /*
     * custom_scan_tlist = NIL: ExecInitCustomScan will use RelationGetDescr
     * for ss_ScanTupleSlot, giving the full physical tupdesc.
     * This is required for correct ExecCopySlot behavior in xpb_batch_method:
     * copying from a BufferHeapTuple into batch slots must use the full
     * relation tupdesc, not a projected subset.
     * Projection from physical to logical columns is handled by the
     * scan.plan.targetlist + ExecProject in ExecScanExtended.
     */
    cscan->custom_scan_tlist    = NIL;
    cscan->custom_private       = best_path->custom_private;

    /*
     * Pass restriction clauses into scan.plan.qual so ExecScanExtended
     * applies them via ExecQual. extract_actual_clauses strips
     * the RestrictInfo wrappers added by the planner.
     */
    cscan->scan.plan.qual = extract_actual_clauses(clauses, false);

    return (Plan *) cscan;
}

/* =========================================================
 * CustomScan state lifecycle (p2–p3)
 * ========================================================= */

static Node *
xpb_create_scan_state(CustomScan *cscan)
{
    XpBatchScanState *state = palloc0(sizeof(XpBatchScanState));

    NodeSetTag(state, T_CustomScanState);
    state->css.methods  = &xpb_exec_methods;
    /*
     * Tell ExecInitCustomScan to create the scan tuple slot with
     * TTSOpsBufferHeapTuple so table_scan_getnextslot() can fill it
     * directly from a heap buffer page.
     */
    state->css.slotOps  = &TTSOpsBufferHeapTuple;
    return (Node *) state;
}

/*
 * xpb_begin — p2/p3 link point.
 *
 * 1. Wire batch_method into PlanState (p2 field) — native batch path.
 * 2. Initialize BatchState with pre-allocated slots (p3).
 *
 * ss_currentRelation is already open at this point (ExecInitCustomScan
 * opens it before calling BeginCustomScan).
 */
static void
xpb_begin(CustomScanState *node, EState *estate, int eflags)
{
    XpBatchScanState *state  = (XpBatchScanState *) node;
    TupleDesc         tupdesc;

    /*
     * p3: use ss_ScanTupleSlot descriptor for batch slots.
     * ss_ScanTupleSlot is set up by ExecInitCustomScan from
     * custom_scan_tlist, NOT from RelationGetDescr.
     */
    tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

    /* p3: initialize BatchState with Virtual slots */
    ExecBatchStateInit(&state->batch,
                       estate->es_query_cxt,
                       tupdesc,
                       xp_batch_size);

    /* p2: register native batch callback — AFTER init (init zeroes method) */
    state->batch.method = xpb_batch_method;

    state->batch_pos       = 0;
    state->batch_exhausted = false;
    state->scandesc        = NULL;   /* lazy open in xpb_batch_method */

    /*
     * Wire scan tuple slot into ExprContext so ExecQual can find it.
     * ExecInitCustomScan already called ExecAssignExprContext().
     */
    node->ss.ps.ps_ExprContext->ecxt_scantuple =
        node->ss.ss_ScanTupleSlot;

    elog(DEBUG2, "xp_batch: begin, batch_size=%d", xp_batch_size);
}

/*
 * xpb_getnext — internal access method for ExecScanExtended.
 *
 * Returns the next tuple from the batch buffer, refilling via
 * ExecProcNodeBatch() when the current batch is drained.
 * Copies each tuple into ss_ScanTupleSlot so callers (Sort, Hash, etc.)
 * get a stable slot they can hold onto.
 */
static TupleTableSlot *
xpb_getnext(ScanState *ss)
{
    XpBatchScanState *state = (XpBatchScanState *) ss;

    for (;;)
    {
        /* refill batch if drained */
        if (state->batch_pos >= state->batch.nslots)
        {
            if (state->batch_exhausted)
                return NULL;

            ExecProcNodeBatch(&ss->ps, &state->batch);
            state->batch_pos = 0;

            if (state->batch.nslots == 0)
            {
                state->batch_exhausted = true;
                return NULL;
            }
            if (state->batch.done)
                state->batch_exhausted = true;
        }

        /*
         * Return the batch slot directly.
         * Batch slots are TTSOpsVirtual — they hold materialized copies
         * of the heap tuples and are stable across refill cycles
         * because ExecBatchStateInit pre-allocates them for the
         * lifetime of the scan.
         *
         * ExecScanExtended copies the slot pointer into econtext
         * and calls ExecProject if needed, which writes into
         * projInfo->pi_state.resultslot (a separate Virtual slot).
         * Sort/Hash receive that projection slot, not our batch slot.
         */
        return state->batch.slots[state->batch_pos++];
    }
}

static bool
xpb_recheck(ScanState *ss, TupleTableSlot *slot)
{
    /* no access-method-level conditions to recheck */
    return true;
}

/*
 * xpb_exec — tuple-at-a-time interface required by ExecCustomScan.
 *
 * Delegates to ExecScanExtended which handles qual + projection
 * correctly, including ResetExprContext per tuple and stable slot
 * ownership for upstream nodes (Sort, Hash, Materialize).
 */
static TupleTableSlot *
xpb_exec(CustomScanState *node)
{
    return ExecScanExtended(&node->ss,
                            xpb_getnext,
                            xpb_recheck,
                            node->ss.ps.state->es_epq_active,
                            node->ss.ps.qual,
                            node->ss.ps.ps_ProjInfo);
}

static void
xpb_end(CustomScanState *node)
{
    XpBatchScanState *state = (XpBatchScanState *) node;

    if (state->scandesc)
    {
        table_endscan(state->scandesc);
        state->scandesc = NULL;
    }
    ExecBatchStateRelease(&state->batch);
}

static void
xpb_rescan(CustomScanState *node)
{
    XpBatchScanState *state = (XpBatchScanState *) node;

    if (state->scandesc)
        table_rescan(state->scandesc, NULL);

    state->batch_pos       = 0;
    state->batch.nslots    = 0;
    state->batch.done      = false;
    state->batch_exhausted = false;
}

static void
xpb_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
    XpBatchScanState *state = (XpBatchScanState *) node;
    char              buf[64];

    ExplainPropertyText("Batch Provider", "XpBatch", es);

    snprintf(buf, sizeof(buf), "%d", xp_batch_size);
    ExplainPropertyText("Batch Size", buf, es);

    /*
     * Provider identity in EXPLAIN output (APF contract requirement).
     * Shows that batch_method is wired (p2 link confirmed).
     */
    ExplainPropertyText("Batch Method",
                        state->batch.method ? "native" : "fallback",
                        es);

    if (es->analyze)
    {
        snprintf(buf, sizeof(buf), "%d",
                 state->batch_pos > 0 ? state->batch_pos : state->batch.nslots);
        ExplainPropertyText("Last Batch Tuples", buf, es);
    }
}

/*
 * xpb_drain_into_tuplesort  (p6a.2 dispatch-compression fast path)
 *
 * Called by XpBatchSort when it recognises its child is XpBatchScan.
 * Fuses scan→sort without generic dispatch layers:
 *
 *   table_scan_getnextslot → [qual] → tuplesort_puttupleslot
 *
 * Scan semantics (qual, slot ops) remain inside XpBatchScan.
 * Sort consumer passes only the opaque sort state.
 *
 * Call depth vs batched path:
 *   Batched:  ExecProcNodeBatch → batch_method → getnextslot
 *   Fused:    xpb_drain_into_tuplesort → getnextslot        (−2 frames)
 *
 * Returns number of tuples fed into sort.
 */
long
xpb_drain_into_tuplesort(CustomScanState *css, Tuplesortstate *sortstate)
{
    XpBatchScanState   *state     = (XpBatchScanState *) css;
    PlanState          *pstate    = &css->ss.ps;
    TupleTableSlot     *scan_slot = css->ss.ss_ScanTupleSlot;
    ScanDirection       dir       = pstate->state->es_direction;
    struct TableScanDescData *scandesc;
    ExprContext        *econtext  = pstate->ps_ExprContext;
    long                n         = 0;

    /* lazy open — same as xpb_batch_method */
    scandesc = state->scandesc;
    if (scandesc == NULL)
    {
        scandesc = table_beginscan(css->ss.ss_currentRelation,
                                   pstate->state->es_snapshot,
                                   0, NULL, 0);
        state->scandesc              = scandesc;
        css->ss.ss_currentScanDesc   = scandesc;
    }

    for (;;)
    {
        ExecClearTuple(scan_slot);

        if (!table_scan_getnextslot(scandesc, dir, scan_slot))
            break;

        /* qual — scan semantics stay in XpBatchScan, not in Sort */
        if (pstate->qual)
        {
            ResetExprContext(econtext);
            econtext->ecxt_scantuple = scan_slot;
            if (!ExecQual(pstate->qual, econtext))
                continue;
        }

        /* direct feed: no intermediate slot copy */
        tuplesort_puttupleslot(sortstate, scan_slot);
        n++;
    }

    return n;
}
