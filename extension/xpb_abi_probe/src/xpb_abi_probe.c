/*
 * xpb_abi_probe.c — registers deliberately malformed provider descriptors.
 *
 * XpbSourceProvider is an ABI between two separately built shared modules:
 * xp_batch.so defines it, an optional provider module fills one in and passes a
 * pointer, and nothing in the build system forces the two to be rebuilt
 * together. This module exists to make a stale descriptor a thing a test can
 * observe instead of a thing a reviewer has to reason about.
 *
 * It is a TEST module. It is not installed by the extension, nothing references
 * it, and it registers providers whose create() must never be called.
 *
 * The `legacy` case is written against the PRE-abi_version layout and compiles
 * whatever xp_batch's current header says, so the same script runs before and
 * after the fix:
 *
 *   before   legacy registers, and filters_rows is read out of the low bytes of
 *            the old describe pointer -- nonzero, so the caller skips the
 *            predicate and the query returns rows outside it
 *   after    legacy is refused at registration
 *
 * The other cases exist so "refused" cannot be satisfied by refusing
 * everything: a current descriptor must be accepted, and a descriptor that
 * omits the appended field must be accepted with that field reading false.
 */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"

#include "xpb_source.h"

PG_MODULE_MAGIC;

/*
 * The pre-fix layout, by hand. Not a copy of the current struct on purpose:
 * this is what a provider .so built before the fix has in its text segment, and
 * it must stay fixed here even as XpbSourceProvider changes.
 */
typedef struct LegacyProvider
{
    const char *name;
    void       *create;
    void       *describe;
} LegacyProvider;

static XpBatchSource *
probe_create(const XpbSourceRequest *req)
{
    (void) req;
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("xpb_abi_probe: create() of a test provider was called")));
    return NULL;        /* unreachable */
}

static bool
probe_describe(const XpbSourceRequest *req, XpbColType *types, int *ncols_out)
{
    (void) req; (void) types; (void) ncols_out;
    return false;
}

static const LegacyProvider legacy_provider = {
    .name     = "abi_legacy",
    .create   = (void *) probe_create,
    .describe = (void *) probe_describe,
};

#ifdef XPB_SOURCE_ABI_VERSION
/* A well-formed current descriptor: must be accepted. */
static const XpbSourceProvider current_provider = {
    XPB_SOURCE_PROVIDER_HEADER,
    .name         = "abi_current",
    .create       = probe_create,
    .describe     = probe_describe,
    .filters_rows = true,
};

/*
 * A descriptor that stops before the appended field, which is what a provider
 * built against an earlier minor version of the same ABI presents. Must be
 * accepted, and filters_rows must read false -- the safe direction, where the
 * caller applies the predicate itself.
 */
static const XpbSourceProvider minimal_provider = {
    .abi_version  = XPB_SOURCE_ABI_VERSION,
    .struct_size  = XPB_SOURCE_PROVIDER_MIN_SIZE,
    .name         = "abi_minimal",
    .create       = probe_create,
    .describe     = probe_describe,
    .filters_rows = true,   /* set, and OUTSIDE struct_size: must not be read */
};

/* Built against a NEWER xp_batch than the one loaded: must be refused. */
static const XpbSourceProvider future_provider = {
    .abi_version = XPB_SOURCE_ABI_VERSION,
    .struct_size = sizeof(XpbSourceProvider) + 16,
    .name        = "abi_future",
    .create      = probe_create,
};

/* Right size, wrong version: must be refused. */
static const XpbSourceProvider badversion_provider = {
    .abi_version = XPB_SOURCE_ABI_VERSION + 1,
    .struct_size = sizeof(XpbSourceProvider),
    .name        = "abi_badversion",
    .create      = probe_create,
};
#endif

/*
 * xpb_abi_register(case) -> 'registered' | raises
 *
 * Returning rather than raising on success lets one script assert both
 * directions without parsing an error message for the accepted cases.
 */
PG_FUNCTION_INFO_V1(xpb_abi_register);

Datum
xpb_abi_register(PG_FUNCTION_ARGS)
{
    char *which = text_to_cstring(PG_GETARG_TEXT_PP(0));

    if (strcmp(which, "legacy") == 0)
        xpb_register_source_provider((const XpbSourceProvider *) &legacy_provider);
#ifdef XPB_SOURCE_ABI_VERSION
    else if (strcmp(which, "current") == 0)
        xpb_register_source_provider(&current_provider);
    else if (strcmp(which, "minimal") == 0)
        xpb_register_source_provider(&minimal_provider);
    else if (strcmp(which, "future") == 0)
        xpb_register_source_provider(&future_provider);
    else if (strcmp(which, "badversion") == 0)
        xpb_register_source_provider(&badversion_provider);
#endif
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("xpb_abi_probe: unknown case \"%s\"", which)));

    PG_RETURN_TEXT_P(cstring_to_text("registered"));
}

/*
 * xpb_abi_filters_rows(name) -> 'absent' | 'false' | 'true'
 *
 * Reads the field the way xpb_batch_join2_groupby reads it, so the test
 * observes the value that decides whether the predicate is applied. Kept here
 * rather than added to xpb_source_providers() so the extension's SQL surface
 * does not grow a column for a test.
 */
PG_FUNCTION_INFO_V1(xpb_abi_filters_rows);

Datum
xpb_abi_filters_rows(PG_FUNCTION_ARGS)
{
    char *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const XpbSourceProvider *p = xpb_find_source_provider(name);

    if (p == NULL)
        PG_RETURN_TEXT_P(cstring_to_text("absent"));

    PG_RETURN_TEXT_P(cstring_to_text(p->filters_rows ? "true" : "false"));
}
