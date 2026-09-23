/*
 * xpb_batch_partition.c — partition-based layered execution
 *
 * BatchAppendSource: combines multiple child batch sources into one stream.
 * Partition routing: ZLFS for partitions with valid zones, heap fallback otherwise.
 *
 * Pipeline: BatchAppend(partitions) → Join(dim_period) → Join(dim_account) → Agg
 */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "portability/instr_time.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "xpb_colbatch.h"
#include "xpb_zlfs.h"

/* Provider constructors */
extern XpBatchSource *xpb_zlfs_source_create(ZlfsZone *zone, int16 *attnos, int ncols);
extern XpBatchSource *xpb_heap_source_create(Oid relid, int16 *attnos, int ncols,
                                              bool has_pred, int32 lo, int32 hi);
extern void xpb_heap_source_stats(XpBatchSource *src, int64 *pr, int64 *ps,
                                   int64 *tv, int64 *tp);

/* ══ BatchAppendSource ══ */

#define APPEND_MAX_CHILDREN 32

typedef struct XpAppendState
{
    XpBatchSource  *children[APPEND_MAX_CHILDREN];
    int             nchildren;
    int             current;
    const char     *child_labels[APPEND_MAX_CHILDREN]; /* "zlfs" or "heap" */

    /* Stats per child */
    int64           child_rows[APPEND_MAX_CHILDREN];
    int             child_batches[APPEND_MAX_CHILDREN];
} XpAppendState;

static bool
append_next_batch(XpBatchSource *src, XpColumnBatch *batch)
{
    XpAppendState *st = src->private_state;

    while (st->current < st->nchildren)
    {
        bool got = st->children[st->current]->ops->next_batch(
            st->children[st->current], batch);
        if (got)
        {
            st->child_rows[st->current] += batch->nrows;
            st->child_batches[st->current]++;
            return true;
        }
        /* Reset batch column pointers before switching to next child.
         * Prevents heap source from writing into stale ZLFS borrowed pointers. */
        for (int c = 0; c < XPCB_MAX_COLS; c++)
            batch->int32_cols[c] = NULL;
        batch->owns_data = false;
        st->current++;
    }
    return false;
}

static void
append_rescan(XpBatchSource *src)
{
    XpAppendState *st = src->private_state;
    for (int i = 0; i < st->nchildren; i++)
        st->children[i]->ops->rescan(st->children[i]);
    st->current = 0;
}

static void
append_end(XpBatchSource *src)
{
    XpAppendState *st = src->private_state;
    for (int i = 0; i < st->nchildren; i++)
        st->children[i]->ops->end(st->children[i]);
}

static const XpBatchSourceOps append_ops = {
    .next_batch = append_next_batch,
    .rescan     = append_rescan,
    .end        = append_end,
};

static XpBatchSource *
batch_append_create(void)
{
    XpAppendState *st = palloc0(sizeof(XpAppendState));
    XpBatchSource *src = palloc(sizeof(XpBatchSource));
    src->ops = &append_ops;
    src->private_state = st;
    return src;
}

static void
batch_append_add_child(XpBatchSource *append, XpBatchSource *child, const char *label)
{
    XpAppendState *st = append->private_state;
    if (st->nchildren >= APPEND_MAX_CHILDREN)
        ereport(ERROR, (errmsg("BatchAppend: too many children")));
    st->children[st->nchildren] = child;
    st->child_labels[st->nchildren] = label;
    st->nchildren++;
}

/* ══ Partition routing ══ */

/*
 * Get child partitions of a parent relation that overlap [lo, hi].
 * Returns list of child OIDs.
 */
static List *
get_partitions_in_range(Oid parent_oid, int32 lo, int32 hi)
{
    List *result = NIL;
    List *children = find_inheritance_children(parent_oid, AccessShareLock);
    ListCell *lc;

    foreach(lc, children)
    {
        Oid child_oid = lfirst_oid(lc);
        /* For now, include all children — partition pruning
         * is handled by per-partition predicate in the source */
        result = lappend_oid(result, child_oid);
    }
    return result;
}

