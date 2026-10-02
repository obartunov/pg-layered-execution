/*
 * xpb_typeops.c
 *
 * Implementations of per-type operations for the xp_batch native scalar path.
 * See xpb_typeops.h for the contract.
 *
 * Adding a new type: add one XpbTypeOps entry here and extend
 * xpb_typeops_lookup().  Nothing else changes.
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/timestamp.h"

#include "xpb_typeops.h"

/* -----------------------------------------------------------------------
 * Shared heap attribute walker.
 *
 * Locates attno in a heap tuple and reads it as int32 or int64.
 *
 * R1-15.  The previous version computed the offset itself, as
 *
 *     tp += att_align_nominal(attr->attlen, attr->attalign);
 *
 * which aligns the attribute's LENGTH rather than the running offset, so the
 * padding before an attribute was never counted.  For every column of equal
 * length and alignment -- (int4, int4, int4) -- the two happen to agree, which
 * is why the benchmark schema never showed it.  One byte of padding anywhere
 * before a fast-path column and every key and aggregate read is off:
 *
 *     CREATE TABLE t (flag bool NOT NULL, k1 int NOT NULL,
 *                     k2 int NOT NULL, v int NOT NULL);
 *     SELECT k1, k2, sum(v) FROM t GROUP BY 1, 2;
 *
 *     PostgreSQL   2400 groups, sum 11 519 175
 *     XpGroupAgg2  2400 groups, sum 193 259 687 116 800
 *
 * Nothing about alignment, short or external varlena headers, or NULL bitmap
 * interpretation is computed here any more.  It is delegated to the same
 * inline helpers from access/tupmacs.h that heap_deform_tuple() uses, which is
 * what xpb_src_heap.c's projected path already does -- align_fetch_then_add()
 * aligns the running offset, fetches, and steps past, in that order.
 *
 * A NULL in a preceding attribute occupies no space and is skipped.  A NULL in
 * the target attribute returns 0, which is only correct because the planner
 * refuses this path when any column it reads is nullable (xpga2_add_path):
 * before R1-14 that gate covered the grouping and aggregate columns but not
 * the predicate columns, and a NULL there was read as a zero that passed the
 * predicate.
 * ----------------------------------------------------------------------- */
static XpbVal
xpb_getattr_scalar(HeapTupleHeader htup, AttrNumber attno, TupleDesc tupdesc)
{
    uint8              *bp      = htup->t_bits;
    bool                hasnull = (htup->t_infomask & HEAP_HASNULL) != 0;
    CompactAttribute   *target  = TupleDescCompactAttr(tupdesc, attno - 1);
    const char         *tp      = (const char *) htup + htup->t_hoff;
    uint32              off     = 0;
    Datum               d       = (Datum) 0;

    if (hasnull && att_isnull(attno - 1, bp))
        return 0;

    /*
     * The descriptor's cached offset is PostgreSQL's own answer, valid while
     * the tuple has no NULLs at all -- the same condition heap_getattr()
     * applies before using it.
     */
    if (!hasnull && target->attcacheoff >= 0)
    {
        d = fetch_att_noerr(tp + target->attcacheoff,
                            target->attbyval, target->attlen);
    }
    else
    {
        for (int i = 0; i < attno; i++)
        {
            CompactAttribute *a = TupleDescCompactAttr(tupdesc, i);

            if (hasnull && att_isnull(i, bp))
                continue;       /* a NULL occupies no space */

            d = align_fetch_then_add(tp, &off, a->attbyval, a->attlen,
                                     a->attalignby);
        }
    }

    if (target->attlen == 8)
        return (XpbVal) DatumGetInt64(d);
    return (XpbVal) DatumGetInt32(d);
}

/* ------- INT4 ------- */
static Datum int4_to_datum(XpbVal val)   { return Int32GetDatum((int32) val); }
static XpbVal int4_from_datum(Datum d)   { return (XpbVal) DatumGetInt32(d); }

const XpbTypeOps xpb_typeops_int4 = {
    .typid = INT4OID, .name = "int4", .byval = true, .attlen = 4,
    .getattr_fn = xpb_getattr_scalar,
    .to_datum_fn = int4_to_datum, .from_datum_fn = int4_from_datum,
    .sentinel_min = PG_INT32_MIN, .sentinel_max = PG_INT32_MAX,
};

/* ------- INT8 ------- */
static Datum int8_to_datum(XpbVal val)   { return Int64GetDatum(val); }
static XpbVal int8_from_datum(Datum d)   { return (XpbVal) DatumGetInt64(d); }

const XpbTypeOps xpb_typeops_int8 = {
    .typid = INT8OID, .name = "int8", .byval = true, .attlen = 8,
    .getattr_fn = xpb_getattr_scalar,
    .to_datum_fn = int8_to_datum, .from_datum_fn = int8_from_datum,
    .sentinel_min = PG_INT64_MIN, .sentinel_max = PG_INT64_MAX,
};

/* ------- DATE (int32: days from 2000-01-01) ------- */
static Datum date_to_datum(XpbVal val)   { return DateADTGetDatum((DateADT) val); }
static XpbVal date_from_datum(Datum d)   { return (XpbVal) DatumGetDateADT(d); }

const XpbTypeOps xpb_typeops_date = {
    .typid = DATEOID, .name = "date", .byval = true, .attlen = 4,
    .getattr_fn = xpb_getattr_scalar,
    .to_datum_fn = date_to_datum, .from_datum_fn = date_from_datum,
    .sentinel_min = PG_INT32_MIN, .sentinel_max = PG_INT32_MAX,
};

/* ------- TIMESTAMP (int64: usec from 2000-01-01, no tz) ------- */
static Datum timestamp_to_datum(XpbVal val) { return TimestampGetDatum((Timestamp) val); }
static XpbVal timestamp_from_datum(Datum d) { return (XpbVal) DatumGetTimestamp(d); }

const XpbTypeOps xpb_typeops_timestamp = {
    .typid = TIMESTAMPOID, .name = "timestamp", .byval = true, .attlen = 8,
    .getattr_fn = xpb_getattr_scalar,
    .to_datum_fn = timestamp_to_datum, .from_datum_fn = timestamp_from_datum,
    .sentinel_min = PG_INT64_MIN, .sentinel_max = PG_INT64_MAX,
};

/* ------- Lookup table ------- */
static const XpbTypeOps * const xpb_type_table[] = {
    &xpb_typeops_int4,
    &xpb_typeops_int8,
    &xpb_typeops_date,
    &xpb_typeops_timestamp,
    NULL
};

const XpbTypeOps *
xpb_typeops_lookup(Oid typid)
{
    for (int i = 0; xpb_type_table[i] != NULL; i++)
        if (xpb_type_table[i]->typid == typid)
            return xpb_type_table[i];
    return NULL;
}
