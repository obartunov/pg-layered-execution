/*
 * xpb_batch_hashjoin.c — BatchHashJoin over compact column batches
 *
 * Build side: small dimension table loaded into compact hash table.
 * Probe side: XpColumnBatch from any provider.
 * Output: XpColumnBatch with appended dimension columns.
 *
 * SQL entry: xpb_batch_join_groupby(lo, hi, source_mode)
 *   Runs: source → BatchHashJoin(dim_period) → BatchGroupAgg
 */
#include "postgres.h"
#include "funcapi.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "portability/instr_time.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "xpb_colbatch.h"
#include "xpb_zlfs.h"
#include "utils/tuplestore.h"

extern XpBatchSource *xpb_zlfs_source_create(ZlfsZone *zone, int16 *requested_attnos, int ncols);
extern XpBatchSource *xpb_heap_source_create(Oid relid, int16 *requested_attnos, int ncols,
                                              bool has_pred, int32 pred_lo, int32 pred_hi);
extern XpBatchSource *xpcn_source_create(Oid relid, int16 *requested_attnos, int ncols,
                                         bool has_pred, int32 pred_lo, int32 pred_hi);
extern void xpcn_source_stats(XpBatchSource *src, int64 *groups, int64 *groups_copied,
                              int64 *rows, int64 *bytes_copied);

/* ── Dimension hash table (build side) ── */

#define DIM_CAP 256

typedef struct DimEntry
{
    int32   key;        /* period_key */
    int32   year;       /* payload */
    bool    occupied;
} DimEntry;

typedef struct DimHashTable
{
    DimEntry entries[DIM_CAP];
    int      nentries;
} DimHashTable;

/*
 * The dimension readers address attributes by fixed attnum, which is the
 * contract the benchmark schema satisfies.  A table that does not satisfy it
 * must be refused here: heap_getattr() past natts reaches getmissingattr(),
 * which dereferences attrmiss for an attribute that has no entry and crashes
 * the backend.  A dropped attribute is equally unusable -- its values are gone
 * and its type is 0 -- and a non-int4 attribute would be read as an int4 Datum,
 * which silently returns nonsense for a by-reference type.
 *
 * Only the attnums actually READ are required.  dim_build used to read attnum 3
 * of dim_period into a `quarter` field that nothing ever consumed, which turned
 * a third column into a load-bearing requirement for no reason -- and the
 * original benchmark schema has `month` there, not a quarter.
 *
 * Not static: xpb_batch_partition.c builds the same dimensions.
 */
void
xpb_dim_check_shape(Relation rel, int nrequired, const char *what)
{
    TupleDesc   desc = RelationGetDescr(rel);

    if (desc->natts < nrequired)
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("%s: \"%s\" has %d columns, %d required",
                        what, RelationGetRelationName(rel),
                        desc->natts, nrequired)));

    for (int i = 0; i < nrequired; i++)
    {
        Form_pg_attribute att = TupleDescAttr(desc, i);

        if (att->attisdropped)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("%s: \"%s\" column %d is dropped",
                            what, RelationGetRelationName(rel), i + 1)));
        if (att->atttypid != INT4OID)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("%s: \"%s\".%s is type %u, integer required",
                            what, RelationGetRelationName(rel),
                            NameStr(att->attname), att->atttypid)));
    }
}

/*
 * Same check for an explicit list of attnums rather than the first N: the fact
 * table is read at 1,2,3,6,7, so "the first five columns" would be the wrong
 * question to ask of it.
 *
 * Not static: the 1C register report checks both its fact tables with it.
 */
void
xpb_check_attnos(Relation rel, const int16 *attnos, int n, const char *what)
{
    TupleDesc   desc = RelationGetDescr(rel);

    for (int i = 0; i < n; i++)
    {
        int16       attno = attnos[i];
        Form_pg_attribute att;

        if (attno < 1 || attno > desc->natts)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("%s: attno %d out of range (1..%d) for \"%s\"",
                            what, attno, desc->natts,
                            RelationGetRelationName(rel))));
        att = TupleDescAttr(desc, attno - 1);
        if (att->attisdropped)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("%s: \"%s\" attno %d is dropped",
                            what, RelationGetRelationName(rel), attno)));
        if (att->atttypid != INT4OID)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("%s: \"%s\".%s is type %u, integer required",
                            what, RelationGetRelationName(rel),
                            NameStr(att->attname), att->atttypid)));
    }
}

