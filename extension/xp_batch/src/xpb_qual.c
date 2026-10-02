/*
 * xpb_qual.c
 *
 * Unified qual pushdown engine for xp_batch.
 *
 * Extracts simple (Var <op> Const) predicates from RestrictInfo lists
 * where the Var type is in the xpb_typeops supported set.
 * Non-pushable predicates set has_residual = true.
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "commands/explain_format.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "access/cmptype.h"

#include "xpb_qual.h"
#include "xpb_typeops.h"

/* -----------------------------------------------------------------------
 * Map btree strategy number to XpbQualOp
 * ----------------------------------------------------------------------- */
static bool
strategy_to_xqo(int strategy, XpbQualOp *op)
{
    switch (strategy)
    {
        case BTLessStrategyNumber:         *op = XQO_LT; return true;
        case BTLessEqualStrategyNumber:    *op = XQO_LE; return true;
        case BTEqualStrategyNumber:        *op = XQO_EQ; return true;
        case BTGreaterEqualStrategyNumber: *op = XQO_GE; return true;
        case BTGreaterStrategyNumber:      *op = XQO_GT; return true;
        default: return false;
    }
}

/* -----------------------------------------------------------------------
 * Try to extract one pushable predicate from an OpExpr.
 *
 * Pattern: (Var <cmpop> Const) or (Const <cmpop> Var)
 * Var type must be in xpb_typeops supported set.
 * Returns true if successfully extracted.
 * ----------------------------------------------------------------------- */
static bool
try_extract_pred(OpExpr *opexpr, XpbQualPred *pred)
{
    Var   *var;
    Const *cnst;
    bool   var_on_left;

    if (list_length(opexpr->args) != 2)
        return false;

    Node *left  = (Node *) linitial(opexpr->args);
    Node *right = (Node *) lsecond(opexpr->args);

    if (IsA(left, Var) && IsA(right, Const))
    {
        var  = (Var *)   left;
        cnst = (Const *) right;
        var_on_left = true;
    }
    else if (IsA(left, Const) && IsA(right, Var))
    {
        var  = (Var *)   right;
        cnst = (Const *) left;
        var_on_left = false;
    }
    else
        return false;

    /* Null constants can't be pushed */
    if (cnst->constisnull)
        return false;

    /* Type must be in our supported scalar set */
    const XpbTypeOps *ops = xpb_typeops_lookup(var->vartype);
    if (ops == NULL)
        return false;

    /* Determine comparison strategy from operator OID */
    Oid      opno = opexpr->opno;
    int      strategy = 0;

    /*
     * Look up btree strategy via get_op_index_interpretation (PG19).
     * Returns a list of OpIndexInterpretation; we need any btree match.
     */
    {
        List *interps = get_op_index_interpretation(opno);
        ListCell *lc2;
        bool found = false;

        foreach(lc2, interps)
        {
            OpIndexInterpretation *interp = (OpIndexInterpretation *) lfirst(lc2);
            /* CompareType values == btree strategy numbers (1..5) */
            if (interp->cmptype >= COMPARE_LT && interp->cmptype <= COMPARE_GT)
            {
                strategy = (int) interp->cmptype;
                found = true;
                break;
            }
        }
        list_free_deep(interps);
        if (!found)
            return false;
    }

    /* If Var is on right, flip the strategy */
    if (!var_on_left)
    {
        switch (strategy)
        {
            case BTLessStrategyNumber:        strategy = BTGreaterStrategyNumber; break;
            case BTLessEqualStrategyNumber:   strategy = BTGreaterEqualStrategyNumber; break;
            case BTGreaterEqualStrategyNumber: strategy = BTLessEqualStrategyNumber; break;
            case BTGreaterStrategyNumber:      strategy = BTLessStrategyNumber; break;
            /* EQ stays EQ */
        }
    }

    XpbQualOp xqo;
    if (!strategy_to_xqo(strategy, &xqo))
        return false;

    pred->attno = var->varattno;
    pred->op    = xqo;
    pred->rhs   = ops->from_datum_fn(cnst->constvalue);
    return true;
}

/* -----------------------------------------------------------------------
 * xpb_qual_extract
 *
 * Walk a list of Expr clauses (already unwrapped from RestrictInfo
 * by the caller in xpb_groupagg2.c) and extract pushable predicates.
 * k1att: leading stream key (for reference, not used in extraction filter).
 * ----------------------------------------------------------------------- */
void
xpb_qual_extract(List *base_quals, XpbQual *pq, AttrNumber k1att)
{
    ListCell *lc;

    memset(pq, 0, sizeof(XpbQual));

    foreach(lc, base_quals)
    {
        Expr *clause = (Expr *) lfirst(lc);

        if (pq->npreds >= XPB_MAX_PREDS)
        {
            pq->has_residual = true;
            continue;
        }

        if (IsA(clause, OpExpr))
        {
            XpbQualPred pred;
            if (try_extract_pred((OpExpr *) clause, &pred))
            {
                pq->preds[pq->npreds++] = pred;
                continue;
            }
        }

        /* Could not push this predicate */
        pq->has_residual = true;
    }
}

