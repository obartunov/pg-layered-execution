/*
 * xpb_colbatch.h — typed columnar batch contract (v2)
 *
 * A batch is a set of dense, equal-length column arrays with explicit type
 * and explicit validity.  Providers fill batches; consumers read them.
 * Neither side knows the other's storage internals.
 *
 * v1 was `int32 *int32_cols[]` with no validity at all.  Every operator
 * inferred the physical layout, NULL had no representation, and a source
 * that met a NULL either raised an error (pgcolumnar) or silently read the
 * wrong bytes (heap, before 326b116).  v2 makes type and NULLability part of
 * the contract so that neither can be inferred wrongly.
 *
 * WHAT THIS IS NOT
 * ----------------
 * Not generic PostgreSQL type support.  The set below is deliberately small
 * and closed: it is what the register workload needs, no more.  There is no
 * fmgr dispatch per value, no expression evaluation, no planner integration.
 *
 * DISPATCH DISCIPLINE
 * -------------------
 * The accessors below validate the column type on every call, so they are
 * meant to be called ONCE PER COLUMN PER BATCH and the resulting pointer
 * reused across the row loop.  Calling them inside a row loop is a
 * correctness no-op but throws away the point of a compact pipeline.
 *
 *     int32 *acct = xpcb_i32(batch, 2);        // hoisted
 *     for (int r = 0; r < batch->nrows; r++)   // tight loop, no dispatch
 *         ... acct[r] ...
 *
 * VALIDITY
 * --------
 *     validity == NULL   every row in the column is valid  (the cheap case;
 *                        a NOT NULL column pays nothing)
 *     validity != NULL   bit r set = row r is valid, clear = row r is NULL
 *
 * Bit-set-means-valid matches pgcolumnar's own present-bitmap convention, so
 * that source can hand its bitmap over without inverting it.
 *
 * NULL is never encoded as a sentinel value.  There is no reserved int32,
 * no magic negative, nothing an operator could collide with real data.
 *
 * OWNERSHIP AND LIFETIME
 * ----------------------
 * Ownership is per column, not per batch, because one batch legitimately
 * mixes both: the pgcolumnar source borrows a decoded stream for one column
 * while materializing another.
 *
 *     owns_data == true    the array was allocated in the consumer's context
 *                          and xpcb_free() releases it
 *     owns_data == false   the array is BORROWED from the source and is only
 *                          valid until the next next_batch()/rescan()/end()
 *                          call on that source
 *
 * No operator may retain a borrowed pointer past that window.  For the
 * pointer-valued types (numeric, varlena) a borrowed column means the
 * pointed-to bytes are the source's too; a column with owns_data == true
 * owns both the Datum array and everything it points at.
 */
#ifndef XPB_COLBATCH_H
#define XPB_COLBATCH_H

#include "postgres.h"
#include "utils/palloc.h"
#include <stdint.h>

#define XPCB_MAX_COLS   8
#define XPCB_BATCH_CAP  65536   /* max rows per batch */

/* ── Column types ── */

typedef enum XpbColType
{
    XPB_COL_UNSET = 0,
    XPB_COL_INT4,       /* int32  data[]                                    */
    XPB_COL_INT8,       /* int64  data[]                                    */
    XPB_COL_NUMERIC,    /* Datum  data[], each a Numeric (varlena)          */
    XPB_COL_VARLENA     /* Datum  data[], each a detoasted varlena          */
} XpbColType;

static inline const char *
xpcb_type_name(XpbColType t)
{
    switch (t)
    {
        case XPB_COL_INT4:    return "int4";
        case XPB_COL_INT8:    return "int8";
        case XPB_COL_NUMERIC: return "numeric";
        case XPB_COL_VARLENA: return "varlena";
        case XPB_COL_UNSET:   break;
    }
    return "unset";
}

/* Width of one element of the column's data array. */
static inline size_t
xpcb_type_width(XpbColType t)
{
    switch (t)
    {
        case XPB_COL_INT4:    return sizeof(int32);
        case XPB_COL_INT8:    return sizeof(int64);
        case XPB_COL_NUMERIC:
        case XPB_COL_VARLENA: return sizeof(Datum);
        case XPB_COL_UNSET:   break;
    }
    return 0;
}

/* ── One column ── */

typedef struct XpBatchColumn
{
    XpbColType  type;
    void       *data;           /* dense array, xpcb_type_width(type) each   */
    uint8      *validity;       /* NULL = all valid; else bit set = valid    */
    bool        owns_data;
    bool        owns_validity;
} XpBatchColumn;

/* ── Compact columnar batch ── */

