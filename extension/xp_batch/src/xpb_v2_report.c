/*
 * xpb_v2_report.c — the Benchmark 05-A register report, on the typed contract
 *
 *   source(reg2 | reg2_col | ZLFS zone, period BETWEEN lo AND hi)
 *     -> join dim_company   (company_key int4 -> company_group)
 *     -> join dim_account2  (account_key int8 -> account_group)
 *     -> GROUP BY company_group, account_group, company_key
 *     -> SUM(debit_cents), SUM(credit_cents), and their difference
 *
 * Separate from xpb_1c_register_report(), which is what benchmarks 02 and 04
 * measure and which stays on the int4 fixed-offset fast path with its
 * checksums unmoved.  This one reads the v2 row shape: a varlena at attnum 2
 * ahead of everything it touches, so the heap arm runs on the GENERIC DEFORM
 * PATH.  That is deliberate and is recorded in the report line -- its source
 * timing is not comparable with benchmark 04's fixed-offset source, because
 * it is not the same mechanism.
 *
 * Only the period predicate changes between measurements.  The joins, the
 * grouping and the aggregate count are fixed, so that what moves across
 * selectivity points is the source and nothing else.
 *
 * WHY int8 MONEY AND NOT numeric
 * ------------------------------
 * Every batch source can carry an int8.  None can carry a numeric: the ZLFS
 * zone format holds int4 and int8, and the pgColumnar source refuses a
 * variable-width stream by name.  Aggregating the numeric columns would
 * therefore leave nothing to compare across paths, which is the whole point
 * of the exercise.  The numeric columns stay in the row -- they are what puts
 * the heap arm on the deform path -- and are measured separately on heap
 * alone.
 *
 * The int8 turnover sums are accumulated with overflow checks and raise
 * "bigint out of range" exactly as PostgreSQL's own int8 arithmetic does.
 * Note what the benchmark gate can and cannot see: sum(debit) - sum(credit)
 * = sum(debit - credit) is a useful invariant, but it is NOT an overflow
 * detector -- under two's-complement wraparound both sides wrap identically
 * and the identity still holds.  It held on a wrapped result before these
 * checks existed, which is why the gate never noticed.
 */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "common/int.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "access/table.h"
#include "utils/tuplestore.h"

#include "xpb_colbatch.h"
#include "xpb_src_heap.h"
#include "xpb_zlfs.h"

extern XpBatchSource *xpb_zlfs_source_create(ZlfsZone *zone, int16 *attnos, int ncols);
extern XpBatchSource *xpcn_source_create(Oid relid, int16 *requested_attnos,
                                         int ncols, bool has_pred,
                                         int32 pred_lo, int32 pred_hi);
extern void xpcn_source_pruning(XpBatchSource *src, int64 *groups_read,
                                int64 *group_rows_seen,
                                int64 *rows_vec_skipped, int64 *rows_emitted);

PG_FUNCTION_INFO_V1(xpb_v2_register_report);

#define V2_DIM1_CAP  256        /* 50 companies   */
#define V2_DIM2_CAP  1024       /* 200 accounts   */
/*
 * Group hash table.
 *
 * Until Hash Aggregate Growth v1 this was a fixed array of V2_GRP_CAP slots
 * with a hard 3/4 load limit and an ERROR past it; benchmark 05-E measured
 * that ceiling at 12 288 groups and showed it was a chosen constant rather
 * than a resource limit.  The table now grows instead.
 *
 * The growth policy is PREREGISTERED -- fixed before any measurement, and not
 * to be tuned from the first results:
 *
 *     grow when the table is half full
 *     double the capacity
 *
 * Everything else is deliberately untouched, so that this milestone isolates
 * growth alone: same hash function, same key equality, same linear probing,
 * same key and aggregate-state layout, same grouping semantics.
 */
#define V2_GRP_CAP   16384      /* INITIAL capacity; the table grows from here */

/*
 * Written as a fraction of the live capacity rather than a second constant,
 * so the threshold cannot drift out of step with the capacity it refers to.
 */
#define V2_GRP_GROW_AT(cap)  ((cap) / 2)

/* batch column order; the predicate column must be first and int4 */
#define V2_C_PERIOD  0
#define V2_C_COMPANY 1
#define V2_C_ACCOUNT 2
#define V2_C_DEBIT   3
#define V2_C_CREDIT  4
#define V2_NCOLS     5

typedef struct V2Dim1 { bool occupied; int32 key; int32 payload; } V2Dim1;
typedef struct V2Dim2 { bool occupied; int64 key; int32 payload; } V2Dim2;

/*
 * Dimension hash tables (Dimension Hash Growth v1).
 *
 * These were fixed arrays with a 3/4 load limit that raised an error, which is
 * what made the group-hash result in benchmark 05-E possible to state but also
 * what capped the whole harness once the group table could grow: reg2_card2
 * only reaches 147 456 groups by holding BOTH of these at their limit -- 192
 * of 256 slots and 768 of 1024. Skew attacks load factor, so measuring skew
 * against saturated dimension tables would make a regression impossible to
 * attribute to one hash or the other.
 *
 * Same preregistered policy as the group hash, and nothing else changes: grow
 * when the table is half full, double, rehash, same hash function, same key
 * equality, same linear probing, same payload, same NULL handling, same
 * duplicate-key behaviour. No spill.
 *
 * The two tables are deliberately NOT unified. dim1 keys are int32 and dim2
 * keys are int64, and both hash with (uint32) key * 2654435761u -- so dim2
 * truncates its key to 32 bits before hashing. That is existing behaviour and
 * is preserved exactly here; it is recorded rather than fixed because changing
 * it would change which keys collide, which is a different experiment. A
 * single generic table would have to hide that asymmetry to look tidy.
 */
#define V2_DIM_GROW_AT(cap)  ((cap) / 2)

/*
 * Test-only initial capacities for the two dimension tables.  Zero means the
 * production policy.  They exist so the paired control can pre-size a table to
 * the capacity natural growth would have reached, measuring what doubling cost,
 * and so growth can be forced in tests without a huge dimension.  Diagnostic
 * only: the real path always grows from its normal initial capacity.
 */
static int  v2_dim1_test_init_cap = 0;
static int  v2_dim2_test_init_cap = 0;

/* Per-table observation.  Never pooled across the two dimensions. */
typedef struct V2DimStats
{
    int         initial_capacity;
    int         capacity;
    int         entries;
    int64       lookups;
    int64       insertions;
    int64       probes;             /* slots examined, build + lookup     */
    int         max_probe_current;  /* since the most recent resize       */
    int         max_probe_lifetime; /* never reset                        */
    int         growths;
    int64       rehash_entries;
    double      rehash_ms;
    size_t      bytes_current;
    size_t      bytes_peak;         /* old + new while both are live      */
} V2DimStats;

typedef struct V2Dim1Table
{
    V2Dim1         *slots;
    MemoryContext   cxt;
    V2DimStats      st;
} V2Dim1Table;

typedef struct V2Dim2Table
{
    V2Dim2         *slots;
    MemoryContext   cxt;
    V2DimStats      st;
} V2Dim2Table;