static void
dim_build(DimHashTable *dim, Oid dim_relid)
{
    memset(dim, 0, sizeof(*dim));

    Relation rel = table_open(dim_relid, AccessShareLock);
    xpb_dim_check_shape(rel, 2, "dim_build");
    Snapshot snap = GetActiveSnapshot();
    TableScanDesc scan = table_beginscan(rel, snap, 0, NULL, 0);
    HeapTuple tup;

    while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        bool isnull;
        int32 pk = DatumGetInt32(heap_getattr(tup, 1, RelationGetDescr(rel), &isnull));
        if (isnull) continue;
        int32 yr = DatumGetInt32(heap_getattr(tup, 2, RelationGetDescr(rel), &isnull));
        if (isnull) continue;

        if (dim->nentries >= DIM_CAP * 3 / 4)
            ereport(ERROR, (errmsg("dim_build: hash overflow (%d entries, cap %d)",
                                    dim->nentries, DIM_CAP)));

        uint32 h = (uint32)pk * 2654435761u;
        for (int i = 0; i < DIM_CAP; i++)
        {
            int idx = (h + i) & (DIM_CAP - 1);
            if (!dim->entries[idx].occupied)
            {
                dim->entries[idx].key = pk;
                dim->entries[idx].year = yr;
                dim->entries[idx].occupied = true;
                dim->nentries++;
                break;
            }
        }
    }
    table_endscan(scan);
    table_close(rel, AccessShareLock);
}

static inline DimEntry *
dim_lookup(DimHashTable *dim, int32 key)
{
    uint32 h = (uint32)key * 2654435761u;
    for (int i = 0; i < DIM_CAP; i++)
    {
        int idx = (h + i) & (DIM_CAP - 1);
        DimEntry *e = &dim->entries[idx];
        if (!e->occupied) return NULL;
        if (e->key == key) return e;
    }
    return NULL;
}

/* ── Batch hash join: probe + append dimension column ── */

/*
 * Input batch columns:  [0]=period_key, [1]=company_key, [2]=amount_dt
 * Output batch columns: [0]=year, [1]=company_key, [2]=amount_dt
 *
 * This is a replace-in-place join for the benchmark query:
 * period_key is consumed by the join and replaced with year.
 * Inner join: rows without match are dropped via selection vector.
 */

typedef struct BatchJoinState
{
    DimHashTable   *dim;
    int32          *year_buf;   /* output column buffer */
    uint32         *sel_buf;    /* selection vector */
    int             capacity;
} BatchJoinState;

static BatchJoinState *
batch_join_create(DimHashTable *dim, int capacity)
{
    BatchJoinState *js = palloc(sizeof(BatchJoinState));
    js->dim = dim;
    js->year_buf = palloc(capacity * sizeof(int32));
    js->sel_buf = palloc(capacity * sizeof(uint32));
    js->capacity = capacity;
    return js;
}

/*
 * Probe batch against dimension, produce output batch.
 * Input:  batch with cols [period_key, company_key, amount_dt]
 * Output: out_batch with cols [year, company_key, amount_dt]
 */
static void
batch_join_probe(BatchJoinState *js, XpColumnBatch *in, XpColumnBatch *out)
{
    int nrows = in->nrows;
    int32 *col_pk = xpcb_i32(in, 0);        /* dispatch once, not per row */
    int nsel = 0;

    /* Probe: lookup each period_key, build selection + year column */
    for (int i = 0; i < nrows; i++)
    {
        DimEntry *e = dim_lookup(js->dim, col_pk[i]);
        if (e)
        {
            js->sel_buf[nsel] = i;
            js->year_buf[nsel] = e->year;
            nsel++;
        }
    }

    /* Build output batch: replace col0 (join key) with year, pass through rest */
    out->nrows = nsel;
    out->ncols = in->ncols;

    /*
     * col 0 is always BORROWED from the join state's year_buf, whatever
     * happens to the other columns.  Under the old batch-level owns_data this
     * was not expressible: the gather branch below set owns_data = true for
     * the whole batch while col 0 still pointed into js->year_buf, so a
     * consumer that had honoured that flag would have pfree()d memory the
     * join state owns and reuses on the next batch.  Per-column ownership is
     * what makes the mixed case sayable.
     */
    xpcb_col_borrow(&out->cols[0], XPB_COL_INT4, js->year_buf, NULL);

    if (nsel == nrows)
    {
        /* All rows matched -- borrow the passthrough columns directly */
        for (int c = 1; c < in->ncols; c++)
            xpcb_col_borrow(&out->cols[c], in->cols[c].type,
                            in->cols[c].data, in->cols[c].validity);
    }
    else
    {
        /* Partial match -- gather selected rows into owned buffers */
        for (int c = 1; c < in->ncols; c++)
        {
            int32  *src = xpcb_i32(in, c);
            int32  *buf = palloc(nsel * sizeof(int32));

            for (int i = 0; i < nsel; i++)
                buf[i] = src[js->sel_buf[i]];

            out->cols[c].type = XPB_COL_INT4;
            out->cols[c].data = buf;
            out->cols[c].validity = NULL;
            out->cols[c].owns_data = true;
            out->cols[c].owns_validity = false;
        }
    }
}

/* ── Group aggregate over join output ── */

#define GRP_CAP      16384
#define GRP_MAX_LOAD (GRP_CAP * 3 / 4)

typedef struct JGroupEntry
{
    int32   year;
    int32   company_key;
    int64   sum;
    bool    occupied;
} JGroupEntry;

/* ── SQL entry point ── */

PG_FUNCTION_INFO_V1(xpb_batch_join_groupby);

