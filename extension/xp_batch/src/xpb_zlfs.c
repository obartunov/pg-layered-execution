/*
 * xpb_zlfs.c — ZLFS v0.2: generalized file-backed analytical zones
 *
 * No hardcoded table names, column names, or byte offsets.
 * Caller provides relid, attnos[], predicate range.
 * Byte offsets computed from TupleDesc at build time.
 * Schema hash stored and validated at load time.
 */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/namespace.h"
#include "portability/instr_time.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "miscadmin.h"
#include "storage/fd.h"

#include "xpb_zlfs.h"
#include "xpb_colbatch.h"
#include "xpb_src_heap.h"
#include "utils/tuplestore.h"

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

#define ZLFS_DIR "zlfs"

ZlfsRegistry *zlfs_reg = NULL;
static bool zlfs_files_scanned = false;

/* ── Schema fingerprint ── */

uint32
zlfs_compute_schema_hash(Oid relid, int16 *attnos, int ncols)
{
    uint32 h = 0x811c9dc5u;  /* FNV-1a seed */
    Relation rel = table_open(relid, AccessShareLock);
    TupleDesc td = RelationGetDescr(rel);

    for (int i = 0; i < ncols; i++)
    {
        int16 attno = attnos[i];
        if (attno < 1 || attno > td->natts)
            ereport(ERROR, (errmsg("ZLFS: attno %d out of range (1..%d)", attno, td->natts)));

        Form_pg_attribute attr = TupleDescAttr(td, attno - 1);

        if (attr->attisdropped)
            ereport(ERROR, (errmsg("ZLFS: attno %d is a dropped column", attno)));

        /* Full attribute fingerprint */
        h ^= (uint32)attno;            h *= 0x01000193u;
        h ^= (uint32)attr->atttypid;   h *= 0x01000193u;
        h ^= (uint32)attr->attlen;     h *= 0x01000193u;
        h ^= (uint32)attr->atttypmod;  h *= 0x01000193u;
        h ^= (uint32)attr->attalign;   h *= 0x01000193u;
        h ^= (uint32)attr->attbyval;   h *= 0x01000193u;
        h ^= (uint32)attr->attnotnull; h *= 0x01000193u;
    }
    table_close(rel, AccessShareLock);
    return h;
}


/* ── Registry ── */

void
zlfs_ensure_registry(void)
{
    if (zlfs_reg) return;
    MemoryContext mcxt = AllocSetContextCreate(TopMemoryContext,
                                               "ZLFS Registry",
                                               ALLOCSET_DEFAULT_SIZES);
    zlfs_reg = MemoryContextAllocZero(mcxt, sizeof(ZlfsRegistry));
    zlfs_reg->mcxt = mcxt;
}

/* ── File I/O ── */

static void
zlfs_write_file(ZlfsZone *zone)
{
    char tmppath[MAXPGPATH];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp.%d", zone->filepath, MyProcPid);
    FILE *f = fopen(tmppath, "wb");
    if (!f)
        ereport(ERROR, (errmsg("ZLFS: cannot open %s for write", tmppath)));

    ZlfsFileHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = ZLFS_MAGIC;
    hdr.version = ZLFS_VERSION;
    hdr.source_relid = zone->source_relid;
    hdr.pred_lo = zone->pred_lo;
    hdr.pred_hi = zone->pred_hi;
    hdr.ncols = zone->ncols;
    hdr.nrows = zone->nrows;
    hdr.col_width = 0;          /* v2-only field; types are per column now */
    hdr.build_time = zone->build_time;
    hdr.freshness = (uint32)zone->freshness;
    hdr.schema_hash = zone->schema_hash;
    memcpy(hdr.col_attnos, zone->col_attnos, sizeof(hdr.col_attnos));
    for (int c = 0; c < zone->ncols; c++)
    {
        hdr.col_types[c] = (uint8) zone->col_types[c];
        hdr.col_widths[c] = zone->col_widths[c];
        hdr.col_flags[c] = zone->col_validity[c] ? ZLFS_COLFLAG_VALIDITY : 0;
    }

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) goto write_err;
    for (int c = 0; c < zone->ncols; c++)
    {
        size_t n = (size_t) zone->nrows * zone->col_widths[c];

        if (zone->nrows > 0 && fwrite(zone->cols[c], 1, n, f) != n)
            goto write_err;
        if (zone->col_validity[c] != NULL)
        {
            size_t vb = ZLFS_VALIDITY_BYTES(zone->nrows);

            if (vb > 0 && fwrite(zone->col_validity[c], 1, vb, f) != vb)
                goto write_err;
        }
    }
    if (fflush(f) != 0) goto write_err;
    if (pg_fsync(fileno(f)) != 0)
        elog(WARNING, "ZLFS: fsync failed for %s", tmppath);
    if (fclose(f) != 0) goto write_err;

    if (rename(tmppath, zone->filepath) != 0)
        ereport(ERROR, (errmsg("ZLFS: cannot rename %s → %s", tmppath, zone->filepath)));
    return;

