/*
 * xpb_typed_pipeline.c — join + GROUP BY + SUM over the typed batch contract
 *
 * The pipeline the contract milestone has to be able to run: read a wide row
 * through a batch source, inner-join a dimension, group by keys that may be
 * int4 or int8 and may be NULL, and sum columns that may be int4, int8 or
 * numeric and may be NULL -- with PostgreSQL's semantics, not an
 * approximation of them.
 *
 * It is deliberately separate from xpb_1c_register_report().  That function
 * is what benchmarks 02 and 04 measure; it stays on the int4 fixed-offset
 * fast path and its checksums do not move.  This one exists to exercise and
 * test the contract.
 *
 * NULL SEMANTICS, WHICH ARE NOT THE SAME IN THE TWO OPERATORS
 * ----------------------------------------------------------
 * Join:      NULL never equals anything, including another NULL.  A NULL on
 *            the fact side matches no dimension row; a NULL dimension key is
 *            not loaded into the hash table at all, so it can never be hit.
 *            Both sides drop the row from an inner join.
 *
 * GROUP BY:  NULLs are all the SAME group.  Grouping equality is "not
 *            distinct from", not "=".
 *
 * Reusing join equality for grouping, or the reverse, is the obvious bug
 * here, so the two comparisons are written separately and named for what
 * they are.  test/contract_tests.sh checks both directions against
 * PostgreSQL.
 *
 * SUM:       NULL inputs are skipped, and a group in which every input was
 *            NULL sums to NULL -- not to zero.  Each accumulator therefore
 *            carries a "saw a value" flag; nothing is initialised to a
 *            neutral element that could be mistaken for a real zero.
 *
 * ACCUMULATOR REPRESENTATION
 * --------------------------
 *   sum(int4)     int64.   Exact: 2^31 * 2^32 rows would be needed to
 *                          overflow, far past what a batch pipeline holds.
 *                          PostgreSQL returns bigint here and so do we.
 *   sum(int8)     INT128 (common/int128.h, the same accumulator PostgreSQL's
 *                          own sum(int8) uses).  PostgreSQL returns numeric;
 *                          the int128 is rendered as a decimal string, which
 *                          is exactly numeric's text form for an integer.
 *   sum(numeric)  PostgreSQL numeric, accumulated with numeric_add.  No
 *                          scaled-integer shortcut: see the numeric strategy
 *                          note in docs/TYPED_BATCH_CONTRACT.md.  Correctness
 *                          first; this is the slow, obviously-right one.
 */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "common/int128.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "xpb_colbatch.h"
#include "xpb_src_heap.h"

PG_FUNCTION_INFO_V1(xpb_typed_report);

#define TP_MAX_KEYS 4
#define TP_MAX_SUMS 4
#define TP_GRP_CAP  16384
#define TP_GRP_LOAD (TP_GRP_CAP * 3 / 4)
#define TP_DIM_CAP  4096

/* ── group key ── */

typedef struct TpKey
{
    int64       val;            /* int4 and int8 both widen to int64 here */
    bool        isnull;
} TpKey;

typedef struct TpSum
{
    bool        seen;           /* false => SQL sum is NULL, not 0 */
    int64       i64;            /* XPB_COL_INT4 */
    INT128      i128;           /* XPB_COL_INT8 */
    Datum       num;            /* XPB_COL_NUMERIC, valid while seen */
} TpSum;

typedef struct TpGroup
{
    bool        occupied;
    int         nkeys;
    TpKey       keys[TP_MAX_KEYS];
    int64       nrows;
    TpSum       sums[TP_MAX_SUMS];
} TpGroup;

/*
 * Grouping equality: "not distinct from".  Two NULLs are the same group.
 * This is NOT the join's comparison -- see the header comment.
 */
static inline bool
tp_keys_same_group(const TpKey *a, const TpKey *b, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (a[i].isnull != b[i].isnull)
            return false;
        if (!a[i].isnull && a[i].val != b[i].val)
            return false;
    }
    return true;
}