Datum
xpb_batch_join_groupby(PG_FUNCTION_ARGS)
{
    int32 lo = PG_GETARG_INT32(0);
    int32 hi = PG_GETARG_INT32(1);
    char *mode = text_to_cstring(PG_GETARG_TEXT_PP(2));

    InitMaterializedSRF(fcinfo, 0);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    Oid fact_relid = RelnameGetRelid("reg_buh");
    Oid dim_relid = RelnameGetRelid("dim_period");
    if (!OidIsValid(fact_relid) || !OidIsValid(dim_relid))
        ereport(ERROR, (errmsg("reg_buh or dim_period not found")));

    /* Build dimension hash table */
    instr_time tb0, tb1;
    INSTR_TIME_SET_CURRENT(tb0);
    DimHashTable dim;
    dim_build(&dim, dim_relid);
    INSTR_TIME_SET_CURRENT(tb1);
    double build_ms = INSTR_TIME_GET_MILLISEC(tb1) - INSTR_TIME_GET_MILLISEC(tb0);

    /* Create source */
    XpBatchSource *source;
    if (strcmp(mode, "zlfs") == 0)
    {
        zlfs_ensure_registry();
        zlfs_scan_directory();
        ZlfsZone *zone = zlfs_lookup_valid_zone(fact_relid, lo, hi);
        if (!zone)
            ereport(ERROR, (errmsg("no valid ZLFS zone for [%d..%d]", lo, hi)));
        int16 req_attnos[3] = { 1, 2, 6 };  /* period_key, company_key, amount_dt */
        source = xpb_zlfs_source_create(zone, req_attnos, 3);
    }
    else if (strcmp(mode, "heap") == 0)
    {
        int16 heap_attnos[3] = { 1, 2, 6 };  /* period_key, company_key, amount_dt */
        source = xpb_heap_source_create(fact_relid, heap_attnos, 3, true, lo, hi);
    }
    else
        ereport(ERROR, (errmsg("unknown mode: %s", mode)));

    /* Pipeline: source → join → aggregate */
    BatchJoinState *js = batch_join_create(&dim, XPCB_BATCH_CAP);
    JGroupEntry *ht = palloc0(GRP_CAP * sizeof(JGroupEntry));
    int ngroups = 0;
    int64 total_rows = 0;
    int nbatches = 0;

    XpColumnBatch in_batch;
    memset(&in_batch, 0, sizeof(in_batch));
    in_batch.ncols = 3;
    in_batch.capacity = XPCB_BATCH_CAP;

    XpColumnBatch join_out;
    memset(&join_out, 0, sizeof(join_out));

    instr_time t0, t1;
    double source_accum = 0, join_accum = 0, agg_accum = 0;
    instr_time tp;

    INSTR_TIME_SET_CURRENT(t0);

    while (true)
    {
        in_batch.nrows = 0;

        INSTR_TIME_SET_CURRENT(tp);
        bool got = source->ops->next_batch(source, &in_batch);
        {
            instr_time tn; INSTR_TIME_SET_CURRENT(tn);
            source_accum += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);
        }

        if (!got) break;
        nbatches++;

        /* Join */
        INSTR_TIME_SET_CURRENT(tp);
        batch_join_probe(js, &in_batch, &join_out);
        {
            instr_time tn; INSTR_TIME_SET_CURRENT(tn);
            join_accum += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);
        }

        /* Aggregate: GROUP BY year, company_key */
        INSTR_TIME_SET_CURRENT(tp);
        int nrows = join_out.nrows;
        int32 *col_yr = xpcb_i32(&join_out, 0);
        int32 *col_ck = xpcb_i32(&join_out, 1);
        int32 *col_dt = xpcb_i32(&join_out, 2);

        for (int i = 0; i < nrows; i++)
        {
            int32 yr = col_yr[i], ck = col_ck[i];
            int64 dt = (int64)col_dt[i];
            uint32 h = (uint32)yr * 2654435761u ^ (uint32)ck * 2246822519u;
            int sl = (int)(h & (GRP_CAP - 1));
            for (int pr = 0; pr < GRP_CAP; pr++)
            {
                int idx = (sl + pr) & (GRP_CAP - 1);
                JGroupEntry *g = &ht[idx];
                if (!g->occupied)
                {
                    if (ngroups >= GRP_MAX_LOAD)
                        ereport(ERROR, (errmsg("join_agg: hash overflow")));
                    g->year = yr; g->company_key = ck; g->sum = dt;
                    g->occupied = true; ngroups++; break;
                }
                if (g->year == yr && g->company_key == ck)
                { g->sum += dt; break; }
            }
        }
        {
            instr_time tn; INSTR_TIME_SET_CURRENT(tn);
            agg_accum += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);
        }

        total_rows += nrows;

        /* Free whatever the join gathered. col 0 is borrowed from the join
         * state's year_buf, so its flag keeps it out of this. */
        xpcb_release_owned(&join_out);
    }

    INSTR_TIME_SET_CURRENT(t1);
    double total_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

    /* Emit results */
    Datum vals[4];
    bool nulls[4] = {false, false, false, false};
    for (int i = 0; i < GRP_CAP; i++)
    {
        JGroupEntry *g = &ht[i];
        if (!g->occupied) continue;
        vals[0] = Int32GetDatum(g->year);
        vals[1] = Int32GetDatum(g->company_key);
        vals[2] = Int64GetDatum(g->sum);
        vals[3] = Float8GetDatum(total_ms);
        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
    }

    elog(NOTICE, "batch_join_groupby [%d..%d] mode=%s: "
         "total=%.1f ms  build=%.1f ms  source=%.1f ms  join=%.1f ms  agg=%.1f ms  "
         "rows=%ld  batches=%d  groups=%d  dim=%d",
         lo, hi, mode,
         total_ms, build_ms, source_accum, join_accum, agg_accum,
         total_rows, nbatches, ngroups, dim.nentries);

    source->ops->end(source);
    pfree(ht);
    pfree(js->year_buf);
    pfree(js->sel_buf);
    pfree(js);

    return (Datum) 0;
}

