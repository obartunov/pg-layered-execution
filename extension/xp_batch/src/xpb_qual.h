/*
 * xpb_qual.h
 *
 * Unified qual pushdown engine for xp_batch.
 * Extracts pushable int-scalar predicates from RestrictInfo lists,
 * evaluates them against heap tuples, and provides EXPLAIN output.
 */

#ifndef XPB_QUAL_H
#define XPB_QUAL_H

#include "postgres.h"
#include "access/htup_details.h"
#include "commands/explain_format.h"
#include "nodes/pathnodes.h"

#ifndef XPB_MAX_PREDS
#define XPB_MAX_PREDS 8
#endif

/* Comparison operators */
typedef enum XpbQualOp
{
    XQO_LT = 0,
    XQO_LE,
    XQO_EQ,
    XQO_GE,
    XQO_GT
} XpbQualOp;

/* Single pushed predicate: col <op> const */
typedef struct XpbQualPred
{
    AttrNumber  attno;      /* column attribute number */
    XpbQualOp   op;         /* comparison operator */
    int64       rhs;        /* constant value (as int64) */
    const void *cached_ops; /* cached XpbTypeOps* (resolved at init) */
    int         fixed_offset; /* byte offset from t_hoff if all-fixed-width prefix, else -1 */
} XpbQualPred;

/* Qual fragment: set of AND-combined pushed predicates */
typedef struct XpbQual
{
    int          npreds;                    /* number of pushed predicates */
    XpbQualPred  preds[XPB_MAX_PREDS];     /* pushed predicate array */
    bool         has_residual;             /* non-pushable quals remain */

    /* Stream key bounds (derived from Class 1 preds at exec init) */
    int64        stream_key_lo;
    int64        stream_key_hi;
    bool         has_key_lo;
    bool         has_key_hi;
    int          n_key_bounds;
} XpbQual;

/*
 * Extract pushable predicates from base_quals.
 * k1att: leading stream key attno (Class 1 predicates use this column).
 * Non-pushable predicates set pq->has_residual = true.
 */
extern void xpb_qual_extract(List *base_quals, XpbQual *pq, AttrNumber k1att);

/* Test a heap tuple against all pushed predicates. Returns true if passes. */
extern bool xpb_qual_test_tuple(const XpbQual *pq,
                                HeapTupleHeader htup, TupleDesc tupdesc);

/* Return number of pushed predicates */
static inline int
xpb_qual_npreds(const XpbQual *pq)
{
    return pq->npreds;
}

/* EXPLAIN output for pushed quals */
extern void xpb_qual_explain(const XpbQual *pq, ExplainState *es);

#endif  /* XPB_QUAL_H */

/* Resolve cached typeops and fixed offsets for all predicates.
 * Must be called once after xpb_qual_extract, before test_tuple. */
extern void xpb_qual_resolve_cached(XpbQual *pq, TupleDesc tupdesc);

/* Fast inline test for first-column predicate at known offset.
 * Returns false if tuple definitely fails; true means "might pass, check fully". */
static inline bool
xpb_qual_fast_k1_check(const XpbQual *pq, HeapTupleHeader htup)
{
    /* Only works if first predicate has fixed_offset >= 0 */
    if (pq->npreds == 0 || pq->preds[0].fixed_offset < 0)
        return true;

    const XpbQualPred *pp = &pq->preds[0];
    int32 val = *(int32 *)((char *)htup + htup->t_hoff + pp->fixed_offset);

    switch (pp->op)
    {
        case XQO_LT: return val <  (int32)pp->rhs;
        case XQO_LE: return val <= (int32)pp->rhs;
        case XQO_EQ: return val == (int32)pp->rhs;
        case XQO_GE: return val >= (int32)pp->rhs;
        case XQO_GT: return val >  (int32)pp->rhs;
        default:     return true;
    }
}

