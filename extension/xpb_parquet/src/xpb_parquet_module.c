/*
 * xpb_parquet_module.c — registration half of the optional Parquet provider.
 *
 * Pure C, no Arrow. This file exists to answer one architectural question
 * before any Parquet code is written: can a provider register itself with
 * xp_batch from a separate shared module, with no link-time dependency in
 * either direction?
 */
#include "postgres.h"
#include "fmgr.h"

#include "xpb_source.h"

PG_MODULE_MAGIC;

static XpBatchSource *
parquet_create(const XpbSourceRequest *req)
{
    /*
     * Placeholder for commit 0002. Reaching this means registration and
     * lookup work, which is what this commit is proving.
     */
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("xpb_parquet: provider registered but not implemented yet"),
             errdetail("uri=%s ncols=%d has_pred=%d",
                       req->uri ? req->uri : "(null)",
                       req->ncols, (int) req->has_pred)));
    return NULL;
}

static const XpbSourceProvider parquet_provider = {
    .name     = "parquet",
    .create   = parquet_create,
    .describe = NULL,
};

void
_PG_init(void)
{
    xpb_register_source_provider(&parquet_provider);
}