typedef struct XpColumnBatch
{
    int             nrows;      /* rows in this batch                        */
    int             capacity;   /* allocated capacity                        */
    int             ncols;      /* number of columns                         */

    XpBatchColumn   cols[XPCB_MAX_COLS];

    /* Optional selection vector (NULL = all rows selected) */
    uint32         *selection;
    int             nselected;  /* valid only if selection != NULL           */
} XpColumnBatch;

/* ── Batch source provider contract ── */

typedef struct XpBatchSource XpBatchSource;

typedef struct XpBatchSourceOps
{
    /*
     * Fill 'batch' with the next chunk of rows.
     * Returns true if batch has rows, false when exhausted.
     * Provider sets each column's type, data, validity and ownership.
     */
    bool    (*next_batch)(XpBatchSource *src, XpColumnBatch *batch);

    /* Reset for rescan */
    void    (*rescan)(XpBatchSource *src);

    /* Release resources */
    void    (*end)(XpBatchSource *src);
} XpBatchSourceOps;

struct XpBatchSource
{
    const XpBatchSourceOps *ops;
    void                   *private_state;
};

/* ── Validity helpers ── */

#define XPCB_VALIDITY_BYTES(nrows)  (((size_t)(nrows) + 7) / 8)

static inline bool
xpcb_isnull(const XpColumnBatch *b, int col, int row)
{
    const uint8 *v = b->cols[col].validity;

    return v != NULL && (v[row >> 3] & (1 << (row & 7))) == 0;
}

static inline bool
xpcb_col_nullable(const XpColumnBatch *b, int col)
{
    return b->cols[col].validity != NULL;
}

/* Mark row valid/NULL while filling a column. Caller allocated the bitmap. */
static inline void
xpcb_set_valid(XpBatchColumn *c, int row)
{
    c->validity[row >> 3] |= (1 << (row & 7));
}

static inline void
xpcb_set_null(XpBatchColumn *c, int row)
{
    c->validity[row >> 3] &= ~(1 << (row & 7));
}

/* ── Typed accessors ── */

/*
 * Hoist these out of row loops (see DISPATCH DISCIPLINE above).  The check is
 * unconditional rather than an Assert: a type mismatch here reinterprets raw
 * memory, which is exactly the class of silent wrong answer this contract
 * exists to remove, and it must not disappear in a non-assert build.
 */
static inline void *
xpcb_typed(const XpColumnBatch *b, int col, XpbColType want)
{
    const XpBatchColumn *c;

    if (col < 0 || col >= b->ncols)
        elog(ERROR, "xp_batch: column %d out of range (ncols %d)", col, b->ncols);

    c = &b->cols[col];
    if (c->type != want)
        elog(ERROR, "xp_batch: column %d is %s, %s requested",
             col, xpcb_type_name(c->type), xpcb_type_name(want));
    if (c->data == NULL)
        elog(ERROR, "xp_batch: column %d has no data", col);

    return c->data;
}

static inline int32 *
xpcb_i32(const XpColumnBatch *b, int col)
{
    return (int32 *) xpcb_typed(b, col, XPB_COL_INT4);
}

static inline int64 *
xpcb_i64(const XpColumnBatch *b, int col)
{
    return (int64 *) xpcb_typed(b, col, XPB_COL_INT8);
}

static inline Datum *
xpcb_numeric(const XpColumnBatch *b, int col)
{
    return (Datum *) xpcb_typed(b, col, XPB_COL_NUMERIC);
}

static inline Datum *
xpcb_varlena(const XpColumnBatch *b, int col)
{
    return (Datum *) xpcb_typed(b, col, XPB_COL_VARLENA);
}

/*
 * Read an integer column as int64 regardless of whether it is int4 or int8.
 * For key handling that accepts both widths: dispatch once, then use the
 * returned flag to pick the tight loop -- do not call this per row.
 */
static inline bool
xpcb_is_integer(const XpColumnBatch *b, int col)
{
    XpbColType t = b->cols[col].type;

    return t == XPB_COL_INT4 || t == XPB_COL_INT8;
}

/* ── Lifecycle ── */

/*
 * Allocate owned storage for one column.  with_validity=false leaves
 * validity NULL, which is the "no NULLs, pay nothing" case; a source that
 * discovers a NULL later must allocate the bitmap before recording it.
 */
static inline void
xpcb_col_alloc(XpBatchColumn *c, XpbColType type, int capacity,
               bool with_validity)
{
    c->type = type;
    c->data = palloc(capacity * xpcb_type_width(type));
    c->owns_data = true;
    if (with_validity)
    {
        /* start all-valid; sources clear bits as they meet NULLs */
        c->validity = (uint8 *) palloc(XPCB_VALIDITY_BYTES(capacity));
        memset(c->validity, 0xFF, XPCB_VALIDITY_BYTES(capacity));
        c->owns_validity = true;
    }
    else
    {
        c->validity = NULL;
        c->owns_validity = false;
    }
}