static void
v2_dim_report(StringInfo out, const char *what, const V2DimStats *st,
              MemoryContext cxt)
{
    appendStringInfo(out,
                     "  %s_initial_cap=%d %s_cap=%d %s_grow_at=%d"
                     " %s_entries=%d %s_load_factor=%.4f"
                     " %s_bytes=%zu %s_bytes_peak=%zu %s_cxt_bytes=%zu"
                     " %s_lookups=" INT64_FORMAT " %s_insertions=" INT64_FORMAT
                     " %s_probes=" INT64_FORMAT
                     " %s_probes_per_lookup_lifetime=%.4f"
                     " %s_max_probe_current=%d %s_max_probe_lifetime=%d"
                     " %s_growths=%d %s_rehashes=%d"
                     " %s_rehash_entries=" INT64_FORMAT " %s_rehash_ms=%.3f",
                     what, st->initial_capacity, what, st->capacity,
                     what, V2_DIM_GROW_AT(st->capacity),
                     what, st->entries,
                     what, (double) st->entries / st->capacity,
                     what, st->bytes_current, what, st->bytes_peak,
                     /*
                      * Measured by the memory system rather than by this
                      * file's arithmetic: if a growth ever failed to release
                      * its predecessor this would exceed _bytes.
                      */
                     what, MemoryContextMemAllocated(cxt, false),
                     what, st->lookups, what, st->insertions,
                     what, st->probes,
                     /*
                      * Named _lifetime because it pools every probe made at
                      * every capacity, build and lookup alike.  It is a figure
                      * about the whole execution, not about the final table --
                      * the group hash's unqualified equivalent had to be
                      * relabelled after review for exactly that reason.
                      */
                     what,
                     (st->lookups + st->insertions) > 0
                         ? (double) st->probes / (double) (st->lookups + st->insertions)
                         : 0.0,
                     what, st->max_probe_current,
                     what, st->max_probe_lifetime,
                     /* one rehash per growth by construction, both printed */
                     what, st->growths, what, st->growths,
                     what, st->rehash_entries, what, st->rehash_ms);
}

static inline void
v2_dim_note_probe(V2DimStats *st, int len)
{
    st->probes += len;
    if (len > st->max_probe_current)
        st->max_probe_current = len;
    if (len > st->max_probe_lifetime)
        st->max_probe_lifetime = len;
}

/*
 * Capacity arithmetic, shared because it is the part that has nothing to do
 * with the key type: doubling is checked for overflow in the slot count and in
 * the byte size before anything is allocated, and the caller's table is left
 * untouched if either cannot be represented.  Returns the new byte size.
 */
static size_t
v2_dim_next_bytes(const char *what, int oldcap, size_t elemsize, int *newcap)
{
    size_t  bytes;

    if (oldcap > INT_MAX / 2)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("v2_register_report: %s hash cannot grow past %d slots",
                        what, oldcap),
                 errdetail("Doubling would overflow the slot count.")));
    *newcap = oldcap * 2;

    if ((size_t) *newcap > SIZE_MAX / elemsize)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("v2_register_report: %s hash cannot grow past %d slots",
                        what, oldcap),
                 errdetail("Doubling would overflow the allocation size.")));
    bytes = (size_t) *newcap * elemsize;

    if (bytes > MaxAllocSize)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("v2_register_report: %s hash would need %zu bytes, over the %zu byte allocation limit",
                        what, bytes, (size_t) MaxAllocSize),
                 errdetail("No spill is implemented; this is a hard boundary.")));
    return bytes;
}

typedef struct V2Group
{
    bool    occupied;
    int32   company_group;
    int32   account_group;
    int32   company_key;
    int64   debit;
    int64   credit;
} V2Group;

/*
 * Test-only policy overrides.  Zero means "use the production policy".  They
 * exist so the correctness tests can force several growths, and hit the exact
 * growth threshold, without building a million-group dataset -- and so that a
 * refused growth can be exercised deliberately instead of by inducing a real
 * OOM.  The benchmark never sets them.
 */
static int  v2_grp_test_init_cap = 0;
static int  v2_grp_test_max_cap = 0;

typedef struct V2GroupTable
{
    V2Group        *slots;
    int             capacity;       /* always a power of two                 */
    int             ngroups;
    MemoryContext   cxt;            /* owns every generation of slots        */

    /* observation; none of it is derived after the fact */
    int             initial_capacity;
    int64           inserts;        /* new groups created                    */
    int64           hits;           /* existing group found                  */
    int64           probes;         /* slots examined by the aggregate loop  */
    /*
      * Two probe high-water marks, because one number cannot answer both
      * questions.  max_probe_lifetime is the worst chain ever walked, which
      * happens just before a growth when the table is at its densest;
      * max_probe_current is reset on every growth and therefore describes the
      * table that actually answered the query.  They were previously one
      * counter, which reported the pre-growth figure and made it look like a
      * property of the grown table.
      */
    int             max_probe_current;
    int             max_probe_lifetime;
    int             growths;
    int64           rehash_groups;  /* groups reinserted, summed over growths */
    int64           rehash_probes;  /* kept apart from aggregate-loop probes  */
    double          rehash_ms;
    size_t          bytes_current;
    size_t          bytes_peak;     /* includes both tables during a rehash  */
} V2GroupTable;

/*
 * Unchanged from the fixed-capacity implementation, character for character.
 * Factored into a function only because rehash has to recompute the same
 * value from the stored keys; this milestone must not alter hash behaviour
 * (section 5), and 05-E's probe statistics remain comparable because of it.
 */
static inline uint32
v2_group_hash(int32 g1, int32 g2, int32 ck)
{
    return (uint32) g1 * 2654435761u ^ (uint32) g2 * 2246822519u
         ^ (uint32) ck * 0x45d9f3bu;
}

static inline void
v2_grp_note_probe(V2GroupTable *t, int len)
{
    if (len > t->max_probe_current)
        t->max_probe_current = len;
    if (len > t->max_probe_lifetime)
        t->max_probe_lifetime = len;
}

static void
v2_grp_init(V2GroupTable *t)
{
    int cap = v2_grp_test_init_cap > 0 ? v2_grp_test_init_cap : V2_GRP_CAP;

    memset(t, 0, sizeof(*t));
    /*
     * A context of its own, so that "growth does not accumulate obsolete
     * tables" can be checked from outside this file's own accounting
     * (section 8) rather than only by trusting the counters below.
     */
    t->cxt = AllocSetContextCreate(CurrentMemoryContext,
                                   "xpb v2 group hash",
                                   ALLOCSET_DEFAULT_SIZES);
    t->capacity = cap;
    t->initial_capacity = cap;
    t->bytes_current = (size_t) cap * sizeof(V2Group);
    t->bytes_peak = t->bytes_current;
    t->slots = MemoryContextAllocZero(t->cxt, t->bytes_current);
}

/*
 * Double the table and reinsert every live group.
 *
 * Called only when a new group is needed and the table is already at least
 * half full, so the decision depends on ngroups and capacity alone and never
 * on where probing happened to stop (section 12).
 */
