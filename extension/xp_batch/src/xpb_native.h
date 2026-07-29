/*-------------------------------------------------------------------------
 * xpb_native.h
 *
 * Native batch format + BatchProperties runtime interface.
 *
 * BatchProperties contract (per @yoda):
 *   - Producer establishes properties at runtime during scan
 *   - Consumer either uses them or takes explicit fallback
 *   - Both paths must be visible in EXPLAIN
 *   - Not a planner artifact — dynamic, established during execution
 *
 * Minimum viable interface:
 *   sorted_by_key  — key column values are ordered
 *   order_scope    — how far the ordering guarantee extends
 *   key_attno      — which column carries the key
 *   order_source   — human-readable: why we believe this property
 *-------------------------------------------------------------------------
 */
#ifndef XPB_NATIVE_H
#define XPB_NATIVE_H

#include "postgres.h"
#include "utils/memutils.h"

/* =========================================================
 * BatchOrderScope
 *
 * Defines how far the sorted_by_key guarantee extends.
 *
 * NONE:   no ordering claim — consumer must not rely on order
 * LOCAL:  sorted within a single batch, not across batches
 *         (e.g. one heap page: tuples in insertion order)
 * STREAM: sorted across all batches in the stream
 *         (e.g. sequential heap scan of table with monotonic key)
 *
 * Consumer contract:
 *   NONE   → must use fallback path (HashAgg, full sort)
 *   LOCAL  → may use local pre-aggregation + merge
 *   STREAM → may use streaming aggregation (no hash table)
 * ========================================================= */
typedef enum BatchOrderScope
{
    BATCH_ORDER_NONE   = 0,
    BATCH_ORDER_LOCAL  = 1,
    BATCH_ORDER_STREAM = 2,
} BatchOrderScope;

/* =========================================================
 * BatchProperties — runtime property carrier
 *
 * Established by producer (scanner) during execution.
 * Passed to consumer (aggregator) as part of batch transport.
 * Consumer reads once at Begin; properties are stream-level invariants.
 *
 * NOT a planner artifact. The planner does not see or set these.
 * This is the dynamic counterpart to pathkeys.
 * ========================================================= */
typedef struct BatchProperties
{
    /* --- Order --- */
    bool            sorted_by_key;  /* key column values are non-decreasing */
    BatchOrderScope order_scope;    /* how far the guarantee extends */
    AttrNumber      key_attno;      /* which column is the key (1-based) */

    /* --- Cardinality (optional, set when known exactly) --- */
    int64           exact_nrows;    /* exact row count if nrows_exact=true */
    bool            nrows_exact;

    /* --- Provenance --- */
    const char     *order_source;   /* why we claim this order:
                                     * "sequential heap scan",
                                     * "index scan on key",
                                     * "quick-probe first 2 pages", etc. */
} BatchProperties;

/* Initialise to "no properties claimed" — safe default */
static inline void
BatchPropertiesInit(BatchProperties *p)
{
    p->sorted_by_key = false;
    p->order_scope   = BATCH_ORDER_NONE;
    p->key_attno     = 0;
    p->exact_nrows   = 0;
    p->nrows_exact   = false;
    p->order_source  = "none";
}

/* =========================================================
 * NativeCountBatch — minimal transport for count(*)
 * ========================================================= */
typedef struct NativeCountBatch
{
    int64   nrows;
    bool    done;
} NativeCountBatch;

/* =========================================================
 * NativeIntBatch — transport for count/sum/min/max + properties
 * ========================================================= */
typedef struct NativeIntBatch
{
    int64           nrows;
    bool            done;
    int32          *values;
    bool           *nulls;

    /* Property carrier — filled by producer, read by consumer */
    BatchProperties props;
} NativeIntBatch;

#define XPB_NATIVE_SCAN_MAGIC  0x4E425343  /* "NBSC" */
#define XPB_NATIVE_AGG_MAGIC   0x4E424147  /* "NBAG" */

#endif /* XPB_NATIVE_H */