write_err:
    fclose(f);
    unlink(tmppath);
    ereport(ERROR, (errmsg("ZLFS: write error on %s", tmppath)));
}

static void
zlfs_update_freshness(ZlfsZone *zone)
{
    int fd = BasicOpenFile(zone->filepath, O_RDWR | PG_BINARY);
    if (fd < 0) return;

    struct flock lk = {0};
    lk.l_type = F_WRLCK; lk.l_whence = SEEK_SET;
    lk.l_start = 0; lk.l_len = sizeof(ZlfsFileHeader);
    (void) fcntl(fd, F_SETLKW, &lk);

    off_t off = offsetof(ZlfsFileHeader, freshness);
    uint32 val = (uint32)zone->freshness;
    if (pg_pwrite(fd, &val, sizeof(val), off) != sizeof(val))
        elog(WARNING, "ZLFS: failed to update freshness for %s", zone->filepath);
    pg_fsync(fd);

    lk.l_type = F_UNLCK;
    (void) fcntl(fd, F_SETLK, &lk);
    close(fd);
}

static ZlfsZone *
zlfs_load_file(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;

    ZlfsFileHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return NULL; }
    if (hdr.magic != ZLFS_MAGIC) { fclose(f); return NULL; }

    /*
     * An incompatible version is refused BY NAME, not passed over as if the
     * file were missing.  Reinterpreting a version-2 file as a version-3 one
     * would read its int32 data as typed columns with no validity and is
     * exactly the silent misreading this format change exists to prevent.
     */
    if (hdr.version != ZLFS_VERSION)
    {
        fclose(f);
        ereport(WARNING,
                (errmsg("ZLFS: %s is format version %u, this build reads version %d",
                        filepath, hdr.version, ZLFS_VERSION),
                 errdetail(hdr.version == ZLFS_VERSION_UNTYPED
                           ? "Version %d is the untyped int32-only layout, which carries no column types or validity and is not read."
                           : "Unknown version.",
                           ZLFS_VERSION_UNTYPED),
                 errhint("Drop and rebuild the zone with zlfs_build_zone().")));
        return NULL;
    }
    if (hdr.ncols > ZLFS_MAX_COLS) { fclose(f); return NULL; }

    MemoryContext old = MemoryContextSwitchTo(zlfs_reg->mcxt);
    ZlfsZone *z = palloc0(sizeof(ZlfsZone));
    z->source_relid = (Oid)hdr.source_relid;
    z->pred_lo = hdr.pred_lo;
    z->pred_hi = hdr.pred_hi;
    z->ncols = hdr.ncols;
    z->nrows = hdr.nrows;
    z->freshness = (ZlfsFreshness)hdr.freshness;
    z->build_time = (TimestampTz)hdr.build_time;
    z->schema_hash = hdr.schema_hash;
    memcpy(z->col_attnos, hdr.col_attnos, sizeof(z->col_attnos));
    snprintf(z->filepath, sizeof(z->filepath), "%s", filepath);
    z->mcxt = zlfs_reg->mcxt;

    for (int c = 0; c < (int)hdr.ncols; c++)
    {
        size_t n;

        z->col_types[c] = (ZlfsColType) hdr.col_types[c];
        z->col_widths[c] = hdr.col_widths[c];

        if ((z->col_types[c] != ZLFS_COL_INT4 && z->col_types[c] != ZLFS_COL_INT8) ||
            z->col_widths[c] != (z->col_types[c] == ZLFS_COL_INT8 ? 8 : 4))
        {
            fclose(f);
            MemoryContextSwitchTo(old);
            ereport(WARNING,
                    (errmsg("ZLFS: %s column %d has type %u width %u, which do not agree",
                            filepath, c, hdr.col_types[c], hdr.col_widths[c])));
            return NULL;
        }

        n = (size_t) hdr.nrows * z->col_widths[c];
        z->cols[c] = palloc(Max(n, 1));
        if (n > 0 && fread(z->cols[c], 1, n, f) != n)
        {
            fclose(f);
            MemoryContextSwitchTo(old);
            return NULL;
        }

        if (hdr.col_flags[c] & ZLFS_COLFLAG_VALIDITY)
        {
            size_t vb = ZLFS_VALIDITY_BYTES(hdr.nrows);

            z->col_validity[c] = palloc(Max(vb, 1));
            if (vb > 0 && fread(z->col_validity[c], 1, vb, f) != vb)
            {
                fclose(f);
                MemoryContextSwitchTo(old);
                return NULL;
            }
        }
    }
    fclose(f);
    MemoryContextSwitchTo(old);
    return z;
}