/* -----------------------------------------------------------------------
 * xpb_qual_resolve_cached
 *
 * Resolve typeops pointers and compute fixed byte offsets for all
 * predicates. Must be called once before test_tuple.
 * For all-fixed-width NOT NULL prefix columns, computes byte offset
 * from t_hoff for direct memory access without getattr walk.
 * ----------------------------------------------------------------------- */
void
xpb_qual_resolve_cached(XpbQual *pq, TupleDesc tupdesc)
{
    for (int i = 0; i < pq->npreds; i++)
    {
        XpbQualPred *pp = &pq->preds[i];
        Oid typid = TupleDescAttr(tupdesc, pp->attno - 1)->atttypid;
        pp->cached_ops = xpb_typeops_lookup(typid);
        pp->fixed_offset = -1;

        /* Check if all columns before this one (and this one) are
         * fixed-width and NOT NULL — then we can compute a direct offset */
        bool all_fixed = true;
        int  offset = 0;
        for (int c = 0; c < pp->attno; c++)
        {
            Form_pg_attribute att = TupleDescAttr(tupdesc, c);
            if (att->attlen <= 0 || att->attbyval == false && att->attlen > 8)
            {
                all_fixed = false;
                break;
            }
            if (!att->attnotnull)
            {
                all_fixed = false;
                break;
            }

            /*
             * R1-15, second site.  This said
             *
             *     offset += att->attlen;
             *
             * under a comment claiming alignment, and the reachability map
             * carried it as a known hole ("Unaligned fixed_offset"): for
             * (int4, int8) a predicate on the int8 read bytes 4..11.  Align
             * the RUNNING offset, then add the length -- and align once more
             * for the target attribute itself, whose own padding is equally
             * real.
             */
            if (c < pp->attno - 1)
            {
                offset = att_align_nominal(offset, att->attalign);
                offset += att->attlen;
            }
            else
                offset = att_align_nominal(offset, att->attalign);
        }
        if (all_fixed)
            pp->fixed_offset = offset;
    }
}

/* -----------------------------------------------------------------------
 * xpb_qual_test_tuple
 *
 * Test a heap tuple against all pushed predicates (AND-semantics).
 * Uses cached typeops and fixed offsets when available.
 * Returns true if tuple passes all predicates.
 * ----------------------------------------------------------------------- */
bool
xpb_qual_test_tuple(const XpbQual *pq, HeapTupleHeader htup, TupleDesc tupdesc)
{
    for (int i = 0; i < pq->npreds; i++)
    {
        const XpbQualPred *pp = &pq->preds[i];
        int64 val;

        if (pp->fixed_offset >= 0)
        {
            /* Fast path: direct memory access at known offset */
            const XpbTypeOps *ops = (const XpbTypeOps *)pp->cached_ops;
            if (ops && ops->attlen == 4)
                val = (int64) *(int32 *)((char *)htup + htup->t_hoff + pp->fixed_offset);
            else if (ops && ops->attlen == 8)
                val = *(int64 *)((char *)htup + htup->t_hoff + pp->fixed_offset);
            else
                goto slow_path;
        }
        else
        {
slow_path:
            ;
            const XpbTypeOps *ops = (const XpbTypeOps *)pp->cached_ops;
            if (!ops)
                ops = xpb_typeops_lookup(
                    TupleDescAttr(tupdesc, pp->attno - 1)->atttypid);

            if (ops)
                val = ops->getattr_fn(htup, pp->attno, tupdesc);
            else
            {
                bool  isnull;
                Datum d = heap_getattr(&(HeapTupleData){
                    .t_len = HeapTupleHeaderGetDatumLength(htup),
                    .t_data = htup
                }, pp->attno, tupdesc, &isnull);
                if (isnull) return false;
                val = (int64) DatumGetInt32(d);
            }
        }

        bool ok;
        switch (pp->op)
        {
            case XQO_LT: ok = val <  pp->rhs; break;
            case XQO_LE: ok = val <= pp->rhs; break;
            case XQO_EQ: ok = val == pp->rhs; break;
            case XQO_GE: ok = val >= pp->rhs; break;
            case XQO_GT: ok = val >  pp->rhs; break;
            default:     ok = true;            break;
        }
        if (!ok)
            return false;
    }
    return true;
}

/* -----------------------------------------------------------------------
 * xpb_qual_explain
 * ----------------------------------------------------------------------- */
static const char *
xqo_name(XpbQualOp op)
{
    switch (op)
    {
        case XQO_LT: return "<";
        case XQO_LE: return "<=";
        case XQO_EQ: return "=";
        case XQO_GE: return ">=";
        case XQO_GT: return ">";
        default:     return "?";
    }
}

void
xpb_qual_explain(const XpbQual *pq, ExplainState *es)
{
    if (pq->npreds == 0)
        return;

    for (int i = 0; i < pq->npreds; i++)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "col%d %s %ld",
                 pq->preds[i].attno, xqo_name(pq->preds[i].op),
                 (long) pq->preds[i].rhs);
        ExplainPropertyText("Pushed Pred", buf, es);
    }
    if (pq->has_residual)
        ExplainPropertyBool("Has Residual", true, es);
}
