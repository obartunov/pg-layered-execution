/*
 * xpb_source.h — source provider registration
 *
 * The batch contract in xpb_colbatch.h says what a source must PRODUCE. It
 * says nothing about how a source comes into existence, and until now that was
 * hardcoded: every call site named a constructor directly
 * (xpb_heap_source_create_ex, xpcn_source_create, xpb_zlfs_source_create), so
 * every source had to be linked into xp_batch.so.
 *
 * That is a link-time dependency, not an architectural one. A provider whose
 * backing library is optional -- Parquet needs Arrow -- cannot live inside a
 * module that sits in shared_preload_libraries, because a missing .so would
 * stop the cluster from starting rather than disable one feature.
 *
 * This header adds the smallest thing that removes the link-time coupling: a
 * name -> constructor registry. A provider in a SEPARATE PostgreSQL shared
 * module calls xpb_register_source_provider() from its own _PG_init(), and
 * resolves this symbol against the already-loaded xp_batch.so because
 * PostgreSQL's dfmgr.c loads modules with RTLD_NOW | RTLD_GLOBAL. No dlopen by
 * hand, no hard-coded paths, no link-time reference in either direction.
 *
 * WHAT THIS IS NOT
 * ----------------
 * Not a plugin framework. There is no versioning, no unregister, no
 * per-provider GUC namespace, no cost model. Registration is append-only for
 * the life of the backend, which is what a provider loaded in _PG_init needs
 * and no more.
 *
 * Nothing in the generic operators consults this. Operators see
 * XpBatchSource * and nothing else, exactly as before; the registry is used
 * only where a source is CONSTRUCTED.
 */
#ifndef XPB_SOURCE_H
#define XPB_SOURCE_H

#include "postgres.h"
#include "xpb_colbatch.h"

#define XPB_SOURCE_MAX_PROVIDERS    8

/*
 * What a caller asks for. The fields are the union of what the existing
 * constructors already take, generalised in two places only:
 *
 *   uri        an object that is not a relation (a Parquet file path). The
 *              existing providers leave it NULL.
 *   colnames   column identity for a source that is keyed by name rather than
 *              by attno. Parquet columns have names, not attnos; heap and
 *              pgcolumnar leave it NULL and use attnos.
 *
 * The predicate stays exactly as narrow as it is today -- one closed range on
 * the leading projected column -- because widening it is a separate decision
 * with its own evidence, and inventing a general predicate object here would
 * be designing for a user that does not exist yet.
 */
typedef struct XpbSourceRequest
{
    Oid                 relid;      /* relation to read, or InvalidOid       */
    const char         *uri;        /* external object, or NULL              */

    const int16        *attnos;     /* 1-based attnos (relation sources)     */
    const char *const  *colnames;   /* column names (name-keyed sources)     */
    int                 ncols;

    bool                has_pred;   /* one range on the leading column       */
    int64               pred_lo;
    int64               pred_hi;
} XpbSourceRequest;

typedef struct XpbSourceProvider
{
    const char     *name;
    XpBatchSource  *(*create)(const XpbSourceRequest *req);

    /*
     * Does this provider APPLY req->has_pred as a row filter, or only use it
     * to skip work?
     *
     * This is not an optimisation hint. It decides who is responsible for the
     * predicate, and getting it wrong is a wrong answer, not a slow one: the
     * heap and zlfs sources drop non-matching rows themselves
     * (xpb_src_heap.c:329), while the Parquet provider uses the range only to
     * exclude whole row groups and hands back every row of the row groups it
     * reads. A caller that assumed the first behaviour and got the second
     * would return rows outside the predicate.
     *
     * false (the zero value, so the default for a provider that does not think
     * about this) means the CALLER must apply the predicate. That direction is
     * the safe one: a redundant filter over already-filtered rows costs a pass
     * and changes no answer, whereas the opposite default would turn a
     * forgotten field into silently wrong output.
     */
    bool            filters_rows;

    /*
     * Optional. Lets a caller ask what the provider will produce before
     * constructing it -- which is what projection pushdown needs in order to
     * type the batch. A provider that cannot answer leaves this NULL and the
     * caller must construct first.
     */
    bool            (*describe)(const XpbSourceRequest *req,
                                XpbColType *types, int *ncols_out);
} XpbSourceProvider;

/*
 * PGDLLEXPORT on all four is load-bearing, not decoration.
 *
 * PostgreSQL builds extension modules with -fvisibility=hidden, so a symbol
 * without it is GLOBAL HIDDEN in the object and LOCAL in the linked .so --
 * present, and invisible to any other module. Without these markers the first
 * LOAD of a provider module fails with
 *
 *   could not load library ".../xpb_parquet.so":
 *   undefined symbol: xpb_register_source_provider
 *
 * which is how this was found. The same mechanism is why this repository
 * carries patches/pgcolumnar/0001-export-fold-reader-api.patch.
 *
 * Called from a provider module's _PG_init(). Duplicate names are rejected
 * rather than silently replaced: two modules claiming one name is a packaging
 * error, and the quiet version of it would make which provider ran depend on
 * load order.
 */
PGDLLEXPORT extern void xpb_register_source_provider(const XpbSourceProvider *p);

/* NULL when no provider of that name is loaded. Never raises. */
PGDLLEXPORT extern const XpbSourceProvider *xpb_find_source_provider(const char *name);

/* For observability: how many are registered, and their names. */
PGDLLEXPORT extern int  xpb_source_provider_count(void);
PGDLLEXPORT extern const char *xpb_source_provider_name(int i);

#endif  /* XPB_SOURCE_H */