/* Allocate a validity bitmap for a column that did not have one. */
static inline void
xpcb_col_add_validity(XpBatchColumn *c, int capacity)
{
    if (c->validity != NULL)
        return;
    c->validity = (uint8 *) palloc(XPCB_VALIDITY_BYTES(capacity));
    memset(c->validity, 0xFF, XPCB_VALIDITY_BYTES(capacity));
    c->owns_validity = true;
}

/* Point a column at data the source owns.  See OWNERSHIP AND LIFETIME. */
static inline void
xpcb_col_borrow(XpBatchColumn *c, XpbColType type, void *data,
                uint8 *validity)
{
    c->type = type;
    c->data = data;
    c->validity = validity;
    c->owns_data = false;
    c->owns_validity = false;
}

static inline void
xpcb_init_typed(XpColumnBatch *b, const XpbColType *types, int ncols,
                int capacity)
{
    memset(b, 0, sizeof(*b));
    b->ncols = ncols;
    b->capacity = capacity;
    for (int c = 0; c < ncols; c++)
        xpcb_col_alloc(&b->cols[c], types[c], capacity, false);
}

/* All-int4 batch: the shape benchmarks 02 and 04 use. */
static inline void
xpcb_init(XpColumnBatch *b, int ncols, int capacity)
{
    memset(b, 0, sizeof(*b));
    b->ncols = ncols;
    b->capacity = capacity;
    for (int c = 0; c < ncols; c++)
        xpcb_col_alloc(&b->cols[c], XPB_COL_INT4, capacity, false);
}

static inline void
xpcb_reset(XpColumnBatch *b)
{
    b->nrows = 0;
    b->nselected = 0;
    b->selection = NULL;
}

/*
 * Release only the columns this batch owns, leaving borrowed ones alone, and
 * mark them released so a second call is a no-op.  Intermediate batches
 * inside a pipeline are recycled per input batch, and they legitimately mix
 * borrowed and owned columns -- an operator that borrows when every row
 * matches and gathers when some do not produces a different ownership map on
 * each batch.  Freeing by a batch-level flag cannot express that, and gets it
 * wrong in both directions: it leaks the gathered buffers or frees memory
 * that belongs to the operator's own scratch space.
 */
/*
 * Refuse a batch carrying NULLs.
 *
 * The contract lets any source hand over a validity bitmap, and three of the
 * four sources can: the heap source's deform and projected paths, pgcolumnar,
 * ZLFS zones and Parquet all produce one when a column has NULLs.  Several
 * operators have no NULL branch at all -- they read cols[c].data[row] and
 * group or sum it -- and for those a bitmap is not a slow path, it is a wrong
 * answer.  Measured on a ZLFS zone over a nullable column:
 *
 *     PostgreSQL              252 groups
 *     xpb_batch_groupby       240 groups    (the NULL key merged into a value)
 *     zlfs_group_sum          240 groups
 *
 * The totals agreed in that case, which is why the group count is the thing to
 * compare: a sum-only check passes.
 *
 * An operator that cannot carry NULLs calls this once per batch and refuses
 * before reading anything.  That is the policy for the whole contract: a path
 * either implements the semantics or declines before execution.  It is NOT a
 * substitute for NULL support where the semantics are wanted -- see
 * xpb_typed_pipeline.c, which implements them.
 */
static inline void
xpcb_require_all_valid(const XpColumnBatch *b, int ncols, const char *where)
{
    for (int c = 0; c < ncols && c < XPCB_MAX_COLS; c++)
        if (b->cols[c].validity != NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("%s: column %d carries NULLs, which this path does not implement",
                            where, c),
                     errdetail("A NULL would be read as whatever the source left in the value slot, "
                               "changing the group it lands in and the sum it joins."),
                     errhint("Use a NOT NULL column, or a path that implements NULL semantics.")));
}

static inline void
xpcb_release_owned(XpColumnBatch *b)
{
    for (int c = 0; c < b->ncols; c++)
    {
        XpBatchColumn *col = &b->cols[c];

        if (col->owns_data && col->data)
            pfree(col->data);
        if (col->owns_validity && col->validity)
            pfree(col->validity);
        col->data = NULL;
        col->validity = NULL;
        col->owns_data = false;
        col->owns_validity = false;
    }
}

static inline void
xpcb_free(XpColumnBatch *b)
{
    for (int c = 0; c < b->ncols; c++)
    {
        XpBatchColumn *col = &b->cols[c];

        if (col->owns_data && col->data)
            pfree(col->data);
        if (col->owns_validity && col->validity)
            pfree(col->validity);
        col->data = NULL;
        col->validity = NULL;
    }
    if (b->selection)
        pfree(b->selection);
    memset(b, 0, sizeof(*b));
}

#endif /* XPB_COLBATCH_H */
