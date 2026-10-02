/*
 * xpb_builtin_providers.c -- heap, ZLFS and pgcolumnar as registry providers.
 *
 * These three were constructed by name from the operators, each behind its own
 * strcmp branch, while Parquet went through the registry. The question the
 * audit had to answer was whether that was legacy wiring or a different
 * interface. It is legacy wiring: all three already produce XpColumnBatch
 * through XpBatchSourceOps, and two of them already take exactly the fields
 * XpbSourceRequest carries. Nothing here changes a source's behaviour; each
 * adapter is a call with the arguments rearranged.
 *
 * They stay LINKED INTO xp_batch.so and registered from its own _PG_init.
 * That is the difference between built-in and external, and it is deliberate:
 * heap has no optional dependency and the cluster must start without any
 * provider module present. Registering them does not make them optional; it
 * makes the LOOKUP uniform, so an operator asks the registry for a source
 * instead of knowing which sources exist.
 *
 * ZLFS is the one adapter that does more than rearrange arguments: its
 * constructor takes an already-looked-up ZlfsZone, so the adapter does the
 * lookup -- the same three lines every caller used to write -- from the
 * request's relid and predicate bounds.
 */
#include "postgres.h"
#include "fmgr.h"

#include "xpb_colbatch.h"
#include "xpb_source.h"
#include "xpb_zlfs.h"

extern XpBatchSource *xpb_zlfs_source_create(ZlfsZone *zone, int16 *requested_attnos, int ncols);
extern XpBatchSource *xpb_heap_source_create(Oid relid, int16 *requested_attnos, int ncols,
                                             bool has_pred, int32 pred_lo, int32 pred_hi);
extern XpBatchSource *xpcn_source_create(Oid relid, int16 *requested_attnos, int ncols,
                                         bool has_pred, int32 pred_lo, int32 pred_hi);

/*
 * The request carries int64 bounds because an external source may key on one;
 * these three take int32. A bound outside int32 is refused rather than
 * truncated -- a silently narrowed predicate reads rows it was asked to skip.
 */
static void
req_pred_int32(const XpbSourceRequest *req, int32 *lo, int32 *hi)
{
    if (!req->has_pred)
    {
        *lo = PG_INT32_MIN;
        *hi = PG_INT32_MAX;
        return;
    }
    if (req->pred_lo < PG_INT32_MIN || req->pred_lo > PG_INT32_MAX ||
        req->pred_hi < PG_INT32_MIN || req->pred_hi > PG_INT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("xp_batch: predicate bound outside int32 for a source that keys on int4")));
    *lo = (int32) req->pred_lo;
    *hi = (int32) req->pred_hi;
}

static void
req_require_attnos(const XpbSourceRequest *req, const char *what)
{
    if (req->attnos == NULL || req->ncols <= 0 || req->ncols > XPCB_MAX_COLS)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xp_batch: %s source needs 1..%d attnos", what, XPCB_MAX_COLS)));
    if (!OidIsValid(req->relid))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xp_batch: %s source needs a relation", what)));
}

/* attnos are const in the request and non-const in the constructors, which
 * predate it; copy rather than cast away const. */
static void
req_copy_attnos(const XpbSourceRequest *req, int16 *out)
{
    for (int i = 0; i < req->ncols; i++)
        out[i] = req->attnos[i];
}

static XpBatchSource *
heap_provider_create(const XpbSourceRequest *req)
{
    int16   attnos[XPCB_MAX_COLS];
    int32   lo, hi;

    req_require_attnos(req, "heap");
    req_copy_attnos(req, attnos);
    req_pred_int32(req, &lo, &hi);

    return xpb_heap_source_create(req->relid, attnos, req->ncols,
                                  req->has_pred, lo, hi);
}

static XpBatchSource *
zlfs_provider_create(const XpbSourceRequest *req)
{
    int16       attnos[XPCB_MAX_COLS];
    int32       lo, hi;
    ZlfsZone   *zone;

    req_require_attnos(req, "zlfs");
    req_copy_attnos(req, attnos);
    req_pred_int32(req, &lo, &hi);

    if (!req->has_pred)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED)),
                errmsg("xp_batch: the zlfs source needs a range, since a zone is built for one"));

    zlfs_ensure_registry();
    zlfs_scan_directory();
    zone = zlfs_lookup_valid_zone(req->relid, lo, hi);
    if (zone == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("xp_batch: no valid ZLFS zone for [%d..%d]", lo, hi),
                 errhint("Build one with zlfs_build_zone().")));

    return xpb_zlfs_source_create(zone, attnos, req->ncols);
}

static XpBatchSource *
pgcolumnar_provider_create(const XpbSourceRequest *req)
{
    int16   attnos[XPCB_MAX_COLS];
    int32   lo, hi;

    req_require_attnos(req, "pgcolumnar");
    req_copy_attnos(req, attnos);
    req_pred_int32(req, &lo, &hi);

    return xpcn_source_create(req->relid, attnos, req->ncols,
                              req->has_pred, lo, hi);
}

/*
 * filters_rows is stated for each, from what the source does with the range:
 * the heap source drops non-matching rows per tuple (xpb_src_heap.c), the
 * pgcolumnar source does the same per row group and per row, and a ZLFS zone
 * IS the range -- it was built for exactly those bounds and holds nothing else,
 * which is a filter already applied rather than one skipped.
 */
static const XpbSourceProvider heap_provider = {
    XPB_SOURCE_PROVIDER_HEADER,
    .name = "heap", .create = heap_provider_create, .filters_rows = true,
};

static const XpbSourceProvider zlfs_provider = {
    XPB_SOURCE_PROVIDER_HEADER,
    .name = "zlfs", .create = zlfs_provider_create, .filters_rows = true,
};

static const XpbSourceProvider pgcolumnar_provider = {
    XPB_SOURCE_PROVIDER_HEADER,
    .name = "pgcolumnar", .create = pgcolumnar_provider_create, .filters_rows = true,
};

void
xpb_register_builtin_providers(void)
{
    xpb_register_source_provider(&heap_provider);
    xpb_register_source_provider(&zlfs_provider);
    xpb_register_source_provider(&pgcolumnar_provider);
}