static void
v2_grp_grow(V2GroupTable *t)
{
    V2Group    *old = t->slots;
    int         oldcap = t->capacity;
    int         newcap;
    size_t      newbytes;
    instr_time  gs, ge;

    INSTR_TIME_SET_CURRENT(gs);

    /*
     * Overflow safety (section 7): the slot count and the byte size are both
     * checked BEFORE anything is allocated, and the table is left entirely
     * untouched if either cannot be represented.  Capacity is never wrapped.
     */
    if (oldcap > INT_MAX / 2)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("v2_register_report: group hash cannot grow past %d slots",
                        oldcap),
                 errdetail("Doubling would overflow the slot count.")));
    newcap = oldcap * 2;

    if ((size_t) newcap > SIZE_MAX / sizeof(V2Group))
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("v2_register_report: group hash cannot grow past %d slots",
                        oldcap),
                 errdetail("Doubling would overflow the allocation size.")));
    newbytes = (size_t) newcap * sizeof(V2Group);

    if (v2_grp_test_max_cap > 0 && newcap > v2_grp_test_max_cap)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("v2_register_report: group hash may not grow past %d slots (test policy)",
                        v2_grp_test_max_cap),
                 errdetail("%d groups held in %d slots.", t->ngroups, oldcap)));

    /*
     * v1 keeps the table inside one ordinary palloc.  Past MaxAllocSize that
     * is a real boundary and is refused cleanly rather than worked around
     * with a huge allocation: no spill, no partitioning in this milestone
     * (section 25).
     */
    if (newbytes > MaxAllocSize)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("v2_register_report: group hash would need " UINT64_FORMAT " bytes, over the %zu byte allocation limit",
                        (uint64) newbytes, (size_t) MaxAllocSize),
                 errdetail("%d groups held in %d slots.", t->ngroups, oldcap)));

    /* Both tables are live between here and the pfree below (section 22). */
    if (t->bytes_current + newbytes > t->bytes_peak)
        t->bytes_peak = t->bytes_current + newbytes;

    t->slots = MemoryContextAllocZero(t->cxt, newbytes);
    t->capacity = newcap;

    /*
     * Reinsert, do not copy.  A slot index is a function of the capacity, so
     * memcpying occupied slots to the same indexes would leave groups sitting
     * where the probe sequence for their key can no longer reach them --
     * every subsequent lookup would create a duplicate group and the sums
     * would silently split (section 6).
     */
    for (int i = 0; i < oldcap; i++)
    {
        V2Group    *g = &old[i];
        uint32      h;
        bool        placed = false;

        if (!g->occupied)
            continue;

        h = v2_group_hash(g->company_group, g->account_group, g->company_key);
        for (int probe = 0; probe < newcap; probe++)
        {
            int         idx = (int) ((h + probe) & (newcap - 1));
            V2Group    *dst = &t->slots[idx];

            if (!dst->occupied)
            {
                *dst = *g;
                t->rehash_probes += probe + 1;
                placed = true;
                break;
            }
        }
        /*
         * Unreachable: the new table is at most a quarter full here.  Checked
         * anyway, because the alternative to noticing is losing a group's
         * accumulated sums without any other symptom.
         */
        if (!placed)
            elog(ERROR, "v2_register_report: rehash found no free slot in %d",
                 newcap);
        t->rehash_groups++;
    }

    pfree(old);
    t->bytes_current = newbytes;
    t->growths++;
    /*
     * The chain lengths just measured belong to the table that has been
     * replaced.  Only the lifetime mark carries across.
     */
    t->max_probe_current = 0;

    INSTR_TIME_SET_CURRENT(ge);
    t->rehash_ms += INSTR_TIME_GET_MILLISEC(ge) - INSTR_TIME_GET_MILLISEC(gs);
}

/*
 * Accumulate one row into its group, growing the table first if a new group
 * is needed and the table is already half full.
 *
 * Invariant, and the thing the threshold tests pin down: a group is only ever
 * added to a table that is strictly less than half full, so ngroups can reach
 * exactly capacity/2 but the load factor never exceeds 0.5.
 */
static inline void
v2_grp_upsert(V2GroupTable *t, int32 g1, int32 g2, int32 ck,
              int64 dt, int64 kt)
{
    uint32  h = v2_group_hash(g1, g2, ck);

    for (;;)
    {
        int     cap = t->capacity;

        for (int probe = 0; probe < cap; probe++)
        {
            int         idx = (int) ((h + probe) & (cap - 1));
            V2Group    *g = &t->slots[idx];

            if (!g->occupied)
            {
                if (t->ngroups >= V2_GRP_GROW_AT(cap))
                    break;          /* grow, then probe again from the top */
                g->occupied = true;
                g->company_group = g1;
                g->account_group = g2;
                g->company_key = ck;
                g->debit = dt;
                g->credit = kt;
                t->ngroups++;
                t->inserts++;
                t->probes += probe + 1;
                v2_grp_note_probe(t, probe + 1);
                return;
            }
            if (g->company_group == g1 && g->account_group == g2 &&
                g->company_key == ck)
            {
                /*
                 * Checked, not wrapped.  An unchecked += here returned a
                 * silently negative turnover on inputs PostgreSQL refuses,
                 * and signed overflow is undefined behaviour besides.  The
                 * message matches PostgreSQL's own int8 arithmetic so a
                 * caller cannot tell the two apart.
                 */
                if (pg_add_s64_overflow(g->debit, dt, &g->debit) ||
                    pg_add_s64_overflow(g->credit, kt, &g->credit))
                    ereport(ERROR,
                            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                             errmsg("bigint out of range"),
                             errdetail("Group (%d, %d, %d) overflowed an int8 turnover sum.",
                                       g1, g2, ck)));
                t->hits++;
                t->probes += probe + 1;
                v2_grp_note_probe(t, probe + 1);
                return;
            }
        }
        v2_grp_grow(t);
    }
}

/*
 * Dimensions are read through the typed heap source on the deform path, so
 * they inherit its guards rather than addressing tuples themselves.  A NULL
 * key is skipped: under SQL semantics it can never equal a fact key, so
 * storing it could only ever produce a wrong match.
 */
static void
v2_dim1_init(V2Dim1Table *t)
{
    memset(t, 0, sizeof(*t));
    t->cxt = AllocSetContextCreate(CurrentMemoryContext,
                                   "xpb v2 dim1 hash",
                                   ALLOCSET_DEFAULT_SIZES);
    t->st.capacity = v2_dim1_test_init_cap > 0 ? v2_dim1_test_init_cap : V2_DIM1_CAP;
    t->st.initial_capacity = t->st.capacity;
    t->st.bytes_current = (size_t) t->st.capacity * sizeof(V2Dim1);
    t->st.bytes_peak = t->st.bytes_current;
    t->slots = MemoryContextAllocZero(t->cxt, t->st.bytes_current);
}