void
zlfs_scan_directory(void)
{
    if (zlfs_files_scanned) return;
    zlfs_files_scanned = true;

    char dirpath[MAXPGPATH];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", DataDir, ZLFS_DIR);
    DIR *d = opendir(dirpath);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (strstr(ent->d_name, ".zlfs") == NULL) continue;
        char fpath[MAXPGPATH];
        snprintf(fpath, sizeof(fpath), "%s/%s", dirpath, ent->d_name);

        /* Skip if already loaded */
        bool found = false;
        for (int i = 0; i < zlfs_reg->nzones; i++)
        {
            if (strcmp(zlfs_reg->zones[i]->filepath, fpath) == 0)
            { found = true; break; }
        }
        if (found) continue;

        ZlfsZone *z = zlfs_load_file(fpath);
        if (z && zlfs_reg->nzones < ZLFS_MAX_ZONES)
        {
            /*
             * Validate schema hash against current catalog.
             *
             * Nothing may leave this block by continue/break/return/goto:
             * PG_END_TRY() is what pops PG_exception_stack and
             * error_context_stack, and skipping it leaves them pointing into
             * this frame after it is gone, so the NEXT ereport(ERROR) anywhere
             * in the session longjmps into a dead frame.  Two `continue`s used
             * to do exactly that; the symptom was a SIGSEGV in a later,
             * unrelated call in the same backend (a stale zone file left by a
             * dropped table was enough to arm it).  Record the verdict, leave
             * the block normally, act afterwards.
             *
             * The catch arm also restores CurrentMemoryContext: on longjmp it
             * is whatever the thrower left current, and allocations made after
             * that land in a context this loop does not own.
             */
            bool    zone_usable = true;
            bool    validate_failed = false;
            uint32  current_hash = 0;

            if (OidIsValid(z->source_relid) && z->ncols > 0)
            {
                MemoryContext caller_cx = CurrentMemoryContext;

                PG_TRY();
                {
                    current_hash = zlfs_compute_schema_hash(
                        z->source_relid, z->col_attnos, z->ncols);
                }
                PG_CATCH();
                {
                    /* the source table may have been dropped since the build */
                    MemoryContextSwitchTo(caller_cx);
                    FlushErrorState();
                    validate_failed = true;
                }
                PG_END_TRY();

                if (validate_failed)
                {
                    elog(WARNING, "ZLFS: cannot validate schema for %s "
                         "(source relation gone?), skipping", fpath);
                    zone_usable = false;
                }
                else if (current_hash != z->schema_hash)
                {
                    elog(WARNING, "ZLFS: schema mismatch for %s "
                         "(stored=%08x, current=%08x), skipping",
                         fpath, z->schema_hash, current_hash);
                    zone_usable = false;
                }
            }

            if (!zone_usable)
                continue;       /* outside the TRY block: legal */

            zlfs_reg->zones[zlfs_reg->nzones++] = z;
        }
    }
    closedir(d);
}

