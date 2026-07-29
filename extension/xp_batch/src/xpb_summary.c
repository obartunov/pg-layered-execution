/*-------------------------------------------------------------------------
 * xpb_summary.c
 *
 * Persistent page summary loader.
 * Direct heap scan on xp_page_summary — no SPI, no planner re-entry.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "xpb_pageagg.h"

/*
 * xpb_load_summaries
 *
 * Direct heap scan on public.xp_page_summary.
 * Loads all rows for this filenode into a palloc'd array[nblocks].
 * Returns NULL if table not found or no matching rows.
 *
 * pS4: validity gate — relpages_snap must equal nblocks.
 */
XpPageSummary *
xpb_load_summaries(Relation rel, BlockNumber nblocks)
{
    Oid             filenode = rel->rd_rel->relfilenode;
    Oid             sum_relid;
    Relation        sum_rel;
    TableScanDesc   scan;
    HeapTuple       tup;
    TupleDesc       tdesc;
    XpPageSummary  *arr = NULL;
    bool            valid = false;
    Snapshot        snap;

    /* Find xp_page_summary table */
    sum_relid = get_relname_relid("xp_page_summary", PG_PUBLIC_NAMESPACE);
    if (!OidIsValid(sum_relid))
        return NULL;

    sum_rel = table_open(sum_relid, AccessShareLock);
    tdesc   = RelationGetDescr(sum_rel);
    snap    = GetTransactionSnapshot();

    scan = table_beginscan(sum_rel, snap, 0, NULL, 0);

    while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
    {
        bool    isnull;
        Datum   d;
        Oid     row_filenode;
        int32   row_relpages;
        int32   blkno;

        /* col 1: relfilenode */
        d = heap_getattr(tup, 1, tdesc, &isnull);
        if (isnull) continue;
        row_filenode = DatumGetObjectId(d);
        if (row_filenode != filenode) continue;

        /* col 2: relpages_snap — pS4 validity gate */
        d = heap_getattr(tup, 2, tdesc, &isnull);
        if (isnull) continue;
        row_relpages = DatumGetInt32(d);
        if ((BlockNumber) row_relpages != nblocks) continue;

        /* Allocate array on first matching row */
        if (arr == NULL)
            arr = palloc0(nblocks * sizeof(XpPageSummary));

        /* col 3: blkno */
        d = heap_getattr(tup, 3, tdesc, &isnull);
        if (isnull) continue;
        blkno = DatumGetInt32(d);
        if (blkno < 0 || (BlockNumber) blkno >= nblocks) continue;

        XpPageSummary *s = &arr[blkno];

        /* col 4: row_count */
        d = heap_getattr(tup, 4, tdesc, &isnull);
        s->row_count  = isnull ? 0 : DatumGetInt32(d);

        /* col 5: min_id */
        d = heap_getattr(tup, 5, tdesc, &isnull);
        s->min_val    = isnull ? PG_INT32_MAX : DatumGetInt32(d);

        /* col 6: max_id */
        d = heap_getattr(tup, 6, tdesc, &isnull);
        s->max_val    = isnull ? PG_INT32_MIN : DatumGetInt32(d);

        /* col 7: all_visible */
        d = heap_getattr(tup, 7, tdesc, &isnull);
        s->all_visible = !isnull && DatumGetBool(d);

        s->valid = true;
        valid    = true;
    }

    table_endscan(scan);
    table_close(sum_rel, AccessShareLock);

    if (!valid)
    {
        if (arr) pfree(arr);
        return NULL;
    }

    return arr;
}