static void
v2_dim1_grow(V2Dim1Table *t)
{
    V2Dim1     *old = t->slots;
    int         oldcap = t->st.capacity;
    int         newcap;
    size_t      newbytes = v2_dim_next_bytes("dim1", oldcap, sizeof(V2Dim1), &newcap);
    instr_time  gs, ge;

    INSTR_TIME_SET_CURRENT(gs);

    /* both tables are live from here until the pfree below */
    if (t->st.bytes_current + newbytes > t->st.bytes_peak)
        t->st.bytes_peak = t->st.bytes_current + newbytes;

    t->slots = MemoryContextAllocZero(t->cxt, newbytes);
    t->st.capacity = newcap;

    /*
     * Reinserted with the new mask, not copied: a slot index is a function of
     * the capacity, so memcpying to the same indexes would leave entries where
     * the probe sequence for their key can no longer find them, and a lookup
     * would then report a missing dimension key -- a wrong join result, not an
     * error.
     */
    for (int i = 0; i < oldcap; i++)
    {
        uint32  h;
        bool    placed = false;

        if (!old[i].occupied)
            continue;
        h = (uint32) old[i].key * 2654435761u;
        for (int probe = 0; probe < newcap; probe++)
        {
            int idx = (int) ((h + probe) & (newcap - 1));

            if (!t->slots[idx].occupied)
            {
                t->slots[idx] = old[i];
                placed = true;
                break;
            }
        }
        if (!placed)
            elog(ERROR, "v2_register_report: dim1 rehash found no free slot in %d",
                 newcap);
        t->st.rehash_entries++;
    }

    pfree(old);
    t->st.bytes_current = newbytes;
    t->st.growths++;
    /* the chains just measured belong to the table that has been replaced */
    t->st.max_probe_current = 0;

    INSTR_TIME_SET_CURRENT(ge);
    t->st.rehash_ms += INSTR_TIME_GET_MILLISEC(ge) - INSTR_TIME_GET_MILLISEC(gs);
}

/*
 * Insert or ignore, preserving the original duplicate-key behaviour: the first
 * payload seen for a key wins and later rows with the same key are dropped.
 *
 * Growth is decided from entries and capacity alone -- never from where probing
 * stopped -- so the threshold cannot depend on insertion order. Invariant: an
 * entry is only ever added to a table that is strictly less than half full, so
 * entries can reach exactly capacity/2 and the load factor never exceeds 0.5.
 */
static void
v2_dim1_insert(V2Dim1Table *t, int32 key, int32 payload)
{
    uint32  h = (uint32) key * 2654435761u;

    for (;;)
    {
        int cap = t->st.capacity;

        for (int probe = 0; probe < cap; probe++)
        {
            int idx = (int) ((h + probe) & (cap - 1));

            if (!t->slots[idx].occupied)
            {
                if (t->st.entries >= V2_DIM_GROW_AT(cap))
                    break;              /* grow, then probe again */
                t->slots[idx].occupied = true;
                t->slots[idx].key = key;
                t->slots[idx].payload = payload;
                t->st.entries++;
                t->st.insertions++;
                v2_dim_note_probe(&t->st, probe + 1);
                return;
            }
            if (t->slots[idx].key == key)
            {
                v2_dim_note_probe(&t->st, probe + 1);
                return;                 /* duplicate: first payload wins */
            }
        }
        v2_dim1_grow(t);
    }
}

static void
v2_dim1_build(V2Dim1Table *t, Oid relid)
{
    int16           attnos[2] = {1, 2};
    XpBatchSource  *src = xpb_heap_source_create_ex(relid, attnos, 2,
                                                    XPB_HEAP_DEFORM, false, 0, 0);
    XpColumnBatch   batch;

    memset(&batch, 0, sizeof(batch));
    batch.capacity = 1024;
    batch.ncols = 2;

    while (src->ops->next_batch(src, &batch))
    {
        int32 *k = xpcb_i32(&batch, 0);
        int32 *p = xpcb_i32(&batch, 1);

        for (int r = 0; r < batch.nrows; r++)
        {
            /*
             * A NULL key or payload is skipped, unchanged: under SQL semantics
             * a NULL key can never equal a fact key, so storing it could only
             * produce a wrong match.
             */
            if (xpcb_isnull(&batch, 0, r) || xpcb_isnull(&batch, 1, r))
                continue;
            v2_dim1_insert(t, k[r], p[r]);
        }
        xpcb_release_owned(&batch);
    }
    src->ops->end(src);
}

static void
v2_dim2_init(V2Dim2Table *t)
{
    memset(t, 0, sizeof(*t));
    t->cxt = AllocSetContextCreate(CurrentMemoryContext,
                                   "xpb v2 dim2 hash",
                                   ALLOCSET_DEFAULT_SIZES);
    t->st.capacity = v2_dim2_test_init_cap > 0 ? v2_dim2_test_init_cap : V2_DIM2_CAP;
    t->st.initial_capacity = t->st.capacity;
    t->st.bytes_current = (size_t) t->st.capacity * sizeof(V2Dim2);
    t->st.bytes_peak = t->st.bytes_current;
    t->slots = MemoryContextAllocZero(t->cxt, t->st.bytes_current);
}

static void
v2_dim2_grow(V2Dim2Table *t)
{
    V2Dim2     *old = t->slots;
    int         oldcap = t->st.capacity;
    int         newcap;
    size_t      newbytes = v2_dim_next_bytes("dim2", oldcap, sizeof(V2Dim2), &newcap);
    instr_time  gs, ge;

    INSTR_TIME_SET_CURRENT(gs);

    if (t->st.bytes_current + newbytes > t->st.bytes_peak)
        t->st.bytes_peak = t->st.bytes_current + newbytes;

    t->slots = MemoryContextAllocZero(t->cxt, newbytes);
    t->st.capacity = newcap;

    /*
     * Reinserted with the new mask, not copied -- same reasoning as dim1.  Note
     * the (uint32) cast on an int64 key: that truncation is the existing hash
     * and is reproduced here deliberately, so a rehashed entry lands where a
     * fresh lookup will look for it.
     */
    for (int i = 0; i < oldcap; i++)
    {
        uint32  h;
        bool    placed = false;

        if (!old[i].occupied)
            continue;
        h = (uint32) old[i].key * 2654435761u;
        for (int probe = 0; probe < newcap; probe++)
        {
            int idx = (int) ((h + probe) & (newcap - 1));

            if (!t->slots[idx].occupied)
            {
                t->slots[idx] = old[i];
                placed = true;
                break;
            }
        }
        if (!placed)
            elog(ERROR, "v2_register_report: dim2 rehash found no free slot in %d",
                 newcap);
        t->st.rehash_entries++;
    }

    pfree(old);
    t->st.bytes_current = newbytes;
    t->st.growths++;
    t->st.max_probe_current = 0;

    INSTR_TIME_SET_CURRENT(ge);
    t->st.rehash_ms += INSTR_TIME_GET_MILLISEC(ge) - INSTR_TIME_GET_MILLISEC(gs);
}