ZlfsZone *
zlfs_lookup_valid_zone(Oid relid, int32 pred_lo, int32 pred_hi)
{
    if (!zlfs_reg) return NULL;
    for (int i = 0; i < zlfs_reg->nzones; i++)
    {
        ZlfsZone *z = zlfs_reg->zones[i];
        if (z->source_relid == relid &&
            z->pred_lo == pred_lo && z->pred_hi == pred_hi &&
            z->freshness == ZLFS_VALID)
            return z;
    }
    return NULL;
}

/* ── SQL: zlfs_build_zone(relname, col_attnos_csv, lo, hi) ── */

PG_FUNCTION_INFO_V1(zlfs_build_zone);

Datum
zlfs_build_zone(PG_FUNCTION_ARGS)
{
    char *relname = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *cols_csv = text_to_cstring(PG_GETARG_TEXT_PP(1));
    int32 lo = PG_GETARG_INT32(2);
    int32 hi = PG_GETARG_INT32(3);

    Oid relid = RelnameGetRelid(relname);
    if (!OidIsValid(relid))
        ereport(ERROR, (errmsg("table %s not found", relname)));

    /* Parse comma-separated attnos */
    int16 attnos[ZLFS_MAX_COLS];
    int ncols = 0;
    char *p = cols_csv;
    while (*p && ncols < ZLFS_MAX_COLS)
    {
        attnos[ncols++] = (int16)atoi(p);
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    if (ncols < 1)
        ereport(ERROR, (errmsg("ZLFS: need at least 1 column")));

    /*
     * No offset computation here any more: the batch source addresses the
     * tuple, and it is the only place that knows how.  The schema hash still
     * fingerprints (attnos + types), so a zone built against one table shape
     * is not served for another.
     */
    uint32 schema_hash = zlfs_compute_schema_hash(relid, attnos, ncols);

    zlfs_ensure_registry();
    zlfs_scan_directory();

    /* Evict existing zone for same range */
    for (int i = 0; i < zlfs_reg->nzones; i++)
    {
        ZlfsZone *z = zlfs_reg->zones[i];
        if (z->source_relid == relid && z->pred_lo == lo && z->pred_hi == hi)
        {
            zlfs_reg->zones[i] = zlfs_reg->zones[--zlfs_reg->nzones];
            break;
        }
    }

    instr_time t0, t1;
    INSTR_TIME_SET_CURRENT(t0);

    /* Allocate zone */
    MemoryContext old = MemoryContextSwitchTo(zlfs_reg->mcxt);
    ZlfsZone *zone = palloc0(sizeof(ZlfsZone));
    zone->source_relid = relid;
    zone->pred_lo = lo;
    zone->pred_hi = hi;
    zone->ncols = ncols;
    zone->schema_hash = schema_hash;
    memcpy(zone->col_attnos, attnos, ncols * sizeof(int16));
    zone->mcxt = zlfs_reg->mcxt;

    /*
     * Fill the zone by draining a typed batch source rather than walking
     * blocks here.  The builder used to keep its own copy of the fixed-offset
     * tuple decoding -- a second place with the same assumptions about
     * layout, alignment and NULLs, which had already drifted from the heap
     * source's copy.  Going through XpBatchSource means ZLFS inherits the
     * source's guards and its types instead of re-deriving them.
     *
     * XPB_HEAP_DEFORM, not the fixed path: a zone may now hold int8 and
     * nullable columns, and the deform path is the one that can read them.
     * The predicate is pushed into the source, so only rows in [lo, hi]
     * arrive.
     */
    XpBatchSource  *src;
    XpColumnBatch   batch;
    int64           capacity = 0;
    int64           nrows = 0;

    src = xpb_heap_source_create_ex(relid, attnos, ncols, XPB_HEAP_DEFORM,
                                    true, lo, hi);

    memset(&batch, 0, sizeof(batch));
    batch.capacity = XPCB_BATCH_CAP;
    batch.ncols = ncols;

    while (src->ops->next_batch(src, &batch))
    {
        MemoryContext o2;

        if (nrows == 0)
        {
            /* First batch fixes the zone's types; later batches must agree. */
            o2 = MemoryContextSwitchTo(zlfs_reg->mcxt);
            for (int c = 0; c < ncols; c++)
            {
                switch (batch.cols[c].type)
                {
                    case XPB_COL_INT4:
                        zone->col_types[c] = ZLFS_COL_INT4;
                        zone->col_widths[c] = sizeof(int32);
                        break;
                    case XPB_COL_INT8:
                        zone->col_types[c] = ZLFS_COL_INT8;
                        zone->col_widths[c] = sizeof(int64);
                        break;
                    default:
                        MemoryContextSwitchTo(o2);
                        ereport(ERROR,
                                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                 errmsg("ZLFS: column attno %d is %s; a zone carries int4 or int8",
                                        attnos[c], xpcb_type_name(batch.cols[c].type))));
                }
            }
            MemoryContextSwitchTo(o2);
        }

        if (nrows + batch.nrows > capacity)
        {
            int64 newcap = Max(capacity * 2, Max(nrows + batch.nrows, 65536));

            o2 = MemoryContextSwitchTo(zlfs_reg->mcxt);
            for (int c = 0; c < ncols; c++)
            {
                zone->cols[c] = zone->cols[c]
                    ? repalloc(zone->cols[c], newcap * zone->col_widths[c])
                    : palloc(newcap * zone->col_widths[c]);
                if (zone->col_validity[c])
                    zone->col_validity[c] =
                        repalloc(zone->col_validity[c], ZLFS_VALIDITY_BYTES(newcap));
            }
            MemoryContextSwitchTo(o2);
            capacity = newcap;
        }

        for (int c = 0; c < ncols; c++)
        {
            const XpBatchColumn *bc = &batch.cols[c];

            if (bc->type != (zone->col_types[c] == ZLFS_COL_INT8
                             ? XPB_COL_INT8 : XPB_COL_INT4))
                ereport(ERROR,
                        (errmsg("ZLFS: column attno %d changed type mid-scan",
                                attnos[c])));

            /*
             * A column only gets a bitmap once a NULL actually turns up, and
             * the rows already copied are retroactively marked valid -- they
             * were, or they would have allocated it themselves.
             */
            if (bc->validity != NULL && zone->col_validity[c] == NULL)
            {
                o2 = MemoryContextSwitchTo(zlfs_reg->mcxt);
                zone->col_validity[c] = palloc(ZLFS_VALIDITY_BYTES(capacity));
                memset(zone->col_validity[c], 0xFF, ZLFS_VALIDITY_BYTES(capacity));
                MemoryContextSwitchTo(o2);
            }

            if (zone->col_types[c] == ZLFS_COL_INT8)
                memcpy((int64 *) zone->cols[c] + nrows, bc->data,
                       batch.nrows * sizeof(int64));
            else
                memcpy((int32 *) zone->cols[c] + nrows, bc->data,
                       batch.nrows * sizeof(int32));

            if (zone->col_validity[c] != NULL)
            {
                uint8 *dst = zone->col_validity[c];

                for (int r = 0; r < batch.nrows; r++)
                {
                    int64 z = nrows + r;

                    if (xpcb_isnull(&batch, c, r))
                        dst[z >> 3] &= ~(1 << (z & 7));
                    else
                        dst[z >> 3] |= (1 << (z & 7));
                }
            }
        }

        nrows += batch.nrows;
        xpcb_release_owned(&batch);
    }
    src->ops->end(src);

    /* A zone with no rows still has to have well-defined columns. */
    if (nrows == 0)
    {
        MemoryContext o2 = MemoryContextSwitchTo(zlfs_reg->mcxt);

        for (int c = 0; c < ncols; c++)
            if (zone->col_types[c] == ZLFS_COL_NONE)
            {
                zone->col_types[c] = ZLFS_COL_INT4;
                zone->col_widths[c] = sizeof(int32);
                zone->cols[c] = palloc(1);
            }
        MemoryContextSwitchTo(o2);
    }

    zone->nrows = nrows;
    zone->freshness = ZLFS_VALID;
    zone->build_time = GetCurrentTimestamp();

    /* Write to disk */
    char dirpath[MAXPGPATH];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", DataDir, ZLFS_DIR);
    mkdir(dirpath, S_IRWXU);
    snprintf(zone->filepath, sizeof(zone->filepath),
             "%s/zone_%u_%d_%d.zlfs", dirpath, relid, lo, hi);
    zlfs_write_file(zone);

    /* Register */
    if (zlfs_reg->nzones < ZLFS_MAX_ZONES)
        zlfs_reg->zones[zlfs_reg->nzones++] = zone;

    INSTR_TIME_SET_CURRENT(t1);
    double build_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

    elog(NOTICE, "ZLFS zone [%d..%d] built: %ld rows, %.0f ms, %ld KB, "
         "ncols=%d, schema_hash=%08x, file=%s",
         lo, hi, nrows, build_ms, nrows * ncols * 4 / 1024,
         ncols, schema_hash, zone->filepath);

    PG_RETURN_TEXT_P(cstring_to_text("VALID"));
}