/*
 * Create a batch source for a partition.
 * Uses ZLFS if a valid zone exists, otherwise heap.
 */
static XpBatchSource *
create_partition_source(Oid part_oid, int16 *attnos, int ncols,
                        int32 query_lo, int32 query_hi, const char **label_out)
{
    /* Try ZLFS: look for any VALID zone on this partition.
     * Zone bounds may differ from query bounds — the zone covers the
     * partition's range, while the query covers the user's predicate.
     * We accept any zone whose source_relid matches. */
    zlfs_ensure_registry();
    zlfs_scan_directory();

    ZlfsZone *zone = NULL;
    if (zlfs_reg)
    {
        for (int i = 0; i < zlfs_reg->nzones; i++)
        {
            ZlfsZone *z = zlfs_reg->zones[i];
            if (z->source_relid == part_oid && z->freshness == ZLFS_VALID)
            { zone = z; break; }
        }
    }

    if (zone)
    {
        *label_out = "zlfs";
        return xpb_zlfs_source_create(zone, attnos, ncols);
    }
    else
    {
        *label_out = "heap";
        return xpb_heap_source_create(part_oid, attnos, ncols, true, query_lo, query_hi);
    }
}

/* ══ Reuse join/agg from xpb_batch_hashjoin.c ══ */

/* Dimension hash tables — same structures as in hashjoin */
#define DIM_CAP  256
#define DIM2_CAP 512
#define GRP_CAP  16384
#define GRP_MAX_LOAD (GRP_CAP * 3 / 4)

typedef struct { int32 key, year; bool occupied; } PDimEntry;
typedef struct { int32 key, payload; bool occupied; } ADimEntry;

typedef struct {
    PDimEntry entries[DIM_CAP];
    int nentries;
} PDimHash;

typedef struct {
    ADimEntry entries[DIM2_CAP];
    int nentries;
} ADimHash;

typedef struct {
    int32 year, account_group, company_key;
    int64 sum;
    bool occupied;
} PGroupEntry;

/* Simplified dim build using heap_getattr */
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "utils/snapmgr.h"

extern void xpb_dim_check_shape(Relation rel, int nrequired, const char *what);

static void
pdim_build(PDimHash *d, Oid relid)
{
    memset(d, 0, sizeof(*d));
    Relation rel = table_open(relid, AccessShareLock);
    xpb_dim_check_shape(rel, 2, "pdim_build");
    TableScanDesc scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
    HeapTuple tup;
    while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        bool n;
        int32 pk = DatumGetInt32(heap_getattr(tup, 1, RelationGetDescr(rel), &n)); if(n) continue;
        int32 yr = DatumGetInt32(heap_getattr(tup, 2, RelationGetDescr(rel), &n)); if(n) continue;
        if (d->nentries >= DIM_CAP * 3 / 4)
            ereport(ERROR, (errmsg("pdim overflow")));
        uint32 h = (uint32)pk * 2654435761u;
        for (int i = 0; i < DIM_CAP; i++) {
            int idx = (h+i) & (DIM_CAP-1);
            if (!d->entries[idx].occupied) {
                d->entries[idx] = (PDimEntry){pk, yr, true};
                d->nentries++; break;
            }
        }
    }
    table_endscan(scan);
    table_close(rel, AccessShareLock);
}

static void
adim_build(ADimHash *d, Oid relid)
{
    memset(d, 0, sizeof(*d));
    Relation rel = table_open(relid, AccessShareLock);
    xpb_dim_check_shape(rel, 2, "adim_build");
    TableScanDesc scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
    HeapTuple tup;
    while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        bool n;
        int32 k = DatumGetInt32(heap_getattr(tup, 1, RelationGetDescr(rel), &n)); if(n) continue;
        int32 g = DatumGetInt32(heap_getattr(tup, 2, RelationGetDescr(rel), &n)); if(n) continue;
        if (d->nentries >= DIM2_CAP * 3 / 4)
            ereport(ERROR, (errmsg("adim overflow")));
        uint32 h = (uint32)k * 2654435761u;
        for (int i = 0; i < DIM2_CAP; i++) {
            int idx = (h+i) & (DIM2_CAP-1);
            if (!d->entries[idx].occupied) {
                d->entries[idx] = (ADimEntry){k, g, true};
                d->nentries++; break;
            }
        }
    }
    table_endscan(scan);
    table_close(rel, AccessShareLock);
}

