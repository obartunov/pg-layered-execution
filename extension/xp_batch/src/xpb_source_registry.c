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
 * The provider descriptors themselves are NOT copied. A provider passes a
 * pointer to its own static descriptor, which lives as long as its module is
 * loaded -- and PostgreSQL never unloads a module once loaded.
 */
static const XpbSourceProvider *xpb_providers[XPB_SOURCE_MAX_PROVIDERS];
static int                      xpb_nproviders = 0;

void
xpb_register_source_provider(const XpbSourceProvider *p)
{
    if (p == NULL || p->name == NULL || p->create == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xp_batch: source provider must have a name and a create function")));

    if (xpb_find_source_provider(p->name) != NULL)
        ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_OBJECT),
                 errmsg("xp_batch: source provider \"%s\" is already registered",
                        p->name),
                 errdetail("Two modules claiming one provider name would make "
                           "the winner depend on load order.")));

    if (xpb_nproviders >= XPB_SOURCE_MAX_PROVIDERS)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("xp_batch: too many source providers (%d)",
                        XPB_SOURCE_MAX_PROVIDERS)));

    xpb_providers[xpb_nproviders++] = p;

    elog(DEBUG1, "xp_batch: registered source provider \"%s\"", p->name);
}

const XpbSourceProvider *
xpb_find_source_provider(const char *name)
{
    if (name == NULL)
        return NULL;

    for (int i = 0; i < xpb_nproviders; i++)
        if (strcmp(xpb_providers[i]->name, name) == 0)
            return xpb_providers[i];

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
    return xpb_providers[i]->name;
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

        vals[0] = CStringGetTextDatum(xpb_providers[i]->name);
        vals[1] = BoolGetDatum(xpb_providers[i]->describe != NULL);
        tuplestore_putvalues(store, tupdesc, vals, nulls);
    }

    MemoryContextSwitchTo(oldcxt);
    return (Datum) 0;
}