static void
v2_dim2_insert(V2Dim2Table *t, int64 key, int32 payload)
{
    uint32  h = (uint32) key * 2654435761u;

    for (;;)
    {
        int cap = t->st.capacity;

        for (int probe = 0; probe < cap; probe++)
        {
            int idx = (int) ((h + probe) & (cap - 1));

            if (!t->slots[idx].occupied)
            {
                if (t->st.entries >= V2_DIM_GROW_AT(cap))
                    break;
                t->slots[idx].occupied = true;
                t->slots[idx].key = key;
                t->slots[idx].payload = payload;
                t->st.entries++;
                t->st.insertions++;
                v2_dim_note_probe(&t->st, probe + 1);
                return;
            }
            if (t->slots[idx].key == key)
            {
                v2_dim_note_probe(&t->st, probe + 1);
                return;
            }
        }
        v2_dim2_grow(t);
    }
}

static void
v2_dim2_build(V2Dim2Table *t, Oid relid)
{
    int16           attnos[2] = {1, 2};
    XpBatchSource  *src = xpb_heap_source_create_ex(relid, attnos, 2,
                                                    XPB_HEAP_DEFORM, false, 0, 0);
    XpColumnBatch   batch;

    memset(&batch, 0, sizeof(batch));
    batch.capacity = 1024;
    batch.ncols = 2;

    while (src->ops->next_batch(src, &batch))
    {
        int64 *k = xpcb_i64(&batch, 0);     /* account_key is int8 */
        int32 *p = xpcb_i32(&batch, 1);

        for (int r = 0; r < batch.nrows; r++)
        {
            if (xpcb_isnull(&batch, 0, r) || xpcb_isnull(&batch, 1, r))
                continue;
            v2_dim2_insert(t, k[r], p[r]);
        }
        xpcb_release_owned(&batch);
    }
    src->ops->end(src);
}

static inline bool
v2_dim1_lookup(V2Dim1Table *t, int32 key, int32 *payload)
{
    uint32      h = (uint32) key * 2654435761u;
    int         cap = t->st.capacity;
    V2Dim1     *d = t->slots;

    t->st.lookups++;
    for (int i = 0; i < cap; i++)
    {
        int idx = (int) ((h + i) & (cap - 1));

        /* first empty slot ends the chain: there are no deletions */
        if (!d[idx].occupied) { v2_dim_note_probe(&t->st, i + 1); return false; }
        if (d[idx].key == key)
        {
            *payload = d[idx].payload;
            v2_dim_note_probe(&t->st, i + 1);
            return true;
        }
    }
    v2_dim_note_probe(&t->st, cap);
    return false;
}

static inline bool
v2_dim2_lookup(V2Dim2Table *t, int64 key, int32 *payload)
{
    uint32      h = (uint32) key * 2654435761u;
    int         cap = t->st.capacity;
    V2Dim2     *d = t->slots;

    t->st.lookups++;
    for (int i = 0; i < cap; i++)
    {
        int idx = (int) ((h + i) & (cap - 1));

        if (!d[idx].occupied) { v2_dim_note_probe(&t->st, i + 1); return false; }
        if (d[idx].key == key)
        {
            *payload = d[idx].payload;
            v2_dim_note_probe(&t->st, i + 1);
            return true;
        }
    }
    v2_dim_note_probe(&t->st, cap);
    return false;
}