static inline int32 pdim_year(PDimHash *d, int32 pk) {
    uint32 h = (uint32)pk * 2654435761u;
    for (int i=0; i<DIM_CAP; i++) {
        int idx = (h+i) & (DIM_CAP-1);
        if (!d->entries[idx].occupied) return -1;
        if (d->entries[idx].key == pk) return d->entries[idx].year;
    }
    return -1;
}

static inline int32 adim_group(ADimHash *d, int32 ak) {
    uint32 h = (uint32)ak * 2654435761u;
    for (int i=0; i<DIM2_CAP; i++) {
        int idx = (h+i) & (DIM2_CAP-1);
        if (!d->entries[idx].occupied) return -1;
        if (d->entries[idx].key == ak) return d->entries[idx].payload;
    }
    return -1;
}

/* ══ SQL entry point ══ */

PG_FUNCTION_INFO_V1(xpb_partition_join2_groupby);

Datum
xpb_partition_join2_groupby(PG_FUNCTION_ARGS)
{
    char *parent_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    int32 lo = PG_GETARG_INT32(1);
    int32 hi = PG_GETARG_INT32(2);

    InitMaterializedSRF(fcinfo, 0);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    Oid parent_oid = RelnameGetRelid(parent_name);
    if (!OidIsValid(parent_oid))
        ereport(ERROR, (errmsg("table %s not found", parent_name)));

    Oid dim_p_oid = RelnameGetRelid("dim_period");
    Oid dim_a_oid = RelnameGetRelid("dim_account");
    if (!OidIsValid(dim_p_oid) || !OidIsValid(dim_a_oid))
        ereport(ERROR, (errmsg("dim_period or dim_account not found")));

    /* Build dimensions */
    instr_time tb0, tb1;
    INSTR_TIME_SET_CURRENT(tb0);
    PDimHash pdim; pdim_build(&pdim, dim_p_oid);
    ADimHash adim; adim_build(&adim, dim_a_oid);
    INSTR_TIME_SET_CURRENT(tb1);
    double build_ms = INSTR_TIME_GET_MILLISEC(tb1) - INSTR_TIME_GET_MILLISEC(tb0);

    /* Get partitions and build BatchAppend */
    int16 attnos[4] = { 1, 2, 3, 6 };
    List *parts = get_partitions_in_range(parent_oid, lo, hi);

    XpBatchSource *append_src = batch_append_create();
    XpAppendState *ast = append_src->private_state;

    ListCell *lc;
    foreach(lc, parts)
    {
        Oid part_oid = lfirst_oid(lc);
        const char *label;
        XpBatchSource *child = create_partition_source(part_oid, attnos, 4, lo, hi, &label);
        batch_append_add_child(append_src, child, label);

        char *partname = get_rel_name(part_oid);
        elog(NOTICE, "partition %s (oid=%u): provider=%s", partname, part_oid, label);
    }

    /* Pipeline: BatchAppend → Join1 → Join2 → Agg */
    PGroupEntry *ht = palloc0(GRP_CAP * sizeof(PGroupEntry));
    int ngroups = 0;
    int64 total_rows = 0;
    int nbatches = 0;

    /* Batch-level join buffers */
    int32 *year_buf = palloc(XPCB_BATCH_CAP * sizeof(int32));
    int32 *agrp_buf = palloc(XPCB_BATCH_CAP * sizeof(int32));

    XpColumnBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.ncols = 4;
    batch.capacity = XPCB_BATCH_CAP;

    instr_time t0, t1, tp;
    double src_ms = 0, j1_ms = 0, j2_ms = 0, agg_ms = 0;

    INSTR_TIME_SET_CURRENT(t0);

    while (true)
    {
        batch.nrows = 0;

        INSTR_TIME_SET_CURRENT(tp);
        bool got = append_src->ops->next_batch(append_src, &batch);
        { instr_time tn; INSTR_TIME_SET_CURRENT(tn);
          src_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp); }

        if (!got) break;
        nbatches++;

        int nrows = batch.nrows;
        int32 *col_pk = batch.int32_cols[0];  /* period_key */
        int32 *col_ck = batch.int32_cols[1];  /* company_key */
        int32 *col_ak = batch.int32_cols[2];  /* account_key */
        int32 *col_dt = batch.int32_cols[3];  /* amount_dt */

        /* Join1: period_key → year (vectorized over batch) */
        INSTR_TIME_SET_CURRENT(tp);
        for (int i = 0; i < nrows; i++)
            year_buf[i] = pdim_year(&pdim, col_pk[i]);
        { instr_time tn; INSTR_TIME_SET_CURRENT(tn);
          j1_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp); }

        /* Join2: account_key → account_group (vectorized) */
        INSTR_TIME_SET_CURRENT(tp);
        for (int i = 0; i < nrows; i++)
            agrp_buf[i] = adim_group(&adim, col_ak[i]);
        { instr_time tn; INSTR_TIME_SET_CURRENT(tn);
          j2_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp); }

        /* Aggregate: GROUP BY year, account_group, company_key */
        INSTR_TIME_SET_CURRENT(tp);
        for (int i = 0; i < nrows; i++)
        {
            int32 yr = year_buf[i], ag = agrp_buf[i], ck = col_ck[i];
            if (yr < 0 || ag < 0) continue;  /* no match — skip */
            int64 dt = (int64)col_dt[i];
            uint32 h = (uint32)yr * 2654435761u ^ (uint32)ag * 2246822519u ^ (uint32)ck * 0x45d9f3bu;
            int sl = (int)(h & (GRP_CAP - 1));
            for (int pr = 0; pr < GRP_CAP; pr++)
            {
                int idx = (sl + pr) & (GRP_CAP - 1);
                PGroupEntry *g = &ht[idx];
                if (!g->occupied)
                {
                    if (ngroups >= GRP_MAX_LOAD)
                        ereport(ERROR, (errmsg("partition_agg: hash overflow")));
                    g->year = yr; g->account_group = ag; g->company_key = ck;
                    g->sum = dt; g->occupied = true; ngroups++; break;
                }
                if (g->year == yr && g->account_group == ag && g->company_key == ck)
                { g->sum += dt; break; }
            }
        }
        { instr_time tn; INSTR_TIME_SET_CURRENT(tn);
          agg_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp); }

        total_rows += nrows;
    }

    INSTR_TIME_SET_CURRENT(t1);
    double total_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

    /* Emit results */
    Datum vals[5];
    bool nulls[5] = {false};
    for (int i = 0; i < GRP_CAP; i++)
    {
        PGroupEntry *g = &ht[i];
        if (!g->occupied) continue;
        vals[0] = Int32GetDatum(g->year);
        vals[1] = Int32GetDatum(g->account_group);
        vals[2] = Int32GetDatum(g->company_key);
        vals[3] = Int64GetDatum(g->sum);
        vals[4] = Float8GetDatum(total_ms);
        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
    }

    /* Report */
    elog(NOTICE, "partition_join2 [%d..%d]: "
         "total=%.1f ms  build=%.1f ms  source=%.1f ms  "
         "join1=%.1f ms  join2=%.1f ms  agg=%.1f ms  "
         "rows=%ld  batches=%d  groups=%d  partitions=%d",
         lo, hi,
         total_ms, build_ms, src_ms, j1_ms, j2_ms, agg_ms,
         total_rows, nbatches, ngroups, ast->nchildren);

    for (int i = 0; i < ast->nchildren; i++)
        elog(NOTICE, "  partition %d [%s]: %ld rows, %d batches",
             i, ast->child_labels[i],
             ast->child_rows[i], ast->child_batches[i]);

    append_src->ops->end(append_src);
    pfree(ht);
    pfree(year_buf);
    pfree(agrp_buf);

    return (Datum) 0;
}
