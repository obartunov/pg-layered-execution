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

/*
 * ABI version of XpbSourceProvider.
 *
 * This descriptor crosses a module boundary: xp_batch.so defines the struct, a
 * provider .so fills one in, and NOTHING IN THE BUILD SYSTEM REBUILDS THEM
 * TOGETHER. The first version of this header had no version field and put a new
 * bool in the middle of the struct, which is a silent wrong answer rather than a
 * failed load: a provider built before that change presents its `describe`
 * pointer where `filters_rows` now sits, the low bytes of a pointer are not
 * zero, so the field reads true, so the caller believes the provider applied the
 * predicate and skips applying it. Demonstrated in a live backend --
 * test/provider_abi.sh, case `legacy`, which reported filters_rows=true from a
 * stale descriptor.
 *
 * The value is deliberately not a small integer. It sits at offset 0, which in
 * every earlier layout held `name` -- a pointer -- so a distinctive constant is
 * what makes "this is a stale descriptor" detectable instead of plausible. Bump
 * it only for a change that is NOT a field appended at the end.
 */
#define XPB_SOURCE_ABI_VERSION      0x58500001u     /* 'X','P' + revision 1 */

typedef struct XpbSourceProvider
{
    /*
     * Both set by XPB_SOURCE_PROVIDER_HEADER. struct_size is sizeof() AS THE
     * PROVIDER SAW IT, which is how a provider that predates an appended field
     * stays usable: the registry copies that many bytes into a zero-filled
     * descriptor, so every field the provider does not have reads as zero.
     */
    uint32          abi_version;
    uint32          struct_size;

    const char     *name;
    XpBatchSource  *(*create)(const XpbSourceRequest *req);

    /*
     * Optional. Lets a caller ask what the provider will produce before
     * constructing it -- which is what projection pushdown needs in order to
     * type the batch. A provider that cannot answer leaves this NULL and the
     * caller must construct first.
     */
    bool            (*describe)(const XpbSourceRequest *req,
                                XpbColType *types, int *ncols_out);

    /* ─────────── APPEND ONLY BELOW THIS LINE ───────────
     * A field added here is invisible to an older provider, which simply
     * reports a smaller struct_size and gets zero. A field inserted ABOVE this
     * line changes the meaning of every older provider's bytes and requires a
     * version bump. Keep XPB_SOURCE_PROVIDER_MIN_SIZE pointing at the first
     * appended field. */

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
     * false means the CALLER must apply the predicate. That direction is the
     * safe one: a redundant filter over already-filtered rows costs a pass and
     * changes no answer. It is now actually the default in both ways a provider
     * can fail to set it -- a zero initialiser, and a descriptor that predates
     * the field -- which was the point of the two header fields above.
     */
    bool            filters_rows;
} XpbSourceProvider;

/*
 * The smallest descriptor a provider of this ABI version may present: every
 * field up to the first appended one. Offsets below it are fixed for the life of
 * the version.
 */
#define XPB_SOURCE_PROVIDER_MIN_SIZE    offsetof(XpbSourceProvider, filters_rows)

/*
 * Every provider's descriptor starts with this. Spelled as a macro so a
 * provider cannot fill in the version by hand and get it wrong, and so
 * struct_size is sizeof() at the PROVIDER's compile time rather than a literal.
 */
#define XPB_SOURCE_PROVIDER_HEADER                      \
    .abi_version = XPB_SOURCE_ABI_VERSION,              \
    .struct_size = sizeof(XpbSourceProvider)

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

/*
 * heap, zlfs and pgcolumnar, registered from xp_batch's own _PG_init. They are
 * linked into xp_batch.so -- registering them makes the LOOKUP uniform, not the
 * dependency optional. See xpb_builtin_providers.c.
 */
extern void xpb_register_builtin_providers(void);

#endif  /* XPB_SOURCE_H */