static inline uint32
tp_hash_keys(const TpKey *k, int n)
{
    uint32 h = 0x9e3779b9u;

    for (int i = 0; i < n; i++)
    {
        uint64 v;

        /*
         * NULL hashes to a fixed constant rather than to some value in the
         * data domain, so that NULLs land together without colliding by
         * pretending to be a particular integer.
         */
        v = k[i].isnull ? 0xD1B54A32D192ED03ULL : (uint64) k[i].val;
        h ^= (uint32) (v ^ (v >> 32));
        h *= 2654435761u;
    }
    return h;
}

/* ── dimension hash (join build side) ── */

typedef struct TpDimEntry
{
    bool        occupied;
    int64       key;
    int64       payload;
    bool        payload_null;
} TpDimEntry;

typedef struct TpDim
{
    TpDimEntry  e[TP_DIM_CAP];
    int         n;
} TpDim;

/*
 * Build the dimension hash.  A dimension row whose KEY is NULL is skipped
 * entirely: under SQL semantics it can never equal a fact key, so putting it
 * in the table could only ever produce a wrong match.  A NULL PAYLOAD is
 * kept -- that is an ordinary value that flows into the group key.
 */
static void
tp_dim_build(TpDim *d, Oid relid, int16 key_attno, int16 payload_attno)
{
    int16           attnos[2];
    XpBatchSource  *src;
    XpColumnBatch   batch;

    memset(d, 0, sizeof(*d));
    attnos[0] = key_attno;
    attnos[1] = payload_attno;

    src = xpb_heap_source_create_ex(relid, attnos, 2, XPB_HEAP_DEFORM,
                                    false, 0, 0);

    memset(&batch, 0, sizeof(batch));
    batch.capacity = 1024;
    batch.ncols = 2;

    while (src->ops->next_batch(src, &batch))
    {
        bool key_is_i8 = (batch.cols[0].type == XPB_COL_INT8);
        bool pay_is_i8 = (batch.cols[1].type == XPB_COL_INT8);

        if (!xpcb_is_integer(&batch, 0) || !xpcb_is_integer(&batch, 1))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("typed_report: dimension columns must be int4 or int8")));

        for (int r = 0; r < batch.nrows; r++)
        {
            int64   key;
            uint32  h;

            if (xpcb_isnull(&batch, 0, r))
                continue;               /* NULL key matches nothing, ever */

            key = key_is_i8 ? ((int64 *) batch.cols[0].data)[r]
                            : ((int32 *) batch.cols[0].data)[r];

            if (d->n >= TP_DIM_CAP * 3 / 4)
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("typed_report: dimension hash overflow (%d rows, cap %d)",
                                d->n, TP_DIM_CAP)));

            h = (uint32) key * 2654435761u;
            for (int i = 0; i < TP_DIM_CAP; i++)
            {
                int idx = (h + i) & (TP_DIM_CAP - 1);

                if (!d->e[idx].occupied)
                {
                    d->e[idx].occupied = true;
                    d->e[idx].key = key;
                    d->e[idx].payload_null = xpcb_isnull(&batch, 1, r);
                    if (!d->e[idx].payload_null)
                        d->e[idx].payload =
                            pay_is_i8 ? ((int64 *) batch.cols[1].data)[r]
                                      : ((int32 *) batch.cols[1].data)[r];
                    d->n++;
                    break;
                }
                if (d->e[idx].key == key)
                    break;              /* first row wins, as before */
            }
        }
        xpcb_release_owned(&batch);
    }
    src->ops->end(src);
}

/*
 * Join probe.  Returns false when the row does not join -- including when the
 * fact key is NULL, which is the SQL rule and not an oversight.
 */
static inline bool
tp_dim_lookup(const TpDim *d, int64 key, int64 *payload, bool *payload_null)
{
    uint32 h = (uint32) key * 2654435761u;

    for (int i = 0; i < TP_DIM_CAP; i++)
    {
        int idx = (h + i) & (TP_DIM_CAP - 1);
        const TpDimEntry *e = &d->e[idx];

        if (!e->occupied)
            return false;
        if (e->key == key)
        {
            *payload = e->payload;
            *payload_null = e->payload_null;
            return true;
        }
    }
    return false;
}

/* ── aggregation ── */