/* ══════════════════════════════════════════════════════════════
 * Two-join pipeline:
 *   Source → BatchHashJoin(dim_period) → BatchHashJoin(dim_account) → Agg
 *
 * Input batch from source:  [period_key, company_key, account_key, amount_dt]
 * After join 1 (dim_period): [year, company_key, account_key, amount_dt]
 * After join 2 (dim_account): [year, account_group, company_key, amount_dt]
 * Aggregate: GROUP BY year, account_group, company_key → SUM(amount_dt)
 * ══════════════════════════════════════════════════════════════ */

/* Second dimension hash table (account_key → account_group) */
typedef struct Dim2Entry
{
    int32   key;
    int32   payload;    /* account_group */
    bool    occupied;
} Dim2Entry;

#define DIM2_CAP 512

typedef struct Dim2HashTable
{
    Dim2Entry entries[DIM2_CAP];
    int       nentries;
} Dim2HashTable;

static void
dim2_build(Dim2HashTable *dim, Oid dim_relid)
{
    memset(dim, 0, sizeof(*dim));
    Relation rel = table_open(dim_relid, AccessShareLock);
    xpb_dim_check_shape(rel, 2, "dim2_build");
    Snapshot snap = GetActiveSnapshot();
    TableScanDesc scan = table_beginscan(rel, snap, 0, NULL, 0);
    HeapTuple tup;

    while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        bool isnull;
        int32 key = DatumGetInt32(heap_getattr(tup, 1, RelationGetDescr(rel), &isnull));
        if (isnull) continue;
        int32 grp = DatumGetInt32(heap_getattr(tup, 2, RelationGetDescr(rel), &isnull));
        if (isnull) continue;

        if (dim->nentries >= DIM2_CAP * 3 / 4)
            ereport(ERROR, (errmsg("dim2_build: hash overflow (%d entries, cap %d)",
                                    dim->nentries, DIM2_CAP)));

        uint32 h = (uint32)key * 2654435761u;
        for (int i = 0; i < DIM2_CAP; i++)
        {
            int idx = (h + i) & (DIM2_CAP - 1);
            if (!dim->entries[idx].occupied)
            {
                dim->entries[idx].key = key;
                dim->entries[idx].payload = grp;
                dim->entries[idx].occupied = true;
                dim->nentries++;
                break;
            }
        }
    }
    table_endscan(scan);
    table_close(rel, AccessShareLock);
}

static inline int32
dim2_lookup(Dim2HashTable *dim, int32 key, bool *found)
{
    uint32 h = (uint32)key * 2654435761u;
    for (int i = 0; i < DIM2_CAP; i++)
    {
        int idx = (h + i) & (DIM2_CAP - 1);
        Dim2Entry *e = &dim->entries[idx];
        if (!e->occupied) { *found = false; return 0; }
        if (e->key == key) { *found = true; return e->payload; }
    }
    *found = false;
    return 0;
}

/* Join2 probe: replaces join key column with dimension payload */
static void
batch_join2_probe(Dim2HashTable *dim, int key_col, int payload_col,
                  XpColumnBatch *in, XpColumnBatch *out,
                  int32 *payload_buf)
{
    int nrows = in->nrows;
    int32 *col_key = xpcb_i32(in, key_col);     /* dispatch once, not per row */
    int nout = 0;

    /* Build selection: which input rows matched the dimension */
    uint32 *sel = palloc(nrows * sizeof(uint32));
    for (int i = 0; i < nrows; i++)
    {
        bool found;
        int32 grp = dim2_lookup(dim, col_key[i], &found);
        if (found)
        {
            sel[nout] = i;
            payload_buf[nout] = grp;
            nout++;
        }
    }

    out->nrows = nout;
    out->ncols = in->ncols;

    if (nout == nrows)
    {
        /* All rows matched -- borrow input columns, replace key with payload */
        for (int c = 0; c < in->ncols; c++)
            xpcb_col_borrow(&out->cols[c], in->cols[c].type,
                            in->cols[c].data, in->cols[c].validity);
        /* payload_buf belongs to the caller: borrowed, never freed here */
        xpcb_col_borrow(&out->cols[payload_col], XPB_COL_INT4, payload_buf, NULL);
    }
    else
    {
        /* Partial match -- gather ALL columns through the selection vector */
        for (int c = 0; c < in->ncols; c++)
        {
            int32 *buf = palloc(nout * sizeof(int32));

            if (c == payload_col)
            {
                /* Payload column already dense in payload_buf */
                memcpy(buf, payload_buf, nout * sizeof(int32));
            }
            else
            {
                int32 *src = xpcb_i32(in, c);

                for (int i = 0; i < nout; i++)
                    buf[i] = src[sel[i]];
            }

            out->cols[c].type = XPB_COL_INT4;
            out->cols[c].data = buf;
            out->cols[c].validity = NULL;
            out->cols[c].owns_data = true;
            out->cols[c].owns_validity = false;
        }
    }
    pfree(sel);
}