/* ── SQL: zlfs_invalidate_zone(lo, hi) ── */

PG_FUNCTION_INFO_V1(zlfs_invalidate_zone);

Datum
zlfs_invalidate_zone(PG_FUNCTION_ARGS)
{
    int32 lo = PG_GETARG_INT32(0);
    int32 hi = PG_GETARG_INT32(1);

    zlfs_ensure_registry();
    zlfs_scan_directory();

    for (int i = 0; i < zlfs_reg->nzones; i++)
    {
        ZlfsZone *z = zlfs_reg->zones[i];
        if (z->pred_lo == lo && z->pred_hi == hi && z->freshness == ZLFS_VALID)
        {
            z->freshness = ZLFS_STALE;
            zlfs_update_freshness(z);
            PG_RETURN_TEXT_P(cstring_to_text("STALE"));
        }
    }
    PG_RETURN_TEXT_P(cstring_to_text("NOT_FOUND"));
}

/* ── SQL: zlfs_drop_zone(lo, hi) ── */

PG_FUNCTION_INFO_V1(zlfs_drop_zone);

Datum
zlfs_drop_zone(PG_FUNCTION_ARGS)
{
    int32 lo = PG_GETARG_INT32(0);
    int32 hi = PG_GETARG_INT32(1);

    zlfs_ensure_registry();
    zlfs_scan_directory();

    for (int i = 0; i < zlfs_reg->nzones; i++)
    {
        ZlfsZone *z = zlfs_reg->zones[i];
        if (z->pred_lo == lo && z->pred_hi == hi)
        {
            unlink(z->filepath);
            zlfs_reg->zones[i] = zlfs_reg->zones[--zlfs_reg->nzones];
            PG_RETURN_VOID();
        }
    }
    PG_RETURN_VOID();
}