static void
tp_sum_add(TpSum *s, XpbColType type, const XpBatchColumn *col, int r,
           MemoryContext numcxt)
{
    switch (type)
    {
        case XPB_COL_INT4:
            if (!s->seen)
                s->i64 = ((int32 *) col->data)[r];
            else
                s->i64 += ((int32 *) col->data)[r];
            break;

        case XPB_COL_INT8:
            if (!s->seen)
                s->i128 = make_int128(0, 0);
            int128_add_int64(&s->i128, ((int64 *) col->data)[r]);
            break;

        case XPB_COL_NUMERIC:
        {
            MemoryContext old = MemoryContextSwitchTo(numcxt);
            Datum         v = ((Datum *) col->data)[r];

            if (!s->seen)
            {
                s->num = PointerGetDatum(DatumGetNumericCopy(v));
            }
            else
            {
                Datum prev = s->num;

                s->num = DirectFunctionCall2(numeric_add, prev, v);
                pfree(DatumGetPointer(prev));
            }
            MemoryContextSwitchTo(old);
            break;
        }

        default:
            elog(ERROR, "typed_report: cannot sum a %s column",
                 xpcb_type_name(type));
    }
    s->seen = true;
}

/* Render an INT128 as a decimal string -- numeric's text form for an integer. */
static char *
tp_int128_str(INT128 v)
{
#if USE_NATIVE_INT128
    char                buf[64];
    char               *p = buf + sizeof(buf);
    bool                neg = (v < 0);
    /* negate in unsigned space so INT128_MIN does not overflow */
    unsigned __int128   mag = neg ? (unsigned __int128) 0 - (unsigned __int128) v
                                  : (unsigned __int128) v;

    *--p = '\0';
    if (mag == 0)
        *--p = '0';
    while (mag > 0)
    {
        *--p = (char) ('0' + (int) (mag % 10));
        mag /= 10;
    }
    if (neg)
        *--p = '-';
    return pstrdup(p);
#else
    /*
     * The portable INT128 struct has no division, so rendering it means
     * reconstructing a numeric from the halves.  Not written until a build
     * without a native int128 actually needs it -- an untested fallback is
     * worse than a refusal that names itself.
     */
    elog(ERROR, "xp_batch: sum(int8) needs a native int128 on this build");
    return NULL;
#endif
}

/* ── entry point ── */

/*
 * xpb_typed_report(fact text, group_attnos int[], sum_attnos int[],
 *                  dim text, join_attno int, path text)
 *   -> (gkey text, nrows bigint, sums text[])
 *
 * When dim is given, the fact column join_attno is inner-joined to dimension
 * column 1 and dimension column 2 becomes an extra group key, prepended.
 * gkey renders the key tuple with NULL as \N, so an empty string and a NULL
 * stay distinguishable in the comparison.
 */