Datum
xpb_v2_register_report(PG_FUNCTION_ARGS)
{
    int32           lo = PG_GETARG_INT32(0);
    int32           hi = PG_GETARG_INT32(1);
    char           *mode = text_to_cstring(PG_GETARG_TEXT_PP(2));
    ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *store;
    MemoryContext   oldcxt;
    Oid             fact_relid,
                    dim1_relid,
                    dim2_relid;
    /*
     * The batch's column ORDER is the same for every layout --
     * [period, company_key, account_key, debit_cents, credit_cents] -- so the
     * pipeline below is identical.  Only which attnums carry them differs.
     */
    int16           attnos_bad[V2_NCOLS] = {1, 3, 4, 8, 9};
    int16           attnos_fixed[V2_NCOLS] = {1, 2, 3, 4, 5};
    int16          *attnos;
    XpBatchSource  *src;
    XpColumnBatch   batch;
    V2Dim1Table     dim1;
    V2Dim2Table     dim2;
    V2GroupTable    grp;
    int32          *grp1_buf,
                   *grp2_buf;
    bool           *keep;
    int64           rows_in = 0;
    int             nbatches = 0;
    bool            is_pgcn = (strcmp(mode, "pgcolumnar") == 0);
    bool            is_heap = false;
    bool            is_card;
    bool            is_card2;
    bool            is_dim;
    const char     *heap_path_used = "n/a";
    /*
     * Benchmark 05-E: observation of the aggregation hash table.  Counted in
     * the probe loop itself, never derived afterwards from the group count.
     * The table is a static open-addressed array with linear probing and no
     * growth, so there is no rehash or capacity event to count -- that
     * absence is itself part of the 05-E result and is reported explicitly
     * rather than left to be inferred.
     */
    instr_time      t0, t1, tp, tn, td0, td1, td2;
    double          dim1_build_ms = 0, dim2_build_ms = 0;
    double          build_ms = 0, open_ms = 0, source_ms = 0,
                    j1_ms = 0, j2_ms = 0, agg_ms = 0;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        (rsi->allowedModes & SFRM_Materialize) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("xpb_v2_register_report: set-valued context required")));

    /*
     * 05-B compares two physical layouts holding identical rows, so the fact
     * table is selectable.  reg2 remains the default, which is what 05-A
     * measured.
     */
    is_card = (strncmp(mode, "card", 4) == 0);
    /*
     * card2 is the wide variant: 192 companies x 768 accounts, the most this
     * report's dimension hashes can hold, used for the high end of the
     * cardinality ladder that reg2_card cannot express.
     */
    is_card2 = (strncmp(mode, "card2", 5) == 0);
    /*
     * Dimension Hash Growth v1 varies dimension cardinality while holding
     * group cardinality at 12 288, so it needs its own fact table and its own
     * pair of dimensions.
     */
    is_dim = (strncmp(mode, "dimgrow", 7) == 0);

    if (is_pgcn)
        fact_relid = RelnameGetRelid("reg2_col");
    else if (strncmp(mode, "bad", 3) == 0)
        fact_relid = RelnameGetRelid("reg2_bad");
    else if (strncmp(mode, "fixedlayout", 11) == 0)
        fact_relid = RelnameGetRelid("reg2_fixed");
    else if (is_dim)
        fact_relid = RelnameGetRelid("reg2_dim");
    else if (is_card2)
        fact_relid = RelnameGetRelid("reg2_card2");
    else if (is_card)
        fact_relid = RelnameGetRelid("reg2_card");
    else
        fact_relid = RelnameGetRelid("reg2");

    attnos = (strncmp(mode, "fixedlayout", 11) == 0 || is_card || is_dim)
             ? attnos_fixed : attnos_bad;

    /*
     * Benchmark 05-E varies group cardinality through its own dimensions, so
     * that the fact table -- and therefore source cost, join shape and both
     * dimension hash occupancies -- stays identical across the whole ladder.
     */
    dim1_relid = RelnameGetRelid(is_dim ? "dim_company_d"
                                 : is_card2 ? "dim_company_c2"
                                 : is_card ? "dim_company_c" : "dim_company");
    dim2_relid = RelnameGetRelid(is_dim ? "dim_account_d"
                                 : is_card2 ? "dim_account_c2"
                                 : is_card ? "dim_account_c" : "dim_account2");
    if (!OidIsValid(fact_relid) || !OidIsValid(dim1_relid) || !OidIsValid(dim2_relid))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("xpb_v2_register_report: fact table or dimensions for mode \"%s\" not found",
                        mode)));

    oldcxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    tupdesc = CreateTemplateTupleDesc(7);
    TupleDescInitEntry(tupdesc, 1, "company_group", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, 2, "account_group", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, 3, "company_key", INT4OID, -1, 0);
    TupleDescInitEntry(tupdesc, 4, "debit_turnover", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 5, "credit_turnover", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 6, "net_turnover", INT8OID, -1, 0);
    TupleDescInitEntry(tupdesc, 7, "total_ms", FLOAT8OID, -1, 0);
    store = tuplestore_begin_heap(true, false, work_mem);
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = store;
    rsi->setDesc = BlessTupleDesc(tupdesc);
    MemoryContextSwitchTo(oldcxt);

    INSTR_TIME_SET_CURRENT(t0);

    INSTR_TIME_SET_CURRENT(tp);
    v2_dim1_init(&dim1);
    v2_dim2_init(&dim2);
    /*
     * Timed separately: the two tables have different capacities and different
     * key widths, so one combined build figure could not be attributed.
     */
    INSTR_TIME_SET_CURRENT(td0);
    v2_dim1_build(&dim1, dim1_relid);
    INSTR_TIME_SET_CURRENT(td1);
    v2_dim2_build(&dim2, dim2_relid);
    INSTR_TIME_SET_CURRENT(td2);
    dim1_build_ms = INSTR_TIME_GET_MILLISEC(td1) - INSTR_TIME_GET_MILLISEC(td0);
    dim2_build_ms = INSTR_TIME_GET_MILLISEC(td2) - INSTR_TIME_GET_MILLISEC(td1);
    v2_grp_init(&grp);
    INSTR_TIME_SET_CURRENT(tn);
    build_ms = INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

    /*
     * Opening the source is timed on its own.  For ZLFS that includes the
     * registry scan, which reads and validates every zone FILE in the data
     * directory -- a cost that grows with zones belonging to other relations,
     * and one that would otherwise sit in total_ms with nothing to attribute
     * it to.  For pgColumnar it includes PgColumnarBeginRead, where zone-map
     * elimination happens.  Naming it keeps total = build + open + source +
     * operators, with nothing unaccounted.
     */
    INSTR_TIME_SET_CURRENT(tp);

    if (strcmp(mode, "zlfs") == 0 || strcmp(mode, "card-zlfs") == 0 ||
        strcmp(mode, "card2-zlfs") == 0)
    {
        ZlfsZone *zone = NULL;

        /*
         * The zone must cover EXACTLY the requested range.  A ZLFS zone is
         * materialized for one predicate range and the source applies no
         * predicate of its own, so taking "any valid zone for this relation"
         * silently answers a different question: with zones for 1..1 and
         * 1..12 both present, a request for 1..12 was served from the 1..1
         * zone and returned 83 334 rows instead of 1 000 008.
         */
        zlfs_ensure_registry();
        zlfs_scan_directory();
        if (zlfs_reg)
            for (int i = 0; i < zlfs_reg->nzones; i++)
                if (zlfs_reg->zones[i]->source_relid == fact_relid &&
                    zlfs_reg->zones[i]->pred_lo == lo &&
                    zlfs_reg->zones[i]->pred_hi == hi &&
                    zlfs_reg->zones[i]->freshness == ZLFS_VALID)
                { zone = zlfs_reg->zones[i]; break; }
        if (zone == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("xpb_v2_register_report: no VALID ZLFS zone for %s covering [%d..%d]",
                            is_card2 ? "reg2_card2" : is_card ? "reg2_card" : "reg2",
                            lo, hi),
                     errhint("Build one with zlfs_build_zone('%s','%s',%d,%d).",
                             is_card2 ? "reg2_card2" : is_card ? "reg2_card" : "reg2",
                             is_card ? "1,2,3,4,5" : "1,3,4,8,9", lo, hi)));
        src = xpb_zlfs_source_create(zone, attnos, V2_NCOLS);
    }
    else if (is_pgcn)
        src = xpcn_source_create(fact_relid, attnos, V2_NCOLS, true, lo, hi);
    else
    {
        /*
         * Three heap modes.
         *
         * The mode is "<table>" or "<table>-<path>":
         *
         *   table    heap        reg2         (05-A's table)
         *            bad         reg2_bad     varlena ahead of the projection
         *            fixedlayout reg2_fixed   projection is a fixed prefix
         *   path     (none)      whatever the layout admits, preferring fixed
         *            -deform     force the generic path even where fixed is
         *                        eligible.  Benchmark-only, and the whole
         *                        point of 05-B: it makes deform and fixed
         *                        comparable on ONE physical table.
         *            -fixed      force the fixed path.  Errors on a layout
         *                        that cannot support it -- never a silent
         *                        downgrade.
         *
         * Nothing here changes production path selection: 'heap' behaves
         * exactly as before, and the two forcing modes exist only for this
         * measurement.
         */
        char       *why = NULL;
        Relation    rel = table_open(fact_relid, AccessShareLock);
        bool        can_fixed = xpb_heap_layout_supports_fixed(rel, attnos,
                                                               V2_NCOLS, &why);
        XpbHeapPath want;

        table_close(rel, AccessShareLock);

        {
            const char *dash = strrchr(mode, '-');

            if (dash == NULL)
                want = can_fixed ? XPB_HEAP_FIXED : XPB_HEAP_DEFORM;
            else if (strcmp(dash, "-deform") == 0)
                want = XPB_HEAP_DEFORM;
            else if (strcmp(dash, "-fixed") == 0)
                want = XPB_HEAP_FIXED;
            else if (strcmp(dash, "-projected") == 0)
                want = XPB_HEAP_PROJECTED;
            else if (strcmp(dash, "-early") == 0)
                want = XPB_HEAP_PROJECTED_EARLY;
            else
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("xpb_v2_register_report: unknown source mode \"%s\"",
                                mode),
                         errhint("<table>[-deform|-fixed|-projected|-early], where table is heap, bad, fixedlayout, card, card2 or dimgrow; or pgcolumnar, zlfs, card-zlfs or card2-zlfs.")));
        }

        heap_path_used = (want == XPB_HEAP_FIXED) ? "fixed"
                       : (want == XPB_HEAP_PROJECTED) ? "projected"
                       : (want == XPB_HEAP_PROJECTED_EARLY) ? "projected-early"
                       : "deform";
        src = xpb_heap_source_create_ex(fact_relid, attnos, V2_NCOLS, want,
                                        true, lo, hi);
        is_heap = true;
    }

    INSTR_TIME_SET_CURRENT(tn);
    open_ms = INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

    memset(&batch, 0, sizeof(batch));
    batch.capacity = XPCB_BATCH_CAP;
    batch.ncols = V2_NCOLS;

    /* Per-batch scratch, allocated once: at XPCB_BATCH_CAP these are about
     * 590 KB together, which does not belong on the stack. */
    grp1_buf = palloc(XPCB_BATCH_CAP * sizeof(int32));
    grp2_buf = palloc(XPCB_BATCH_CAP * sizeof(int32));
    keep = palloc(XPCB_BATCH_CAP * sizeof(bool));

    for (;;)
    {
        int32  *col_co;
        int64  *col_ac, *col_dt, *col_kt;
        int     nrows;

        INSTR_TIME_SET_CURRENT(tp);
        if (!src->ops->next_batch(src, &batch))
        {
            INSTR_TIME_SET_CURRENT(tn);
            source_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);
            break;
        }
        INSTR_TIME_SET_CURRENT(tn);
        source_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

        nbatches++;
        nrows = batch.nrows;
        rows_in += nrows;

        /* dispatch once per batch, never inside a row loop */
        col_co = xpcb_i32(&batch, V2_C_COMPANY);
        col_ac = xpcb_i64(&batch, V2_C_ACCOUNT);
        col_dt = xpcb_i64(&batch, V2_C_DEBIT);
        col_kt = xpcb_i64(&batch, V2_C_CREDIT);

        /* join 1: company_key -> company_group */
        INSTR_TIME_SET_CURRENT(tp);
        for (int r = 0; r < nrows; r++)
            keep[r] = !xpcb_isnull(&batch, V2_C_COMPANY, r) &&
                      v2_dim1_lookup(&dim1, col_co[r], &grp1_buf[r]);
        INSTR_TIME_SET_CURRENT(tn);
        j1_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

        /* join 2: account_key -> account_group */
        INSTR_TIME_SET_CURRENT(tp);
        for (int r = 0; r < nrows; r++)
            if (keep[r])
                keep[r] = !xpcb_isnull(&batch, V2_C_ACCOUNT, r) &&
                          v2_dim2_lookup(&dim2, col_ac[r], &grp2_buf[r]);
        INSTR_TIME_SET_CURRENT(tn);
        j2_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

        /* aggregate */
        INSTR_TIME_SET_CURRENT(tp);
        for (int r = 0; r < nrows; r++)
        {
            int32   g1, g2, ck;
            int64   dt, kt;

            if (!keep[r])
                continue;
            g1 = grp1_buf[r];
            g2 = grp2_buf[r];
            ck = col_co[r];
            dt = col_dt[r];
            kt = col_kt[r];

            v2_grp_upsert(&grp, g1, g2, ck, dt, kt);
        }
        INSTR_TIME_SET_CURRENT(tn);
        agg_ms += INSTR_TIME_GET_MILLISEC(tn) - INSTR_TIME_GET_MILLISEC(tp);

        xpcb_release_owned(&batch);
        CHECK_FOR_INTERRUPTS();
    }

    INSTR_TIME_SET_CURRENT(t1);

    {
        double  total_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);
        StringInfoData  extra;

        initStringInfo(&extra);
        if (is_pgcn)
        {
            int64 gr, grs, vskip, remit;

            xpcn_source_pruning(src, &gr, &grs, &vskip, &remit);
            appendStringInfo(&extra,
                             "  rowgroups_read=" INT64_FORMAT
                             " rows_in_read_groups=" INT64_FORMAT
                             " rows_vec_skipped=" INT64_FORMAT
                             " rows_emitted=" INT64_FORMAT,
                             gr, grs, vskip, remit);
        }
        else if (is_heap)
        {
            int64   td_, ad_;
            int     ar_;
            bool    isdef;
            int64   ts_, aw_, am_;
            int64   ta_, tr_, wa_, wr_, ma_, mr_;

            xpb_heap_source_deform_stats(src, &td_, &ad_, &ar_, &isdef);
            xpb_heap_source_projected_stats(src, &ts_, &aw_, &am_);
            xpb_heap_source_early_stats(src, &ta_, &tr_, &wa_, &wr_, &ma_, &mr_);
            appendStringInfo(&extra,
                             "  heap_path=%s  tuples_deformed=" INT64_FORMAT
                             " attrs_deformed=" INT64_FORMAT
                             " tuples_scanned=" INT64_FORMAT
                             " attrs_walked=" INT64_FORMAT
                             " attrs_materialized=" INT64_FORMAT
                             " tuples_accepted=" INT64_FORMAT
                             " tuples_rejected_early=" INT64_FORMAT
                             " walked_acc=" INT64_FORMAT " walked_rej=" INT64_FORMAT
                             " mat_acc=" INT64_FORMAT " mat_rej=" INT64_FORMAT
                             " attrs_requested=%d",
                             heap_path_used, td_, ad_, ts_, aw_, am_,
                             ta_, tr_, wa_, wr_, ma_, mr_, ar_);
        }

        /*
         * Aggregation hash table.  grp_bytes is the live allocation, which is
         * sizeof(V2Group) * the CURRENT capacity; grp_bytes_peak additionally
         * covers the moment during a rehash when the old and new tables are
         * both live.  Reported as allocations rather than divided by the group
         * count to manufacture a per-group figure.
         *
         * This block described a fixed-capacity table until Hash Aggregate
         * Growth v1 and said growths and rehashes were constants.  They are
         * not; the table doubles at half full.
         */
        appendStringInfo(&extra,
                         "  grp_initial_cap=%d grp_cap=%d grp_grow_at=%d"
                         " grp_occupied=%d grp_load_factor=%.4f"
                         " grp_bytes=%zu grp_bytes_peak=%zu grp_entry_bytes=%zu"
                         " grp_inserts=" INT64_FORMAT " grp_hits=" INT64_FORMAT
                         " grp_lookups=" INT64_FORMAT
                         " grp_probes=" INT64_FORMAT " grp_probes_per_lookup=%.4f"
                         " grp_max_probe_current=%d grp_max_probe_lifetime=%d"
                         " grp_growths=%d grp_rehashes=%d"
                         " grp_rehash_groups=" INT64_FORMAT
                         " grp_rehash_probes=" INT64_FORMAT
                         " grp_rehash_ms=%.3f grp_agg_minus_rehash_ms=%.3f"
                         " grp_cxt_bytes=%zu",
                         grp.initial_capacity, grp.capacity,
                         V2_GRP_GROW_AT(grp.capacity),
                         grp.ngroups, (double) grp.ngroups / grp.capacity,
                         grp.bytes_current, grp.bytes_peak, sizeof(V2Group),
                         grp.inserts, grp.hits, grp.inserts + grp.hits,
                         grp.probes,
                         (grp.inserts + grp.hits) > 0
                             ? (double) grp.probes / (double) (grp.inserts + grp.hits)
                             : 0.0,
                         grp.max_probe_current, grp.max_probe_lifetime,
                         /*
                          * One rehash per growth by construction, reported
                          * separately anyway so the two stay distinguishable
                          * if a later policy ever rehashes without growing.
                          */
                         grp.growths, grp.growths,
                         grp.rehash_groups, grp.rehash_probes,
                         grp.rehash_ms,
                         /*
                          * Not a fake subtraction: rehash runs inside the
                          * aggregate phase and is timed with the same clock,
                          * so the difference is exact by construction
                          * (section 10).
                          */
                         agg_ms - grp.rehash_ms,
                         /*
                          * Measured by the memory system, not by this file's
                          * own arithmetic: if a growth ever failed to release
                          * its predecessor, this would exceed grp_bytes.
                          */
                         MemoryContextMemAllocated(grp.cxt, false));

        /*
         * Dimension hash tables, reported per table and never pooled: the two
         * have different key widths and different capacities, so one combined
         * figure could not be attributed to either.
         */
        v2_dim_report(&extra, "dim1", &dim1.st, dim1.cxt);
        v2_dim_report(&extra, "dim2", &dim2.st, dim2.cxt);
        appendStringInfo(&extra, "  dim1_build_ms=%.3f dim2_build_ms=%.3f",
                         dim1_build_ms, dim2_build_ms);

        elog(NOTICE,
             "v2_register_report [%d..%d] mode=%s: total=%.1f ms  build=%.1f ms  "
             "open=%.1f ms  source=%.1f ms  join1=%.1f ms  join2=%.1f ms  agg=%.1f ms  "
             "operators=%.1f ms  rows=" INT64_FORMAT "  batches=%d  groups=%d%s",
             lo, hi, mode, total_ms, build_ms, open_ms, source_ms, j1_ms, j2_ms, agg_ms,
             j1_ms + j2_ms + agg_ms, rows_in, nbatches, grp.ngroups, extra.data);

        for (int i = 0; i < grp.capacity; i++)
        {
            Datum   vals[7];
            bool    nulls[7] = {false, false, false, false, false, false, false};

            if (!grp.slots[i].occupied)
                continue;
            vals[0] = Int32GetDatum(grp.slots[i].company_group);
            vals[1] = Int32GetDatum(grp.slots[i].account_group);
            vals[2] = Int32GetDatum(grp.slots[i].company_key);
            vals[3] = Int64GetDatum(grp.slots[i].debit);
            vals[4] = Int64GetDatum(grp.slots[i].credit);
            /*
             * net is derived at emit time from the two accumulated sums
             * rather than by a second pass over the data.  Checked for the
             * same reason they are: debit and credit can each be in range
             * while their difference is not.
             */
            {
                int64   net;

                if (pg_sub_s64_overflow(grp.slots[i].debit,
                                        grp.slots[i].credit, &net))
                    ereport(ERROR,
                            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                             errmsg("bigint out of range"),
                             errdetail("Group (%d, %d, %d) overflowed sum(debit) - sum(credit).",
                                       grp.slots[i].company_group,
                                       grp.slots[i].account_group,
                                       grp.slots[i].company_key)));
                vals[5] = Int64GetDatum(net);
            }
            vals[6] = Float8GetDatum(total_ms);
            tuplestore_putvalues(store, rsi->setDesc, vals, nulls);
        }
    }

    /*
     * The group table is released explicitly once its rows have been copied
     * into the tuplestore, rather than left for the surrounding context to
     * reset.  Growth allocates a fresh table each time and pfrees the old one
     * immediately, so this should be reclaiming exactly one live table; the
     * grp_cxt_bytes counter reported above is the independent check on that
     * (section 8).
     */
    MemoryContextDelete(grp.cxt);
    MemoryContextDelete(dim1.cxt);
    MemoryContextDelete(dim2.cxt);

    src->ops->end(src);
    return (Datum) 0;
}