/* ── SQL: zlfs_zone_info() ── */

PG_FUNCTION_INFO_V1(zlfs_zone_info);

Datum
zlfs_zone_info(PG_FUNCTION_ARGS)
{
    zlfs_ensure_registry();
    zlfs_scan_directory();

    InitMaterializedSRF(fcinfo, 0);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    for (int i = 0; i < zlfs_reg->nzones; i++)
    {
        ZlfsZone *z = zlfs_reg->zones[i];
        Datum vals[7];
        bool nulls[7] = {false};

        vals[0] = Int32GetDatum(z->pred_lo);
        vals[1] = Int32GetDatum(z->pred_hi);
        vals[2] = Int64GetDatum(z->nrows);
        vals[3] = Int64GetDatum(z->nrows * z->ncols * 4 / 1024);
        vals[4] = CStringGetTextDatum(
            z->freshness == ZLFS_VALID ? "VALID" :
            z->freshness == ZLFS_STALE ? "STALE" : "MISSING");
        vals[5] = TimestampTzGetDatum(z->build_time);
        vals[6] = CStringGetTextDatum(z->filepath);

        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
    }
    return (Datum) 0;
}

/* ── SQL: zlfs_group_sum(relname, cols_csv, lo, hi) — backward compat SRF ── */

PG_FUNCTION_INFO_V1(zlfs_group_sum);

