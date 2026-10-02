/*
 * xpb_source_registry.c — name -> source-provider registry
 *
 * See xpb_source.h for why this exists. The whole point is that xp_batch.so
 * does not reference any optional provider, so nothing here mentions Parquet,
 * Arrow, or any specific source.
 */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "xpb_source.h"

/*
 * Append-only for the life of the backend. Providers register from _PG_init(),
 * which runs once per backend per module, so the table is written during
 * startup and read-only afterwards. No locking: a backend only ever sees its
 * own copy.
 *
 * Descriptors are COPIED, by value, and that is load-bearing in two ways.
 *
 * The copy is zero-filled first and only struct_size bytes are taken from the
 * caller, so a provider built against an earlier minor version of this ABI --
 * one that predates an appended field -- reads as zero in every field it does
 * not have, instead of this file reading past the end of the provider's object.
 *
 * And the stored descriptor no longer depends on the provider's static object
 * staying put. It never moved in practice, because PostgreSQL does not unload a
 * module, but a table of pointers made the layout of someone else's struct this
 * file's problem at every read rather than once at registration.
 */
static XpbSourceProvider    xpb_providers[XPB_SOURCE_MAX_PROVIDERS];
static int                  xpb_nproviders = 0;

void
xpb_register_source_provider(const XpbSourceProvider *p)
{
    XpbSourceProvider   local;

    if (p == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xp_batch: source provider descriptor is NULL")));

    /*
     * Version and size BEFORE anything else, because every other field's offset
     * is only meaningful once these two agree. A descriptor from a module built
     * against a different revision of xpb_source.h has its fields somewhere
     * else, and reading its name or create pointer first is reading whatever
     * happens to sit at those offsets.
     */
    if (p->abi_version != XPB_SOURCE_ABI_VERSION)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xp_batch: source provider has ABI version 0x%08X, this build expects 0x%08X",
                        p->abi_version, XPB_SOURCE_ABI_VERSION),
                 errdetail("The provider module was built against a different "
                           "xpb_source.h than the loaded xp_batch."),
                 errhint("Rebuild and reinstall both modules from the same tree.")));

    if (p->struct_size < XPB_SOURCE_PROVIDER_MIN_SIZE ||
        p->struct_size > sizeof(XpbSourceProvider))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xp_batch: source provider declares struct_size %u, outside the accepted range %zu..%zu",
                        p->struct_size,
                        (size_t) XPB_SOURCE_PROVIDER_MIN_SIZE,
                        sizeof(XpbSourceProvider)),
                 errdetail("A larger descriptor than this build knows about "
                           "carries fields whose meaning is unknown here, which "
                           "is refused rather than ignored."),
                 errhint("Rebuild and reinstall both modules from the same tree.")));

    memset(&local, 0, sizeof(local));
    memcpy(&local, p, p->struct_size);

    if (local.name == NULL || local.create == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xp_batch: source provider must have a name and a create function")));

    if (xpb_find_source_provider(local.name) != NULL)
        ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_OBJECT),
                 errmsg("xp_batch: source provider \"%s\" is already registered",
                        local.name),
                 errdetail("Two modules claiming one provider name would make "
                           "the winner depend on load order.")));

    if (xpb_nproviders >= XPB_SOURCE_MAX_PROVIDERS)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("xp_batch: too many source providers (%d)",
                        XPB_SOURCE_MAX_PROVIDERS)));

    xpb_providers[xpb_nproviders++] = local;

    elog(DEBUG1, "xp_batch: registered source provider \"%s\" (struct_size %u, filters_rows %d)",
         local.name, local.struct_size, (int) local.filters_rows);
}

const XpbSourceProvider *
xpb_find_source_provider(const char *name)
{
    if (name == NULL)
        return NULL;

    for (int i = 0; i < xpb_nproviders; i++)
        if (strcmp(xpb_providers[i].name, name) == 0)
            return &xpb_providers[i];

    return NULL;
}

int
xpb_source_provider_count(void)
{
    return xpb_nproviders;
}

const char *
xpb_source_provider_name(int i)
{
    if (i < 0 || i >= xpb_nproviders)
        return NULL;
    return xpb_providers[i].name;
}

/*
 * SQL view of the registry, so "is the Parquet module loaded in this backend?"
 * is answerable without reading a log. Returns zero rows when nothing optional
 * is loaded, which is the normal state.
 */
PG_FUNCTION_INFO_V1(xpb_source_providers);

Datum
xpb_source_providers(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *store;
    MemoryContext   oldcxt;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("xpb_source_providers: set-valued context required")));

    oldcxt = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

    tupdesc = CreateTemplateTupleDesc(2);
    TupleDescInitEntry(tupdesc, 1, "name", TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, 2, "describes", BOOLOID, -1, 0);
    tupdesc = BlessTupleDesc(tupdesc);

    store = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult  = store;
    rsinfo->setDesc    = tupdesc;

    for (int i = 0; i < xpb_nproviders; i++)
    {
        Datum   vals[2];
        bool    nulls[2] = {false, false};

        vals[0] = CStringGetTextDatum(xpb_providers[i].name);
        vals[1] = BoolGetDatum(xpb_providers[i].describe != NULL);
        tuplestore_putvalues(store, tupdesc, vals, nulls);
    }

    MemoryContextSwitchTo(oldcxt);
    return (Datum) 0;
}