Datum
xpb_typed_report(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *store;
    MemoryContext   oldcxt,
                    numcxt;
    ArrayType      *garr,
                   *sarr;
    Oid             fact_relid,
                    dim_relid = InvalidOid;
    int16           attnos[XPCB_MAX_COLS];
    int16           group_attnos[TP_MAX_KEYS];
    int16           sum_attnos[TP_MAX_SUMS];
    int             ngroup,
                    nsum,
                    ncols;
    int16           join_attno = 0;
    bool            has_join;
    XpbHeapPath     path;
    XpBatchSource  *src;
    XpColumnBatch   batch;
    TpDim           dim;
    TpGroup        *ht;
    XpbColType      sum_types[TP_MAX_SUMS];
    int             ngroups = 0;
    char           *pstr;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        (rsi->allowedModes & SFRM_Materialize) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("xpb_typed_report: set-valued context required")));

    fact_relid = RelnameGetRelid(text_to_cstring(PG_GETARG_TEXT_PP(0)));
    if (!OidIsValid(fact_relid))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("xpb_typed_report: fact relation not found")));

    garr = PG_GETARG_ARRAYTYPE_P(1);
    sarr = PG_GETARG_ARRAYTYPE_P(2);
    ngroup = ArrayGetNItems(ARR_NDIM(garr), ARR_DIMS(garr));
    nsum = ArrayGetNItems(ARR_NDIM(sarr), ARR_DIMS(sarr));

    has_join = !PG_ARGISNULL(3);
    if (has_join)
    {
        dim_relid = RelnameGetRelid(text_to_cstring(PG_GETARG_TEXT_PP(3)));
        if (!OidIsValid(dim_relid))
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg("xpb_typed_report: dimension relation not found")));
        join_attno = (int16) PG_GETARG_INT32(4);
    }

    /* the join adds one more group key, so leave room for it */
    if (ngroup < 1 || ngroup + (has_join ? 1 : 0) > TP_MAX_KEYS)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_typed_report: %d group keys (1..%d, and the join adds one)",
                        ngroup, TP_MAX_KEYS)));
    if (nsum < 0 || nsum > TP_MAX_SUMS)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_typed_report: %d sum columns (0..%d)", nsum, TP_MAX_SUMS)));

    for (int i = 0; i < ngroup; i++)
        group_attnos[i] = (int16) ((int32 *) ARR_DATA_PTR(garr))[i];
    for (int i = 0; i < nsum; i++)
        sum_attnos[i] = (int16) ((int32 *) ARR_DATA_PTR(sarr))[i];

    pstr = text_to_cstring(PG_GETARG_TEXT_PP(5));
    path = (strcmp(pstr, "fixed") == 0) ? XPB_HEAP_FIXED : XPB_HEAP_DEFORM;

    /*
     * Batch layout: group keys, then sum columns, then the join key last so
     * the indices of everything else do not shift with has_join.
     */
    ncols = 0;
    for (int i = 0; i < ngroup; i++)
        attnos[ncols++] = group_attnos[i];
    for (int i = 0; i < nsum; i++)
        attnos[ncols++] = sum_attnos[i];
    if (has_join)
        attnos[ncols++] = join_attno;

    if (ncols > XPCB_MAX_COLS)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_typed_report: %d columns needed, batch carries %d",
                        ncols, XPCB_MAX_COLS)));

    oldcxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    tupdesc = CreateTemplateTupleDesc(3);
    TupleDescInitEntry(tupdesc, 1, "gkey", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, 2, "nrows", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 3, "sums", TEXTARRAYOID, -1, 0);
    store = tuplestore_begin_heap(true, false, work_mem);
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = store;
    rsi->setDesc = BlessTupleDesc(tupdesc);
    MemoryContextSwitchTo(oldcxt);

    numcxt = AllocSetContextCreate(CurrentMemoryContext,
                                   "xpb typed numeric accumulators",
                                   ALLOCSET_DEFAULT_SIZES);

    if (has_join)
        tp_dim_build(&dim, dim_relid, 1, 2);

    ht = palloc0(TP_GRP_CAP * sizeof(TpGroup));

    src = xpb_heap_source_create_ex(fact_relid, attnos, ncols, path,
                                    false, 0, 0);

    memset(&batch, 0, sizeof(batch));
    batch.capacity = 1024;
    batch.ncols = ncols;
    for (int s = 0; s < nsum; s++)
        sum_types[s] = XPB_COL_UNSET;

    while (src->ops->next_batch(src, &batch))
    {
        int join_col = ncols - 1;

        /*
         * Record the sum column types while a batch is live.  The emit loop
         * runs after the last batch has been released and must not read the
         * batch's own column metadata back.
         */
        for (int s = 0; s < nsum; s++)
            sum_types[s] = batch.cols[ngroup + s].type;       /* only meaningful when has_join */

        for (int r = 0; r < batch.nrows; r++)
        {
            TpKey   keys[TP_MAX_KEYS];
            int     nk = 0;
            uint32  h;
            int     slot = -1;

            CHECK_FOR_INTERRUPTS();

            if (has_join)
            {
                int64   fk,
                        payload;
                bool    payload_null;

                /*
                 * A NULL join key joins to nothing.  Not "joins to the NULL
                 * dimension row" -- there is no such thing under SQL
                 * semantics, and the dimension never stored one.
                 */
                if (xpcb_isnull(&batch, join_col, r))
                    continue;

                fk = (batch.cols[join_col].type == XPB_COL_INT8)
                    ? ((int64 *) batch.cols[join_col].data)[r]
                    : ((int32 *) batch.cols[join_col].data)[r];

                if (!tp_dim_lookup(&dim, fk, &payload, &payload_null))
                    continue;           /* inner join drops the row */

                keys[nk].val = payload;
                keys[nk].isnull = payload_null;
                nk++;
            }

            for (int i = 0; i < ngroup; i++)
            {
                const XpBatchColumn *col = &batch.cols[i];

                keys[nk].isnull = xpcb_isnull(&batch, i, r);
                if (keys[nk].isnull)
                    keys[nk].val = 0;
                else if (col->type == XPB_COL_INT8)
                    keys[nk].val = ((int64 *) col->data)[r];
                else if (col->type == XPB_COL_INT4)
                    keys[nk].val = ((int32 *) col->data)[r];
                else
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("xpb_typed_report: group key column %d is %s, int4 or int8 required",
                                    i, xpcb_type_name(col->type))));
                nk++;
            }

            h = tp_hash_keys(keys, nk);
            for (int probe = 0; probe < TP_GRP_CAP; probe++)
            {
                int      idx = (int) ((h + probe) & (TP_GRP_CAP - 1));
                TpGroup *g = &ht[idx];

                if (!g->occupied)
                {
                    if (ngroups >= TP_GRP_LOAD)
                        ereport(ERROR,
                                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                                 errmsg("xpb_typed_report: group hash overflow (%d groups, cap %d)",
                                        ngroups, TP_GRP_CAP)));
                    g->occupied = true;
                    g->nkeys = nk;
                    memcpy(g->keys, keys, nk * sizeof(TpKey));
                    ngroups++;
                    slot = idx;
                    break;
                }
                /* grouping equality, not join equality */
                if (g->nkeys == nk && tp_keys_same_group(g->keys, keys, nk))
                {
                    slot = idx;
                    break;
                }
            }

            ht[slot].nrows++;
            for (int s = 0; s < nsum; s++)
            {
                int c = ngroup + s;

                if (xpcb_isnull(&batch, c, r))
                    continue;           /* sum skips NULLs */
                tp_sum_add(&ht[slot].sums[s], batch.cols[c].type,
                           &batch.cols[c], r, numcxt);
            }
        }
        xpcb_release_owned(&batch);
    }
    src->ops->end(src);

    /* ── emit ── */
    for (int i = 0; i < TP_GRP_CAP; i++)
    {
        TpGroup        *g = &ht[i];
        StringInfoData  key;
        Datum           vals[3];
        bool            nulls[3] = {false, false, false};
        Datum           elems[TP_MAX_SUMS];
        bool            elnulls[TP_MAX_SUMS];

        if (!g->occupied)
            continue;

        initStringInfo(&key);
        for (int k = 0; k < g->nkeys; k++)
        {
            if (k > 0)
                appendStringInfoChar(&key, '|');
            if (g->keys[k].isnull)
                appendStringInfoString(&key, "\\N");
            else
                appendStringInfo(&key, INT64_FORMAT, g->keys[k].val);
        }

        for (int s = 0; s < nsum; s++)
        {
            TpSum *sm = &g->sums[s];

            if (!sm->seen)
            {
                /* every input was NULL: SQL says the sum is NULL, not 0 */
                elems[s] = (Datum) 0;
                elnulls[s] = true;
                continue;
            }
            elnulls[s] = false;
            switch (sum_types[s])
            {
                case XPB_COL_INT4:
                    elems[s] = CStringGetTextDatum(psprintf(INT64_FORMAT, sm->i64));
                    break;
                case XPB_COL_INT8:
                    elems[s] = CStringGetTextDatum(tp_int128_str(sm->i128));
                    break;
                case XPB_COL_NUMERIC:
                    elems[s] = CStringGetTextDatum(
                        DatumGetCString(DirectFunctionCall1(numeric_out, sm->num)));
                    break;
                default:
                    elog(ERROR, "xpb_typed_report: unsummable column");
            }
        }

        vals[0] = CStringGetTextDatum(key.data);
        vals[1] = Int64GetDatum(g->nrows);
        vals[2] = PointerGetDatum(construct_md_array(elems, elnulls, 1, &nsum,
                                                     (int[]) {1}, TEXTOID,
                                                     -1, false, TYPALIGN_INT));
        tuplestore_putvalues(store, rsi->setDesc, vals, nulls);
        pfree(key.data);
    }

    MemoryContextDelete(numcxt);
    return (Datum) 0;
}