Datum
zlfs_group_sum(PG_FUNCTION_ARGS)
{
    int32 lo = PG_GETARG_INT32(0);
    int32 hi = PG_GETARG_INT32(1);

    /* Backward compat: hardcoded to reg_buh cols 1,2,6 */
    Oid relid = RelnameGetRelid("reg_buh");
    if (!OidIsValid(relid))
        ereport(ERROR, (errmsg("table reg_buh not found")));

    zlfs_ensure_registry();
    zlfs_scan_directory();
    ZlfsZone *zone = zlfs_lookup_valid_zone(relid, lo, hi);

    InitMaterializedSRF(fcinfo, 0);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    if (!zone || zone->freshness != ZLFS_VALID)
    {
        /* Heap fallback omitted for brevity — use xpb_batch_groupby(lo,hi,'heap') */
        ereport(ERROR, (errmsg("ZLFS: no valid zone, use xpb_batch_groupby for heap fallback")));
    }

    /* ZLFS scan path — find columns by attno */
    int pk_idx = -1, ck_idx = -1, dt_idx = -1;
    for (int c = 0; c < zone->ncols; c++)
    {
        if (zone->col_attnos[c] == 1) pk_idx = c;  /* period_key = attno 1 */
        if (zone->col_attnos[c] == 2) ck_idx = c;  /* company_key = attno 2 */
        if (zone->col_attnos[c] == 6) dt_idx = c;  /* amount_dt = attno 6 */
    }
    if (pk_idx < 0 || ck_idx < 0 || dt_idx < 0)
        ereport(ERROR, (errmsg("ZLFS: zone missing required columns")));

    instr_time t0, t1;
    INSTR_TIME_SET_CURRENT(t0);

    #define GRP_CAP 16384
    #define GRP_MAX_LOAD (GRP_CAP * 3 / 4)
    typedef struct { int32 k1, k2; int64 sum; bool occ; } GrpEntry;
    GrpEntry *ht = palloc0(GRP_CAP * sizeof(GrpEntry));
    int ngroups = 0;

    int32 *col_pk = zone->cols[pk_idx];
    int32 *col_ck = zone->cols[ck_idx];
    int32 *col_dt = zone->cols[dt_idx];

    for (int64 i = 0; i < zone->nrows; i++)
    {
        int32 pk = col_pk[i], ck = col_ck[i], dt = col_dt[i];
        uint32 h = (uint32)pk * 2654435761u ^ (uint32)ck * 2246822519u;
        int sl = (int)(h & (GRP_CAP - 1));
        for (int pr = 0; pr < GRP_CAP; pr++)
        {
            int idx = (sl + pr) & (GRP_CAP - 1);
            if (!ht[idx].occ) {
                if (ngroups >= GRP_MAX_LOAD)
                    ereport(ERROR, (errmsg("ZLFS: hash overflow")));
                ht[idx].k1=pk; ht[idx].k2=ck; ht[idx].sum=dt; ht[idx].occ=true;
                ngroups++; break;
            }
            if (ht[idx].k1==pk && ht[idx].k2==ck) { ht[idx].sum+=dt; break; }
        }
    }

    INSTR_TIME_SET_CURRENT(t1);
    double scan_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

    Datum vals[4]; bool nulls[4] = {false};
    for (int i = 0; i < GRP_CAP; i++) {
        if (!ht[i].occ) continue;
        vals[0] = Int32GetDatum(ht[i].k1);
        vals[1] = Int32GetDatum(ht[i].k2);
        vals[2] = Int64GetDatum(ht[i].sum);
        vals[3] = Float8GetDatum(scan_ms);
        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
    }

    elog(NOTICE, "ZLFS: scan=%.1f ms  zone=[%d..%d]  rows=%ld  groups=%d",
         scan_ms, lo, hi, zone->nrows, ngroups);

    pfree(ht);
    return (Datum) 0;
}