/*
 * xpb_grp_test_policy(initial_capacity int, max_capacity int) -> text
 *
 * Test-only.  Forces a small initial capacity so that several growths and the
 * exact growth threshold can be exercised without a million-group dataset,
 * and an artificial ceiling so that a refused growth can be tested without
 * inducing a real OOM.  Passing 0 restores the production policy.
 *
 * The cardinality ladder never calls this, and run-hash-growth.sh prints
 * grp_initial_cap before measuring so that a stray hook is visible in the
 * output -- a check a reader can make, not one the runner enforces.  One
 * benchmark does call it deliberately: run-rehash-control.sh pre-sizes the
 * table to the final capacity so that the cost of doubling can be measured
 * against an otherwise identical table.
 */
PG_FUNCTION_INFO_V1(xpb_grp_test_policy);

Datum
xpb_grp_test_policy(PG_FUNCTION_ARGS)
{
    int     init = PG_ARGISNULL(0) ? 0 : PG_GETARG_INT32(0);
    int     maxc = PG_ARGISNULL(1) ? 0 : PG_GETARG_INT32(1);

    /*
     * The slot index is computed with a mask, so a capacity that is not a
     * power of two would silently address only part of the table.
     */
    if (init < 0 || (init > 0 && (init < 2 || (init & (init - 1)) != 0)))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_grp_test_policy: initial capacity must be 0 or a power of two >= 2, got %d",
                        init)));
    if (maxc < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_grp_test_policy: max capacity must be >= 0, got %d",
                        maxc)));

    v2_grp_test_init_cap = init;
    v2_grp_test_max_cap = maxc;

    PG_RETURN_TEXT_P(cstring_to_text(psprintf(
        "initial_capacity=%d max_capacity=%d",
        init > 0 ? init : V2_GRP_CAP,
        maxc)));
}