/* Aggregate: GROUP BY year, account_group, company_key → SUM(amount_dt) */
typedef struct J2GroupEntry
{
    int32   year;
    int32   account_group;
    int32   company_key;
    int64   sum;
    bool    occupied;
} J2GroupEntry;

PG_FUNCTION_INFO_V1(xpb_batch_join2_groupby);

Datum
xpb_batch_join2_groupby(PG_FUNCTION_ARGS)
{
    int32 lo = PG_GETARG_INT32(0);
    int32 hi = PG_GETARG_INT32(1);
    char *mode = text_to_cstring(PG_GETARG_TEXT_PP(2));

    InitMaterializedSRF(fcinfo, 0);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    Oid fact_relid = RelnameGetRelid("reg_buh");
    Oid dim_period_relid = RelnameGetRelid("dim_period");
    Oid dim_account_relid = RelnameGetRelid("dim_account");
    if (!OidIsValid(fact_relid) || !OidIsValid(dim_period_relid) || !OidIsValid(dim_account_relid))
        ereport(ERROR, (errmsg("reg_buh, dim_period, or dim_account not found")));

    /* Build both dimension hash tables */
    instr_time tb0, tb1;
    INSTR_TIME_SET_CURRENT(tb0);
    DimHashTable dim1;
    dim_build(&dim1, dim_period_relid);
    Dim2HashTable dim2;
    dim2_build(&dim2, dim_account_relid);
    INSTR_TIME_SET_CURRENT(tb1);
    double build_ms = INSTR_TIME_GET_MILLISEC(tb1) - INSTR_TIME_GET_MILLISEC(tb0);

    /* Source: 4 columns [period_key, company_key, account_key, amount_dt] */
    XpBatchSource *source;
    if (strcmp(mode, "zlfs") == 0)
    {
        zlfs_ensure_registry();
        zlfs_scan_directory();
        ZlfsZone *zone = zlfs_lookup_valid_zone(fact_relid, lo, hi);
        if (!zone)
            ereport(ERROR, (errmsg("no valid ZLFS zone for [%d..%d]", lo, hi)));
        int16 req_attnos[4] = { 1, 2, 3, 6 };
        source = xpb_zlfs_source_create(zone, req_attnos, 4);
    }
    else if (strcmp(mode, "heap") == 0)
    {
        int16 heap_attnos[4] = { 1, 2, 3, 6 };
        source = xpb_heap_source_create(fact_relid, heap_attnos, 4, true, lo, hi);
    }
    else if (strcmp(mode, "pgcolumnar") == 0)
    {
        /*
         * The columnar arm reads reg_buh_col: the same rows as reg_buh, stored
         * USING pgcolumnar. Two tables rather than one because the comparison
         * is between storage layers, and a table has exactly one.
         */
        int16 pgcn_attnos[4] = { 1, 2, 3, 6 };
        Oid   col_relid = RelnameGetRelid("reg_buh_col");

        if (!OidIsValid(col_relid))
            ereport(ERROR, (errmsg("reg_buh_col not found"),
                            errhint("Create it with benchmarks/common/schema-columnar.sql.")));
        source = xpcn_source_create(col_relid, pgcn_attnos, 4, true, lo, hi);
    }
    else
        ereport(ERROR, (errmsg("unknown mode: %s", mode)));

    /* Pipeline: source → join1(period→year) → join2(account→group) → agg */
    BatchJoinState *js1 = batch_join_create(&dim1, XPCB_BATCH_CAP);
    int32 *acct_group_buf = palloc(XPCB_BATCH_CAP * sizeof(int32));

    J2GroupEntry *ht = palloc0(GRP_CAP * sizeof(J2GroupEntry));
    int ngroups = 0;
    int64 total_rows = 0;
    int nbatches = 0;

    XpColumnBatch in_batch, j1_out, j2_out;
    memset(&in_batch, 0, sizeof(in_batch));
    in_batch.ncols = 4;
    in_batch.capacity = XPCB_BATCH_CAP;
    memset(&j1_out, 0, sizeof(j1_out));
    memset(&j2_out, 0, sizeof(j2_out));

    instr_time t0, t1, tp;
    double src_ms = 0, j1_ms = 0, j2_ms = 0, agg_ms = 0;

    INSTR_TIME_SET_CURRENT(t0);

    while (true)
    {
        in_batch.nrows = 0;

        INSTR_TIME_SET_CURRENT(tp);
        bool got = source->ops->next_batch(source, &in_batch);
        { instr_time tn; INSTR_TIME_SET_CURRENT(tn);
          src_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp); }

        if (!got) break;
        nbatches++;

        /*
         * Input batch columns: [0]=period_key, [1]=company_key, [2]=account_key, [3]=amount_dt
         *
         * Join 1: period_key(col0) → year
         * Output: [0]=year, [1]=company_key, [2]=account_key, [3]=amount_dt
         */
        INSTR_TIME_SET_CURRENT(tp);
        batch_join_probe(js1, &in_batch, &j1_out);
        { instr_time tn; INSTR_TIME_SET_CURRENT(tn);
          j1_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp); }

        /*
         * Join 2: account_key(col2) → account_group
         * Replace col2 with account_group
         * Output: [0]=year, [1]=company_key, [2]=account_group, [3]=amount_dt
         */
        INSTR_TIME_SET_CURRENT(tp);
        batch_join2_probe(&dim2, 2, 2, &j1_out, &j2_out, acct_group_buf);
        { instr_time tn; INSTR_TIME_SET_CURRENT(tn);
          j2_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp); }

        /* Aggregate: GROUP BY year(0), account_group(2), company_key(1) → SUM(amount_dt(3)) */
        INSTR_TIME_SET_CURRENT(tp);
        int nrows = j2_out.nrows;
        int32 *col_yr = xpcb_i32(&j2_out, 0);
        int32 *col_ck = xpcb_i32(&j2_out, 1);
        int32 *col_ag = xpcb_i32(&j2_out, 2);
        int32 *col_dt = xpcb_i32(&j2_out, 3);

        for (int i = 0; i < nrows; i++)
        {
            int32 yr = col_yr[i], ag = col_ag[i], ck = col_ck[i];
            int64 dt = (int64)col_dt[i];
            uint32 h = (uint32)yr * 2654435761u ^ (uint32)ag * 2246822519u ^ (uint32)ck * 0x45d9f3bu;
            int sl = (int)(h & (GRP_CAP - 1));
            for (int pr = 0; pr < GRP_CAP; pr++)
            {
                int idx = (sl + pr) & (GRP_CAP - 1);
                J2GroupEntry *g = &ht[idx];
                if (!g->occupied)
                {
                    if (ngroups >= GRP_MAX_LOAD)
                        ereport(ERROR, (errmsg("join2_agg: hash overflow")));
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

        /*
         * Free whatever the two joins gathered, and nothing else.  The old
         * batch-level flag made this loop free column 0 of j1_out as well,
         * which on the all-matched path is the join state's own year_buf --
         * borrowed memory the next batch still reads.
         */
        xpcb_release_owned(&j2_out);
        xpcb_release_owned(&j1_out);
    }

    INSTR_TIME_SET_CURRENT(t1);
    double total_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

    /* Emit results */
    Datum vals[5];
    bool nulls[5] = {false, false, false, false, false};
    for (int i = 0; i < GRP_CAP; i++)
    {
        J2GroupEntry *g = &ht[i];
        if (!g->occupied) continue;
        vals[0] = Int32GetDatum(g->year);
        vals[1] = Int32GetDatum(g->account_group);
        vals[2] = Int32GetDatum(g->company_key);
        vals[3] = Int64GetDatum(g->sum);
        vals[4] = Float8GetDatum(total_ms);
        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
    }

    {
        /*
         * For the columnar source, say how much of the scan was borrowed and
         * how much had to be materialized: it is the first split of the source
         * time and it costs nothing to report.
         */
        char    srcinfo[128] = "";

        if (strcmp(mode, "pgcolumnar") == 0)
        {
            int64 g, gc, rws, bc;

            xpcn_source_stats(source, &g, &gc, &rws, &bc);
            snprintf(srcinfo, sizeof(srcinfo),
                     "  rowgroups=%ld copied=%ld copied_bytes=%ld",
                     (long) g, (long) gc, (long) bc);
        }

        elog(NOTICE, "batch_join2_groupby [%d..%d] mode=%s: "
             "total=%.1f ms  build=%.1f ms  source=%.1f ms  "
             "join1=%.1f ms  join2=%.1f ms  agg=%.1f ms  "
             "rows=%ld  batches=%d  groups=%d  dim1=%d dim2=%d%s",
             lo, hi, mode,
             total_ms, build_ms, src_ms, j1_ms, j2_ms, agg_ms,
             total_rows, nbatches, ngroups, dim1.nentries, dim2.nentries,
             srcinfo);
    }

    source->ops->end(source);
    pfree(ht);
    pfree(js1->year_buf); pfree(js1->sel_buf); pfree(js1);
    pfree(acct_group_buf);

    return (Datum) 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 1C-like analytical register report (benchmarks/04-1c-like-register)
 *
 * Same pipeline shape as above, but carrying TWO resources and producing
 * THREE aggregates in one pass:
 *
 *   source(period, company, account, amount_dt, amount_kt)
 *     -> BatchHashJoin(dim_period)   period  -> year
 *     -> BatchHashJoin(dim_account)  account -> account_group
 *     -> Agg GROUP BY year, account_group, company
 *            SUM(amount_dt), SUM(amount_kt), SUM(amount_dt - amount_kt)
 *
 * net is derived at emit time from the two sums that were accumulated, not by
 * running the query twice and subtracting outside: a second pass would read
 * and join the rows again, which is exactly the cost being measured.
 *
 * Both sums are int64. For this benchmark the money columns are integers and
 * int64 accumulation is exact for them; that is a property of the dataset, not
 * support for PostgreSQL numeric.
 *
 * The existing 4-column entry point is left alone -- benchmark 02 and its
 * published checksum depend on it.
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct RegGroupEntry
{
    int32   year;
    int32   account_group;
    int32   company_key;
    int64   debit;
    int64   credit;
    bool    occupied;
} RegGroupEntry;

PG_FUNCTION_INFO_V1(xpb_1c_register_report);

Datum
xpb_1c_register_report(PG_FUNCTION_ARGS)
{
    int32 lo = PG_GETARG_INT32(0);
    int32 hi = PG_GETARG_INT32(1);
    char *mode = text_to_cstring(PG_GETARG_TEXT_PP(2));

    /* attnums this function reads from the fact table, in batch column order */
    int16 fact_attnos[5] = { 1, 2, 3, 6, 7 };

    InitMaterializedSRF(fcinfo, 0);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    Oid fact_relid = RelnameGetRelid("reg_buh");
    Oid dim_period_relid = RelnameGetRelid("dim_period");
    Oid dim_account_relid = RelnameGetRelid("dim_account");
    if (!OidIsValid(fact_relid) || !OidIsValid(dim_period_relid) ||
        !OidIsValid(dim_account_relid))
        ereport(ERROR, (errmsg("reg_buh, dim_period, or dim_account not found")));

    /*
     * Check the fact table's shape before anything addresses it by attnum.
     * The sources take attnos on trust; reading one past natts reaches
     * getmissingattr() and takes the backend down, which is how the dim_build
     * crash presented. Same check, applied to the side that was still unguarded.
     */
    {
        Relation frel = relation_open(fact_relid, AccessShareLock);

        xpb_check_attnos(frel, fact_attnos, 5, "1c_register_report(reg_buh)");
        relation_close(frel, AccessShareLock);
    }

    instr_time tb0, tb1;
    INSTR_TIME_SET_CURRENT(tb0);
    DimHashTable dim1;
    dim_build(&dim1, dim_period_relid);
    Dim2HashTable dim2;
    dim2_build(&dim2, dim_account_relid);
    INSTR_TIME_SET_CURRENT(tb1);
    double build_ms = INSTR_TIME_GET_MILLISEC(tb1) - INSTR_TIME_GET_MILLISEC(tb0);

    XpBatchSource *source;
    if (strcmp(mode, "zlfs") == 0)
    {
        zlfs_ensure_registry();
        zlfs_scan_directory();
        ZlfsZone *zone = zlfs_lookup_valid_zone(fact_relid, lo, hi);
        if (!zone)
            ereport(ERROR, (errmsg("no valid ZLFS zone for [%d..%d]", lo, hi)));
        source = xpb_zlfs_source_create(zone, fact_attnos, 5);
    }
    else if (strcmp(mode, "heap") == 0)
        source = xpb_heap_source_create(fact_relid, fact_attnos, 5, true, lo, hi);
    else if (strcmp(mode, "pgcolumnar") == 0)
    {
        Oid col_relid = RelnameGetRelid("reg_buh_col");

        if (!OidIsValid(col_relid))
            ereport(ERROR, (errmsg("reg_buh_col not found"),
                            errhint("Load the dataset with its schema-columnar.sql.")));
        {
            Relation crel = relation_open(col_relid, AccessShareLock);

            xpb_check_attnos(crel, fact_attnos, 5,
                             "1c_register_report(reg_buh_col)");
            relation_close(crel, AccessShareLock);
        }
        source = xpcn_source_create(col_relid, fact_attnos, 5, true, lo, hi);
    }
    else
        ereport(ERROR, (errmsg("unknown mode: %s", mode)));

    BatchJoinState *js1 = batch_join_create(&dim1, XPCB_BATCH_CAP);
    int32 *acct_group_buf = palloc(XPCB_BATCH_CAP * sizeof(int32));
    RegGroupEntry *ht = palloc0(GRP_CAP * sizeof(RegGroupEntry));
    int ngroups = 0;
    int64 total_rows = 0;
    int nbatches = 0;

    XpColumnBatch in_batch, j1_out, j2_out;
    memset(&in_batch, 0, sizeof(in_batch));
    in_batch.ncols = 5;
    in_batch.capacity = XPCB_BATCH_CAP;
    memset(&j1_out, 0, sizeof(j1_out));
    memset(&j2_out, 0, sizeof(j2_out));

    instr_time t0, t1, tp;
    double src_ms = 0, j1_ms = 0, j2_ms = 0, agg_ms = 0;

    INSTR_TIME_SET_CURRENT(t0);

    while (true)
    {
        in_batch.nrows = 0;

        INSTR_TIME_SET_CURRENT(tp);
        bool got = source->ops->next_batch(source, &in_batch);
        { instr_time tn; INSTR_TIME_SET_CURRENT(tn);
          src_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp); }
        if (!got) break;
        nbatches++;

        INSTR_TIME_SET_CURRENT(tp);
        batch_join_probe(js1, &in_batch, &j1_out);
        { instr_time tn; INSTR_TIME_SET_CURRENT(tn);
          j1_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp); }

        INSTR_TIME_SET_CURRENT(tp);
        batch_join2_probe(&dim2, 2, 2, &j1_out, &j2_out, acct_group_buf);
        { instr_time tn; INSTR_TIME_SET_CURRENT(tn);
          j2_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp); }

        INSTR_TIME_SET_CURRENT(tp);
        {
            int nrows = j2_out.nrows;
            int32 *col_yr = xpcb_i32(&j2_out, 0);
            int32 *col_ck = xpcb_i32(&j2_out, 1);
            int32 *col_ag = xpcb_i32(&j2_out, 2);
            int32 *col_dt = xpcb_i32(&j2_out, 3);
            int32 *col_kt = xpcb_i32(&j2_out, 4);

            for (int i = 0; i < nrows; i++)
            {
                int32 yr = col_yr[i], ag = col_ag[i], ck = col_ck[i];
                int64 dt = (int64) col_dt[i];
                int64 kt = (int64) col_kt[i];
                uint32 h = (uint32)yr * 2654435761u ^ (uint32)ag * 2246822519u
                         ^ (uint32)ck * 0x45d9f3bu;
                int sl = (int)(h & (GRP_CAP - 1));

                for (int pr = 0; pr < GRP_CAP; pr++)
                {
                    int idx = (sl + pr) & (GRP_CAP - 1);
                    RegGroupEntry *g = &ht[idx];

                    if (!g->occupied)
                    {
                        if (ngroups >= GRP_MAX_LOAD)
                            ereport(ERROR, (errmsg("1c_register_report: hash overflow "
                                                   "(%d groups, cap %d)",
                                                   ngroups, GRP_CAP)));
                        g->year = yr; g->account_group = ag; g->company_key = ck;
                        g->debit = dt; g->credit = kt;
                        g->occupied = true; ngroups++; break;
                    }
                    if (g->year == yr && g->account_group == ag &&
                        g->company_key == ck)
                    { g->debit += dt; g->credit += kt; break; }
                }
            }
            total_rows += nrows;
        }
        { instr_time tn; INSTR_TIME_SET_CURRENT(tn);
          agg_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp); }

        xpcb_release_owned(&j2_out);
        xpcb_release_owned(&j1_out);
    }

    INSTR_TIME_SET_CURRENT(t1);
    double total_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

    {
        Datum vals[7];
        bool nulls[7] = {false, false, false, false, false, false, false};

        for (int i = 0; i < GRP_CAP; i++)
        {
            RegGroupEntry *g = &ht[i];

            if (!g->occupied) continue;
            vals[0] = Int32GetDatum(g->year);
            vals[1] = Int32GetDatum(g->account_group);
            vals[2] = Int32GetDatum(g->company_key);
            vals[3] = Int64GetDatum(g->debit);
            vals[4] = Int64GetDatum(g->credit);
            vals[5] = Int64GetDatum(g->debit - g->credit);
            vals[6] = Float8GetDatum(total_ms);
            tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
        }
    }

    {
        char srcinfo[128] = "";

        if (strcmp(mode, "pgcolumnar") == 0)
        {
            int64 g, gc, rws, bc;

            xpcn_source_stats(source, &g, &gc, &rws, &bc);
            snprintf(srcinfo, sizeof(srcinfo),
                     "  rowgroups=%ld copied=%ld copied_bytes=%ld",
                     (long) g, (long) gc, (long) bc);
        }
        elog(NOTICE, "1c_register_report [%d..%d] mode=%s: "
             "total=%.1f ms  build=%.1f ms  source=%.1f ms  "
             "join1=%.1f ms  join2=%.1f ms  agg=%.1f ms  "
             "rows=%ld  batches=%d  groups=%d  dim1=%d dim2=%d%s",
             lo, hi, mode,
             total_ms, build_ms, src_ms, j1_ms, j2_ms, agg_ms,
             total_rows, nbatches, ngroups, dim1.nentries, dim2.nentries,
             srcinfo);
    }

    source->ops->end(source);
    pfree(ht);
    pfree(js1->year_buf); pfree(js1->sel_buf); pfree(js1);
    pfree(acct_group_buf);

    return (Datum) 0;
}
