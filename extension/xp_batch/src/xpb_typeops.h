/*
 * xpb_typeops.h
 *
 * Minimal typed descriptor for xp_batch native scalar fast path.
 *
 * This is NOT a generic framework for all PostgreSQL types.
 * It is a narrow internal contract for the ordered-scalar subset:
 * currently INT4, INT8, DATE, and TIMESTAMP.
 *
 * SUPPORTED TYPES (native scalar fast path)
 * -----------------------------------------
 *   INT4OID        — 4-byte, byval, attlen=4
 *   INT8OID        — 8-byte, byval, attlen=8
 *   DATEOID        — 4-byte, byval, attlen=4  (days from 2000-01-01)
 *   TIMESTAMPOID   — 8-byte, byval, attlen=8  (usec from 2000-01-01)
 *
 * DISCIPLINE
 * ----------
 *   All type-sensitive code paths MUST go through the descriptor.
 *   add_path() type acceptance uses xpb_typeops_supported(),
 *   which delegates to xpb_typeops_lookup() — single source of truth.
 */

#ifndef XPB_TYPEOPS_H
#define XPB_TYPEOPS_H

#include "postgres.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "utils/tuplesort.h"

typedef int64 XpbVal;

typedef struct XpbTypeOps
{
    Oid         typid;
    const char *name;
    bool        byval;
    int16       attlen;
    XpbVal    (*getattr_fn)(HeapTupleHeader htup, AttrNumber attno, TupleDesc tupdesc);
    Datum     (*to_datum_fn)(XpbVal val);
    XpbVal    (*from_datum_fn)(Datum d);
    XpbVal      sentinel_min;
    XpbVal      sentinel_max;
} XpbTypeOps;

extern const XpbTypeOps *xpb_typeops_lookup(Oid typid);

extern const XpbTypeOps xpb_typeops_int4;
extern const XpbTypeOps xpb_typeops_int8;
extern const XpbTypeOps xpb_typeops_date;
extern const XpbTypeOps xpb_typeops_timestamp;

static inline bool
xpb_typeops_supported(Oid typid)
{
    return (xpb_typeops_lookup(typid) != NULL);
}

#endif  /* XPB_TYPEOPS_H */
