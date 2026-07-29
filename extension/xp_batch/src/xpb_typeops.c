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
 * Walks the fixed-width attribute chain up to attno, then reads
 * the value as int32 or int64 depending on attlen.
 * NULL -> returns 0 (checked at plan time: no nulls in fast-path columns).
 * ----------------------------------------------------------------------- */
static XpbVal
xpb_getattr_scalar(HeapTupleHeader htup, AttrNumber attno, TupleDesc tupdesc)
{
    uint8  *bp      = htup->t_bits;
    bool    hasnull = (htup->t_infomask & HEAP_HASNULL) != 0;

    if (hasnull && att_isnull(attno - 1, bp))
        return 0;

    char *tp = (char *) htup + htup->t_hoff;
    for (int i = 0; i < attno - 1; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        if (hasnull && att_isnull(i, bp)) continue;
        if (attr->attlen > 0)
            tp += att_align_nominal(attr->attlen, attr->attalign);
        else
            tp += att_addlength_pointer(0, attr->attlen, tp);
    }

    if (TupleDescAttr(tupdesc, attno - 1)->attlen == 8)
        return (XpbVal) *((int64 *) tp);
    return (XpbVal) *((int32 *) tp);
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