/*
 * xpb_dim_test_policy(dim1_initial int, dim2_initial int) -> text
 *
 * Test-only, and diagnostic only.  Pre-sizes the dimension tables so the
 * paired control can measure what doubling cost against an otherwise identical
 * table.  0 restores the production policy.  Process-local, so it must be set
 * in the same connection as the report it governs.
 */
PG_FUNCTION_INFO_V1(xpb_dim_test_policy);

Datum
xpb_dim_test_policy(PG_FUNCTION_ARGS)
{
    int     d1 = PG_ARGISNULL(0) ? 0 : PG_GETARG_INT32(0);
    int     d2 = PG_ARGISNULL(1) ? 0 : PG_GETARG_INT32(1);

    /* the slot index is a mask, so a non-power-of-two would address part of it */
    if (d1 < 0 || (d1 > 0 && (d1 < 2 || (d1 & (d1 - 1)) != 0)) ||
        d2 < 0 || (d2 > 0 && (d2 < 2 || (d2 & (d2 - 1)) != 0)))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_dim_test_policy: capacities must be 0 or powers of two >= 2, got %d and %d",
                        d1, d2)));
    v2_dim1_test_init_cap = d1;
    v2_dim2_test_init_cap = d2;
    PG_RETURN_TEXT_P(cstring_to_text(psprintf("dim1=%d dim2=%d",
                                              d1 > 0 ? d1 : V2_DIM1_CAP,
                                              d2 > 0 ? d2 : V2_DIM2_CAP)));
}
